// =============================================================================
// Last modified: 2026-04-25 21:29
// timeman.cpp -- Time management implementation
//
// Facon 1.1 — Herrumbre
//   Replaced the single fixed time budget with a soft/hard limit model.
//
// Facon 1.2 -- Rojo Vivo
//   - Verbosity: start() reports base, soft, and hard limits computed for each
//     move. extend_time() reports trigger reason, old/new soft, and headroom.
//   - Time forfeit fix: OVERHEAD_MS, HARD_FACTOR 2.0, SAFETY_FACTOR 0.90,
//     hard_limit cap remaining/3, STOP_GRACE_MS 100ms.
//
// Facon 1.3 -- Yunque
//   - extend_time() guard: if factor <= 1.0, returns immediately. A factor
//     <= 1.0 would silently shrink the soft limit; use reduce_time() instead.
//   - accumulated_ext_ cap REMOVED: the cap (MAX_EXTENSION_FACTOR=2.0) was
//     causing legitimate deep-search extensions to be silently dropped because
//     near-zero quadratic factors at low depths consumed the budget. The soft
//     limit is now bounded only by the hard limit.
//   - reduce_time(): mirrors extend_time() for easy positions. Multiplies the
//     soft limit by a factor < 1.0, floored at MIN_TIME_MS. Emits an info
//     string with hard and headroom (consistent with extend_time output).
//   - cancel_easy_move(): reverses a prior easy-move reduction by multiplying
//     the soft limit by 1/reduce_factor. Does NOT touch accumulated_ext_.
//     Emits an info string with hard and headroom.
//   - extend_time() depth parameter + complex position path: when depth >=
//     EMERGENCY_DEPTH (25) and new_soft > hard, instead of capping soft at
//     hard we raise hard to match soft (cap: 50% of raw remaining clock).
//     Both limits then rise together on subsequent extensions. A distinct
//     "complex position" message is emitted showing both limit changes, the
//     absolute ceiling, and remaining headroom.
//
// Facon 1.4 -- Hoja
//   - movestogo support: start() uses TM.movestogo when > 0, otherwise falls
//     back to the MOVES_TO_GO constant (25). Parsed from "go ... movestogo N"
//     in uci.cpp. Cleared in reset().
//   - Depth-only search fix: "go depth N" with no clock previously activated
//     the TM fallback (600ms soft / 1000ms hard), causing time extensions and
//     premature stops. Now treated like infinite — unlimited time, depth limit
//     controls when to stop.
//   - MOVES_TO_GO reduced from 30 to 25: more time per move, compensated by
//     the increment. Realistic for games averaging 35-45 moves at this level.
//   - Time cap relaxed from remaining/3 to remaining*2/5: slightly more
//     permissive, allows deeper searches in endgames where the tree narrows.
// =============================================================================

#include "timeman.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>

// Global instance
TimeManager TM;

// =============================================================================
// CONSTANTS
// =============================================================================

// Assumed number of moves remaining in the game when no movestogo is provided.
constexpr int MOVES_TO_GO = 25;

// Fraction of the base time budget used as the soft limit.
// Below 1.0 to leave headroom for dynamic extensions.
constexpr double SOFT_FACTOR = 0.6;

// Multiplier from soft limit to hard limit.
// Lowered from 3.0 to 2.0: the old value allowed extensions to triple the
// budget, which combined with PV instability caused systematic time forfeits.
constexpr double HARD_FACTOR = 2.0;

// Safety margin applied to all computed limits.
// Lowered from 0.95 to 0.90: the old 5% buffer was insufficient for GUI
// latency + OS scheduling jitter, especially at low clock times.
constexpr double SAFETY_FACTOR = 0.90;

// Fixed overhead deducted from the remaining clock before all calculations.
// Accounts for move transmission time, GUI round-trip, and OS scheduling
// jitter. Applied upfront so every limit derived from remaining is safe.
// 100ms is conservative -- typical UCI round-trip is 10-50ms.
constexpr int OVERHEAD_MS = 100;

// Grace buffer in should_stop(): stop OVERHEAD_MS before the hard limit.
// Ensures the engine stops and sends bestmove before the GUI clock expires,
// even if the last node batch takes longer than expected.
constexpr int STOP_GRACE_MS = 100;

// Minimum time budget per move in milliseconds.
constexpr int MIN_TIME_MS = 10;

// =============================================================================
// HELPERS
// =============================================================================

