// =============================================================================
// tt.cpp — Transposition Table implementation
// =============================================================================

#include "tt.h"
#include <iostream>

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
    resize(mb);
}

void TranspositionTable::resize(int mb) {
    uint64_t bytes   = uint64_t(mb) * 1024 * 1024;
    uint64_t entries = bytes / sizeof(TTEntry);

    size_ = prev_power_of_two(entries);
    mask_ = size_ - 1;

    table_.assign(size_, TTEntry{});

    // Diagnostic output goes to stderr — stdout is reserved for UCI protocol
    std::cerr << "TT: " << mb << " MB ("
              << size_ << " entries, "
              << sizeof(TTEntry) << " bytes each)\n";
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

    // Move preservation: if the new result has no best move but we already
    // have one stored for this exact position, keep the old move.
    // This avoids losing the PV move when storing a lower-quality result.
    if (move == MOVE_NONE && entry.hash == hash)
        move = entry.move;

    entry.hash  = hash;
    entry.move  = move;
    entry.score = int16_t(score_to_tt(score, ply));
    entry.depth = int8_t(depth);
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

Move TranspositionTable::probe_move(uint64_t hash) const {
    const TTEntry& entry = table_[index(hash)];
    if (entry.hash != hash) return MOVE_NONE;
    return entry.move;
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
