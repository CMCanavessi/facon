// =============================================================================
// bitboard.h — Bitboard definitions, utilities, and attack table declarations
//
// A bitboard is a 64-bit integer (uint64_t) where each bit corresponds to one
// square on the chess board. Bit 0 = a1, bit 63 = h8.
//
// This file provides:
//   - Bit manipulation utilities (set, clear, pop, count)
//   - Pre-computed attack tables for all piece types
//   - Magic bitboard structures for sliding pieces (bishop, rook)
//   - The init_bitboards() function that must be called at startup
// =============================================================================

#pragma once

#include "types.h"
#include <string>

// =============================================================================
// COMPILE-TIME BITBOARD CONSTANTS
// =============================================================================

// Full files (all squares on a given file)
constexpr Bitboard FILE_A_BB = 0x0101010101010101ULL;
constexpr Bitboard FILE_B_BB = FILE_A_BB << 1;
constexpr Bitboard FILE_C_BB = FILE_A_BB << 2;
constexpr Bitboard FILE_D_BB = FILE_A_BB << 3;
constexpr Bitboard FILE_E_BB = FILE_A_BB << 4;
constexpr Bitboard FILE_F_BB = FILE_A_BB << 5;
constexpr Bitboard FILE_G_BB = FILE_A_BB << 6;
constexpr Bitboard FILE_H_BB = FILE_A_BB << 7;

// Full ranks (all squares on a given rank)
constexpr Bitboard RANK_1_BB = 0x00000000000000FFULL;
constexpr Bitboard RANK_2_BB = RANK_1_BB << (8 * 1);
constexpr Bitboard RANK_3_BB = RANK_1_BB << (8 * 2);
constexpr Bitboard RANK_4_BB = RANK_1_BB << (8 * 3);
constexpr Bitboard RANK_5_BB = RANK_1_BB << (8 * 4);
constexpr Bitboard RANK_6_BB = RANK_1_BB << (8 * 5);
constexpr Bitboard RANK_7_BB = RANK_1_BB << (8 * 6);
constexpr Bitboard RANK_8_BB = RANK_1_BB << (8 * 7);

// All squares / empty board
constexpr Bitboard ALL_SQUARES = ~Bitboard(0);
constexpr Bitboard EMPTY_BB    = 0ULL;

// =============================================================================
// BIT MANIPULATION UTILITIES
// =============================================================================

// Return a bitboard with only the given square's bit set
inline Bitboard square_bb(Square s) {
    return Bitboard(1ULL << s);
}

// Test whether a square is set in a bitboard
inline bool test_bit(Bitboard bb, Square s) {
    return (bb >> s) & 1;
}

// Set a bit (turn square ON in the bitboard)
inline void set_bit(Bitboard& bb, Square s) {
    bb |= square_bb(s);
}

// Clear a bit (turn square OFF in the bitboard)
inline void clear_bit(Bitboard& bb, Square s) {
    bb &= ~square_bb(s);
}

// Toggle a bit (flip the square's state)
inline void toggle_bit(Bitboard& bb, Square s) {
    bb ^= square_bb(s);
}

// Count the number of set bits (number of pieces in the bitboard).
// __builtin_popcountll is a GCC/Clang intrinsic that uses the CPU's POPCNT
// instruction — much faster than a manual loop.
inline int popcount(Bitboard bb) {
    return __builtin_popcountll(bb);
}

// Find and REMOVE the least significant bit (lowest-numbered set square).
// Used to iterate over all set squares: while (bb) { sq = pop_lsb(bb); ... }
// __builtin_ctzll returns the count of trailing zeros = index of lowest bit.
inline Square pop_lsb(Bitboard& bb) {
    Square s = Square(__builtin_ctzll(bb));
    bb &= bb - 1;  // Clear the lowest set bit (classic bit trick)
    return s;
}

// Get the least significant bit square WITHOUT removing it
inline Square lsb(Bitboard bb) {
    return Square(__builtin_ctzll(bb));
}

// Get the most significant bit square (highest-numbered set square)
inline Square msb(Bitboard bb) {
    return Square(63 ^ __builtin_clzll(bb));
}

// =============================================================================
// SHIFTING BITBOARDS IN DIRECTIONS
// =============================================================================
// When we shift, we must mask out wrap-around: shifting east on file H would
// "wrap" to file A of the next rank. We prevent this by masking off the edge.

