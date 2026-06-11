// =============================================================================
// Last modified: 2026-06-06 19:54
// eval.cpp -- Static position evaluation
//
// Implements evaluate(), the function that assigns a static score (in
// centipawns, from the side-to-move's perspective) to a board position
// without performing any search. Components:
//
//   - Material balance (piece values from eval.h)
//   - Piece-square tables (tapered MG/EG, defined below)
//   - King safety (tunable linear terms: per-attacker-type weights plus
//     attacker-count buckets on the king zone)
//   - Piece tropism (tunable per-piece bonus for proximity to the enemy king,
//     bucketed by Chebyshev distance)
//   - Pawn structure (isolated, doubled, backward, passed, connected)
//   - Positional terms (mobility, open files, rook on 7th, bishop pair,
//     knight outposts)
//   - Mopup term (drives toward mate in pawnless winning endings)
//
// Output convention: positive = good for the side to move, negative = bad.
// All values are in centipawns (100 = one pawn). Game phase is tracked on
// a 0..256 scale, with 256 = full middlegame material and 0 = pure endgame
// (only kings on the board); see game_phase() below.
//
// Facon 1.1 -- Herrumbre
//   - King safety: penalty for enemy pieces attacking the king zone.
//     The king zone is the king's square plus all adjacent squares (up to 9
//     squares total). Each enemy piece type has an attack weight, and the
//     penalty grows quadratically with the total weight. Scaled by game phase
//     so it only applies in the middlegame.
//
// Facon 1.2 -- Rojo Vivo
//   - Mopup evaluation: in pawnless endings with a decisive material
//     advantage, adds a bonus to the winning side for: (a) pushing the
//     losing king toward a corner (Manhattan distance from central region),
//     and (b) closing the distance between kings (14 - manhattan_distance).
//     Activated only when no pawns remain and the raw material+PST advantage
//     exceeds MOPUP_THRESHOLD. Constants defined in eval.h.
//
// Facon 1.3 -- Yunque
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
// Facon 1.4 -- Hoja
//   - Positional evaluation (positional_eval): mobility for knights, bishops,
//     rooks, queens (count of pseudo-legal squares, excluding own pieces).
//     Rook on open/semi-open files and 7th rank. Bishop pair bonus. Knight
//     outpost bonus for knights that cannot be attacked by enemy pawns.
//     All terms computed via bitboard operations, constants in eval.h.
//
// Facon 1.5 -- Espiga
//   - evaluate_verbose(): debug helper that prints a per-component breakdown
//     of evaluate() to stdout. Reproduces the exact logic of evaluate() but
//     accumulates each term separately. Called from the UCI "eval" command.
//     Not in the search hot path.
//   - game_phase() inversion fix: the function returned the opposite of its
//     documented contract -- 0 in startpos (should have been 256 = middlegame)
//     and 256 in pure endgame (should have been 0). Consumers (king_safety,
//     pst_value for the king) were written assuming the documented contract,
//     so they silently used the wrong tables: king_safety returned 0 in the
//     middlegame (zeroed by phase_mg=0), and pst_value used PST_KING_EG
//     instead of PST_KING_MG from move 1 (incentivizing centralizing the king
//     from the opening). The fix inverts the counting: phase now starts at 0
//     and adds material present, instead of starting at TOTAL_PHASE and
//     subtracting material absent. Consumers are unchanged. Bench signature
//     changes (expected: evaluation values move, alpha-beta cutoffs change,
//     node counts shift).
//   - Knight outpost forward_mask fix: the forward_mask used to disqualify
//     outposts incorrectly included the knight's own rank, causing
//     false-negatives when an enemy pawn sat on the same rank in an adjacent
//     file. Such pawns cannot attack the knight (pawns capture diagonally
//     forward, not horizontally), so they should not disqualify the outpost.
//     Fix: forward_mask is now STRICTLY ahead of the knight. Includes a guard
//     against undefined behavior (1ULL << 64) for the edge case of a white
//     knight on rank 8 (which can occur in endgames).
//   - eval command label fix: the game_phase line in the "eval" output said
//     "0 = endgame, 256 = middlegame", which was misleading because phase
//     reflects non-pawn material remaining and does not distinguish opening
//     from middlegame. Corrected to "0 = bare kings, 256 = full non-pawn
//     material" -- factually accurate.
//   - Mopup insufficient material guard extended: the guard for drawn
//     endgames now covers K+N+N vs K and K+B+B vs K (when both bishops are
//     on same-colored squares), in addition to the previously covered K+B
//     vs K and K+N vs K. Without these additions the strong side's material
//     exceeded MOPUP_THRESHOLD and corner-chasing activated in theoretically
//     drawn positions, causing the engine to refuse draws and wander.
//     K+B+B with opposite-colored bishops correctly remains a winning
//     endgame and continues to trigger mopup.
//   - Knight outpost refactor: the previous outpost detection was incomplete
//     -- it required only that the square not be attackable by enemy pawns,
//     but the documented design also called for friendly pawn support, which
//     was never implemented. The bonus was a flat 20cp regardless of how
//     anchored the knight was. Refactored to two tiers:
//     KNIGHT_OUTPOST_REACHABLE (10cp, no friendly pawn support) and
//     KNIGHT_OUTPOST_SUPPORTED (25cp, supported by a friendly pawn).
//     Following the convention used by other HCE engines.
//   - evaluate_verbose() label fix: the "skipped: |partial| below threshold"
//     message was emitted in cases where mopup_eval was actually called and
//     returned 0 due to its insufficient material guard. Added a third
//     explanatory branch: "(skipped: insufficient material in mopup)" for
//     the cases where partial DOES exceed the threshold but the position
//     is a theoretical draw (KB vs K, KN vs K, KNN vs K, KBB same color vs K).
//
// Note on drawn pawnless endings: a "fix" was attempted that overrode the
// final score to 0 when mopup_eval correctly identified a drawn material
// configuration (KB vs K, KN vs K, KNN vs K, KBB same color vs K). The
// override was logically correct (these endings are theoretically drawn)
// but caused measurable gameplay regression in extended testing: the engine
// became less motivated to reach positions that propagated such draws back
// through search, even when intermediate positions along the way were
// genuinely winning. The override was reverted. The underlying limitation
// -- evaluate() reports the raw material count for these pawnless endings
// rather than recognizing them as draws -- is acknowledged and deferred
// to a future version with proper material-signature endgame recognition.
//
// Facon 1.6 -- Temple
//   - PSTs refactored to fully tapered form. Each non-king PST (pawn,
//     knight, bishop, rook, queen) is now stored as two separate tables,
//     PST_<piece>_MG and PST_<piece>_EG, interpolated at lookup time by
//     game_phase -- the same shape PST_KING_MG / PST_KING_EG already used.
//     Initial values: each MG and EG pair holds identical values, equal to
//     the previous single table, so the static evaluation is mathematically
//     unchanged. The duplication is the refactor; positional refinement
//     (pulling MG and EG apart) is the next step.
//   - pst_value() unified: the per-piece-type switch now selects one (MG,EG)
//     pair and runs the same interpolation formula for all piece types.
//     The previous code had a fast-path returning the raw table value for
//     the five non-king types and only interpolated the king. The new path
//     is uniform; with MG == EG the interpolation is an algebraic identity,
//     so values produced are bit-identical to the previous version.
//   - Comment audit pass: comments across this file (and its companion
//     eval.h) revised for completeness and didactic clarity. PSTs now
//     document the chess intuition behind each table's shape; the mirror
//     trick in pst_value() is explained with a concrete example; the
//     double-counting of the queen in king_safety() is justified; the
//     historical note inside game_phase() is condensed into a contract
//     statement. Em dashes and other non-ASCII punctuation in comments
//     replaced with ASCII equivalents for portability. No functional
//     changes.
//   - Pawn structure refactored to tapered form. The five hand-set
//     constants (isolated, doubled, backward, connected, passed-by-rank)
//     are each split into MG and EG variants. pawn_structure() now takes
//     phase_mg as a parameter, accumulates two running totals -- one for
//     middlegame, one for endgame -- across all five terms and both sides,
//     and blends them with a single integer division at the end (the
//     "blend-of-difference" convention; see notes in pawn_structure()).
//     Initial values: every MG equals its corresponding EG, so the blend
//     returns the same value the pre-tapered code did. Bench signature
//     is preserved bit-for-bit at the starting state of 1.6.
//   - Positional evaluation refactored to tapered form. Same shape as the
//     pawn-structure refactor: the ten positional constants (mobility per
//     piece type, rook open/semi-open file and 7th rank, bishop pair, the
//     two knight-outpost tiers) are each split into MG and EG variants.
//     positional_eval() now takes phase_mg, accumulates running mg/eg
//     totals across all terms and both sides, and blends them with a single
//     integer division at the end (Option B). Initial values: every MG
//     equals its corresponding EG, so bench signature is preserved.
//     With this change the entire eval (PSTs, pawn structure, positional
//     terms) is uniformly tapered. King safety stays single-valued and
//     remains the only intentional exception, since its quadratic shape
//     makes a linear MG/EG split poorly behaved -- it will be reformulated
//     as a tunable lookup table later in 1.6.
//   - Material + PST loop refactored to "blend-of-totals" (Option B).
//     pst_value() was retired; two new helpers, pst_mg_value() and
//     pst_eg_value(), return the raw MG and EG table entries respectively
//     (no internal blend). The loop in evaluate() now accumulates mg_score
//     and eg_score directly -- both include material (phase-independent,
//     identical in MG and EG) and the corresponding raw PST term -- and a
//     single blend formula at the end produces the score. evaluate_verbose()
//     mirrors the same shape internally; its displayed "PST" line still
//     shows the same blended figure users saw before.
//     Why this matters. pawn_structure() and positional_eval() already used
//     Option B; the material+PST loop was the last component on per-piece
//     Option A. With the loop unified, the full evaluation produces a single
//     phase blend per call, which is the convention any external evaluation
//     tooling will consume.
//     Bench signature changes by a small amount. The king PST has MG != EG
//     by design -- a king on rank 1 scores +20 in MG and -50 in EG, because
//     the king should hide in the corner in the middlegame and march to the
//     centre in the endgame. With one piece having unequal MG and EG,
//     reordering the blend (per-piece in Option A vs single global blend in
//     Option B) shifts integer truncation by up to ~1 cp per position.
//     Non-king PSTs all have MG == EG in 1.6's initial state, so they
//     contribute zero to the shift. The signature change is expected and
//     is documented in the per-version notes (project rule 3.4).
//   - Tunable weights centralized into a single flat array.
//     `eval_weights[NUM_WEIGHTS]` (934 entries) holds every weight the
//     tuner can adjust. Reads in pawn_structure(), positional_eval(),
//     pst_mg_value(), and pst_eg_value() now look up `eval_weights[...]`
//     instead of the previously-named compile-time constants. All previous
//     per-weight constexpr (PAWN_ISOLATED_MG, MOBILITY_KNIGHT_MG, etc.) and
//     all per-piece PST arrays (PST_PAWN_MG[64] etc.) are removed; their
//     values are now the initial entries of `eval_weights`. Material values
//     (PIECE_VALUE), king-safety constants (KING_ATTACK_WEIGHT, SAFETY_SCALE),
//     mopup constants and game-phase tables remain as compile-time data:
//     they are out of the tuning scope by design and do not need to be
//     mutable. The change is pure storage refactor -- the evaluate() loop
//     reads from the array exactly the same numbers it previously read from
//     named constants, in the same order. Bench signature is preserved
//     bit-for-bit.
//   - Eval trace mode. trace_evaluate() walks the same code paths that
//     evaluate() does but, instead of multiplying counts by weights and
//     accumulating a score, it counts how many times each eval_weights[]
//     slot would have been added (white contributions positive, black
//     contributions negative). The result is a coefficient vector that
//     external evaluation tooling can use to reconstruct evaluate(board)
//     and, more importantly, to recompute the score under hypothetical
//     alternate weight sets without re-running the full eval machinery.
//     The phase_mg of the original blend is stored alongside the vector
//     so the consumer can reapply the exact blend the engine used.
//     Mopup (not tunable, no MG/EG split) is accumulated into a single
//     `additional_score` scalar from White's perspective, together with
//     material. King safety used to live there too but is now a tunable
//     group recorded in the coefficient vector like every other tuned term.
//
//     The contract is bit-exact:
//
//         evaluate(board) ==
//             score_from_trace(trace, eval_weights, board.side_to_move)
//
//     for every legal position. score_from_trace() applies the same blend
//     formula evaluate() uses internally, so when called with the live
//     eval_weights[] it must match the engine score exactly. A divergence
//     means trace_evaluate() has a bug -- some feature is being counted
//     wrong, or some feature contributing to the score is missing from
//     the coefficient vector entirely.
//
//     trace_evaluate() and score_from_trace() are not called from search.
//     Bench signature is preserved bit-for-bit.
//   - First Texel-tuned weight set integrated. The eval_weights[] initialiser
//     now holds values produced by Texel tuning on a labeled quiet-position
//     dataset, instead of the original hand-set values. Material was modelled
//     as a separate tunable during tuning and then folded back into the PSTs
//     (see the big comment on the eval_weights[] definition), so PIECE_VALUE
//     stays fixed at 100/320/330/500/900 while the evaluation reproduces what
//     the tuner optimised. This is the first change of the 1.6 cycle that
//     intentionally alters playing strength, so bench signature changes and
//     self-play vs the previous dev is expected to show a real Elo delta (the
//     whole point of the change).
//   - Weight-group offsets converted from enum to constexpr int (in eval.h).
//     Eliminates the C++20 -Wdeprecated-enum-enum-conversion warnings that
//     arose from summing two different enum types in index arithmetic
//     (GROUP_START + offset). Identical generated code; bench signature
//     preserved bit-for-bit. No change to this file's logic -- listed here
//     only because the per-version notes track the whole eval subsystem.
//   - King safety reformulated from a quadratic penalty into a sum of linear,
//     tunable terms, and moved into eval_weights[] (new KING_SAFETY group,
//     16 slots appended at offset 820; NUM_WEIGHTS 820 -> 836). The old
//     KING_ATTACK_WEIGHT[] table and SAFETY_SCALE quadratic are gone. The new
//     form has two families: a per-attacker-type weight (knight, bishop, rook,
//     queen) added once per attacking piece, and an attacker-count bucket
//     (1, 2, 3, 4+) added once according to how many pieces attack the zone.
//     The buckets are binary features, so the model stays linear (a hard
//     requirement for Texel tuning) while still letting the count term grow
//     non-linearly; the 4+ bucket is a structural ceiling, so the unbounded
//     penalties that sank the earlier hand-set quadratic attempt cannot occur.
//     The queen's old double-count is dropped -- it is one attacker with its
//     own tuned weight. king_safety() now returns raw MG/EG penalties that
//     evaluate() folds into its single end-of-function blend, and
//     trace_evaluate() records the king-safety counts as coefficients (no
//     longer in additional_score), so the term is tuned jointly with every
//     other weight. The evaluate() == score_from_trace() bit-exact contract
//     is preserved (verified on king-attack positions). This alters playing
//     strength and the bench signature; the weights ship at hand-set seed
//     values in this dev and are then Texel-tuned (the tuned build is what is
//     measured by self-play).
//   - Piece tropism added as a new tunable group (TROPISM, 32 slots appended
//     at offset 836; NUM_WEIGHTS 836 -> 868). For each knight, bishop, rook
//     and queen, the Chebyshev distance to the enemy king is bucketed into
//     {1, 2, 3, 4+} and the corresponding (piece, bucket) weight is added.
//     Bucketing keeps the model linear (required for tuning) while letting the
//     distance->value curve be non-linear. This reintroduces a feature that
//     measured neutral when hand-set in 1.5; here it is Texel-tuned. Chebyshev
//     distance (a new helper) is used deliberately instead of the Manhattan
//     king_distance() in mopup -- concentric king-pressure rings vs corralling
//     a lone king, different geometries for different purposes. Folded into the
//     single evaluate() blend and recorded as trace coefficients, so the
//     evaluate() == score_from_trace() contract holds (within the documented
//     <=2cp blend-rounding tolerance). Alters playing strength and the bench
//     signature; ships at hand-set seeds and is then tuned.
//   - Pawn shelter/storm added as a new tunable group (SHELTER_STORM, 32 slots
//     appended at offset 868). Scores the pawns in front of each king as two
//     families. Shelter: for the king's file and the two adjacent files, the
//     friendly pawn nearest the king shelters it, scored by the rank gap
//     bucketed into {1, 2, 3, 4+}, where a file with no friendly pawn maps to
//     the 4+ (worst) bucket. Storm: enemy pawns advancing on those same files
//     are a threat, also bucketed by distance. Tapered, folded into the single
//     evaluate() blend, and recorded as trace coefficients. Ships at hand-set
//     seeds and is then tuned.
//   - King safety extended with a second tunable group (KING_SAFETY_V2, 16
//     slots appended at offset 900), complementing the attacker-based
//     KING_SAFETY group. Two families, scored per king and tapered. Open files
//     toward the king: the king's file and the two adjacent files are each
//     classified as open (no pawns) or semi-open (no friendly pawns), with the
//     king's own file weighted separately from the adjacent ones. Safe checks:
//     squares from which an enemy piece could deliver check without being
//     captured, counted per attacking piece type. Folded into the single
//     evaluate() blend and recorded as trace coefficients. Ships at hand-set
//     seeds and is then tuned.
//   - Positional refinement group added (POSITIONAL2, appended at offset 916),
//     a small thematic set of features each validated by self-play before
//     inclusion. Tempo: a side-to-move bonus, the one term that captures who
//     is on move. Bishop outpost: a bishop on an advanced square no enemy pawn
//     can challenge, with a larger bonus when a friendly pawn supports it
//     (mirrors the existing knight outpost). Passed-pawn refinement: for each
//     passed pawn, king proximity (own king near the pawn is good, enemy king
//     near is bad, weighted to the endgame), blockade (an enemy piece on the
//     stop square neutralizes the pawn, a minor more effectively than a major),
//     free path (a clear run to promotion), and protection (the pawn defended
//     by a friendly pawn). All terms are tapered, folded into the single
//     evaluate() blend, and recorded as trace coefficients. Each ships at
//     hand-set seeds and is then tuned.
// =============================================================================

