# Changelog

All notable changes to Facón will be documented here.

---

## [1.1] "Herrumbre" — 2026-03-11

**+120 Elo over 1.0** (gauntlet testing vs field Elo ~1358, 1040 games at 2min+1sec)

### Search
- **Killer move heuristic**: quiet moves that cause a beta cutoff are stored per-ply (two slots per node) and tried before other quiet moves in sibling nodes. Improves move ordering in quiet positions where MVV-LVA has no effect.
- **Dynamic time management**: replaced the fixed time budget with a soft/hard limit model. The soft limit is extended when the PV move changes between iterations (×1.5) or the score drops by ≥30 centipawns (×1.25). The hard limit acts as an absolute ceiling.
- **seldepth tracking**: maximum depth reached including quiescence search is tracked in `stats_.seldepth` and reported in the UCI info line each iteration.

### Evaluation
- **King safety**: penalty for enemy pieces attacking the king zone (king square plus adjacent squares). Attack weights: Knight=2, Bishop=2, Rook=3, Queen=5 (queens counted twice — once as diagonal, once as straight attacker). Penalty grows quadratically with total weight and scales with game phase (middlegame only).

### Stability
- **Abort flag** (`abort_search_`): when the hard time limit is hit inside the search, a flag is set and every level of the call stack returns immediately. Ensures all `unmake_move()` calls execute, leaving the board consistent for the next search. Previously, a direct return from deep in the recursion could leave `make_move()` calls unmatched.
- **Root best move tracking** (`root_best_move_`): the best move at ply 0 is saved directly inside `negamax()` whenever a new best is found. The search never does TT early returns at the root, so this is always set from the actual move loop — independent of TT state. Eliminates `bestmove 0000` outputs.
- **`is_legal()` piece ownership check**: `is_legal()` now verifies that the from-square contains a piece belonging to the side to move before performing the expensive board copy. Without this, stale TT entries pointing to empty squares or wrong-color pieces would pass the legality check silently, producing ghost moves in the PV output.
- **TT move validation**: before using a TT move for move ordering, it is verified against the generated move list. Hash collisions can produce moves valid in a different position; this check ensures garbage never enters the sort.
- **TT pollution prevention**: if `abort_search_` is set when returning from a child node, the result is discarded without storing anything in the TT. Storing a score of 0 from an incomplete search would pollute the table and cause the next search to make decisions based on garbage results.
- **`safe_move` pre-seed**: before the iterative deepening loop starts, the first legal move is found and stored as a last-resort fallback. Guarantees `bestmove` is never `0000` even if depth 1 is aborted before evaluating a single node.

### Cleanup
- `probe_move()` removed from `tt.h`/`tt.cpp` — was dead code once the root no longer relied on TT probes for bestmove.
- Non-ASCII characters removed from all source files (comments use `--` instead of `—`).
- All source file headers updated to `Facon 1.1 — Herrumbre`.

---

## [1.0] "Óxido" — 2026-03-05

Initial public release.

### Engine
- Bitboard board representation with redundant piece array for O(1) square queries
- Magic bitboards for sliding piece attack generation (rooks, bishops, queens)
- Zobrist hashing with incremental updates on make/unmake
- Full make/unmake move stack supporting any game length
- Draw detection: threefold repetition and 50-move rule

### Move Generation
- Complete pseudo-legal move generation for all piece types
- Separate capture-only generator for quiescence search
- Handles all special moves: castling, en passant, promotions (all four pieces)

### Search
- Negamax alpha-beta with iterative deepening
- Quiescence search at leaf nodes
- Transposition table (Zobrist hash, 16 MB default, configurable via UCI)
- Move ordering: TT move first, then MVV-LVA for captures, then quiet moves
- Time management with Fischer clock support and movetime override

### Evaluation
- Material count (P=100, N=320, B=330, R=500, Q=900)
- Piece-square tables for all piece types
- King PST interpolated between middlegame and endgame tables based on remaining material

### UCI
- Full UCI protocol support
- Supported commands: `uci`, `isready`, `ucinewgame`, `position`, `go`, `stop`, `setoption`, `quit`
- Supported options: `Hash` (transposition table size in MB)
- Debug command: `d` (display current board)

### Build
- CMake build system
- Linux native build (GCC/Clang, C++17)
- Windows cross-compilation via MinGW-w64
- Statically linked Windows binary (no DLL dependencies)
