// =============================================================================
// Last modified: 2026-03-19 00:00
// search.cpp — Negamax alpha-beta search with iterative deepening
//
// Facon 1.1 — Herrumbre
//
//   KILLER MOVES (search improvement):
//     Quiet moves that cause a beta cutoff are stored as "killers" for that ply.
//     On the next search of the same ply, killers are tried before other quiet
//     moves. Improves move ordering significantly in quiet positions where
//     MVV-LVA has no effect.
//
//   SELDEPTH (UCI output):
//     The maximum depth reached including quiescence search is now tracked in
//     stats_.seldepth and reported in the UCI info line each iteration.
//
//   ABORT FLAG (stability):
//     When the hard time limit is hit inside the search, abort_search_ is set
//     and every level of the call stack returns immediately. This ensures all
//     unmake_move() calls execute, leaving the board consistent. Without this,
//     returning mid-recursion would leave make_move() calls unmatched and
//     corrupt the board for the next search.
//
//   SAFE BESTMOVE (stability):
//     root_best_move_: the best move at ply 0 is saved directly inside
//     negamax() whenever a new best is found at the root. We never do TT
//     early returns at ply 0, so root_best_move_ is always set from the
//     actual move loop — never from a potentially stale or collided TT entry.
//
//     safe_move: pre-seeded before the loop with the first legal move found
//     from generate_all_moves(). Last-resort fallback that guarantees
//     bestmove is never "0000" even under extreme time pressure.
//
//   ILLEGAL PV MOVE FIX (bugfix):
//     TT entries from earlier positions can contain moves that were valid then
//     but are illegal now (wrong piece, wrong color, or hash collision). These
//     are now caught by is_legal() in board.cpp, which checks piece ownership
//     on the from-square before doing the expensive board copy. This fixes the
//     "ghost move" bug where is_legal() would accept moves from empty squares.
//
//   DYNAMIC TIME MANAGEMENT (time control):
//     Replaced the single hard stop with a soft/hard limit model. After each
//     completed iteration, the search checks soft_stop() instead of should_stop().
//     If the PV move changed or the score dropped significantly, the soft limit
//     is extended to give the engine more time on difficult positions.
//
// Facon 1.2 — Rojo Vivo
//
//   SINGLE MOVE GENERATION IN TT PROBE PATH (cleanup):
//     In 1.1, negamax() called generate_all_moves() twice per node when a TT
//     hit with a valid tt_move was found: once to validate the TT move, and
//     again for the main move loop. Fixed by generating the list once before
//     the TT probe and reusing it for both purposes.
//
//   NULL MOVE PRUNING — NMP (search improvement):
//     If the side to move can pass without moving and the resulting search at
//     (depth - 1 - NMP_REDUCTION) still returns a score >= beta, the position
//     is likely so good that a cutoff is safe. Disabled in check, at the root,
//     when do_null=false (prevents consecutive null moves), and when the side
//     to move has only pawns and king (zugzwang guard).
//
//   TRIANGULAR PV ARRAY (search improvement):
//     Tracks the full principal variation using a MAX_PLY x MAX_PLY table.
//     pv_table_[ply][0..pv_length_[ply]-1] holds the PV from that ply onward.
//     Updated whenever a move raises alpha. Table is reset at the start of
//     each iterative deepening iteration.
//
//   PV REPETITION DETECTION (bugfix):
//     The PV walk in go() seeds a seen[] array with hashes from the full game
//     history plus the root position. After each make_move() in the walk, if
//     the resulting hash appears in seen[], the move is not printed and the
//     walk stops. This prevents the GUI warning "PV continues after threefold
//     repetition". The underlying root cause was the unmake_move() hash
//     corruption fixed in board.cpp — the alternating root hash between ID
//     iterations caused seen[] lookups to fail silently.
//
//   VERBOSITY (observability):
//     Three output mechanisms for better search visibility, especially at
//     long time controls:
//
//     currmove: at the root (ply==0), emit "info currmove X currmovenumber N"
//       before searching each move. Standard UCI fields — GUIs display this
//       in a dedicated panel. Lets the operator see which move is being
//       explored in real time.
//
//     new best: when the best move at the root *changes* relative to the
//       previous completed iteration, emit "info string new best: <SAN>
//       <score> -- move N/total depth D [h:mm:ss,ms]". Suppressed when the
//       same move comes back best at the start of a new depth (alpha resets
//       to -INFINITY each iteration, which would otherwise fire for every
//       depth even with an unchanged move). Time formatted as h:mm:ss,ms
//       for readability at long time controls. Pure integer arithmetic.
//
//     heartbeat: if no output has been emitted for HEARTBEAT_INTERVAL_MS
//       (5 minutes), emit a status line with current depth, nodes, nps,
//       time, and hashfull. Essential for distinguishing a crash from a
//       deep search at very long time controls (e.g. 12h+10min).
//
//   QSEARCH TIME CHECK FIX (bugfix):
//     quiescence() was checking (stats_.nodes & 2047) for its periodic time
//     test, but stats_.nodes is only incremented in negamax(), not in
//     quiescence(). During deep tactical sequences (many recursive qsearch
//     calls without returning to negamax), the condition could never re-fire,
//     leaving the time check effectively disabled for the duration of those
//     sequences. Fixed by using stats_.qnodes in the quiescence time check.
//
//   NODES / NPS REPORTING FIX (bugfix):
//     All UCI output (info line, heartbeat, new-best) was reporting
//     stats_.nodes, which only counts negamax() calls. stats_.qnodes (the
//     quiescence node counter) was never included. In tactical positions
//     qsearch can produce 5-10x more nodes than the main search, causing
//     the reported node count and NPS to be severely underestimated. Fixed
//     by using (stats_.nodes + stats_.qnodes) everywhere nodes are reported.
//
//   SEEN[] ARRAY SIZE FIX (bugfix):
//     The PV repetition detection array was declared as
//     seen[MAX_GAME_HISTORY + MAX_PLY] (1152 slots). In the worst case it
//     needs history_ply+1 entries for the seed (up to 1025) plus one per
//     PV move (up to 128), for a total of 1153. The guard
//     "if (seen_count < ...)" prevented an actual overflow but silently
//     dropped the last position from tracking. Fixed by declaring
//     seen[MAX_GAME_HISTORY + MAX_PLY + 2].
//
// Move ordering tiers (highest to lowest):
//   1. TT move    — best move from a previous search at this position
//   2. Captures   — ordered by MVV-LVA (Most Valuable Victim, Least Valuable Attacker)
//   3. Promotions — treated similarly to captures
//   4. Killer 1   — most recent quiet move that caused a beta cutoff at this ply
//   5. Killer 2   — second killer move at this ply
//   6. Quiet moves — history heuristic planned for 1.3; no further ordering here
// =============================================================================

