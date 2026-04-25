// =============================================================================
// Last modified: 2026-04-19 15:56
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
// Facon 1.3 — Yunque (post-gauntlet fixes)
//
//   MATE REDUCTION ONE-SHOT (bugfix):
//     is_mate_score(last_score) is true on every iteration once a mate is
//     found. reduce_time(0.05, "mate found") was firing every depth (13, 14,
//     15...) causing x0.05^N collapse of the soft limit. Fixed by adding
//     mate_reduction_applied_ bool: the reduction fires at most once per
//     search, same pattern as easy_cumulative_.
//
//   COMPLEX POSITION TM PATH (time management):
//     extend_time() now accepts the current depth. When depth >= EMERGENCY_DEPTH
//     (25, an internal TM constant in timeman.cpp) and the new soft would exceed
//     the hard limit, hard is raised to match soft (capped at 50% of remaining
//     clock) instead of capping soft at hard. Both limits then rise together on
//     subsequent extensions. Only triggered by real instability — a stable
//     position at depth 25+ would have already fired the easy-move reduction.
//     TM.raise_emergency_limit() removed; EMERGENCY_DEPTH removed from search.h.
//
//   ASPIRATION WINDOW FAIL-LOW FIX (correctness):
//     The old fail-low handler set beta_asp = (alpha_asp + beta_asp) / 2,
//     which squeezed the upper bound. When re-searching a position where the
//     TT and LMR interactions cause the score to rebound, this artificial beta
//     ceiling could trigger a fail-high, forcing a third search (yo-yo effect).
//     Fixed: on fail-low, widen alpha only and leave beta untouched. On
//     fail-high, widen beta only and leave alpha untouched. Standard impl.
//
//   ASPIRATION WINDOW VERBOSITY (observability):
//     Fail-low and fail-high events emit "info string AW:" lines following
//     the same style as TM messages: two-letter prefix, description with --
//     separator, before->after window bounds, delta, elapsed timestamp.
//     "ASP" renamed to "AW" for two-letter consistency with "TM".
//
//   SEEN[] GUARD FIX (bugfix):
//     The seen[] array was declared with MAX_GAME_HISTORY + MAX_PLY + 2 slots
//     (1154) in 1.2, but the insertion guard still used MAX_GAME_HISTORY + MAX_PLY
//     (1152), stopping 2 entries early. Updated the guard to match the array size.
//
// Facon 1.3 — Yunque
//
//   DEPTH == 0 (prerequisite for LMR):
//     Changed quiescence entry condition from depth <= 0 to depth == 0.
//     With LMR, nodes can be called with a reduced depth that is already 0;
//     we want to enter quiescence exactly at that point. A negative depth
//     no longer silently enters quiescence — it causes a search hang,
//     making depth bugs immediately detectable. All reduction call sites
//     use std::max(0, reduced_depth) to ensure depth never goes negative.
//
//   NMP DEPTH FLOOR (bugfix):
//     The NMP recursive call now uses std::max(0, depth - 1 - NMP_REDUCTION).
//     Without this, at depth == NMP_MIN_DEPTH (3), NMP would pass depth -1.
//     With depth == 0, that -1 no longer entered quiescence, causing
//     infinite recursion and a hang starting at depth 4.
//
//   QUADRATIC EXTENSION SCALING:
//     extend_time() calls in go() now pre-scale the factor by
//     (depth^2 / EXTENSION_FULL_DEPTH^2). PV changes at depth 2-9 (normal,
//     not instability) have near-zero effect; extensions at depth 14+
//     apply the full factor. Cap: 2.0x total per move (in timeman.cpp).
//
//   EASY MOVE REDUCTION:
//     reduce_time() is called after each completed iteration when the
//     position is clearly resolved: mate found (x0.05), single legal move
//     (x0.1), or PV+score stable for 7+ consecutive iterations at depth > 12
//     (x EASY_REDUCE_FACTOR=0.40 — one-shot cut). The easy move trigger uses
//     == not >= to fire exactly once. If instability is later detected (PV
//     change or score drop), cancel_easy_move() restores the soft limit
//     before applying the extension so the extension acts on the full value.
//     stable_iters_ tracks the stable iteration count; resets on any change.
//
//   LMR (Late Move Reductions):
//     Quiet moves that appear after the first LMR_MIN_MOVES legal moves at
//     depth >= LMR_MIN_DEPTH are searched at reduced depth. Reduction formula:
//     reduction = log(depth) * log(move_number) / LMR_DIVISOR, floored at 1.
//     Skipped for: captures, en passant, promotions, killer moves, in check.
//     If the reduced search raises alpha, the move is re-searched at full depth.
//
//   HISTORY HEURISTIC:
//     history_[color][from][to] is incremented by depth^2 whenever a quiet
//     move causes a beta cutoff. In move_score(), quiet moves return their
//     history score instead of the flat ORDER_QUIET=0. This gives the move
//     ordering meaningful differentiation among quiet moves — historically good
//     moves rise in the list and are less likely to be LMR-reduced.
//     Table is capped at HISTORY_MAX and reset at the start of each search.
//
//   ASPIRATION WINDOWS:
//     Instead of searching with (-INFINITE, +INFINITE) at the start of each
//     iteration, we use a narrow window centered on the previous iteration's
//     score. If the search falls outside the window (fail-low or fail-high),
//     we widen the window and re-search. Most iterations stay inside the
//     window and save significant node count. Only applied from depth >= 4
//     (below that the score is too unstable to trust for windowing).
//     Also bypassed when the previous score is a mate score — a window of
//     +/-50cp around a mate score can produce pathological re-searches.
//     Initial window: +/- ASP_WINDOW (50cp). On each failure, the window
//     doubles on the failing side until the score fits or the window reaches
//     SCORE_INFINITE (full window, equivalent to no aspiration).
//
// Move ordering tiers (highest to lowest):
//   1. TT move    — best move from a previous search at this position
//   2. Captures   — ordered by MVV-LVA (Most Valuable Victim, Least Valuable Attacker)
//   3. Promotions — treated similarly to captures
//   4. Killer 1   — most recent quiet move that caused a beta cutoff at this ply
//   5. Killer 2   — second killer move at this ply
//   6. Quiet moves — ordered by history score (higher = caused more beta cutoffs)
//
// Facon 1.4 -- Hoja
//
//   MOVE_TO_UCI() DEDUPLICATION (cleanup):
//     The static move_to_uci() helper was duplicated in search.cpp and uci.cpp.
//     Moved to types.h as an inline function. Both files now use the shared
//     version via #include "types.h" (already included via search.h / uci.h).
//
//   LMR TABLE PRECALCULATION (speedup):
//     The per-move reduction formula log(depth)*log(move)/LMR_DIVISOR is now
//     precomputed into LMR_table[MAX_PLY][MAX_MOVES] at startup. The hot path
//     in negamax replaces two log() calls and a division with a single array
//     lookup. Same formula, same results — pure speedup.
//
//   MOVE SCORES COMPUTED ONCE (speedup):
//     sort_moves() previously called move_score() on every comparison in
//     the selection sort — O(n^2) calls for n moves. Now scores are computed
//     once into a parallel array (O(n) calls), and the sort operates on the
//     precomputed scores. The scores array is swapped in parallel with moves.
//
//   MAKE/UNMAKE LEGALITY (speedup):
//     negamax() and quiescence() no longer call is_legal() (which copies the
//     entire Board struct ~700 bytes per pseudo-legal move). Instead, the move
//     is made on the real board, the king-in-check test is performed directly,
//     and if the move is illegal it is unmade. Legal moves proceed to search
//     and are unmade after. Eliminates ~30 Board copies per node.
//     is_legal() is unchanged and still used by non-hot-path callers (perft,
//     move_to_san, PV walk, root move counting, parse_move).
//
//   FACON_DEBUG DIAGNOSTIC COUNTERS:
//     When compiled with -DFACON_DEBUG, LMR/NMP/TT counters are incremented
//     in the search and reported via "info string ST:" after each completed
//     iteration. Zero cost in release builds — all behind #ifdef.
//
//   CHECK EXTENSION:
//     When the side to move is in check, depth is extended by 1. Being in
//     check is a forcing situation with very few legal responses. Dropping
//     into quiescence while in check would miss critical evasions (qsearch
//     only considers captures). Applied before the depth==0 quiescence
//     entry, so a node at depth 0 in check gets extended to depth 1.
//
//   STATIC EXCHANGE EVALUATION (SEE):
//     see(board, m) simulates the full capture sequence on the target square
//     and returns the material gain/loss. Used in qsearch to skip losing
//     captures (SEE < 0), preventing the engine from wasting time on lines
//     like QxP where the pawn is defended.
//
//   REVERSE FUTILITY PRUNING (RFP):
//     At shallow depths (1-3), if the static eval minus a margin already
//     exceeds beta, the node is pruned entirely — like a cheaper NMP without
//     the null move search. Margin: 100cp per depth level.
//
//   MOVE-LEVEL FUTILITY PRUNING:
//     At depth 1-2, quiet moves (non-capture, non-promotion) are skipped if
//     static_eval + margin is below alpha. At least one legal move is always
//     searched to detect checkmate/stalemate. Margin: 150cp per depth level.
// =============================================================================