#include "eval.h"
#include "bitboard.h"
#include <algorithm>  // std::max
#include <iostream>   // std::cout in evaluate_verbose
#include <iomanip>    // std::setw in evaluate_verbose

// =============================================================================
// EVAL_WEIGHTS DEFINITION AND INITIAL VALUES
// =============================================================================
// `eval_weights[NUM_WEIGHTS]` holds every tunable weight in the evaluation.
// The array is grouped (see the WeightGroup enum in eval.h for the layout).
// At program startup it is initialised to the hand-set values that previous
// versions held as named compile-time constants; tuning then replaces these
// initial entries with optimised values.
//
// IMPORTANT: this is not `const`. The whole point of centralising the weights
// is that they can be mutated in place during tuning. Read-only callers (the
// eval hot path) treat the array as read-only by convention; only the tuning
// driver writes to it.
//
// Layout reminder (consult eval.h for the authoritative definition):
//
//   [  0,  16)  PAWN_STRUCT      -- 4 features * (MG, EG), slots 8..15 reserved
//   [ 16,  32)  PASSED_BONUS     -- 8 ranks  * (MG, EG); ranks 0/1/7 stay zero
//   [ 32,  40)  MOBILITY         -- 4 piece types * (MG, EG)
//   [ 40,  46)  ROOK_BONUSES     -- 3 features * (MG, EG)
//   [ 46,  48)  BISHOP_PAIR      -- 1 feature  * (MG, EG)
//   [ 48,  52)  KNIGHT_OUTPOST   -- 2 tiers   * (MG, EG)
//   [ 52, 180)  PST_PAWN         -- 64 squares * (MG, EG), interleaved
//   [180, 308)  PST_KNIGHT       -- ...
//   [308, 436)  PST_BISHOP       -- ...
//   [436, 564)  PST_ROOK         -- ...
//   [564, 692)  PST_QUEEN        -- ...
//   [692, 820)  PST_KING         -- only PST whose MG and EG differ initially
//
// Notation in the PST blocks below. Each row of a PST holds (mg, eg) pairs
// for the eight files of one rank. For example, "(  5,  5)" in the pawn PST
// block at the row labelled "rank 2" means MG = 5, EG = 5 for the a2 square
// (when reading file 0..7 left to right). White's perspective: index 0 = a1,
// index 63 = h8. Black pieces are mirrored vertically inside the lookup
// helpers (sq ^ 56) before indexing.
//
// All values are in centipawns (100 cp = one pawn). Negative entries are
// penalties for bad squares; positive entries are bonuses.
//
// MG and EG explanations per PST follow the table that initialises them.
// In 1.6's starting state every MG and EG pair has identical values for all
// non-king PSTs; the king PST already has different MG and EG values because
// king behaviour inverts between phases (corner in MG, centre in EG).

int eval_weights[NUM_WEIGHTS] = {
// ---- PAWN_STRUCT (offsets 0..15): 4 used + 4 reserved padding ----
      -10,   -9,     -3,  -21,      0,   -5,     11,    9,      0,    0,      0,    0,      0,    0,      0,    0,
// ---- PASSED_BONUS (offsets 16..31): by rank (ranks 0 and 7 unreachable=0) ----
        0,    0,    -24,  -37,    -36,  -21,    -32,   16,     -2,   55,      0,  132,     40,  171,      0,    0,
// ---- MOBILITY (offsets 32..39) ----
        7,    4,      7,    4,      3,    3,      4,    1,
// ---- ROOK_BONUSES (offsets 40..45) ----
       38,    4,     14,   14,     -1,   37,
// ---- BISHOP_PAIR (offsets 46..47) ----
       24,   74,
// ---- KNIGHT_OUTPOST (offsets 48..51) ----
       35,   -9,     49,   27,
// ---- PST_PAWN (offsets 52..179) -- material folded in ----
      -17,   14,    -17,   14,    -17,   14,    -17,   14,    -17,   14,    -17,   14,    -17,   14,    -17,   14,
      -42,   34,    -36,   31,    -29,   25,    -36,   20,    -20,   32,    -10,   23,      2,   13,    -34,   11,
      -48,   29,    -40,   25,    -33,   19,    -27,   20,    -18,   19,    -22,   19,    -11,   12,    -26,    9,
      -38,   36,    -36,   35,    -20,   19,     -7,   11,     -2,   10,     -8,   16,    -17,   22,    -26,   16,
      -32,   56,    -19,   44,    -16,   30,     -9,   12,     16,    9,      2,   20,     -3,   33,    -21,   31,
       -8,   77,    -12,   82,     24,   44,     29,   10,     48,    3,     65,   26,     32,   62,    -11,   66,
       25,  100,     46,   85,     21,   98,     57,   48,     42,   49,     19,   69,    -33,   99,    -75,  118,
      -17,   14,    -17,   14,    -17,   14,    -17,   14,    -17,   14,    -17,   14,    -17,   14,    -17,   14,
// ---- PST_KNIGHT (offsets 180..307) ----
       -1,    9,     39,    3,     42,   27,     60,   28,     64,   29,     70,   17,     43,   13,     36,    3,
       34,   15,     47,   28,     61,   38,     78,   36,     80,   35,     75,   34,     72,   18,     67,   27,
       39,   24,     62,   39,     68,   47,     80,   59,     97,   57,     79,   41,     85,   34,     64,   27,
       45,   37,     60,   48,     79,   61,     80,   59,     93,   65,     92,   55,     92,   43,     67,   28,
       41,   51,     56,   55,     75,   64,     76,   69,     74,   73,     94,   63,     66,   62,     68,   40,
       31,   44,     58,   49,     62,   61,     64,   70,     96,   60,     88,   45,     59,   44,     22,   37,
       42,   29,     74,   38,     94,   42,     99,   45,     64,   44,    125,   32,     46,   44,     61,    9,
      -97,  -47,    -67,   19,    -26,   45,     16,   33,     43,   40,    -25,   18,    -73,   28,    -55,  -70,
// ---- PST_BISHOP (offsets 308..435) ----
       74,    0,     96,   23,     74,    9,     72,   17,     80,   15,     70,   22,     89,    6,     99,  -23,
       82,   15,     87,    8,     98,    8,     77,   21,     89,   22,     97,   16,    110,   14,     93,   -5,
       68,   16,     89,   30,     88,   30,     84,   34,     88,   42,     91,   33,     92,   24,     93,    8,
       59,   16,     54,   27,     70,   33,     92,   28,     90,   29,     71,   33,     70,   31,     81,    3,
       36,   29,     66,   31,     57,   30,     66,   45,     73,   33,     61,   39,     65,   33,     32,   38,
       39,   38,     47,   30,     47,   37,     38,   33,     29,   40,     62,   42,     45,   40,     29,   46,
       54,   22,     82,   27,     64,   34,     47,   45,     50,   36,     49,   40,     55,   37,     25,   30,
       36,   33,     16,   39,     17,   37,    -31,   51,    -38,   55,    -19,   36,     29,   33,      2,   21,
// ---- PST_ROOK (offsets 436..563) ----
       21,  157,     25,  155,     29,  162,     39,  152,     45,  146,     41,  150,     42,  146,     28,  141,
        5,  157,     10,  162,     24,  162,     29,  158,     33,  152,     39,  147,     58,  137,     21,  141,
        2,  169,      6,  168,     11,  167,     14,  167,     24,  161,     27,  156,     58,  139,     35,  141,
        1,  181,      1,  183,     11,  181,     18,  177,     16,  177,      6,  178,     32,  169,     17,  167,
        5,  194,     22,  188,     20,  195,     18,  192,     10,  183,     23,  180,     25,  185,     15,  180,
        7,  194,     38,  189,     33,  192,     18,  192,     35,  187,     40,  183,     76,  177,     31,  177,
       19,  160,     19,  170,     38,  171,     41,  165,      3,  174,     53,  159,     44,  155,     61,  145,
       27,  189,     31,  190,     26,  198,     16,  196,     14,  194,     38,  194,     42,  194,     49,  189,
// ---- PST_QUEEN (offsets 564..691) ----
      193,  353,    199,  354,    206,  359,    208,  368,    211,  354,    196,  357,    208,  339,    203,  334,
      202,  355,    203,  365,    207,  371,    214,  374,    212,  378,    219,  356,    226,  326,    234,  304,
      193,  368,    197,  397,    196,  411,    194,  406,    199,  413,    205,  404,    219,  385,    214,  374,
      191,  385,    183,  417,    188,  420,    196,  427,    198,  411,    195,  412,    204,  403,    212,  396,
      176,  404,    182,  417,    184,  417,    169,  430,    165,  429,    182,  413,    181,  423,    185,  408,
      186,  399,    180,  409,    177,  434,    165,  434,    157,  446,    184,  427,    189,  402,    184,  407,
      171,  402,    161,  419,    162,  447,    129,  463,     90,  493,    158,  435,    151,  443,    202,  410,
      136,  414,    152,  409,    168,  426,    186,  417,    156,  422,    160,  425,    213,  367,    149,  412,
// ---- PST_KING (offsets 692..819) -- no material fold (king=0) ----
       14,  -80,     38,  -56,     31,  -35,    -38,  -13,     13,  -38,    -28,  -20,     20,  -50,     22,  -92,
       38,  -56,      4,  -20,      8,   -9,     -9,   -2,    -14,    1,     -7,   -8,     13,  -24,     18,  -50,
      -34,  -40,     -2,  -15,    -31,    3,    -26,   12,    -18,   10,    -35,    5,    -18,  -11,    -53,  -26,
      -56,  -38,    -27,  -10,    -47,   14,    -88,   30,    -79,   28,    -51,   12,    -69,    3,   -139,   -9,
      -44,  -26,    -12,   -1,    -41,   21,    -90,   36,    -88,   34,    -66,   29,    -66,   16,   -141,    6,
      -74,  -11,     73,    1,     17,   20,    -23,   34,     25,   33,     82,   19,     21,   22,    -20,   -4,
      -64,  -16,     18,   10,      1,   19,    109,   -1,     41,   14,     31,   31,     43,   22,    -29,    2,
      108, -113,    122,  -62,    108,  -42,     -2,   -1,     24,  -20,    -12,   -6,     44,  -16,    185, -133,
// ---- KING_SAFETY (offsets 820..835): linear king-safety terms ----
       -7,    8,    -31,   -6,    -32,   10,    -19,  -19,     24,    2,     18,    0,    -22,   -9,   -126,   43,
// ---- TROPISM (offsets 836..867): piece proximity to enemy king ----
      -21,   26,    -15,   41,    -37,   62,    -69,   62,    -56,   22,    -35,   48,    -56,   57,    -72,   71,
      -18,   76,    -34,   86,    -62,   95,    -84,  105,      8,    0,    -31,  142,    -93,  147,   -118,  131,
// ---- SHELTER_STORM (offsets 868..899): pawn shelter/storm ----
       24,   -4,     11,    2,      0,   -5,    -10,    2,     14,    1,      1,    6,     -2,   -1,     -2,   -3,
       87,    9,    -11,  -21,     15,  -15,     22,  -19,      4,   27,    -21,  -12,     -4,  -15,     11,  -16,
// ---- KING_SAFETY_V2 (offsets 900..915): open files + safe checks ----
      -41,  -10,    -20,   -7,    -18,   21,    -20,   23,   -110,   -4,    -40,  -22,   -106,    9,    -42,  -74,
// ---- POSITIONAL2 (offsets 916..933): tempo + bishop-outpost + passer refinement ----
       23,   29,     30,   -1,     56,   15,      5,  -10,      1,   13,    -22,  -50,     -9,  -10,    -19,   26,     15,    7
};


// =============================================================================
// GAME PHASE DETECTION
// =============================================================================
// Returns a 0..256 weight used to blend middlegame and endgame evaluation
// terms (mostly PSTs). 256 means "full non-pawn material on the board"
// (startpos, treated as full middlegame); 0 means "only kings left" (pure
// endgame). The transition is smooth: as material comes off, the weight
// drops gradually.
//
// Each piece type contributes to the raw phase count:
//   Knight=1, Bishop=1, Rook=2, Queen=4 (pawns and kings don't count)
// Maximum raw phase = 4*1 + 4*1 + 4*2 + 2*4 = 24 (full piece set, both sides).
// The result is then scaled to 0..256 for easy use in fixed-point blending
// (multiply by phase_mg / 256, no floating point needed).
//
// Contract: the function counts material PRESENT, not material absent. This
// matches what consumers (king_safety, pst_mg_value / pst_eg_value, and the
// per-block tapered functions) expect. (An earlier version of this function
// inverted the count, with all the downstream symptoms one would expect; the
// inversion was fixed in 1.5.)

static const int PHASE_VALUE[7] = { 0, 0, 1, 1, 2, 4, 0 };
constexpr int TOTAL_PHASE = 24;  // 4 knights + 4 bishops + 4 rooks + 2 queens

static int game_phase(const Board& board) {
    int phase = 0;
    // Sum phase contribution from every minor and major piece on the board.
    for (int pt = KNIGHT; pt <= QUEEN; pt++)
        phase += popcount(board.piece_bb(PieceType(pt))) * PHASE_VALUE[pt];
    // Clamp: under-promotions can create more queens than the starting position
    // had, pushing the raw count above TOTAL_PHASE. Cap it so the scaled
    // result stays within 0..256.
    phase = std::min(phase, TOTAL_PHASE);
    // Scale 0..TOTAL_PHASE linearly to 0..256, with rounding (+ TOTAL_PHASE/2).
    return (phase * 256 + TOTAL_PHASE / 2) / TOTAL_PHASE;
}

// =============================================================================
// PST LOOKUP
// =============================================================================
// Two helpers return the raw MG and EG table entries for a piece of type pt
// for side c on square sq. The blend between them is done in the caller, not
// here. Result is in centipawns, positive for "good square for this piece"
// and negative for "bad square".
//
// The mirror for Black. All tables are written from White's perspective. To
// look up a Black piece's value we mirror the square vertically so it indexes
// the same conceptual position on the table:
//
//   black_idx = sq ^ 56
//
// XOR with 56 = 0b00111000 flips the three rank bits of the square index.
// Example: a black rook on a8 (sq=56). 56 ^ 56 = 0, so it reads
// eval_weights[PST_ROOK_START + 0*2 + (MG or EG offset)], the same entry a
// white rook on a1 reads. Both rooks are on their starting square; the table
// value is the same. Likewise b8 ^ 56 = b1, h5 ^ 56 = h4, etc.
//
// Why MG and EG are returned separately. Earlier in 1.6 a single pst_value()
// function blended MG and EG internally and returned one number; the caller
// (the evaluate() loop) just summed those blended numbers. That shape mixed
// blending conventions: pawn_structure() and positional_eval() blend at the
// very end (Option B), while the material+PST loop was blending per piece
// (Option A). With MG == EG the two conventions are bit-identical in integer
// arithmetic, but once tuning pulls them apart they diverge by a few cp per
// position. Splitting pst_value() into pst_mg_value() and pst_eg_value() lets
// the loop accumulate raw MG and EG totals and blend once at the end -- the
// same shape the rest of the eval uses. With MG == EG (the initial state of
// 1.6) the integer arithmetic identity
//
//   (V * p + V * (256 - p)) / 256 = (V * 256) / 256 = V
//
// guarantees that summing raw values and blending later gives the same result
// summing already-blended values does, so bench signature is preserved.

