// =============================================================================
// Last modified: 2026-05-01 15:37
// tt.cpp -- Transposition Table implementation
//
// Facon 1.4 -- Hoja
//   - Depth-preferred replacement: store() now refuses to overwrite a deeper
//     entry for a different position. Only overwrites if the slot is empty,
//     the hash matches (same position, newer data), or the new depth >=
//     the stored depth. Prevents shallow searches from evicting expensive
//     deep results.
//
// Facon 1.5 -- Espiga
//   - Generation-based aging: store() and probe() now use a generation
//     counter to identify stale entries. The counter advances once per
//     Search::go() via new_search(). store() may now replace an entry even
//     if the new depth is shallower, provided the stored entry is from a
//     generation that is sufficiently old. probe() refreshes the generation
//     of a hit entry to keep it from being aged out while still useful.
//     hashfull() now reports only entries from the current generation.
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

    size_       = prev_power_of_two(entries);
    mask_       = size_ - 1;
    mb_         = mb;
    generation_ = 0;  // Fresh table starts at generation 0

    table_.assign(size_, TTEntry{});

    // Diagnostic output goes to stderr -- stdout is reserved for UCI protocol.
    // Suppressed during initial construction (silent=true) because global
    // objects are constructed before main() runs, before the banner is printed.
    if (!silent)
        print_info();
}

void TranspositionTable::clear() {
    std::memset(table_.data(), 0, size_ * sizeof(TTEntry));
    generation_ = 0;  // Reset generation counter on TT.clear() (new game)
}

// Increment the generation counter. Wraps cyclically at 64 (6-bit field).
// The modular arithmetic in store()'s age computation handles the wrap
// correctly, so we don't need to do anything special when wrapping.
void TranspositionTable::new_search() {
    generation_ = (generation_ + 1) & 0x3F;
}

// =============================================================================
// STORE
// =============================================================================

void TranspositionTable::store(uint64_t hash, Move move, Score score,
                                int depth, BoundType bound, int ply) {
    TTEntry& entry = table_[index(hash)];

    BoundType stored_bound = bound_of(entry.gen_bound);
    uint8_t   stored_gen   = gen_of(entry.gen_bound);

    // Age = how many generations behind the current one is the stored entry.
    // Modular arithmetic (& 0x3F) handles the wrap-around at 64 generations:
    // if the counter wrapped from 63 to 0 and the stored gen is 62, the
    // subtraction gives -62 which becomes 2 after the mask -- correct.
    int age = int((generation_ - stored_gen) & 0x3F);

    // Replace if any of the following holds:
    //   1. Slot is empty (BOUND_NONE) -- nothing to preserve.
    //   2. Same position (hash match) -- newer data for the same position
    //      is always more current, even at shallower depth.
    //   3. New depth >= stored depth -- deeper search is more valuable.
    //   4. Stored entry is from an old generation (aging) -- the search
    //      has moved on and the old entry is no longer relevant. Threshold
    //      of 2 means "older than the immediately previous search".
    bool replace = (stored_bound == BOUND_NONE)
                || (entry.hash == hash)
                || (depth >= int(entry.depth))
                || (age >= 2);

    if (!replace)
        return;

    // Move preservation: if the new result has no best move but we already
    // have one stored for this exact position, keep the old move. This avoids
    // losing the PV move when storing a lower-quality result for the same
    // position (e.g. a fail-low storing BOUND_UPPER without a best move).
    if (move == MOVE_NONE && entry.hash == hash)
        move = entry.move;

    entry.hash      = hash;
    entry.move      = move;
    entry.score     = int16_t(score_to_tt(score, ply));
    entry.depth     = uint8_t(depth);
    entry.gen_bound = make_gen_bound(generation_, bound);
}

// =============================================================================
// PROBE
// =============================================================================

bool TranspositionTable::probe(uint64_t hash, TTEntry& out) {
    TTEntry& entry = table_[index(hash)];

    // Verify the full hash to filter out index collisions (type-1 errors):
    // two different positions mapping to the same slot with different hashes.
    if (entry.hash != hash)                    return false;
    if (bound_of(entry.gen_bound) == BOUND_NONE) return false;

    // Refresh the entry's generation so it isn't aged out while it's still
    // being used in the current search. This is why probe() is no longer
    // const: the side effect on the in-place entry is essential to aging.
    entry.gen_bound = make_gen_bound(generation_, bound_of(entry.gen_bound));

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
//
// Counts only entries from the CURRENT generation. This makes the value a
// meaningful indicator of how much of the TT contains data relevant to the
// search in progress -- entries from previous moves of the same game (still
// physically in the table but stale) are not counted.

int TranspositionTable::hashfull() const {
    int      used   = 0;
    uint64_t sample = std::min(size_, uint64_t(1000));
    for (uint64_t i = 0; i < sample; i++) {
        const TTEntry& e = table_[i];
        if (bound_of(e.gen_bound) != BOUND_NONE
            && gen_of(e.gen_bound) == generation_) {
            used++;
        }
    }
    return (used * 1000) / int(sample);
}
