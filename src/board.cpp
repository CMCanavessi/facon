// =============================================================================
// board.cpp — Board state implementation: FEN parsing, make/unmake, queries
// =============================================================================

#include "board.h"
#include <sstream>
#include <iostream>
#include <iomanip>
#include <random>
#include <cctype>    // tolower() in set_fen()
#include <cmath>     // std::abs() in make_move()

// =============================================================================
// ZOBRIST TABLES — definitions (declared extern in board.h)
// =============================================================================

namespace Zobrist {
    uint64_t piece_square[15][64];
    uint64_t castling[16];
    uint64_t en_passant[8];
    uint64_t side_to_move;

    void init() {
        // Fixed seed: guarantees the same hash values every run.
        // This is important for reproducibility and debugging.
        std::mt19937_64 rng(0xFA60E8CE55E0A000ULL);

        for (int p = 0; p < 15; p++)
            for (int s = 0; s < 64; s++)
                piece_square[p][s] = rng();

        for (int c = 0; c < 16; c++)
            castling[c] = rng();

        for (int f = 0; f < 8; f++)
            en_passant[f] = rng();

        side_to_move = rng();
    }
}

// =============================================================================
// INTERNAL HELPERS
// =============================================================================
// These three functions keep the bitboards, piece array, and Zobrist hash
// in sync at all times. Every piece movement goes through one of these.

// Place a piece on a square (square must be empty)
static inline void put_piece(Board& b, Piece p, Square s) {
    b.piece_on[s]            = p;
    b.pieces[type_of(p)]    |= square_bb(s);
    b.by_color[color_of(p)] |= square_bb(s);
    b.hash ^= Zobrist::piece_square[p][s];
}

// Remove the piece on a square (square must be occupied)
static inline void remove_piece(Board& b, Square s) {
    Piece p = b.piece_on[s];
    b.piece_on[s]             = NO_PIECE;
    b.pieces[type_of(p)]    &= ~square_bb(s);
    b.by_color[color_of(p)] &= ~square_bb(s);
    b.hash ^= Zobrist::piece_square[p][s];
}

// Move a piece from one square to another (destination must be empty)
static inline void move_piece(Board& b, Square from, Square to) {
    Piece    p    = b.piece_on[from];
    Bitboard mask = square_bb(from) | square_bb(to);
    b.piece_on[from]          = NO_PIECE;
    b.piece_on[to]            = p;
    b.pieces[type_of(p)]    ^= mask;
    b.by_color[color_of(p)] ^= mask;
    b.hash ^= Zobrist::piece_square[p][from];
    b.hash ^= Zobrist::piece_square[p][to];
}

// =============================================================================
// BOARD CONSTRUCTOR
// =============================================================================

Board::Board() {
    for (int i = 0; i < 7;  i++) pieces[i]   = 0;
    for (int i = 0; i < 2;  i++) by_color[i] = 0;
    for (int i = 0; i < 64; i++) piece_on[i]  = NO_PIECE;
    side_to_move     = WHITE;
    castling_rights  = NO_CASTLING;
    ep_square        = NO_SQUARE;
    half_move_clock  = 0;
    full_move_number = 1;
    hash             = 0;
    history_ply      = 0;
    game_ply         = 0;
}

// =============================================================================
// FEN PARSING
// =============================================================================
// FEN format: "<pieces> <side> <castling> <ep> <halfmove> <fullmove>"
// Example:    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
//
// Piece placement: ranks from 8 down to 1, separated by '/'.
//   Uppercase letters = White pieces, lowercase = Black pieces.
//   Digits = consecutive empty squares.

