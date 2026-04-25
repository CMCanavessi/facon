// =============================================================================
// Last modified: 2026-04-13 08:53
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
//
// Facon 1.4 -- Hoja
//   - bench command: "bench" or "bench depth N" searches a fixed set of 10
//     positions at a given depth (default 15) and reports per-position node
//     counts and a total NPS figure. Positions are hardcoded and chosen to
//     stress different engine features: LMR, NMP, quiescence, king safety,
//     pawn structure, mopup, aspiration windows, and NMP zugzwang guards.
//     Deterministic: same depth = same nodes. Used to measure speedup from
//     optimization changes by comparing NPS before and after.
//   - move_to_uci() deduplication: the static helper was identical in
//     search.cpp and uci.cpp. Moved to types.h as an inline function.
//     Both files now use the shared version.
//   - movestogo UCI parameter: "go ... movestogo N" is now parsed and passed
//     to TM.movestogo. Previously ignored with a hardcoded assumption of 30.
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
        else if (token == "movestogo") ss >> TM.movestogo;
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
// COMMAND: bench (not part of UCI spec, used for performance measurement)
// =============================================================================
// Syntax: bench
//         bench depth <N>
//
// Searches a fixed set of 10 positions at the given depth (default 15) and
// reports per-position node counts and a total NPS figure. The positions are
// chosen to stress different engine features: LMR, NMP, quiescence, king
// safety, pawn structure, mopup, aspiration windows, and NMP zugzwang guards.
//
// Deterministic: same depth always produces the same total node count,
// regardless of the machine. Only the elapsed time (and thus NPS) varies.
// Run 3-5 times and average the NPS to account for OS scheduling jitter.
//
// The TT is cleared before each position so results are independent of
// search order. board_ is not modified (operates on a local copy).

// Bench positions: 10 hand-crafted positions covering all game phases and
// engine features. 100% original — not copied from any other engine.
static const char* BENCH_POSITIONS[] = {
    // 1. Opening — Italian Game move 4. Baseline NPS, TT warm-up.
    "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",

    // 2. Middlegame — Najdorf-like. ~35+ legal moves, stresses LMR + history.
    "r2q1rk1/1bp1bppp/p1np1n2/1p2p3/4P3/1BP2N1P/PP1P1PP1/RNBQR1K1 w - - 0 12",

    // 3. Middlegame — tactical. Captures in cascade, stresses quiescence.
    "r1b1r1k1/pp1n1ppp/2p1p3/q2P4/1bPN4/4BN2/PP2QPPP/R3K2R w KQ - 0 12",

    // 4. Middlegame — clear material advantage. Stresses NMP pruning.
    "r1b2rk1/pp2qppp/2n5/3p4/3Pn3/2PB1N2/PP1Q1PPP/R1B1R1K1 w - - 0 15",

    // 5. Middlegame closed — locked pawn chain. LMR + NMP guards.
    "r1b2rk1/pp1nqppp/2n1p3/2ppP3/3P4/2PB1N2/PP1NQPPP/R1B2RK1 w - - 0 12",

    // 6. King safety — exposed king on f7, coordinated attack.
    //    Pawn on e6 blocks the Bc4-f7 diagonal so the position is legal.
    "r1bq1r2/ppp2kpp/2n1pn2/2b1p1B1/2B1P3/3P1N2/PPP2PPP/RN1Q1RK1 w - - 0 8",

    // 7. Pawn structure — passed, isolated, doubled. All eval terms active.
    "r3r1k1/1p3pp1/p1p4p/P2pP3/1P1P4/2P2N1P/5PP1/R3R1K1 w - - 0 22",

    // 8. Endgame — rook + pawns, passed pawn on d5. Deep tree.
    "3r2k1/pp3ppp/8/3Pp3/8/4R1P1/PP3PKP/8 w - - 0 28",

    // 9. Endgame — pure pawn, zugzwang-prone. NMP guards tested.
    "8/8/1p1k4/pPp1p3/P1PpP3/3K4/8/8 w - - 0 40",

    // 10. Endgame — K+R vs K. Mopup evaluation active.
    "8/8/4k3/8/8/4K3/8/3R4 w - - 0 50",
};
static constexpr int BENCH_POSITION_COUNT = 10;
static constexpr int BENCH_DEFAULT_DEPTH  = 15;

