// =============================================================================
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
// =============================================================================

#include "timeman.h"
#include <algorithm>
#include <chrono>    // std::chrono::steady_clock, duration_cast (also in timeman.h, explicit here for portability)

// Global instance
TimeManager TM;

// =============================================================================
// CONSTANTS
// =============================================================================

// Assumed number of moves remaining in the game when no movestogo is provided.
// A simple fixed estimate — more sophisticated engines adapt this based on
// game phase and material balance, but 30 is a reasonable average.
constexpr int MOVES_TO_GO = 30;

// Fraction of the base time budget used as the soft limit.
// The engine aims to finish within soft_limit under normal conditions.
// Set below 1.0 to leave headroom for dynamic extensions.
constexpr double SOFT_FACTOR = 0.6;

// Multiplier from soft limit to hard limit.
// The hard limit is how far extensions can push the actual search time.
// 3.0 means the engine can spend up to 3x the base budget if needed.
constexpr double HARD_FACTOR = 3.0;

// Safety margin applied to all computed limits.
// Ensures we never use 100% of the allocated time, leaving a small buffer
// for move transmission, GUI latency, and OS scheduling jitter.
constexpr double SAFETY_FACTOR = 0.95;

// Minimum time budget per move in milliseconds.
// Prevents the engine from spending 0ms on a move when the clock is critical.
constexpr int MIN_TIME_MS = 10;

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
    int remaining = (side == WHITE) ? time_white : time_black;
    int increment = (side == WHITE) ? inc_white  : inc_black;

    if (remaining <= 0) {
        // No clock information provided: fall back to a safe 1-second default
        soft_limit_ms = 600;   // 600ms soft
        hard_limit_ms = 1000;  // 1s hard
        return;
    }

    // Base formula: spread remaining time over expected moves left,
    // then add most of the increment (it is replenished each move).
    int base_time = remaining / MOVES_TO_GO + increment * 3 / 4;

    // Apply safety margin
    base_time = int(base_time * SAFETY_FACTOR);
    base_time = std::max(base_time, MIN_TIME_MS);

    // Hard cap on base_time: never plan to spend more than half the remaining
    // time on a single move, even before extensions are applied.
    base_time = std::min(base_time, remaining / 2);

    // Soft limit: the normal target. Below the base to leave room for extensions.
    soft_limit_ms = int(base_time * SOFT_FACTOR);
    soft_limit_ms = std::max(soft_limit_ms, MIN_TIME_MS);

    // Hard limit: the absolute ceiling including any extensions.
    // Capped at half the remaining time to prevent catastrophic time trouble.
    hard_limit_ms = int(base_time * HARD_FACTOR);
    hard_limit_ms = std::min(hard_limit_ms, remaining / 2);
    hard_limit_ms = std::max(hard_limit_ms, soft_limit_ms);  // Hard >= soft always
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
    return elapsed_ms() >= hard_limit_ms;
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

void TimeManager::extend_time(double factor) {
    soft_limit_ms = int(soft_limit_ms * factor);
    soft_limit_ms = std::min(soft_limit_ms, hard_limit_ms);  // Never exceed hard
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
