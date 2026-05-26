// =============================================================================
// Last modified: 2026-05-01 15:37
// tt.h -- Transposition Table
//
// A hash table that caches search results to avoid re-searching positions
// we've already analyzed. Indexed by the Zobrist hash of the position.
//
// Uses a fixed-size array. Collision resolution: depth-preferred with
// generation-based aging. New entries can replace existing entries when
// the slot is empty, the position matches (hash hit), the new depth is at
// least as great as the stored depth, OR the stored entry is from an old
// generation (the search has moved on and the old entry is no longer
// relevant). Each slot holds exactly one TTEntry.
//
// Facon 1.0 -- Oxido
//   - Initial implementation: Zobrist-indexed fixed-size hash table, always-
//     replace strategy, probe()/store() interface, configurable size via
//     UCI setoption Hash.
//
// Facon 1.1 -- Herrumbre
//   - Removed probe_move(): dead code. The root no longer relies on TT probes
//     for bestmove -- root_best_move_ is set directly in negamax(). Keeping
//     an unused public method that exposes internal table access was
//     unnecessary and misleading.
//
// Facon 1.2 -- Rojo Vivo
//   - Silent constructor: TT is a global object initialized before main().
//     The constructor now calls resize(mb, silent=true) so no output is
//     emitted during static initialization. print_info() is called explicitly
//     from main() after the banner, gated behind isatty().
//   - print_info(): new method that emits the TT size message on demand,
//     replacing the output that was previously in the constructor.
//   - resize(mb, silent=false): added silent parameter so the constructor
//     can suppress output while callers from setoption still get feedback.
//
// Facon 1.4 -- Hoja
//   - TTEntry::depth changed from int8_t (max 127) to uint8_t (max 255).
//     MAX_PLY is 128, which overflows int8_t but fits in uint8_t. The old
//     type was a latent bug -- depth 128 would store as -128, corrupting
//     the depth comparison in probe(). Entry size unchanged (16 bytes).
//   - Depth-preferred replacement in store(): shallow entries for different
//     positions no longer evict deeper entries. Overwrite only if the slot
//     is empty, same position, or new depth >= stored depth.
//
// Facon 1.5 -- Espiga
//   - Generation-based aging: TTEntry now packs a 6-bit generation counter
//     and a 2-bit bound type into a single byte (gen_bound), keeping the
//     entry size at 16 bytes. A generation counter advances once per search
//     (new_search()), and the replacement policy now allows overwriting an
//     entry from an old generation even if its depth is greater. This
//     reclaims slots occupied by stale data from earlier moves of the same
//     game, which were previously protected by depth-preferred replacement
//     even though they were no longer relevant.
//   - probe() is no longer const: it refreshes the generation of a hit
//     entry to the current generation, protecting entries actively used in
//     the current search from being aged out.
//   - hashfull() now counts only entries from the current generation, so
//     the value reflects how much of the TT contains data relevant to the
//     search in progress (not stale data from earlier moves).
// =============================================================================

#pragma once

#include "types.h"
#include <cstring>
#include <vector>

// =============================================================================
// BOUND TYPE
// =============================================================================
// Describes what kind of score bound is stored in a TT entry.

enum BoundType : uint8_t {
    BOUND_NONE  = 0,  // Entry is empty / invalid
    BOUND_EXACT = 1,  // Score is exact (all moves were searched)
    BOUND_LOWER = 2,  // Score is a lower bound (beta cutoff occurred)
    BOUND_UPPER = 3   // Score is an upper bound (all moves failed low)
};

// =============================================================================
// GENERATION + BOUND PACKING
// =============================================================================
// We pack the entry's generation (when it was last written or refreshed) and
// its bound type into a single byte, leaving the entry size at 16 bytes.
//
//   bit layout:  GGGGGG BB
//                 |      \-- bound type (2 bits, 4 values fit)
//                 \-- generation (6 bits, 64 values, wraps cyclically)
//
// 64 generations is enough: a chess game rarely exceeds 100 moves, and the
// modular arithmetic in age computation (see store()) handles wrap-around
// correctly. An entry would have to be >63 generations old AND survive that
// long without being overwritten by other criteria (depth, hash match) to
// hit a wrap edge case -- in practice such entries are evicted long before.

