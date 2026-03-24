// =============================================================================
// Last modified: 2026-03-13 00:00
// timeman.h — Time management
//
// Decides how much time to spend on the current move based on the remaining
// clock time, increment, and search feedback.
//
// The search calls should_stop() periodically and stops when time is up.
//
// Facon 1.1 -- Herrumbre
//   - Soft/hard limit model replaces the single fixed time budget:
//       soft_limit: the target time. If a completed iteration exceeds this,
//                   the search stops normally. Can be extended dynamically.
//       hard_limit: the absolute maximum. The search stops immediately when
//                   this is exceeded, even mid-iteration.
//   - Dynamic extension (extend_time): the soft limit is stretched when the
//     search detects instability (PV move changed) or a score drop between
//     iterations. Allows more time on difficult positions.
//
// Facon 1.2 -- Rojo Vivo
//   - extend_time() reason parameter: accepts optional label, emits
//     "info string TM extend xF (reason) -- soft Xms -> Yms [h:mm:ss,ms]".
//   - start() allocation report: emits soft/hard limits and effective clock
//     on each call, for real-time TM diagnostics.
//   - Time forfeit fix: OVERHEAD_MS subtracted from remaining upfront;
//     HARD_FACTOR lowered 3.0->2.0; SAFETY_FACTOR lowered 0.95->0.90;
//     hard_limit capped at remaining/3 (was /2); should_stop() returns true
//     STOP_GRACE_MS (100ms) before hard_limit to guarantee bestmove delivery.
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
    // 'reason' is a short human-readable label emitted in the info string.
    void extend_time(double factor, const char* reason = "");

    // Reset all configuration fields to their defaults.
    // Call before populating with a new "go" command's parameters.
    void reset();
};

// Global time manager instance
extern TimeManager TM;
