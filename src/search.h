// =============================================================================
// Last modified: 2026-05-07 07:30
// search.h -- Search engine declarations
//
// Implements iterative deepening with negamax alpha-beta search and
// quiescence search. Uses the transposition table and time manager.
//
// Facon 1.1 — Herrumbre
//   - Killer move heuristic, seldepth tracking, abort flag, root_best_move_,
//     dynamic time management (soft_stop + extend_time).
//
// Facon 1.2 — Rojo Vivo
//   - Null Move Pruning (NMP): NMP_MIN_DEPTH, NMP_REDUCTION.
//   - Triangular PV array: pv_table_, pv_length_.
//   - Verbosity: currmove, new-best SAN, heartbeat (last_heartbeat_ms_).
//
// Facon 1.3 — Yunque
//   - depth == 0: quiescence entry condition changed from <= to == as LMR
//     prerequisite. All reduction call sites use std::max(0, reduced_depth).
//   - EXTENSION_FULL_DEPTH: depth at which TM extensions apply their full
//     factor. Below this, the quadratic scaling in go() makes them near-zero.
//     Calibrated from 1.2 VVLTC showcase data (avg depth 17.0).
//   - stable_iters_: counts consecutive iterations where PV move and score
//     are both stable. Drives the easy move reduction in go().
//   - LMR (Late Move Reductions): moves searched after the first LMR_MIN_MOVES
//     legal moves at depth >= LMR_MIN_DEPTH are searched at reduced depth if
//     they are quiet and not in check. Re-searched at full depth if they raise
//     alpha. Constants: LMR_MIN_DEPTH=3, LMR_MIN_MOVES=3, LMR_DIVISOR=2.25.
//   - History heuristic: quiet moves that cause a beta cutoff increment
//     history_[color][from][to] by depth^2. Scores replace ORDER_QUIET=0
//     in move ordering, improving LMR accuracy. Reset each search.
//   - Aspiration windows: the ID loop searches with a narrow window around
//     the previous iteration's score instead of (-INFINITE, +INFINITE). On
//     fail-low or fail-high, the window widens on the failing side and the
//     search repeats. Applied from depth >= 4. ASP_WINDOW=50cp initial width.
//
// Facon 1.3 — Yunque (post-gauntlet fixes)
//   - mate_reduction_applied_: one-shot guard so reduce_time("mate found")
//     fires at most once per search. Without it, is_mate_score() is true on
//     every iteration after a mate is found, applying x0.05 repeatedly and
//     collapsing the soft limit exponentially (x0.05^N).
//   - Complex position TM path: extend_time() now accepts a depth parameter.
//     When depth >= EMERGENCY_DEPTH (25, defined in timeman.cpp) and new_soft
//     would exceed the hard limit, hard is raised to match soft instead of
//     capping soft. Both limits rise together on subsequent extensions, capped
//     at 50% of raw remaining clock. Only triggered by real instability (a
//     stable position at depth 25+ would have fired easy-move reduction first).
//     EMERGENCY_DEPTH removed from search.h — it is a TM internal constant.
//   - Aspiration window fail-low fix: beta_asp is no longer modified on a
//     fail-low. The old "beta_asp = (alpha_asp + beta_asp) / 2" squeezed the
//     upper bound and could cause artificial fail-highs on re-search via TT
//     and LMR interactions (yo-yo / see-saw effect). Fix: widen only in the
//     failing direction.
//   - Aspiration window verbosity: fail-low and fail-high events emit
//     "info string AW:" lines in the same style as TM messages.
//
// Facon 1.4 -- Hoja
//   - total_nodes(): public getter returning stats_.nodes + stats_.qnodes
//     after a go() call. Used by the bench command to report per-position
//     node counts without exposing the full SearchStats struct.
//   - LMR reduction table: precomputed int LMR_table[MAX_PLY][MAX_MOVES]
//     replaces the per-move std::log(depth) * std::log(move) / LMR_DIVISOR
//     calculation in the negamax hot path. Initialized once at startup via
//     init_lmr_table(). Same formula, same results — pure speedup.
//   - FACON_DEBUG diagnostic counters: LMR attempted/re-searched, NMP
//     attempted/cutoffs, TT cutoffs. Compiled only with -DFACON_DEBUG.
//     Reported via "info string ST:" after each completed iteration.
//     Zero cost in release builds.
//   - Futility pruning constants: RFP_MAX_DEPTH, RFP_MARGIN, FUTILITY_MAX_DEPTH,
//     FUTILITY_MARGIN. Used by reverse futility pruning (node-level) and
//     move-level futility pruning in negamax.
//
// Facon 1.5 -- Espiga
//   - negamax() takes a new Move prev_move parameter (default MOVE_NONE),
//     used by the countermove heuristic to look up the move that most
//     recently refuted the opponent's last move at this node. The default
//     argument keeps existing call sites (root entry from go(), NMP
//     recursion) compatible without code changes.
//   - move_score() and sort_moves() take a new Move countermove parameter,
//     pre-computed once per node by the caller. Inserted as a new ordering
//     tier (ORDER_COUNTERMOVE = 65000) between killer2 (80000) and history
//     (max 50000).
//   - countermoves_[15][64]: new table indexed by [piece][to_square]. The
//     first dimension is sized 15 because the Piece enum uses non-contiguous
//     values (0=NO_PIECE, 1..6=W_PAWN..W_KING, 9..14=B_PAWN..B_KING).
//     Reset along with killers_ and history_ at the start of each search.
//   - Internal Iterative Reductions (IIR_MIN_DEPTH = 4): at PV nodes
//     without a TT hit, depth is reduced by 1 to avoid wasting nodes on
//     poorly-ordered first moves.
//   - Razoring (RAZOR_MAX_DEPTH = 2, RAZOR_MARGIN = 250): non-PV nodes at
//     shallow depth where static_eval falls far below alpha drop into a
//     quiescence verification that may return early.
//   - Late Move Pruning (LMP_MAX_DEPTH = 6): in non-PV nodes at shallow
//     depth, after enough legal quiet moves have been searched, additional
//     quiets are pruned entirely. Per-depth thresholds in LMP_TABLE
//     (search.cpp). Captures, promotions, and killers are exempt.
//   - SEE-aware capture ordering: captures are split into GOOD (SEE >= 0,
//     scored above killers) and BAD (SEE < 0, scored below history).
//     Replaces the single ORDER_CAPTURE constant. SEE is computed only
//     when needed; captures with attacker no more valuable than victim
//     skip the SEE call as they are always favorable.
// =============================================================================