#include "search.h"
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <cstring>

// Global instance
Search Searcher;

// =============================================================================
// HELPERS
// =============================================================================

// Convert a Move to its UCI string (e.g. "e2e4", "e7e8q").
// Duplicated from uci.cpp to keep search.cpp self-contained — search needs
// this for currmove and new-best output without depending on the UCI layer.
static std::string move_to_uci(Move m) {
    if (m == MOVE_NONE) return "0000";
    Square from = from_sq(m);
    Square to   = to_sq(m);
    std::string s;
    s += char('a' + file_of(from));
    s += char('1' + rank_of(from));
    s += char('a' + file_of(to));
    s += char('1' + rank_of(to));
    if (move_type(m) == PROMOTION) {
        const char promo[] = "nbrq";
        s += promo[promotion_type(m) - KNIGHT];
    }
    return s;
}

// =============================================================================
// MOVE ORDERING
// =============================================================================
// Alpha-beta efficiency depends heavily on searching good moves first.
// We assign a score to each move and sort by descending score before searching.

// MVV-LVA table indexed by [attacker piece type][victim piece type].
// Higher value = search this capture first.
// Example: PxQ (pawn captures queen) scores 505, QxP scores 101.
static const int MVV_LVA[7][7] = {
    // victim:    x    P    N    B    R    Q    K
    {  0,   0,   0,   0,   0,   0,   0  },  // attacker: none
    {  0, 105, 205, 305, 405, 505, 605  },  // attacker: P
    {  0, 104, 204, 304, 404, 504, 604  },  // attacker: N
    {  0, 103, 203, 303, 403, 503, 603  },  // attacker: B
    {  0, 102, 202, 302, 402, 502, 602  },  // attacker: R
    {  0, 101, 201, 301, 401, 501, 601  },  // attacker: Q
    {  0, 100, 200, 300, 400, 500, 600  },  // attacker: K
};

constexpr int ORDER_TT_MOVE = 1000000;  // Always search TT move first
constexpr int ORDER_CAPTURE =  100000;  // Base score for captures (+ MVV-LVA)
constexpr int ORDER_KILLER1 =   90000;  // Most recent killer for this ply
constexpr int ORDER_KILLER2 =   80000;  // Second killer for this ply
constexpr int ORDER_QUIET   =       0;  // Quiet moves: history heuristic planned for 1.3

