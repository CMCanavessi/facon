// =============================================================================
// Last modified: 2026-03-14 00:00
// timeman.cpp — Time management implementation
//
// Facon 1.1 — Herrumbre
//   Replaced the single fixed time budget with a soft/hard limit model.
//
//   soft_limit: the normal target time. After each completed iteration, if
//               elapsed >= soft_limit the search stops. This limit can be
//               extended dynamically (see extend_time) when the search detects
//               that the position is difficult.
//
//   hard_limit: the absolute ceiling. The search stops immediately at any
//               point when elapsed >= hard_limit, even mid-iteration. Set to
//               a multiple of the soft limit to allow extensions without
//               risking time trouble.
//
// Dynamic extension policy (applied in search.cpp after each iteration):
//   - PV move changed between iterations: x1.5 — the engine is reconsidering
//     its best move, which often means it needs more time to stabilize.
//   - Score dropped significantly (>= SCORE_DROP_THRESHOLD): x1.25 — the
//     engine found something bad; extra time helps find a way out.
//   Both conditions can apply simultaneously (multiplicative).
//   The extended soft limit is always capped at hard_limit.
//
// Facon 1.2 -- Rojo Vivo
//   - Verbosity: start() reports base, soft, and hard limits computed for each
//     move. extend_time() reports trigger reason, old/new soft, and headroom.
//     These are pure stdout writes with no effect on search correctness.
//   - Time forfeit fix (late 1.2): engine was losing ~75% of games on time in
//     the gauntlet due to three compounding issues: (a) HARD_FACTOR=3.0 was
//     too generous, (b) SAFETY_FACTOR=0.95 left only 5% headroom for OS
//     jitter/GUI latency, (c) no fixed overhead buffer for move transmission.
//     Fix: added OVERHEAD_MS (subtracted from remaining upfront), lowered
//     HARD_FACTOR to 2.0 and SAFETY_FACTOR to 0.90, added 100ms grace buffer
//     in should_stop() and a hard_limit floor guard. The hard_limit is also
//     capped at remaining/3 (was /2) to prevent single-move time blowout.
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
constexpr int MOVES_TO_GO = 30;

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
    start_time = Clock::now();
    stop       = false;

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
        soft_limit_ms = 600;
        hard_limit_ms = 1000;
        return;
    }

    // Subtract a fixed overhead before any calculation. This accounts for
    // move transmission time, GUI round-trip latency, and OS scheduling
    // jitter. All limits derived below are therefore safe even under load.
    int remaining = std::max(raw_remaining - OVERHEAD_MS, MIN_TIME_MS);

    // Base formula: spread remaining time over expected moves left,
    // then add most of the increment (replenished each move).
    int base_time = remaining / MOVES_TO_GO + increment * 3 / 4;

    // Apply safety margin and floor.
    base_time = int(base_time * SAFETY_FACTOR);
    base_time = std::max(base_time, MIN_TIME_MS);

    // Hard cap: never plan more than 1/3 of remaining time on a single move.
    // Tighter than the old /2 cap -- prevents single-move blowout at low clock.
    base_time = std::min(base_time, remaining / 3);

    // Soft limit: normal target, below base to leave room for extensions.
    soft_limit_ms = int(base_time * SOFT_FACTOR);
    soft_limit_ms = std::max(soft_limit_ms, MIN_TIME_MS);

    // Hard limit: absolute ceiling including extensions.
    // Capped at 1/3 of remaining (matching base_time cap).
    // Hard >= soft always.
    hard_limit_ms = int(base_time * HARD_FACTOR);
    hard_limit_ms = std::min(hard_limit_ms, remaining / 3);
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
// Stretches the soft limit by the given factor when the search detects a
// difficult position. The hard limit acts as a ceiling — extensions can never
// push the engine past the absolute time budget.
//
// Typical callers:
//   - PV move changed: extend_time(1.5)
//   - Score dropped significantly: extend_time(1.25)
// Both can apply in the same iteration (multiplicative effect).

void TimeManager::extend_time(double factor, const char* reason) {
    // No-op during infinite search -- limits are the sentinel value (1<<28),
    // there is nothing meaningful to extend or report.
    if (infinite) return;

    int old_soft  = soft_limit_ms;
    soft_limit_ms = int(soft_limit_ms * factor);
    soft_limit_ms = std::min(soft_limit_ms, hard_limit_ms);
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

// =============================================================================
// RESET
// =============================================================================

void TimeManager::reset() {
    time_white    = 0;
    time_black    = 0;
    inc_white     = 0;
    inc_black     = 0;
    movetime      = 0;
    depth_limit   = 0;
    infinite      = false;
    soft_limit_ms = 0;
    hard_limit_ms = 0;
    stop          = false;
}
