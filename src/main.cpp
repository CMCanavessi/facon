// =============================================================================
// main.cpp — Facón Chess Engine entry point
//
// Initializes all subsystems and enters the UCI loop.
// =============================================================================

#include "bitboard.h"
#include "board.h"
#include "uci.h"

int main() {
    // Initialize magic bitboard attack tables.
    // Must be called before any move generation or board operations.
    init_bitboards();

    // Initialize Zobrist hash keys.
    // Must be called before any board is set up.
    Zobrist::init();

    // Enter the UCI command loop.
    // Blocks here until the "quit" command is received.
    Uci.loop();

    return 0;
}