#pragma once

#include "types.h"
#include "board.h"
#include "tt.h"
#include "timeman.h"
#include "movegen.h"
#include "eval.h"

// =============================================================================
// SEARCH RESULT
// =============================================================================
// Returned by the top-level search function with the best move and score.

struct SearchResult {
    Move  best_move = MOVE_NONE;
    Score score     = 0;
    int   depth     = 0;  // Depth of the last completed iteration
};

// =============================================================================
// SEARCH STATS
// =============================================================================
// Counters updated during search, used for UCI info output.

struct SearchStats {
    uint64_t nodes    = 0;  // Nodes visited in negamax() only (not quiescence)
    uint64_t tt_hits  = 0;  // Transposition table hits
    uint64_t qnodes   = 0;  // Nodes visited in quiescence()
    int      seldepth = 0;  // Maximum depth reached including quiescence
    // Note: use (nodes + qnodes) for total node count and NPS reporting.

#ifdef FACON_DEBUG
    // Diagnostic counters — compiled only with -DFACON_DEBUG (cmake -DFACON_DEBUG=ON).
    // Zero cost in release builds. Reported via "info string ST:" after each iteration.
    uint64_t lmr_attempted  = 0;  // Moves searched at reduced depth
    uint64_t lmr_re_searched = 0; // LMR moves that raised alpha and were re-searched
    uint64_t nmp_attempted  = 0;  // Null move pruning attempts
    uint64_t nmp_cutoffs    = 0;  // NMP attempts that produced a beta cutoff
    uint64_t tt_cutoffs     = 0;  // TT probe hits that produced a cutoff (early return)
#endif
};

// =============================================================================
// SEARCH CLASS
// =============================================================================

class Search {
public:
    // Run a full iterative deepening search on the given board.
    // Uses TM (TimeManager) for time control.
    // Prints UCI info lines to stdout and returns the best move found.
    SearchResult go(Board& board);

    // Returns total nodes searched in the last go() call (negamax + quiescence).
    // Used by the bench command to report per-position and total node counts.
    uint64_t total_nodes() const { return stats_.nodes + stats_.qnodes; }

    // -------------------------------------------------------------------------
    // PUBLIC SEARCH CONFIGURATION CONSTANTS
    // -------------------------------------------------------------------------
    // Constants used by structures defined outside the class (e.g. LMP_TABLE
    // in search.cpp) must be public to be accessible from the global scope.