#include "search.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Global instance
Search Searcher;

// LMR reduction table — precomputed at startup by init_lmr_table().
int LMR_table[MAX_PLY][MAX_MOVES];

void init_lmr_table() {
    // LMR_DIVISOR (2.25) is defined as Search::LMR_DIVISOR in search.h.
    // Hardcoded here because the init function is not a class member.
    // If LMR_DIVISOR changes, update this value too.
    constexpr double divisor = 2.25;
    for (int depth = 0; depth < MAX_PLY; depth++) {
        for (int move = 0; move < MAX_MOVES; move++) {
            if (depth < 1 || move < 1)
                LMR_table[depth][move] = 0;
            else
                LMR_table[depth][move] = int(std::log(double(depth))
                                           * std::log(double(move))
                                           / divisor);
        }
    }
}

// =============================================================================
// STATIC EXCHANGE EVALUATION (SEE)
// =============================================================================
// Evaluates a capture by simulating the full exchange sequence on the target
// square. Returns the material gain (positive) or loss (negative) from the
// perspective of the side making the initial capture.
//
// Algorithm:
//   1. Record the initial capture value in gain[0].
//   2. Remove the moving piece from occupancy (may reveal x-ray attackers).
//   3. Find the cheapest attacker of the defending side on the target square.
//   4. gain[d] = value_of_piece_just_captured - gain[d-1]  (negamax-style).
//   5. Remove attacker, recompute attackers, switch sides. Repeat.
//   6. Propagate backwards: at each step, the side can choose NOT to recapture
//      if it would make them worse off.
//
// Used in quiescence search to prune losing captures (SEE < 0) and in move
// ordering to distinguish good captures from bad ones.

