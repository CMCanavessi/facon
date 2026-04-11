// =============================================================================
// Last modified: 2026-04-05 00:01
// uci.cpp — UCI protocol implementation
//
// Facon 1.0 -- Oxido
//   - Initial implementation: command dispatch loop, position parsing,
//     go command, setoption Hash, parse_move() via move generation.
//
// Facon 1.2 -- Rojo Vivo
//   - UCI threading: cmd_go() now launches the search in a dedicated
//     std::thread (search_thread_) and returns immediately.
//   - isatty()-gated prompt: the interactive "> " prompt is emitted to stderr
//     only when stdin is a terminal.
//
// Facon 1.3 -- Yunque
//   - perft command: static helper perft() recursively counts leaf nodes.
//     cmd_perft() handles "perft N" (total count + time) and
//     "perft divide N" (per-move breakdown + total). Operates on a copy
//     of board_ so the current position is not modified.
//   - id name now uses FACON_VERSION from version.h instead of a hardcoded
//     string. To change the version, edit CMakeLists.txt and rerun cmake.
//   - Race condition fix in cmd_ucinewgame(): if the search thread is still
//     active when "ucinewgame" arrives, it is joined before TT.clear() and
//     board_.set_startpos() execute. Previously TT.clear() (std::memset)
//     could race with TT.probe()/TT.store() in the search thread, causing
//     undefined behavior.
// =============================================================================

#include "version.h"
#include "uci.h"
#include "search.h"
#include "tt.h"
#include "timeman.h"
#include "movegen.h"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#ifdef _WIN32
#  include <io.h>
#  define IS_INTERACTIVE() (_isatty(_fileno(stdin)))
#else
#  include <unistd.h>
#  define IS_INTERACTIVE() (isatty(fileno(stdin)))
#endif

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
    std::cout << "id name Facon " << FACON_VERSION << "\n";
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
    // Join the search thread before touching shared state. If "ucinewgame"
    // arrives while the engine is still searching (e.g. after an abrupt
    // "stop" + "ucinewgame" sequence), TT.clear() (which uses std::memset)
    // would race with TT.probe() / TT.store() in the search thread, causing
    // undefined behavior. Joining first guarantees the thread is done.
    if (search_thread_.joinable()) {
        TM.stop = true;
        search_thread_.join();
    }
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

    // If a previous search thread is still joinable (e.g. the GUI sent "go"
    // without a prior "stop"), join it before launching a new one.
    if (search_thread_.joinable())
        search_thread_.join();

    // Launch the search in a dedicated thread so the UCI loop can keep
    // reading stdin. The thread prints "bestmove" when the search finishes.
    // We capture board_ by value so the search works on a stable copy —
    // the UCI loop must not modify board_ while the search is running
    // (the GUI is required by the UCI spec to send "stop" before "position").
    Board search_board = board_;
    search_thread_ = std::thread([search_board]() mutable {
        SearchResult result = Searcher.go(search_board);
        std::cout << "bestmove " << move_to_uci(result.best_move) << "\n"
                  << std::flush;
    });
}

// =============================================================================
// COMMAND: stop
// =============================================================================

void UCI::cmd_stop() {
    // Signal the search to abort. The search checks TM.should_stop() every
    // 2048 nodes and sets abort_search_, which unwinds the recursion cleanly.
    TM.stop = true;

    // Wait for the search thread to finish and print "bestmove" before we
    // return. This ensures the GUI always gets a response to its "stop".
    if (search_thread_.joinable())
        search_thread_.join();
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
// COMMAND: perft (not part of UCI spec, used for movegen validation)
// =============================================================================
// Syntax: perft <depth>
//         perft divide <depth>
//
// Recursively counts leaf nodes to exactly the given depth without evaluating.
// A single wrong node count at any depth indicates a movegen or make/unmake bug.
//
// Bulk-counting optimization at depth 1: count legal moves directly without
// making each one, avoiding an extra make/unmake per leaf node.
//
// Known correct values from the starting position:
//   depth 1:         20       depth 4:    197,281
//   depth 2:        400       depth 5:  4,865,609
//   depth 3:      8,902       depth 6: 119,060,324

static uint64_t perft(Board& board, int depth) {
    MoveList moves;
    generate_all_moves(board, moves);

    if (depth == 1) {
        // Bulk-count: avoid make/unmake at the leaf level
        uint64_t count = 0;
        for (int i = 0; i < moves.count; i++)
            if (board.is_legal(moves.moves[i])) count++;
        return count;
    }

    uint64_t nodes = 0;
    for (int i = 0; i < moves.count; i++) {
        Move m = moves.moves[i];
        if (!board.is_legal(m)) continue;
        board.make_move(m);
        nodes += perft(board, depth - 1);
        board.unmake_move(m);
    }
    return nodes;
}

void UCI::cmd_perft(std::istringstream& ss) {
    std::string token;
    ss >> token;

    bool divide = false;
    int  depth  = 0;

    if (token == "divide") {
        divide = true;
        if (!(ss >> depth)) { std::cout << "perft divide: missing depth\n" << std::flush; return; }
    } else {
        // Parse manually — exceptions are disabled in release builds (-fno-exceptions)
        bool valid = !token.empty();
        for (char c : token) if (c < '0' || c > '9') { valid = false; break; }
        if (!valid) { std::cout << "perft: invalid depth\n" << std::flush; return; }
        depth = 0;
        for (char c : token) depth = depth * 10 + (c - '0');
    }

    if (depth <= 0) {
        std::cout << "perft: depth must be >= 1\n" << std::flush;
        return;
    }

    // Operate on a copy — the current position is not affected
    Board board = board_;

    auto t0 = std::chrono::steady_clock::now();
    uint64_t total = 0;

    if (divide) {
        MoveList moves;
        generate_all_moves(board, moves);
        for (int i = 0; i < moves.count; i++) {
            Move m = moves.moves[i];
            if (!board.is_legal(m)) continue;
            board.make_move(m);
            uint64_t count = (depth == 1) ? 1 : perft(board, depth - 1);
            board.unmake_move(m);
            total += count;

            // Print move in UCI format
            Square from = from_sq(m), to = to_sq(m);
            std::cout << char('a' + file_of(from)) << char('1' + rank_of(from))
                      << char('a' + file_of(to))   << char('1' + rank_of(to));
            if (move_type(m) == PROMOTION) {
                const char promo[] = "nbrq";
                std::cout << promo[promotion_type(m) - KNIGHT];
            }
            std::cout << ": " << count << "\n";
        }
    } else {
        total = perft(board, depth);
    }

    auto t1 = std::chrono::steady_clock::now();
    int  ms  = int(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    std::cout << "\nNodes: " << total << "\n";
    std::cout << "Time:  " << ms << " ms";
    if (ms > 0) std::cout << " (" << (total / ms) << "K nps)";
    std::cout << "\n" << std::flush;
}

// =============================================================================
// MAIN UCI LOOP
// =============================================================================
// Reads one command per line from stdin and dispatches to the appropriate
// handler. Unknown commands are silently ignored as required by the UCI spec.

void UCI::loop() {
    board_.set_startpos();

    if (IS_INTERACTIVE()) std::cerr << "> " << std::flush;

    std::string line, token;

    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            if (IS_INTERACTIVE()) std::cerr << "> " << std::flush;
            continue;
        }

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
        else if (token == "perft")      cmd_perft(ss);
        else if (token == "quit") {
            TM.stop = true;
            if (search_thread_.joinable())
                search_thread_.join();
            break;
        }

        if (IS_INTERACTIVE()) std::cerr << "> " << std::flush;
    }
}
