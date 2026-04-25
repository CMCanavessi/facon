// =============================================================================
// Last modified: 2026-04-18 22:51
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
//
// Facon 1.2 — Rojo Vivo
//   - Mopup evaluation: in pawnless endings with a decisive material
//     advantage, adds a bonus to the winning side for: (a) pushing the
//     losing king toward a corner (Manhattan distance from central region),
//     and (b) closing the distance between kings (14 - manhattan_distance).
//     Activated only when no pawns remain and the raw material+PST advantage
//     exceeds MOPUP_THRESHOLD. Constants defined in eval.h.
//
// Facon 1.3 — Yunque
//   - Pawn structure evaluation: five terms computed via bitboard operations
//     with no per-square loops. All terms are from White's perspective and
//     mirrored for Black by flipping rank bits.
//     * Isolated pawns: no friendly pawns on adjacent files. Penalty per pawn.
//     * Doubled pawns: more than one pawn on the same file. Penalty per extra.
//     * Backward pawns: cannot advance safely, cannot be supported by a
//       friendly pawn. Penalty per pawn.
//     * Passed pawns: no enemy pawn can block or attack on the path to
//       promotion. Bonus scaled by rank (rank 7 = largest bonus).
//     * Connected pawns: supported diagonally by a friendly pawn. Small bonus.
//     Constants defined in eval.h.
//
// Facon 1.4 — Hoja
//   - Positional evaluation (positional_eval): mobility for knights, bishops,
//     rooks, queens (count of pseudo-legal squares, excluding own pieces).
//     Rook on open/semi-open files and 7th rank. Bishop pair bonus. Knight
//     outpost bonus for knights that cannot be attacked by enemy pawns.
//     All terms computed via bitboard operations, constants in eval.h.
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
// and the double count reflects that naturally. KING_ATTACK_WEIGHT[QUEEN]
// is defined for completeness but never indexed directly.
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
    5,  // QUEEN  — defined for completeness; queens are counted twice via
        //          bishop+rook loops, so this value is never indexed directly
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
// MOPUP EVALUATION
// =============================================================================
// In pawnless endings with a decisive material advantage, the PST king tables
// alone are insufficient to guide conversion — the engine may wander without
// making progress. Mopup adds a bonus for two things:
//
//   1. Corner distance of the losing king: how far the weak king is from the
//      center. We want to maximize this — a king in the corner has fewer
//      escape squares and is easier to checkmate.
//      Range: 0 (in the central 2x2 region) to 6 (corner). Computed as the
//      Manhattan distance from the d-e file range x ranks 4-5.
//
//   2. King proximity: the Manhattan distance between the two kings.
//      We want to minimize this, so we reward (14 - distance).
//      Range: 0..14. A score of 14 means the kings are adjacent.
//
// The bonus is added to the winning side's score from White's perspective,
// then returned as a signed value (positive if White is winning, negative
// if Black is winning) to be added directly to the main eval score.
//
// Activation conditions:
//   - No pawns remain on the board.
//   - The raw material+PST advantage exceeds MOPUP_THRESHOLD (300cp).
//     Below this threshold the position may be drawn — mopup would
//     cause the engine to chase the opponent's king in a drawn endgame.
//
// This function is only called when both conditions are already verified
// by evaluate(). It does not re-check them internally.

// Returns the Manhattan distance from the central 2x2 region of the board
// (d-e files x ranks 4-5). Returns 0 if the square is within that region,
// up to 6 for a corner square.
static int center_distance(Square sq) {
    int file = file_of(sq);
    int rank = rank_of(sq);
    // Horizontal distance from the nearest central file (d=3 or e=4).
    // std::max(a, b) is always >= 0 here since at least one of the two
    // expressions is non-negative for any file in [0..7].
    int fd = std::max(3 - file, file - 4);
    int rd = std::max(3 - rank, rank - 4);
    return std::max(fd, 0) + std::max(rd, 0);
}