// Format milliseconds as h:mm:ss,ms for readability at long time controls.
// Pure integer arithmetic -- no floating point, no allocations.
static void format_time(char* buf, size_t sz, int64_t ms) {
    int64_t s   = (ms / 1000) % 60;
    int64_t m   = (ms / 60000) % 60;
    int64_t h   =  ms / 3600000;
    int64_t ms3 =  ms % 1000;
    std::snprintf(buf, sz, "%lld:%02lld:%02lld,%03lld",
                  (long long)h, (long long)m, (long long)s, (long long)ms3);
}

// =============================================================================
// START
// =============================================================================

void TimeManager::start(Color side) {
    start_time        = Clock::now();
    stop              = false;
    accumulated_ext_  = 1.0;
    raw_remaining_ms_ = (side == WHITE) ? time_white : time_black;

    // Infinite search: allocate an arbitrarily large time budget.
    // The search will only stop when an explicit "stop" command arrives.
    if (infinite) {
        soft_limit_ms = 1 << 28;  // ~74 hours — effectively unlimited
        hard_limit_ms = 1 << 28;
        return;
    }

    // Fixed movetime: use exactly the requested duration minus safety margin.
    // Soft and hard limits are the same — no extensions allowed.
    if (movetime > 0) {
        soft_limit_ms = int(movetime * SAFETY_FACTOR);
        hard_limit_ms = soft_limit_ms;
        return;
    }

    // --- Normal clock management ---
    int raw_remaining = (side == WHITE) ? time_white : time_black;
    int increment     = (side == WHITE) ? inc_white  : inc_black;

    if (raw_remaining <= 0) {
        if (depth_limit > 0) {
            // Depth-limited search with no clock: behave like infinite.
            // Setting infinite=true suppresses all TM output (extend_time,
            // reduce_time, start allocation report) and makes soft_stop()
            // and should_stop() always return false. The depth limit in
            // the search loop controls when to stop.
            infinite      = true;
            soft_limit_ms = 1 << 28;
            hard_limit_ms = 1 << 28;
        } else {
            // No clock and no depth limit — should not happen in normal UCI,
            // but provide a minimal safety fallback.
            soft_limit_ms = 600;
            hard_limit_ms = 1000;
        }
        return;
    }

    // Subtract a fixed overhead before any calculation. This accounts for
    // move transmission time, GUI round-trip latency, and OS scheduling
    // jitter. All limits derived below are therefore safe even under load.
    int remaining = std::max(raw_remaining - OVERHEAD_MS, MIN_TIME_MS);

    // Base formula: spread remaining time over expected moves left,
    // then add most of the increment (replenished each move).
    // Use movestogo from the GUI if provided, otherwise fall back to the
    // default assumption (MOVES_TO_GO = 25).
    int moves_left = (movestogo > 0) ? movestogo : MOVES_TO_GO;
    int base_time  = remaining / moves_left + increment * 3 / 4;

    // Apply safety margin and floor.
    base_time = int(base_time * SAFETY_FACTOR);
    base_time = std::max(base_time, MIN_TIME_MS);

    // Hard cap: never plan more than 2/5 of remaining time on a single move.
    // Slightly more permissive than the old /3 cap — allows deeper searches
    // in endgames where the tree is narrow and depth rises fast.
    base_time = std::min(base_time, remaining * 2 / 5);

    // Soft limit: normal target, below base to leave room for extensions.
    soft_limit_ms = int(base_time * SOFT_FACTOR);
    soft_limit_ms = std::max(soft_limit_ms, MIN_TIME_MS);

    // Hard limit: absolute ceiling including extensions.
    // Capped at 2/5 of remaining (matching base_time cap).
    // Hard >= soft always.
    hard_limit_ms = int(base_time * HARD_FACTOR);
    hard_limit_ms = std::min(hard_limit_ms, remaining * 2 / 5);
    hard_limit_ms = std::max(hard_limit_ms, soft_limit_ms);

    char t_soft[32], t_hard[32];
    format_time(t_soft, sizeof(t_soft), soft_limit_ms);
    format_time(t_hard, sizeof(t_hard), hard_limit_ms);

    std::cout << "info string TM: allocated "
              << t_soft << " soft / "
              << t_hard << " hard"
              << " (clock " << raw_remaining << "ms"
              << ", effective " << remaining << "ms"
              << ", inc " << increment << "ms)\n"
              << std::flush;
}

