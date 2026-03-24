// =============================================================================
// Last modified: 2026-03-11 23:49
// eval.h — Position evaluation
//
// Returns a score in centipawns (100 = one pawn advantage) from the
// perspective of the side to move. Positive = good for the mover.
//
// Facon 1.0 — Oxido
//   - Material count
//   - Piece-square tables (PST): bonus/penalty per piece per square
//
// Facon 1.1 — Herrumbre
//   - King safety: penalty for enemy pieces attacking the king zone
//
// Facon 1.2 — Rojo Vivo
//   - Mopup evaluation: in pawnless endings with a decisive material
//     advantage, reward pushing the losing king toward a corner and
//     closing the distance between kings. Without this the engine can
//     hold a winning advantage but fail to convert — wandering without
//     a plan. Applied only when no pawns remain and the material
//     advantage exceeds MOPUP_THRESHOLD.
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
// MOPUP EVALUATION CONSTANTS
// =============================================================================
// Used in pawnless endings to guide the winning king toward the losing king
// and push the losing king toward a corner.

// Minimum material advantage (in centipawns) required to activate mopup.
// Below this threshold the position may be drawn — mopup would be misleading.
// 300cp = one minor piece, which is the minimum realistic mating material.
constexpr Score MOPUP_THRESHOLD = 300;

// Reward per unit of center distance of the losing king (0=center, 6=corner).
// Encourages driving the losing king toward the edge and corners.
constexpr int MOPUP_CORNER_WEIGHT = 10;

// Reward per unit of king proximity (applied as 14 - manhattan_distance).
// Encourages the winning king to close in on the losing king.
constexpr int MOPUP_PROXIMITY_WEIGHT = 5;

// =============================================================================
// EVALUATION FUNCTION
// =============================================================================

// Evaluate the position and return a score from the perspective of
// board.side_to_move. Positive = good for the side to move.
Score evaluate(const Board& board);