void Board::set_fen(const std::string& fen) {
    *this = Board();  // Reset to empty board first

    std::istringstream ss(fen);
    std::string token;

    // --- 1. Piece placement ---
    ss >> token;
    int rank = 7, file = 0;  // Start at a8 (rank 8, file a)
    for (char c : token) {
        if (c == '/') {
            rank--;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += (c - '0');  // Skip N empty squares
        } else {
            Color     col = (c >= 'a') ? BLACK : WHITE;
            PieceType pt;
            switch (tolower(c)) {
                case 'p': pt = PAWN;   break;
                case 'n': pt = KNIGHT; break;
                case 'b': pt = BISHOP; break;
                case 'r': pt = ROOK;   break;
                case 'q': pt = QUEEN;  break;
                case 'k': pt = KING;   break;
                default:  pt = NO_PIECE_TYPE; break;
            }
            if (pt != NO_PIECE_TYPE)
                put_piece(*this, make_piece(col, pt), make_square(file, rank));
            file++;
        }
    }

    // --- 2. Side to move ---
    ss >> token;
    side_to_move = (token == "w") ? WHITE : BLACK;
    if (side_to_move == BLACK)
        hash ^= Zobrist::side_to_move;

    // --- 3. Castling rights ---
    ss >> token;
    castling_rights = NO_CASTLING;
    for (char c : token) {
        switch (c) {
            case 'K': castling_rights |= WHITE_KINGSIDE;  break;
            case 'Q': castling_rights |= WHITE_QUEENSIDE; break;
            case 'k': castling_rights |= BLACK_KINGSIDE;  break;
            case 'q': castling_rights |= BLACK_QUEENSIDE; break;
            default: break;
        }
    }
    hash ^= Zobrist::castling[castling_rights];

    // --- 4. En passant square ---
    ss >> token;
    if (token != "-") {
        int ep_file = token[0] - 'a';
        int ep_rank = token[1] - '1';
        ep_square = make_square(ep_file, ep_rank);
        hash ^= Zobrist::en_passant[ep_file];
    } else {
        ep_square = NO_SQUARE;
    }

    // --- 5. Half-move clock and full move number ---
    ss >> half_move_clock >> full_move_number;
}

// =============================================================================
// FEN OUTPUT
// =============================================================================

std::string Board::get_fen() const {
    std::string fen;

    // --- 1. Piece placement ---
    // Index into this string using the Piece enum value (0..14)
    static const char piece_chars[] = ".PNBRQK..pnbrqk";
    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            Square s = make_square(file, rank);
            Piece  p = piece_on[s];
            if (p == NO_PIECE) {
                empty++;
            } else {
                if (empty) { fen += char('0' + empty); empty = 0; }
                fen += piece_chars[p];
            }
        }
        if (empty) fen += char('0' + empty);
        if (rank > 0) fen += '/';
    }

    // --- 2. Side to move ---
    fen += (side_to_move == WHITE) ? " w " : " b ";

    // --- 3. Castling rights ---
    if (castling_rights == NO_CASTLING) {
        fen += '-';
    } else {
        if (castling_rights & WHITE_KINGSIDE)  fen += 'K';
        if (castling_rights & WHITE_QUEENSIDE) fen += 'Q';
        if (castling_rights & BLACK_KINGSIDE)  fen += 'k';
        if (castling_rights & BLACK_QUEENSIDE) fen += 'q';
    }

    // --- 4. En passant square ---
    fen += ' ';
    fen += (ep_square != NO_SQUARE) ? square_to_string(ep_square) : "-";

    // --- 5. Move clocks ---
    fen += ' ';
    fen += std::to_string(half_move_clock);
    fen += ' ';
    fen += std::to_string(full_move_number);

    return fen;
}

