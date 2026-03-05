// =============================================================================
// tt.h — Transposition Table
//
// A hash table that caches search results to avoid re-searching positions
// we've already analyzed. Indexed by the Zobrist hash of the position.
//
// Uses a fixed-size array. Collision resolution strategy: always overwrite,
// but preserve the stored move if the new entry has none (move preservation).
// Each slot holds exactly one TTEntry.
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
// TT ENTRY — 16 bytes exactly, no padding
// =============================================================================

struct TTEntry {
    uint64_t  hash;   // Full Zobrist hash for collision verification  (8 bytes)
    Move      move;   // Best move found from this position            (4 bytes)
    int16_t   score;  // Search score (mate-distance adjusted)         (2 bytes)
    int8_t    depth;  // Depth at which this entry was searched        (1 byte, max 127)
    BoundType bound;  // Type of score bound stored                    (1 byte)
};

// =============================================================================
// TRANSPOSITION TABLE
// =============================================================================

class TranspositionTable {
public:
    // Create a TT with the given size in megabytes (default: 16 MB)
    explicit TranspositionTable(int mb = 16);

    // Resize and clear the table (used by the UCI "setoption Hash" command)
    void resize(int mb);

    // Clear all entries (call at the start of a new game)
    void clear();

    // Store a search result. Overwrites the existing entry at this hash slot.
    // If 'move' is MOVE_NONE and an entry already exists for this hash,
    // the previously stored move is preserved.
    void store(uint64_t hash, Move move, Score score, int depth,
               BoundType bound, int ply);

    // Probe the table. Returns true and fills 'entry' if a valid entry exists
    // for this hash. Returns false if the slot is empty or hash doesn't match.
    bool probe(uint64_t hash, TTEntry& entry) const;

    // Returns the best move stored for this position, or MOVE_NONE if not found.
    // Faster than probe() when only the move is needed.
    Move probe_move(uint64_t hash) const;

    // Estimate how full the table is (per-mille, 0..1000).
    // Used for the UCI "info hashfull" output.
    int hashfull() const;

private:
    std::vector<TTEntry> table_;  // Flat array of entries
    uint64_t             mask_;   // Bitmask for index computation (size - 1)
    uint64_t             size_;   // Number of entries (always a power of two)

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