// =============================================================================
// ELAPSED TIME
// =============================================================================

int TimeManager::elapsed_ms() const {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start_time).count());
}

// =============================================================================
// SHOULD STOP (hard limit)
// =============================================================================
// Called every 2048 nodes inside the search.
// Returns true if the search must stop immediately.

bool TimeManager::should_stop() const {
    if (stop)     return true;   // External abort from UCI "stop" command
    if (infinite) return false;  // Never stop on time during infinite search
    // STOP_GRACE_MS before the hard limit: ensures bestmove is sent before
    // the GUI clock expires, even if the last 2048-node batch is slow.
    return elapsed_ms() >= hard_limit_ms - STOP_GRACE_MS;
}

// =============================================================================
// SOFT STOP
// =============================================================================
// Called in the iterative deepening loop after each completed iteration.
// Returns true if we have used enough time and should not start a new iteration.
// Unlike should_stop(), this does not abort mid-iteration.

bool TimeManager::soft_stop() const {
    if (stop)     return true;
    if (infinite) return false;
    return elapsed_ms() >= soft_limit_ms;
}

// =============================================================================
// EXTEND TIME
// =============================================================================
// Stretches the soft limit by the given factor when the search detects
// instability (PV change or score drop).
//
// Normal path (depth < EMERGENCY_DEPTH or new_soft <= hard):
//   soft = min(soft * factor, hard). The hard limit stays fixed.
//   Message: "TM: extend xF (reason) -- soft OLD -> NEW (hard H, headroom R) [T]"
//
// Complex position path (depth >= EMERGENCY_DEPTH and new_soft > hard):
//   The position is genuinely deep AND unstable. Instead of capping soft at
//   hard, we raise hard to match soft (cap: 50% of raw remaining clock).
//   Both limits rise together on subsequent extensions. This is triggered by
//   real instability — stable positions at depth 25+ would have fired the
//   easy-move reduction before reaching here.
//   Message: "TM: extend xF (reason, complex position) -- soft OLD -> NEW /
//             hard OLDH -> NEWH (absolute limit L, headroom R) [T]"

// Depth at which the complex position path is activated. This is intentionally
// a TM constant rather than a search constant — it governs TM behavior directly.
// At depth < EMERGENCY_DEPTH, extend_time() always caps soft at hard (normal).
static constexpr int EMERGENCY_DEPTH = 25;

void TimeManager::extend_time(double factor, const char* reason, int depth) {
    if (infinite)      return;
    if (factor <= 1.0) return;  // Use reduce_time() to shrink

    int new_soft = int(soft_limit_ms * factor);
    accumulated_ext_ *= factor;

    if (depth >= EMERGENCY_DEPTH && new_soft > hard_limit_ms) {
        // Complex position path: soft would exceed hard, so raise hard to match.
        // Both are capped at 50% of raw remaining clock — the absolute ceiling
        // for any single move regardless of instability.
        int absolute_limit = raw_remaining_ms_ / 2;
        new_soft           = std::min(new_soft, absolute_limit);
        int old_soft       = soft_limit_ms;
        int old_hard       = hard_limit_ms;
        soft_limit_ms      = new_soft;
        hard_limit_ms      = new_soft;  // hard matches soft exactly
        int headroom       = absolute_limit - soft_limit_ms;

        char t_now[32], t_soft_old[32], t_soft_new[32],
             t_hard_old[32], t_hard_new[32], t_abs[32], t_head[32];
        format_time(t_now,      sizeof(t_now),      elapsed_ms());
        format_time(t_soft_old, sizeof(t_soft_old), old_soft);
        format_time(t_soft_new, sizeof(t_soft_new), soft_limit_ms);
        format_time(t_hard_old, sizeof(t_hard_old), old_hard);
        format_time(t_hard_new, sizeof(t_hard_new), hard_limit_ms);
        format_time(t_abs,      sizeof(t_abs),      absolute_limit);
        format_time(t_head,     sizeof(t_head),     headroom);

        std::cout << "info string TM: extend x" << factor;
        if (reason && reason[0]) std::cout << " (" << reason << ", complex position)";
        std::cout << " -- soft " << t_soft_old << " -> " << t_soft_new
                  << " / hard " << t_hard_old << " -> " << t_hard_new
                  << " (absolute limit " << t_abs << ", headroom " << t_head << ")"
                  << " [" << t_now << "]\n"
                  << std::flush;
    } else {
        // Normal path: soft is capped at hard as usual.
        int old_soft  = soft_limit_ms;
        soft_limit_ms = std::min(new_soft, hard_limit_ms);
        int headroom  = hard_limit_ms - soft_limit_ms;

        char t_now[32], t_old[32], t_new[32], t_hard[32], t_head[32];
        format_time(t_now,  sizeof(t_now),  elapsed_ms());
        format_time(t_old,  sizeof(t_old),  old_soft);
        format_time(t_new,  sizeof(t_new),  soft_limit_ms);
        format_time(t_hard, sizeof(t_hard), hard_limit_ms);
        format_time(t_head, sizeof(t_head), headroom);

        std::cout << "info string TM: extend x" << factor;
        if (reason && reason[0]) std::cout << " (" << reason << ")";
        std::cout << " -- soft " << t_old << " -> " << t_new
                  << " (hard " << t_hard << ", headroom " << t_head << ")"
                  << " [" << t_now << "]\n"
                  << std::flush;
    }
}

