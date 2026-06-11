// =============================================================================
// Last modified: 2026-06-06 19:27
// eval.h -- Position evaluation
//
// Returns a score in centipawns (100 = one pawn advantage) from the
// perspective of the side to move. Positive = good for the mover.
//
// Facon 1.0 -- Oxido
//   - Material count
//   - Piece-square tables (PST): bonus/penalty per piece per square
//
// Facon 1.1 -- Herrumbre
//   - King safety: penalty for enemy pieces attacking the king zone
//
// Facon 1.2 -- Rojo Vivo
//   - Mopup evaluation: in pawnless endings with a decisive material
//     advantage, reward pushing the losing king toward a corner and
//     closing the distance between kings. Without this the engine can
//     hold a winning advantage but fail to convert -- wandering without
//     a plan. Applied only when no pawns remain and the material
//     advantage exceeds MOPUP_THRESHOLD.
//
// Facon 1.3 -- Yunque
//   - Pawn structure: isolated, doubled, backward, passed, connected pawns.
//     All terms computed via bitboard operations. Constants below.
//   - Insufficient material guard: mopup_eval() now returns 0 for K+B vs K
//     and K+N vs K (theoretical draws). Without this, K+B (330cp) exceeded
//     the MOPUP_THRESHOLD (300cp) and activated corner-chasing in drawn
//     endings.
//
// Facon 1.4 -- Hoja
//   - Positional evaluation: mobility, open/semi-open files, rook on 7th,
//     bishop pair, knight outposts. All computed in positional_eval() via
//     bitboard operations, called once per evaluate().
//
// Facon 1.5 -- Espiga
//   - evaluate_verbose(): debug helper that prints a per-component breakdown
//     of evaluate() to stdout. Used by the UCI "eval" command. Not in the
//     search hot path -- runs once per command invocation.
//   - Knight outpost refactor: replaced the single KNIGHT_OUTPOST constant
//     (flat 20cp) with two graduated bonuses:
//       KNIGHT_OUTPOST_REACHABLE (10cp): square is safe from pawn attack
//         but not defended by a friendly pawn.
//       KNIGHT_OUTPOST_SUPPORTED (25cp): square is safe AND supported by
//         a friendly pawn (firmly anchored).
//     The previous flat bonus did not distinguish the two cases.
//
// Facon 1.6 -- Temple
//   - Piece-square tables refactored to fully tapered form. All non-king
//     PSTs (pawn, knight, bishop, rook, queen) are now split into separate
//     middlegame (MG) and endgame (EG) tables interpolated by game_phase,
//     matching the shape already used by the king PST. Initial values for
//     each MG/EG pair are identical to the previous monolithic table, so
//     behavior is mathematically preserved. The structural change opens
//     these terms for later positional refinement.
//   - Comment audit pass: comments across this file (and its companion
//     eval.cpp) revised for completeness and didactic clarity. Material
//     values now reference the Kaufman convention they follow; positional
//     constants explain their expected ranges; PSTs document the chess
//     intuition behind each table's shape. Non-ASCII punctuation removed
//     for portability. No functional changes.
//   - Pawn structure refactored to tapered form. The five named constants
//     (PAWN_ISOLATED, PAWN_DOUBLED, PAWN_BACKWARD, PAWN_CONNECTED, and the
//     per-rank PASSED_BONUS table in eval.cpp) are now each split into MG
//     and EG variants. The pawn_structure() function accumulates two running
//     totals (one for each phase) and blends them at the end via the same
//     game_phase weight used by the PSTs. Initial values: every MG equals
//     its corresponding EG, so behavior is preserved exactly. Future tuning
//     will pull them apart -- typically EG values for passed pawns and
//     connected pawns rise relative to MG, while doubled-pawn penalties
//     soften in the endgame.
//   - Positional evaluation refactored to tapered form. The ten positional
//     constants (mobility per piece type, rook open/semi-open file and 7th
//     rank, bishop pair, knight outposts) are each split into MG and EG
//     variants. positional_eval() now takes phase_mg, accumulates running
//     mg/eg totals across all terms and both sides, and blends them with a
//     single integer division at the end (same "blend-of-difference"
//     convention used for pawn structure). Initial values: every MG equals
//     its corresponding EG, so behavior is preserved exactly. With this
//     change the full eval (PSTs, pawn structure, positional terms) is
//     uniformly tapered -- king safety remains the single exception, and
//     is handled separately for reasons documented in eval.cpp.
//   - Material + PST loop refactored to "blend-of-totals" (Option B). The
//     main loop in evaluate() now accumulates two running scores -- mg_score
//     and eg_score -- by summing material (which is phase-independent) and
//     the raw MG / EG entries of each piece's PST (read separately via
//     pst_mg_value() / pst_eg_value()), then blends them once at the end.
//     Previously the loop blended each piece individually via pst_value()
//     and summed the results, which mixed Option A (per-piece blend) with
//     the Option B blends used by pawn_structure() and positional_eval().
//     Unifying everything on Option B leaves the entire eval -- material,
//     PSTs, pawn structure, positional terms -- with a single phase blend
//     and a single integer division per evaluation, which is the shape
//     evaluation-tuning tooling expects to consume.
//     Bench signature changes by a small amount: the king PST has MG != EG
//     entries (the table inverts between phases by design, since the king
//     wants the corner in the middlegame and the centre in the endgame),
//     so reordering its blend changes integer truncation by up to ~1 cp
//     per position. Non-king PSTs all have MG == EG in 1.6's initial
//     state, so they contribute zero to the signature change. The signature
//     shift is expected and is documented per release rule 3.4.
//   - Tunable weights centralized into a single mutable array. All weights
//     that the planned tuning workflow will adjust -- pawn structure
//     constants, passed-pawn bonuses by rank, mobility per piece type, rook
//     placement bonuses, the bishop pair bonus, knight outposts, and every
//     entry of every piece-square table (MG and EG) -- now live in
//     `int eval_weights[NUM_WEIGHTS]` in eval.cpp, accessed through a set
//     of group-base offsets declared as an enum in this header. The
//     individual named constants (PAWN_ISOLATED_MG, MOBILITY_KNIGHT_MG,
//     etc.) and the per-piece PST arrays (PST_PAWN_MG[64] etc.) no longer
//     exist; their values now live as initial entries of `eval_weights[]`.
//     Material values, mopup constants, king-safety constants, and the
//     game-phase tables remain as constexpr / static data because none of
//     them are tuned (mopup and king safety are out of the tuning scope by
//     design; material values are stable enough not to need tuning at this
//     strength). Bench signature is preserved bit-for-bit -- this is a pure
//     storage refactor, the arithmetic of evaluate() is unchanged.
//   - Eval trace mode added. trace_evaluate() and the EvalTrace struct
//     produce, for a given position, the vector of per-weight coefficients
//     (white_count - black_count for each entry in eval_weights[]) plus the
//     "additional score" for terms that lie outside the tunable array
//     (mopup and king safety). The contract is:
//
//         evaluate(board) ==
//             dot(trace.coefficients, eval_weights) + trace.additional_score
//
//     to the bit. trace_evaluate() is a parallel implementation of
//     evaluate() that counts feature occurrences instead of multiplying
//     them by weights. It is exposed through the UCI "trace <fen>" command
//     for inspection. Not used in the search hot path -- adds no cost to
//     normal play. The function exists so that external evaluation tooling
//     that consumes per-position coefficient vectors can verify, position
//     by position, that its own extraction matches the engine's evaluation.
//   - Weight-group offsets converted from `enum` to `constexpr int`. The
//     group-base offsets (PAWN_STRUCT_START etc.) and per-group offsets
//     (W_PAWN_ISOLATED_MG etc.) used to be scoped enums. Because they are
//     summed together in index arithmetic (GROUP_START + offset), and they
//     were different enum types, C++20 flagged the additions as deprecated
//     (-Wdeprecated-enum-enum-conversion). Plain integer constants remove
//     the warning and generate identical code. No behavioural change; bench
//     signature is preserved bit-for-bit.
//   - SHELTER_STORM group added (32 slots at offset 868): pawn shelter (the
//     friendly pawns sheltering each king, by distance bucket) and pawn storm
//     (enemy pawns advancing on the king's files). Tapered and tunable; see
//     the W_SS_* offsets below.
//   - KING_SAFETY_V2 group added (16 slots at offset 900): open/semi-open
//     files toward each king (king's file weighted apart from the adjacent
//     files) and safe checks (squares an enemy can check from without being
//     captured, per piece type). Tapered and tunable; see the W_KSV2_* offsets.
//   - POSITIONAL2 group added (offset 916): tempo (side-to-move bonus), bishop
//     outpost (reachable and pawn-supported tiers) and passed-pawn refinement
//     (king proximity, blockade by an enemy minor/major, free path, and pawn
//     protection). Each feature was added and validated individually. Tapered
//     and tunable; see the W_POS2_* offsets below.
// =============================================================================