    // Maximum depth where LMP applies. Beyond this, threshold is too large
    // to provide meaningful savings vs the risk of pruning a critical move.
    // Used by LMP_TABLE (defined in search.cpp) to size and index the
    // per-depth move-count threshold array.
    static constexpr int LMP_MAX_DEPTH = 6;

private:
    SearchStats stats_;

    // Abort flag: set to true when the hard time limit is hit inside the search.
    // Checked at the top of every negamax() and quiescence() call so the
    // recursion unwinds naturally — all unmake_move() calls execute, the board
    // stays consistent, and no TT pollution occurs from partial results.
    // Reset to false at the start of each go() call.
    bool abort_search_ = false;

    // Best move at the root (ply 0), updated directly inside negamax() whenever
    // a new best move is found at the root node. We never do TT early returns
    // at ply 0, so this is always set from the actual move loop. This is the
    // authoritative source for bestmove — always legal in the current position.
    // Reset to MOVE_NONE at the start of each iteration.
    Move root_best_move_ = MOVE_NONE;

    // Killer moves: up to two quiet moves per ply that caused a beta cutoff.
    // They are likely to be good in sibling nodes at the same ply, so we
    // search them before other quiet moves.
    // Indexed by [ply][slot] where slot is 0 (most recent) or 1 (older).
    // Reset at the start of each search — killers from previous positions
    // are irrelevant and can mislead move ordering.
    Move killers_[MAX_PLY][2];

    // History heuristic table: records how often a quiet move from [from] to
    // [to] by [color] has caused a beta cutoff. Incremented by depth^2 on each
    // cutoff so deeper searches contribute more. Used in move_score() to order
    // quiet moves — replaces the flat ORDER_QUIET=0 score.
    // Capped at HISTORY_MAX to prevent overflow and keep scores in a stable
    // range relative to ORDER_KILLER2. Reset at the start of each search.
    static constexpr int HISTORY_MAX = 50000;  // Below ORDER_KILLER2 (80000)
    int history_[2][64][64];                   // [color][from][to]

    // Countermove table: for each (piece, to_square) of the previous move
    // played, remembers the quiet move that refuted it (caused a beta cutoff)
    // most recently. Used as an additional move ordering tier between killers
    // and history.
    //
    // Indexed by [piece][to_square]. The Piece enum uses values 0..14 with
    // gaps (1..6 for WPAWN..WKING, 9..14 for BPAWN..BKING, 0 for NO_PIECE),
    // so we size the first dimension to 15 to accommodate the maximum value.
    // Color is part of the index so refutations to white's pawn-to-e4 are
    // kept separate from refutations to black's pawn-to-e4 -- they are
    // different positions with different responses.
    //
    // Total size: 15 * 64 * 4 bytes = 3840 bytes. Negligible.
    // Reset at the start of each search alongside killers_ and history_.
    Move countermoves_[15][64];

    // Triangular PV array: stores the principal variation at each ply.
    // pv_table_[ply] holds the PV line from that ply to the end of the search.
    // pv_length_[ply] is the number of valid moves in that line.
    // The table is triangular because the line at ply N can be at most
    // (MAX_PLY - N) moves long — deeper plies have shorter remaining lines.
    Move pv_table_[MAX_PLY][MAX_PLY];
    int  pv_length_[MAX_PLY];

    // -------------------------------------------------------------------------
    // NULL MOVE PRUNING PARAMETERS
    // -------------------------------------------------------------------------

    // Minimum remaining depth to attempt a null move.
    static constexpr int NMP_MIN_DEPTH = 3;

    // Depth reduction for the null move search.
    // Null move runs at std::max(0, depth - 1 - NMP_REDUCTION).
    // The std::max(0, ...) floor is required with depth == 0: without it,
    // NMP at depth==NMP_MIN_DEPTH would pass depth -1, causing infinite
    // recursion since depth -1 no longer silently enters quiescence.
    static constexpr int NMP_REDUCTION = 3;

    // Depth at which TM extensions apply their full factor.
    // The quadratic scaling in go() gives near-zero effect at low depths
    // and the full factor at EXTENSION_FULL_DEPTH. Set to 15 so that
    // PV changes at depth 10+ have meaningful impact. The progressive
    // easy-move reduction counterbalances by returning time when the
    // position is stable.
    static constexpr int EXTENSION_FULL_DEPTH = 15;

