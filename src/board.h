// =============================================================================
// board.h — Chess board state representation
//
// The Board struct holds the complete state of a chess position and provides
// methods to make/unmake moves, parse FEN strings, and query the position.
//
// We use two redundant representations kept in sync at all times:
//   1. Bitboards: fast for bulk operations (attacks, move generation)
//   2. Piece array: fast for "what piece is on square X?" queries
// =============================================================================

#pragma once

#include "types.h"
#include "bitboard.h"
#include <string>

// =============================================================================
// POSITION HISTORY ENTRY
// =============================================================================
// Before making a move, we save the parts of the position that CANNOT be
// recovered by unmaking the move (captured piece, castling rights, etc.).
// Everything else (piece positions) is restored by reversing the move.

struct StateInfo {
    // Captured piece (NO_PIECE if not a capture)
    Piece    captured_piece;

    // En passant target square: if the last move was a double pawn push,
    // this is the square the pawn "passed through" (where it could be captured)
    Square   ep_square;

    // Castling rights BEFORE this move was made (4-bit mask)
    uint8_t  castling_rights;

    // Half-move clock: counts half-moves since last capture or pawn move.
    // Used for the 50-move draw rule.
    int      half_move_clock;

    // Zobrist hash of the position BEFORE this move (for TT and repetition)
    uint64_t hash;
};

// Maximum number of moves we can store in the history stack.
// 1024 is more than enough for any real game.
constexpr int MAX_GAME_HISTORY = 1024;

// =============================================================================
// BOARD
// =============================================================================

struct Board {

    // -------------------------------------------------------------------------
    // POSITION DATA
    // -------------------------------------------------------------------------

    // Bitboards indexed by piece type: pieces[PAWN] has all pawns of both colors.
    // pieces[0] is unused (NO_PIECE_TYPE = 0).
    Bitboard pieces[7];

    // Bitboards indexed by color: by_color[WHITE] = all white pieces
    Bitboard by_color[2];

    // piece_on[square] = what piece is on that square (NO_PIECE if empty)
    Piece    piece_on[64];

    // Side to move
    Color    side_to_move;

    // Current castling rights (4-bit mask, see CastlingRights in types.h)
    uint8_t  castling_rights;

    // En passant square (NO_SQUARE if not available)
    Square   ep_square;

    // Half-move clock (for 50-move rule)
    int      half_move_clock;

    // Full move number (starts at 1, incremented after Black's move)
    int      full_move_number;

    // Zobrist hash of the current position
    uint64_t hash;

    // -------------------------------------------------------------------------
    // HISTORY STACK
    // -------------------------------------------------------------------------
    // We store one StateInfo per move made. When we unmake a move, we pop it.

    StateInfo history[MAX_GAME_HISTORY];
    int       history_ply;  // Current depth in the history stack
    int       game_ply;     // Total half-moves played from the starting position

    // -------------------------------------------------------------------------
    // CONSTRUCTORS / SETUP
    // -------------------------------------------------------------------------

    // Initialize an empty board (all zeros, no pieces)
    Board();

    // Set up the board from a FEN string.
    // FEN (Forsyth-Edwards Notation) is the standard way to describe a position.
    // Example starting position FEN:
    //   "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
    void set_fen(const std::string& fen);

    // Return the FEN string for the current position
    std::string get_fen() const;

    // Set up the standard starting position
    void set_startpos();

    // -------------------------------------------------------------------------
    // PIECE QUERIES
    // -------------------------------------------------------------------------

    // Get the bitboard of all pieces of a given type (both colors combined)
    inline Bitboard piece_bb(PieceType pt) const {
        return pieces[pt];
    }

    // Get the bitboard of all pieces of a given type AND color
    inline Bitboard piece_bb(Color c, PieceType pt) const {
        return pieces[pt] & by_color[c];
    }

    // Get the bitboard of ALL occupied squares
    inline Bitboard occupancy() const {
        return by_color[WHITE] | by_color[BLACK];
    }

    // Get the bitboard of all pieces of a given color
    inline Bitboard all_pieces(Color c) const {
        return by_color[c];
    }

    // Get the square of the king of a given color
    inline Square king_square(Color c) const {
        return lsb(piece_bb(c, KING));
    }

    // Get the piece on a given square
    inline Piece piece_at(Square s) const {
        return piece_on[s];
    }

    // Returns true if the given square is empty
    inline bool is_empty(Square s) const {
        return piece_on[s] == NO_PIECE;
    }

    // -------------------------------------------------------------------------
    // CHECK AND ATTACK QUERIES
    // -------------------------------------------------------------------------

    // Return a bitboard of all squares attacked by the given color
    Bitboard attacked_by(Color c) const;

    // Return a bitboard of all pieces of color 'c' that attack square 's'
    Bitboard attackers_to(Square s, Color c) const;

    // Return true if the side to move's king is currently in check
    bool in_check() const;

    // Return true if square 's' is attacked by any piece of color 'c'
    bool is_attacked(Square s, Color c) const;

    // -------------------------------------------------------------------------
    // MAKE / UNMAKE MOVES
    // -------------------------------------------------------------------------

    // Apply a move to the board. Saves irreversible state to the history stack.
    void make_move(Move m);

    // Undo the last move. Restores state from the history stack.
    void unmake_move(Move m);

    // Make a "null move": pass the turn without moving any piece.
    // Used in null move pruning during search.
    void make_null_move();
    void unmake_null_move();

    // -------------------------------------------------------------------------
    // MOVE LEGALITY
    // -------------------------------------------------------------------------

    // Returns true if the move is legal (does not leave our king in check)
    bool is_legal(Move m) const;

    // Returns true if the current position has occurred before in the game
    // history. Used to detect draws by threefold repetition.
    bool is_repetition() const;

    // -------------------------------------------------------------------------
    // DEBUG
    // -------------------------------------------------------------------------

    // Print the board to stdout in a human-readable format
    void print() const;
};

// =============================================================================
// ZOBRIST HASHING
// =============================================================================
// Zobrist hashing assigns a unique random 64-bit number to each (piece, square)
// combination. The hash of a position is the XOR of all active numbers.
// Updating the hash after a move requires only a few XOR operations — very fast.
// The hash is used by the transposition table and for repetition detection.

namespace Zobrist {
    // Random number for each piece on each square: [piece][square]
    extern uint64_t piece_square[15][64];

    // Random number for each possible castling rights combination (0..15)
    extern uint64_t castling[16];

    // Random number for each en passant file (0=a .. 7=h)
    extern uint64_t en_passant[8];

    // XOR this into the hash whenever it is Black's turn to move
    extern uint64_t side_to_move;

    // Initialize all Zobrist tables with pseudorandom values.
    // Must be called once at startup after init_bitboards().
    void init();
}