#pragma once

#include "types.h"
#include "board.h"

// =============================================================================
// MATERIAL VALUES (in centipawns)
// =============================================================================
// One pawn = 100 cp. The other values follow the well-known Kaufman scale
// (Larry Kaufman's empirical study of grandmaster games), which is also the
// starting point most modern engines use before tuning:
//
//   Pawn   = 100 cp  (reference unit, by definition)
//   Knight = 320 cp  (slightly less than a bishop in most positions)
//   Bishop = 330 cp  (slight edge over knight; the bishop pair adds more
//                     on top via BISHOP_PAIR_BONUS_MG/EG in positional_eval)
//   Rook   = 500 cp  (a rook is worth about 5 pawns)
//   Queen  = 900 cp  (close to two rooks but more flexible)
//   King   = 0 cp    (cannot be captured in legal play, so material value
//                     is meaningless; only its PST contribution matters)

constexpr Score PAWN_VALUE   = 100;
constexpr Score KNIGHT_VALUE = 320;
constexpr Score BISHOP_VALUE = 330;
constexpr Score ROOK_VALUE   = 500;
constexpr Score QUEEN_VALUE  = 900;
constexpr Score KING_VALUE   = 0;    // King has no material value (never captured)

// Array indexed by PieceType for convenient lookup.
// Size is 7: NO_PIECE_TYPE at index 0, then PAWN..KING at indices 1..6.
// (See types.h for the PieceType enum definition.)
constexpr Score PIECE_VALUE[7] = {
    0,             // NO_PIECE_TYPE (index 0, never queried in practice)
    PAWN_VALUE,    // index 1
    KNIGHT_VALUE,  // index 2
    BISHOP_VALUE,  // index 3
    ROOK_VALUE,    // index 4
    QUEEN_VALUE,   // index 5
    KING_VALUE     // index 6
};

