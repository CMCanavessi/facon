// =============================================================================
// types.h — Core type definitions for the Facon chess engine
//
// This file defines all the fundamental types used throughout the engine:
// squares, pieces, colors, moves, and bitboards.
// Everything else in the engine builds on top of these types.
// =============================================================================

#pragma once

#include <cstdint>   // uint8_t, uint16_t, uint32_t, uint64_t
#include <string>    // std::string
#include <cassert>   // assert()
#include <cmath>     // std::abs() in is_mate_score()

// =============================================================================
// BASIC TYPES
// =============================================================================

// A Bitboard is a 64-bit integer where each bit represents one square.
// Bit 0 = a1, Bit 1 = b1, ..., Bit 7 = h1, Bit 8 = a2, ..., Bit 63 = h8
using Bitboard = uint64_t;

// =============================================================================
// COLOR
// =============================================================================

enum Color : uint8_t {
    WHITE    = 0,
    BLACK    = 1,
    NO_COLOR = 2
};

// Flip the color: WHITE -> BLACK, BLACK -> WHITE
inline Color operator~(Color c) {
    return Color(c ^ 1);
}

// =============================================================================
// PIECE TYPES
// =============================================================================

// The type of piece, regardless of color
enum PieceType : uint8_t {
    NO_PIECE_TYPE = 0,
    PAWN   = 1,
    KNIGHT = 2,
    BISHOP = 3,
    ROOK   = 4,
    QUEEN  = 5,
    KING   = 6
};

// A piece is a combination of color and piece type.
// Layout: bits [0..2] = piece type, bit [3] = color
// White pawn = 1, White knight = 2, ..., White king = 6
// Black pawn = 9, Black knight = 10, ..., Black king = 14
enum Piece : uint8_t {
    NO_PIECE = 0,
    W_PAWN   = 1,  W_KNIGHT = 2,  W_BISHOP = 3,
    W_ROOK   = 4,  W_QUEEN  = 5,  W_KING   = 6,
    B_PAWN   = 9,  B_KNIGHT = 10, B_BISHOP = 11,
    B_ROOK   = 12, B_QUEEN  = 13, B_KING   = 14
};

// Create a piece from color and type
inline Piece make_piece(Color c, PieceType pt) {
    return Piece((c << 3) | pt);
}

// Extract piece type from a piece
inline PieceType type_of(Piece p) {
    return PieceType(p & 7);
}

// Extract color from a piece (must not be NO_PIECE)
inline Color color_of(Piece p) {
    assert(p != NO_PIECE);
    return Color(p >> 3);
}

// =============================================================================
// SQUARES
// =============================================================================

// Squares are numbered 0..63 starting from a1=0 to h8=63.
// Layout: rank * 8 + file  (a1=0, b1=1, ..., h1=7, a2=8, ..., h8=63)
enum Square : uint8_t {
    A1, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,
    NO_SQUARE = 64
};

// Build a square from file (0=a..7=h) and rank (0=rank1..7=rank8)
inline Square make_square(int file, int rank) {
    return Square((rank << 3) | file);
}

// Extract file from a square (0=a .. 7=h)
inline int file_of(Square s) {
    return s & 7;
}

// Extract rank from a square (0=rank1 .. 7=rank8)
inline int rank_of(Square s) {
    return s >> 3;
}

// =============================================================================
// MOVES
// =============================================================================

// A move is packed into a 32-bit integer:
//
//  Bits  0.. 5 : from square        (6 bits, 0..63)
//  Bits  6..11 : to square          (6 bits, 0..63)
//  Bits 12..13 : move type          (2 bits, see MoveType)
//  Bits 14..15 : promotion piece    (2 bits, only valid when type == PROMOTION)
//
// This compact representation is fast to copy, compare, and store in the TT.
using Move = uint32_t;

// Move type flags stored in bits 12..13
enum MoveType : uint32_t {
    NORMAL     = 0,        // Regular move or capture
    PROMOTION  = 1 << 12,  // Pawn promotion (piece in bits 14..15)
    EN_PASSANT = 2 << 12,  // En passant capture
    CASTLING   = 3 << 12   // Kingside or queenside castling
};

// Sentinel value: no move (used in TT, search, and UCI output)
constexpr Move MOVE_NONE = 0;

// Build a normal or special move from its components
inline Move make_move(Square from, Square to, MoveType type = NORMAL) {
    return Move(from | (to << 6) | type);
}

// Build a promotion move.
// promo must be one of KNIGHT, BISHOP, ROOK, QUEEN.
// Stored as (promo - KNIGHT) in bits 14..15: KNIGHT=0, BISHOP=1, ROOK=2, QUEEN=3
inline Move make_promotion(Square from, Square to, PieceType promo) {
    return Move(from | (to << 6) | PROMOTION | ((promo - KNIGHT) << 14));
}

// Extract the origin square from a move
inline Square from_sq(Move m) {
    return Square(m & 0x3F);
}

// Extract the destination square from a move
inline Square to_sq(Move m) {
    return Square((m >> 6) & 0x3F);
}

// Extract the move type (NORMAL, PROMOTION, EN_PASSANT, or CASTLING)
inline MoveType move_type(Move m) {
    return MoveType(m & (3 << 12));
}

// Extract the promotion piece type (only valid when move_type == PROMOTION)
inline PieceType promotion_type(Move m) {
    return PieceType(((m >> 14) & 3) + KNIGHT);
}

// =============================================================================
// CASTLING RIGHTS
// =============================================================================
// Represented as a 4-bit mask. Each bit encodes one castling right.

enum CastlingRights : uint8_t {
    NO_CASTLING     = 0,
    WHITE_KINGSIDE  = 1,   // bit 0: White O-O
    WHITE_QUEENSIDE = 2,   // bit 1: White O-O-O
    BLACK_KINGSIDE  = 4,   // bit 2: Black O-O
    BLACK_QUEENSIDE = 8,   // bit 3: Black O-O-O
    ALL_CASTLING    = 15
};

// =============================================================================
// SCORE
// =============================================================================

using Score = int;

constexpr Score SCORE_INFINITE = 30000;   // Used as alpha/beta bounds in search
constexpr Score SCORE_NONE     = 32001;   // Uninitialized / invalid score
constexpr Score SCORE_MATE     = 29000;   // Base mate score (adjusted by ply)
constexpr Score SCORE_DRAW     = 0;       // Draw score

// Returns true if the score represents a forced mate (in either direction)
inline bool is_mate_score(Score s) {
    return std::abs(s) >= SCORE_MATE - 512;
}

// =============================================================================
// CONSTANTS
// =============================================================================

constexpr int MAX_MOVES = 256;   // Safe upper bound on moves in any position
constexpr int MAX_PLY   = 128;   // Maximum search depth

// =============================================================================
// SQUARE TO STRING
// =============================================================================

// Convert a square to its algebraic name (e.g. E4 -> "e4", NO_SQUARE -> "-")
inline std::string square_to_string(Square s) {
    if (s == NO_SQUARE) return "-";
    std::string r;
    r += char('a' + file_of(s));
    r += char('1' + rank_of(s));
    return r;
}