static int pst_mg_value(PieceType pt, Color c, Square sq) {
    // Mirror Black's square so it indexes the table from White's perspective.
    int idx = (c == WHITE) ? sq : (sq ^ 56);

    // Each PST block in eval_weights is laid out as 64 squares * 2 phases,
    // interleaved [sq*2+0]=MG, [sq*2+1]=EG. Per-piece base offsets are
    // declared in eval.h.
    switch (pt) {
        case PAWN:   return eval_weights[PST_PAWN_START   + idx * 2 + 0];
        case KNIGHT: return eval_weights[PST_KNIGHT_START + idx * 2 + 0];
        case BISHOP: return eval_weights[PST_BISHOP_START + idx * 2 + 0];
        case ROOK:   return eval_weights[PST_ROOK_START   + idx * 2 + 0];
        case QUEEN:  return eval_weights[PST_QUEEN_START  + idx * 2 + 0];
        case KING:   return eval_weights[PST_KING_START   + idx * 2 + 0];
        // NO_PIECE_TYPE should never reach this function in legal positions.
        // Returning 0 is defensive -- it keeps a stray call harmless rather
        // than reading random memory or crashing.
        default: return 0;
    }
}

static int pst_eg_value(PieceType pt, Color c, Square sq) {
    // Same mirror as pst_mg_value -- the two functions index the same square,
    // just read the EG slot (offset + 1) instead of the MG slot (offset + 0).
    int idx = (c == WHITE) ? sq : (sq ^ 56);
    switch (pt) {
        case PAWN:   return eval_weights[PST_PAWN_START   + idx * 2 + 1];
        case KNIGHT: return eval_weights[PST_KNIGHT_START + idx * 2 + 1];
        case BISHOP: return eval_weights[PST_BISHOP_START + idx * 2 + 1];
        case ROOK:   return eval_weights[PST_ROOK_START   + idx * 2 + 1];
        case QUEEN:  return eval_weights[PST_QUEEN_START  + idx * 2 + 1];
        case KING:   return eval_weights[PST_KING_START   + idx * 2 + 1];
        default: return 0;
    }
}

// =============================================================================
// KING SAFETY
// =============================================================================
// Penalizes a king that has enemy pieces threatening its zone. The zone is
// the king's square plus the 8 adjacent squares -- up to 9 squares in total,
// fewer if the king is on an edge or in a corner.
//
// LINEAR, TUNABLE FORMULATION (Facon 1.6).
//   Earlier versions used a quadratic penalty, penalty = total_weight^2 *
//   SCALE, where each attacking piece type added a hand-set weight to
//   total_weight. The quadratic captured a real effect -- two or three
//   coordinating attackers are far more dangerous than one -- but it could
//   not be Texel-tuned, because the tuner optimises a strictly linear model
//   (a w^2 term has a weight-dependent gradient the tuner does not compute).
//   Hand-setting those constants is exactly what failed at this strength
//   level previously.
//
//   The penalty is now a sum of linear terms, each with its own tunable
//   weight in eval_weights[]:
//
//     (a) Per-attacker-type weight: for every enemy piece that attacks the
//         king zone, add that piece type's weight (knight, bishop, rook,
//         queen). Replaces the old KING_ATTACK_WEIGHT[] table.
//
//     (b) Attacker-count bucket: add one weight chosen by how many distinct
//         pieces attack the zone -- bucket 1, 2, 3, or 4-or-more. Each bucket
//         is selected by the count, so the tuner can make three attackers
//         cost much more than three times one attacker, recovering the
//         non-linear "coordination" effect. Crucially the 4+ bucket is a hard
//         ceiling: there is no way to produce the unbounded penalties that
//         the old quadratic could (the 1.5 attempt generated thousands of cp
//         and lost rating). A count of 0 contributes nothing.
//
//   A queen is counted as a SINGLE attacker (one piece, one entry in the
//   count) whose extra danger is expressed by its large per-type weight in
//   (a). This drops the old "count the queen twice" hack: the queen now has
//   an explicit, separately tuned weight instead.
//
//   Both families are tapered (MG, EG). King safety is a middlegame concern;
//   rather than an endgame early-return, the EG weights are free to tune
//   toward ~0 and the phase blend fades the term out. king_safety() applies
//   that blend itself and returns a side-relative centipawn penalty so the
//   call sites in evaluate() are unchanged.

// king_zone_attackers: shared counting routine, the single source of truth for
// both king_safety() (which turns counts into a score) and trace_evaluate()
// (which turns the same counts into tuner coefficients). Keeping one routine
// guarantees the traced coefficients reproduce the score exactly.
//
// Fills counts[KNIGHT..QUEEN] with how many enemy pieces of each type attack
// the king zone of `us`, and returns the total number of attacking pieces.
struct KingZoneAttackers {
    int knight;
    int bishop;
    int rook;
    int queen;
    int total;  // knight + bishop + rook + queen
};

static inline KingZoneAttackers king_zone_attackers(const Board& board, Color us) {
    Color    them      = ~us;
    Square   king_sq   = board.king_square(us);
    // King zone = king's square + all adjacent squares. king_attack returns
    // the up-to-8 adjacent squares; OR in the king's own square.
    Bitboard king_zone = king_attack(king_sq) | square_bb(king_sq);
    Bitboard occ       = board.occupancy();

    KingZoneAttackers a{0, 0, 0, 0, 0};

    // Knights: leapers, attack pattern fixed by location, occupancy-independent.
    Bitboard knights = board.piece_bb(them, KNIGHT);
    while (knights) {
        if (knight_attack(pop_lsb(knights)) & king_zone)
            a.knight++;
    }

    // Bishops (diagonal sliders).
    Bitboard bishops = board.piece_bb(them, BISHOP);
    while (bishops) {
        if (bishop_attack(pop_lsb(bishops), occ) & king_zone)
            a.bishop++;
    }

    // Rooks (straight sliders).
    Bitboard rooks = board.piece_bb(them, ROOK);
    while (rooks) {
        if (rook_attack(pop_lsb(rooks), occ) & king_zone)
            a.rook++;
    }

    // Queens: a single attacker, counted once. The queen attacks along both
    // diagonals and ranks/files, so it reaches the zone if EITHER a bishop-like
    // or a rook-like ray from its square touches the zone.
    Bitboard queens = board.piece_bb(them, QUEEN);
    while (queens) {
        Square qs = pop_lsb(queens);
        if ((bishop_attack(qs, occ) | rook_attack(qs, occ)) & king_zone)
            a.queen++;
    }

    a.total = a.knight + a.bishop + a.rook + a.queen;
    return a;
}

// count_bucket_offset: maps a (>=1) attacker count to its bucket's MG offset
// within the KING_SAFETY group. Counts of 4 or more share the 4+ bucket.
// Caller must ensure count >= 1.
static inline int count_bucket_offset(int count) {
    switch (count) {
        case 1:  return W_KS_COUNT_1_MG;
        case 2:  return W_KS_COUNT_2_MG;
        case 3:  return W_KS_COUNT_3_MG;
        default: return W_KS_COUNT_4PLUS_MG;  // 4 or more
    }
}

// king_safety: computes one king's safety penalty. Fills `mg_out` and `eg_out`
// with the RAW (un-blended) middlegame and endgame penalties -- the sums of the
// tunable weights -- and also returns the phase-blended value for callers that
// want a single number (evaluate_verbose). evaluate() uses the raw outputs and
// folds them into its single end-of-function blend, so that evaluate() and
// score_from_trace() perform the exact same number of integer divisions and
// agree bit-for-bit (no per-term rounding drift).
//
// Raw values are negative (penalties); the sign that makes a penalty reduce
// `us`'s score is applied by the caller (evaluate adds White's and subtracts
// Black's), matching the rest of the eval's White-relative convention.
static Score king_safety(const Board& board, Color us, int phase_mg,
                         int& mg_out, int& eg_out) {
    KingZoneAttackers a = king_zone_attackers(board, us);

    mg_out = 0;
    eg_out = 0;

    // No attackers: no penalty in either phase. (Also the common case, so it
    // is cheap to short-circuit.)
    if (a.total == 0) return 0;

    const int* w = &eval_weights[KING_SAFETY_START];

    int mg = 0, eg = 0;

    // (a) Per-attacker-type weights, one add per attacking piece.
    mg += a.knight * w[W_KS_ATTACKER_KNIGHT_MG];
    eg += a.knight * w[W_KS_ATTACKER_KNIGHT_EG];
    mg += a.bishop * w[W_KS_ATTACKER_BISHOP_MG];
    eg += a.bishop * w[W_KS_ATTACKER_BISHOP_EG];
    mg += a.rook   * w[W_KS_ATTACKER_ROOK_MG];
    eg += a.rook   * w[W_KS_ATTACKER_ROOK_EG];
    mg += a.queen  * w[W_KS_ATTACKER_QUEEN_MG];
    eg += a.queen  * w[W_KS_ATTACKER_QUEEN_EG];

    // (b) Attacker-count bucket, a single add chosen by the total count.
    const int bucket = count_bucket_offset(a.total);
    mg += w[bucket + 0];
    eg += w[bucket + 1];

    mg_out = mg;
    eg_out = eg;

    // Phase-blended value (for verbose display only).
    return Score((mg * phase_mg + eg * (256 - phase_mg)) / 256);
}

// =============================================================================
// PIECE TROPISM
// =============================================================================
// Rewards having your pieces close to the enemy king -- a positional pressure
// term complementary to king safety (king safety penalizes the defender for
// attackers on the king zone; tropism rewards the attacker for proximity).
//
// For each of your knights, bishops, rooks and queens, we take the Chebyshev
// distance to the enemy king and bucket it into {1, 2, 3, 4-or-more}. Each
// (piece type, bucket) pair has its own tunable (MG, EG) weight. Bucketing
// keeps the model linear in the weights -- a hard requirement for Texel tuning
// -- while letting the distance->value relationship be non-linear: the tuner
// can make a queen adjacent to the king worth far more than one three squares
// out, rather than forcing a straight line. The 4+ bucket lumps together every
// piece far enough to exert little king pressure.
//
// Chebyshev (king-move) distance is the natural "rings around the king" metric
// for tropism. Note this is deliberately a DIFFERENT metric from the Manhattan
// distance used in mopup_eval(): mopup models corralling a lone king toward a
// corner (where Manhattan is the calibrated, validated choice), whereas
// tropism models concentric king-pressure rings. Same idea (king distance),
// different geometry, each suited to its purpose.

// Chebyshev distance between two squares: max(|file diff|, |rank diff|), i.e.
// the number of king moves between them. Range 0..7.
static inline int chebyshev_distance(Square a, Square b) {
    return std::max(std::abs(file_of(a) - file_of(b)),
                    std::abs(rank_of(a) - rank_of(b)));
}

// tropism_bucket: maps a Chebyshev distance (>=1 in practice; a piece is never
// on the enemy king's square) to its bucket index 0..3 for distances
// {1, 2, 3, 4+}. Distance 0 would also map to bucket 0 but cannot occur.
static inline int tropism_bucket(int dist) {
    if (dist <= 1) return 0;
    if (dist == 2) return 1;
    if (dist == 3) return 2;
    return 3;  // 4 or more
}

// tropism_counts: shared counting routine, the single source of truth for both
// tropism() (which turns counts into a score) and trace_evaluate() (which turns
// the same counts into tuner coefficients). For the side `us`, fills a 4x4
// matrix counts[piece_index][bucket] where piece_index is 0..3 for
// {knight, bishop, rook, queen}, counting how many of that piece type sit at
// each Chebyshev-distance bucket from the enemy king.
struct TropismCounts {
    int c[4][4];  // [piece: N,B,R,Q][bucket: d1,d2,d3,d4+]
};

static inline TropismCounts tropism_counts(const Board& board, Color us) {
    TropismCounts tc{};  // zero-initialised
    Color  them      = ~us;
    Square ksq       = board.king_square(them);

    const PieceType types[4] = { KNIGHT, BISHOP, ROOK, QUEEN };
    for (int pi = 0; pi < 4; pi++) {
        Bitboard bb = board.piece_bb(us, types[pi]);
        while (bb) {
            Square s = pop_lsb(bb);
            tc.c[pi][tropism_bucket(chebyshev_distance(s, ksq))]++;
        }
    }
    return tc;
}

// tropism: computes one side's tropism bonus. Fills raw (un-blended) MG and EG
// sums and returns the phase-blended value (for verbose display). evaluate()
// uses the raw outputs and folds them into its single blend so evaluate() and
// score_from_trace() stay bit-identical. The sign that makes this a bonus for
// `us` is applied by the caller (evaluate adds White's, subtracts Black's).
static Score tropism(const Board& board, Color us, int phase_mg,
                     int& mg_out, int& eg_out) {
    TropismCounts tc = tropism_counts(board, us);
    const int* w = &eval_weights[TROPISM_START];

    int mg = 0, eg = 0;
    // Each piece type occupies 8 consecutive weights (4 buckets x 2 phases).
    for (int pi = 0; pi < 4; pi++) {
        const int base = pi * 8;
        for (int b = 0; b < 4; b++) {
            const int n = tc.c[pi][b];
            if (!n) continue;
            mg += n * w[base + b * 2 + 0];
            eg += n * w[base + b * 2 + 1];
        }
    }

    mg_out = mg;
    eg_out = eg;
    return Score((mg * phase_mg + eg * (256 - phase_mg)) / 256);
}

// =============================================================================
// PAWN SHELTER / STORM
// =============================================================================
// Scores the pawns in front of each king. Two complementary phenomena:
//
//   SHELTER: friendly pawns shield the king. For each of the three files
//     around the king (king file + two adjacent), the friendly pawn NEAREST
//     the king is the shelter for that file; the closer it is, the safer.
//
//   STORM: enemy pawns advancing on the king are a threat. For the same three
//     files, the enemy pawn MOST ADVANCED toward our king leads the storm; the
//     closer it is, the more dangerous.
//
// Distance is the rank gap between the relevant pawn and the king, bucketed
// {1, 2, 3, 4+} -- the same linear-in-weights bucketing used by tropism, so
// the tuner can give each ring its own value. The king's own file is scored
// with its own weights; the two adjacent files share a second set (the king
// file is the more critical, but distinguishing both adjacent files
// separately would only add weights for little signal).
//
// A file with NO friendly pawn maps SHELTER to the 4+ bucket (an open file in
// front of the king is the worst shelter -- a pawn "infinitely far"). A file
// with no enemy pawn contributes no STORM term. Files off the board edge
// (king on the a- or h-file) are simply skipped.
//
// This term coexists with king_safety() (piece attackers on the king zone)
// and tropism (our pieces' proximity to their king); shelter/storm is the
// pawn-structure component of king safety, and the three are independent.

// shelter_storm_bucket: rank gap (>=1 in practice) to bucket 0..3 for
// {1, 2, 3, 4+}. A gap of 0 cannot occur (a pawn never shares the king's
// square); callers pass SS_DIST_NONE for "no pawn", which also lands in 4+.
static constexpr int SS_DIST_NONE = 99;
static inline int shelter_storm_bucket(int gap) {
    if (gap <= 1) return 0;
    if (gap == 2) return 1;
    if (gap == 3) return 2;
    return 3;  // 4 or more, including SS_DIST_NONE
}

// Counts, the single source of truth shared by shelter_storm() and
// trace_evaluate(). For side `us`, fills two 2x4 matrices:
//   shelter[cat][bucket] -- friendly-pawn shelter, cat 0 = king file, 1 = adj
//   storm[cat][bucket]   -- enemy-pawn storm, same categories
// Each cell is a count of files (0..2) contributing that (category, bucket).
struct ShelterStormCounts {
    int shelter[2][4];  // [cat: kfile, adj][bucket: d1,d2,d3,d4+]
    int storm[2][4];
};

static inline ShelterStormCounts shelter_storm_counts(const Board& board, Color us) {
    ShelterStormCounts sc{};  // zero-initialised
    Color  them   = ~us;
    Square ksq    = board.king_square(us);
    int    kfile  = file_of(ksq);
    int    krank  = rank_of(ksq);

    Bitboard our_pawns   = board.piece_bb(us,   PAWN);
    Bitboard their_pawns = board.piece_bb(them, PAWN);

    // Walk the king's file and the two adjacent files.
    for (int df = -1; df <= 1; df++) {
        int f = kfile + df;
        if (f < 0 || f > 7) continue;             // off the board edge
        int cat = (df == 0) ? 0 : 1;              // 0 = king file, 1 = adjacent

        // Mask of file f (same as file_bb(), inlined here because file_bb is
        // defined later in this translation unit, below pawn_structure).
        Bitboard file_mask = FILE_A_BB << f;

        // SHELTER: friendly pawn nearest the king on this file, measured by the
        // smallest rank gap. Taking the minimum |rank - krank| handles both
        // colours symmetrically. No friendly pawn -> SS_DIST_NONE -> 4+ bucket.
        Bitboard own_on_file = our_pawns & file_mask;
        int shelter_gap = SS_DIST_NONE;
        while (own_on_file) {
            Square s = pop_lsb(own_on_file);
            int gap = std::abs(rank_of(s) - krank);
            if (gap < shelter_gap) shelter_gap = gap;
        }
        sc.shelter[cat][shelter_storm_bucket(shelter_gap)]++;

        // STORM: enemy pawn most advanced toward our king on this file, i.e.
        // the enemy pawn with the smallest rank gap to the king. No enemy pawn
        // on this file -> no storm term.
        Bitboard enemy_on_file = their_pawns & file_mask;
        int storm_gap = SS_DIST_NONE;
        while (enemy_on_file) {
            Square s = pop_lsb(enemy_on_file);
            int gap = std::abs(rank_of(s) - krank);
            if (gap < storm_gap) storm_gap = gap;
        }
        if (storm_gap != SS_DIST_NONE) {
            sc.storm[cat][shelter_storm_bucket(storm_gap)]++;
        }
    }
    return sc;
}

