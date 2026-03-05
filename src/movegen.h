// =============================================================================
// movegen.h — Move generation
//
// Generates all pseudo-legal moves for a given position.
// Pseudo-legal means the moves are valid piece movements but may leave the
// king in check — legality is verified separately before making each move.
//
// Usage:
//   MoveList moves;
//   generate_all_moves(board, moves);
//   for (int i = 0; i < moves.size(); i++) {
//       Move m = moves[i];
//       if (!board.is_legal(m)) continue;
//       board.make_move(m);
//       ...
//       board.unmake_move(m);
//   }
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

// Generate only "noisy" moves: captures and promotion captures (all four
// promotion pieces). Quiet promotions (pawn push to last rank without
// capturing) are intentionally excluded — they are rare and the search
// handles them at full depth in generate_all_moves.
// Used in quiescence search to resolve tactical sequences.
void generate_captures(const Board& board, MoveList& moves);