// =============================================================================
// TUNABLE EVALUATION WEIGHTS
// =============================================================================
// All weights that the planned tuning workflow will adjust live here as a
// single mutable array `eval_weights[NUM_WEIGHTS]` (defined in eval.cpp).
// The array is split into named groups; this enum stores the base index of
// each group. To read a specific weight, callers compute the absolute index
// as `GROUP_START + offset_within_group`.
//
// Why a flat array instead of named constants. The first-pass tuner consumes
// weights as a single contiguous vector that it can modify in place at each
// iteration. Mapping every named constant to a slot of one shared array is
// what makes that loop possible. The cost of this design is that reading a
// weight is slightly more verbose at the call site ("eval_weights[MOBILITY_START
// + 2]" instead of "MOBILITY_KNIGHT_MG"); the gain is that the engine and
// the tuner agree on exactly one source of truth for every value.
//
// Why offsets rather than one enum entry per individual weight. Per-weight
// entries (W_PAWN_ISOLATED_MG, W_PAWN_ISOLATED_EG, ...) would balloon to
// ~820 enum constants and tie us tightly to a flat naming scheme. The
// tuner's data structures are naturally grouped (one int16_t coefficient
// array per feature group), so matching the engine's layout to that
// grouping keeps the mapping between engine and tuner trivial.
//
// What is NOT in eval_weights:
//   - PIECE_VALUE[] (material values): stable enough not to need tuning at
//     this strength level; kept as constexpr above.
//   - MOPUP_*: mopup is out of the tuning scope by design (it is a tactical
//     guidance term for already-decided endings, not a positional weight).
//   - KING_ATTACK_WEIGHT[] / SAFETY_SCALE: king safety stays single-valued
//     (no MG/EG split) due to its quadratic shape, which a linear tuner
//     cannot move sensibly. A separate refactor to a tunable lookup table
//     will replace these in a later dev.
//   - PHASE_VALUE[] and TOTAL_PHASE: tapering machinery, not weights.
//
// Layout (934 weights total):
//
//   [  0,  16)  PAWN_STRUCT_START      Pawn-structure scalars (4 features
//                                       * 2 phases = 8 used, 16 reserved for
//                                       alignment / future expansion).
//   [ 16,  32)  PASSED_BONUS_START     Passed-pawn bonus by rank (8 ranks
//                                       * 2 phases). Rank 0 (own back rank)
//                                       and rank 7 (already promoted) are
//                                       unreachable for a passed pawn and stay
//                                       zero; they occupy slots only for
//                                       simple indexing.
//   [ 32,  40)  MOBILITY_START         Mobility scalars per piece type
//                                       (knight, bishop, rook, queen)
//                                       * 2 phases.
//   [ 40,  46)  ROOK_BONUSES_START     Rook open file, semi-open file, 7th
//                                       rank: 3 features * 2 phases.
//   [ 46,  48)  BISHOP_PAIR_START      Bishop pair bonus * 2 phases.
//   [ 48,  52)  KNIGHT_OUTPOST_START   Knight outpost reachable and
//                                       supported tiers * 2 phases.
//   [ 52, 180)  PST_PAWN_START         Pawn PST: 64 squares * 2 phases,
//                                       interleaved as [sq*2+0]=MG, [sq*2+1]=EG.
//   [180, 308)  PST_KNIGHT_START       Same layout, 64 * 2.
//   [308, 436)  PST_BISHOP_START       Same layout, 64 * 2.
//   [436, 564)  PST_ROOK_START         Same layout, 64 * 2.
//   [564, 692)  PST_QUEEN_START        Same layout, 64 * 2.
//   [692, 820)  PST_KING_START         Same layout, 64 * 2. The king PST is
//                                       the only one whose MG and EG values
//                                       differ at the initial state of 1.6.
//   [820, 836)  KING_SAFETY_START      King-safety terms: 4 per-attacker-type
//                                       weights + 4 attacker-count buckets,
//                                       * 2 phases. Linear form (tunable);
//                                       see the KS_* offsets and king_safety()
//                                       in eval.cpp.
//   [836, 868)  TROPISM_START          Piece tropism: 4 piece types * 4
//                                       Chebyshev-distance buckets {1,2,3,4+}
//                                       * 2 phases. Linear/bucketed (tunable);
//                                       see the TROP_* offsets and tropism()
//                                       in eval.cpp.
//   [868, 900)  SHELTER_STORM_START    Pawn shelter (3 files * 4 distance
//                                       buckets) and pawn storm, * 2 phases.
//                                       Tunable; see the W_SS_* offsets and
//                                       shelter_storm() in eval.cpp.
//   [900, 916)  KING_SAFETY_V2_START   Open/semi-open files toward the king
//                                       (king file vs adjacent) and safe
//                                       checks per piece type, * 2 phases.
//                                       Tunable; see the W_KSV2_* offsets and
//                                       king_safety_v2() in eval.cpp.
//   [916, 934)  POSITIONAL2_START      Tempo, bishop outpost (2 tiers) and
//                                       passed-pawn refinement (king proximity
//                                       own/enemy, blockade minor/major, free
//                                       path, protected), * 2 phases. Tunable;
//                                       see the W_POS2_* offsets and
//                                       positional2() in eval.cpp.