// shelter_storm: one side's shelter/storm score. Fills raw (un-blended) MG and
// EG and returns the phase-blended value (verbose display only). evaluate()
// uses the raw outputs and folds them into its single blend so evaluate() and
// score_from_trace() stay bit-identical. Sign is applied by the caller
// (evaluate adds White's, subtracts Black's).
static Score shelter_storm(const Board& board, Color us, int phase_mg,
                           int& mg_out, int& eg_out) {
    ShelterStormCounts sc = shelter_storm_counts(board, us);
    const int* w = &eval_weights[SHELTER_STORM_START];

    int mg = 0, eg = 0;

    // SHELTER: king-file weights at W_SS_SHELTER_KFILE_D1_MG (0), adjacent at (8).
    for (int cat = 0; cat < 2; cat++) {
        const int base = (cat == 0) ? W_SS_SHELTER_KFILE_D1_MG
                                     : W_SS_SHELTER_AFILE_D1_MG;
        for (int b = 0; b < 4; b++) {
            const int n = sc.shelter[cat][b];
            if (!n) continue;
            mg += n * w[base + b * 2 + 0];
            eg += n * w[base + b * 2 + 1];
        }
    }
    // STORM: king-file at W_SS_STORM_KFILE_D1_MG (16), adjacent at (24).
    for (int cat = 0; cat < 2; cat++) {
        const int base = (cat == 0) ? W_SS_STORM_KFILE_D1_MG
                                     : W_SS_STORM_AFILE_D1_MG;
        for (int b = 0; b < 4; b++) {
            const int n = sc.storm[cat][b];
            if (!n) continue;
            mg += n * w[base + b * 2 + 0];
            eg += n * w[base + b * 2 + 1];
        }
    }

    mg_out = mg;
    eg_out = eg;
    return Score((mg * phase_mg + eg * (256 - phase_mg)) / 256);
}

// =============================================================================
// KING SAFETY v2: OPEN FILES + SAFE CHECKS
// =============================================================================
// Two further king-safety families, complementary to king_safety() (piece
// attackers in the king zone) and shelter/storm (pawns in front of the king):
//
//   OPEN FILES: a fully-open file (no pawns) or a semi-open file (no friendly
//     pawns) running through the king's file or an adjacent file lets enemy
//     rooks/queens attack the king. Each of the three files is classified
//     open / semi-open / closed; closed scores nothing. The king's file has
//     its own weights, the two adjacent files share a set.
//
//   SAFE CHECKS: a square from which an enemy piece could give check WITHOUT
//     being captured -- a checking square the enemy actually attacks that the
//     defender does NOT cover. The defender's full attack map decides safety.
//     One penalty per checking piece type; a safe queen check is gravest.
//
// Like the other king-safety terms these are penalties, summed raw and folded
// into evaluate()'s single blend. king_safety_v2_counts() is the shared source
// of truth for both king_safety_v2() and trace_evaluate().

struct KingSafetyV2Counts {
    int open_king;     // 1 if king's file is fully open, else 0
    int open_adj;      // number of adjacent files (0..2) fully open
    int semi_king;     // 1 if king's file is semi-open (no friendly pawn)
    int semi_adj;      // number of adjacent files (0..2) semi-open
    int safe_n;        // 1 if a safe knight check exists
    int safe_b;        // 1 if a safe bishop check exists
    int safe_r;        // 1 if a safe rook check exists
    int safe_q;        // 1 if a safe queen check exists
};

static inline KingSafetyV2Counts king_safety_v2_counts(const Board& board, Color us) {
    KingSafetyV2Counts c{};  // zero-initialised
    Color    them    = ~us;
    Square   king_sq = board.king_square(us);
    int      kfile   = file_of(king_sq);
    Bitboard occ     = board.occupancy();

    Bitboard our_pawns   = board.piece_bb(us,   PAWN);
    Bitboard their_pawns = board.piece_bb(them, PAWN);

    // ---- Open / semi-open files on the king's file and the two adjacent ----
    for (int df = -1; df <= 1; df++) {
        int f = kfile + df;
        if (f < 0 || f > 7) continue;
        Bitboard fmask = FILE_A_BB << f;
        bool no_friendly = !(our_pawns   & fmask);
        bool no_enemy    = !(their_pawns & fmask);
        if (no_friendly && no_enemy) {
            // Fully open.
            if (df == 0) c.open_king = 1; else c.open_adj++;
        } else if (no_friendly) {
            // Semi-open (no friendly pawn, but an enemy pawn present).
            if (df == 0) c.semi_king = 1; else c.semi_adj++;
        }
        // else: a friendly pawn is on the file -> closed -> no term.
    }

    // ---- Safe checks: checking squares the enemy attacks and the defender
    // does not. The defender's attack map covers "defended" squares; a checking
    // square not in that map and not occupied by a friendly (enemy-of-king)
    // piece is a safe check. Empty or enemy-occupied checking squares both
    // count as long as the enemy piece can move there to give check.
    Bitboard defended   = board.attacked_by(us);     // squares the king's side covers
    Bitboard them_occ   = board.all_pieces(them);    // enemy cannot land on its own pieces
    Bitboard safe       = ~defended & ~them_occ;      // squares enemy can use without recapture

    // Checking-square templates from the king's square (where a piece must be
    // to check this king), intersected with the enemy pieces' actual reach.
    Bitboard knight_check_sq = knight_attack(king_sq);
    Bitboard bishop_check_sq = bishop_attack(king_sq, occ);
    Bitboard rook_check_sq   = rook_attack(king_sq, occ);

    // Enemy reach by type (where each enemy piece type can move to).
    Bitboard enemy_knight_reach = 0;
    Bitboard kn = board.piece_bb(them, KNIGHT);
    while (kn) enemy_knight_reach |= knight_attack(pop_lsb(kn));

    Bitboard enemy_bishop_reach = 0;
    Bitboard bi = board.piece_bb(them, BISHOP);
    while (bi) enemy_bishop_reach |= bishop_attack(pop_lsb(bi), occ);

    Bitboard enemy_rook_reach = 0;
    Bitboard ro = board.piece_bb(them, ROOK);
    while (ro) enemy_rook_reach |= rook_attack(pop_lsb(ro), occ);

    Bitboard enemy_queen_reach = 0;
    Bitboard qu = board.piece_bb(them, QUEEN);
    while (qu) {
        Square qs = pop_lsb(qu);
        enemy_queen_reach |= bishop_attack(qs, occ) | rook_attack(qs, occ);
    }

    // A safe check of a given type exists if any checking square for that type
    // is both reachable by an enemy piece of that type and safe.
    if (knight_check_sq & enemy_knight_reach & safe) c.safe_n = 1;
    if (bishop_check_sq & enemy_bishop_reach & safe) c.safe_b = 1;
    if (rook_check_sq   & enemy_rook_reach   & safe) c.safe_r = 1;
    if ((bishop_check_sq | rook_check_sq) & enemy_queen_reach & safe) c.safe_q = 1;

    return c;
}

// king_safety_v2: one king's open-file + safe-check penalty. Fills raw MG/EG
// and returns the blended value (verbose only). evaluate() uses the raw outputs
// and folds them into its single blend; sign applied by the caller.
static Score king_safety_v2(const Board& board, Color us, int phase_mg,
                            int& mg_out, int& eg_out) {
    KingSafetyV2Counts c = king_safety_v2_counts(board, us);
    const int* w = &eval_weights[KING_SAFETY_V2_START];

    int mg = 0, eg = 0;

    // Open files.
    mg += c.open_king * w[W_KSV2_OPENFILE_KING_MG];
    eg += c.open_king * w[W_KSV2_OPENFILE_KING_EG];
    mg += c.open_adj  * w[W_KSV2_OPENFILE_ADJ_MG];
    eg += c.open_adj  * w[W_KSV2_OPENFILE_ADJ_EG];
    mg += c.semi_king * w[W_KSV2_SEMIFILE_KING_MG];
    eg += c.semi_king * w[W_KSV2_SEMIFILE_KING_EG];
    mg += c.semi_adj  * w[W_KSV2_SEMIFILE_ADJ_MG];
    eg += c.semi_adj  * w[W_KSV2_SEMIFILE_ADJ_EG];

    // Safe checks.
    mg += c.safe_n * w[W_KSV2_SAFECHECK_N_MG];
    eg += c.safe_n * w[W_KSV2_SAFECHECK_N_EG];
    mg += c.safe_b * w[W_KSV2_SAFECHECK_B_MG];
    eg += c.safe_b * w[W_KSV2_SAFECHECK_B_EG];
    mg += c.safe_r * w[W_KSV2_SAFECHECK_R_MG];
    eg += c.safe_r * w[W_KSV2_SAFECHECK_R_EG];
    mg += c.safe_q * w[W_KSV2_SAFECHECK_Q_MG];
    eg += c.safe_q * w[W_KSV2_SAFECHECK_Q_EG];

    mg_out = mg;
    eg_out = eg;
    return Score((mg * phase_mg + eg * (256 - phase_mg)) / 256);
}

// Flood-fill a bitboard toward each promotion edge. Used by passed-pawn
// detection in the positional batch (same as the lambdas in pawn_structure).
static inline Bitboard fill_north_bb(Bitboard b) {
    b |= b << 8; b |= b << 16; b |= b << 32;
    return b;
}
static inline Bitboard fill_south_bb(Bitboard b) {
    b |= b >> 8; b |= b >> 16; b |= b >> 32;
    return b;
}

// Is the pawn on `sq` (belonging to `us`) passed? Same flood-fill test as
// pawn_structure(): no enemy pawn on its front file or the adjacent files
// ahead of it. No per-rank loops.
static inline bool is_passed_pawn(Square sq, Color us, Bitboard their_pawns) {
    Bitboard front, attack_forward;
    Square stop = (us == WHITE) ? Square(sq + 8) : Square(sq - 8);
    if (us == WHITE) {
        front          = fill_north_bb(square_bb(stop));
        attack_forward = fill_north_bb(pawn_attacks_bb(us, square_bb(sq)));
    } else {
        front          = fill_south_bb(square_bb(stop));
        attack_forward = fill_south_bb(pawn_attacks_bb(us, square_bb(sq)));
    }
    return !(their_pawns & (front | attack_forward));
}

// Adjacent files mask (file f-1 and f+1). Shared by pawn structure and the
// positional batch (bishop outpost). Defined here so positional2_counts can
// use it.
static inline Bitboard adjacent_files_bb(int f) {
    Bitboard adj = 0;
    if (f > 0) adj |= FILE_A_BB << (f - 1);
    if (f < 7) adj |= FILE_A_BB << (f + 1);
    return adj;
}

// =============================================================================
// POSITIONAL BATCH (1.6): built up one feature at a time
// =============================================================================
// Features are added incrementally; each is validated by self-play before the
// next is included. positional2_counts() is the shared source of truth for
// positional2() and trace_evaluate(). Holds tempo, bishop outpost (reachable
// and supported), and passed-pawn refinement (king proximity, blockade by an
// enemy minor/major, free path, and pawn protection).

struct Positional2Counts {
    int tempo;           // +1 white to move, -1 black to move (side-to-move)
    int boutpost_reach;  // bishops on a safe advanced square, unsupported (W-B)
    int boutpost_supp;   // bishops on a safe advanced square, pawn-supported (W-B)
    int passer_own_kdist;    // sum over our passers of own-king Chebyshev dist (W-B)
    int passer_enemy_kdist;  // sum over our passers of enemy-king Chebyshev dist (W-B)
    int passer_blocked_minor; // our passers blocked by an enemy minor (W-B)
    int passer_blocked_major; // our passers blocked by an enemy major (W-B)
    int passer_free_path;     // our passers with a clear path to promotion (W-B)
    int passer_protected;     // our passers defended by a friendly pawn (W-B)
};

static inline Positional2Counts positional2_counts(const Board& board, Color stm) {
    Positional2Counts c{};
    c.tempo = (stm == WHITE) ? +1 : -1;

    Bitboard white_pawns = board.piece_bb(WHITE, PAWN);
    Bitboard black_pawns = board.piece_bb(BLACK, PAWN);

    for (int ci = WHITE; ci <= BLACK; ci++) {
        Color us  = Color(ci);
        int   s   = (us == WHITE) ? +1 : -1;
        Bitboard our_pawns = (us == WHITE) ? white_pawns : black_pawns;

        // ---- Bishop outpost: identical geometry to the knight outpost. A
        // bishop on relative ranks 4-6, on a square no enemy pawn can attack
        // now or in the future (no enemy pawn on an adjacent file ahead of it),
        // gets a bonus; more if a friendly pawn supports the square.
        Bitboard bishops = board.piece_bb(us, BISHOP);
        while (bishops) {
            Square sq = pop_lsb(bishops);
            int rel_rank = (us == WHITE) ? rank_of(sq) : 7 - rank_of(sq);
            if (rel_rank < 3 || rel_rank > 5) continue;
            int f = file_of(sq);
            Bitboard adj = adjacent_files_bb(f);
            Bitboard enemy_pawns_adj = (us == WHITE) ? (black_pawns & adj) : (white_pawns & adj);
            // Enemy pawns strictly ahead (from their perspective) can attack
            // the square; pawns behind/beside cannot. Mask by shifts (no loop).
            Bitboard forward_mask;
            if (us == WHITE) {
                int r = rank_of(sq);
                forward_mask = (r >= 7) ? 0ULL : ~((1ULL << (8 * (r + 1))) - 1);
            } else {
                forward_mask = ((1ULL << (8 * rank_of(sq))) - 1);
            }
            if (enemy_pawns_adj & forward_mask) continue;  // not safe
            // Supported if a friendly pawn defends the square (same trick as
            // the knight outpost: pawn_attack(~us, sq) gives the squares a
            // friendly pawn would defend from).
            Bitboard support_mask = pawn_attack(~us, sq);
            if (our_pawns & support_mask) c.boutpost_supp += s;
            else                          c.boutpost_reach += s;
        }

        // ---- Passed-pawn king proximity + refinements. For each of our
        // passed pawns: king distances (own penalized, enemy rewarded, tapered
        // to the endgame), blockade (enemy piece on the stop square), free path
        // (all squares ahead empty), and protection (defended by a friendly
        // pawn). All masks/fills, no per-square loops.
        Bitboard their_pawns = (us == WHITE) ? black_pawns : white_pawns;
        Square our_king   = board.king_square(us);
        Square their_king = board.king_square(~us);
        Bitboard enemy_minors = board.piece_bb(~us, KNIGHT) | board.piece_bb(~us, BISHOP);
        Bitboard enemy_majors = board.piece_bb(~us, ROOK)   | board.piece_bb(~us, QUEEN);
        Bitboard occ = board.occupancy();
        Bitboard pawns = our_pawns;
        while (pawns) {
            Square psq = pop_lsb(pawns);
            if (!is_passed_pawn(psq, us, their_pawns)) continue;
            c.passer_own_kdist   += s * chebyshev_distance(our_king, psq);
            c.passer_enemy_kdist += s * chebyshev_distance(their_king, psq);

            // Front span: all squares ahead of the pawn up to (and including)
            // the promotion square.
            Square stop = (us == WHITE) ? Square(psq + 8) : Square(psq - 8);
            Bitboard front_span = (us == WHITE) ? fill_north_bb(square_bb(stop))
                                                : fill_south_bb(square_bb(stop));
            // Blockade: enemy piece directly on the stop square.
            Bitboard stop_bb = square_bb(stop);
            if      (stop_bb & enemy_minors) c.passer_blocked_minor += s;
            else if (stop_bb & enemy_majors) c.passer_blocked_major += s;
            // Free path: nothing (either colour) on the whole front span.
            if (!(front_span & occ)) c.passer_free_path += s;
            // Protected: a friendly pawn defends the passer's square.
            if (pawn_attack(~us, psq) & our_pawns) c.passer_protected += s;
        }
    }
    return c;
}

