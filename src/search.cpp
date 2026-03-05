// =============================================================================
// search.cpp — Negamax alpha-beta search with iterative deepening
// =============================================================================

#include "search.h"
#include <iostream>
#include <algorithm>

// Global instance
Search Searcher;

// =============================================================================
// MOVE ORDERING
// =============================================================================
// Alpha-beta efficiency depends heavily on searching good moves first.
// We assign a score to each move and sort by descending score before searching.
//
// Ordering tiers (highest to lowest):
//   1. TT move      — best move from a previous search at this position
//   2. Captures     — ordered by MVV-LVA (Most Valuable Victim, Least Valuable Attacker)
//   3. Promotions   — treated similarly to captures
//   4. Quiet moves  — no special ordering in 1.0

// MVV-LVA table indexed by [attacker piece type][victim piece type].
// Higher value = search this capture first.
// Example: PxQ (pawn captures queen) scores 505, QxP scores 101.
static const int MVV_LVA[7][7] = {
    // victim:    x    P    N    B    R    Q    K
    {  0,   0,   0,   0,   0,   0,   0  },  // attacker: none
    {  0, 105, 205, 305, 405, 505, 605  },  // attacker: P
    {  0, 104, 204, 304, 404, 504, 604  },  // attacker: N
    {  0, 103, 203, 303, 403, 503, 603  },  // attacker: B
    {  0, 102, 202, 302, 402, 502, 602  },  // attacker: R
    {  0, 101, 201, 301, 401, 501, 601  },  // attacker: Q
    {  0, 100, 200, 300, 400, 500, 600  },  // attacker: K
};

constexpr int ORDER_TT_MOVE = 1000000;  // Always search TT move first
constexpr int ORDER_CAPTURE = 100000;   // Base score for captures (+ MVV-LVA)
constexpr int ORDER_QUIET   = 0;        // Quiet moves: no bonus in 1.0

int Search::move_score(const Board& board, Move m, Move tt_move) const {
    if (m == tt_move) return ORDER_TT_MOVE;

    Square to     = to_sq(m);
    Piece  victim = board.piece_at(to);

    if (victim != NO_PIECE || move_type(m) == EN_PASSANT) {
        Piece attacker = board.piece_at(from_sq(m));
        int   att_type = type_of(attacker);
        int   vic_type = (move_type(m) == EN_PASSANT) ? PAWN : type_of(victim);
        return ORDER_CAPTURE + MVV_LVA[att_type][vic_type];
    }

    if (move_type(m) == PROMOTION)
        return ORDER_CAPTURE + 50;

    return ORDER_QUIET;
}

void Search::sort_moves(const Board& board, MoveList& moves,
                         int start, Move tt_move) const {
    // Selection sort: find the highest-scored move and swap it to position i.
    // O(n²) but fast enough for move lists of ~30-50 moves per node.
    for (int i = start; i < moves.count; i++) {
        int best_idx   = i;
        int best_score = move_score(board, moves.moves[i], tt_move);

        for (int j = i + 1; j < moves.count; j++) {
            int s = move_score(board, moves.moves[j], tt_move);
            if (s > best_score) {
                best_score = s;
                best_idx   = j;
            }
        }
        std::swap(moves.moves[i], moves.moves[best_idx]);
    }
}

// =============================================================================
// QUIESCENCE SEARCH
// =============================================================================
// Called at depth=0 instead of returning evaluate() directly.
// Searches captures until a "quiet" position is reached, preventing the
// horizon effect: the engine thinking it gained material without seeing the
// immediate recapture.
//
// Uses "stand pat": if the static eval already exceeds beta, we assume the
// side to move can maintain this score and prune immediately.

Score Search::quiescence(Board& board, Score alpha, Score beta, int ply) {
    stats_.qnodes++;

    // Time check (piggybacking on the negamax node counter to save overhead)
    if ((stats_.nodes & 2047) == 0 && TM.should_stop())
        return 0;

    // Stand pat: assume we can always do at least as well as the static eval
    Score stand_pat = evaluate(board);
    if (stand_pat >= beta) return beta;
    if (stand_pat > alpha) alpha = stand_pat;

    // Hard limit to prevent stack overflow in extreme positions
    if (ply >= MAX_PLY - 1) return stand_pat;

    MoveList captures;
    generate_captures(board, captures);
    sort_moves(board, captures, 0, MOVE_NONE);

    for (int i = 0; i < captures.count; i++) {
        Move m = captures.moves[i];
        if (!board.is_legal(m)) continue;

        board.make_move(m);
        Score score = -quiescence(board, -beta, -alpha, ply + 1);
        board.unmake_move(m);

        if (TM.should_stop()) return 0;

        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }

    return alpha;
}

// =============================================================================
// NEGAMAX ALPHA-BETA
// =============================================================================
// Negamax with alpha-beta pruning. Returns the score from the perspective
// of the side to move (positive = good for the mover).
//
// Alpha = best score the current player is guaranteed so far
// Beta  = best score the opponent is guaranteed so far
// If score >= beta, the opponent won't allow this position → prune.

