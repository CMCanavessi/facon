// =============================================================================
// Last modified: 2026-04-06 00:06
// timeman.h — Time management
//
// Decides how much time to spend on the current move based on the remaining
// clock time, increment, and search feedback.
//
// The search calls should_stop() periodically and stops when time is up.
//
// Facon 1.1 -- Herrumbre
//   - Soft/hard limit model replaces the single fixed time budget.
//   - Dynamic extension (extend_time): soft limit stretched on PV instability
//     or score drop.
//
// Facon 1.2 -- Rojo Vivo
//   - extend_time() reason parameter; start() allocation report.
//   - Time forfeit fix: OVERHEAD_MS, HARD_FACTOR 2.0, SAFETY_FACTOR 0.90,
//     hard_limit cap remaining/3, STOP_GRACE_MS 100ms.
//
// Facon 1.3 -- Yunque
//   - reduce_time(): mirrors extend_time() for easy positions. Multiplies
//     the soft limit by a factor < 1.0, floored at MIN_TIME_MS. Triggers:
//     mate found (x0.05), forced move (x0.1), PV stable 7+ iters (x0.4).
//   - accumulated_ext_ cap REMOVED: near-zero quadratic factors at low depths
//     consumed the 2.0x budget, leaving no headroom for real extensions at
//     depth 14+. The soft limit is now bounded only by the hard limit.
//   - extend_time() guard: if factor <= 1.0, returns immediately without
//     effect. Use reduce_time() to shrink the soft limit.
//   - cancel_easy_move(): reverses a prior easy-move reduction before
//     applying a time extension. Multiplies soft limit by 1/factor.
//     Does NOT touch accumulated_ext_. Capped at hard_limit.
//   - extend_time() depth parameter: when depth >= EMERGENCY_DEPTH (25) and
//     the computed new soft would exceed the hard limit, instead of capping
//     soft at hard we raise hard to match soft (capped at 50% of the raw
//     remaining clock). This allows genuinely deep, unstable positions to use
//     more time without triggering the mechanism on stable easy positions.
//     Both soft and hard then rise together on subsequent extensions.
//     A distinct "complex position" message is emitted in this case.
//     raw_remaining_ms_ is stored at start() to make this computation
//     available throughout the search.
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

    // Accumulated extension factor since start(). Tracks the product of all
    // factors applied via extend_time(). No longer used as a hard cap —
    // the cap was removed because near-zero factors at low depths consumed
    // the budget before real extensions at depth 14+ could fire. Kept for
    // diagnostic output only. Reset to 1.0 in start() and reset().
    double accumulated_ext_ = 1.0;

    // Raw remaining clock time stored at start(), used by extend_time() to
    // compute the 50%-of-remaining ceiling for the complex position path.
    int raw_remaining_ms_ = 0;

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
    //
    // Normal path (depth < EMERGENCY_DEPTH or new_soft <= hard):
    //   soft = min(soft * factor, hard). Message: "TM: extend ... (hard ..., headroom ...)"
    //
    // Complex position path (depth >= EMERGENCY_DEPTH and new_soft > hard):
    //   hard is raised to match new_soft, capped at 50% of raw remaining clock.
    //   Both soft and hard rise together on subsequent extensions.
    //   Message: "TM: extend ... (PV change, complex position) ... / hard ... -> ... (absolute limit ..., headroom ...)"
    //
    // 'reason' is a short human-readable label emitted in the info string.
    // 'depth' is the current iterative deepening depth (default 0 = normal path).
    void extend_time(double factor, const char* reason = "", int depth = 0);

    // Shrink the soft limit by a given factor (e.g. 0.4 = 60% reduction).
    // Called when the position is clearly resolved — mate found, forced move,
    // or PV and score have been stable for several consecutive iterations.
    // The reduced limit is floored at MIN_TIME_MS.
    // 'reason' is a short human-readable label emitted in the info string.
    void reduce_time(double factor, const char* reason = "");

    // Cancel a previous easy-move reduction before applying a time extension.
    // Multiplies the soft limit by 1/reduce_factor (the reciprocal of whatever
    // factor was passed to reduce_time for the easy move). Does NOT touch
    // accumulated_ext_. Capped at hard_limit. Emits an info string.
    void cancel_easy_move(double reduce_factor);

    // Reset all configuration fields to their defaults.
    // Call before populating with a new "go" command's parameters.
    void reset();
};

// Global time manager instance
extern TimeManager TM;