    // -------------------------------------------------------------------------
    // LATE MOVE REDUCTIONS (LMR) PARAMETERS
    // -------------------------------------------------------------------------

    // Minimum remaining depth to attempt LMR. At shallow depths the overhead
    // of a re-search on fail-high outweighs the pruning benefit.
    static constexpr int LMR_MIN_DEPTH = 3;

    // Number of legal moves to search at full depth before applying LMR.
    // The first LMR_MIN_MOVES moves are the highest-scored (TT move, captures,
    // killers) and most likely to be best — always searched fully.
    static constexpr int LMR_MIN_MOVES = 3;

    // Divisor in the reduction formula: reduction = log(depth) * log(move) / LMR_DIVISOR.
    // Larger value = less reduction per move = safer but less pruning.
    static constexpr double LMR_DIVISOR = 2.25;

    // Easy move reduction factor per iteration. When a position qualifies as
    // easy (PV and score stable for >= 5 iterations at depth >= 10), each
    // subsequent stable iteration multiplies the soft limit by this factor.
    // The effect is progressive: 0.95^5 = 0.77, 0.95^10 = 0.60. If the PV
    // changes or score drops significantly, the cumulative reduction is
    // reversed via cancel_easy_move(easy_cumulative_).
    static constexpr double EASY_REDUCE_FACTOR = 0.95;

    // -------------------------------------------------------------------------
    // FUTILITY PRUNING PARAMETERS
    // -------------------------------------------------------------------------

    // Reverse futility pruning (RFP): at the node level, if static_eval minus
    // a margin already exceeds beta, skip the entire node. Maximum depth for RFP.
    static constexpr int RFP_MAX_DEPTH = 3;

    // Margin per depth level for reverse futility pruning (centipawns).
    // At depth d, prune if eval - RFP_MARGIN * d >= beta.
    static constexpr int RFP_MARGIN = 100;

    // Move-level futility pruning: skip quiet moves at shallow depths when
    // static_eval + margin is below alpha. Maximum depth for move-level futility.
    static constexpr int FUTILITY_MAX_DEPTH = 2;

    // Margin per depth level for move-level futility pruning (centipawns).
    // At depth d, skip quiet moves if eval + FUTILITY_MARGIN * d <= alpha.
    static constexpr int FUTILITY_MARGIN = 150;

    // -------------------------------------------------------------------------
    // INTERNAL ITERATIVE REDUCTIONS (IIR)
    // -------------------------------------------------------------------------
    // When a node has no TT move available, move ordering at this node is
    // less reliable. Rather than searching at full depth with a poor first
    // move, reduce the depth by 1 and let a shallower search produce a
    // better ordering signal that propagates back via the TT.
    // Applied only at PV nodes (where window > 1) -- in non-PV nodes the
    // ordering quality matters less because we expect cutoffs anyway.

    // Minimum depth for IIR. Below this, the savings from reducing are
    // negligible and the loss of accuracy is not worth it.
    static constexpr int IIR_MIN_DEPTH = 4;

    // -------------------------------------------------------------------------
    // RAZORING
    // -------------------------------------------------------------------------
    // At very shallow depths, if static_eval is so far below alpha that even
    // a generous margin does not bring it close, drop directly into quiescence
    // instead of doing a full search. This is the dual of RFP: RFP prunes
    // when we are clearly above beta, razoring prunes when we are clearly
    // below alpha. Applied only at non-PV nodes.

    // Maximum depth for razoring. Standard value used by HCE engines.
    static constexpr int RAZOR_MAX_DEPTH = 2;

    // Margin per depth level for razoring (centipawns). At depth d, razor
    // is considered if static_eval + RAZOR_MARGIN * d < alpha. Then a
    // quiescence verification is run; if it confirms eval is well below
    // alpha, we return the qsearch score and skip the full search.
    static constexpr int RAZOR_MARGIN = 250;

    // -------------------------------------------------------------------------
    // COUNTERMOVE HEURISTIC
    // -------------------------------------------------------------------------
    // For each (piece, to_square) of the opponent's previous move, remember
    // which quiet move of ours refuted it. Used as an additional ordering
    // tier between killers and history.

    // Move ordering bonus for the countermove. Sits below ORDER_KILLER2
    // (80000) and above HISTORY_MAX (50000), so ordering tiers are:
    //   TT_MOVE (1,000,000) > captures (100,000+) > killer1 (90,000)
    //   > killer2 (80,000) > countermove (65,000) > history (0..50,000)
    static constexpr int ORDER_COUNTERMOVE = 65000;

