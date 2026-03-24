// =============================================================================
// Last modified: 2026-03-12 12:30
// main.cpp — Facon Chess Engine entry point
//
// Initializes all subsystems and enters the UCI loop.
//
// Facon 1.0 -- Oxido
//   - Initial implementation: initialize magic bitboard tables and Zobrist
//     hash keys, then enter the UCI command loop.
//
// Facon 1.2 -- Rojo Vivo
//   - FACON_VERSION define: single source of truth for the version string,
//     used in the startup banner. Avoids version drift across files.
//   - isatty()-gated startup output: the engine banner (name, version, author)
//     and TT.print_info() are emitted to stderr only when stdin is a terminal
//     (IS_INTERACTIVE()). When launched by a GUI or fastchess these are
//     suppressed so the UCI handshake is not polluted.
//   - TT.print_info() placement: TT is a global constructed before main(), so
//     its constructor uses silent=true. We call print_info() here explicitly,
//     after the banner, to preserve the correct output order.
// =============================================================================

#include "bitboard.h"
#include "board.h"
#include "tt.h"
#include "uci.h"
#include <iostream>
#ifdef _WIN32
#  include <io.h>
#  define IS_INTERACTIVE() (_isatty(_fileno(stdin)))
#else
#  include <unistd.h>
#  define IS_INTERACTIVE() (isatty(fileno(stdin)))
#endif

#define FACON_VERSION "1.2"

int main() {
    // Only print startup messages when running interactively.
    // When launched by a GUI or fastchess, stderr is piped and these messages
    // would pollute the match output.
    bool interactive = IS_INTERACTIVE();

    if (interactive) {
        std::cerr << "Facon " << FACON_VERSION
                  << " by Carlos M. Canavessi, UCI Chess Engine\n\n";
    }

    // Initialize magic bitboard attack tables.
    // Must be called before any move generation or board operations.
    init_bitboards();

    // Initialize Zobrist hash keys.
    // Must be called before any board is set up.
    Zobrist::init();

    if (interactive) {
        // Print TT size here instead of in the constructor: TT is a global
        // object constructed before main() runs, so printing there would
        // appear before the banner. resize() uses silent=true in the
        // constructor; we report the initial size here in the right order.
        TT.print_info();
        std::cerr << "\n";
    }

    // Enter the UCI command loop.
    // Blocks here until the "quit" command is received.
    Uci.loop();

    return 0;
}
