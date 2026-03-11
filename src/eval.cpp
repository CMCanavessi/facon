// =============================================================================
// eval.cpp — Material + Piece-Square Table evaluation
//
// PST values are from the perspective of WHITE, stored from a1..h8
// (index 0=a1, index 63=h8). For BLACK we mirror the table vertically
// by flipping the rank: black_sq = sq ^ 56 (XOR 56 flips the rank index).
//
// All values are in centipawns (100 = one pawn).
//
// Facon 1.1 — Herrumbre
//   - King safety: penalty for enemy pieces attacking the king zone.
//     The king zone is the king's square plus all adjacent squares (up to 9
//     squares total). Each enemy piece type has an attack weight, and the
//     penalty grows quadratically with the total weight. Scaled by game phase
//     so it only applies in the middlegame.
// =============================================================================

#include "eval.h"
#include "bitboard.h"
#include <algorithm>  // std::max

// =============================================================================
// PIECE-SQUARE TABLES
// =============================================================================
// Positional bonuses/penalties for each piece on each square.
// Values are relative to the piece's base material value (can be negative).
// All tables are from White's perspective: index 0=a1, index 63=h8.
// For Black, the square index is mirrored vertically: idx = sq ^ 56.

// Pawns: reward center control and advancement
static const int PST_PAWN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,  // rank 1 (unreachable for pawns)
     5, 10, 10,-20,-20, 10, 10,  5,  // rank 2
     5, -5,-10,  0,  0,-10, -5,  5,  // rank 3
     0,  0,  0, 20, 20,  0,  0,  0,  // rank 4
     5,  5, 10, 25, 25, 10,  5,  5,  // rank 5
    10, 10, 20, 30, 30, 20, 10, 10,  // rank 6
    50, 50, 50, 50, 50, 50, 50, 50,  // rank 7
     0,  0,  0,  0,  0,  0,  0,  0   // rank 8 (promotion rank)
};

// Knights: strongly reward center squares, penalize edges and corners
static const int PST_KNIGHT[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,  // rank 1
    -40,-20,  0,  5,  5,  0,-20,-40,  // rank 2
    -30,  5, 10, 15, 15, 10,  5,-30,  // rank 3
    -30,  0, 15, 20, 20, 15,  0,-30,  // rank 4
    -30,  5, 15, 20, 20, 15,  5,-30,  // rank 5
    -30,  0, 10, 15, 15, 10,  0,-30,  // rank 6
    -40,-20,  0,  0,  0,  0,-20,-40,  // rank 7
    -50,-40,-30,-30,-30,-30,-40,-50   // rank 8
};

// Bishops: reward long diagonals and center activity, penalize corners
static const int PST_BISHOP[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,  // rank 1
    -10,  5,  0,  0,  0,  0,  5,-10,  // rank 2
    -10, 10, 10, 10, 10, 10, 10,-10,  // rank 3
    -10,  0, 10, 10, 10, 10,  0,-10,  // rank 4
    -10,  5,  5, 10, 10,  5,  5,-10,  // rank 5
    -10,  0,  5, 10, 10,  5,  0,-10,  // rank 6
    -10,  0,  0,  0,  0,  0,  0,-10,  // rank 7
    -20,-10,-10,-10,-10,-10,-10,-20   // rank 8
};

// Rooks: reward the 7th rank and open files, slight penalty for rim files
static const int PST_ROOK[64] = {
     0,  0,  0,  5,  5,  0,  0,  0,  // rank 1
    -5,  0,  0,  0,  0,  0,  0, -5,  // rank 2
    -5,  0,  0,  0,  0,  0,  0, -5,  // rank 3
    -5,  0,  0,  0,  0,  0,  0, -5,  // rank 4
    -5,  0,  0,  0,  0,  0,  0, -5,  // rank 5
    -5,  0,  0,  0,  0,  0,  0, -5,  // rank 6
     5, 10, 10, 10, 10, 10, 10,  5,  // rank 7 (strong rank bonus)
     0,  0,  0,  0,  0,  0,  0,  0   // rank 8
};