static int see(const Board& board, Move m) {
    Square from = from_sq(m);
    Square to   = to_sq(m);

    // Determine the initial capture value and attacker piece type
    int gain[64];
    int d = 0;

    PieceType attacker_type = type_of(board.piece_at(from));

    if (move_type(m) == EN_PASSANT) {
        gain[d] = PIECE_VALUE[PAWN];
    } else if (move_type(m) == PROMOTION) {
        Piece victim = board.piece_at(to);
        gain[d] = (victim != NO_PIECE ? PIECE_VALUE[type_of(victim)] : 0)
                + PIECE_VALUE[promotion_type(m)] - PIECE_VALUE[PAWN];
        attacker_type = promotion_type(m);
    } else {
        Piece victim = board.piece_at(to);
        if (victim == NO_PIECE) return 0;
        gain[d] = PIECE_VALUE[type_of(victim)];
    }

    // Build occupancy with the moving piece removed from its origin
    Bitboard occ = board.occupancy();
    occ ^= (1ULL << from);

    // For en passant, also remove the captured pawn from occupancy
    if (move_type(m) == EN_PASSANT) {
        Square ep_captured = (board.side_to_move == WHITE)
                           ? Square(to - 8) : Square(to + 8);
        occ ^= (1ULL << ep_captured);
    }

    // Get all attackers to the target square with updated occupancy
    Bitboard attackers = board.all_attackers_to(to, occ) & occ;

    // The opponent recaptures first
    Color side = ~board.side_to_move;

    while (true) {
        d++;

        // Find attackers belonging to the current side
        Bitboard side_attackers = attackers & board.by_color[side];
        if (!side_attackers) break;

        // Find the cheapest attacker (PAWN < KNIGHT < BISHOP < ROOK < QUEEN < KING)
        PieceType pt;
        for (pt = PAWN; pt <= KING; pt = PieceType(pt + 1)) {
            if (side_attackers & board.pieces[pt]) break;
        }

        // Negamax gain: what we capture minus what the previous side gained
        gain[d] = PIECE_VALUE[attacker_type] - gain[d - 1];

        // Pruning: if even capturing for free can't beat the current best,
        // stop early (the side to move will choose not to recapture)
        if (std::max(-gain[d - 1], gain[d]) < 0) break;

        attacker_type = pt;

        // Remove the cheapest attacker from occupancy (x-ray discovery)
        occ ^= (1ULL << lsb(side_attackers & board.pieces[pt]));

        // Recompute attackers with the new occupancy
        attackers = board.all_attackers_to(to, occ) & occ;

        side = ~side;
    }

    // Propagate backwards: at each ply, the capturing side chooses the best
    // option between capturing (gain[d]) and not capturing (-gain[d-1]).
    while (--d) {
        gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
    }

    return gain[0];
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
// Quiet moves: scored by history_[color][from][to], range [0, HISTORY_MAX=50000].
// This sits below ORDER_KILLER2 so killers always outrank history-scored quiets.

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

    // History heuristic: quiet moves are ordered by their accumulated bonus.
    // Moves that have caused beta cutoffs in previous searches of similar
    // positions are scored higher and searched earlier.
    Color us = board.side_to_move;
    return history_[us][from_sq(m)][to_sq(m)];
}

