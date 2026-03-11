// =============================================================================
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
// Move ordering tiers (highest to lowest):
//   1. TT move    — best move from a previous search at this position
//   2. Captures   — ordered by MVV-LVA (Most Valuable Victim, Least Valuable Attacker)
//   3. Promotions — treated similarly to captures
//   4. Killer 1   — most recent quiet move that caused a beta cutoff at this ply
//   5. Killer 2   — second killer move at this ply
//   6. Quiet moves — no further ordering in 1.1
// =============================================================================

#include "search.h"
#include <iostream>
#include <algorithm>
#include <cstring>

// Global instance
Search Searcher;

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
constexpr int ORDER_QUIET   =       0;  // Quiet moves: no bonus in 1.1

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

    // Time check: every 2048 nodes to minimize overhead.
    // Set the abort flag instead of returning directly — this ensures every
    // level of the call stack sees the flag and returns, so all make_move()
    // calls are matched by unmake_move() calls and the board stays consistent.
    if ((stats_.nodes & 2047) == 0 && TM.should_stop()) {
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
                       int depth, int ply) {
    // If the search was aborted (time expired), return immediately so the
    // entire call stack unwinds cleanly without corrupting the board state.
    if (abort_search_) return 0;

    stats_.nodes++;

    // Time check: every 2048 nodes to minimize overhead.
    // Set the abort flag instead of returning directly — same reasoning as
    // in quiescence(): ensures proper board state after unwinding.
    if ((stats_.nodes & 2047) == 0 && TM.should_stop()) {
        abort_search_ = true;
        return 0;
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
            MoveList pseudo;
            generate_all_moves(board, pseudo);
            bool found = false;
            for (int i = 0; i < pseudo.count; i++) {
                if (pseudo.moves[i] == tt_move) { found = true; break; }
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
    // MOVE GENERATION AND SEARCH
    // -------------------------------------------------------------------------
    MoveList moves;
    generate_all_moves(board, moves);
    sort_moves(board, moves, 0, tt_move, ply);

    int   legal_count = 0;
    Move  best_move   = MOVE_NONE;
    Score best_score  = -SCORE_INFINITE;
    Score orig_alpha  = alpha;

    for (int i = 0; i < moves.count; i++) {
        Move m = moves.moves[i];
        if (!board.is_legal(m)) continue;
        legal_count++;

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
            if (ply == 0) root_best_move_ = m;

            if (score > alpha) {
                alpha = score;
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
        return board.in_check()
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
    stats_        = SearchStats{};
    abort_search_ = false;

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
        stats_.seldepth = depth;
        root_best_move_ = MOVE_NONE;

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
        int nps     = (elapsed > 0) ? int(stats_.nodes * 1000 / elapsed) : 0;

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

        std::cout << " nodes "    << stats_.nodes
                  << " nps "      << nps
                  << " time "     << elapsed
                  << " hashfull " << TT.hashfull();

        // Print best move as the PV. is_legal() now checks piece ownership on
        // the from-square before doing the board copy, so ghost moves from TT
        // collisions are caught here without the overhead of generate_all_moves().
        if (last_move != MOVE_NONE && board.is_legal(last_move)) {
            std::cout << " pv ";
            Square from = from_sq(last_move);
            Square to   = to_sq(last_move);
            std::cout << char('a' + file_of(from))
                      << char('1' + rank_of(from))
                      << char('a' + file_of(to))
                      << char('1' + rank_of(to));
            if (move_type(last_move) == PROMOTION) {
                const char promo[] = "nbrq";
                std::cout << promo[promotion_type(last_move) - KNIGHT];
            }
        }
        std::cout << "\n" << std::flush;

        // ---------------------------------------------------------------------
        // DYNAMIC TIME MANAGEMENT
        // ---------------------------------------------------------------------
        // Check the soft limit after each completed iteration.
        if (TM.soft_stop()) break;

        // PV instability: the best move changed between iterations.
        // The engine is reconsidering its top choice — give it more time.
        if (depth > 1 && last_move != prev_move && last_move != MOVE_NONE)
            TM.extend_time(1.5);

        // Score drop: the evaluation fell significantly from the previous
        // iteration. Extra time helps find the best defensive response.
        if (depth > 1 && (prev_score - last_score) >= SCORE_DROP_THRESHOLD)
            TM.extend_time(1.25);

        // Save state for the next iteration's comparisons
        prev_move  = last_move;
        prev_score = last_score;

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
