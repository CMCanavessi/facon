// =============================================================================
// uci.cpp — UCI protocol implementation
// =============================================================================

#include "uci.h"
#include "search.h"
#include "tt.h"
#include "timeman.h"
#include "movegen.h"
#include <iostream>
#include <sstream>
#include <string>

// Global instance
UCI Uci;

// =============================================================================
// HELPERS
// =============================================================================

// Convert a Move to its UCI string representation.
// Examples: normal move -> "e2e4", promotion -> "e7e8q", null -> "0000"
static std::string move_to_uci(Move m) {
    if (m == MOVE_NONE) return "0000";

    std::string s;
    Square from = from_sq(m);
    Square to   = to_sq(m);

    s += char('a' + file_of(from));
    s += char('1' + rank_of(from));
    s += char('a' + file_of(to));
    s += char('1' + rank_of(to));

    if (move_type(m) == PROMOTION) {
        const char promo[] = "nbrq";
        s += promo[promotion_type(m) - KNIGHT];
    }
    return s;
}

// =============================================================================
// MOVE PARSING
// =============================================================================
// Parse a UCI move string (e.g. "e2e4", "e7e8q") into a Move.
// We generate all pseudo-legal moves and return the one that matches
// both the string representation and passes legality checking.

Move UCI::parse_move(const std::string& str) {
    MoveList moves;
    generate_all_moves(board_, moves);

    for (int i = 0; i < moves.count; i++) {
        Move m = moves.moves[i];
        if (!board_.is_legal(m)) continue;
        if (move_to_uci(m) == str) return m;
    }
    return MOVE_NONE;
}

// =============================================================================
// COMMAND: uci
// =============================================================================

void UCI::cmd_uci() {
    std::cout << "id name Facon 1.1 - Herrumbre\n";
    std::cout << "id author Carlos M. Canavessi\n";
    std::cout << "\n";
    std::cout << "option name Hash type spin default 16 min 1 max 1024\n";
    std::cout << "uciok\n" << std::flush;
}

// =============================================================================
// COMMAND: isready
// =============================================================================

void UCI::cmd_isready() {
    // All initialization happens at startup — we are always ready here
    std::cout << "readyok\n" << std::flush;
}

// =============================================================================
// COMMAND: ucinewgame
// =============================================================================

void UCI::cmd_ucinewgame() {
    // Clear the TT: entries from a previous game can mislead the search
    TT.clear();
    board_.set_startpos();
}

// =============================================================================
// COMMAND: position
// =============================================================================
// Syntax: position startpos [moves e2e4 e7e5 ...]
//         position fen <fen_string> [moves ...]

void UCI::cmd_position(std::istringstream& ss) {
    std::string token;
    ss >> token;

    if (token == "startpos") {
        board_.set_startpos();
        ss >> token;  // Consume optional "moves" keyword
    }
    else if (token == "fen") {
        // FEN is up to 6 space-separated tokens; read until "moves" or end
        std::string fen;
        while (ss >> token && token != "moves") {
            if (!fen.empty()) fen += ' ';
            fen += token;
        }
        board_.set_fen(fen);
        // After the loop, 'token' is either "moves" or the last FEN field
    }

    // Apply the move list if present
    if (token == "moves") {
        while (ss >> token) {
            Move m = parse_move(token);
            if (m != MOVE_NONE)
                board_.make_move(m);
        }
    }
}

// =============================================================================
// COMMAND: go
// =============================================================================
// Syntax: go [wtime X] [btime X] [winc X] [binc X]
//            [movetime X] [depth X] [infinite] [movestogo X]

void UCI::cmd_go(std::istringstream& ss) {
    TM.reset();

    std::string token;
    while (ss >> token) {
        if      (token == "wtime")    ss >> TM.time_white;
        else if (token == "btime")    ss >> TM.time_black;
        else if (token == "winc")     ss >> TM.inc_white;
        else if (token == "binc")     ss >> TM.inc_black;
        else if (token == "movetime") ss >> TM.movetime;
        else if (token == "depth")    ss >> TM.depth_limit;
        else if (token == "infinite") TM.infinite = true;
        // "movestogo" is ignored — we always assume 30 moves remaining
    }

    SearchResult result = Searcher.go(board_);
    std::cout << "bestmove " << move_to_uci(result.best_move) << "\n"
              << std::flush;
}

// =============================================================================
// COMMAND: stop
// =============================================================================

void UCI::cmd_stop() {
    TM.stop = true;
}

// =============================================================================
// COMMAND: setoption
// =============================================================================
// Syntax: setoption name <name> value <value>

void UCI::cmd_setoption(std::istringstream& ss) {
    std::string token, name, value;

    ss >> token;
    if (token != "name") return;

    while (ss >> token && token != "value")
        name += (name.empty() ? "" : " ") + token;

    while (ss >> token)
        value += (value.empty() ? "" : " ") + token;

    if (name == "Hash") {
        int mb = std::stoi(value);
        TT.resize(mb);
    }
}

// =============================================================================
// COMMAND: d (display — not part of UCI spec, used for debugging)
// =============================================================================

void UCI::cmd_display() {
    board_.print();
}

// =============================================================================
// MAIN UCI LOOP
// =============================================================================
// Reads one command per line from stdin and dispatches to the appropriate
// handler. Unknown commands are silently ignored as required by the UCI spec.

void UCI::loop() {
    board_.set_startpos();

    std::string line, token;

    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        std::istringstream ss(line);
        ss >> token;

        if      (token == "uci")        cmd_uci();
        else if (token == "isready")    cmd_isready();
        else if (token == "ucinewgame") cmd_ucinewgame();
        else if (token == "position")   cmd_position(ss);
        else if (token == "go")         cmd_go(ss);
        else if (token == "stop")       cmd_stop();
        else if (token == "setoption")  cmd_setoption(ss);
        else if (token == "d")          cmd_display();
        else if (token == "quit")       break;
    }
}
