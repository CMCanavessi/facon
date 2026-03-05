// =============================================================================
// bitboard.cpp — Bitboard initialization and attack table generation
//
// At startup, init_bitboards() fills every attack table used by the engine.
// Magic numbers for sliding pieces are found automatically using a randomized
// search — this guarantees correctness and avoids hardcoded values.
// =============================================================================

#include "bitboard.h"
#include <iostream>
#include <random>
#include <vector>
#include <algorithm>

// =============================================================================
// TABLE DEFINITIONS
// =============================================================================

Bitboard pawn_attacks[2][64];
Bitboard knight_attacks[64];
Bitboard king_attacks[64];

Magic    bishop_magics[64];
Magic    rook_magics[64];

Bitboard bishop_attack_table[0x1480];
Bitboard rook_attack_table[0x19000];

Bitboard between_bb[64][64];
Bitboard line_bb[64][64];

// =============================================================================
// SLOW ATTACK FUNCTIONS
// =============================================================================
// These compute sliding piece attacks ray by ray, blocking on occupied squares.
// They are ONLY used during initialization — both to fill the magic attack
// tables and to build the between/line tables. All runtime attack lookups
// go through the magic tables instead.

static Bitboard rook_attacks_slow(Square sq, Bitboard occupied) {
    Bitboard attacks = 0;
    int r = rank_of(sq), f = file_of(sq);

    for (int rr = r+1; rr <= 7; rr++) {
        attacks |= square_bb(make_square(f, rr));
        if (occupied & square_bb(make_square(f, rr))) break;
    }
    for (int rr = r-1; rr >= 0; rr--) {
        attacks |= square_bb(make_square(f, rr));
        if (occupied & square_bb(make_square(f, rr))) break;
    }
    for (int ff = f+1; ff <= 7; ff++) {
        attacks |= square_bb(make_square(ff, r));
        if (occupied & square_bb(make_square(ff, r))) break;
    }
    for (int ff = f-1; ff >= 0; ff--) {
        attacks |= square_bb(make_square(ff, r));
        if (occupied & square_bb(make_square(ff, r))) break;
    }
    return attacks;
}

static Bitboard bishop_attacks_slow(Square sq, Bitboard occupied) {
    Bitboard attacks = 0;
    int r = rank_of(sq), f = file_of(sq);

    for (int rr=r+1, ff=f+1; rr<=7 && ff<=7; rr++,ff++) {
        attacks |= square_bb(make_square(ff, rr));
        if (occupied & square_bb(make_square(ff, rr))) break;
    }
    for (int rr=r+1, ff=f-1; rr<=7 && ff>=0; rr++,ff--) {
        attacks |= square_bb(make_square(ff, rr));
        if (occupied & square_bb(make_square(ff, rr))) break;
    }
    for (int rr=r-1, ff=f+1; rr>=0 && ff<=7; rr--,ff++) {
        attacks |= square_bb(make_square(ff, rr));
        if (occupied & square_bb(make_square(ff, rr))) break;
    }
    for (int rr=r-1, ff=f-1; rr>=0 && ff>=0; rr--,ff--) {
        attacks |= square_bb(make_square(ff, rr));
        if (occupied & square_bb(make_square(ff, rr))) break;
    }
    return attacks;
}

// =============================================================================
// OCCUPANCY MASKS
// =============================================================================
// The mask contains the "relevant" squares for a sliding piece on a given
// square — the squares whose occupancy actually affects the attack rays.
// Edge squares are excluded: the ray always stops at the board edge regardless
// of what is there, so including them would waste bits in the magic index.

static Bitboard rook_mask(Square sq) {
    Bitboard mask = 0;
    int r = rank_of(sq), f = file_of(sq);
    for (int rr = r+1; rr <= 6; rr++) mask |= square_bb(make_square(f, rr));
    for (int rr = r-1; rr >= 1; rr--) mask |= square_bb(make_square(f, rr));
    for (int ff = f+1; ff <= 6; ff++) mask |= square_bb(make_square(ff, r));
    for (int ff = f-1; ff >= 1; ff--) mask |= square_bb(make_square(ff, r));
    return mask;
}

static Bitboard bishop_mask(Square sq) {
    Bitboard mask = 0;
    int r = rank_of(sq), f = file_of(sq);
    for (int rr=r+1,ff=f+1; rr<=6 && ff<=6; rr++,ff++) mask |= square_bb(make_square(ff,rr));
    for (int rr=r+1,ff=f-1; rr<=6 && ff>=1; rr++,ff--) mask |= square_bb(make_square(ff,rr));
    for (int rr=r-1,ff=f+1; rr>=1 && ff<=6; rr--,ff++) mask |= square_bb(make_square(ff,rr));
    for (int rr=r-1,ff=f-1; rr>=1 && ff>=1; rr--,ff--) mask |= square_bb(make_square(ff,rr));
    return mask;
}