    // -------------------------------------------------------------------------
    // LATE MOVE PRUNING (LMP)
    // -------------------------------------------------------------------------
    // After searching enough quiet moves at shallow depth in non-PV nodes,
    // the remaining quiets are unlikely to produce a beta cutoff that the
    // earlier ones missed. Skip them entirely. This is the dual of LMR --
    // LMR reduces depth, LMP cuts the move out of the search.
    //
    // The threshold per depth is taken from a precomputed table indexed by
    // depth (0..LMP_MAX_DEPTH). At depth d, only the first LMP_TABLE[d]
    // legal quiet moves are searched; any later quiet move is pruned.
    // Captures, promotions, killers, and check evasions are exempt.
    //
    // LMP_MAX_DEPTH is declared in the public section (above) so LMP_TABLE
    // in search.cpp can size and index by it from the global scope.

    // -------------------------------------------------------------------------
    // SEE-AWARE CAPTURE ORDERING
    // -------------------------------------------------------------------------
    // Captures are split into two ordering tiers based on Static Exchange
    // Evaluation (SEE):
    //   GOOD captures (SEE >= 0): material is recovered or gained. Ordered
    //       above killers so winning captures are tried first.
    //   BAD captures (SEE < 0): the capturing side loses material in the
    //       eventual exchange. Ordered below history (i.e. below all
    //       quiet moves) so they are tried last, after the search has
    //       considered safer alternatives.
    //
    // To avoid the cost of computing SEE for every capture, we skip SEE
    // for "obviously good" captures (attacker value <= victim value).
    // PxQ, NxQ, BxR, etc. are always GOOD captures because even if the
    // capturing piece is lost, the trade is favorable.

    // Capture ordering bonuses. GOOD sits above ORDER_KILLER1 (90000),
    // BAD sits below all history values (0..50000) and below 0.
    // MVV-LVA (max ~605 for PxQ) is added to break ties within each tier.
    static constexpr int ORDER_CAPTURE_GOOD = 110000;
    static constexpr int ORDER_CAPTURE_BAD  = -100000;

    // -------------------------------------------------------------------------
    // ASPIRATION WINDOW PARAMETERS
    // -------------------------------------------------------------------------

    // Initial half-width of the aspiration window around the previous score.
    // 50cp is a standard starting value: wide enough to rarely fail spuriously
    // in stable positions, narrow enough to save meaningful nodes when it holds.
    static constexpr int ASP_WINDOW = 50;  // centipawns

    // -------------------------------------------------------------------------
    // VERBOSITY / OBSERVABILITY
    // -------------------------------------------------------------------------

    // Heartbeat interval in milliseconds. If no output has been emitted for
    // this long, a status line is printed to stdout. Only relevant at very
    // long time controls (VVLTC) where a single iteration can run for many
    // minutes — allows distinguishing a live deep search from a crash.
    static constexpr int64_t HEARTBEAT_INTERVAL_MS = 300000;  // 5 minutes

    // Timestamp of the last output line of ANY kind. Updated after every
    // stdout write including currmove and new-best. Not used by the heartbeat
    // check (see last_heartbeat_ms_ below).
    int64_t last_output_ms_ = 0;

    // Timestamp of the last VISIBLE output. The heartbeat fires when
    // (now - last_heartbeat_ms_) >= HEARTBEAT_INTERVAL_MS, and exists to
    // prevent long silent stretches in the operator's log. Updated by:
    //   - end-of-iteration UCI info lines (go())
    //   - the heartbeat itself (negamax())
    //   - new-best info strings (negamax()) — these appear as text lines
    //     in the GUI log, so they constitute visible activity that resets
    //     the silence counter.
    // NOT updated by currmove lines — GUIs consume currmove internally
    // to update a dedicated panel, but it never appears in the text log.
    // The operator sees nothing from currmove, so it cannot be treated as
    // a sign of activity from their perspective.
    int64_t last_heartbeat_ms_ = 0;

    // Depth of the iteration currently in progress. Set at the top of each
    // iteration in go(). Used in heartbeat messages and currmove output so
    // the operator knows which depth is being searched.
    int current_depth_ = 0;

    // Total number of legal moves at the root for the current iteration.
    // Set once per iteration before the move loop. Used to format currmove
    // output as "move N / total" for context.
    int root_move_count_ = 0;