// Returns the Manhattan distance between two squares.
static int king_distance(Square a, Square b) {
    return std::abs(file_of(a) - file_of(b))
         + std::abs(rank_of(a) - rank_of(b));
}

// Returns the mopup bonus in centipawns from White's perspective.
// strong_side: the side with the material advantage.
// weak_side:   the side being mated.
static Score mopup_eval(const Board& board, Color strong_side) {
    Color  weak_side    = ~strong_side;

    // Insufficient material guard: K+B vs K and K+N vs K are theoretical
    // draws — no sequence of moves can force checkmate. Without this guard,
    // K+B (330cp) and K+N (320cp) exceed MOPUP_THRESHOLD (300cp) and activate
    // corner-chasing in drawn endings, causing the engine to refuse draws and
    // wander indefinitely.
    Bitboard strong_pieces = board.all_pieces(strong_side);
    int strong_count = popcount(strong_pieces);  // includes king
    if (strong_count == 2) {
        // King + one piece. Check if that piece is a lone bishop or knight.
        if (board.piece_bb(strong_side, BISHOP) || board.piece_bb(strong_side, KNIGHT))
            return 0;
    }

    Square strong_king  = board.king_square(strong_side);
    Square weak_king    = board.king_square(weak_side);

    // Push the weak king toward the corner and close the distance.
    int bonus = MOPUP_CORNER_WEIGHT   *  center_distance(weak_king)
              + MOPUP_PROXIMITY_WEIGHT * (14 - king_distance(strong_king, weak_king));

    // Return from White's perspective: positive if White is winning, negative
    // if Black is winning. evaluate() will flip for side to move as usual.
    return (strong_side == WHITE) ? Score(bonus) : Score(-bonus);
}

// =============================================================================
// PAWN STRUCTURE EVALUATION
// =============================================================================
// Evaluates pawn structure for both sides and returns the score from White's
// perspective. All terms use bitboard operations — no per-square loops.
//
// The five terms and their intuition:
//
// ISOLATED: a pawn with no friendly pawns on adjacent files has no support
//   and controls fewer squares. Penalty per isolated pawn.
//
// DOUBLED: two pawns on the same file block each other, only one can advance.
//   Penalty per extra pawn beyond the first on each file.
//
// BACKWARD: a pawn that cannot advance safely (the stop square is attacked by
//   an enemy pawn) and cannot be supported by a friendly pawn push. This is
//   the weakest pawn structure weakness: the pawn is stuck and an easy target.
//   Penalty per backward pawn.
//
// PASSED: no enemy pawn can ever capture or block this pawn on its path to
//   promotion. Passed pawns are winning endgame assets. Bonus scaled by rank:
//   the closer to promotion, the larger the bonus.
//
// CONNECTED: a pawn diagonally supported by a friendly pawn. Connected pawns
//   are mobile and mutually defending — harder to attack than isolated ones.
//   Small bonus per connected pawn.
//
// All computations are symmetric: White uses the actual bitboard operations,
// Black uses the same logic with north/south swapped (shift direction inverted).

// Returns a bitboard with all squares on a given file set.
// Used to detect doubled pawns and build passed pawn masks.
static inline Bitboard file_bb(int f) {
    return FILE_A_BB << f;
}

// Returns a bitboard of all squares on files adjacent to file f (but not f itself).
// Used to detect isolated pawns: if (pawns & adjacent_files(f)) == 0, isolated.
static inline Bitboard adjacent_files_bb(int f) {
    Bitboard adj = 0;
    if (f > 0) adj |= FILE_A_BB << (f - 1);
    if (f < 7) adj |= FILE_A_BB << (f + 1);
    return adj;
}

// Bonus indexed by rank (0=rank1 .. 7=rank8) for passed pawns.
// Rank 0 and 1 are unreachable for a passed pawn (would have promoted).
// Bonus grows sharply in the last three ranks where promotion is imminent.
static const int PASSED_BONUS[8] = { 0, 0, 10, 20, 35, 55, 80, 0 };
// Rank 7 is 0 because a pawn on rank 8 has promoted — can't happen.

