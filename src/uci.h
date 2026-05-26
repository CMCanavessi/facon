// =============================================================================
// Last modified: 2026-05-14 23:15
// uci.h -- Universal Chess Interface protocol handler
//
// Reads commands from stdin line by line and dispatches them to the engine.
// Sends responses to stdout. All I/O is text-based as per the UCI spec.
//
// Reference: https://www.shredderchess.com/chess-features/uci-universal-chess-interface.html
//
// Facon 1.0 -- Oxido
//   - Initial implementation: full UCI command loop supporting uci, isready,
//     ucinewgame, position (startpos and fen), go, stop, setoption (Hash),
//     quit, and debug command d (display board).
//
// Facon 1.2 -- Rojo Vivo
//   - UCI threading: cmd_go() launches the search in a dedicated std::thread
//     (search_thread_) and returns immediately. The UCI loop can now read
//     stdin while the engine is searching, making "stop" functional.
//   - isatty()-gated prompt: the interactive "> " prompt is printed to stderr
//     only when stdin is a terminal.
//
// Facon 1.3 -- Yunque
//   - perft command: "perft N" counts leaf nodes to depth N from the current
//     position. "perft divide N" breaks the count down by first legal move.
//     Used to verify move generator correctness before any movegen changes.
//   - id name now uses FACON_VERSION from version.h (CMake-generated) instead
//     of a hardcoded string.
//
// Facon 1.4 -- Hoja
//   - bench command: "bench" or "bench depth N" searches a fixed set of 10
//     positions at a given depth (default 15) and reports total nodes, time,
//     and NPS. Used to measure the impact of optimization changes (LMR table,
//     move scores, make/unmake) by comparing NPS before and after.
//   - move_to_uci() deduplication: moved from static functions in search.cpp
//     and uci.cpp to a shared inline function in types.h.
//
// Facon 1.5 -- Espiga
//   - run_bench(): public entry point that wraps the private cmd_bench()
//     handler. Used by main.cpp to support invoking the benchmark from the
//     command line (./facon-1.5 bench). Accepts the same argument string as
//     the UCI "bench" command ("verbose", "depth N", or both).
// =============================================================================

#pragma once

#include "board.h"
#include <sstream>
#include <string>
#include <thread>

// =============================================================================
// UCI HANDLER
// =============================================================================

class UCI {
public:
    // Enter the main UCI loop. Reads from stdin until "quit" is received.
    void loop();

    // Run the benchmark once (with optional "verbose" and/or "depth N" flags)
    // and return. Provided as a public entry point so the binary can be
    // invoked as `./facon-X.Y bench [args]` without entering the UCI loop.
    // Internally constructs an istringstream and dispatches to cmd_bench().
    void run_bench(const std::string& args);

private:
    // The current board position — updated by "position" commands
    Board board_;

    // Thread running the current search. Launched by cmd_go(), joined by
    // cmd_stop() or at the end of the search when bestmove is printed.
    // Using a dedicated thread allows the UCI loop to keep reading stdin
    // (specifically "stop") while the engine is searching.
    std::thread search_thread_;

    // -------------------------------------------------------------------------
    // COMMAND HANDLERS
    // -------------------------------------------------------------------------

    // "uci" — print engine identity and supported options, then "uciok"
    void cmd_uci();

    // "isready" — respond with "readyok" once all initialization is complete
    void cmd_isready();

    // "ucinewgame" — clear state from the previous game
    void cmd_ucinewgame();

    // "position startpos [moves ...]"
    // "position fen <fen> [moves ...]"
    void cmd_position(std::istringstream& ss);

    // "go [wtime X] [btime X] [winc X] [binc X] [movetime X] [depth X]
    //     [movestogo X] [infinite]"
    void cmd_go(std::istringstream& ss);

    // "stop" — abort the current search immediately
    void cmd_stop();

    // "setoption name <name> value <value>" — apply a configuration change
    void cmd_setoption(std::istringstream& ss);

    // "d" — display the current board (debug command, not part of UCI spec)
    void cmd_display();

    // "eval" -- print a per-component breakdown of the static evaluation of
    // the current position (debug command, not part of UCI spec). Shows
    // material, PST, king safety, pawn structure, positional, and mopup
    // contributions so the user can diagnose evaluation changes.
    void cmd_eval();

    // "perft N" / "perft divide N" — count leaf nodes to depth N.
    // Not part of the UCI spec. Used to verify move generator correctness.
    // "perft N" prints total nodes and elapsed time.
    // "perft divide N" also prints the node count per first legal move.
    void cmd_perft(std::istringstream& ss);

    // "bench" / "bench depth N" — search a fixed set of 10 positions at
    // depth N (default 18) and report total nodes, time, and NPS.
    // Not part of the UCI spec. Used to measure optimization impact.
    // Deterministic: same depth always produces the same node count.
    void cmd_bench(std::istringstream& ss);

    // -------------------------------------------------------------------------
    // HELPERS
    // -------------------------------------------------------------------------

    // Parse a move string like "e2e4" or "e7e8q" into a Move object.
    // Generates all legal moves and returns the one that matches the string.
    // Returns MOVE_NONE if no legal move matches.
    Move parse_move(const std::string& str);
};

// Global UCI instance
extern UCI Uci;
