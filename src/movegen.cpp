// =============================================================================
// movegen.cpp — Pseudo-legal move generation
//
// Generates moves piece by piece in this order:
//   1. Pawns  (most complex: pushes, double pushes, promotions, en passant)
//   2. Knights
//   3. Bishops
//   4. Rooks
//   5. Queens
//   6. King (including castling)
//
// All generated moves are pseudo-legal: they respect piece movement rules but
// may leave the king in check. The caller must verify legality with
// board.is_legal() before making each move.
// =============================================================================

#include "movegen.h"

// =============================================================================
// INTERNAL HELPERS
// =============================================================================

// Add moves from 'from' to every set square in 'targets' to the move list.
static inline void add_moves(MoveList& moves, Square from, Bitboard targets) {
    while (targets)
        moves.add(make_move(from, pop_lsb(targets)));
}

// Add all four promotion moves for a pawn going from 'from' to 'to'.
// We always generate all four options — the search will prefer the queen
// but underpromotions can occasionally be the best move.
static inline void add_promotions(MoveList& moves, Square from, Square to) {
    moves.add(make_promotion(from, to, QUEEN));   // Most common
    moves.add(make_promotion(from, to, ROOK));
    moves.add(make_promotion(from, to, BISHOP));
    moves.add(make_promotion(from, to, KNIGHT));  // Underpromotion
}

// =============================================================================
// PAWN MOVE GENERATION
// =============================================================================
// Pawns are the most complex piece:
//   - Direction depends on color (White moves north, Black moves south)
//   - Move straight, capture diagonally
//   - Can push two squares from the starting rank
//   - Must promote when reaching the last rank
//   - Can capture en passant

static void generate_pawn_moves(const Board& board, MoveList& moves,
                                  bool captures_only) {
    Color    us      = board.side_to_move;
    Color    them    = ~us;
    Bitboard pawns   = board.piece_bb(us, PAWN);
    Bitboard occ     = board.occupancy();
    Bitboard enemies = board.all_pieces(them);

    // Split pawns into those about to promote and the rest
    Bitboard promo_pawns  = pawns &  (us == WHITE ? RANK_7_BB : RANK_2_BB);
    Bitboard normal_pawns = pawns & ~(us == WHITE ? RANK_7_BB : RANK_2_BB);

    // -------------------------------------------------------------------------
    // QUIET PAWN MOVES (skipped in captures-only mode)
    // -------------------------------------------------------------------------
    if (!captures_only) {

        // Single push: one square forward into an empty square
        Bitboard single_push = (us == WHITE)
            ? shift_north(normal_pawns) & ~occ
            : shift_south(normal_pawns) & ~occ;

        // Double push: from the starting rank only; the intermediate square
        // must also be empty (already guaranteed by single_push being empty)
        Bitboard double_push = (us == WHITE)
            ? shift_north(single_push & RANK_3_BB) & ~occ
            : shift_south(single_push & RANK_6_BB) & ~occ;

        Bitboard sp = single_push;
        while (sp) {
            Square to   = pop_lsb(sp);
            Square from = (us == WHITE) ? Square(to - 8) : Square(to + 8);
            moves.add(make_move(from, to));
        }

        Bitboard dp = double_push;
        while (dp) {
            Square to   = pop_lsb(dp);
            Square from = (us == WHITE) ? Square(to - 16) : Square(to + 16);
            moves.add(make_move(from, to));
        }

        // Quiet promotion: pawn on 7th/2nd rank pushes to the last rank
        Bitboard promo_push = (us == WHITE)
            ? shift_north(promo_pawns) & ~occ
            : shift_south(promo_pawns) & ~occ;

        while (promo_push) {
            Square to   = pop_lsb(promo_push);
            Square from = (us == WHITE) ? Square(to - 8) : Square(to + 8);
            add_promotions(moves, from, to);
        }
    }

    // -------------------------------------------------------------------------
    // PAWN CAPTURES
    // -------------------------------------------------------------------------

    // Normal captures (non-promoting pawns)
    Bitboard np = normal_pawns;
    while (np) {
        Square   from = pop_lsb(np);
        Bitboard atk  = pawn_attack(us, from) & enemies;
        add_moves(moves, from, atk);
    }

    // Promotion captures (pawns on the 7th/2nd rank capturing diagonally)
    Bitboard pp = promo_pawns;
    while (pp) {
        Square   from = pop_lsb(pp);
        Bitboard atk  = pawn_attack(us, from) & enemies;
        while (atk) {
            Square to = pop_lsb(atk);
            add_promotions(moves, from, to);
        }
    }

    // -------------------------------------------------------------------------
    // EN PASSANT
    // -------------------------------------------------------------------------
    if (board.ep_square != NO_SQUARE) {
        // Find our pawns that can reach the en passant target square.
        // We use the opponent's pawn attack pattern from ep_square to find them.
        Bitboard ep_attackers = pawn_attack(them, board.ep_square) & pawns;
        while (ep_attackers) {
            Square from = pop_lsb(ep_attackers);
            moves.add(make_move(from, board.ep_square, EN_PASSANT));
        }
    }
}