inline BoundType bound_of(uint8_t gen_bound) {
    return BoundType(gen_bound & 0x3);
}

inline uint8_t gen_of(uint8_t gen_bound) {
    return gen_bound >> 2;
}

inline uint8_t make_gen_bound(uint8_t gen, BoundType bound) {
    // gen is masked to 6 bits in case caller passes a wider value
    return uint8_t((gen & 0x3F) << 2) | uint8_t(bound);
}

// =============================================================================
// TT ENTRY -- 16 bytes exactly, no padding
// =============================================================================

struct TTEntry {
    uint64_t  hash;       // Full Zobrist hash for collision verification  (8 bytes)
    Move      move;       // Best move found from this position            (4 bytes)
    int16_t   score;      // Search score (mate-distance adjusted)         (2 bytes)
    uint8_t   depth;      // Depth at which this entry was searched        (1 byte, max 255)
    uint8_t   gen_bound;  // Packed generation (6 bits) + bound (2 bits)   (1 byte)
};

// =============================================================================
// TRANSPOSITION TABLE
// =============================================================================

class TranspositionTable {
public:
    // Create a TT with the given size in megabytes (default: 16 MB)
    explicit TranspositionTable(int mb = 16);

    // Resize and clear the table (used by the UCI "setoption Hash" command).
    // Pass silent=true to suppress the diagnostic message (used internally
    // during construction, before the engine banner is printed).
    void resize(int mb, bool silent = false);

    // Clear all entries (call at the start of a new game)
    void clear();

    // Store a search result. Overwrites the existing entry at this hash slot
    // when one of these conditions holds:
    //   - The slot is empty.
    //   - Same position (hash match).
    //   - New depth >= stored depth.
    //   - Stored entry is from an old generation (aging).
    // If 'move' is MOVE_NONE and an entry already exists for this hash,
    // the previously stored move is preserved.
    void store(uint64_t hash, Move move, Score score, int depth,
               BoundType bound, int ply);

    // Probe the table. Returns true and fills 'entry' if a valid entry exists
    // for this hash. Returns false if the slot is empty or hash doesn't match.
    // Side effect: on a successful probe, the entry's generation is refreshed
    // to the current generation. This protects entries that are still being
    // used in the current search from being evicted as "old" by aging.
    bool probe(uint64_t hash, TTEntry& entry);

    // Increment the generation counter. Called once at the start of each
    // search (Search::go()) so entries stored in previous searches age
    // naturally and become eligible for replacement when slots are contested.
    void new_search();

    // Estimate how full the table is (per-mille, 0..1000).
    // Counts only entries from the CURRENT generation, so the value reflects
    // how much of the TT contains data relevant to the search in progress.
    // Used for the UCI "info hashfull" output.
    int hashfull() const;

    // Print the current TT configuration to stderr.
    // Called from main() after the engine banner so output appears in order.
    void print_info() const;

private:
    std::vector<TTEntry> table_;       // Flat array of entries
    uint64_t             mask_;        // Bitmask for index computation (size - 1)
    uint64_t             size_;        // Number of entries (always a power of two)
    int                  mb_;          // Current size in MB (for print_info())
    uint8_t              generation_;  // Current generation counter (6 bits, wraps at 64)

    // Map a Zobrist hash to a table index using bitmasking (fast modulo)
    inline uint64_t index(uint64_t hash) const { return hash & mask_; }
};

// =============================================================================
// MATE SCORE ADJUSTMENT
// =============================================================================
// Mate scores encode distance-to-mate in plies. When stored in the TT they
// must be adjusted relative to the current ply so that a hit from a different
// search depth still produces the correct mate distance at the root.

// Adjust score before storing: make it relative to the TT (add ply offset)
inline Score score_to_tt(Score s, int ply) {
    if (s >=  SCORE_MATE - 512) return s + ply;
    if (s <= -SCORE_MATE + 512) return s - ply;
    return s;
}

// Adjust score after retrieval: make it relative to the current search ply
inline Score score_from_tt(Score s, int ply) {
    if (s >=  SCORE_MATE - 512) return s - ply;
    if (s <= -SCORE_MATE + 512) return s + ply;
    return s;
}

// Global TT instance (used throughout the engine)
extern TranspositionTable TT;