// Group base offsets into eval_weights[]. These were an `enum` until 1.6;
// they are now plain `constexpr int` constants because they participate in
// integer arithmetic (GROUP_START + offset) together with the per-group
// offset constants below. Mixing two different enum types in one arithmetic
// expression is deprecated in C++20 (-Wdeprecated-enum-enum-conversion);
// plain integer constants avoid that entirely while producing identical code.
constexpr int PAWN_STRUCT_START    =   0;
constexpr int PASSED_BONUS_START   =  16;
constexpr int MOBILITY_START       =  32;
constexpr int ROOK_BONUSES_START   =  40;
constexpr int BISHOP_PAIR_START    =  46;
constexpr int KNIGHT_OUTPOST_START =  48;
constexpr int PST_PAWN_START       =  52;
constexpr int PST_KNIGHT_START     = 180;
constexpr int PST_BISHOP_START     = 308;
constexpr int PST_ROOK_START       = 436;
constexpr int PST_QUEEN_START      = 564;
constexpr int PST_KING_START       = 692;
// King-safety group, appended after the PSTs so every pre-existing offset is
// unchanged (the 820 weights below KING_SAFETY_START keep their positions;
// only 16 new slots are added at the end). See the KS_* offsets below and the
// king_safety() implementation in eval.cpp for what each slot means.
constexpr int KING_SAFETY_START    = 820;
// Piece-tropism group, appended after king safety (again, no pre-existing
// offset moves). 4 piece types * 4 distance buckets * 2 phases = 32 slots.
// See the TROP_* offsets below and tropism() in eval.cpp.
constexpr int TROPISM_START        = 836;
// Pawn shelter/storm group, appended after tropism (no pre-existing offset
// moves). 2 file categories (king file, adjacent file) * 4 distance buckets
// * 2 phenomena (own-pawn shelter, enemy-pawn storm) * 2 phases = 32 slots.
// See the SHELTER/STORM offsets below and shelter_storm() in eval.cpp.
constexpr int SHELTER_STORM_START  = 868;
// King-safety v2 group, appended after shelter/storm. Two new families folded
// into king safety: open/semi-open files toward the king (4 weights) and safe
// checks by piece type (4 weights). 8 logical terms * 2 phases = 16 slots.
// See the KSV2 offsets below and king_safety_v2() in eval.cpp.
constexpr int KING_SAFETY_V2_START = 900;
// Positional batch (1.6), appended at the end to avoid renumbering earlier
// offsets. Features are added one at a time (or in small thematic groups),
// each validated by self-play before inclusion; only gainers stay. Currently:
// tempo (1 pair) + bishop outpost (2 pairs) + passed-pawn refinement
// (king proximity own/enemy, blockade minor/major, free path, protected; 6 pairs).
constexpr int POSITIONAL2_START    = 916;
constexpr int NUM_WEIGHTS          = 934;

// Offsets within the PAWN_STRUCT group.
// Use as: eval_weights[PAWN_STRUCT_START + W_PAWN_ISOLATED_MG], etc.
constexpr int W_PAWN_ISOLATED_MG  = 0;
constexpr int W_PAWN_ISOLATED_EG  = 1;
constexpr int W_PAWN_DOUBLED_MG   = 2;
constexpr int W_PAWN_DOUBLED_EG   = 3;
constexpr int W_PAWN_BACKWARD_MG  = 4;
constexpr int W_PAWN_BACKWARD_EG  = 5;
constexpr int W_PAWN_CONNECTED_MG = 6;
constexpr int W_PAWN_CONNECTED_EG = 7;
// slots 8..15 reserved (alignment to 16 / future expansion)

// Offsets within the MOBILITY group.
// Use as: eval_weights[MOBILITY_START + W_MOBILITY_KNIGHT_MG], etc.
constexpr int W_MOBILITY_KNIGHT_MG = 0;
constexpr int W_MOBILITY_KNIGHT_EG = 1;
constexpr int W_MOBILITY_BISHOP_MG = 2;
constexpr int W_MOBILITY_BISHOP_EG = 3;
constexpr int W_MOBILITY_ROOK_MG   = 4;
constexpr int W_MOBILITY_ROOK_EG   = 5;
constexpr int W_MOBILITY_QUEEN_MG  = 6;
constexpr int W_MOBILITY_QUEEN_EG  = 7;

// Offsets within the ROOK_BONUSES group.
constexpr int W_ROOK_OPEN_FILE_MG      = 0;
constexpr int W_ROOK_OPEN_FILE_EG      = 1;
constexpr int W_ROOK_SEMI_OPEN_FILE_MG = 2;
constexpr int W_ROOK_SEMI_OPEN_FILE_EG = 3;
constexpr int W_ROOK_ON_7TH_MG         = 4;
constexpr int W_ROOK_ON_7TH_EG         = 5;

// Offsets within the BISHOP_PAIR group.
constexpr int W_BISHOP_PAIR_MG = 0;
constexpr int W_BISHOP_PAIR_EG = 1;

// Offsets within the KNIGHT_OUTPOST group.
constexpr int W_KNIGHT_OUTPOST_REACHABLE_MG = 0;
constexpr int W_KNIGHT_OUTPOST_REACHABLE_EG = 1;
constexpr int W_KNIGHT_OUTPOST_SUPPORTED_MG = 2;
constexpr int W_KNIGHT_OUTPOST_SUPPORTED_EG = 3;

