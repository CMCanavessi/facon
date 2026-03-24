// =============================================================================
// Last modified: 2026-03-19 00:00
// search.h — Search engine declarations
//
// Implements iterative deepening with negamax alpha-beta search and
// quiescence search. Uses the transposition table and time manager.
//
// Facon 1.1 — Herrumbre
//   - Killer move heuristic: two quiet moves per ply that caused a beta
//     cutoff are stored and tried before other quiet moves in subsequent
//     searches of the same ply. Improves ordering in quiet positions without
//     the overhead of a full history table.
//   - seldepth tracking: maximum depth reached including quiescence search,
//     reported in the UCI info line each iteration.
//   - Illegal PV move fix: is_legal() in board.cpp now checks piece ownership
//     on the from-square before doing the expensive board copy. This catches
//     ghost moves from stale TT entries (wrong piece, wrong color, or hash
//     collision) that previously passed the legality check silently.
//   - Dynamic time management: uses soft_stop() after each iteration and
//     calls TM.extend_time() when PV instability or score drops are detected.
//   - root_best_move_: best move at ply 0 is saved directly in negamax()
//     whenever a new best is found at the root node. We never do TT early
//     returns at ply 0, so this is always set from the actual move loop —
//     never from a potentially stale or collided TT entry.
//
// Facon 1.2 — Rojo Vivo
//   - Null Move Pruning (NMP): if the side to move can pass without moving
//     and the resulting reduced-depth search still exceeds beta, the position
//     is likely so good that a cutoff is safe. Controlled by NMP_MIN_DEPTH
//     and NMP_REDUCTION. Disabled in zugzwang-prone positions (pawnless
//     endings), in check, and at the root.
//   - Triangular PV array: tracks the full principal variation at each ply
//     using a MAX_PLY x MAX_PLY table. Replaces single-move root PV output
//     with the full line. Members: pv_table_ and pv_length_.
//   - Verbosity: currmove output at the root for each move explored;
//     "new best" info string when the best move changes relative to the
//     PREVIOUS iteration (guarded by prev_best_move_ member -- without
//     this, alpha resetting each depth would fire on every iteration);
//     time formatted as h:mm:ss,ms; heartbeat every 5 minutes as two
//     separate lines (standard info + info string) so Arena shows both.
//     Heartbeat uses last_heartbeat_ms_ (NOT last_output_ms_) so that
//     high-frequency currmove lines cannot suppress it during long iterations.
//     new-best lines DO update last_heartbeat_ms_ (they are substantive
//     output, unlike currmove lines which carry no status info).
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

    // Minimum remaining depth to attempt a null move. Below this threshold
    // the cost of the reduced search outweighs the expected pruning benefit.
    static constexpr int NMP_MIN_DEPTH   = 3;

    // Depth reduction applied to the null move search (R in standard NMP).
    // The null move search runs at (depth - 1 - NMP_REDUCTION).
    // A value of 3 is aggressive but standard for engines without LMR.
    static constexpr int NMP_REDUCTION   = 3;

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
    Score negamax(Board& board, Score alpha, Score beta, int depth, int ply,
                  bool do_null = true);

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
    int move_score(const Board& board, Move m, Move tt_move, int ply) const;

    // Sort moves in-place from index 'start' onward using selection sort.
    // Selection sort is simple and fast enough for typical move list sizes (~30-50).
    // Takes ply to pass through to move_score for killer lookup.
    void sort_moves(const Board& board, MoveList& moves,
                    int start, Move tt_move, int ply) const;

    // Store a killer move for the given ply.
    // Does nothing if the move is already in slot 0.
    // Otherwise shifts slot 0 to slot 1 and stores the new move in slot 0.
    void store_killer(Move m, int ply);

};

// Global search instance
extern Search Searcher;
