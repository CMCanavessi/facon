// =============================================================================
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
    uint64_t nodes    = 0;  // Total nodes visited (negamax + quiescence)
    uint64_t tt_hits  = 0;  // Transposition table hits
    uint64_t qnodes   = 0;  // Quiescence search nodes
    int      seldepth = 0;  // Maximum depth reached including quiescence
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

    // -------------------------------------------------------------------------
    // CORE SEARCH FUNCTIONS
    // -------------------------------------------------------------------------

    // Negamax alpha-beta search.
    // Returns the score from the perspective of the side to move.
    //   ply   = distance from the root (0 at root, increases with each move)
    //   depth = remaining depth to search (decreases toward 0)
    Score negamax(Board& board, Score alpha, Score beta, int depth, int ply);

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
