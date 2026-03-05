// =============================================================================
// timeman.h — Time management
//
// Decides how much time to spend on the current move based on the remaining
// clock time, increment, and move number.
//
// The search calls should_stop() periodically and stops when time is up.
// =============================================================================

#pragma once

#include "types.h"
#include <chrono>
#include <atomic>

// Convenient aliases for our clock type
using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

// =============================================================================
// TIME MANAGER
// =============================================================================

struct TimeManager {

    // -------------------------------------------------------------------------
    // CONFIGURATION — set by UCI before each search
    // -------------------------------------------------------------------------

    // Remaining time on the clock for each side (milliseconds)
    int time_white = 0;
    int time_black = 0;

    // Increment per move for each side (milliseconds, Fischer clock)
    int inc_white  = 0;
    int inc_black  = 0;

    // Fixed time per move — overrides clock management when set.
    // Used for the "go movetime X" UCI command.
    int movetime   = 0;

    // Maximum search depth (0 = no limit, stop by time instead).
    // Used for the "go depth X" UCI command.
    int depth_limit = 0;

    // Infinite search: never stop until an explicit "stop" command.
    // Used for the "go infinite" UCI command.
    bool infinite  = false;

    // -------------------------------------------------------------------------
    // INTERNAL STATE
    // -------------------------------------------------------------------------

    // Timestamp when the current search started
    TimePoint start_time;

    // How many milliseconds we have budgeted for this move
    int allocated_ms = 0;

    // Set to true by the UCI thread to abort the search immediately.
    // Must be atomic because it is written from one thread and read from another.
    std::atomic<bool> stop { false };

    // -------------------------------------------------------------------------
    // INTERFACE
    // -------------------------------------------------------------------------

    // Call at the start of each search to record the start time and compute
    // the time budget for this move. 'side' is the color currently moving.
    void start(Color side);

    // Returns the number of milliseconds elapsed since start() was called.
    int elapsed_ms() const;

    // Returns true if the search should stop immediately.
    // Called periodically inside the search to check time and stop signals.
    bool should_stop() const;

    // Reset all configuration fields to their defaults.
    // Call before populating with a new "go" command's parameters.
    void reset();
};

// Global time manager instance
extern TimeManager TM;