static Score positional2(const Board& board, Color stm, int phase_mg,
                         int& mg_out, int& eg_out) {
    Positional2Counts c = positional2_counts(board, stm);
    const int* w = &eval_weights[POSITIONAL2_START];
    int mg = 0, eg = 0;
    mg += c.tempo          * w[W_POS2_TEMPO_MG];
    eg += c.tempo          * w[W_POS2_TEMPO_EG];
    mg += c.boutpost_reach * w[W_POS2_BOUTPOST_REACH_MG];
    eg += c.boutpost_reach * w[W_POS2_BOUTPOST_REACH_EG];
    mg += c.boutpost_supp  * w[W_POS2_BOUTPOST_SUPP_MG];
    eg += c.boutpost_supp  * w[W_POS2_BOUTPOST_SUPP_EG];
    mg += c.passer_own_kdist    * w[W_POS2_PASSER_OWN_KDIST_MG];
    eg += c.passer_own_kdist    * w[W_POS2_PASSER_OWN_KDIST_EG];
    mg += c.passer_enemy_kdist  * w[W_POS2_PASSER_ENEMY_KDIST_MG];
    eg += c.passer_enemy_kdist  * w[W_POS2_PASSER_ENEMY_KDIST_EG];
    mg += c.passer_blocked_minor * w[W_POS2_PASSER_BLOCKED_MINOR_MG];
    eg += c.passer_blocked_minor * w[W_POS2_PASSER_BLOCKED_MINOR_EG];
    mg += c.passer_blocked_major * w[W_POS2_PASSER_BLOCKED_MAJOR_MG];
    eg += c.passer_blocked_major * w[W_POS2_PASSER_BLOCKED_MAJOR_EG];
    mg += c.passer_free_path     * w[W_POS2_PASSER_FREE_PATH_MG];
    eg += c.passer_free_path     * w[W_POS2_PASSER_FREE_PATH_EG];
    mg += c.passer_protected     * w[W_POS2_PASSER_PROTECTED_MG];
    eg += c.passer_protected     * w[W_POS2_PASSER_PROTECTED_EG];
    mg_out = mg;
    eg_out = eg;
    return Score((mg * phase_mg + eg * (256 - phase_mg)) / 256);
}

// =============================================================================
// MOPUP EVALUATION
// =============================================================================
// In pawnless endings with a decisive material advantage, the PST king tables
// alone are insufficient to guide conversion -- the engine may wander without
// making progress. Mopup adds a bonus for two things:
//
//   1. Corner distance of the losing king: how far the weak king is from the
//      center. We want to maximize this -- a king in the corner has fewer
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
//     Below this threshold the position may be drawn -- mopup would
//     cause the engine to chase the opponent's king in a drawn endgame.
//
// This function is only called when both conditions are already verified
// by evaluate(). It does not re-check them internally.

// Returns the Manhattan distance from the central 2x2 region of the board.
// The central region in 0-based indices is files 3..4 (d and e) and ranks
// 3..4 (the 4th and 5th ranks in 1-based chess notation). A square inside
// that region returns 0; the farther it is, the larger the result, up to 6
// for the four corners.
static int center_distance(Square sq) {
    int file = file_of(sq);
    int rank = rank_of(sq);
    // For each axis, take the distance to the nearest central coordinate
    // (file 3 or 4 horizontally, rank 3 or 4 vertically). max(3-x, x-4) is
    // non-negative when x is outside [3,4] and may be negative when x is
    // inside; the std::max(_, 0) below clamps that to 0.
    int fd = std::max(3 - file, file - 4);
    int rd = std::max(3 - rank, rank - 4);
    return std::max(fd, 0) + std::max(rd, 0);
}

// Returns the Manhattan distance between two squares (sum of file and rank
// differences). Used as a king-to-king distance proxy in mopup.
static int king_distance(Square a, Square b) {
    return std::abs(file_of(a) - file_of(b))
         + std::abs(rank_of(a) - rank_of(b));
}

// Returns the mopup bonus in centipawns from White's perspective.
// strong_side: the side with the material advantage.
// weak_side:   the side being mated.
static Score mopup_eval(const Board& board, Color strong_side) {
    Color  weak_side    = ~strong_side;

    // Insufficient material guard: certain combinations are theoretical draws
    // -- no sequence of moves can force checkmate against optimal defense.
    // Without this guard, the strong side's material would exceed
    // MOPUP_THRESHOLD (300cp), corner-chasing would activate, and the engine
    // would refuse draws to wander indefinitely chasing the opposing king.
    //
    // Drawn combinations covered:
    //   K + B   vs K          (lone bishop, 330cp)
    //   K + N   vs K          (lone knight, 320cp)
    //   K + N+N vs K          (two knights, 640cp -- drawn against optimal play)
    //   K + B+B vs K          ONLY when both bishops occupy same-colored squares
    //                         (660cp -- drawn; opposite-colored bishops do mate)
    Bitboard strong_pieces = board.all_pieces(strong_side);
    int strong_count = popcount(strong_pieces);  // includes king

    if (strong_count == 2) {
        // King + one piece. Drawn if that piece is a lone bishop or knight.
        if (board.piece_bb(strong_side, BISHOP) || board.piece_bb(strong_side, KNIGHT))
            return 0;
    }
    else if (strong_count == 3) {
        // King + two pieces. Check the two specific drawn combinations.
        Bitboard knights = board.piece_bb(strong_side, KNIGHT);
        Bitboard bishops = board.piece_bb(strong_side, BISHOP);
        Bitboard rooks   = board.piece_bb(strong_side, ROOK);
        Bitboard queens  = board.piece_bb(strong_side, QUEEN);

        // K + N + N: drawn (two knights cannot force mate against a lone king).
        if (popcount(knights) == 2 && bishops == 0 && rooks == 0 && queens == 0)
            return 0;

        // K + B + B: drawn ONLY if both bishops are on same-colored squares.
        // Two same-color bishops can only attack squares of one color and
        // cannot force the lone king into a mating net. Opposite-colored
        // bishops DO mate (a known elementary endgame), so we must distinguish.
        if (popcount(bishops) == 2 && knights == 0 && rooks == 0 && queens == 0) {
            // Get both bishop squares and check if they share square color.
            // Square color parity: (file + rank) & 1 -- 0 = one color, 1 = the other.
            Bitboard b_copy = bishops;
            Square s1 = pop_lsb(b_copy);
            Square s2 = pop_lsb(b_copy);
            int parity1 = (file_of(s1) + rank_of(s1)) & 1;
            int parity2 = (file_of(s2) + rank_of(s2)) & 1;
            if (parity1 == parity2)
                return 0;  // Same color -- drawn
            // Opposite colors -- fall through to normal mopup (this IS a win).
        }
    }

    Square strong_king  = board.king_square(strong_side);
    Square weak_king    = board.king_square(weak_side);

    // The mopup bonus has two additive terms:
    //   1. Drive the weak king toward the corner: bonus grows with how far
    //      the weak king is from the centre (0 if centred, max 6 in a corner).
    //   2. Bring the strong king close: bonus grows as the kings get nearer
    //      (king_distance is Manhattan; subtracting from 14 inverts the sign
    //      so closer = higher).
    // These together encode the standard mating procedure: corral the lone
    // king into a corner using both kings working as a team.
    int bonus = MOPUP_CORNER_WEIGHT   *  center_distance(weak_king)
              + MOPUP_PROXIMITY_WEIGHT * (14 - king_distance(strong_king, weak_king));

    // Return from White's perspective: positive if White is the strong side,
    // negative if Black is. evaluate() handles the final flip for side to move.
    return (strong_side == WHITE) ? Score(bonus) : Score(-bonus);
}

// =============================================================================
// PAWN STRUCTURE EVALUATION
// =============================================================================
// Evaluates pawn structure for both sides and returns the score from White's
// perspective, blended by game phase. All terms use bitboard operations --
// no per-square loops.
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
//   are mobile and mutually defending -- harder to attack than isolated ones.
//   Small bonus per connected pawn.
//
// All five terms are tapered. Each constant exists in both MG and EG forms
// (see eval.h) and PASSED_BONUS has parallel MG/EG arrays (below). The
// function takes the current phase_mg as a parameter and returns the blended
// score in centipawns, from White's perspective.
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

// Note on the passed-pawn bonus table. Its per-rank values live as 16
// entries of the central eval_weights[] array starting at PASSED_BONUS_START
// (8 ranks * 2 phases, MG and EG interleaved). pawn_structure() reads them
// directly. The first two ranks (0, 1) and rank 7 are always 0 because a
// passed pawn cannot exist on those ranks (rank 1 for white means White's
// starting rank, rank 7 from White's perspective is the promotion rank for
// White -- a pawn there has already promoted, so it is no longer indexed).
// Tuning will likely raise the EG values relative to the MG values, since a
// passed pawn in the endgame is often decisive while in a piece-heavy
// middlegame it may be blockaded or attacked before it queens.

static Score pawn_structure(const Board& board, int phase_mg) {
    // Running totals for the two phase poles. mg_score accumulates the
    // middlegame contribution, eg_score the endgame contribution; both are
    // tracked from White's perspective (White additions are positive, Black
    // additions are negative). At the end of the function a single integer
    // division blends them by phase_mg.
    //
    // Why two accumulators instead of one per side. We use the "blend of the
    // difference" convention:
    //
    //   blend((wMG - bMG), (wEG - bEG))     [Option B]
    //
    // rather than the alternative "difference of the blends":
    //
    //   blend(wMG, wEG) - blend(bMG, bEG)   [Option A]
    //
    // With MG == EG (the initial state of 1.6) the two give identical
    // results. With MG != EG they differ by up to ~1 cp from integer
    // truncation. We use Option B for two reasons: (1) it does a single
    // integer division instead of two, and (2) it matches the convention
    // used by external evaluation tooling that consumes per-position MG and
    // EG totals separately and blends them at the very end. Producing the
    // engine's score under the same convention keeps internal and external
    // values bit-identical for any given position, which is essential when
    // diagnosing discrepancies.
    int mg_score = 0;
    int eg_score = 0;

    Bitboard white_pawns = board.piece_bb(WHITE, PAWN);
    Bitboard black_pawns = board.piece_bb(BLACK, PAWN);

    // ------------------------------------------------------------------------
    // Precompute "attack spans" for both sides.
    //
    // A pawn's attack span is the set of squares it (or its descendants on
    // the same file structure) could ever attack on the way to promotion.
    // For a white pawn on e4 that means d5, d6, ..., d8 and f5, f6, ..., f8:
    // the diagonal capture squares NE/NW from the pawn, swept forward.
    //
    // We need attack spans for two terms in this function:
    //   (1) PASSED detection -- a passed pawn has no enemy pawn in its front
    //       file or its attack span (the enemy can never capture it on its way).
    //   (2) BACKWARD detection -- a backward pawn is one whose own square is
    //       NOT inside its side's attack span (no friendly pawn behind on an
    //       adjacent file could push to defend it).
    //
    // We build a span by computing pawn attacks one step forward, then
    // flooding that bitboard further ahead through all remaining ranks.
    // ------------------------------------------------------------------------

    // Flood-fill helpers: propagate a bitboard north or south through every
    // rank. Doubling each shift (8 -> 16 -> 32) achieves the full board height
    // in three OR-shift steps, since 8 + 16 + 32 = 56 = 7 rank shifts max.
    auto fill_north = [](Bitboard b) -> Bitboard {
        b |= b << 8; b |= b << 16; b |= b << 32;
        return b;
    };
    auto fill_south = [](Bitboard b) -> Bitboard {
        b |= b >> 8; b |= b >> 16; b |= b >> 32;
        return b;
    };

    // Full forward attack span for each side: start from one-step pawn
    // attacks, then flood toward the promotion edge.
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
        // Sign carries the side: White contributions add to the mg/eg
        // accumulators with +1, Black contributions subtract them with -1.
        // This keeps the running totals as "White minus Black" throughout,
        // which is what the final blend formula expects.
        int sign = (c == WHITE) ? +1 : -1;

        // --- ISOLATED and DOUBLED: loop over files ---
        for (int f = 0; f < 8; f++) {
            Bitboard pawns_on_file = our_pawns & file_bb(f);
            if (!pawns_on_file) continue;

            int count = popcount(pawns_on_file);

            // Doubled: more than one pawn on the file.
            if (count > 1) {
                mg_score += sign * eval_weights[PAWN_STRUCT_START + W_PAWN_DOUBLED_MG] * (count - 1);
                eg_score += sign * eval_weights[PAWN_STRUCT_START + W_PAWN_DOUBLED_EG] * (count - 1);
            }

            // Isolated: no friendly pawns on adjacent files.
            if (!(our_pawns & adjacent_files_bb(f))) {
                mg_score += sign * eval_weights[PAWN_STRUCT_START + W_PAWN_ISOLATED_MG] * count;
                eg_score += sign * eval_weights[PAWN_STRUCT_START + W_PAWN_ISOLATED_EG] * count;
            }
        }

        // --- BACKWARD, PASSED, CONNECTED: loop over individual pawns ---
        Bitboard pawns = our_pawns;
        while (pawns) {
            Square sq   = pop_lsb(pawns);
            int    rank = rank_of(sq);

            // "Stop square" = the square immediately in front of this pawn,
            // i.e. where it would move if it advanced one square. We test the
            // safety of this square in the backward-pawn check below.
            // For White, "in front" is +8 (one rank up); for Black, -8.
            Square stop = (c == WHITE) ? Square(sq + 8) : Square(sq - 8);

            // ----- PASSED -----
            // A pawn is passed if no enemy pawn can block or capture it on
            // its way to promotion. Concretely, the union of:
            //   - its front file from the stop square forward, and
            //   - the two adjacent files from the stop square forward
            // must contain no enemy pawns.
            //
            // We compute that union per-pawn by flooding the relevant single
            // square (stop) and the pawn's diagonal attacks to the promotion
            // edge. Direction depends on color.
            Bitboard this_front;
            Bitboard this_attack_forward;
            if (c == WHITE) {
                this_front          = fill_north(square_bb(stop));
                this_attack_forward = fill_north(pawn_attacks_bb(c, square_bb(sq)));
            } else {
                this_front          = fill_south(square_bb(stop));
                this_attack_forward = fill_south(pawn_attacks_bb(c, square_bb(sq)));
            }
            bool is_passed = !(their_pawns & (this_front | this_attack_forward));
            if (is_passed) {
                // Index the per-rank tables from the pawn's own perspective:
                // White uses its absolute rank, Black mirrors (7 - rank).
                int bonus_rank = (c == WHITE) ? rank : (7 - rank);
                // PASSED_BONUS lives in eval_weights as 8 ranks * 2 phases
                // interleaved (MG at base + rank*2 + 0, EG at +1).
                mg_score += sign * eval_weights[PASSED_BONUS_START + bonus_rank * 2 + 0];
                eg_score += sign * eval_weights[PASSED_BONUS_START + bonus_rank * 2 + 1];
            }

            // ----- BACKWARD -----
            // A pawn is backward when it is stuck: it cannot safely advance,
            // and no friendly pawn can come to its defense. Formally:
            //   1. The stop square is attacked by some enemy pawn (any advance
            //      walks into a capture).
            //   2. No friendly pawn could push to defend the stop square (i.e.
            //      this pawn lies on a square no friendly pawn attack-span
            //      reaches; meaning no pawn behind us on an adjacent file
            //      could ever push up to support).
            //
            // We approximate condition 2 with a bitboard test:
            //   our pawn at `sq` is supported-from-behind iff (square_bb(sq) &
            //   friendly_attack_span) is non-zero. If zero, no friendly pawn
            //   could ever defend us by advancing -- we are backward.
            //
            // We exclude pawns that are already classified as passed (a passed
            // pawn is a good thing even if it happens to be backward by this
            // definition) and pawns already counted as isolated (their isolation
            // penalty above covers the same weakness).
            Bitboard friendly_attack_span = (c == WHITE) ? white_attack_span
                                                         : black_attack_span;
            bool stop_attacked_by_enemy = (pawn_attacks_bb(c, square_bb(stop))
                                           & their_pawns) != 0;
            bool no_friendly_support    = !(square_bb(sq) & friendly_attack_span);
            if (stop_attacked_by_enemy && no_friendly_support && !is_passed) {
                mg_score += sign * eval_weights[PAWN_STRUCT_START + W_PAWN_BACKWARD_MG];
                eg_score += sign * eval_weights[PAWN_STRUCT_START + W_PAWN_BACKWARD_EG];
            }

            // ----- CONNECTED -----
            // A pawn is connected when a friendly pawn diagonally defends its
            // square. Trick: the squares from which a friendly pawn would
            // attack `sq` are exactly the squares that an enemy pawn standing
            // on `sq` would attack. So pawn_attacks_bb(~c, square_bb(sq))
            // gives those would-be defender squares; AND with our_pawns tells
            // us if any friendly pawn actually sits there.
            if (pawn_attacks_bb(~c, square_bb(sq)) & our_pawns) {
                mg_score += sign * eval_weights[PAWN_STRUCT_START + W_PAWN_CONNECTED_MG];
                eg_score += sign * eval_weights[PAWN_STRUCT_START + W_PAWN_CONNECTED_EG];
            }
        }
    }

    // Final blend (Option B): mg_score is the W-B middlegame total,
    // eg_score the W-B endgame total. phase_mg=256 yields the pure MG total,
    // phase_mg=0 the pure EG total, intermediate values a weighted mix.
    // With MG == EG (the initial state of 1.6) this collapses to the table
    // value algebraically: (v*p + v*(256-p))/256 = v.
    return Score((mg_score * phase_mg + eg_score * (256 - phase_mg)) / 256);
}

