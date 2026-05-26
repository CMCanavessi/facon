// =============================================================================
// Last modified: 2026-04-25 21:29
// movegen.h -- Move generation
//
// Generates all pseudo-legal moves for a given position.
// Pseudo-legal means the moves are valid piece movements but may leave the
// king in check -- legality is verified separately before making each move.
//
// Usage in the search hot path (avoids the Board copy that is_legal() does):
//   MoveList moves;
//   generate_all_moves(board, moves);
//   for (int i = 0; i < moves.count; i++) {
//       Move m = moves.moves[i];
//       board.make_move(m);
//       if (board.is_attacked(board.king_square(~board.side_to_move),
//                             board.side_to_move)) {
//           board.unmake_move(m);
//           continue;
//       }
//       ...
//       board.unmake_move(m);
//   }
//
// Usage in cold paths (perft, move parsing, SAN disambiguation):
//   board.is_legal(m) is fine -- the Board copy cost is acceptable when
//   not in the hot loop.
//
// Facon 1.0 -- Oxido
//   - Initial implementation: MoveList struct (fixed 256-move array, no heap
//     allocation, with add() and array access), generate_all_moves() for
//     full pseudo-legal move generation, generate_captures() for quiescence
//     search. Per-piece generators: pawn (single push, double push, captures
//     with edge masking, promotions, en passant), knight, bishop, rook,
//     queen (via attack table lookups; sliders use magic bitboards), king
//     (normal moves and castling with empty-squares + not-attacked checks).
//
// Facon 1.4 -- Hoja
//   - generate_captures() now includes quiet queen promotions (pawn push
//     to last rank without capturing). Previously these were only generated
//     by generate_all_moves(), making free queens invisible to qsearch.
// =============================================================================

#pragma once

#include "types.h"
#include "board.h"

// =============================================================================
// MOVE LIST
// =============================================================================
// A fixed-size array of moves. Avoids heap allocation for performance.
// 256 is a safe upper bound for any legal chess position.

struct MoveList {
    Move moves[MAX_MOVES];  // The move array
    int  count;             // How many moves are stored

    MoveList() : count(0) {}

    // Add a move to the list
    inline void add(Move m) {
        moves[count++] = m;
    }

    // Number of moves in the list
    inline int size() const { return count; }

    // Array access
    inline Move operator[](int i) const { return moves[i]; }
};

// =============================================================================
// MOVE GENERATION FUNCTIONS
// =============================================================================

// Generate ALL pseudo-legal moves for the side to move.
// This is the main function called from the search.
void generate_all_moves(const Board& board, MoveList& moves);

// Generate "noisy" moves: captures, promotion captures (all four promotion
// pieces), en passant, and quiet queen promotions (pawn push to the last
// rank without capturing — gaining a queen for free is as significant as
// a capture). Only queen promotion is generated for quiet pushes;
// underpromotions are nearly always inferior and would add noise.
// Used in quiescence search to resolve tactical sequences.
void generate_captures(const Board& board, MoveList& moves);
