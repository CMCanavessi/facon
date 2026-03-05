// =============================================================================
// uci.h — Universal Chess Interface protocol handler
//
// Reads commands from stdin line by line and dispatches them to the engine.
// Sends responses to stdout. All I/O is text-based as per the UCI spec.
//
// Reference: https://www.shredderchess.com/chess-features/uci-universal-chess-interface.html
// =============================================================================

#pragma once

#include "board.h"
#include <sstream>
#include <string>

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