void Search::sort_moves(const Board& board, MoveList& moves,
                         int start, Move tt_move, int ply) const {
    // Score all moves once into a parallel array, then selection-sort
    // using the precomputed scores. Previously move_score() was called
    // per comparison (O(n^2) calls); now it is called once per move (O(n)).
    int scores[MAX_MOVES];
    for (int i = start; i < moves.count; i++)
        scores[i] = move_score(board, moves.moves[i], tt_move, ply);

    // Selection sort: find the highest-scored move and swap it to position i.
    // O(n^2) comparisons but only O(n) score computations. Fast enough for
    // move lists of ~30-50 moves per node.
    for (int i = start; i < moves.count; i++) {
        int best_idx   = i;
        int best_score = scores[i];

        for (int j = i + 1; j < moves.count; j++) {
            if (scores[j] > best_score) {
                best_score = scores[j];
                best_idx   = j;
            }
        }
        std::swap(moves.moves[i], moves.moves[best_idx]);
        std::swap(scores[i], scores[best_idx]);
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

        // SEE pruning: skip captures with a negative exchange value.
        // A losing capture (e.g. QxP where the pawn is defended) wastes time
        // exploring a line that will fail low. Quiet queen promotions (no
        // victim on the target square) are exempt — they are always good.
        // Shortcut: if the victim is worth >= the attacker, the capture is
        // always winning or equal — skip the full SEE computation.
        if (board.piece_at(to_sq(m)) != NO_PIECE) {
            PieceType vic = type_of(board.piece_at(to_sq(m)));
            PieceType att = type_of(board.piece_at(from_sq(m)));
            if (PIECE_VALUE[vic] < PIECE_VALUE[att] && see(board, m) < 0)
                continue;
        }

        board.make_move(m);

        // Legality check: does the move leave our king in check?
        // Same make/unmake approach as negamax — avoids the ~700 byte Board
        // copy that is_legal() would perform.
        if (board.is_attacked(board.king_square(~board.side_to_move),
                              board.side_to_move)) {
            board.unmake_move(m);
            continue;
        }

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

    // Time check: every 2048 negamax nodes to minimize overhead.
    // Note: quiescence() has its own identical check using stats_.qnodes,
    // because stats_.nodes is never incremented there — during deep tactical
    // sequences the negamax counter is frozen and this check would never fire.
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

    // Hard depth limit to prevent stack overflow
    if (ply >= MAX_PLY - 1)
        return evaluate(board);

    // Compute in_check once — used for check extension, NMP guard, LMR guard,
    // and checkmate detection. Avoids calling board.in_check() multiple times.
    bool in_check = board.in_check();

    // -------------------------------------------------------------------------
    // CHECK EXTENSION
    // -------------------------------------------------------------------------
    // When the side to move is in check, extend the search by one ply.
    // Being in check is a forcing situation — the side to move has very few
    // legal responses (often just 1-3). Dropping into quiescence while in
    // check would miss critical evasions. The extension ensures that check
    // sequences are resolved at full depth before evaluation.
    // Applied BEFORE the depth==0 quiescence entry so that a node arriving
    // at depth 0 while in check gets extended to depth 1 (full search) instead
    // of entering qsearch where only captures are considered.
    if (in_check) depth++;

    // At depth 0, drop into quiescence to resolve captures before evaluating.
    // Must be == 0, not <= 0: with LMR a node can be called with a reduced
    // depth of exactly 0, and we want to enter quiescence at that point.
    // Using <= 0 would silently absorb negative depths from buggy reductions,
    // masking the error. With == 0, a negative depth falls through to full
    // search and causes an immediate, detectable hang. All reduction call
    // sites use std::max(0, reduced_depth) to prevent negative depths.
    if (depth == 0)
        return quiescence(board, alpha, beta, ply);

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
#ifdef FACON_DEBUG
                stats_.tt_cutoffs++;
#endif
                return tt_score;
            }
        }
    }

    // -------------------------------------------------------------------------
    // STATIC EVALUATION (for pruning decisions)
    // -------------------------------------------------------------------------
    // Computed once per node and reused by reverse futility pruning and
    // move-level futility pruning. Only needed when not in check AND at
    // shallow depths where pruning applies. At depth 4+ neither RFP nor
    // move-level futility fires, so calling evaluate() would be wasted work.
    Score static_eval = 0;
    bool can_prune = !in_check && ply > 0
                  && depth <= std::max(RFP_MAX_DEPTH, FUTILITY_MAX_DEPTH);
    if (can_prune) static_eval = evaluate(board);

    // -------------------------------------------------------------------------
    // REVERSE FUTILITY PRUNING (RFP) / Static Null Move Pruning
    // -------------------------------------------------------------------------
    // At shallow depths, if the static evaluation is so far above beta that
    // even a significant score drop would not bring it below beta, return
    // immediately. This is like a cheaper version of NMP that does not require
    // a null move search — just a static eval comparison.
    //
    // Guards: not in check, not at root, shallow depth (1-3).
    // Margin: 100cp per depth level (d1=100, d2=200, d3=300).
    if (can_prune && depth <= RFP_MAX_DEPTH
        && static_eval - RFP_MARGIN * depth >= beta)
    {
        return static_eval;
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
#ifdef FACON_DEBUG
        stats_.nmp_attempted++;
#endif
        board.make_null_move();
        // do_null=false prevents the child node from making another null move.
        // The reduced depth is (depth - 1 - NMP_REDUCTION): the -1 accounts
        // for the move itself (like any other recursive call), and
        // -NMP_REDUCTION is the additional reduction that makes NMP efficient.
        Score null_score = -negamax(board, -beta, -beta + 1,
                                    std::max(0, depth - 1 - NMP_REDUCTION), ply + 1, false);
        board.unmake_null_move();

        if (abort_search_) return 0;

        // If even with a free move the opponent cannot beat beta, we prune.
        // We do not update the TT here: the null move score is not a reliable
        // bound on the true score of this position (it was searched with a
        // reduced depth and a free move for the opponent).
        if (null_score >= beta) {
#ifdef FACON_DEBUG
            stats_.nmp_cutoffs++;
#endif
            return beta;
        }
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

        // LMR eligibility: checked BEFORE make_move so piece_at(to_sq(m))
        // correctly identifies captures (after make_move the captured piece
        // is gone and the square holds the moving piece instead).
        bool is_capture = (board.piece_at(to_sq(m)) != NO_PIECE)
                       || (move_type(m) == EN_PASSANT);
        bool is_killer  = (m == killers_[ply][0]) || (m == killers_[ply][1]);

        board.make_move(m);

        // Legality check via make/unmake: instead of copying the entire Board
        // (~700 bytes) in is_legal(), we make the move on the real board and
        // check if our king is left in check. If illegal, unmake and skip.
        // After make_move, side_to_move has flipped — our king (the side that
        // just moved) is ~side_to_move, and the attacker is side_to_move.
        if (board.is_attacked(board.king_square(~board.side_to_move),
                              board.side_to_move)) {
            board.unmake_move(m);
            continue;
        }

        legal_count++;

        // Move-level futility pruning: at shallow depths, if the static eval
        // plus a margin is still below alpha, quiet moves are unlikely to raise
        // alpha. Skip them to save search time. Only applied to non-captures,
        // non-promotions, when not in check, and after at least one legal move
        // has been searched (to detect checkmate/stalemate correctly).
        if (   can_prune
            && depth <= FUTILITY_MAX_DEPTH
            && legal_count > 1
            && !is_capture
            && move_type(m) != PROMOTION
            && static_eval + FUTILITY_MARGIN * depth <= alpha)
        {
            board.unmake_move(m);
            continue;
        }

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

        bool do_lmr     = (depth >= LMR_MIN_DEPTH)
                       && (legal_count > LMR_MIN_MOVES)
                       && !in_check
                       && !is_capture
                       && (move_type(m) != PROMOTION)
                       && !is_killer;

        Score score;
        if (do_lmr) {
            // Reduction from precomputed table (initialized at startup).
            // Floored at 1 and capped so reduced_depth >= 1 — we never LMR
            // directly into quiescence (depth == 0).
            int reduction     = std::max(1, LMR_table[depth][legal_count]);
            int reduced_depth = std::max(1, depth - 1 - reduction);

#ifdef FACON_DEBUG
            stats_.lmr_attempted++;
#endif
            // Zero-window search at reduced depth: only need to detect fail-low.
            score = -negamax(board, -alpha - 1, -alpha, reduced_depth, ply + 1);

            // If the reduced search did not fail low, re-search at full depth.
            if (score > alpha) {
#ifdef FACON_DEBUG
                stats_.lmr_re_searched++;
#endif
                score = -negamax(board, -beta, -alpha, depth - 1, ply + 1);
            }
        } else {
            score = -negamax(board, -beta, -alpha, depth - 1, ply + 1);
        }
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
                    // Beta cutoff: update move ordering heuristics for quiet moves.
                    // Captures are already well-ordered by MVV-LVA; killers and
                    // history only apply to quiet moves (no capture, no promotion,
                    // no en passant). is_capture was computed before make_move.
                    if (!is_capture && move_type(m) != PROMOTION)
                    {
                        store_killer(m, ply);

                        // History bonus: depth^2 so deeper cutoffs count more.
                        // Capped at HISTORY_MAX to prevent scores from drifting
                        // above ORDER_KILLER2 (which would break the ordering tier).
                        int bonus = depth * depth;
                        int& entry = history_[board.side_to_move][from_sq(m)][to_sq(m)];
                        entry = std::min(entry + bonus, HISTORY_MAX);
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
    stats_             = SearchStats{};
    abort_search_      = false;
    last_output_ms_    = 0;
    last_heartbeat_ms_ = 0;
    current_depth_     = 0;
    root_move_count_   = 0;
    prev_best_move_    = MOVE_NONE;
    stable_iters_             = 0;
    easy_cumulative_          = 1.0;
    mate_reduction_applied_   = false;

    // Clear killer and history tables: entries from a previous position are
    // irrelevant and can mislead move ordering for the new position.
    std::memset(killers_, 0, sizeof(killers_));
    std::memset(history_, 0, sizeof(history_));

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
    int legal_root_count = 0;
    {
        MoveList seed_moves;
        generate_all_moves(board, seed_moves);
        for (int i = 0; i < seed_moves.count; i++) {
            if (board.is_legal(seed_moves.moves[i])) {
                if (safe_move == MOVE_NONE) safe_move = seed_moves.moves[i];
                legal_root_count++;
            }
        }
    }

    // Forced move: only one legal move exists. No point searching — play it
    // immediately without spending any time. Every millisecond saved here is
    // banked for future moves where there IS a choice.
    // Emit a minimal info line so GUIs and match runners (fastchess, cutechess)
    // can extract a score — some crash or warn if bestmove arrives without any
    // preceding info line.
    if (legal_root_count == 1) {
        std::cout << "info depth 0 score cp 0 nodes 0 time 0 pv "
                  << move_to_uci(safe_move) << "\n" << std::flush;
        result.best_move = safe_move;
        result.score     = 0;
        result.depth     = 0;
        return result;
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

        // Legal move count at root: computed once before the loop.
        root_move_count_ = legal_root_count;

        // Aspiration windows: search with a narrow window around the previous
        // iteration's score. Saves nodes when the true score is close to the
        // expected value. On failure, widen the window on the failing side and
        // re-search. Only applied at depth >= 4 — below that scores are too
        // unstable to narrow around.
        Score score;
        int aw_fail_highs = 0;  // Count fail-highs for TM extension
        if (depth < 4 || last_score == 0 || is_mate_score(last_score)) {
            // Full window for early depths, no prior score, or when the previous
            // score was a mate. Mate scores near +/-SCORE_MATE would make the
            // window wrap around or sit entirely outside the valid range, causing
            // unnecessary re-searches or pathological behavior.
            score = negamax(board, -SCORE_INFINITE, SCORE_INFINITE, depth, 0);
        } else {
            // Start with a narrow window centered on the previous score.
            Score alpha_asp = last_score - ASP_WINDOW;
            Score beta_asp  = last_score + ASP_WINDOW;
            Score delta     = ASP_WINDOW;

            while (true) {
                score = negamax(board, alpha_asp, beta_asp, depth, 0);

                // Abort propagation: if the search was aborted, exit immediately.
                if (abort_search_) break;

                if (score <= alpha_asp) {
                    // Fail-low: score is below our window. Widen on the low side only.
                    // Do NOT modify beta_asp — squeezing the upper bound risks an
                    // artificial fail-high on re-search (yo-yo effect) when TT and
                    // LMR interactions cause the score to rebound above the narrowed
                    // ceiling. Standard: only ever widen in the failing direction.
                    int old_alpha = alpha_asp;
                    alpha_asp = std::max(score - delta, -SCORE_INFINITE);
                    delta    *= 2;

                    char t_now[32];
                    { int64_t ms=TM.elapsed_ms(); int64_t s=(ms/1000)%60,m=(ms/60000)%60,h=ms/3600000,ms3=ms%1000;
                      std::snprintf(t_now,sizeof(t_now),"%lld:%02lld:%02lld,%03lld",(long long)h,(long long)m,(long long)s,(long long)ms3); }
                    std::cout << "info string AW: fail-low"
                              << " depth " << depth
                              << " score " << (score >= 0 ? "+" : "") << score << "cp"
                              << " -- window [" << old_alpha << ", " << beta_asp << "]"
                              << " -> [" << alpha_asp << ", " << beta_asp << "]"
                              << " delta " << delta
                              << " [" << t_now << "]\n" << std::flush;
                } else if (score >= beta_asp) {
                    // Fail-high: score is above our window. Widen on the high side only.
                    int old_beta = beta_asp;
                    beta_asp = std::min(score + delta, SCORE_INFINITE);
                    delta   *= 2;
                    aw_fail_highs++;

                    char t_now[32];
                    { int64_t ms=TM.elapsed_ms(); int64_t s=(ms/1000)%60,m=(ms/60000)%60,h=ms/3600000,ms3=ms%1000;
                      std::snprintf(t_now,sizeof(t_now),"%lld:%02lld:%02lld,%03lld",(long long)h,(long long)m,(long long)s,(long long)ms3); }
                    std::cout << "info string AW: fail-high"
                              << " depth " << depth
                              << " score " << (score >= 0 ? "+" : "") << score << "cp"
                              << " -- window [" << alpha_asp << ", " << old_beta << "]"
                              << " -> [" << alpha_asp << ", " << beta_asp << "]"
                              << " delta " << delta
                              << " [" << t_now << "]\n" << std::flush;
                } else {
                    // Score is inside the window — search is complete for this depth.
                    break;
                }
            }
        }

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

                if (seen_count < MAX_GAME_HISTORY + MAX_PLY + 2)
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

#ifdef FACON_DEBUG
        // Diagnostic stats for this iteration. Counters are cumulative across
        // all iterations — shows the running total, not per-iteration delta.
        std::cout << "info string ST: depth " << depth
                  << " lmr " << stats_.lmr_attempted
                  << "/" << stats_.lmr_re_searched
                  << " nmp " << stats_.nmp_attempted
                  << "/" << stats_.nmp_cutoffs
                  << " tt_cut " << stats_.tt_cutoffs
                  << "\n" << std::flush;
#endif

        // -------------------------------------------------------------------------
        // DYNAMIC TIME MANAGEMENT
        // -------------------------------------------------------------------------
        // Quadratic depth scaling: scale extension factors by
        // (depth^2 / EXTENSION_FULL_DEPTH^2) so that PV changes at depth 2-9
        // (normal search behavior) have near-zero effect on the soft limit,
        // while extensions at the engine's operating depth (14+) apply the
        // full factor. At EXTENSION_FULL_DEPTH the scale is exactly 1.0.
        double depth_scale = std::min(1.0,
            double(depth) * double(depth) /
            double(EXTENSION_FULL_DEPTH) / double(EXTENSION_FULL_DEPTH));

        // PV instability: best move changed between iterations.
        // Before applying any extension: if easy-move reductions were applied,
        // cancel them first so the extension acts on the un-discounted soft
        // limit. Otherwise extending on a reduced value is nearly useless.
        if (depth > 1 && last_move != prev_move && last_move != MOVE_NONE) {
            if (easy_cumulative_ < 1.0) {
                TM.cancel_easy_move(easy_cumulative_);
                easy_cumulative_ = 1.0;
                stable_iters_ = 0;
            }
            TM.extend_time(1.0 + 0.5 * depth_scale, "PV change", depth);
        }

        // Score drop: evaluation fell significantly.
        if (depth > 1 && (prev_score - last_score) >= SCORE_DROP_THRESHOLD) {
            if (easy_cumulative_ < 1.0) {
                TM.cancel_easy_move(easy_cumulative_);
                easy_cumulative_ = 1.0;
                stable_iters_ = 0;
            }
            TM.extend_time(1.0 + 0.25 * depth_scale, "score drop", depth);
        }

        // AW fail-high extension: if the aspiration window failed high one or
        // more times this iteration, the position is tactically volatile — the
        // engine found something better than expected and needs time to resolve
        // the window correctly. The extension is proportional to the number of
        // fail-highs: x1.10 per fail-high, capped at x1.50.
        if (aw_fail_highs > 0) {
            double aw_factor = 1.0 + std::min(aw_fail_highs * 0.10, 0.50) * depth_scale;
            if (easy_cumulative_ < 1.0) {
                TM.cancel_easy_move(easy_cumulative_);
                easy_cumulative_ = 1.0;
                stable_iters_ = 0;
            }
            TM.extend_time(aw_factor, "AW fail-high", depth);
        }

        // Easy move reductions and special cases.
        // Depth >= 10: engine is past the opening instability. Stable iters
        // start counting here; the first reduction fires at depth 15 (10 + 5).
        if (depth >= 10) {
            // Mate found by us: reduce time aggressively (one-shot).
            // Only for winning mate (score > 0). If we're being mated
            // (score < 0), we need MORE time to find a defense, not less.
            if (is_mate_score(last_score) && last_score > 0
                && !mate_reduction_applied_)
            {
                TM.reduce_time(0.05, "mate found");
                mate_reduction_applied_ = true;
            } else if (!is_mate_score(last_score)) {
                // Progressive easy-move: track stable iterations where
                // both move and score are unchanged (within 5cp).
                bool move_stable  = (last_move == prev_move && last_move != MOVE_NONE);
                bool score_stable = (std::abs(last_score - prev_score) < 5);
                if (move_stable && score_stable)
                    stable_iters_++;
                else
                    stable_iters_ = 0;

                // After 5 stable iterations, each subsequent stable iteration
                // applies a x0.95 reduction. Progressive: the longer the
                // position stays stable, the more time is saved.
                // 0.95^5 = 0.77, 0.95^10 = 0.60, 0.95^15 = 0.46.
                if (stable_iters_ >= 5) {
                    TM.reduce_time(EASY_REDUCE_FACTOR, "easy move");
                    easy_cumulative_ *= EASY_REDUCE_FACTOR;
                }
            }
        }

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
