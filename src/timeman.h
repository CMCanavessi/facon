// =============================================================================
// timeman.h — Time management
//
// Decides how much time to spend on the current move based on the remaining
// clock time, increment, and search feedback.
//
// The search calls should_stop() periodically and stops when time is up.
//
// Facon 1.1 — Herrumbre
//   - Soft/hard limit model replaces the single fixed time budget:
//       soft_limit: the target time. If a completed iteration exceeds this,
//                   the search stops normally. Can be extended dynamically.
//       hard_limit: the absolute maximum. The search stops immediately when
//                   this is exceeded, even mid-iteration.
//   - Dynamic extension: the soft limit is stretched when the search detects
//     instability (PV move changed) or a score drop between iterations.
//     This allows the engine to spend more time on difficult positions and
//     less on clear ones, rather than always using the same fixed budget.
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

    // Soft limit: target time budget for this move (milliseconds).
    // The search checks this after each completed iteration. If elapsed time
    // exceeds the soft limit, the search stops — unless a dynamic extension
    // has been applied (see extend_time()).
    int soft_limit_ms = 0;

    // Hard limit: absolute maximum time for this move (milliseconds).
    // The search stops immediately when this is exceeded, regardless of
    // whether the current iteration is complete.
    int hard_limit_ms = 0;

    // Set to true by the UCI thread to abort the search immediately.
    // Must be atomic because it is written from one thread and read from another.
    std::atomic<bool> stop { false };

    // -------------------------------------------------------------------------
    // INTERFACE
    // -------------------------------------------------------------------------

    // Call at the start of each search to record the start time and compute
    // the soft/hard time limits for this move. 'side' is the color to move.
    void start(Color side);

    // Returns the number of milliseconds elapsed since start() was called.
    int elapsed_ms() const;

    // Returns true if the search must stop immediately (hard limit or stop flag).
    // Called every 2048 nodes inside the search to minimize overhead.
    bool should_stop() const;

    // Returns true if the search should stop after completing the current
    // iteration (soft limit check). Called in the iterative deepening loop.
    bool soft_stop() const;

    // Extend the soft limit by a given factor (e.g. 1.5 = 50% more time).
    // Called when the search detects PV instability or a score drop.
    // The extended limit is capped at the hard limit to prevent time trouble.
    void extend_time(double factor);

    // Reset all configuration fields to their defaults.
    // Call before populating with a new "go" command's parameters.
    void reset();
};

// Global time manager instance
extern TimeManager TM;