// =============================================================================
// MAGIC NUMBER FINDER
// =============================================================================
// For a given square and piece type, finds a magic number that produces a
// collision-free mapping from occupancy subsets to attack table indices.
//
// The mapping is:   index = ((occupancy & mask) * magic) >> (64 - bits)
//
// We try random candidates until one works: no two different occupancy subsets
// produce the same index with conflicting attack values.
// "Constructive collisions" (same index, same attack value) are allowed.
// In practice a working magic is found in under 1000 attempts per square.

static Bitboard find_magic(Square sq, bool is_bishop, std::mt19937_64& rng) {
    Bitboard mask  = is_bishop ? bishop_mask(sq) : rook_mask(sq);
    int      bits  = popcount(mask);
    int      n     = 1 << bits;   // Total number of occupancy subsets
    int      shift = 64 - bits;

    // Pre-compute all occupancy subsets and their corresponding attacks.
    // The carry-rippler trick enumerates all subsets of 'mask' in order.
    std::vector<Bitboard> occ(n), atk(n);
    Bitboard subset = 0;
    for (int i = 0; i < n; i++) {
        occ[i] = subset;
        atk[i] = is_bishop ? bishop_attacks_slow(sq, subset)
                            : rook_attacks_slow(sq, subset);
        subset = (subset - mask) & mask;  // Carry-rippler: advance to next subset
    }

    // Scratch table used to detect collisions during the candidate search
    std::vector<Bitboard> table(n);

    // Try random candidates. ANDing three randoms biases toward sparse bit
    // patterns, which empirically produce better (faster-found) magic numbers.
    for (;;) {
        Bitboard magic = rng() & rng() & rng();

        // Quick filter: skip candidates that are clearly too sparse
        if (popcount((mask * magic) >> 56) < 6) continue;

        // Test this candidate against all occupancy subsets
        std::fill(table.begin(), table.end(), 0ULL);
        bool ok = true;

        for (int i = 0; i < n; i++) {
            unsigned idx = unsigned(((occ[i] & mask) * magic) >> shift);

            if (table[idx] == 0ULL) {
                table[idx] = atk[i];          // Empty slot: store attack
            } else if (table[idx] != atk[i]) {
                ok = false;                   // Destructive collision: reject
                break;
            }
            // table[idx] == atk[i]: constructive collision, perfectly fine
        }

        if (ok) return magic;  // This magic works for all subsets
    }
}

// =============================================================================
// MAGIC TABLE INITIALIZATION (one square at a time)
// =============================================================================

static void init_magic(Square sq, Bitboard* table, bool is_bishop,
                        std::mt19937_64& rng) {
    Bitboard mask  = is_bishop ? bishop_mask(sq) : rook_mask(sq);
    int      bits  = popcount(mask);
    int      shift = 64 - bits;
    int      n     = 1 << bits;

    // Find a working magic number for this square
    Bitboard magic = find_magic(sq, is_bishop, rng);

    // Store the magic entry so the attack getters in bitboard.h can use it
    Magic& m  = is_bishop ? bishop_magics[sq] : rook_magics[sq];
    m.mask    = mask;
    m.magic   = magic;
    m.attacks = table;
    m.shift   = shift;

    // Fill the attack table: for every occupancy subset, store the attacks
    Bitboard subset = 0;
    for (int i = 0; i < n; i++) {
        unsigned idx = m.index(subset);
        table[idx]   = is_bishop ? bishop_attacks_slow(sq, subset)
                                 : rook_attacks_slow(sq, subset);
        subset = (subset - mask) & mask;  // Carry-rippler: next subset
    }
}

// =============================================================================
// MAIN INITIALIZATION
// =============================================================================

