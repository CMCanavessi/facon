// =============================================================================
// timeman.cpp — Time management implementation
// =============================================================================

#include "timeman.h"
#include <algorithm>

// Global instance
TimeManager TM;

// =============================================================================
// CONSTANTS
// =============================================================================

// Assumed number of moves remaining in the game.
// A simple fixed estimate — real engines use adaptive models based on
// the move number and material balance.
constexpr int MOVES_TO_GO = 30;

// Safety margin applied to the computed time budget.
// Ensures we never use 100% of the allocated time, leaving a small buffer
// for overhead (move transmission, GUI latency, etc.)
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
        allocated_ms = 1 << 28;  // ~74 hours — effectively unlimited
        return;
    }

    // Fixed movetime: use exactly the requested duration minus safety margin
    if (movetime > 0) {
        allocated_ms = int(movetime * SAFETY_FACTOR);
        return;
    }

    // --- Normal clock management ---
    int remaining = (side == WHITE) ? time_white : time_black;
    int increment = (side == WHITE) ? inc_white  : inc_black;

    if (remaining <= 0) {
        // No clock information provided: fall back to a safe 1-second default
        allocated_ms = 1000;
        return;
    }

    // Base formula: spread remaining time over expected moves left,
    // then add most of the increment (it is replenished each move).
    int base_time = remaining / MOVES_TO_GO + increment * 3 / 4;

    // Apply safety margin and clamp to valid range
    allocated_ms = int(base_time * SAFETY_FACTOR);
    allocated_ms = std::max(allocated_ms, MIN_TIME_MS);

    // Hard cap: never spend more than half the remaining time on one move.
    // This prevents catastrophic time trouble if the position is complex.
    allocated_ms = std::min(allocated_ms, remaining / 2);
}

// =============================================================================
// ELAPSED TIME
// =============================================================================

int TimeManager::elapsed_ms() const {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start_time).count());
}

// =============================================================================
// SHOULD STOP
// =============================================================================

bool TimeManager::should_stop() const {
    if (stop)     return true;   // External abort signal from UCI "stop"
    if (infinite) return false;  // Never stop on time during infinite search
    return elapsed_ms() >= allocated_ms;
}

// =============================================================================
// RESET
// =============================================================================

void TimeManager::reset() {
    time_white   = 0;
    time_black   = 0;
    inc_white    = 0;
    inc_black    = 0;
    movetime     = 0;
    depth_limit  = 0;
    infinite     = false;
    allocated_ms = 0;
    stop         = false;
}
