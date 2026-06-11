// =============================================================================
// Last modified: 2026-06-06 16:05
// bitboard.cpp -- Bitboard initialization and attack table generation
//
// At startup, init_bitboards() fills every attack table used by the engine.
// Sliding-piece (bishop/rook) lookups use magic bitboards. The magic numbers
// are hardcoded constants (see HARDCODED MAGIC NUMBERS below), so startup only
// fills the attack tables from them, with no search.
//
// Facon 1.3 -- Yunque
//   - isatty()-gated init message: only printed when stderr is a terminal,
//     suppressed under a GUI or fastchess. (As of 1.6 the normal startup path
//     prints nothing here; the message survives only on the regeneration
//     path described below.)
//
// Facon 1.6 -- Temple
//   - Magic numbers hardcoded. Previously they were searched at startup with a
//     randomized algorithm using a fixed seed; the search took a noticeable
//     fraction of init time. The values that search produced are now baked in
//     as constants, making initialization effectively instant. The attacks are
//     bit-identical to the previous search-based ones.
//   - The randomized search is preserved below under FACON_REGENERATE_MAGICS so
//     the constants can be regenerated offline if the masks ever change; it is
//     not compiled into the normal build.
//   - Comment audit pass: non-ASCII punctuation in comments replaced with ASCII
//     equivalents for portability. No functional changes.
// =============================================================================

#include "bitboard.h"
#include <iostream>
#include <random>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#  include <io.h>
#  define IS_STDERR_INTERACTIVE() (_isatty(_fileno(stderr)))
#else
#  include <unistd.h>
#  define IS_STDERR_INTERACTIVE() (isatty(fileno(stderr)))
#endif

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
// They are ONLY used during initialization -- both to fill the magic attack
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
// square -- the squares whose occupancy actually affects the attack rays.
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
// HARDCODED MAGIC NUMBERS
// =============================================================================
// These magic numbers were found once (offline) by the randomized search below
// and are now baked in as constants. Hardcoding them removes the search from
// startup, making initialization effectively instant: init only fills the
// attack tables from these fixed magics. The values are tied to the mask and
// indexing scheme in this file (index = ((occ & mask) * magic) >> (64 - bits));
// they are not portable to a different mask layout. The search routine is kept
// below for provenance and so the constants can be regenerated if the masks
// ever change, but it is no longer called during normal startup.

// Bishop magics (found with seed 0xDEADBEEFCAFE0000)
static const Bitboard BISHOP_MAGIC_HARDCODED[64] = {
    0x4050301012908b30ULL,    0x0008110404024200ULL,
    0x129000808110105aULL,    0x2104040688080043ULL,
    0x0241104048072000ULL,    0x5001042005024081ULL,
    0x0004012108200031ULL,    0x00020303080a0212ULL,
    0x0001111022008412ULL,    0x02189110012304a0ULL,
    0x0201081806418000ULL,    0x1000c4104a001090ULL,
    0x1000040420c00000ULL,    0x1001482210108002ULL,
    0x0502020222210400ULL,    0x0208064402011082ULL,
    0x0011024042082100ULL,    0x0004006008009120ULL,
    0x2008002400401206ULL,    0x010c008240108000ULL,
    0x8001000820084892ULL,    0x0102000100808420ULL,
    0x0020806620942002ULL,    0x0004800200840124ULL,
    0x200404a0a0600440ULL,    0x4008484022100100ULL,
    0x0000880410004014ULL,    0x8248180002820042ULL,
    0x0201011043004004ULL,    0x4409020001082112ULL,
    0x1000840438940400ULL,    0x000202a0020b1304ULL,
    0x001010108e266400ULL,    0x3894020218210420ULL,
    0x2442109002080443ULL,    0x0c00040400080120ULL,
    0x0014150010040040ULL,    0x00b8080020211000ULL,
    0x9098048114208800ULL,    0x00030c08a2898610ULL,
    0x0002080440400400ULL,    0x4100480809880400ULL,
    0x0002001404000208ULL,    0x0002084204800800ULL,
    0x4000202012104100ULL,    0x01080200a4080200ULL,
    0x0020619404841108ULL,    0x80a408408c210d00ULL,
    0x0802080108889201ULL,    0x0010820890044012ULL,
    0x1800902108088000ULL,    0x4000100884040804ULL,
    0x0000181092088011ULL,    0x090404700a060004ULL,
    0xe00a900128010003ULL,    0x8048500080810200ULL,
    0x0001002215044040ULL,    0x0001060101019000ULL,
    0x0010420104091110ULL,    0x0008042041048802ULL,
    0x6401240008210110ULL,    0x04841110a001410cULL,
    0x3200401c4870a503ULL,    0x1040101200410028ULL,
};

