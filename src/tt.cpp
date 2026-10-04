// =============================================================================
// Last modified: 2026-07-17 09:25
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
//
// Facon 1.7 -- Filo
//   - resize() hardened. Pipeline, in order: the request is clamped to
//     [HASH_MIN_MB, HASH_MAX_MB] (defense in depth -- the UCI parser clamps
//     first and reports to the user), capped so that at least
//     HASH_RAM_RESERVE_MB of physical RAM stays free (a table that lands in
//     swap turns TT probes into disk seeks), rounded down to a power of two,
//     and only then allocated. The old table is freed before the new one is
//     allocated so peak memory never holds both. Allocation uses
//     new(std::nothrow); on failure the size is halved and retried down to
//     a 1 MB floor. No Hash request can terminate the process anymore
//     (previously, an unsatisfiable size aborted via std::terminate under
//     -fno-exceptions). mb_ now records the size actually allocated, so
//     print_info() reports the truth (it used to echo the request even
//     when the table was rounded down).
//   - available_physical_mb(): new file-local helper reading MemAvailable
//     on Linux and GlobalMemoryStatusEx() on Windows. Returns 0 when it
//     cannot tell, in which case no RAM cap is applied.
//   - Size adjustments are reported as UCI "info string Hash ..." lines on
//     stdout (rounding, RAM cap, allocation fallback).
//   - store() carries the node's static evaluation into the entry
//     (entry.eval, EVAL_NONE for in-check nodes); the stored move shrank
//     to 16 bits (entry.move16) to make room without growing the entry.
//     Move preservation on same-hash refresh uses best_move().
// =============================================================================

#include "tt.h"
#include <iostream>
#include <cstring>   // std::memset() in clear()
#include <cstdio>    // std::fopen() in available_physical_mb() (Linux path)
#include <new>       // std::nothrow

#if defined(_WIN32)
// The build system also defines WIN32_LEAN_AND_MEAN for Windows targets
// (see CMakeLists.txt, Windows-specific settings); the guard keeps this
// translation unit self-contained without redefining it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GlobalMemoryStatusEx() in available_physical_mb()
#endif

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

// How much physical RAM (in MB) could the process claim right now without
// pushing the system into swap? Used by resize() to cap oversized requests:
// a transposition table bigger than available physical RAM gets paged out,
// and a TT probe that hits swap is a disk seek -- catastrophically slower
// than probing the smaller table we build instead.
//
// The reading is a snapshot and other processes keep allocating, so this is
// a best-effort cap, not a guarantee; the allocation fallback in resize()
// stays underneath as the safety net for genuine races.
//
// Returns 0 when availability cannot be determined (exotic system, API
// failure); the caller applies no cap in that case.
static uint64_t available_physical_mb() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms))
        return 0;
    return uint64_t(ms.ullAvailPhys) / (1024 * 1024);
#else
    // MemAvailable (Linux 3.14+) is the kernel's own estimate of how much
    // memory a new workload can claim without swapping -- unlike MemFree,
    // it counts reclaimable page cache. Parsed with C stdio: no exceptions,
    // no allocations, safe under -fno-exceptions.
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f)
        return 0;
    char               line[128];
    unsigned long long kb     = 0;
    uint64_t           result = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::sscanf(line, "MemAvailable: %llu", &kb) == 1) {
            result = uint64_t(kb) / 1024;
            break;
        }
    }
    std::fclose(f);
    return result;
#endif
}

// =============================================================================
// CONSTRUCTOR / RESIZE
// =============================================================================

TranspositionTable::TranspositionTable(int mb) {
    resize(mb, /*silent=*/true);
}