// =============================================================================
// POSITIONAL EVALUATION
// =============================================================================
// Returns positional bonuses beyond material and PSTs, from White's
// perspective, blended by game phase. Five terms:
//
// MOBILITY: each minor and major piece scores a small bonus per safe square
//   it can reach. "Safe" excludes squares occupied by our own pieces and
//   squares attacked by enemy pawns (which would let an enemy pawn capture
//   anything that lands there). Different piece types get different per-square
//   weights -- see MOBILITY_*_MG / MOBILITY_*_EG constants in eval.h.
//
// KNIGHT OUTPOSTS: a knight standing on a square in advanced enemy territory
//   that no enemy pawn can ever attack gets a bonus. Strength depends on
//   whether a friendly pawn currently defends the square.
//
// ROOK PLACEMENT: rooks score bonuses for sitting on (semi-)open files and
//   on the 7th rank.
//
// BISHOP PAIR: a one-time bonus when both bishops are still on the board.
//
// QUEEN ACTIVITY: only mobility for the queen -- placement is handled by the
//   queen PST.
//
// All terms are tapered. Each constant exists in MG and EG forms (eval.h).
// The function takes phase_mg as a parameter, accumulates running mg/eg
// totals across all five terms and both sides, and returns the phase-blended
// score in centipawns from White's perspective. We use the same "blend of
// the difference" convention (Option B) introduced in pawn_structure() -- a
// single integer division at the very end. With MG == EG (the initial state
// of 1.6) the blend collapses algebraically to the table value, so behavior
// is bit-identical to the pre-tapered version.
//
// All computations are bitboard-based and process both colors inside a single
// outer loop. The function is called once per evaluate() invocation.

static Score positional_eval(const Board& board, int phase_mg) {
    // Running mg/eg totals, from White's perspective. White contributions
    // add to the accumulators with sign +1, Black contributions with sign -1.
    // Same "blend-of-difference" (Option B) convention used in pawn_structure().
    int mg_score = 0;
    int eg_score = 0;

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
        // Sign carries the side: White adds to the running totals as +,
        // Black subtracts. Final result is (white - black) per phase.
        int sign = (us == WHITE) ? +1 : -1;

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
            mg_score += sign * eval_weights[MOBILITY_START + W_MOBILITY_KNIGHT_MG] * moves;
            eg_score += sign * eval_weights[MOBILITY_START + W_MOBILITY_KNIGHT_EG] * moves;

            // Knight outpost: on relative ranks 4-6 (advanced enemy territory),
            // not attackable by any enemy pawn now or in the future, with a
            // graduated bonus depending on whether a friendly pawn supports
            // (defends) the square.
            int rel_rank = (us == WHITE) ? rank_of(sq) : 7 - rank_of(sq);
            if (rel_rank >= 3 && rel_rank <= 5) {
                // Check if any enemy pawn on adjacent files can advance to attack this square.
                int f = file_of(sq);
                Bitboard adj = adjacent_files_bb(f);
                Bitboard enemy_pawns_adj = (us == WHITE) ? (black_pawns & adj) : (white_pawns & adj);

                // Enemy pawns behind, beside, or on the same rank as this square
                // cannot attack it. Only pawns STRICTLY AHEAD (from enemy's
                // perspective) matter, since pawns capture diagonally forward.
                //
                // White knight: only ranks STRICTLY > rank_of(sq) count.
                //   Edge case: if the knight is on rank 8 (rank_of=7), the
                //   shift would be 1ULL << 64 which is undefined behavior.
                //   Guard with explicit zero in that case (no enemy pawns
                //   above rank 8 exist anyway). Knights occasionally end up
                //   on rank 8 in endgames so the guard is real, not theoretical.
                // Black knight: only ranks STRICTLY < rank_of(sq) count.
                //   Edge case: rank 1 (rank_of=0) yields shift 0 which gives
                //   mask=0 (no enemy pawns can attack rank 1 anyway, since
                //   white pawns attack toward higher ranks). No guard needed.
                Bitboard forward_mask;
                if (us == WHITE) {
                    int r = rank_of(sq);
                    forward_mask = (r >= 7) ? 0ULL
                                            : ~((1ULL << (8 * (r + 1))) - 1);
                } else {
                    forward_mask = ((1ULL << (8 * rank_of(sq))) - 1);
                }

                if (!(enemy_pawns_adj & forward_mask)) {
                    // Square is safe from pawn attack. Now check if a friendly
                    // pawn supports the square (defends it from capture by an
                    // enemy minor piece).
                    //
                    // Trick: the squares from which a friendly pawn would
                    // defend `sq` are exactly the squares that an enemy pawn
                    // would attack from `sq`. We use pawn_attack(~us, sq) to
                    // get those squares, then AND with our pawns.
                    //
                    // Example: white knight on e5. Friendly pawns defending
                    // e5 sit on d4 and f4. Equivalently, a black pawn on e5
                    // would attack d4 and f4. So pawn_attack(BLACK, e5)
                    // returns the mask {d4, f4}, and (white_pawns & mask)
                    // tells us if any friendly pawn supports the knight.
                    Bitboard our_pawns    = (us == WHITE) ? white_pawns : black_pawns;
                    Bitboard support_mask = pawn_attack(~us, sq);

                    if (our_pawns & support_mask) {
                        mg_score += sign * eval_weights[KNIGHT_OUTPOST_START + W_KNIGHT_OUTPOST_SUPPORTED_MG];
                        eg_score += sign * eval_weights[KNIGHT_OUTPOST_START + W_KNIGHT_OUTPOST_SUPPORTED_EG];
                    } else {
                        mg_score += sign * eval_weights[KNIGHT_OUTPOST_START + W_KNIGHT_OUTPOST_REACHABLE_MG];
                        eg_score += sign * eval_weights[KNIGHT_OUTPOST_START + W_KNIGHT_OUTPOST_REACHABLE_EG];
                    }
                }
            }
        }

        // ----- BISHOP MOBILITY + PAIR -----
        Bitboard bishops = board.piece_bb(us, BISHOP);

        // Bishop pair: one-shot bonus when both bishops are still on the board.
        // The two bishops together control both color complexes, which the
        // sum of two single-bishop evaluations does not capture.
        if (popcount(bishops) >= 2) {
            mg_score += sign * eval_weights[BISHOP_PAIR_START + W_BISHOP_PAIR_MG];
            eg_score += sign * eval_weights[BISHOP_PAIR_START + W_BISHOP_PAIR_EG];
        }

        // Per-bishop mobility: count safe squares reachable along diagonals.
        while (bishops) {
            Square sq = pop_lsb(bishops);
            int moves = popcount(bishop_attack(sq, occ) & mob_area);
            mg_score += sign * eval_weights[MOBILITY_START + W_MOBILITY_BISHOP_MG] * moves;
            eg_score += sign * eval_weights[MOBILITY_START + W_MOBILITY_BISHOP_EG] * moves;
        }

        // ----- ROOK MOBILITY + FILES + 7TH RANK -----
        Bitboard rooks = board.piece_bb(us, ROOK);
        while (rooks) {
            Square sq = pop_lsb(rooks);
            int moves = popcount(rook_attack(sq, occ) & mob_area);
            mg_score += sign * eval_weights[MOBILITY_START + W_MOBILITY_ROOK_MG] * moves;
            eg_score += sign * eval_weights[MOBILITY_START + W_MOBILITY_ROOK_EG] * moves;

            // Open vs semi-open file. We split the file's pawn population into
            // ours and theirs:
            //   - no pawns of either color  -> open file        -> larger bonus
            //   - only enemy pawns          -> semi-open file   -> smaller bonus
            //   - friendly pawn(s) present  -> closed for us    -> no bonus
            Bitboard this_file = file_bb(file_of(sq));
            Bitboard our_pawns_on_file   = (us == WHITE) ? (white_pawns & this_file)
                                                         : (black_pawns & this_file);
            Bitboard their_pawns_on_file = (us == WHITE) ? (black_pawns & this_file)
                                                         : (white_pawns & this_file);
            if (!our_pawns_on_file) {
                if (!their_pawns_on_file) {
                    mg_score += sign * eval_weights[ROOK_BONUSES_START + W_ROOK_OPEN_FILE_MG];
                    eg_score += sign * eval_weights[ROOK_BONUSES_START + W_ROOK_OPEN_FILE_EG];
                } else {
                    mg_score += sign * eval_weights[ROOK_BONUSES_START + W_ROOK_SEMI_OPEN_FILE_MG];
                    eg_score += sign * eval_weights[ROOK_BONUSES_START + W_ROOK_SEMI_OPEN_FILE_EG];
                }
            }

            // Rook on the 7th rank (the rank where the enemy starts its pawns).
            // For White that is absolute rank 7 (0-indexed 6); for Black it is
            // absolute rank 2 (0-indexed 1), which mirrors to 6 in our relative
            // coordinates.
            int rel_rank_r = (us == WHITE) ? rank_of(sq) : 7 - rank_of(sq);
            if (rel_rank_r == 6) {
                mg_score += sign * eval_weights[ROOK_BONUSES_START + W_ROOK_ON_7TH_MG];
                eg_score += sign * eval_weights[ROOK_BONUSES_START + W_ROOK_ON_7TH_EG];
            }
        }

        // ----- QUEEN MOBILITY -----
        // The queen has only a mobility term here; its placement is handled by
        // the queen PST and its position-driven value is dominated by raw
        // material (900 cp) anyway. We compute the attack set as the union of
        // rook-style and bishop-style attacks (the two halves of queen movement).
        Bitboard queens = board.piece_bb(us, QUEEN);
        while (queens) {
            Square sq = pop_lsb(queens);
            Bitboard attacks = bishop_attack(sq, occ) | rook_attack(sq, occ);
            int moves = popcount(attacks & mob_area);
            mg_score += sign * eval_weights[MOBILITY_START + W_MOBILITY_QUEEN_MG] * moves;
            eg_score += sign * eval_weights[MOBILITY_START + W_MOBILITY_QUEEN_EG] * moves;
        }
    }

    // Final blend (Option B): mg_score is the W-B middlegame total,
    // eg_score the W-B endgame total. phase_mg=256 yields the pure MG total,
    // phase_mg=0 the pure EG total, intermediate values a weighted mix.
    // With MG == EG (the initial state of 1.6) this collapses to a single
    // value algebraically: (v*p + v*(256-p))/256 = v.
    return Score((mg_score * phase_mg + eg_score * (256 - phase_mg)) / 256);
}

// =============================================================================
// MAIN EVALUATION FUNCTION
// =============================================================================
// Returns a static evaluation score for `board`, in centipawns, from the
// side-to-move's perspective (positive = good for side to move). This is the
// function called by search at leaf nodes and wherever else a static eval is
// needed. It is in the hot path -- millions of calls per second during search
// -- so per-piece work is kept tight (bitboard ops, no allocations, no virtual
// dispatch).
//
// Computation order:
//   1. game_phase()         -- determines MG/EG blend weight.
//   2. material + PST loop  -- accumulates raw mg_score and eg_score
//                              totals (Option B), one blend at the end.
//   3. king_safety()        -- one call per side, returns blended centipawns.
//   4. pawn_structure()     -- one call, returns symmetric W-B blended score.
//   5. positional_eval()    -- one call, returns symmetric W-B blended score.
//   6. mopup_eval()         -- only in pawnless decisive endings.
//   7. Sign-flip for side to move.

Score evaluate(const Board& board) {
    int phase_mg = game_phase(board);

    // ---- Material + PST: accumulate raw MG and EG totals separately.
    //
    // mg_score and eg_score hold the running White-minus-Black totals for
    // the middlegame and endgame poles respectively. For each piece we add
    // its material value (phase-independent, so it goes into BOTH mg and eg)
    // plus the appropriate raw PST entry (MG into mg_score, EG into
    // eg_score). White contributions are added with sign +1, Black with -1.
    //
    // Then a single blend at the very end (after king safety, pawn
    // structure, and positional eval are added) collapses the two poles
    // into one score. This is the same "blend-of-totals" (Option B) shape
    // used by pawn_structure() and positional_eval(), so the entire eval
    // now lives in a single blend.
    int mg_score = 0;
    int eg_score = 0;

    for (int pt = PAWN; pt <= KING; pt++) {
        PieceType piece_type = PieceType(pt);

        // White pieces: add material + raw MG entry to mg_score,
        // and material + raw EG entry to eg_score.
        Bitboard wb = board.piece_bb(WHITE, piece_type);
        while (wb) {
            Square sq = pop_lsb(wb);
            mg_score += PIECE_VALUE[pt] + pst_mg_value(piece_type, WHITE, sq);
            eg_score += PIECE_VALUE[pt] + pst_eg_value(piece_type, WHITE, sq);
        }

        // Black pieces: same shape, subtracted instead of added.
        Bitboard bb = board.piece_bb(BLACK, piece_type);
        while (bb) {
            Square sq = pop_lsb(bb);
            mg_score -= PIECE_VALUE[pt] + pst_mg_value(piece_type, BLACK, sq);
            eg_score -= PIECE_VALUE[pt] + pst_eg_value(piece_type, BLACK, sq);
        }
    }

    // ---- King safety: fold the raw (un-blended) MG and EG penalties into the
    // running mg_score / eg_score totals BEFORE the single blend below. White's
    // king being attacked is a penalty to White (subtract its raw penalty,
    // which is negative, i.e. add it); Black's is a penalty to Black (negate).
    // Accumulating raw and blending once -- rather than blending each king's
    // penalty separately -- keeps evaluate() bit-identical to score_from_trace()
    // (same number of integer divisions, no per-term truncation drift).
    int ks_w_mg, ks_w_eg, ks_b_mg, ks_b_eg;
    king_safety(board, WHITE, phase_mg, ks_w_mg, ks_w_eg);
    king_safety(board, BLACK, phase_mg, ks_b_mg, ks_b_eg);
    mg_score += ks_w_mg - ks_b_mg;
    eg_score += ks_w_eg - ks_b_eg;

    // ---- Piece tropism: same pattern as king safety. White's pieces near the
    // black king is a bonus for White (add White's raw, subtract Black's), all
    // folded into the single blend below for bit-exact agreement with the trace.
    int tr_w_mg, tr_w_eg, tr_b_mg, tr_b_eg;
    tropism(board, WHITE, phase_mg, tr_w_mg, tr_w_eg);
    tropism(board, BLACK, phase_mg, tr_b_mg, tr_b_eg);
    mg_score += tr_w_mg - tr_b_mg;
    eg_score += tr_w_eg - tr_b_eg;

    // ---- Pawn shelter/storm: same pattern. Good shelter / dangerous storm is
    // scored per king from White's perspective (add White's raw, subtract
    // Black's), folded into the single blend below for bit-exact agreement
    // with the trace.
    int ss_w_mg, ss_w_eg, ss_b_mg, ss_b_eg;
    shelter_storm(board, WHITE, phase_mg, ss_w_mg, ss_w_eg);
    shelter_storm(board, BLACK, phase_mg, ss_b_mg, ss_b_eg);
    mg_score += ss_w_mg - ss_b_mg;
    eg_score += ss_w_eg - ss_b_eg;

    // ---- King safety v2: open files toward the king + safe checks. Same
    // pattern -- penalties scored per king from White's perspective, folded
    // raw into the single blend for bit-exact agreement with the trace.
    int k2_w_mg, k2_w_eg, k2_b_mg, k2_b_eg;
    king_safety_v2(board, WHITE, phase_mg, k2_w_mg, k2_w_eg);
    king_safety_v2(board, BLACK, phase_mg, k2_b_mg, k2_b_eg);
    mg_score += k2_w_mg - k2_b_mg;
    eg_score += k2_w_eg - k2_b_eg;

    // ---- Blend material + PST + king safety (+ v2) + tropism + shelter/storm
    // to a single score in centipawns. (v*p + v*(256-p))/256 = v when mg==eg.
    Score score = Score((mg_score * phase_mg + eg_score * (256 - phase_mg)) / 256);

    // ---- Pawn structure: already returned from White's perspective, add directly.
    // Takes phase_mg because pawn-structure weights are tapered (since 1.6).
    score += pawn_structure(board, phase_mg);

    // ---- Positional eval (mobility, files, outposts, etc.): same convention.
    // Tapered since 1.6 -- takes phase_mg like pawn_structure.
    score += positional_eval(board, phase_mg);

    // ---- Positional batch (1.6): tempo, bishop outpost, and passed-pawn
    // refinement. Returns a blended White-perspective score (tempo carries the
    // side-to-move sign internally, which survives the final sign-flip below).
    // Added before mopup.
    int p2_mg, p2_eg;
    score += positional2(board, board.side_to_move, phase_mg, p2_mg, p2_eg);

    // ---- Mopup: only in pawnless endings where one side has a decisive
    // material edge. The threshold prevents corner-chasing in unclear or
    // drawn positions; mopup_eval() also has its own internal guard for
    // specific drawn material combinations (KB vs K, KN vs K, etc.).
    bool no_pawns = board.piece_bb(PAWN) == 0;
    if (no_pawns && std::abs(score) >= MOPUP_THRESHOLD) {
        Color strong_side = (score > 0) ? WHITE : BLACK;
        score += mopup_eval(board, strong_side);
    }

    // ---- Final sign-flip: search expects "good for side to move" sign.
    return (board.side_to_move == WHITE) ? score : -score;
}

