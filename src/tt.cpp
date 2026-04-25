// =============================================================================
// Last modified: 2026-04-19 02:02
// tt.cpp — Transposition Table implementation
//
// Facon 1.4 -- Hoja
//   - Depth-preferred replacement: store() now refuses to overwrite a deeper
//     entry for a different position. Only overwrites if the slot is empty,
//     the hash matches (same position, newer data), or the new depth >=
//     the stored depth. Prevents shallow searches from evicting expensive
//     deep results.
// =============================================================================

#include "tt.h"
#include <iostream>
#include <cstring>   // std::memset() in clear()

// Global TT instance
TranspositionTable TT;

// =============================================================================
// HELPERS
// =============================================================================

// Round down to the nearest power of two.
// The table size must be a power of two so we can use a fast bitmask
// (hash & mask) instead of a slow modulo (hash % size) for index computation.
static uint64_t prev_power_of_two(uint64_t n) {
    if (n == 0) return 1;
    uint64_t p = 1;
    while (p * 2 <= n) p *= 2;
    return p;
}

// =============================================================================
// CONSTRUCTOR / RESIZE
// =============================================================================

TranspositionTable::TranspositionTable(int mb) {
    resize(mb, /*silent=*/true);
}

void TranspositionTable::resize(int mb, bool silent) {
    uint64_t bytes   = uint64_t(mb) * 1024 * 1024;
    uint64_t entries = bytes / sizeof(TTEntry);

    size_ = prev_power_of_two(entries);
    mask_ = size_ - 1;
    mb_   = mb;

    table_.assign(size_, TTEntry{});

    // Diagnostic output goes to stderr — stdout is reserved for UCI protocol.
    // Suppressed during initial construction (silent=true) because global
    // objects are constructed before main() runs, before the banner is printed.
    if (!silent)
        print_info();
}

void TranspositionTable::clear() {
    std::memset(table_.data(), 0, size_ * sizeof(TTEntry));
}

// =============================================================================
// STORE
// =============================================================================

void TranspositionTable::store(uint64_t hash, Move move, Score score,
                                int depth, BoundType bound, int ply) {
    TTEntry& entry = table_[index(hash)];

    // Depth-preferred replacement: only overwrite an existing entry if at
    // least one of these conditions holds:
    //   1. The slot is empty (BOUND_NONE) — nothing to preserve.
    //   2. Same position (hash match) — newer data for the same position is
    //      always more current, even at shallower depth.
    //   3. New depth >= stored depth — deeper search is more valuable.
    //
    // This prevents a shallow search at depth 2 from evicting a deep result
    // at depth 15 for a different position. The cost: some shallow entries
    // for new positions are lost when a deep entry occupies the slot. This
    // is the right tradeoff — deep results are expensive to recompute.
    if (entry.bound != BOUND_NONE && entry.hash != hash
        && depth < int(entry.depth))
    {
        return;  // Refuse to overwrite a deeper entry for a different position
    }

    // Move preservation: if the new result has no best move but we already
    // have one stored for this exact position, keep the old move.
    // This avoids losing the PV move when storing a lower-quality result.
    if (move == MOVE_NONE && entry.hash == hash)
        move = entry.move;

    entry.hash  = hash;
    entry.move  = move;
    entry.score = int16_t(score_to_tt(score, ply));
    entry.depth = uint8_t(depth);
    entry.bound = bound;
}

// =============================================================================
// PROBE
// =============================================================================

bool TranspositionTable::probe(uint64_t hash, TTEntry& out) const {
    const TTEntry& entry = table_[index(hash)];

    // Verify the full hash to filter out index collisions (type-1 errors):
    // two different positions mapping to the same slot with different hashes.
    if (entry.hash  != hash)       return false;
    if (entry.bound == BOUND_NONE) return false;

    out = entry;
    return true;
}

// =============================================================================
// PRINT INFO
// =============================================================================

void TranspositionTable::print_info() const {
    std::cerr << "Setting Transposition Table (Hash) size to "
              << mb_ << " MB ("
              << size_ << " entries, "
              << sizeof(TTEntry) << " bytes each)\n";
}

// =============================================================================
// HASHFULL
// =============================================================================
// Sample the first 1000 entries to estimate table occupancy.
// Returns per-mille (0..1000) for the UCI "info hashfull" field.

int TranspositionTable::hashfull() const {
    int      used   = 0;
    uint64_t sample = std::min(size_, uint64_t(1000));
    for (uint64_t i = 0; i < sample; i++)
        if (table_[i].bound != BOUND_NONE) used++;
    return (used * 1000) / int(sample);
}
