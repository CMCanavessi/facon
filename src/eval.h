// =============================================================================
// eval.h — Position evaluation
//
// Returns a score in centipawns (100 = one pawn advantage) from the
// perspective of the side to move. Positive = good for the mover.
//
// Facón 1.0 evaluation:
//   - Material count
//   - Piece-square tables (PST): bonus/penalty per piece per square
// =============================================================================

#pragma once

#include "types.h"
#include "board.h"

// =============================================================================
// MATERIAL VALUES (in centipawns)
// =============================================================================

constexpr Score PAWN_VALUE   = 100;
constexpr Score KNIGHT_VALUE = 320;
constexpr Score BISHOP_VALUE = 330;
constexpr Score ROOK_VALUE   = 500;
constexpr Score QUEEN_VALUE  = 900;
constexpr Score KING_VALUE   = 0;    // King has no material value

// Array indexed by PieceType for convenient lookup
constexpr Score PIECE_VALUE[7] = {
    0,             // NO_PIECE_TYPE
    PAWN_VALUE,
    KNIGHT_VALUE,
    BISHOP_VALUE,
    ROOK_VALUE,
    QUEEN_VALUE,
    KING_VALUE
};

// =============================================================================
// EVALUATION FUNCTION
// =============================================================================

// Evaluate the position and return a score from the perspective of
// board.side_to_move. Positive = good for the side to move.
Score evaluate(const Board& board);