Score Search::negamax(Board& board, Score alpha, Score beta,
                       int depth, int ply) {
    stats_.nodes++;

    // Time check — every 2048 nodes to minimize overhead
    if ((stats_.nodes & 2047) == 0 && TM.should_stop())
        return 0;

    // Draw detection: repetition or 50-move rule.
    // Skip at ply=0 so the root always returns a real move.
    if (ply > 0 && (board.is_repetition() || board.half_move_clock >= 100))
        return SCORE_DRAW;

    // At depth 0, resolve captures before evaluating
    if (depth <= 0)
        return quiescence(board, alpha, beta, ply);

    // Hard depth limit to prevent stack overflow
    if (ply >= MAX_PLY - 1)
        return evaluate(board);

    // -------------------------------------------------------------------------
    // TRANSPOSITION TABLE PROBE
    // -------------------------------------------------------------------------
    Move    tt_move  = MOVE_NONE;
    TTEntry tt_entry;

    if (TT.probe(board.hash, tt_entry)) {
        stats_.tt_hits++;
        tt_move = tt_entry.move;

        // Use the stored score if it was searched at least as deep and
        // the bound type is compatible with our current search window
        if (tt_entry.depth >= depth) {
            Score tt_score = score_from_tt(tt_entry.score, ply);

            if (tt_entry.bound == BOUND_EXACT)                         return tt_score;
            if (tt_entry.bound == BOUND_LOWER && tt_score >= beta)     return tt_score;
            if (tt_entry.bound == BOUND_UPPER && tt_score <= alpha)    return tt_score;
        }
    }

    // -------------------------------------------------------------------------
    // MOVE GENERATION AND SEARCH
    // -------------------------------------------------------------------------
    MoveList moves;
    generate_all_moves(board, moves);
    sort_moves(board, moves, 0, tt_move);

    int   legal_count  = 0;
    Move  best_move    = MOVE_NONE;
    Score best_score   = -SCORE_INFINITE;
    Score orig_alpha   = alpha;

    for (int i = 0; i < moves.count; i++) {
        Move m = moves.moves[i];
        if (!board.is_legal(m)) continue;
        legal_count++;

        board.make_move(m);
        Score score = -negamax(board, -beta, -alpha, depth - 1, ply + 1);
        board.unmake_move(m);

        if (TM.should_stop()) return 0;

        if (score > best_score) {
            best_score = score;
            best_move  = m;

            if (score > alpha) {
                alpha = score;
                if (alpha >= beta) {
                    // Beta cutoff: opponent won't allow this position.
                    // Store as lower bound (the real score may be even higher).
                    TT.store(board.hash, best_move, best_score,
                             depth, BOUND_LOWER, ply);
                    return alpha;
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // CHECKMATE AND STALEMATE
    // -------------------------------------------------------------------------
    if (legal_count == 0) {
        // No legal moves: either checkmate or stalemate
        return board.in_check()
            ? -SCORE_MATE + ply   // Checkmate: prefer faster mates (lower ply)
            : SCORE_DRAW;         // Stalemate
    }

    // -------------------------------------------------------------------------
    // STORE IN TRANSPOSITION TABLE
    // -------------------------------------------------------------------------
    // EXACT if we improved alpha; UPPER if no move improved alpha (all failed low)
    BoundType bound = (best_score <= orig_alpha) ? BOUND_UPPER : BOUND_EXACT;
    TT.store(board.hash, best_move, best_score, depth, bound, ply);

    return best_score;
}

// =============================================================================
// ITERATIVE DEEPENING — TOP LEVEL
// =============================================================================
// Search at depth 1, 2, 3, ... until time runs out.
// Each iteration uses results from the previous one (via the TT) to search
// more efficiently. If time expires mid-search, we return the last
// fully completed iteration's result.

SearchResult Search::go(Board& board) {
    stats_ = SearchStats{};
    TM.start(board.side_to_move);

    SearchResult result;
    Score        last_score = 0;
    Move         last_move  = MOVE_NONE;
    int          last_depth = 0;

    int max_depth = (TM.depth_limit > 0) ? TM.depth_limit : MAX_PLY;

    for (int depth = 1; depth <= max_depth; depth++) {
        Score score = negamax(board, -SCORE_INFINITE, SCORE_INFINITE, depth, 0);

        // Discard incomplete results (time ran out mid-search)
        if (TM.should_stop() && depth > 1) break;

        last_score = score;
        last_move  = TT.probe_move(board.hash);
        last_depth = depth;

        int elapsed = TM.elapsed_ms();
        int nps     = (elapsed > 0) ? int(stats_.nodes * 1000 / elapsed) : 0;

        // UCI info line
        std::cout << "info depth " << depth;

        if (is_mate_score(last_score)) {
            int moves_to_mate = (SCORE_MATE - std::abs(last_score) + 1) / 2;
            std::cout << " score mate "
                      << (last_score > 0 ? moves_to_mate : -moves_to_mate);
        } else {
            std::cout << " score cp " << last_score;
        }

        std::cout << " nodes "    << stats_.nodes
                  << " nps "      << nps
                  << " time "     << elapsed
                  << " hashfull " << TT.hashfull();

        if (last_move != MOVE_NONE) {
            std::cout << " pv ";
            Square from = from_sq(last_move);
            Square to   = to_sq(last_move);
            std::cout << char('a' + file_of(from))
                      << char('1' + rank_of(from))
                      << char('a' + file_of(to))
                      << char('1' + rank_of(to));
            if (move_type(last_move) == PROMOTION) {
                const char promo[] = "nbrq";
                std::cout << promo[promotion_type(last_move) - KNIGHT];
            }
        }
        std::cout << "\n" << std::flush;

        if (TM.should_stop()) break;
    }

    result.best_move = last_move;
    result.score     = last_score;
    result.depth     = last_depth;

    return result;
}
