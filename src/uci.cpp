// =============================================================================
// Last modified: 2026-05-27 15:23
// uci.cpp -- UCI protocol implementation
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
//
// Facon 1.5 -- Espiga
//   - cmd_setoption() Hash parse safety: replaced std::stoi() with the
//     same manual digit-parse pattern used by cmd_perft(). Release builds
//     are compiled with -fno-exceptions, so std::stoi on a non-numeric value
//     would call std::terminate() and crash the engine. Defensive against
//     malformed setoption commands (e.g. "setoption name Hash value abc").
//   - bench rebalancing: replaced 6 of 10 positions (1, 3, 7, 8, 9, 10) and
//     kept the other 4 (2, 4, 5, 6). Default depth raised from 15 to 18
//     for more meaningful measurements (1.4 set was 0.01% to 38.85% per
//     position at depth 18; new set is 2.7% to 16.7% -- much better
//     balanced). New BenchPosition struct couples each FEN with a label
//     describing the primary feature exercised. Verbose output now prints
//     position number, label, FEN, and the nodes/ms summary.
//   - cmd_eval(): new debug command "eval" that prints a per-component
//     breakdown of the static evaluation of the current position. Calls
//     evaluate_verbose() in eval.cpp. Used to diagnose evaluation changes
//     by comparing output before and after a code change. Not part of the
//     UCI spec; not in the search hot path.
//   - run_bench(): public wrapper around cmd_bench() so main.cpp can
//     dispatch the benchmark from the command line (./facon-1.5 bench
//     [verbose] [depth N]) without entering the UCI loop. Pure wrapper:
//     constructs an istringstream from the argument string and calls
//     cmd_bench() directly. No change to bench behaviour or output.
//
// Facon 1.6 -- Temple
//   - cmd_trace(): new debug command "trace" that calls trace_evaluate()
//     on the current position and prints the resulting coefficient vector
//     plus the additional_score scalar. Includes a built-in fidelity check
//     comparing evaluate() to the score reconstructed from the trace; any
//     mismatch is flagged in the output. Intended for inspection of the
//     coefficient layout that external evaluation tooling consumes. Not
//     part of the UCI spec; not in the search hot path.
//   - Comment audit pass: non-ASCII punctuation in comments replaced with
//     ASCII equivalents for portability. No functional changes.
// =============================================================================

#include "version.h"
#include "uci.h"
#include "search.h"
#include "tt.h"
#include "timeman.h"
#include "movegen.h"
#include "eval.h"
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
    // All initialization happens at startup -- we are always ready here
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
    // We capture board_ by value so the search works on a stable copy --
    // the UCI loop must not modify board_ while the search is running.
    // GUIs are expected to send "stop" before changing position, but this
    // is convention rather than a strict UCI spec requirement.
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
        // Parse manually -- exceptions are disabled in release builds
        // (-fno-exceptions), so std::stoi on a malformed value would call
        // std::terminate() and crash the engine. Defensive against malformed
        // setoption commands (e.g. "setoption name Hash value abc").
        bool valid = !value.empty();
        for (char c : value) if (c < '0' || c > '9') { valid = false; break; }
        if (!valid) {
            std::cout << "setoption: invalid Hash value\n" << std::flush;
            return;
        }
        int mb = 0;
        for (char c : value) mb = mb * 10 + (c - '0');
        TT.resize(mb);
    }
}

// =============================================================================
// COMMAND: d (display -- not part of UCI spec, used for debugging)
// =============================================================================

void UCI::cmd_display() {
    board_.print();
}

// =============================================================================
// COMMAND: eval (not part of UCI spec, used for evaluation debugging)
// =============================================================================
// Prints a per-component breakdown of the static evaluation of the current
// position. Useful for diagnosing evaluation changes -- run "eval" before
// and after a code change to see exactly which terms moved.
//
// The output shows material, PST, king safety, pawn structure, positional
// (mobility / outposts / files / etc.), and mopup contributions, with the
// final total from both White's perspective and the side to move's
// perspective.

void UCI::cmd_eval() {
    evaluate_verbose(board_);
}

// =============================================================================
// COMMAND: trace (not part of UCI spec, used for tuning-vector inspection)
// =============================================================================
// Syntax: trace
//
// Dumps the coefficient vector that trace_evaluate() produces for the
// current position, plus the additional-score scalar and the phase weight.
// Format: human-readable, grouped by feature, with non-zero coefficients
// only (the vector is large but sparse for any given position).
//
// Before printing, runs a fidelity check that compares the engine's
// evaluate() value with the score reconstructed from the trace under the
// live eval_weights[]. Any divergence is flagged at the top of the output
// and indicates a bug in trace_evaluate() that must be fixed before the
// trace can be trusted by external tooling.