void TranspositionTable::resize(int mb, bool silent) {
    // Defense in depth: cmd_setoption() clamps and reports out-of-range
    // values before calling us, but no caller -- present or future -- gets
    // to reintroduce an out-of-range size. Silent by design: user-facing
    // range messages belong to the parser, which knows what the user typed.
    if (mb < HASH_MIN_MB) mb = HASH_MIN_MB;
    if (mb > HASH_MAX_MB) mb = HASH_MAX_MB;

    // Cap the request to available physical RAM minus a reserve, so the
    // table never lands in swap. avail == 0 means "could not determine":
    // no cap is applied and the allocation fallback below absorbs failures.
    uint64_t request_mb = uint64_t(mb);
    uint64_t capped_mb  = request_mb;
    uint64_t avail      = available_physical_mb();
    if (avail > 0) {
        uint64_t usable = (avail > uint64_t(HASH_RAM_RESERVE_MB))
                        ? avail - uint64_t(HASH_RAM_RESERVE_MB)
                        : uint64_t(HASH_MIN_MB);
        if (capped_mb > usable)
            capped_mb = usable;
    }

    uint64_t bytes   = capped_mb * 1024 * 1024;
    uint64_t entries = bytes / sizeof(TTEntry);

    size_ = prev_power_of_two(entries);

    // Free the old table BEFORE allocating the new one, so peak memory
    // never holds both tables at once. With large tables this is the
    // difference between a resize that fits and one that fails: growing
    // from 8 GB to 16 GB must not require 24 GB.
    table_.reset();

    // Allocate with new(std::nothrow). Under -fno-exceptions a failed
    // ordinary allocation calls std::terminate() -- the exact crash this
    // function must never produce. On failure, halve and retry down to a
    // 1 MB floor (halving preserves the power-of-two invariant).
    //
    // Residual risk, documented: Linux's default overcommit policy can
    // grant an allocation that physical memory cannot back, in which case
    // the OOM killer acts when the value-initialization below touches the
    // pages. No userspace program can defend against that. The RAM cap
    // above makes such a grant unlikely in the first place; the window
    // between our availability snapshot and the zeroing cannot be closed
    // from here.
    const uint64_t floor_entries = (1024 * 1024) / sizeof(TTEntry);  // 1 MB
    TTEntry* raw = nullptr;
    for (;;) {
        raw = new (std::nothrow) TTEntry[size_]();  // value-init: zeroed
        if (raw || size_ <= floor_entries)
            break;
        size_ /= 2;
    }
    // If even the 1 MB floor failed, raw is null here and the next
    // allocation anywhere in the process (an std::string, an I/O buffer)
    // dies the same way: a system in that state cannot run a chess engine
    // and no recovery is pretended. Unreachable in practice -- the RAM cap
    // above never requests more than what was available moments earlier.
    table_.reset(raw);

    mask_       = size_ - 1;
    mb_         = int((size_ * sizeof(TTEntry)) / (1024 * 1024));
    generation_ = 0;  // Fresh table starts at generation 0

    // Report size adjustments as UCI "info string" lines on stdout. At most
    // one of {RAM cap, power-of-two rounding} fires -- the RAM cap message
    // subsumes the rounding one, and both report the post-rounding size --
    // plus the allocation fallback only if a genuine race beat the cap.
    // Informational messages respect 'silent' (constructor runs before the
    // banner); an allocation failure prints unconditionally -- if that
    // fires, being heard matters more than being tidy.
    uint64_t attempted_mb = (prev_power_of_two(entries) * sizeof(TTEntry))
                          / (1024 * 1024);
    if (!silent && capped_mb < request_mb) {
        std::cout << "info string Hash value exceeds available RAM, reduced to "
                  << attempted_mb << " MB\n" << std::flush;
    } else if (!silent && attempted_mb != request_mb) {
        std::cout << "info string Hash value not a power of two, rounded down to "
                  << attempted_mb << " MB\n" << std::flush;
    }
    if (uint64_t(mb_) < attempted_mb) {
        std::cout << "info string Hash allocation failed, reduced to "
                  << mb_ << " MB\n" << std::flush;
    }

    // Diagnostic output goes to stderr -- stdout is reserved for UCI protocol.
    // Suppressed during initial construction (silent=true) because global
    // objects are constructed before main() runs, before the banner is printed.
    if (!silent)
        print_info();
}

void TranspositionTable::clear() {
    std::memset(table_.get(), 0, size_ * sizeof(TTEntry));
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
                                Score eval, int depth, BoundType bound,
                                int ply) {
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
        move = entry.best_move();

    entry.hash      = hash;
    entry.move16    = uint16_t(move);
    entry.score     = int16_t(score_to_tt(score, ply));
    entry.eval      = int16_t(eval);
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