// Rook magics (found with seed 0xDEADBEEFCAFE0000)
static const Bitboard ROOK_MAGIC_HARDCODED[64] = {
    0x2180104000208004ULL,    0x0040002000401000ULL,
    0x4080200008801000ULL,    0x0100041000082101ULL,
    0x0200102008020005ULL,    0x0900440006280300ULL,
    0x2080010002000080ULL,    0x82800e40a1000180ULL,
    0x0020800080204000ULL,    0x0409004000810026ULL,
    0xb880801000200081ULL,    0x4001001001002008ULL,
    0x0040800800800400ULL,    0x0042001002000804ULL,
    0x0904800200010080ULL,    0x0022800100105080ULL,
    0x40400080068029c0ULL,    0x4000260040820100ULL,
    0x0480828010002002ULL,    0x0180220008104200ULL,
    0x4800050008009101ULL,    0x0001010002040008ULL,
    0x0e0004001002a108ULL,    0x0011ce0000a40041ULL,
    0x0000209080004000ULL,    0x0000200c80400080ULL,
    0x0900110100200840ULL,    0x0001000900201000ULL,
    0x0009002500500800ULL,    0x0081000300040008ULL,
    0x9848010400100802ULL,    0x0040008200040041ULL,
    0x4880002000404000ULL,    0x8002200044401006ULL,
    0x0800801008802002ULL,    0x2010801000800800ULL,
    0x0000800402800800ULL,    0x8202001002000804ULL,
    0x0062004102006824ULL,    0x800120608a000401ULL,
    0x4080004020004000ULL,    0x0820003000404002ULL,
    0x0490002000108080ULL,    0x2041001002210008ULL,
    0x0005001008010004ULL,    0x0c24008002008004ULL,
    0x8011384610040043ULL,    0xe050010060820004ULL,
    0x0180022001400440ULL,    0x0000208208410200ULL,
    0x0020080010004040ULL,    0x0110010820110500ULL,
    0x0088802800040180ULL,    0x0146808200040080ULL,
    0x0000080201100400ULL,    0x6001410400806200ULL,
    0x0000681080030041ULL,    0x5800402900128202ULL,
    0x4101c90012406001ULL,    0x0210100009000421ULL,
    0x100a000851042032ULL,    0x0202000150088422ULL,
    0x9400c11000820834ULL,    0x000200c400910422ULL,
};

// =============================================================================
// MAGIC NUMBER FINDER (offline regeneration only)
// =============================================================================
// This randomized search produced the hardcoded constants above. It is no
// longer compiled into the engine; startup uses the constants directly. To
// regenerate the magics (e.g. if the masks change), build with
// -DFACON_REGENERATE_MAGICS, which re-enables this search and the wiring that
// calls it, then print bishop_magics[]/rook_magics[] and paste them above.
//
// The mapping is:   index = ((occupancy & mask) * magic) >> (64 - bits)
//
// We try random candidates until one works: no two different occupancy subsets
// produce the same index with conflicting attack values.
// "Constructive collisions" (same index, same attack value) are allowed.
#ifdef FACON_REGENERATE_MAGICS

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

#endif  // FACON_REGENERATE_MAGICS

// =============================================================================
// MAGIC TABLE INITIALIZATION (one square at a time)
// =============================================================================

static void init_magic(Square sq, Bitboard* table, bool is_bishop
#ifdef FACON_REGENERATE_MAGICS
                        , std::mt19937_64& rng
#endif
                        ) {
    Bitboard mask  = is_bishop ? bishop_mask(sq) : rook_mask(sq);
    int      bits  = popcount(mask);
    int      shift = 64 - bits;
    int      n     = 1 << bits;

#ifdef FACON_REGENERATE_MAGICS
    // Regeneration: search for a working magic at runtime.
    Bitboard magic = find_magic(sq, is_bishop, rng);
#else
    // Normal startup: use the precomputed constant (no search).
    Bitboard magic = is_bishop ? BISHOP_MAGIC_HARDCODED[sq]
                               : ROOK_MAGIC_HARDCODED[sq];
#endif

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
    // The magic numbers are hardcoded constants (see above); init only fills
    // the attack tables from them, so there is no startup search. Each square
    // gets a contiguous slice of the flat attack table. Under
    // FACON_REGENERATE_MAGICS the magics are instead searched at runtime using
    // a fixed RNG seed (reproducible) so the constants can be regenerated.
    // -------------------------------------------------------------------------
#ifdef FACON_REGENERATE_MAGICS
    if (IS_STDERR_INTERACTIVE()) std::cerr << "Searching magic bitboards..." << std::flush;
    std::mt19937_64 rng(0xDEADBEEFCAFE0000ULL);
#endif

    Bitboard* b_ptr = bishop_attack_table;
    Bitboard* r_ptr = rook_attack_table;

    for (Square s = A1; s <= H8; s = Square(s + 1)) {
        init_magic(s, b_ptr, /*is_bishop=*/true
#ifdef FACON_REGENERATE_MAGICS
                   , rng
#endif
                   );
        b_ptr += (1 << popcount(bishop_mask(s)));  // Advance by this square's slice size

        init_magic(s, r_ptr, /*is_bishop=*/false
#ifdef FACON_REGENERATE_MAGICS
                   , rng
#endif
                   );
        r_ptr += (1 << popcount(rook_mask(s)));
    }
#ifdef FACON_REGENERATE_MAGICS
    if (IS_STDERR_INTERACTIVE()) std::cerr << " done.\n" << std::flush;
#endif

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