// =============================================================================
// KNIGHT MOVE GENERATION
// =============================================================================

static void generate_knight_moves(const Board& board, MoveList& moves,
                                    Bitboard target_mask) {
    Bitboard knights = board.piece_bb(board.side_to_move, KNIGHT);
    while (knights) {
        Square   from    = pop_lsb(knights);
        Bitboard targets = knight_attack(from) & target_mask;
        add_moves(moves, from, targets);
    }
}

// =============================================================================
// BISHOP MOVE GENERATION
// =============================================================================

static void generate_bishop_moves(const Board& board, MoveList& moves,
                                    Bitboard target_mask) {
    Bitboard bishops = board.piece_bb(board.side_to_move, BISHOP);
    Bitboard occ     = board.occupancy();
    while (bishops) {
        Square   from    = pop_lsb(bishops);
        Bitboard targets = bishop_attack(from, occ) & target_mask;
        add_moves(moves, from, targets);
    }
}

// =============================================================================
// ROOK MOVE GENERATION
// =============================================================================

static void generate_rook_moves(const Board& board, MoveList& moves,
                                  Bitboard target_mask) {
    Bitboard rooks = board.piece_bb(board.side_to_move, ROOK);
    Bitboard occ   = board.occupancy();
    while (rooks) {
        Square   from    = pop_lsb(rooks);
        Bitboard targets = rook_attack(from, occ) & target_mask;
        add_moves(moves, from, targets);
    }
}

// =============================================================================
// QUEEN MOVE GENERATION
// =============================================================================

static void generate_queen_moves(const Board& board, MoveList& moves,
                                   Bitboard target_mask) {
    Bitboard queens = board.piece_bb(board.side_to_move, QUEEN);
    Bitboard occ    = board.occupancy();
    while (queens) {
        Square   from    = pop_lsb(queens);
        Bitboard targets = queen_attack(from, occ) & target_mask;
        add_moves(moves, from, targets);
    }
}

// =============================================================================
// KING MOVE GENERATION (including castling)
// =============================================================================

static void generate_king_moves(const Board& board, MoveList& moves,
                                  Bitboard target_mask, bool captures_only) {
    Color  us   = board.side_to_move;
    Square from = board.king_square(us);

    // Normal one-step king moves
    add_moves(moves, from, king_attack(from) & target_mask);

    // -------------------------------------------------------------------------
    // CASTLING
    // Requirements:
    //   1. Castling right for this side/direction is still available
    //   2. All squares between king and rook are empty
    //   3. King is not currently in check
    //   4. King does not cross or land on a square attacked by the opponent
    // -------------------------------------------------------------------------
    if (captures_only)    return;  // Castling is a quiet move
    if (board.in_check()) return;  // Cannot castle out of check

    Color them = ~us;

    if (us == WHITE) {
        if ((board.castling_rights & WHITE_KINGSIDE) &&
            board.is_empty(F1) && board.is_empty(G1) &&
            !board.is_attacked(F1, them) && !board.is_attacked(G1, them))
            moves.add(make_move(E1, G1, CASTLING));

        if ((board.castling_rights & WHITE_QUEENSIDE) &&
            board.is_empty(D1) && board.is_empty(C1) && board.is_empty(B1) &&
            !board.is_attacked(D1, them) && !board.is_attacked(C1, them))
            moves.add(make_move(E1, C1, CASTLING));
    } else {
        if ((board.castling_rights & BLACK_KINGSIDE) &&
            board.is_empty(F8) && board.is_empty(G8) &&
            !board.is_attacked(F8, them) && !board.is_attacked(G8, them))
            moves.add(make_move(E8, G8, CASTLING));

        if ((board.castling_rights & BLACK_QUEENSIDE) &&
            board.is_empty(D8) && board.is_empty(C8) && board.is_empty(B8) &&
            !board.is_attacked(D8, them) && !board.is_attacked(C8, them))
            moves.add(make_move(E8, C8, CASTLING));
    }
}

// =============================================================================
// PUBLIC INTERFACE
// =============================================================================

void generate_all_moves(const Board& board, MoveList& moves) {
    Color    us          = board.side_to_move;
    // Can move to any square not occupied by our own pieces
    Bitboard target_mask = ~board.all_pieces(us);

    generate_pawn_moves  (board, moves, /*captures_only=*/false);
    generate_knight_moves(board, moves, target_mask);
    generate_bishop_moves(board, moves, target_mask);
    generate_rook_moves  (board, moves, target_mask);
    generate_queen_moves (board, moves, target_mask);
    generate_king_moves  (board, moves, target_mask, /*captures_only=*/false);
}

void generate_captures(const Board& board, MoveList& moves) {
    Color    us          = board.side_to_move;
    // Can only move to squares occupied by the opponent
    Bitboard target_mask = board.all_pieces(~us);

    generate_pawn_moves  (board, moves, /*captures_only=*/true);
    generate_knight_moves(board, moves, target_mask);
    generate_bishop_moves(board, moves, target_mask);
    generate_rook_moves  (board, moves, target_mask);
    generate_queen_moves (board, moves, target_mask);
    generate_king_moves  (board, moves, target_mask, /*captures_only=*/true);
}