// =============================================================================
// VERBOSE EVALUATION (debug command)
// =============================================================================
// Prints a per-component breakdown of evaluate() to stdout. Reproduces the
// exact logic of evaluate() but accumulates each component separately so the
// caller can see which terms contribute to the final score. Output format
// uses cp units (centipawns) and is from White's perspective until the final
// "Total (side to move)" line, which is flipped if Black is to move.
//
// Used by the UCI "eval" command for debugging evaluation changes. Not in
// the search hot path -- runs once per command invocation.

// =============================================================================
// EVAL TRACE
// =============================================================================
// trace_evaluate(board, trace) walks the same code as evaluate() but, instead
// of multiplying counts by weights and accumulating a score, it counts how
// many times each eval_weights[] slot would have been added. White
// contributions are recorded positively, Black contributions negatively. The
// resulting coefficient vector, together with the phase_mg used by the blend
// and an additional_score scalar covering the non-tuned terms (mopup, king
// safety, and material -- material is phase-independent and stays out of
// the tunable array), is enough for an external consumer to reconstruct
// evaluate(board) bit-for-bit (see score_from_trace below) and to recompute
// the score under hypothetical alternate weights.
//
// What goes where in EvalTrace:
//
//   coefficients[i]   = signed count for eval_weights[i]
//                       (white_count - black_count, summed across the position)
//   phase_mg          = the blend weight evaluate() used for this position
//   additional_score  = material + mopup (from White's perspective). King
//                       safety USED to live here too, but in 1.6 it became a
//                       tunable group and now contributes via coefficients[]
//                       like every other tuned term. Material stays here
//                       (phase-independent, deliberately not tuned).
//
// To reconstruct, see score_from_trace().
//
// We implement trace_evaluate as a copy of evaluate() with the score updates
// replaced by coefficient updates. We deliberately do NOT call the helpers
// pawn_structure() / positional_eval() etc., because those functions return
// blended scores; we need the raw counts before any blend.

// Helper: bump the MG and EG slots of a feature by `delta`. Used heavily
// in the trace walk. `mg_idx` is the absolute index in coefficients[] of
// the MG slot; the EG slot is assumed to be the next index (this matches
// the layout used by every group in eval_weights[]).
static inline void bump_mg_eg(EvalTrace& trace, int mg_idx, int delta) {
    trace.coefficients[mg_idx + 0] += delta;
    trace.coefficients[mg_idx + 1] += delta;
}

void trace_evaluate(const Board& board, EvalTrace& trace) {
    // Zero the trace before we start. The coefficient vector is dense, but
    // most entries will end up zero in any given position; we still need to
    // initialise to a known state so the consumer can sum cleanly.
    for (int i = 0; i < NUM_WEIGHTS; i++) trace.coefficients[i] = 0;
    trace.additional_score = 0;
    trace.phase_mg         = game_phase(board);
    int phase_mg           = trace.phase_mg;

    // ---- Material + PST loop. Material contributes to additional_score
    // (phase-independent, not tunable). PSTs contribute to coefficients
    // (one slot per square per phase). White adds with +1, Black with -1.
    for (int pt = PAWN; pt <= KING; pt++) {
        PieceType piece_type = PieceType(pt);

        Bitboard wb = board.piece_bb(WHITE, piece_type);
        while (wb) {
            Square sq = pop_lsb(wb);
            trace.additional_score += PIECE_VALUE[pt];
            // PST slots: see the per-piece base offsets in eval.h.
            // The mirror for Black is handled below by inverting `sq`
            // through (sq ^ 56) when computing idx.
            int idx = sq;  // White uses the raw square index.
            int mg_slot;
            switch (pt) {
                case PAWN:   mg_slot = PST_PAWN_START   + idx * 2; break;
                case KNIGHT: mg_slot = PST_KNIGHT_START + idx * 2; break;
                case BISHOP: mg_slot = PST_BISHOP_START + idx * 2; break;
                case ROOK:   mg_slot = PST_ROOK_START   + idx * 2; break;
                case QUEEN:  mg_slot = PST_QUEEN_START  + idx * 2; break;
                case KING:   mg_slot = PST_KING_START   + idx * 2; break;
                default: continue;
            }
            bump_mg_eg(trace, mg_slot, +1);
        }

        Bitboard bb = board.piece_bb(BLACK, piece_type);
        while (bb) {
            Square sq = pop_lsb(bb);
            trace.additional_score -= PIECE_VALUE[pt];
            int idx = sq ^ 56;  // Mirror for Black.
            int mg_slot;
            switch (pt) {
                case PAWN:   mg_slot = PST_PAWN_START   + idx * 2; break;
                case KNIGHT: mg_slot = PST_KNIGHT_START + idx * 2; break;
                case BISHOP: mg_slot = PST_BISHOP_START + idx * 2; break;
                case ROOK:   mg_slot = PST_ROOK_START   + idx * 2; break;
                case QUEEN:  mg_slot = PST_QUEEN_START  + idx * 2; break;
                case KING:   mg_slot = PST_KING_START   + idx * 2; break;
                default: continue;
            }
            bump_mg_eg(trace, mg_slot, -1);
        }
    }

    // ---- King safety: tunable in 1.6. Instead of folding a fixed penalty
    // into additional_score, we record coefficients for the KING_SAFETY group
    // so the tuner can optimise the per-attacker-type weights and the
    // attacker-count buckets. We MUST use the same counting routine king_safety()
    // uses (king_zone_attackers) so the traced coefficients reproduce the score
    // exactly.
    //
    // SIGN CONVENTION (subtle -- verified against evaluate()). The king-safety
    // weights are stored NEGATIVE (a penalty). evaluate() folds White's raw
    // penalty into mg_score with a +1 multiplier (mg_score += ks_w_mg, which is
    // negative, lowering White's score) and Black's with -1 (mg_score -= ks_b_mg,
    // raising the net for White). score_from_trace multiplies coefficient by
    // weight, so to match evaluate the coefficient for an attack on WHITE's king
    // must be +1 (giving +1 * negative_weight = the same negative contribution)
    // and for an attack on BLACK's king must be -1. In other words the sign is
    // +1 when `us` is White and -1 when `us` is Black -- the OPPOSITE of a naive
    // "penalty is negative so use -1" reading.
    for (Color us : {WHITE, BLACK}) {
        KingZoneAttackers a = king_zone_attackers(board, us);
        if (a.total == 0) continue;
        int sign = (us == WHITE) ? +1 : -1;

        // (a) Per-attacker-type counts.
        bump_mg_eg(trace, KING_SAFETY_START + W_KS_ATTACKER_KNIGHT_MG, sign * a.knight);
        bump_mg_eg(trace, KING_SAFETY_START + W_KS_ATTACKER_BISHOP_MG, sign * a.bishop);
        bump_mg_eg(trace, KING_SAFETY_START + W_KS_ATTACKER_ROOK_MG,   sign * a.rook);
        bump_mg_eg(trace, KING_SAFETY_START + W_KS_ATTACKER_QUEEN_MG,  sign * a.queen);

        // (b) Attacker-count bucket: exactly one bucket gets +/-1.
        const int bucket = count_bucket_offset(a.total);
        bump_mg_eg(trace, KING_SAFETY_START + bucket, sign);
    }

    // ---- Piece tropism: record the (piece, distance-bucket) counts so the
    // tuner optimises the tropism weights. Same shared counting routine
    // tropism() uses (tropism_counts), so the coefficients reproduce the score
    // exactly. Tropism is a bonus to `us` (pieces near the enemy king), and
    // evaluate() folds White's with +1 and Black's with -1; since the weights
    // are positive, the matching coefficient sign is +1 for White, -1 for Black.
    for (Color us : {WHITE, BLACK}) {
        TropismCounts tc = tropism_counts(board, us);
        int sign = (us == WHITE) ? +1 : -1;
        for (int pi = 0; pi < 4; pi++) {
            const int base = pi * 8;
            for (int b = 0; b < 4; b++) {
                const int n = tc.c[pi][b];
                if (n) bump_mg_eg(trace, TROPISM_START + base + b * 2, sign * n);
            }
        }
    }

    // ---- Pawn shelter/storm: record the (category, bucket) counts so the
    // tuner optimises the shelter and storm weights. Same shared counting
    // routine shelter_storm() uses (shelter_storm_counts), so the coefficients
    // reproduce the score exactly. Like tropism, evaluate() folds White's
    // contribution with +1 and Black's with -1, so the matching coefficient
    // sign is +1 for White and -1 for Black (the weights are free-signed:
    // shelter likely positive, storm likely negative -- the tuner decides).
    for (Color us : {WHITE, BLACK}) {
        ShelterStormCounts sc = shelter_storm_counts(board, us);
        int sign = (us == WHITE) ? +1 : -1;
        // SHELTER: king file (base 0) then adjacent (base 8).
        for (int cat = 0; cat < 2; cat++) {
            const int base = (cat == 0) ? W_SS_SHELTER_KFILE_D1_MG
                                         : W_SS_SHELTER_AFILE_D1_MG;
            for (int b = 0; b < 4; b++) {
                const int n = sc.shelter[cat][b];
                if (n) bump_mg_eg(trace, SHELTER_STORM_START + base + b * 2, sign * n);
            }
        }
        // STORM: king file (base 16) then adjacent (base 24).
        for (int cat = 0; cat < 2; cat++) {
            const int base = (cat == 0) ? W_SS_STORM_KFILE_D1_MG
                                         : W_SS_STORM_AFILE_D1_MG;
            for (int b = 0; b < 4; b++) {
                const int n = sc.storm[cat][b];
                if (n) bump_mg_eg(trace, SHELTER_STORM_START + base + b * 2, sign * n);
            }
        }
    }

    // ---- King safety v2: open files + safe checks. Same shared counting
    // routine king_safety_v2() uses, so coefficients reproduce the score. Sign
    // is +1 for White and -1 for Black (penalties, free-signed weights).
    for (Color us : {WHITE, BLACK}) {
        KingSafetyV2Counts c = king_safety_v2_counts(board, us);
        int sign = (us == WHITE) ? +1 : -1;
        const int B = KING_SAFETY_V2_START;
        if (c.open_king) bump_mg_eg(trace, B + W_KSV2_OPENFILE_KING_MG, sign * c.open_king);
        if (c.open_adj)  bump_mg_eg(trace, B + W_KSV2_OPENFILE_ADJ_MG,  sign * c.open_adj);
        if (c.semi_king) bump_mg_eg(trace, B + W_KSV2_SEMIFILE_KING_MG, sign * c.semi_king);
        if (c.semi_adj)  bump_mg_eg(trace, B + W_KSV2_SEMIFILE_ADJ_MG,  sign * c.semi_adj);
        if (c.safe_n)    bump_mg_eg(trace, B + W_KSV2_SAFECHECK_N_MG,    sign * c.safe_n);
        if (c.safe_b)    bump_mg_eg(trace, B + W_KSV2_SAFECHECK_B_MG,    sign * c.safe_b);
        if (c.safe_r)    bump_mg_eg(trace, B + W_KSV2_SAFECHECK_R_MG,    sign * c.safe_r);
        if (c.safe_q)    bump_mg_eg(trace, B + W_KSV2_SAFECHECK_Q_MG,    sign * c.safe_q);
    }

    // ---- Positional batch (1.6): tempo carries the side-to-move sign in
    // perspective-of-White, matching positional2()/evaluate(); it survives the
    // final flip in score_from_trace().
    {
        Positional2Counts c = positional2_counts(board, board.side_to_move);
        if (c.tempo) bump_mg_eg(trace, POSITIONAL2_START + W_POS2_TEMPO_MG, c.tempo);
        if (c.boutpost_reach) bump_mg_eg(trace, POSITIONAL2_START + W_POS2_BOUTPOST_REACH_MG, c.boutpost_reach);
        if (c.boutpost_supp)  bump_mg_eg(trace, POSITIONAL2_START + W_POS2_BOUTPOST_SUPP_MG,  c.boutpost_supp);
        if (c.passer_own_kdist)   bump_mg_eg(trace, POSITIONAL2_START + W_POS2_PASSER_OWN_KDIST_MG,   c.passer_own_kdist);
        if (c.passer_enemy_kdist) bump_mg_eg(trace, POSITIONAL2_START + W_POS2_PASSER_ENEMY_KDIST_MG, c.passer_enemy_kdist);
        if (c.passer_blocked_minor) bump_mg_eg(trace, POSITIONAL2_START + W_POS2_PASSER_BLOCKED_MINOR_MG, c.passer_blocked_minor);
        if (c.passer_blocked_major) bump_mg_eg(trace, POSITIONAL2_START + W_POS2_PASSER_BLOCKED_MAJOR_MG, c.passer_blocked_major);
        if (c.passer_free_path)     bump_mg_eg(trace, POSITIONAL2_START + W_POS2_PASSER_FREE_PATH_MG,     c.passer_free_path);
        if (c.passer_protected)     bump_mg_eg(trace, POSITIONAL2_START + W_POS2_PASSER_PROTECTED_MG,     c.passer_protected);
    }

    // ---- Pawn structure: walk the same logic as pawn_structure() but
    // record counts instead of multiplying by weights. We do NOT call
    // pawn_structure() because it returns a phase-blended score; we need
    // the raw counts before the blend.

    Bitboard white_pawns = board.piece_bb(WHITE, PAWN);
    Bitboard black_pawns = board.piece_bb(BLACK, PAWN);

    // Precompute attack spans (same as in pawn_structure()).
    auto fill_north = [](Bitboard b) -> Bitboard {
        b |= b << 8; b |= b << 16; b |= b << 32;
        return b;
    };
    auto fill_south = [](Bitboard b) -> Bitboard {
        b |= b >> 8; b |= b >> 16; b |= b >> 32;
        return b;
    };
    Bitboard white_attack_span = fill_north(pawn_attacks_bb(WHITE, white_pawns));
    Bitboard black_attack_span = fill_south(pawn_attacks_bb(BLACK, black_pawns));

    for (Color c : {WHITE, BLACK}) {
        Bitboard our_pawns   = (c == WHITE) ? white_pawns : black_pawns;
        Bitboard their_pawns = (c == WHITE) ? black_pawns : white_pawns;
        int sign = (c == WHITE) ? +1 : -1;

        // --- ISOLATED and DOUBLED: loop over files ---
        for (int f = 0; f < 8; f++) {
            Bitboard pawns_on_file = our_pawns & file_bb(f);
            if (!pawns_on_file) continue;
            int count = popcount(pawns_on_file);

            if (count > 1) {
                bump_mg_eg(trace, PAWN_STRUCT_START + W_PAWN_DOUBLED_MG,
                           sign * (count - 1));
            }
            if (!(our_pawns & adjacent_files_bb(f))) {
                bump_mg_eg(trace, PAWN_STRUCT_START + W_PAWN_ISOLATED_MG,
                           sign * count);
            }
        }

        // --- BACKWARD, PASSED, CONNECTED: per-pawn loop ---
        Bitboard pawns = our_pawns;
        while (pawns) {
            Square sq   = pop_lsb(pawns);
            int    rank = rank_of(sq);
            Square stop = (c == WHITE) ? Square(sq + 8) : Square(sq - 8);

            // PASSED
            Bitboard this_front;
            Bitboard this_attack_forward;
            if (c == WHITE) {
                this_front          = fill_north(square_bb(stop));
                this_attack_forward = fill_north(pawn_attacks_bb(c, square_bb(sq)));
            } else {
                this_front          = fill_south(square_bb(stop));
                this_attack_forward = fill_south(pawn_attacks_bb(c, square_bb(sq)));
            }
            bool is_passed = !(their_pawns & (this_front | this_attack_forward));
            if (is_passed) {
                int bonus_rank = (c == WHITE) ? rank : (7 - rank);
                bump_mg_eg(trace, PASSED_BONUS_START + bonus_rank * 2, sign);
            }

            // BACKWARD (same condition shape as pawn_structure())
            Bitboard friendly_attack_span = (c == WHITE) ? white_attack_span
                                                         : black_attack_span;
            bool stop_attacked_by_enemy = (pawn_attacks_bb(c, square_bb(stop))
                                           & their_pawns) != 0;
            bool no_friendly_support    = !(square_bb(sq) & friendly_attack_span);
            if (stop_attacked_by_enemy && no_friendly_support && !is_passed) {
                bump_mg_eg(trace, PAWN_STRUCT_START + W_PAWN_BACKWARD_MG, sign);
            }

            // CONNECTED
            if (pawn_attacks_bb(~c, square_bb(sq)) & our_pawns) {
                bump_mg_eg(trace, PAWN_STRUCT_START + W_PAWN_CONNECTED_MG, sign);
            }
        }
    }

    // ---- Positional eval: same pattern. Walk the logic of positional_eval()
    // but record counts.
    Bitboard occ                = board.occupancy();
    Bitboard white_pawn_attacks = pawn_attacks_bb(WHITE, white_pawns);
    Bitboard black_pawn_attacks = pawn_attacks_bb(BLACK, black_pawns);

    for (int c = WHITE; c <= BLACK; c++) {
        Color us = Color(c);
        int sign = (us == WHITE) ? +1 : -1;

        Bitboard our_pieces         = board.by_color[us];
        Bitboard enemy_pawn_attacks = (us == WHITE) ? black_pawn_attacks
                                                    : white_pawn_attacks;
        Bitboard mob_area = ~our_pieces & ~enemy_pawn_attacks;

        // KNIGHT mobility + outpost
        Bitboard knights = board.piece_bb(us, KNIGHT);
        while (knights) {
            Square sq = pop_lsb(knights);
            int moves = popcount(knight_attack(sq) & mob_area);
            bump_mg_eg(trace, MOBILITY_START + W_MOBILITY_KNIGHT_MG, sign * moves);

            int rel_rank = (us == WHITE) ? rank_of(sq) : 7 - rank_of(sq);
            if (rel_rank >= 3 && rel_rank <= 5) {
                int f = file_of(sq);
                Bitboard adj = adjacent_files_bb(f);
                Bitboard enemy_pawns_adj = (us == WHITE) ? (black_pawns & adj)
                                                         : (white_pawns & adj);
                Bitboard forward_mask;
                if (us == WHITE) {
                    int r = rank_of(sq);
                    forward_mask = (r >= 7) ? 0ULL
                                            : ~((1ULL << (8 * (r + 1))) - 1);
                } else {
                    forward_mask = ((1ULL << (8 * rank_of(sq))) - 1);
                }
                if (!(enemy_pawns_adj & forward_mask)) {
                    Bitboard our_pawns    = (us == WHITE) ? white_pawns : black_pawns;
                    Bitboard support_mask = pawn_attack(~us, sq);
                    if (our_pawns & support_mask) {
                        bump_mg_eg(trace,
                                   KNIGHT_OUTPOST_START + W_KNIGHT_OUTPOST_SUPPORTED_MG,
                                   sign);
                    } else {
                        bump_mg_eg(trace,
                                   KNIGHT_OUTPOST_START + W_KNIGHT_OUTPOST_REACHABLE_MG,
                                   sign);
                    }
                }
            }
        }

        // BISHOP mobility + pair
        Bitboard bishops = board.piece_bb(us, BISHOP);
        if (popcount(bishops) >= 2) {
            bump_mg_eg(trace, BISHOP_PAIR_START + W_BISHOP_PAIR_MG, sign);
        }
        while (bishops) {
            Square sq = pop_lsb(bishops);
            int moves = popcount(bishop_attack(sq, occ) & mob_area);
            bump_mg_eg(trace, MOBILITY_START + W_MOBILITY_BISHOP_MG, sign * moves);
        }

        // ROOK mobility + files + 7th rank
        Bitboard rooks = board.piece_bb(us, ROOK);
        while (rooks) {
            Square sq = pop_lsb(rooks);
            int moves = popcount(rook_attack(sq, occ) & mob_area);
            bump_mg_eg(trace, MOBILITY_START + W_MOBILITY_ROOK_MG, sign * moves);

            Bitboard this_file = file_bb(file_of(sq));
            Bitboard our_pawns_on_file   = (us == WHITE) ? (white_pawns & this_file)
                                                         : (black_pawns & this_file);
            Bitboard their_pawns_on_file = (us == WHITE) ? (black_pawns & this_file)
                                                         : (white_pawns & this_file);
            if (!our_pawns_on_file) {
                if (!their_pawns_on_file) {
                    bump_mg_eg(trace,
                               ROOK_BONUSES_START + W_ROOK_OPEN_FILE_MG, sign);
                } else {
                    bump_mg_eg(trace,
                               ROOK_BONUSES_START + W_ROOK_SEMI_OPEN_FILE_MG, sign);
                }
            }
            int rel_rank_r = (us == WHITE) ? rank_of(sq) : 7 - rank_of(sq);
            if (rel_rank_r == 6) {
                bump_mg_eg(trace,
                           ROOK_BONUSES_START + W_ROOK_ON_7TH_MG, sign);
            }
        }

        // QUEEN mobility
        Bitboard queens = board.piece_bb(us, QUEEN);
        while (queens) {
            Square sq = pop_lsb(queens);
            Bitboard attacks = bishop_attack(sq, occ) | rook_attack(sq, occ);
            int moves = popcount(attacks & mob_area);
            bump_mg_eg(trace, MOBILITY_START + W_MOBILITY_QUEEN_MG, sign * moves);
        }
    }

    // ---- Mopup. The activation test in evaluate() looks at the
    // score-so-far (everything except mopup). We have to reproduce that
    // test exactly to know whether mopup contributes. We compute the
    // pre-mopup score from the trace as it stands and apply the same guard.
    bool no_pawns = board.piece_bb(PAWN) == 0;
    if (no_pawns) {
        // Pre-mopup score: same blend evaluate() does, plus additional_score.
        // We need a temporary blend just to make the mopup decision.
        int mg_total = 0, eg_total = 0;
        for (int i = 0; i < NUM_WEIGHTS; i += 2) {
            mg_total += trace.coefficients[i + 0] * eval_weights[i + 0];
            eg_total += trace.coefficients[i + 1] * eval_weights[i + 1];
        }
        Score pre_mopup = Score((mg_total * phase_mg
                               + eg_total * (256 - phase_mg)) / 256)
                        + trace.additional_score;
        if (std::abs(pre_mopup) >= MOPUP_THRESHOLD) {
            Color strong_side = (pre_mopup > 0) ? WHITE : BLACK;
            trace.additional_score += mopup_eval(board, strong_side);
        }
    }
}