void Board::set_startpos() {
    set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

// =============================================================================
// ATTACK QUERIES
// =============================================================================

// Returns true if square 's' is attacked by any piece of color 'c'.
// Uses reverse attack logic: pretend there is a piece of each type on 's'
// and check if it attacks any piece of that type belonging to 'c'.
bool Board::is_attacked(Square s, Color c) const {
    Bitboard occ = occupancy();

    if (pawn_attack(~c, s)    & piece_bb(c, PAWN))   return true;
    if (knight_attack(s)      & piece_bb(c, KNIGHT)) return true;
    if (king_attack(s)        & piece_bb(c, KING))   return true;
    if (bishop_attack(s, occ) & (piece_bb(c, BISHOP) | piece_bb(c, QUEEN))) return true;
    if (rook_attack(s, occ)   & (piece_bb(c, ROOK)   | piece_bb(c, QUEEN))) return true;
    return false;
}

// Returns a bitboard of all pieces of color 'c' that attack square 's'.
// Currently unused in search but available for future use (SEE, pin detection).
Bitboard Board::attackers_to(Square s, Color c) const {
    Bitboard occ = occupancy();
    Bitboard att = 0;

    att |= pawn_attack(~c, s)    & piece_bb(c, PAWN);
    att |= knight_attack(s)      & piece_bb(c, KNIGHT);
    att |= king_attack(s)        & piece_bb(c, KING);
    att |= bishop_attack(s, occ) & (piece_bb(c, BISHOP) | piece_bb(c, QUEEN));
    att |= rook_attack(s, occ)   & (piece_bb(c, ROOK)   | piece_bb(c, QUEEN));
    return att;
}

// Returns a bitboard of ALL squares attacked by color 'c'.
// Currently unused in search but available for future use (king safety).
Bitboard Board::attacked_by(Color c) const {
    Bitboard result = 0;
    Bitboard occ    = occupancy();

    // Pawns: batch shift is faster than a per-square loop
    result |= pawn_attacks_bb(c, piece_bb(c, PAWN));

    Bitboard knights = piece_bb(c, KNIGHT);
    while (knights) result |= knight_attack(pop_lsb(knights));

    result |= king_attack(king_square(c));

    Bitboard diag = piece_bb(c, BISHOP) | piece_bb(c, QUEEN);
    while (diag) result |= bishop_attack(pop_lsb(diag), occ);

    Bitboard straight = piece_bb(c, ROOK) | piece_bb(c, QUEEN);
    while (straight) result |= rook_attack(pop_lsb(straight), occ);

    return result;
}

// Returns true if the side to move's king is currently in check.
bool Board::in_check() const {
    return is_attacked(king_square(side_to_move), ~side_to_move);
}

// =============================================================================
// MAKE MOVE
// =============================================================================

// Castling rights update mask indexed by square.
// When a piece moves FROM or TO a castling-relevant square, we AND the current
// castling rights with this mask to revoke the affected right(s).
// Squares not involved in castling have mask 15 (0b1111 = keep all rights).
static const uint8_t CASTLING_RIGHTS_MASK[64] = {
    // a1                          e1 (white king)              h1
    uint8_t(~WHITE_QUEENSIDE), 15, 15, 15, uint8_t(~(WHITE_KINGSIDE|WHITE_QUEENSIDE)), 15, 15, uint8_t(~WHITE_KINGSIDE),
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    // a8                          e8 (black king)              h8
    uint8_t(~BLACK_QUEENSIDE), 15, 15, 15, uint8_t(~(BLACK_KINGSIDE|BLACK_QUEENSIDE)), 15, 15, uint8_t(~BLACK_KINGSIDE)
};

void Board::make_move(Move m) {
    // Save irreversible state before modifying anything
    StateInfo& st      = history[history_ply++];
    st.captured_piece  = NO_PIECE;
    st.ep_square       = ep_square;
    st.castling_rights = castling_rights;
    st.half_move_clock = half_move_clock;
    st.hash            = hash;
    game_ply++;

    Square   from = from_sq(m);
    Square   to   = to_sq(m);
    MoveType mt   = move_type(m);
    Piece    pc   = piece_on[from];
    Color    us   = side_to_move;
    Color    them = ~us;

    // Reset half-move clock on pawn moves or captures; increment otherwise
    if (type_of(pc) == PAWN || piece_on[to] != NO_PIECE)
        half_move_clock = 0;
    else
        half_move_clock++;

    // Remove the old en passant contribution from the hash before clearing it
    if (ep_square != NO_SQUARE)
        hash ^= Zobrist::en_passant[file_of(ep_square)];
    ep_square = NO_SQUARE;

    if (mt == NORMAL) {
        if (piece_on[to] != NO_PIECE) {
            st.captured_piece = piece_on[to];
            remove_piece(*this, to);
        }
        move_piece(*this, from, to);

        // A double pawn push creates an en passant target square
        if (type_of(pc) == PAWN && std::abs(int(to) - int(from)) == 16) {
            ep_square = Square((int(from) + int(to)) / 2);
            hash ^= Zobrist::en_passant[file_of(ep_square)];
        }
    }
    else if (mt == PROMOTION) {
        if (piece_on[to] != NO_PIECE) {
            st.captured_piece = piece_on[to];
            remove_piece(*this, to);
        }
        remove_piece(*this, from);
        put_piece(*this, make_piece(us, promotion_type(m)), to);
    }
    else if (mt == EN_PASSANT) {
        // The captured pawn sits one rank behind the destination square
        Square cap_sq = (us == WHITE) ? Square(to - 8) : Square(to + 8);
        st.captured_piece = piece_on[cap_sq];
        remove_piece(*this, cap_sq);
        move_piece(*this, from, to);
    }
    else if (mt == CASTLING) {
        // King: e1->g1 or e1->c1 (White), e8->g8 or e8->c8 (Black)
        // Rook: h1->f1 or a1->d1 (White), h8->f8 or a8->d8 (Black)
        bool   kingside  = (to > from);
        Square rook_from = (us == WHITE) ? (kingside ? H1 : A1) : (kingside ? H8 : A8);
        Square rook_to   = (us == WHITE) ? (kingside ? F1 : D1) : (kingside ? F8 : D8);
        move_piece(*this, from, to);
        move_piece(*this, rook_from, rook_to);
    }

    // Revoke any castling rights affected by this move
    hash ^= Zobrist::castling[castling_rights];
    castling_rights &= CASTLING_RIGHTS_MASK[from];
    castling_rights &= CASTLING_RIGHTS_MASK[to];
    hash ^= Zobrist::castling[castling_rights];

    // Flip side to move
    side_to_move = them;
    hash ^= Zobrist::side_to_move;

    // Full move number increments after Black's move
    if (side_to_move == WHITE)
        full_move_number++;
}

// =============================================================================
// UNMAKE MOVE
// =============================================================================

void Board::unmake_move(Move m) {
    StateInfo& st = history[--history_ply];
    game_ply--;

    // The side that made the move is now ~side_to_move
    side_to_move  = ~side_to_move;
    Color us      = side_to_move;
    Color them    = ~us;  // Used in EN_PASSANT case to restore captured pawn

    Square   from = from_sq(m);
    Square   to   = to_sq(m);
    MoveType mt   = move_type(m);

    // Restore all irreversible state from the history entry
    ep_square       = st.ep_square;
    castling_rights = st.castling_rights;
    half_move_clock = st.half_move_clock;
    hash            = st.hash;

    if (side_to_move == BLACK)
        full_move_number--;

    if (mt == NORMAL) {
        move_piece(*this, to, from);
        if (st.captured_piece != NO_PIECE)
            put_piece(*this, st.captured_piece, to);
    }
    else if (mt == PROMOTION) {
        remove_piece(*this, to);
        put_piece(*this, make_piece(us, PAWN), from);
        if (st.captured_piece != NO_PIECE)
            put_piece(*this, st.captured_piece, to);
    }
    else if (mt == EN_PASSANT) {
        move_piece(*this, to, from);
        Square cap_sq = (us == WHITE) ? Square(to - 8) : Square(to + 8);
        put_piece(*this, make_piece(them, PAWN), cap_sq);
    }
    else if (mt == CASTLING) {
        bool   kingside  = (to > from);
        Square rook_from = (us == WHITE) ? (kingside ? H1 : A1) : (kingside ? H8 : A8);
        Square rook_to   = (us == WHITE) ? (kingside ? F1 : D1) : (kingside ? F8 : D8);
        move_piece(*this, to, from);
        move_piece(*this, rook_to, rook_from);
    }
}

// =============================================================================
// NULL MOVE
// =============================================================================
// A null move passes the turn without moving any piece.
// Used in null move pruning during search to get a quick beta cutoff estimate.

void Board::make_null_move() {
    StateInfo& st      = history[history_ply++];
    st.ep_square       = ep_square;
    st.castling_rights = castling_rights;
    st.half_move_clock = half_move_clock;
    st.hash            = hash;
    st.captured_piece  = NO_PIECE;
    game_ply++;

    if (ep_square != NO_SQUARE) {
        hash ^= Zobrist::en_passant[file_of(ep_square)];
        ep_square = NO_SQUARE;
    }
    side_to_move = ~side_to_move;
    hash ^= Zobrist::side_to_move;
    half_move_clock++;
}

void Board::unmake_null_move() {
    StateInfo& st   = history[--history_ply];
    game_ply--;

    ep_square       = st.ep_square;
    castling_rights = st.castling_rights;
    half_move_clock = st.half_move_clock;
    hash            = st.hash;
    side_to_move    = ~side_to_move;

    if (side_to_move == BLACK)
        full_move_number--;
}

// =============================================================================
// MOVE LEGALITY
// =============================================================================
// A move is legal if:
//   1. The from-square contains a piece belonging to the side to move.
//   2. The move does not leave our own king in check.
//
// Check (1) is done first — it is O(1) and avoids the expensive board copy
// for moves that come from a stale or hash-collided TT entry. Without this
// guard, make_move() on a from-square that has NO_PIECE (or the wrong color)
// silently "moves nothing", the king stays safe, and is_legal() incorrectly
// returns true — causing illegal PV moves to be printed in long games where
// the TT is full of entries from earlier positions.

bool Board::is_legal(Move m) const {
    Square from = from_sq(m);
    Piece  p    = piece_on[from];

    // Reject immediately if from-square is empty or holds the opponent's piece.
    // This catches the most common source of ghost moves from TT collisions.
    if (p == NO_PIECE || color_of(p) != side_to_move)
        return false;

    // Full legality check: make the move on a copy and verify the king is safe.
    // After make_move(), side_to_move has flipped — our king is ~copy.side_to_move.
    Board copy = *this;
    copy.make_move(m);
    return !copy.is_attacked(copy.king_square(~copy.side_to_move), copy.side_to_move);
}

// =============================================================================
// REPETITION DETECTION
// =============================================================================
// Returns true if the current position has appeared before in the game history.
// We step back through the history 2 plies at a time (same side to move) and
// compare Zobrist hashes. We stop at the last irreversible move (capture or
// pawn push) since the position cannot repeat across such moves.

bool Board::is_repetition() const {
    for (int i = history_ply - 2;
         i >= 0 && i >= history_ply - half_move_clock;
         i -= 2)
    {
        if (history[i].hash == hash)
            return true;
    }
    return false;
}

// =============================================================================
// PRINT BOARD
// =============================================================================

void Board::print() const {
    // Indexed by Piece enum value (0..15); entries 7 and 8 are unused gaps
    static const char* piece_str[] = {
        ".", "P", "N", "B", "R", "Q", "K", ".",
        ".", "p", "n", "b", "r", "q", "k", "."
    };

    std::cout << "\n  +-----------------+\n";
    for (int rank = 7; rank >= 0; rank--) {
        std::cout << "  " << (rank + 1) << "| ";
        for (int file = 0; file < 8; file++) {
            Square s = make_square(file, rank);
            std::cout << piece_str[piece_on[s]] << " ";
        }
        std::cout << "|\n";
    }
    std::cout << "  +-----------------+\n";
    std::cout << "    a b c d e f g h\n\n";
    std::cout << "  FEN:          " << get_fen() << "\n";
    std::cout << "  Side to move: " << (side_to_move == WHITE ? "White" : "Black") << "\n";
    std::cout << "  Hash:         0x" << std::hex << std::setw(16)
              << std::setfill('0') << hash << std::dec << "\n\n";
}