static Score pawn_structure(const Board& board) {
    Score white_score = 0;
    Score black_score = 0;

    Bitboard white_pawns = board.piece_bb(WHITE, PAWN);
    Bitboard black_pawns = board.piece_bb(BLACK, PAWN);

    // Precompute attack spans for both sides:
    // white_attack_span: all squares that white pawns attack, extended forward.
    // A pawn on e4 attacks d5 and f5, and "controls" d6,d7,d8,f6,f7,f8 too.
    // We need the full forward span to detect passed pawns and backward pawns.
    //
    // fill_north / fill_south: flood-fill in one direction through all ranks.
    // We build these manually via repeated shifts.

    // Pawn attack spans (all squares attacked on the path to promotion):
    // White: shift NE and NW for all white pawns, then fill north.
    // Black: shift SE and SW for all black pawns, then fill south.

    // Helper: fill a bitboard northward through all ranks
    auto fill_north = [](Bitboard b) -> Bitboard {
        b |= b << 8; b |= b << 16; b |= b << 32;
        return b;
    };
    auto fill_south = [](Bitboard b) -> Bitboard {
        b |= b >> 8; b |= b >> 16; b |= b >> 32;
        return b;
    };

    // Attack span: squares attacked on the path to promotion.
    // Used for: (1) passed pawn detection, (2) backward pawn detection.
    Bitboard white_attack_span = fill_north(pawn_attacks_bb(WHITE, white_pawns));
    Bitboard black_attack_span = fill_south(pawn_attacks_bb(BLACK, black_pawns));

    // -------------------------------------------------------------------------
    // Evaluate each color's pawn structure
    // -------------------------------------------------------------------------
    // We loop over files (0..7) for doubled/isolated detection,
    // then loop over individual pawns for backward/passed/connected.

    for (Color c : {WHITE, BLACK}) {
        Bitboard our_pawns   = (c == WHITE) ? white_pawns  : black_pawns;
        Bitboard their_pawns = (c == WHITE) ? black_pawns  : white_pawns;
        Score& score = (c == WHITE) ? white_score : black_score;

        // --- ISOLATED and DOUBLED: loop over files ---
        for (int f = 0; f < 8; f++) {
            Bitboard pawns_on_file = our_pawns & file_bb(f);
            if (!pawns_on_file) continue;

            int count = popcount(pawns_on_file);

            // Doubled: more than one pawn on the file
            if (count > 1)
                score += PAWN_DOUBLED * (count - 1);

            // Isolated: no friendly pawns on adjacent files
            if (!(our_pawns & adjacent_files_bb(f)))
                score += PAWN_ISOLATED * count;
        }

        // --- BACKWARD, PASSED, CONNECTED: loop over individual pawns ---
        Bitboard pawns = our_pawns;
        while (pawns) {
            Square sq   = pop_lsb(pawns);
            int    rank = rank_of(sq);

            // Stop square: the square directly in front of this pawn.
            // White: one rank up. Black: one rank down.
            Square stop = (c == WHITE) ? Square(sq + 8) : Square(sq - 8);

            // --- PASSED ---
            // A pawn is passed if no enemy pawn occupies its front span
            // (directly ahead) or its attack span (diagonally ahead).
            // We build both spans for this specific pawn and check against
            // the enemy pawn bitboard.
            Bitboard this_front = fill_north(square_bb(stop));
            Bitboard this_attack_forward = fill_north(pawn_attacks_bb(c, square_bb(sq)));
            if (c == BLACK) {
                this_front        = fill_south(square_bb(stop));
                this_attack_forward = fill_south(pawn_attacks_bb(c, square_bb(sq)));
            }
            bool is_passed = !(their_pawns & (this_front | this_attack_forward));
            if (is_passed) {
                // Bonus by rank from our perspective: White rank 0-7, Black mirror.
                int bonus_rank = (c == WHITE) ? rank : (7 - rank);
                score += PASSED_BONUS[bonus_rank];
            }

            // --- BACKWARD ---
            // A pawn is backward if:
            //   1. Its stop square is attacked by an enemy pawn.
            //   2. There is no friendly pawn that could advance to support it.
            //      (i.e., no friendly pawn on adjacent files that is on the same
            //       rank or behind this pawn that could push forward to defend.)
            // We approximate condition 2 as: this pawn is NOT in the attack span
            // of any friendly pawn (meaning no friend attacks the stop square
            // from behind).
            Bitboard friendly_attack_span = (c == WHITE) ? white_attack_span
                                                         : black_attack_span;
            bool stop_attacked_by_enemy = (pawn_attacks_bb(c, square_bb(stop))
                                           & their_pawns) != 0;
            bool no_friendly_support    = !(square_bb(sq) & friendly_attack_span);
            // Don't double-count isolated (which is already penalized above).
            // Backward only applies when the pawn is genuinely stuck.
            if (stop_attacked_by_enemy && no_friendly_support && !is_passed)
                score += PAWN_BACKWARD;

            // --- CONNECTED ---
            // A pawn is connected if it is diagonally supported by a friendly pawn
            // (i.e., a friendly pawn attacks this pawn's square).
            if (pawn_attacks_bb(~c, square_bb(sq)) & our_pawns)
                score += PAWN_CONNECTED;
        }
    }

    return white_score - black_score;
}