// Offsets within the KING_SAFETY group.
// Use as: eval_weights[KING_SAFETY_START + W_KS_ATTACKER_KNIGHT_MG], etc.
//
// King safety is expressed as a SUM OF LINEAR TERMS so it can be Texel-tuned
// (the tuner optimises a strictly linear model; a quadratic penalty cannot be
// fit directly). Two complementary families of terms:
//
//   (a) Per-attacker-type weight: one weight per enemy piece type that
//       attacks the defender's king zone. Replaces the old hand-set
//       KING_ATTACK_WEIGHT[] table.
//
//   (b) Attacker-count buckets: one weight per distinct count of attacking
//       pieces (1, 2, 3, 4-or-more). Each bucket is a binary feature
//       ("exactly N attackers present" -> 1), which keeps the model linear
//       while letting the tuner make three attackers cost far more than three
//       times one attacker. This recovers the non-linearity the old quadratic
//       captured, but with a structural ceiling (the 4+ bucket) instead of an
//       unbounded square -- so the runaway penalties that sank the 1.5
//       attempt (thousands of cp) cannot occur by construction.
//
// The total king-safety contribution for one king is therefore:
//   sum over attacking pieces of attacker_weight[piece_type]
//   + count_bucket_weight[min(num_attackers, 4)]      (only when >= 1)
//
// All weights are tapered (MG, EG). King safety naturally matters in the
// middlegame and fades in the endgame; rather than hard-coding that with an
// early return, the EG weights are simply free to tune toward ~0, and the
// phase blend in score_from_trace / evaluate handles the fade.
constexpr int W_KS_ATTACKER_KNIGHT_MG = 0;
constexpr int W_KS_ATTACKER_KNIGHT_EG = 1;
constexpr int W_KS_ATTACKER_BISHOP_MG = 2;
constexpr int W_KS_ATTACKER_BISHOP_EG = 3;
constexpr int W_KS_ATTACKER_ROOK_MG   = 4;
constexpr int W_KS_ATTACKER_ROOK_EG   = 5;
constexpr int W_KS_ATTACKER_QUEEN_MG  = 6;
constexpr int W_KS_ATTACKER_QUEEN_EG  = 7;
// Attacker-count buckets. Index by min(num_attackers, 4); a count of 0 has no
// slot (no attackers -> no king-safety penalty), so bucket 1 is the first.
constexpr int W_KS_COUNT_1_MG     =  8;
constexpr int W_KS_COUNT_1_EG     =  9;
constexpr int W_KS_COUNT_2_MG     = 10;
constexpr int W_KS_COUNT_2_EG     = 11;
constexpr int W_KS_COUNT_3_MG     = 12;
constexpr int W_KS_COUNT_3_EG     = 13;
constexpr int W_KS_COUNT_4PLUS_MG = 14;
constexpr int W_KS_COUNT_4PLUS_EG = 15;

// Offsets within the TROPISM group.
// Use as: eval_weights[TROPISM_START + W_TROP_KNIGHT_D1_MG], etc.
//
// Piece tropism rewards having your pieces close to the enemy king. For each
// of your knights, bishops, rooks and queens, the Chebyshev distance to the
// enemy king is bucketed into {1, 2, 3, 4-or-more}, and that (piece, bucket)
// weight is added. Bucketing (rather than a single linear distance term) lets
// the tuner assign an independent value to each ring around the king, so the
// distance->value relationship can be non-linear (e.g. a knight adjacent to
// the king worth far more than one three squares away). Distance 4+ shares one
// bucket -- a piece that far rarely exerts king pressure.
//
// Layout: per piece type, four (MG, EG) bucket pairs in ascending distance.
// All tapered; the tuner is free to push the EG side toward 0 if king pressure
// is mainly a middlegame concern, with the phase blend handling the fade.
constexpr int W_TROP_KNIGHT_D1_MG = 0;
constexpr int W_TROP_KNIGHT_D1_EG = 1;
constexpr int W_TROP_KNIGHT_D2_MG = 2;
constexpr int W_TROP_KNIGHT_D2_EG = 3;
constexpr int W_TROP_KNIGHT_D3_MG = 4;
constexpr int W_TROP_KNIGHT_D3_EG = 5;
constexpr int W_TROP_KNIGHT_D4_MG = 6;   // distance 4 or more
constexpr int W_TROP_KNIGHT_D4_EG = 7;
constexpr int W_TROP_BISHOP_D1_MG = 8;
constexpr int W_TROP_BISHOP_D1_EG = 9;
constexpr int W_TROP_BISHOP_D2_MG = 10;
constexpr int W_TROP_BISHOP_D2_EG = 11;
constexpr int W_TROP_BISHOP_D3_MG = 12;
constexpr int W_TROP_BISHOP_D3_EG = 13;
constexpr int W_TROP_BISHOP_D4_MG = 14;
constexpr int W_TROP_BISHOP_D4_EG = 15;
constexpr int W_TROP_ROOK_D1_MG   = 16;
constexpr int W_TROP_ROOK_D1_EG   = 17;
constexpr int W_TROP_ROOK_D2_MG   = 18;
constexpr int W_TROP_ROOK_D2_EG   = 19;
constexpr int W_TROP_ROOK_D3_MG   = 20;
constexpr int W_TROP_ROOK_D3_EG   = 21;
constexpr int W_TROP_ROOK_D4_MG   = 22;
constexpr int W_TROP_ROOK_D4_EG   = 23;
constexpr int W_TROP_QUEEN_D1_MG  = 24;
constexpr int W_TROP_QUEEN_D1_EG  = 25;
constexpr int W_TROP_QUEEN_D2_MG  = 26;
constexpr int W_TROP_QUEEN_D2_EG  = 27;
constexpr int W_TROP_QUEEN_D3_MG  = 28;
constexpr int W_TROP_QUEEN_D3_EG  = 29;
constexpr int W_TROP_QUEEN_D4_MG  = 30;
constexpr int W_TROP_QUEEN_D4_EG  = 31;