// Queens: discourage early development, reward center in middlegame
static const int PST_QUEEN[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,  // rank 1
    -10,  0,  5,  0,  0,  0,  0,-10,  // rank 2
    -10,  5,  5,  5,  5,  5,  0,-10,  // rank 3
      0,  0,  5,  5,  5,  5,  0, -5,  // rank 4
     -5,  0,  5,  5,  5,  5,  0, -5,  // rank 5
    -10,  0,  5,  5,  5,  5,  0,-10,  // rank 6
    -10,  0,  0,  0,  0,  0,  0,-10,  // rank 7
    -20,-10,-10, -5, -5,-10,-10,-20   // rank 8
};

// King middlegame: reward castled positions, penalize exposed center squares
static const int PST_KING_MG[64] = {
     20, 30, 10,  0,  0, 10, 30, 20,  // rank 1 — castled king is safe
     20, 20,  0,  0,  0,  0, 20, 20,  // rank 2
    -10,-20,-20,-20,-20,-20,-20,-10,  // rank 3
    -20,-30,-30,-40,-40,-30,-30,-20,  // rank 4
    -30,-40,-40,-50,-50,-40,-40,-30,  // rank 5
    -30,-40,-40,-50,-50,-40,-40,-30,  // rank 6
    -30,-40,-40,-50,-50,-40,-40,-30,  // rank 7
    -30,-40,-40,-50,-50,-40,-40,-30   // rank 8
};

// King endgame: reward centralization (opposite of middlegame priorities)
static const int PST_KING_EG[64] = {
    -50,-30,-30,-30,-30,-30,-30,-50,  // rank 1
    -30,-30,  0,  0,  0,  0,-30,-30,  // rank 2
    -30,-10, 20, 30, 30, 20,-10,-30,  // rank 3
    -30,-10, 30, 40, 40, 30,-10,-30,  // rank 4
    -30,-10, 30, 40, 40, 30,-10,-30,  // rank 5
    -30,-10, 20, 30, 30, 20,-10,-30,  // rank 6
    -30,-20,-10,  0,  0,-10,-20,-30,  // rank 7
    -50,-40,-30,-20,-20,-30,-40,-50   // rank 8
};

// =============================================================================
// GAME PHASE DETECTION
// =============================================================================
// We interpolate between middlegame and endgame PST values based on remaining
// material. This prevents the king from abruptly changing behavior when the
// last piece is traded.
//
// Each piece type contributes to the phase score:
//   Knight=1, Bishop=1, Rook=2, Queen=4 (pawns and kings don't count)
// Maximum phase = 4*1 + 4*1 + 4*2 + 2*4 = 24

static const int PHASE_VALUE[7] = { 0, 0, 1, 1, 2, 4, 0 };
constexpr int TOTAL_PHASE = 24;  // 4 knights + 4 bishops + 4 rooks + 2 queens

// Returns a value 0..256 where 256 = full middlegame, 0 = full endgame
static int game_phase(const Board& board) {
    int phase = TOTAL_PHASE;
    for (int pt = KNIGHT; pt <= QUEEN; pt++)
        phase -= popcount(board.piece_bb(PieceType(pt))) * PHASE_VALUE[pt];
    phase = std::max(phase, 0);  // Clamp in case of unusual positions
    return (phase * 256 + TOTAL_PHASE / 2) / TOTAL_PHASE;
}

// =============================================================================
// PST LOOKUP
// =============================================================================
// Returns the positional bonus for a piece of the given type and color on sq.
// For Black, the square is mirrored vertically so both sides use the same table.

static int pst_value(PieceType pt, Color c, Square sq, int phase_mg) {
    int idx = (c == WHITE) ? sq : (sq ^ 56);  // Mirror for Black

    switch (pt) {
        case PAWN:   return PST_PAWN[idx];
        case KNIGHT: return PST_KNIGHT[idx];
        case BISHOP: return PST_BISHOP[idx];
        case ROOK:   return PST_ROOK[idx];
        case QUEEN:  return PST_QUEEN[idx];
        case KING: {
            // Interpolate between MG and EG tables based on current phase.
            // phase_mg=256 → pure middlegame, phase_mg=0 → pure endgame.
            int mg = PST_KING_MG[idx];
            int eg = PST_KING_EG[idx];
            return (mg * phase_mg + eg * (256 - phase_mg)) / 256;
        }
        default: return 0;
    }
}