void UCI::cmd_trace() {
    EvalTrace trace;
    trace_evaluate(board_, trace);

    Score engine_score = evaluate(board_);
    Score trace_score  = score_from_trace(trace, eval_weights, board_.side_to_move);

    std::cout << "=== trace ===\n";
    std::cout << "phase_mg         : " << trace.phase_mg << " /256\n";
    std::cout << "additional_score : " << trace.additional_score
              << " cp  (material + king safety + mopup, from White's POV)\n";
    std::cout << "engine evaluate(): " << engine_score
              << " cp  (from side-to-move's POV)\n";
    std::cout << "trace recons.    : " << trace_score
              << " cp  (from side-to-move's POV)\n";
    if (engine_score == trace_score) {
        std::cout << "fidelity         : OK (engine == trace)\n";
    } else {
        std::cout << "fidelity         : *** MISMATCH ***  delta = "
                  << (engine_score - trace_score)
                  << " cp -- trace_evaluate() has a bug\n";
    }
    std::cout << "\n";

    // Print the non-zero coefficients grouped by feature. Each line is:
    //   group_name [absolute_index]: coefficient
    // We walk all NUM_WEIGHTS slots, skip zeros, and let the indices speak
    // for themselves (the group enum in eval.h is the authoritative key).
    std::cout << "non-zero coefficients (idx: count):\n";
    int nonzero = 0;
    for (int i = 0; i < NUM_WEIGHTS; i++) {
        if (trace.coefficients[i] != 0) {
            std::cout << "  [" << i << "] = " << trace.coefficients[i] << "\n";
            nonzero++;
        }
    }
    std::cout << "(" << nonzero << " non-zero out of " << NUM_WEIGHTS << ")\n";
    std::cout.flush();
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
        // Parse manually -- exceptions are disabled in release builds (-fno-exceptions)
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

    // Operate on a copy -- the current position is not affected
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
//         bench <N>
//         bench verbose
//
// Searches a fixed set of 10 positions at the given depth (default 18) and
// reports per-position node counts and a total NPS figure. Each position is
// chosen to stress one specific engine feature (see labels below).
//
// Deterministic: same depth always produces the same total node count,
// regardless of the machine. Only the elapsed time (and thus NPS) varies.
// Run 3-5 times and average the NPS to account for OS scheduling jitter.
//
// The TT is cleared before each position so results are independent of
// search order. board_ is not modified (operates on a local copy).

struct BenchPosition {
    const char* label;
    const char* fen;
};

// Bench positions: 10 hand-crafted positions covering all game phases and
// engine features. Each label describes the phase and the primary feature
// being exercised. 100% original -- not copied from any other engine.
static const BenchPosition BENCH_POSITIONS[] = {
    { "Opening, Italian Game with c3 -- baseline NPS, TT cold-cache",
      "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/2P2N2/PP1P1PPP/RNBQK2R b KQkq - 0 4" },

    { "Middlegame, ~35 legal moves -- move ordering, LMR, history heuristic",
      "r2q1rk1/1bp1bppp/p1np1n2/1p2p3/4P3/1BP2N1P/PP1P1PP1/RNBQR1K1 w - - 0 12" },

    { "Middlegame with hanging exchanges -- quiescence search, SEE x-ray detection",
      "r2qr1k1/pp1n1ppp/2pb1n2/3p4/3P4/1QPB1N2/PP3PPP/R1B1R1K1 w - - 0 13" },

    { "Middlegame with material advantage -- null-move pruning effectiveness",
      "r1b2rk1/pp2qppp/2n5/3p4/3Pn3/2PB1N2/PP1Q1PPP/R1B1R1K1 w - - 0 15" },

    { "Middlegame closed structure -- NMP zugzwang guards, futility margins",
      "r1b2rk1/pp1nqppp/2n1p3/2ppP3/3P4/2PB1N2/PP1NQPPP/R1B2RK1 w - - 0 12" },

    { "Middlegame king attack -- king safety zone evaluation",
      "r1bq1r2/ppp2kpp/2n1pn2/2b1p1B1/2B1P3/3P1N2/PPP2PPP/RN1Q1RK1 w - - 0 8" },

    { "Middlegame with pawn structure tension -- isolated/doubled/connected eval",
      "r2q1rk1/pp1bbppp/2n1pn2/3p4/3P4/2NBPN2/PP3PPP/R1BQ1RK1 w - - 0 10" },

    { "Middlegame with checking sequences -- check extensions, deep tactics",
      "r3r1k1/pp1q1ppp/2n2n2/2bp4/3P1B2/2N2N2/PPQ1RPPP/3R2K1 w - - 0 14" },

    { "Endgame R+2P vs R -- deep search, passed pawn dynamics",
      "5k2/8/8/4PP2/8/8/r7/4K2R w K - 0 1" },

    { "Endgame KBN vs K -- mopup corner-distance evaluation",
      "8/8/8/4k3/8/3K4/8/4BN2 w - - 0 1" },
};
static constexpr int BENCH_POSITION_COUNT = 10;
static constexpr int BENCH_DEFAULT_DEPTH  = 18;

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
        bench_board.set_fen(BENCH_POSITIONS[i].fen);

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

        // Verbose-style summary: label, FEN, then nodes/ms.
        // Two-digit position number (" 1/10" .. "10/10") for vertical alignment.
        std::cout << "Position " << (i + 1 < 10 ? " " : "")
                  << (i + 1) << "/" << BENCH_POSITION_COUNT
                  << " -- " << BENCH_POSITIONS[i].label << "\n"
                  << "  fen: " << BENCH_POSITIONS[i].fen << "\n"
                  << "  " << nodes << " nodes, " << pos_ms << " ms\n\n"
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
// run_bench -- public entry point for CLI bench invocation
// =============================================================================
// Wraps cmd_bench() so that main.cpp can dispatch "bench" from the command
// line without having to construct an istringstream itself or expose the
// private command handler. The args string is parsed the same way as the
// argument to a UCI "bench" command (e.g. "verbose depth 22").

void UCI::run_bench(const std::string& args) {
    std::istringstream ss(args);
    cmd_bench(ss);
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
        else if (token == "eval")       cmd_eval();
        else if (token == "trace")      cmd_trace();
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