inline Bitboard shift_north     (Bitboard bb) { return bb << 8; }
inline Bitboard shift_south     (Bitboard bb) { return bb >> 8; }
inline Bitboard shift_east      (Bitboard bb) { return (bb & ~FILE_H_BB) << 1; }
inline Bitboard shift_west      (Bitboard bb) { return (bb & ~FILE_A_BB) >> 1; }
inline Bitboard shift_north_east(Bitboard bb) { return (bb & ~FILE_H_BB) << 9; }
inline Bitboard shift_north_west(Bitboard bb) { return (bb & ~FILE_A_BB) << 7; }
inline Bitboard shift_south_east(Bitboard bb) { return (bb & ~FILE_H_BB) >> 7; }
inline Bitboard shift_south_west(Bitboard bb) { return (bb & ~FILE_A_BB) >> 9; }

// =============================================================================
// MAGIC BITBOARD STRUCTURE
// =============================================================================
// For each square and each sliding piece, we store a "Magic" entry that lets
// us look up the attacks for any occupancy configuration in O(1) time.

struct Magic {
    Bitboard  mask;     // Relevant occupancy squares for this piece/square
    Bitboard  magic;    // The magic multiplier (found by randomized search)
    Bitboard* attacks;  // Pointer into the flat attack table for this square
    int       shift;    // Right-shift amount = 64 - popcount(mask)

    // Compute the index into the attacks table for a given board occupancy.
    // This is the core of the magic bitboard technique:
    //   index = ((occupancy & mask) * magic) >> shift
    unsigned index(Bitboard occupancy) const {
        return unsigned(((occupancy & mask) * magic) >> shift);
    }
};

// =============================================================================
// ATTACK TABLES (declared here, defined in bitboard.cpp)
// =============================================================================

// Non-sliding pieces: attacks are fixed regardless of occupancy
extern Bitboard pawn_attacks[2][64];  // [color][square]
extern Bitboard knight_attacks[64];   // [square]
extern Bitboard king_attacks[64];     // [square]

// Sliding pieces: attacks depend on occupancy — use magic bitboards
extern Magic bishop_magics[64];       // Magic entry for each square (bishop)
extern Magic rook_magics[64];         // Magic entry for each square (rook)

// Flat attack tables — Magic::attacks points into these arrays
extern Bitboard bishop_attack_table[0x1480];  // Total entries for all squares
extern Bitboard rook_attack_table[0x19000];

// Between-squares table: all squares STRICTLY between s1 and s2 on a ray.
// between_bb[s1][s2] is empty if s1 and s2 are not on the same ray.
// Used for check blocking and pin detection.
extern Bitboard between_bb[64][64];

// Line table: all squares on the full ray through s1 and s2 (including both).
// line_bb[s1][s2] is empty if s1 and s2 are not on the same ray.
// Used for pin detection.
extern Bitboard line_bb[64][64];

// =============================================================================
// ATTACK GETTERS (inline for speed — no function call overhead)
// =============================================================================

// Pawn attacks from a single square for the given color
inline Bitboard pawn_attack(Color c, Square s) {
    return pawn_attacks[c][s];
}

// Pawn attacks from a BITBOARD of pawns — computes all attacks in one step
inline Bitboard pawn_attacks_bb(Color c, Bitboard pawns) {
    if (c == WHITE)
        return shift_north_east(pawns) | shift_north_west(pawns);
    else
        return shift_south_east(pawns) | shift_south_west(pawns);
}

// Knight attacks from a square
inline Bitboard knight_attack(Square s) {
    return knight_attacks[s];
}

// King attacks from a square
inline Bitboard king_attack(Square s) {
    return king_attacks[s];
}

// Bishop attacks from a square given the current board occupancy
inline Bitboard bishop_attack(Square s, Bitboard occupancy) {
    return bishop_magics[s].attacks[bishop_magics[s].index(occupancy)];
}

// Rook attacks from a square given the current board occupancy
inline Bitboard rook_attack(Square s, Bitboard occupancy) {
    return rook_magics[s].attacks[rook_magics[s].index(occupancy)];
}

// Queen attacks = bishop attacks + rook attacks combined
inline Bitboard queen_attack(Square s, Bitboard occupancy) {
    return bishop_attack(s, occupancy) | rook_attack(s, occupancy);
}

// =============================================================================
// INITIALIZATION
// =============================================================================

// Must be called once at engine startup before anything else.
// Fills all attack tables and computes magic numbers.
void init_bitboards();

// Debug: print a bitboard visually to stdout (useful during development)
void print_bitboard(Bitboard bb, const std::string& label = "");