// Offsets within the SHELTER_STORM group.
// Use as: eval_weights[SHELTER_STORM_START + W_SS_SHELTER_KFILE_D1_MG], etc.
//
// Pawn shelter/storm scores the pawns in front of each king. Two phenomena,
// kept as separate tunable families:
//
//   SHELTER (own pawns): for each of the three files around the king (the
//     king's own file and the two adjacent files), the friendly pawn nearest
//     the king shelters it. The nearer that pawn, the better the shelter.
//     Distance is the rank gap between the pawn and the king, bucketed into
//     {1, 2, 3, 4+}. A file with NO friendly pawn maps to the 4+ bucket -- an
//     open file in front of the king is the worst shelter, modelled as a pawn
//     infinitely far away (no extra "missing" slot needed).
//
//   STORM (enemy pawns): for the same three files, the enemy pawn most
//     advanced toward our king leads the storm. The nearer that pawn, the
//     greater the threat. Same {1, 2, 3, 4+} distance buckets. A file with no
//     enemy pawn contributes nothing (no storm on that file).
//
// File category: the king's own file is more critical than the adjacent
// files, so it has its own weights; the two adjacent files share one set.
// That gives 2 categories x 4 buckets x 2 phenomena = 16 logical terms, each
// a (MG, EG) pair = 32 slots.
//
// All tapered. Both kings are scored; evaluate() adds White's contribution
// and subtracts Black's, folding the raw MG/EG into the single end blend, so
// evaluate() and score_from_trace() stay bit-identical. The geometry helper
// shelter_storm_counts() in eval.cpp is the single source of truth shared by
// shelter_storm() and trace_evaluate().
//
// Layout: SHELTER first (king-file buckets, then adjacent-file buckets), then
// STORM (same order).
constexpr int W_SS_SHELTER_KFILE_D1_MG = 0;
constexpr int W_SS_SHELTER_KFILE_D1_EG = 1;
constexpr int W_SS_SHELTER_KFILE_D2_MG = 2;
constexpr int W_SS_SHELTER_KFILE_D2_EG = 3;
constexpr int W_SS_SHELTER_KFILE_D3_MG = 4;
constexpr int W_SS_SHELTER_KFILE_D3_EG = 5;
constexpr int W_SS_SHELTER_KFILE_D4_MG = 6;   // distance 4+ or no friendly pawn
constexpr int W_SS_SHELTER_KFILE_D4_EG = 7;
constexpr int W_SS_SHELTER_AFILE_D1_MG = 8;
constexpr int W_SS_SHELTER_AFILE_D1_EG = 9;
constexpr int W_SS_SHELTER_AFILE_D2_MG = 10;
constexpr int W_SS_SHELTER_AFILE_D2_EG = 11;
constexpr int W_SS_SHELTER_AFILE_D3_MG = 12;
constexpr int W_SS_SHELTER_AFILE_D3_EG = 13;
constexpr int W_SS_SHELTER_AFILE_D4_MG = 14;  // distance 4+ or no friendly pawn
constexpr int W_SS_SHELTER_AFILE_D4_EG = 15;
constexpr int W_SS_STORM_KFILE_D1_MG   = 16;
constexpr int W_SS_STORM_KFILE_D1_EG   = 17;
constexpr int W_SS_STORM_KFILE_D2_MG   = 18;
constexpr int W_SS_STORM_KFILE_D2_EG   = 19;
constexpr int W_SS_STORM_KFILE_D3_MG   = 20;
constexpr int W_SS_STORM_KFILE_D3_EG   = 21;
constexpr int W_SS_STORM_KFILE_D4_MG   = 22;  // distance 4+ (no enemy pawn = no term)
constexpr int W_SS_STORM_KFILE_D4_EG   = 23;
constexpr int W_SS_STORM_AFILE_D1_MG   = 24;
constexpr int W_SS_STORM_AFILE_D1_EG   = 25;
constexpr int W_SS_STORM_AFILE_D2_MG   = 26;
constexpr int W_SS_STORM_AFILE_D2_EG   = 27;
constexpr int W_SS_STORM_AFILE_D3_MG   = 28;
constexpr int W_SS_STORM_AFILE_D3_EG   = 29;
constexpr int W_SS_STORM_AFILE_D4_MG   = 30;  // distance 4+ (no enemy pawn = no term)
constexpr int W_SS_STORM_AFILE_D4_EG   = 31;