void init_bitboards() {

    // -------------------------------------------------------------------------
    // 1. PAWN ATTACKS
    // White pawns attack north-east and north-west.
    // Black pawns attack south-east and south-west.
    // -------------------------------------------------------------------------
    for (Square s = A1; s <= H8; s = Square(s + 1)) {
        Bitboard bb = square_bb(s);
        pawn_attacks[WHITE][s] = shift_north_east(bb) | shift_north_west(bb);
        pawn_attacks[BLACK][s] = shift_south_east(bb) | shift_south_west(bb);
    }

    // -------------------------------------------------------------------------
    // 2. KNIGHT ATTACKS
    // Knights jump in an L-shape: 2 squares in one direction + 1 perpendicular.
    // File masks prevent wrap-around when shifting across the board edge.
    // -------------------------------------------------------------------------
    for (Square s = A1; s <= H8; s = Square(s + 1)) {
        Bitboard bb     = square_bb(s);
        Bitboard not_a  = ~FILE_A_BB;               // Can move 1 step west
        Bitboard not_ab = ~(FILE_A_BB | FILE_B_BB); // Can move 2 steps west
        Bitboard not_h  = ~FILE_H_BB;               // Can move 1 step east
        Bitboard not_gh = ~(FILE_G_BB | FILE_H_BB); // Can move 2 steps east

        knight_attacks[s] =
            ((bb & not_ab) >> 10) |  // 2 west, 1 south
            ((bb & not_a ) >> 17) |  // 1 west, 2 south
            ((bb & not_h ) >> 15) |  // 1 east, 2 south
            ((bb & not_gh) >>  6) |  // 2 east, 1 south
            ((bb & not_gh) << 10) |  // 2 east, 1 north
            ((bb & not_h ) << 17) |  // 1 east, 2 north
            ((bb & not_a ) << 15) |  // 1 west, 2 north
            ((bb & not_ab) <<  6);   // 2 west, 1 north
    }

    // -------------------------------------------------------------------------
    // 3. KING ATTACKS
    // King moves one step in any of the 8 directions.
    // -------------------------------------------------------------------------
    for (Square s = A1; s <= H8; s = Square(s + 1)) {
        Bitboard bb = square_bb(s);
        king_attacks[s] =
            shift_north(bb)      | shift_south(bb)      |
            shift_east(bb)       | shift_west(bb)        |
            shift_north_east(bb) | shift_north_west(bb)  |
            shift_south_east(bb) | shift_south_west(bb);
    }

    // -------------------------------------------------------------------------
    // 4. MAGIC BITBOARDS FOR BISHOPS AND ROOKS
    // A fixed RNG seed makes the magic numbers reproducible across runs.
    // Each square gets a contiguous slice of the flat attack table.
    // -------------------------------------------------------------------------
    std::cerr << "Initializing magic bitboards..." << std::flush;
    std::mt19937_64 rng(0xDEADBEEFCAFE0000ULL);

    Bitboard* b_ptr = bishop_attack_table;
    Bitboard* r_ptr = rook_attack_table;

    for (Square s = A1; s <= H8; s = Square(s + 1)) {
        init_magic(s, b_ptr, /*is_bishop=*/true,  rng);
        b_ptr += (1 << popcount(bishop_mask(s)));  // Advance by this square's slice size

        init_magic(s, r_ptr, /*is_bishop=*/false, rng);
        r_ptr += (1 << popcount(rook_mask(s)));
    }
    std::cerr << " done.\n" << std::flush;

    // -------------------------------------------------------------------------
    // 5. BETWEEN AND LINE TABLES
    // between_bb[s1][s2] = squares strictly between s1 and s2 on a shared ray.
    // line_bb[s1][s2]    = all squares on the full ray through s1 and s2.
    // Both tables are zero if s1 and s2 do not share a ray.
    // -------------------------------------------------------------------------
    for (Square s1 = A1; s1 <= H8; s1 = Square(s1 + 1)) {
        for (Square s2 = A1; s2 <= H8; s2 = Square(s2 + 1)) {
            if (bishop_attacks_slow(s1, 0) & square_bb(s2)) {
                // s1 and s2 share a diagonal ray
                between_bb[s1][s2] =
                    bishop_attacks_slow(s1, square_bb(s2)) &
                    bishop_attacks_slow(s2, square_bb(s1));
                line_bb[s1][s2] =
                    (bishop_attacks_slow(s1, 0) & bishop_attacks_slow(s2, 0))
                    | square_bb(s1) | square_bb(s2);
            }
            else if (rook_attacks_slow(s1, 0) & square_bb(s2)) {
                // s1 and s2 share a straight ray
                between_bb[s1][s2] =
                    rook_attacks_slow(s1, square_bb(s2)) &
                    rook_attacks_slow(s2, square_bb(s1));
                line_bb[s1][s2] =
                    (rook_attacks_slow(s1, 0) & rook_attacks_slow(s2, 0))
                    | square_bb(s1) | square_bb(s2);
            }
            // Squares not on a shared ray: between_bb and line_bb stay zero
        }
    }
}

// =============================================================================
// DEBUG UTILITY
// =============================================================================

void print_bitboard(Bitboard bb, const std::string& label) {
    if (!label.empty())
        std::cout << "  " << label << "\n";
    std::cout << "  +-----------------+\n";
    for (int rank = 7; rank >= 0; rank--) {
        std::cout << "  " << (rank + 1) << "| ";
        for (int file = 0; file < 8; file++) {
            Square s = make_square(file, rank);
            std::cout << (test_bit(bb, s) ? "X " : ". ");
        }
        std::cout << "|\n";
    }
    std::cout << "  +-----------------+\n";
    std::cout << "    a b c d e f g h\n\n";
}