// =============================================================================
// KING SAFETY
// =============================================================================
// Evaluates the safety of 'us' king by counting enemy pieces that attack
// the king zone (the king's square plus all adjacent squares — up to 9
// squares total).
//
// Attack weights by piece type:
//   Knight=2, Bishop=2, Rook=3, Queen=5
//
// Note: queens are counted twice — once in the diagonal loop (as bishops)
// and once in the straight loop (as rooks). This is intentional: a queen
// attacking the king zone is more dangerous than a single rook or bishop,
// and the double count reflects that naturally.
//
// The penalty grows quadratically with the total attack weight:
//   penalty = weight^2 * SAFETY_SCALE
//
// Quadratic growth means a single attacker barely matters, but two or more
// attackers coordinating cause a significant penalty. This matches practical
// chess experience: a single piece near the king is rarely decisive, but a
// coordinated attack with multiple pieces is very dangerous.
//
// The result is scaled by game phase so king safety only penalizes in the
// middlegame. In the endgame both kings should centralize.

static const int KING_ATTACK_WEIGHT[7] = {
    0,  // NO_PIECE_TYPE
    0,  // PAWN   — not included (handled by PST pawn shield implicitly)
    2,  // KNIGHT
    2,  // BISHOP
    3,  // ROOK
    5,  // QUEEN
    0   // KING   — king attacks not counted
};
constexpr int SAFETY_SCALE = 3;  // Tune this to adjust aggressiveness of penalty

static Score king_safety(const Board& board, Color us, int phase_mg) {
    // King safety is irrelevant in the endgame
    if (phase_mg == 0) return 0;

    Color    them      = ~us;
    Square   king_sq   = board.king_square(us);
    Bitboard king_zone = king_attack(king_sq) | square_bb(king_sq);
    Bitboard occ       = board.occupancy();

    int total_weight = 0;

    // Knights: check if any knight attack pattern reaches the king zone
    Bitboard knights = board.piece_bb(them, KNIGHT);
    while (knights) {
        if (knight_attack(pop_lsb(knights)) & king_zone)
            total_weight += KING_ATTACK_WEIGHT[KNIGHT];
    }

    // Bishops and queens (diagonal rays): counted with bishop weight.
    // Queens are included here AND in the rook loop below (intentional).
    Bitboard diag_pieces = board.piece_bb(them, BISHOP)
                         | board.piece_bb(them, QUEEN);
    while (diag_pieces) {
        if (bishop_attack(pop_lsb(diag_pieces), occ) & king_zone)
            total_weight += KING_ATTACK_WEIGHT[BISHOP];
    }

    // Rooks and queens (straight rays): counted with rook weight.
    Bitboard straight_pieces = board.piece_bb(them, ROOK)
                             | board.piece_bb(them, QUEEN);
    while (straight_pieces) {
        if (rook_attack(pop_lsb(straight_pieces), occ) & king_zone)
            total_weight += KING_ATTACK_WEIGHT[ROOK];
    }

    // Quadratic penalty, scaled down to middlegame proportion.
    // phase_mg=256 → full penalty applied, phase_mg=0 → no penalty.
    Score penalty = total_weight * total_weight * SAFETY_SCALE;
    return -(penalty * phase_mg) / 256;
}

// =============================================================================
// MAIN EVALUATION FUNCTION
// =============================================================================

Score evaluate(const Board& board) {
    int   phase_mg    = game_phase(board);
    Score white_score = 0;
    Score black_score = 0;

    // Sum material and positional scores for every piece on the board.
    // KING_VALUE is 0, so the king contributes only its PST bonus.
    for (int pt = PAWN; pt <= KING; pt++) {
        PieceType piece_type = PieceType(pt);

        Bitboard wb = board.piece_bb(WHITE, piece_type);
        while (wb) {
            Square sq    = pop_lsb(wb);
            white_score += PIECE_VALUE[pt];
            white_score += pst_value(piece_type, WHITE, sq, phase_mg);
        }

        Bitboard bb = board.piece_bb(BLACK, piece_type);
        while (bb) {
            Square sq    = pop_lsb(bb);
            black_score += PIECE_VALUE[pt];
            black_score += pst_value(piece_type, BLACK, sq, phase_mg);
        }
    }

    // King safety: applied symmetrically — both kings are evaluated.
    // Each call returns a penalty (negative value) for 'us' king under attack.
    white_score += king_safety(board, WHITE, phase_mg);
    black_score += king_safety(board, BLACK, phase_mg);

    // Compute score from White's perspective, then flip for the side to move
    Score score = white_score - black_score;
    return (board.side_to_move == WHITE) ? score : -score;
}