// Offsets within the KING_SAFETY_V2 group.
// Use as: eval_weights[KING_SAFETY_V2_START + W_KSV2_OPENFILE_KING_MG], etc.
//
// Two families fold into king safety here, both scored per king and tapered:
//
//   OPEN FILES toward the king: a file with no pawns at all (fully open) or no
//     friendly pawns (semi-open) lets enemy rooks/queens bear down on the king.
//     For the king's file and the two adjacent files, classify each as open or
//     semi-open (a file with a friendly pawn is closed and scores nothing). The
//     king's file is more critical than the adjacent files, so it has its own
//     pair of weights. 2 states (open, semi-open) x 2 file categories (king,
//     adjacent) = 4 logical terms.
//
//   SAFE CHECKS: squares from which an enemy piece could deliver check WITHOUT
//     being captured -- i.e. a checking square the enemy actually attacks that
//     is NOT defended by the king's side. The defender's full attack map
//     (board.attacked_by) decides "safe". One weight per checking piece type
//     (knight, bishop, rook, queen); a queen safe-check is the most dangerous.
//     4 logical terms.
//
// 8 logical terms total, each a (MG, EG) pair = 16 slots. Weights are penalties
// (expected negative for the defender); evaluate() adds White's contribution
// and subtracts Black's, folding the raw MG/EG into the single end blend so
// evaluate() and score_from_trace() stay bit-identical. king_safety_v2() in
// eval.cpp is the single source of truth shared with trace_evaluate().
constexpr int W_KSV2_OPENFILE_KING_MG  = 0;   // fully-open file on the king's file
constexpr int W_KSV2_OPENFILE_KING_EG  = 1;
constexpr int W_KSV2_OPENFILE_ADJ_MG   = 2;   // fully-open file on an adjacent file
constexpr int W_KSV2_OPENFILE_ADJ_EG   = 3;
constexpr int W_KSV2_SEMIFILE_KING_MG  = 4;   // semi-open (no friendly pawn) king's file
constexpr int W_KSV2_SEMIFILE_KING_EG  = 5;
constexpr int W_KSV2_SEMIFILE_ADJ_MG   = 6;   // semi-open adjacent file
constexpr int W_KSV2_SEMIFILE_ADJ_EG   = 7;
constexpr int W_KSV2_SAFECHECK_N_MG    = 8;   // safe knight check available
constexpr int W_KSV2_SAFECHECK_N_EG    = 9;
constexpr int W_KSV2_SAFECHECK_B_MG    = 10;  // safe bishop check
constexpr int W_KSV2_SAFECHECK_B_EG    = 11;
constexpr int W_KSV2_SAFECHECK_R_MG    = 12;  // safe rook check
constexpr int W_KSV2_SAFECHECK_R_EG    = 13;
constexpr int W_KSV2_SAFECHECK_Q_MG    = 14;  // safe queen check
constexpr int W_KSV2_SAFECHECK_Q_EG    = 15;

// Offsets within the POSITIONAL2 group (1.6 positional batch, built up one
// feature at a time). Use as eval_weights[POSITIONAL2_START + W_POS2_*].
//   TEMPO: bonus for the side to move. Added per the side to move (not per
//     colour), so it breaks the usual White-minus-Black symmetry; evaluate()
//     adds it once with the right sign, which survives the final sign-flip.
constexpr int W_POS2_TEMPO_MG = 0;
constexpr int W_POS2_TEMPO_EG = 1;
//   BISHOP OUTPOST: same shape as the knight outpost -- a bishop on an
//     advanced square (relative ranks 4-6) safe from enemy pawns now and in
//     the future (reachable), more if also defended by a friendly pawn
//     (supported).
constexpr int W_POS2_BOUTPOST_REACH_MG = 2;
constexpr int W_POS2_BOUTPOST_REACH_EG = 3;
constexpr int W_POS2_BOUTPOST_SUPP_MG  = 4;
constexpr int W_POS2_BOUTPOST_SUPP_EG  = 5;
//   PASSED-PAWN KING PROXIMITY: for each friendly passed pawn, the Chebyshev
//     distance from each king to the pawn. own_king_dist is summed over our
//     passers (weight expected negative: our king far from our passer is bad);
//     enemy_king_dist likewise (weight expected positive: enemy king far is
//     good for us). Tapered -- matters in the endgame, ~0 in the middlegame.
constexpr int W_POS2_PASSER_OWN_KDIST_MG   = 6;
constexpr int W_POS2_PASSER_OWN_KDIST_EG   = 7;
constexpr int W_POS2_PASSER_ENEMY_KDIST_MG = 8;
constexpr int W_POS2_PASSER_ENEMY_KDIST_EG = 9;
//   PASSED-PAWN REFINEMENT (continued): blockade, free path, protected.
//     blocked_minor/major: an enemy piece sits on the pawn's stop square,
//       neutralizing it (penalty to the passer's side; a minor blockades more
//       efficiently than a major). free_path: every square ahead to promotion
//       is empty (bonus). protected: a friendly pawn defends the passer (bonus).
constexpr int W_POS2_PASSER_BLOCKED_MINOR_MG = 10;
constexpr int W_POS2_PASSER_BLOCKED_MINOR_EG = 11;
constexpr int W_POS2_PASSER_BLOCKED_MAJOR_MG = 12;
constexpr int W_POS2_PASSER_BLOCKED_MAJOR_EG = 13;
constexpr int W_POS2_PASSER_FREE_PATH_MG     = 14;
constexpr int W_POS2_PASSER_FREE_PATH_EG     = 15;
constexpr int W_POS2_PASSER_PROTECTED_MG     = 16;
constexpr int W_POS2_PASSER_PROTECTED_EG     = 17;

// Passed-pawn bonus is read as eval_weights[PASSED_BONUS_START + rank*2 + 0]
// for the MG value and eval_weights[PASSED_BONUS_START + rank*2 + 1] for EG.
// Ranks are White-relative (0..7); pawn_structure() mirrors Black's rank.

// PSTs are read as eval_weights[PST_<PIECE>_START + sq*2 + 0] for MG, +1 for EG.
// `sq` is the square index from the piece-owner's perspective (Black is
// mirrored vertically inside pst_mg_value / pst_eg_value before indexing).

// The array itself is defined and initialised in eval.cpp.
extern int eval_weights[NUM_WEIGHTS];

// =============================================================================
// MOPUP EVALUATION CONSTANTS
// =============================================================================
// Used in pawnless endings to guide the winning king toward the losing king
// and push the losing king toward a corner. Without this term the engine
// can hold a clearly winning material advantage but fail to convert -- the
// PSTs alone are not sharp enough to drive an endgame mate.

// Minimum material advantage (in centipawns) required to activate mopup.
// Set at 300 cp: just under the value of a minor piece. Below this we
// consider the position too close to a possible draw to start chasing the
// enemy king. The exact value is conservative; we also have an explicit
// insufficient-material guard inside mopup_eval() for the specific drawn
// material combinations that exceed this threshold (KB vs K, KN vs K,
// KNN vs K, KBB-same-color vs K).
constexpr Score MOPUP_THRESHOLD = 300;

