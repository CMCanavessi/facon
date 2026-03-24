// =============================================================================
// Last modified: 2026-03-12 12:30
// uci.h — Universal Chess Interface protocol handler
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
//     Previously cmd_go() blocked the loop and "stop" was never received.
//     cmd_stop() sets TM.stop and joins the thread, which unwinds cleanly
//     via the abort_search_ flag in Search.
//   - isatty()-gated prompt: the interactive "> " prompt is printed to stderr
//     only when stdin is a terminal. Suppressed when launched by a GUI or
//     fastchess so stdout stays clean for automated UCI parsing.
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

    // "go [wtime X] [btime X] [winc X] [binc X] [movetime X] [depth X] [infinite]"
    void cmd_go(std::istringstream& ss);

    // "stop" — abort the current search immediately
    void cmd_stop();

    // "setoption name <name> value <value>" — apply a configuration change
    void cmd_setoption(std::istringstream& ss);

    // "d" — display the current board (debug command, not part of UCI spec)
    void cmd_display();

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