// Null stream buffer: discards all output. Used by bench in quiet mode to
// suppress UCI info lines from Searcher.go() without modifying search code.
class NullBuffer : public std::streambuf {
protected:
    int overflow(int c) override { return c; }
};

void UCI::cmd_bench(std::istringstream& ss) {
    // Parse options in any order: depth (number or "depth N") and "verbose".
    // Examples: bench, bench 10, bench depth 10, bench verbose,
    //           bench 10 verbose, bench verbose 10, bench depth 15 verbose
    int  depth   = BENCH_DEFAULT_DEPTH;
    bool verbose = false;

    std::string token;
    while (ss >> token) {
        if (token == "verbose") {
            verbose = true;
        } else if (token == "depth") {
            if (!(ss >> depth) || depth < 1) {
                std::cout << "bench: invalid depth\n" << std::flush;
                return;
            }
        } else {
            // Try parsing as a number
            bool valid = !token.empty();
            for (char c : token) if (c < '0' || c > '9') { valid = false; break; }
            if (valid) {
                depth = 0;
                for (char c : token) depth = depth * 10 + (c - '0');
            } else {
                std::cout << "bench: unknown option '" << token << "'\n" << std::flush;
                return;
            }
        }
    }

    // Join any running search before we touch TM and Searcher
    if (search_thread_.joinable()) {
        TM.stop = true;
        search_thread_.join();
    }

    std::cout << "\nBenchmarking: " << BENCH_POSITION_COUNT
              << " positions, depth " << depth
              << (verbose ? ", verbose" : "") << "\n\n" << std::flush;

    // In quiet mode, redirect cout to a null buffer during the search so
    // UCI info lines, currmove, new-best, heartbeat, and ST debug lines
    // are all suppressed. Only our position summaries and final result
    // are printed (we restore cout before each print).
    NullBuffer         null_buf;
    std::streambuf*    orig_buf = std::cout.rdbuf();

    uint64_t total_nodes = 0;
    auto bench_start = std::chrono::steady_clock::now();

    for (int i = 0; i < BENCH_POSITION_COUNT; i++) {
        // Clear TT between positions so results are independent of order
        TT.clear();

        Board bench_board;
        bench_board.set_fen(BENCH_POSITIONS[i]);

        // Configure TM for a depth-limited search (no clock, no TM interference).
        // infinite=true makes soft_stop() and should_stop() always return false,
        // suppresses all TM extension/reduction messages, and prevents start()
        // from computing time limits. The depth_limit controls when to stop.
        TM.reset();
        TM.depth_limit = depth;
        TM.infinite    = true;

        // Suppress search output in quiet mode
        if (!verbose) std::cout.rdbuf(&null_buf);

        auto pos_start = std::chrono::steady_clock::now();
        Searcher.go(bench_board);
        auto pos_end = std::chrono::steady_clock::now();

        // Restore output for position summary
        if (!verbose) std::cout.rdbuf(orig_buf);

        uint64_t nodes = Searcher.total_nodes();
        total_nodes += nodes;

        int pos_ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(
            pos_end - pos_start).count());

        std::cout << "Position " << (i + 1) << "/" << BENCH_POSITION_COUNT
                  << ": " << nodes << " nodes, " << pos_ms << " ms\n"
                  << std::flush;
    }

    auto bench_end = std::chrono::steady_clock::now();
    int total_ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(
        bench_end - bench_start).count());
    int nps = (total_ms > 0) ? int(total_nodes * 1000 / total_ms) : 0;

    std::cout << "\nBench results: " << total_nodes << " nodes, "
              << total_ms << " ms, " << nps << " nps\n" << std::flush;
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
        else if (token == "bench")      cmd_bench(ss);
        else if (token == "quit") {
            TM.stop = true;
            if (search_thread_.joinable())
                search_thread_.join();
            break;
        }

        if (IS_INTERACTIVE()) std::cerr << "> " << std::flush;
    }
}