// Reward per unit of center distance of the losing king (0=center, 6=corner).
// Encourages driving the losing king toward the edge and into corners,
// where it has fewer escape squares and is easier to checkmate.
// Maximum contribution: 10 * 6 = 60 cp.
constexpr int MOPUP_CORNER_WEIGHT = 10;

// Reward per unit of king proximity, applied as (14 - manhattan_distance).
// The kings can never sit on adjacent squares (it would be self-check), so
// the closest they can legally be is 2 squares apart -- making the maximum
// realistic value 12, not 14. The 14 - dist form just keeps the sign positive.
// Encourages the winning king to close in on the losing king.
// Maximum contribution (in practice): 5 * 12 = 60 cp.
constexpr int MOPUP_PROXIMITY_WEIGHT = 5;

// =============================================================================
// EVALUATION FUNCTION
// =============================================================================

// Evaluate the position and return a score in centipawns from the perspective
// of board.side_to_move. Positive = good for the side to move, negative = bad
// for the side to move. The score combines material, piece-square tables,
// king safety, pawn structure, positional factors, and (in pawnless winning
// endings) the mopup term. Called from search at every leaf and at internal
// nodes that need static evaluation; runs in the hot path.
Score evaluate(const Board& board);

// Print a per-component breakdown of evaluate() to stdout. Reproduces the
// exact logic of evaluate() but accumulates each term separately, so the
// caller can see which factors contribute to the final score. Used by the
// UCI "eval" debug command. Not in the search hot path -- runs once per
// command invocation.
void evaluate_verbose(const Board& board);

// =============================================================================
// EVAL TRACE
// =============================================================================
// EvalTrace captures, for a single position, the contribution of every
// tunable feature in a form that external optimisation tooling can consume.
//
// Concept. The evaluation function can be written as
//
//     score = blend( sum_mg(coef_i * weight_i), sum_eg(coef_i * weight_i) )
//           + additional_terms
//
// where each entry of `eval_weights[]` is one slot of a phase pair (MG at
// even indices within each group, EG at odd indices), and `coef_i` is the
// signed count of how many times that slot was added (white contributions
// counted as positive, black contributions as negative). The
// `additional_terms` cover king safety and mopup, which lie outside the
// tunable array by design.
//
// trace_evaluate() fills out the per-slot count vector `coefficients[]`,
// the phase_mg the blend used, and the `additional_score` scalar. With
// these and a copy of `eval_weights[]`, any external consumer can
// reconstruct the score evaluate() produced, and -- more importantly --
// can recompute the score under a hypothetical alternate weight set
// without re-running the full evaluate() machinery:
//
//     mg_total  = sum over MG slots of  coefficients[i] * alt_weights[i]
//     eg_total  = sum over EG slots of  coefficients[i] * alt_weights[i]
//     blended   = (mg_total * phase_mg + eg_total * (256 - phase_mg)) / 256
//     score     = blended + additional_score
//
// (negate for Black to match evaluate()'s side-to-move convention.)
//
// This is what an iterative optimiser needs to do at every step.
//
// Contract (bit-exact). For every legal position:
//
//     evaluate(board) ==
//         score_from_trace(trace, eval_weights, board.side_to_move)
//
// where `trace` is filled by trace_evaluate(board, trace). The contract
// is checked at runtime by the UCI "trace" command on a sample of FENs;
// any divergence indicates a bug in trace_evaluate that has to be fixed
// before the trace can be trusted.
//
// Layout of `coefficients`. Indices match `eval_weights[]` slot for slot,
// so coefficients[i] is the count for weights[i]. The MG / EG roles of
// adjacent slots are part of the array layout (documented above with the
// WeightGroup enum) and are the same for both arrays.

struct EvalTrace {
    // One slot per entry in eval_weights[]. For each weight, this stores
    // the signed contribution count for that weight in the traced position:
    // positive when the feature appears net for White, negative for Black.
    int coefficients[NUM_WEIGHTS];

    // The phase blend weight used when the trace was generated. The
    // coefficients are stored unblended (raw counts); the consumer applies
    // the blend at reconstruction time. Range 0..256, same convention as
    // game_phase() internally.
    int phase_mg;

    // Score contribution from features that are NOT in eval_weights[]
    // (king safety, mopup). Already blended where applicable. From White's
    // perspective: positive if it helps White. The final sign-flip for
    // side to move is the caller's responsibility, just as it is in
    // evaluate().
    Score additional_score;
};

// Fill `trace` with the coefficient vector and additional-score contribution
// for `board`. The function is a parallel implementation of evaluate(): it
// walks the same code paths but, instead of multiplying counts by weights,
// records the raw counts. Not in the search hot path; called only from the
// UCI "trace" command and from external tooling that links against Facon.
void trace_evaluate(const Board& board, EvalTrace& trace);

// Compute the score implied by a (coefficients, phase, additional) trace
// under a given weight set. Returns the same value evaluate() would return
// for the position the trace came from, when called with `weights` equal to
// `eval_weights`. Provided as a building block so tooling that loads its
// own alternate weight set can reuse the engine's blending logic exactly.
//
// Convention: result is from `side_to_move`'s perspective, just like
// evaluate(). The trace itself does not know the side to move; the caller
// passes it as a parameter.
Score score_from_trace(const EvalTrace& trace,
                       const int*       weights,
                       Color            side_to_move);