// score_from_trace: reconstructs the score evaluate() would produce, given a
// trace and a weight set. The weight set is parameterised so external tooling
// can plug in alternate weights (the whole point of tracing).
//
// Sums MG slots together, EG slots together, applies the phase blend, adds
// the additional_score, and finally flips sign for Black to match evaluate()'s
// side-to-move convention.
//
// Layout reminder: in every WeightGroup, MG slots are at even local offsets
// and EG slots at odd local offsets. Because group base indices are all even
// (see the enum in eval.h), absolute even indices are MG and absolute odd
// indices are EG. We rely on that here.
Score score_from_trace(const EvalTrace& trace,
                       const int*       weights,
                       Color            side_to_move) {
    int mg_total = 0;
    int eg_total = 0;
    for (int i = 0; i < NUM_WEIGHTS; i += 2) {
        mg_total += trace.coefficients[i + 0] * weights[i + 0];
        eg_total += trace.coefficients[i + 1] * weights[i + 1];
    }
    Score blended = Score((mg_total * trace.phase_mg
                         + eg_total * (256 - trace.phase_mg)) / 256);
    Score from_white = blended + trace.additional_score;
    return (side_to_move == WHITE) ? from_white : -from_white;
}

void evaluate_verbose(const Board& board) {
    int   phase_mg = game_phase(board);

    // Material is phase-independent (KING_VALUE is 0 anyway). We display per
    // side and as a difference, just like the previous version.
    Score material_w  = 0, material_b  = 0;

    // For the displayed "PST" row we still want a single blended number per
    // side. Since pst_mg_value() and pst_eg_value() now return raw entries,
    // we accumulate raw mg/eg totals per side and blend them once per side
    // for display. The total at the bottom uses the real evaluate() shape
    // (one global blend of (W - B) at the end), so the displayed PST rows
    // are informational -- they may differ from the contribution to the
    // total by a few cp once tuning pulls MG and EG apart, but for the
    // initial 1.6 state where MG == EG they are bit-identical.
    int pst_mg_w = 0, pst_eg_w = 0;
    int pst_mg_b = 0, pst_eg_b = 0;

    for (int pt = PAWN; pt <= KING; pt++) {
        PieceType piece_type = PieceType(pt);

        Bitboard wb = board.piece_bb(WHITE, piece_type);
        while (wb) {
            Square sq    = pop_lsb(wb);
            material_w  += PIECE_VALUE[pt];
            pst_mg_w    += pst_mg_value(piece_type, WHITE, sq);
            pst_eg_w    += pst_eg_value(piece_type, WHITE, sq);
        }

        Bitboard bb = board.piece_bb(BLACK, piece_type);
        while (bb) {
            Square sq    = pop_lsb(bb);
            material_b  += PIECE_VALUE[pt];
            pst_mg_b    += pst_mg_value(piece_type, BLACK, sq);
            pst_eg_b    += pst_eg_value(piece_type, BLACK, sq);
        }
    }

    // Per-side blended PST for display rows.
    Score pst_w = Score((pst_mg_w * phase_mg + pst_eg_w * (256 - phase_mg)) / 256);
    Score pst_b = Score((pst_mg_b * phase_mg + pst_eg_b * (256 - phase_mg)) / 256);

    // King safety per side (blended value for display). The raw MG/EG outputs
    // are not needed here, so they go into throwaway locals.
    int ks_vw_mg, ks_vw_eg, ks_vb_mg, ks_vb_eg;
    Score king_safety_w = king_safety(board, WHITE, phase_mg, ks_vw_mg, ks_vw_eg);
    Score king_safety_b = king_safety(board, BLACK, phase_mg, ks_vb_mg, ks_vb_eg);

    // Tropism and shelter/storm per side (blended, for display and the partial
    // total). Like king safety these fold into evaluate()'s single blend, so
    // the per-side display values can drift ~1 cp from evaluate(); the real
    // total below is recomputed from raw mg/eg to stay bit-exact.
    int tr_vw_mg, tr_vw_eg, tr_vb_mg, tr_vb_eg;
    Score tropism_w = tropism(board, WHITE, phase_mg, tr_vw_mg, tr_vw_eg);
    Score tropism_b = tropism(board, BLACK, phase_mg, tr_vb_mg, tr_vb_eg);

    int ss_vw_mg, ss_vw_eg, ss_vb_mg, ss_vb_eg;
    Score shelter_w = shelter_storm(board, WHITE, phase_mg, ss_vw_mg, ss_vw_eg);
    Score shelter_b = shelter_storm(board, BLACK, phase_mg, ss_vb_mg, ss_vb_eg);

    int k2_vw_mg, k2_vw_eg, k2_vb_mg, k2_vb_eg;
    Score ksv2_w = king_safety_v2(board, WHITE, phase_mg, k2_vw_mg, k2_vw_eg);
    Score ksv2_b = king_safety_v2(board, BLACK, phase_mg, k2_vb_mg, k2_vb_eg);

    Score pawn_struct  = pawn_structure(board, phase_mg);
    Score positional   = positional_eval(board, phase_mg);
    int p2v_mg, p2v_eg;
    Score positional2_s = positional2(board, board.side_to_move, phase_mg, p2v_mg, p2v_eg);

    // Real total = same Option B blend evaluate() does. We compute it from
    // the raw mg/eg totals to match evaluate() bit-for-bit (not from the
    // displayed per-side blends, which can drift by ~1 cp once MG != EG due
    // to integer truncation rounding in different places).
    int mg_total = (material_w - material_b) + (pst_mg_w - pst_mg_b);
    int eg_total = (material_w - material_b) + (pst_eg_w - pst_eg_b);
    Score material_pst_blended = Score((mg_total * phase_mg
                                      + eg_total * (256 - phase_mg)) / 256);

    // Partial total = everything except mopup. Used to replicate the mopup
    // activation test from evaluate() (which uses the score-so-far).
    Score partial = material_pst_blended
                  + (king_safety_w - king_safety_b)
                  + (ksv2_w - ksv2_b)
                  + (tropism_w - tropism_b)
                  + (shelter_w - shelter_b)
                  + pawn_struct + positional + positional2_s;

    Score mopup = 0;
    bool no_pawns = board.piece_bb(PAWN) == 0;
    if (no_pawns && std::abs(partial) >= MOPUP_THRESHOLD) {
        Color strong_side = (partial > 0) ? WHITE : BLACK;
        mopup = mopup_eval(board, strong_side);
    }

    Score total_white_pov = partial + mopup;
    Score total_stm = (board.side_to_move == WHITE) ? total_white_pov : -total_white_pov;

    // Determine why mopup was skipped (if it was), so we can show an accurate
    // label. There are three reasons mopup can be 0:
    //   1. Pawns are present on the board (mopup only applies to pawnless endings).
    //   2. The partial score does not exceed MOPUP_THRESHOLD (no material edge).
    //   3. mopup_eval was called but returned 0 due to its insufficient-material
    //      guard (e.g. KB vs K, KN vs K, KNN vs K, KBB same-color vs K).
    // Cases 1 and 2 are detected directly. Case 3 we detect by replicating the
    // guard logic from mopup_eval here (small duplication, but keeps the hot
    // path untouched).
    bool mopup_drawn_material = false;
    if (mopup == 0 && no_pawns && std::abs(partial) >= MOPUP_THRESHOLD) {
        Color strong_side = (partial > 0) ? WHITE : BLACK;
        Bitboard strong_pieces = board.all_pieces(strong_side);
        int strong_count = popcount(strong_pieces);
        if (strong_count == 2) {
            if (board.piece_bb(strong_side, BISHOP) || board.piece_bb(strong_side, KNIGHT))
                mopup_drawn_material = true;
        }
        else if (strong_count == 3) {
            Bitboard knights = board.piece_bb(strong_side, KNIGHT);
            Bitboard bishops = board.piece_bb(strong_side, BISHOP);
            Bitboard rooks   = board.piece_bb(strong_side, ROOK);
            Bitboard queens  = board.piece_bb(strong_side, QUEEN);
            if (popcount(knights) == 2 && bishops == 0 && rooks == 0 && queens == 0)
                mopup_drawn_material = true;
            else if (popcount(bishops) == 2 && knights == 0 && rooks == 0 && queens == 0) {
                Bitboard b_copy = bishops;
                Square s1 = pop_lsb(b_copy);
                Square s2 = pop_lsb(b_copy);
                int parity1 = (file_of(s1) + rank_of(s1)) & 1;
                int parity2 = (file_of(s2) + rank_of(s2)) & 1;
                if (parity1 == parity2) mopup_drawn_material = true;
            }
        }
    }

    std::cout << "Static evaluation (cp = centipawns, +ve favors White unless noted):\n";
    std::cout << "  game phase (0 = bare kings, 256 = full non-pawn material): " << phase_mg << "\n";
    std::cout << "\n";
    std::cout << "                         White       Black       Diff (W - B)\n";
    std::cout << "  Material:           "
              << std::setw(6) << material_w << "      "
              << std::setw(6) << material_b << "      "
              << std::setw(6) << (material_w - material_b) << "\n";
    std::cout << "  PST:                "
              << std::setw(6) << pst_w << "      "
              << std::setw(6) << pst_b << "      "
              << std::setw(6) << (pst_w - pst_b) << "\n";
    std::cout << "  King safety:        "
              << std::setw(6) << king_safety_w << "      "
              << std::setw(6) << king_safety_b << "      "
              << std::setw(6) << (king_safety_w - king_safety_b) << "\n";
    std::cout << "  King safety v2:     "
              << std::setw(6) << ksv2_w << "      "
              << std::setw(6) << ksv2_b << "      "
              << std::setw(6) << (ksv2_w - ksv2_b) << "\n";
    std::cout << "  Tropism:            "
              << std::setw(6) << tropism_w << "      "
              << std::setw(6) << tropism_b << "      "
              << std::setw(6) << (tropism_w - tropism_b) << "\n";
    std::cout << "  Shelter/storm:      "
              << std::setw(6) << shelter_w << "      "
              << std::setw(6) << shelter_b << "      "
              << std::setw(6) << (shelter_w - shelter_b) << "\n";
    std::cout << "\n";
    std::cout << "  Pawn structure (W-B perspective):  " << std::setw(6) << pawn_struct << "\n";
    std::cout << "  Positional     (W-B perspective):  " << std::setw(6) << positional << "\n";
    std::cout << "  Positional2    (1.6 batch, stm):   " << std::setw(6) << positional2_s << "\n";
    std::cout << "  Mopup          (W-B perspective):  " << std::setw(6) << mopup;
    if (mopup == 0) {
        if      (!no_pawns)            std::cout << "  (skipped: pawns present)";
        else if (mopup_drawn_material) std::cout << "  (skipped: insufficient material in mopup)";
        else                           std::cout << "  (skipped: |partial| below threshold)";
    }
    std::cout << "\n";
    std::cout << "\n";
    std::cout << "  Total (White's perspective):       " << std::setw(6) << total_white_pov << "\n";
    std::cout << "  Total (side to move = "
              << (board.side_to_move == WHITE ? "White" : "Black") << "):       "
              << std::setw(6) << total_stm << "\n";
    std::cout << std::flush;
}