    // Best move from the previous completed iteration of iterative deepening.
    // Used by negamax() at ply 0 to suppress the "new best" info string when
    // the same move is re-confirmed at a new depth (alpha resets to -INFINITY
    // each iteration, so the first alpha-raise would fire every depth without
    // this guard). Set at the end of each completed iteration in go().
    Move prev_best_move_ = MOVE_NONE;

    // Counter for consecutive completed iterations where both the PV move
    // and score have been stable (same move, score change < 5cp).
    // When this reaches >= 5 at depth >= 10, go() applies progressive
    // easy-move reductions (x0.95 per stable iteration). Resets to 0
    // whenever the PV or score changes meaningfully.
    int stable_iters_ = 0;

    // Cumulative easy-move reduction factor. Starts at 1.0 (no reduction).
    // Each stable iteration multiplies by EASY_REDUCE_FACTOR (0.95).
    // When a PV change or score drop triggers cancel_easy_move(), the
    // soft limit is divided by this factor to restore the original value,
    // then easy_cumulative_ resets to 1.0 and stable_iters_ to 0.
    double easy_cumulative_ = 1.0;

    // Set to true when the "mate found" TM reduction has been applied this move.
    // Without this guard, is_mate_score(last_score) is true on every subsequent
    // iteration after a mate is first found, applying x0.05 repeatedly and
    // collapsing the soft limit exponentially (x0.05^N).
    bool mate_reduction_applied_ = false;

    // -------------------------------------------------------------------------
    // CORE SEARCH FUNCTIONS
    // -------------------------------------------------------------------------

    // Negamax alpha-beta search.
    // Returns the score from the perspective of the side to move.
    //   ply     = distance from the root (0 at root, increases with each move)
    //   depth   = remaining depth to search (decreases toward 0)
    //   do_null = whether null move pruning is allowed at this node.
    //             Set to false for the node immediately after a null move to
    //             prevent two consecutive null moves (infinite loop / unsound pruning).
    //   prev_move: the move played by the opponent to reach this node (i.e.
    //             the most recent move on the board). Used by the countermove
    //             heuristic to find the move that refuted prev_move when this
    //             node was reached previously. MOVE_NONE at the root and
    //             after a null move (no real previous move).
    Score negamax(Board& board, Score alpha, Score beta, int depth, int ply,
                  bool do_null = true, Move prev_move = MOVE_NONE);

    // Quiescence search: called at depth=0 to resolve tactical sequences.
    // Searches captures only until the position is "quiet".
    // Prevents the horizon effect (missing that a piece can be immediately recaptured).
    // Updates stats_.seldepth when it reaches a new maximum depth.
    Score quiescence(Board& board, Score alpha, Score beta, int ply);

    // -------------------------------------------------------------------------
    // MOVE ORDERING
    // -------------------------------------------------------------------------

    // Returns a score for a move used to determine search order.
    // Higher score = search this move earlier.
    // Takes ply to look up killer moves for this node.
    // countermove is the precomputed countermove for this node (looked up
    // once per node from countermoves_ using the parent's prev_move). If
    // MOVE_NONE, no countermove ordering bonus applies.
    int move_score(const Board& board, Move m, Move tt_move,
                   Move countermove, int ply) const;

    // Sort moves in-place from index 'start' onward using selection sort.
    // Selection sort is simple and fast enough for typical move list sizes (~30-50).
    // Takes ply to pass through to move_score for killer lookup.
    // countermove is passed through to move_score for ordering.
    void sort_moves(const Board& board, MoveList& moves,
                    int start, Move tt_move, Move countermove, int ply) const;

    // Store a killer move for the given ply.
    // Does nothing if the move is already in slot 0.
    // Otherwise shifts slot 0 to slot 1 and stores the new move in slot 0.
    void store_killer(Move m, int ply);

};

// Global search instance
extern Search Searcher;

// =============================================================================
// LMR REDUCTION TABLE
// =============================================================================
// Precomputed reduction values for Late Move Reductions. Indexed by
// [depth][move_number]. Initialized once at startup by init_lmr_table().
// Formula: max(1, int(log(depth) * log(move) / LMR_DIVISOR)).
// Entries for depth < LMR_MIN_DEPTH or move <= LMR_MIN_MOVES are 0 (unused).

extern int LMR_table[MAX_PLY][MAX_MOVES];

// Must be called once at startup (from main.cpp) to fill LMR_table.
void init_lmr_table();