// Score drop threshold for dynamic time extension.
// If the best score drops by this many centipawns between iterations, the
// engine extends the soft time limit to search for a better response.
constexpr int SCORE_DROP_THRESHOLD = 30;

int Search::move_score(const Board& board, Move m,
                        Move tt_move, int ply) const {
    if (m == tt_move) return ORDER_TT_MOVE;

    Square to     = to_sq(m);
    Piece  victim = board.piece_at(to);

    // Captures and en passant: order by MVV-LVA
    if (victim != NO_PIECE || move_type(m) == EN_PASSANT) {
        Piece attacker = board.piece_at(from_sq(m));
        int   att_type = type_of(attacker);
        int   vic_type = (move_type(m) == EN_PASSANT) ? PAWN : type_of(victim);
        return ORDER_CAPTURE + MVV_LVA[att_type][vic_type];
    }

    // Promotions: treat like captures (queen promotion is almost always best)
    if (move_type(m) == PROMOTION)
        return ORDER_CAPTURE + 50;

    // Killer moves: quiet moves that previously caused a beta cutoff at this ply.
    // Only checked for quiet moves — captures are already handled above.
    if (m == killers_[ply][0]) return ORDER_KILLER1;
    if (m == killers_[ply][1]) return ORDER_KILLER2;

    return ORDER_QUIET;
}

void Search::sort_moves(const Board& board, MoveList& moves,
                         int start, Move tt_move, int ply) const {
    // Selection sort: find the highest-scored move and swap it to position i.
    // O(n^2) but fast enough for move lists of ~30-50 moves per node.
    for (int i = start; i < moves.count; i++) {
        int best_idx   = i;
        int best_score = move_score(board, moves.moves[i], tt_move, ply);

        for (int j = i + 1; j < moves.count; j++) {
            int s = move_score(board, moves.moves[j], tt_move, ply);
            if (s > best_score) {
                best_score = s;
                best_idx   = j;
            }
        }
        std::swap(moves.moves[i], moves.moves[best_idx]);
    }
}

void Search::store_killer(Move m, int ply) {
    // Don't store the same move twice — it's already in slot 0
    if (m == killers_[ply][0]) return;
    // Shift the current best killer to slot 1, store the new one in slot 0
    killers_[ply][1] = killers_[ply][0];
    killers_[ply][0] = m;
}

// =============================================================================
// QUIESCENCE SEARCH
// =============================================================================
// Called at depth=0 instead of returning evaluate() directly.
// Searches captures until a "quiet" position is reached, preventing the
// horizon effect: the engine thinking it gained material without seeing the
// immediate recapture.
//
// Uses "stand pat": if the static eval already exceeds beta, we assume the
// side to move can maintain this score and prune immediately.

Score Search::quiescence(Board& board, Score alpha, Score beta, int ply) {
    // If the search was aborted (time expired), return immediately so the
    // entire call stack unwinds cleanly without corrupting the board state.
    if (abort_search_) return 0;

    stats_.qnodes++;

    // Track the maximum depth reached including quiescence for seldepth reporting
    if (ply > stats_.seldepth) stats_.seldepth = ply;

    // Time check: every 2048 quiescence nodes to minimize overhead.
    // Uses stats_.qnodes (not stats_.nodes) because stats_.nodes is only
    // incremented in negamax(). During long tactical sequences, negamax()
    // is never re-entered, so stats_.nodes stays fixed and a nodes-based
    // check here would never fire. Using qnodes ensures the check triggers
    // regularly regardless of how deep the capture search goes.
    if ((stats_.qnodes & 2047) == 0 && TM.should_stop()) {
        abort_search_ = true;
        return 0;
    }

    // Stand pat: assume we can always do at least as well as the static eval
    Score stand_pat = evaluate(board);
    if (stand_pat >= beta) return beta;
    if (stand_pat > alpha) alpha = stand_pat;

    // Hard limit to prevent stack overflow in extreme positions
    if (ply >= MAX_PLY - 1) return stand_pat;

    MoveList captures;
    generate_captures(board, captures);
    // Killers are irrelevant in quiescence (captures only) — pass ply anyway
    // for interface consistency but killer slots will never match captures
    sort_moves(board, captures, 0, MOVE_NONE, ply);

    for (int i = 0; i < captures.count; i++) {
        Move m = captures.moves[i];
        if (!board.is_legal(m)) continue;

        board.make_move(m);
        Score score = -quiescence(board, -beta, -alpha, ply + 1);
        board.unmake_move(m);

        // Propagate abort up the call stack
        if (abort_search_) return 0;

        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }

    return alpha;
}