// =============================================================================
// POSITIONAL EVALUATION (Facon 1.4)
// =============================================================================
// Evaluates positional factors beyond material and PSTs: piece mobility,
// rook placement, bishop pair, and knight outposts. Returns a score from
// White's perspective.

static Score positional_eval(const Board& board) {
    Score white_score = 0;
    Score black_score = 0;

    Bitboard occ         = board.occupancy();
    Bitboard white_pawns = board.piece_bb(WHITE, PAWN);
    Bitboard black_pawns = board.piece_bb(BLACK, PAWN);

    // Pawn attack spans: squares attacked by enemy pawns. Used to exclude
    // unsafe squares from mobility and to detect knight outposts.
    Bitboard white_pawn_attacks = pawn_attacks_bb(WHITE, white_pawns);
    Bitboard black_pawn_attacks = pawn_attacks_bb(BLACK, black_pawns);

    // --- Process each color ---
    for (int c = WHITE; c <= BLACK; c++) {
        Color us   = Color(c);
        Score& our_score = (us == WHITE) ? white_score : black_score;

        Bitboard our_pieces   = board.by_color[us];
        Bitboard enemy_pawn_attacks = (us == WHITE) ? black_pawn_attacks : white_pawn_attacks;

        // Squares available for mobility: not occupied by own pieces and
        // not attacked by enemy pawns (unsafe squares are penalized implicitly
        // by excluding them from the mobility count).
        Bitboard mob_area = ~our_pieces & ~enemy_pawn_attacks;

        // ----- KNIGHT MOBILITY + OUTPOSTS -----
        Bitboard knights = board.piece_bb(us, KNIGHT);
        while (knights) {
            Square sq = pop_lsb(knights);
            int moves = popcount(knight_attack(sq) & mob_area);
            our_score += MOBILITY_KNIGHT * moves;

            // Knight outpost: on ranks 4-6 (relative), supported by own pawn,
            // and no enemy pawn can attack the square.
            int rel_rank = (us == WHITE) ? rank_of(sq) : 7 - rank_of(sq);
            if (rel_rank >= 3 && rel_rank <= 5) {
                // Check if any enemy pawn on adjacent files can advance to attack this square.
                // Use forward fill of adjacent files from enemy pawn perspective.
                int f = file_of(sq);
                Bitboard adj = adjacent_files_bb(f);
                Bitboard enemy_pawns_adj = (us == WHITE) ? (black_pawns & adj) : (white_pawns & adj);

                // Enemy pawns behind or beside this square cannot attack it.
                // Only pawns ahead of this square (from enemy's perspective) matter.
                Bitboard forward_mask = (us == WHITE)
                    ? ~((1ULL << (8 * rank_of(sq))) - 1)      // ranks above sq
                    : ((1ULL << (8 * (rank_of(sq) + 1))) - 1); // ranks below sq

                if (!(enemy_pawns_adj & forward_mask)) {
                    our_score += KNIGHT_OUTPOST;
                }
            }
        }

        // ----- BISHOP MOBILITY + PAIR -----
        Bitboard bishops = board.piece_bb(us, BISHOP);

        // Bishop pair: both bishops present
        if (popcount(bishops) >= 2)
            our_score += BISHOP_PAIR_BONUS;

        while (bishops) {
            Square sq = pop_lsb(bishops);
            int moves = popcount(bishop_attack(sq, occ) & mob_area);
            our_score += MOBILITY_BISHOP * moves;
        }

        // ----- ROOK MOBILITY + FILES + 7TH RANK -----
        Bitboard rooks = board.piece_bb(us, ROOK);
        while (rooks) {
            Square sq = pop_lsb(rooks);
            int moves = popcount(rook_attack(sq, occ) & mob_area);
            our_score += MOBILITY_ROOK * moves;

            // Open/semi-open file
            Bitboard this_file = file_bb(file_of(sq));
            Bitboard our_pawns_on_file   = (us == WHITE) ? (white_pawns & this_file)
                                                         : (black_pawns & this_file);
            Bitboard their_pawns_on_file = (us == WHITE) ? (black_pawns & this_file)
                                                         : (white_pawns & this_file);
            if (!our_pawns_on_file) {
                if (!their_pawns_on_file)
                    our_score += ROOK_OPEN_FILE;
                else
                    our_score += ROOK_SEMI_OPEN_FILE;
            }

            // Rook on 7th rank (relative)
            int rel_rank_r = (us == WHITE) ? rank_of(sq) : 7 - rank_of(sq);
            if (rel_rank_r == 6)  // 0-indexed rank 6 = 7th rank
                our_score += ROOK_ON_7TH;
        }

        // ----- QUEEN MOBILITY -----
        Bitboard queens = board.piece_bb(us, QUEEN);
        while (queens) {
            Square sq = pop_lsb(queens);
            Bitboard attacks = bishop_attack(sq, occ) | rook_attack(sq, occ);
            int moves = popcount(attacks & mob_area);
            our_score += MOBILITY_QUEEN * moves;
        }
    }

    return white_score - black_score;
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

    // Compute score from White's perspective, then flip for the side to move.
    Score score = white_score - black_score;

    // Pawn structure: isolated, doubled, backward, passed, connected.
    // Returned from White's perspective — added directly to score.
    score += pawn_structure(board);

    // Positional evaluation: mobility, open files, rook on 7th, bishop pair,
    // knight outposts. Returned from White's perspective.
    score += positional_eval(board);

    // -------------------------------------------------------------------------
    // MOPUP EVALUATION
    // -------------------------------------------------------------------------
    // In pawnless endings with a decisive material advantage, guide conversion
    // by rewarding corner confinement of the losing king and king proximity.
    // Skipped when pawns are present (pawn endings have their own logic) or
    // when the advantage is below the threshold (avoids chasing in drawn endings).
    bool no_pawns = board.piece_bb(PAWN) == 0;
    if (no_pawns && std::abs(score) >= MOPUP_THRESHOLD) {
        Color strong_side = (score > 0) ? WHITE : BLACK;
        score += mopup_eval(board, strong_side);
    }

    return (board.side_to_move == WHITE) ? score : -score;
}
