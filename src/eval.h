// =============================================================================
// Last modified: 2026-04-18 22:31
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
//
// Facon 1.3 — Yunque
//   - Pawn structure: isolated, doubled, backward, passed, connected pawns.
//     All terms computed via bitboard operations. Constants below.
//   - Insufficient material guard: mopup_eval() now returns 0 for K+B vs K
//     and K+N vs K (theoretical draws). Without this, K+B (330cp) exceeded
//     the MOPUP_THRESHOLD (300cp) and activated corner-chasing in drawn
//     endings.
//
// Facon 1.4 — Hoja
//   - Positional evaluation: mobility, open/semi-open files, rook on 7th,
//     bishop pair, knight outposts. All computed in positional_eval() via
//     bitboard operations, called once per evaluate().
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
// PAWN STRUCTURE CONSTANTS
// =============================================================================
// All values in centipawns. Penalties are negative, bonuses positive.
// These are starting values calibrated by hand — Texel tuning in 1.7 will
// refine them. Signs: negative = bad for the pawn's owner.

// Penalty per isolated pawn (no friendly pawns on adjacent files).
// Isolated pawns cannot be defended by other pawns and require piece support.
constexpr int PAWN_ISOLATED  = -15;

// Penalty per extra pawn on the same file beyond the first.
// Doubled pawns block each other and are easy targets on open files.
constexpr int PAWN_DOUBLED   = -15;

// Penalty per backward pawn (stop square attacked by enemy, no friendly
// pawn can advance to support it). The weakest pawn structure weakness.
constexpr int PAWN_BACKWARD  = -12;

// Bonus per connected pawn (diagonally supported by a friendly pawn).
// Connected pawns are mutually defending and harder to target.
constexpr int PAWN_CONNECTED  =  8;

// Passed pawn bonus by rank (defined in eval.cpp as PASSED_BONUS[8]).
// Declared here for documentation; the array lives in eval.cpp.

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
// POSITIONAL EVALUATION CONSTANTS (Facon 1.4)
// =============================================================================

// Mobility: bonus per pseudo-legal square (excluding own pieces).
// Higher values for pieces that benefit most from mobility.
constexpr int MOBILITY_KNIGHT =  4;  // ~4-8 squares typical
constexpr int MOBILITY_BISHOP =  5;  // ~5-13 squares, benefits most from diagonals
constexpr int MOBILITY_ROOK   =  2;  // ~5-14 squares, high count even when passive
constexpr int MOBILITY_QUEEN  =  1;  // ~9-27 squares, small bonus to avoid overvaluing

// Rook on open file (no pawns of either color) and semi-open file (no friendly pawns).
constexpr int ROOK_OPEN_FILE      = 20;
constexpr int ROOK_SEMI_OPEN_FILE = 10;

// Rook on the 7th rank (opponent's 2nd rank). Strong because it attacks pawns
// on their starting rank and confines the enemy king to the back rank.
constexpr int ROOK_ON_7TH = 20;

// Bishop pair bonus. Two bishops complement each other's color coverage and
// are stronger together than the sum of their parts, especially in open positions.
constexpr int BISHOP_PAIR_BONUS = 30;

// Knight outpost bonus. A knight on a square that cannot be attacked by enemy
// pawns is a powerful anchor — it controls key squares permanently.
constexpr int KNIGHT_OUTPOST = 20;

// =============================================================================
// EVALUATION FUNCTION
// =============================================================================

// Evaluate the position and return a score from the perspective of
// board.side_to_move. Positive = good for the side to move.
Score evaluate(const Board& board);