// =============================================================================
// NEGAMAX ALPHA-BETA
// =============================================================================
// Negamax with alpha-beta pruning. Returns the score from the perspective
// of the side to move (positive = good for the mover).
//
// Alpha = best score the current player is guaranteed so far
// Beta  = best score the opponent is guaranteed so far
// If score >= beta, the opponent won't allow this position — prune.

Score Search::negamax(Board& board, Score alpha, Score beta,
                       int depth, int ply, bool do_null) {
    // If the search was aborted (time expired), return immediately so the
    // entire call stack unwinds cleanly without corrupting the board state.
    if (abort_search_) return 0;

    stats_.nodes++;

    // Initialize PV length for this ply. Must be done before any early return
    // that could leave pv_length_[ply] stale from a previous search iteration.
    pv_length_[ply] = 0;

    // Time check: every 2048 nodes to minimize overhead.
    // Set the abort flag instead of returning directly — this ensures every
    // level of the call stack sees the flag and returns, so all make_move()
    // calls are matched by unmake_move() calls and the board stays consistent.
    if ((stats_.nodes & 2047) == 0 && TM.should_stop()) {
        abort_search_ = true;
        return 0;
    }

    // Heartbeat: if no SUBSTANTIVE output has been emitted for
    // HEARTBEAT_INTERVAL_MS, print a status line. Uses last_heartbeat_ms_,
    // not last_output_ms_ — currmove lines must not reset this timer, since
    // they carry no status info and would otherwise suppress the heartbeat
    // during long iterations with many root moves.
    if ((stats_.nodes & 2047) == 0) {
        int64_t now = TM.elapsed_ms();
        if (now - last_heartbeat_ms_ >= HEARTBEAT_INTERVAL_MS) {
            uint64_t total_nodes = stats_.nodes + stats_.qnodes;
            int nps = (now > 0) ? int(total_nodes * 1000 / now) : 0;
            std::cout << "info depth "  << current_depth_
                      << " nodes "      << total_nodes
                      << " nps "        << nps
                      << " time "       << now
                      << " hashfull "   << TT.hashfull() << "\n"
                      << "info string still searching"
                      << " -- last completed depth " << (current_depth_ - 1)
                      << ", " << (now - last_heartbeat_ms_) / 60000
                      << " min since last output"
                      << " -- " << (total_nodes / 1000000) << "M nodes"
                      << ", " << nps << " nps\n" << std::flush;
            last_output_ms_    = now;
            last_heartbeat_ms_ = now;
        }
    }

    // Draw detection: repetition or 50-move rule.
    // Skip at ply=0 so the root always returns a real move.
    if (ply > 0 && (board.is_repetition() || board.half_move_clock >= 100))
        return SCORE_DRAW;

    // At depth 0, resolve captures before evaluating
    if (depth <= 0)
        return quiescence(board, alpha, beta, ply);

    // Hard depth limit to prevent stack overflow
    if (ply >= MAX_PLY - 1)
        return evaluate(board);

    // Compute in_check once — used for NMP guard below and for checkmate
    // detection at the bottom. Avoids calling board.in_check() twice per node.
    bool in_check = board.in_check();

    // -------------------------------------------------------------------------
    // MOVE GENERATION
    // -------------------------------------------------------------------------
    // Generate the full move list once here and reuse it throughout this node:
    // for TT move validation below, and for the main move loop further down.
    // Generating twice (once for validation, once for the loop) was the 1.1
    // behavior — eliminated as part of the 1.2 pre-work cleanup.
    MoveList moves;
    generate_all_moves(board, moves);

    // -------------------------------------------------------------------------
    // TRANSPOSITION TABLE PROBE
    // -------------------------------------------------------------------------
    Move    tt_move  = MOVE_NONE;
    TTEntry tt_entry;

    if (TT.probe(board.hash, tt_entry)) {
        stats_.tt_hits++;
        tt_move = tt_entry.move;

        // Guard against hash collisions producing garbage moves.
        // A collided entry can have a move that is completely invalid in this
        // position (wrong piece, wrong color, out-of-range squares). Using it
        // for ordering is harmless if it doesn't match any generated move, but
        // verifying it is pseudo-legal avoids subtle issues downstream.
        if (tt_move != MOVE_NONE) {
            bool found = false;
            for (int i = 0; i < moves.count; i++) {
                if (moves.moves[i] == tt_move) { found = true; break; }
            }
            if (!found) tt_move = MOVE_NONE;
        }

        // Use the stored score if it was searched at least as deep and
        // the bound type is compatible with our current search window.
        // At ply 0 (root) we never return early — we always search all moves
        // so that root_best_move_ is set from the actual move loop, never from
        // a potentially stale or hash-collided TT entry. The TT move is still
        // used for move ordering (tt_move is passed to sort_moves below).
        if (tt_entry.depth >= depth && ply > 0) {
            Score tt_score = score_from_tt(tt_entry.score, ply);

            if (tt_entry.bound == BOUND_EXACT
                || (tt_entry.bound == BOUND_LOWER && tt_score >= beta)
                || (tt_entry.bound == BOUND_UPPER && tt_score <= alpha))
            {
                return tt_score;
            }
        }
    }

    // -------------------------------------------------------------------------
    // NULL MOVE PRUNING (NMP)
    // -------------------------------------------------------------------------
    // Idea: if we pass the turn entirely (make a "null move") and the resulting
    // position still scores >= beta at a shallower depth, the position is so
    // strong that beta is almost certainly exceeded in the real search too.
    // We prune immediately and return beta (a lower bound on the true score).
    //
    // The null move search uses a zero window (-beta, -beta+1): we only need
    // to detect a beta cutoff, not measure the exact score.
    //
    // Guards — NMP is disabled when:
    //   1. do_null == false: the previous move was already a null move.
    //      Two consecutive null moves (passes) is unsound and causes the
    //      search to loop. do_null is set to false for the recursive call.
    //   2. in_check: passing while in check is illegal and the zugzwang
    //      argument breaks down — any move might be forced.
    //   3. ply == 0: we never prune at the root; root_best_move_ must be
    //      set from the actual move loop.
    //   4. depth < NMP_MIN_DEPTH: at shallow depths the cost of the reduced
    //      search is not worth the potential pruning benefit.
    //   5. No non-pawn material: in positions with only pawns and kings,
    //      zugzwang is common — passing the move can be genuinely terrible.
    //      We check that the side to move has at least one piece beyond pawns
    //      and king before allowing NMP.
    if (   do_null
        && !in_check
        && ply > 0
        && depth >= NMP_MIN_DEPTH
        && (board.by_color[board.side_to_move]
            & ~board.pieces[PAWN]
            & ~board.pieces[KING]))
    {
        board.make_null_move();
        // do_null=false prevents the child node from making another null move.
        // The reduced depth is (depth - 1 - NMP_REDUCTION): the -1 accounts
        // for the move itself (like any other recursive call), and
        // -NMP_REDUCTION is the additional reduction that makes NMP efficient.
        Score null_score = -negamax(board, -beta, -beta + 1,
                                    depth - 1 - NMP_REDUCTION, ply + 1, false);
        board.unmake_null_move();

        if (abort_search_) return 0;

        // If even with a free move the opponent cannot beat beta, we prune.
        // We do not update the TT here: the null move score is not a reliable
        // bound on the true score of this position (it was searched with a
        // reduced depth and a free move for the opponent).
        if (null_score >= beta)
            return beta;
    }

    // -------------------------------------------------------------------------
    // MOVE SEARCH
    // -------------------------------------------------------------------------
    sort_moves(board, moves, 0, tt_move, ply);

    int   legal_count = 0;
    Move  best_move   = MOVE_NONE;
    Score best_score  = -SCORE_INFINITE;
    Score orig_alpha  = alpha;

    for (int i = 0; i < moves.count; i++) {
        Move m = moves.moves[i];
        if (!board.is_legal(m)) continue;
        legal_count++;

        // currmove: at the root, emit which move is being explored and its
        // position in the ordered list. Standard UCI fields — GUIs display
        // this in a dedicated panel. Not emitted at depth 1 (too fast to
        // be meaningful) or in inner nodes (ply > 0).
        if (ply == 0 && current_depth_ > 1) {
            int64_t now = TM.elapsed_ms();
            std::cout << "info depth "       << current_depth_
                      << " currmove "        << move_to_uci(m)
                      << " currmovenumber "  << legal_count
                      << "\n" << std::flush;
            last_output_ms_ = now;
            // Note: last_heartbeat_ms_ is NOT updated here — currmove lines
            // fire at high frequency and must not suppress the heartbeat
            // during long iterations.
        }

        board.make_move(m);
        Score score = -negamax(board, -beta, -alpha, depth - 1, ply + 1);
        board.unmake_move(m);

        // If aborted mid-child, unwind without storing anything.
        // Storing a score from an incomplete search would pollute the TT and
        // cause the next search to make decisions based on garbage results.
        if (abort_search_) return 0;

        if (score > best_score) {
            best_score = score;
            best_move  = m;

            // At the root (ply 0), record the best move directly.
            // We never do TT early returns at the root, so this is always set
            // from the actual move loop — guaranteed to be legal in the current
            // position and independent of TT state (always-replace can overwrite
            // the root entry during the search before go() reads it back).
            if (ply == 0) {
                root_best_move_ = m;

                // new best: emit only when the move actually changes relative
                // to the previous completed iteration. Without this guard,
                // alpha resets to -INFINITY at the start of each depth, so
                // the first alpha-raise would fire every iteration even for
                // the same move. Also suppressed at depth 1 (no prior best).
                if (current_depth_ > 1 && m != prev_best_move_) {
                    int64_t now = TM.elapsed_ms();

                    // Format elapsed time as h:mm:ss,ms for readability at
                    // long time controls. Pure integer arithmetic -- no cost.
                    int64_t ms   =  now % 1000;
                    int64_t secs = (now / 1000) % 60;
                    int64_t mins = (now / 60000) % 60;
                    int64_t hrs  =  now / 3600000;
                    char time_buf[32];
                    std::snprintf(time_buf, sizeof(time_buf),
                                  "%lld:%02lld:%02lld,%03lld",
                                  (long long)hrs, (long long)mins,
                                  (long long)secs, (long long)ms);

                    std::string score_str;
                    if (is_mate_score(score)) {
                        int mtm = (SCORE_MATE - std::abs(score) + 1) / 2;
                        score_str = "mate " + std::to_string(score > 0 ? mtm : -mtm);
                    } else {
                        score_str = (score >= 0 ? "+" : "") + std::to_string(score) + "cp";
                    }
                    int nps = (now > 0) ? int((stats_.nodes + stats_.qnodes) * 1000 / now) : 0;
                    std::cout << "info string new best: "
                              << board.move_to_san(m)
                              << " " << score_str
                              << " -- move " << legal_count
                              << "/" << root_move_count_
                              << " depth " << current_depth_
                              << " nodes " << (stats_.nodes + stats_.qnodes)
                              << " nps " << nps
                              << " [" << time_buf << "]\n" << std::flush;
                    last_output_ms_    = now;
                    last_heartbeat_ms_ = now;
                    // new-best is substantive output (a genuinely new best
                    // move was found at the root) so it resets the heartbeat
                    // timer. currmove lines do NOT reset it — they fire at
                    // high frequency and carry no status info.
                }
            }

            if (score > alpha) {
                alpha = score;

                // Update the triangular PV array.
                // We copy this move into pv_table_[ply][0], then append the
                // child's PV (pv_table_[ply+1]) to build the full line from
                // this ply onward. This only happens when alpha is raised —
                // a move that fails low (score <= alpha) does not belong in
                // the PV even if it is the best move seen so far.
                pv_table_[ply][0] = m;
                for (int j = 0; j < pv_length_[ply + 1]; j++)
                    pv_table_[ply][j + 1] = pv_table_[ply + 1][j];
                pv_length_[ply] = pv_length_[ply + 1] + 1;

                if (alpha >= beta) {
                    // Beta cutoff: store as killer if this is a quiet move.
                    // Captures are already well-ordered by MVV-LVA so the
                    // killer heuristic is only useful for quiet moves.
                    // En passant and promotions are excluded — they are rare
                    // enough that killer slots are better used for regular quiets.
                    if (board.piece_at(to_sq(m)) == NO_PIECE
                        && move_type(m) != EN_PASSANT
                        && move_type(m) != PROMOTION)
                    {
                        store_killer(m, ply);
                    }

                    // Store as lower bound: the real score may be even higher
                    TT.store(board.hash, best_move, best_score,
                             depth, BOUND_LOWER, ply);
                    return alpha;
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // CHECKMATE AND STALEMATE
    // -------------------------------------------------------------------------
    if (legal_count == 0) {
        // in_check was computed before the move loop — reuse it here.
        return in_check
            ? -SCORE_MATE + ply  // Checkmate: prefer faster mates (lower ply)
            : SCORE_DRAW;        // Stalemate
    }

    // -------------------------------------------------------------------------
    // STORE IN TRANSPOSITION TABLE
    // -------------------------------------------------------------------------
    // EXACT if we improved alpha; UPPER if no move improved alpha (all failed low)
    BoundType bound = (best_score <= orig_alpha) ? BOUND_UPPER : BOUND_EXACT;
    TT.store(board.hash, best_move, best_score, depth, bound, ply);

    return best_score;
}

// =============================================================================
// ITERATIVE DEEPENING — TOP LEVEL
// =============================================================================
// Search at depth 1, 2, 3, ... until time runs out.
// Each iteration uses results from the previous one (via the TT) to search
// more efficiently. If time expires mid-search, we use the last fully
// completed iteration's result.
//
// Best move resolution per iteration (priority order):
//   1. root_best_move_: set directly in negamax() when ply==0 finds a new best.
//      Always comes from the actual move loop — we never do TT early returns
//      at the root, so this is guaranteed to be a legal move in the position.
//   2. safe_move: pre-seeded before the loop with the first legal move.
//      Last resort — guarantees bestmove is never "0000" under any circumstances,
//      including if depth 1 aborts before finding any move (extreme time pressure).
//
// After each completed iteration:
//   - Check soft_stop() instead of should_stop() — allows time extensions.
//   - If the best move changed between iterations, extend soft limit x1.5
//     (PV instability: the engine is reconsidering, needs more time).
//   - If the score dropped >= SCORE_DROP_THRESHOLD, extend soft limit x1.25
//     (the engine found something bad and needs time to find a response).
//   Both extensions can apply in the same iteration (multiplicative).

SearchResult Search::go(Board& board) {
    stats_          = SearchStats{};
    abort_search_   = false;
    last_output_ms_ = 0;
    last_heartbeat_ms_ = 0;
    current_depth_  = 0;
    root_move_count_= 0;
    prev_best_move_ = MOVE_NONE;

    // Clear killer table: killers from a previous position are irrelevant
    // and can mislead move ordering for the new position.
    std::memset(killers_, 0, sizeof(killers_));

    TM.start(board.side_to_move);

    SearchResult result;
    Score        last_score = 0;
    Score        prev_score = 0;
    Move         last_move  = MOVE_NONE;
    Move         prev_move  = MOVE_NONE;
    int          last_depth = 0;

    // Pre-seed safe_move with the first legal move before the search starts.
    // Last-resort fallback — guarantees bestmove is never "0000" even if depth 1
    // aborts instantly (extreme time pressure) and both root_best_move_ and the
    // TT probe fail. Updated to a better move as soon as an iteration completes.
    Move safe_move = MOVE_NONE;
    {
        MoveList seed_moves;
        generate_all_moves(board, seed_moves);
        for (int i = 0; i < seed_moves.count; i++) {
            if (board.is_legal(seed_moves.moves[i])) {
                safe_move = seed_moves.moves[i];
                break;
            }
        }
    }

    int max_depth = (TM.depth_limit > 0) ? TM.depth_limit : MAX_PLY;

    for (int depth = 1; depth <= max_depth; depth++) {
        // Reset per-iteration state.
        // root_best_move_ must start as MOVE_NONE so we can detect whether
        // negamax() reached the move loop at the root or returned via TT hit.
        // pv_length_ must be zeroed so stale PV lines from the previous
        // iteration do not bleed into the new one at any ply.
        stats_.seldepth = depth;
        root_best_move_ = MOVE_NONE;
        current_depth_  = depth;
        std::memset(pv_length_, 0, sizeof(pv_length_));

        // Count legal moves at the root for currmove "N/total" formatting.
        // Done once per iteration; the count is stable within an iteration.
        root_move_count_ = 0;
        {
            MoveList root_moves;
            generate_all_moves(board, root_moves);
            for (int i = 0; i < root_moves.count; i++)
                if (board.is_legal(root_moves.moves[i])) root_move_count_++;
        }

        Score score = negamax(board, -SCORE_INFINITE, SCORE_INFINITE, depth, 0);

        // Discard incomplete results: if aborted mid-iteration the score and
        // root_best_move_ are unreliable — keep the last fully completed result.
        // Depth 1 is never discarded since there is no prior fallback.
        if (abort_search_ && depth > 1) break;

        // root_best_move_ is always set from the move loop at ply 0 — we never
        // do TT early returns at the root. MOVE_NONE here means the position
        // has no legal moves (mate or stalemate); safe_move is the fallback.
        Move iter_move = root_best_move_;
        // Update tracking variables only if the move is legal in the current
        // position. This guards against stale TT entries that passed an earlier
        // is_legal() check but became invalid as the position evolved across
        // iterations (e.g. a move that was legal at iteration N is no longer
        // legal at iteration N+1 after the board state changed).
        if (iter_move != MOVE_NONE && board.is_legal(iter_move)) {
            last_move = iter_move;
            safe_move = iter_move;
        }

        last_score = score;
        last_depth = depth;

        int elapsed = TM.elapsed_ms();
        uint64_t total_nodes = stats_.nodes + stats_.qnodes;
        int nps     = (elapsed > 0) ? int(total_nodes * 1000 / elapsed) : 0;

        // UCI info line
        std::cout << "info depth "   << depth
                  << " seldepth "    << stats_.seldepth;

        if (is_mate_score(last_score)) {
            int moves_to_mate = (SCORE_MATE - std::abs(last_score) + 1) / 2;
            std::cout << " score mate "
                      << (last_score > 0 ? moves_to_mate : -moves_to_mate);
        } else {
            std::cout << " score cp " << last_score;
        }

        std::cout << " nodes "    << total_nodes
                  << " nps "      << nps
                  << " time "     << elapsed
                  << " hashfull " << TT.hashfull();

        // Print the full PV line from the triangular PV array.
        // We walk a copy of the board and verify each move with is_legal()
        // in the correct intermediate position (moves at ply > 0 are only
        // legal in their own positions, not at the root).
        // A seen[] array seeded with the full game history and root hash
        // detects repetitions: if any resulting position was seen before,
        // the move is not printed and the walk stops.
        if (pv_length_[0] > 0) {
            std::cout << " pv";
            Board pv_board = board;

            // Seed seen[] with the full game history plus the root position.
            // history[i].hash is the hash BEFORE move i was made, covering
            // all positions the opponent can claim threefold repetition on.
            // We then check after each make_move() whether the resulting hash
            // appears in seen[]. If so, the move leads to a repeated position
            // and we stop WITHOUT printing it — the engine will not play past
            // that point in a real game.
            uint64_t seen[MAX_GAME_HISTORY + MAX_PLY + 2];
            int      seen_count = 0;
            for (int i = 0; i < pv_board.history_ply; i++)
                seen[seen_count++] = pv_board.history[i].hash;
            seen[seen_count++] = pv_board.hash;  // root position itself

            for (int i = 0; i < pv_length_[0]; i++) {
                Move pv_move = pv_table_[0][i];
                if (!pv_board.is_legal(pv_move)) break;

                // Make the move first, then check for repetition.
                // Checking before make_move() would test the position we're
                // leaving, not the one we're entering — wrong in both directions.
                pv_board.make_move(pv_move);

                bool repeated = false;
                for (int j = 0; j < seen_count; j++) {
                    if (seen[j] == pv_board.hash) { repeated = true; break; }
                }
                if (repeated) break;

                if (seen_count < MAX_GAME_HISTORY + MAX_PLY)
                    seen[seen_count++] = pv_board.hash;

                Square from = from_sq(pv_move);
                Square to   = to_sq(pv_move);
                std::cout << ' '
                          << char('a' + file_of(from))
                          << char('1' + rank_of(from))
                          << char('a' + file_of(to))
                          << char('1' + rank_of(to));
                if (move_type(pv_move) == PROMOTION) {
                    const char promo[] = "nbrq";
                    std::cout << promo[promotion_type(pv_move) - KNIGHT];
                }
            }
        } else if (last_move != MOVE_NONE && board.is_legal(last_move)) {
            // Fallback: PV array empty but we have a best move from the root.
            // Should not happen in normal search but guards against edge cases
            // (e.g. depth 1 where no alpha raise occurred but a move was found).
            Square from = from_sq(last_move);
            Square to   = to_sq(last_move);
            std::cout << " pv "
                      << char('a' + file_of(from))
                      << char('1' + rank_of(from))
                      << char('a' + file_of(to))
                      << char('1' + rank_of(to));
            if (move_type(last_move) == PROMOTION) {
                const char promo[] = "nbrq";
                std::cout << promo[promotion_type(last_move) - KNIGHT];
            }
        }
        std::cout << "\n" << std::flush;
        last_output_ms_    = elapsed;
        last_heartbeat_ms_ = elapsed;  // end-of-iteration info line counts as substantive output
        // ---------------------------------------------------------------------
        // Check the soft limit after each completed iteration.
        if (TM.soft_stop()) break;

        // PV instability: the best move changed between iterations.
        // The engine is reconsidering its top choice — give it more time.
        if (depth > 1 && last_move != prev_move && last_move != MOVE_NONE)
            TM.extend_time(1.5, "PV change");

        // Score drop: the evaluation fell significantly from the previous
        // iteration. Extra time helps find the best defensive response.
        if (depth > 1 && (prev_score - last_score) >= SCORE_DROP_THRESHOLD)
            TM.extend_time(1.25, "score drop");

        // Save state for the next iteration's comparisons
        prev_move       = last_move;
        prev_score      = last_score;
        prev_best_move_ = last_move;  // used by negamax() ply==0 for new-best guard

        // Re-check soft limit after potential extensions
        if (TM.soft_stop()) break;
    }

    // Final bestmove: prefer last_move (best from last completed iteration),
    // fall back to safe_move (pre-seeded first legal move) if nothing better
    // was found. This guarantees bestmove is never "0000".
    result.best_move = (last_move != MOVE_NONE) ? last_move : safe_move;
    result.score     = last_score;
    result.depth     = last_depth;

    return result;
}