// =============================================================================
// REDUCE TIME
// =============================================================================
// Shrinks the soft limit for easy positions — the engine already knows what
// to play. Floored at MIN_TIME_MS to always spend a minimal amount of time.
//
// Typical callers (from search.cpp go()):
//   - Mate line found at root:            reduce_time(0.05, "mate found")
//   - Single legal move at root:          reduce_time(0.10, "forced move")
//   - PV+score stable >= 7 iters, d > 12: reduce_time(0.40, "easy move")

void TimeManager::reduce_time(double factor, const char* reason) {
    if (infinite) return;

    int old_soft  = soft_limit_ms;
    soft_limit_ms = std::max(int(soft_limit_ms * factor), MIN_TIME_MS);
    int headroom  = hard_limit_ms - soft_limit_ms;

    char t_now[32], t_old[32], t_new[32], t_hard[32], t_head[32];
    format_time(t_now,  sizeof(t_now),  elapsed_ms());
    format_time(t_old,  sizeof(t_old),  old_soft);
    format_time(t_new,  sizeof(t_new),  soft_limit_ms);
    format_time(t_hard, sizeof(t_hard), hard_limit_ms);
    format_time(t_head, sizeof(t_head), headroom);

    std::cout << "info string TM: reduce x" << factor;
    if (reason && reason[0]) std::cout << " (" << reason << ")";
    std::cout << " -- soft " << t_old << " -> " << t_new
              << " (hard " << t_hard << ", headroom " << t_head << ")"
              << " [" << t_now << "]\n"
              << std::flush;
}

// =============================================================================
// CANCEL EASY MOVE
// =============================================================================
// Reverses a previous easy-move reduction before applying a time extension.
// Multiplies soft_limit by 1/reduce_factor (the reciprocal of whatever factor
// was used in the easy-move reduce_time call). Capped at hard_limit.
//
// Critically, this does NOT touch accumulated_ext_. A restoration is not a
// real extension — it simply undoes the easy-move discount.

void TimeManager::cancel_easy_move(double reduce_factor) {
    if (infinite) return;

    int old_soft  = soft_limit_ms;
    soft_limit_ms = std::min(int(soft_limit_ms / reduce_factor), hard_limit_ms);
    int headroom  = hard_limit_ms - soft_limit_ms;

    char t_now[32], t_old[32], t_new[32], t_hard[32], t_head[32];
    format_time(t_now,  sizeof(t_now),  elapsed_ms());
    format_time(t_old,  sizeof(t_old),  old_soft);
    format_time(t_new,  sizeof(t_new),  soft_limit_ms);
    format_time(t_hard, sizeof(t_hard), hard_limit_ms);
    format_time(t_head, sizeof(t_head), headroom);

    std::cout << "info string TM: cancel easy"
              << " -- soft " << t_old << " -> " << t_new
              << " (hard " << t_hard << ", headroom " << t_head << ")"
              << " [" << t_now << "]\n" << std::flush;
}

// =============================================================================
// RESET
// =============================================================================

void TimeManager::reset() {
    time_white        = 0;
    time_black        = 0;
    inc_white         = 0;
    inc_black         = 0;
    movetime          = 0;
    depth_limit       = 0;
    infinite          = false;
    movestogo         = 0;
    soft_limit_ms     = 0;
    hard_limit_ms     = 0;
    accumulated_ext_  = 1.0;
    raw_remaining_ms_ = 0;
    stop              = false;
}
