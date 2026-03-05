# Changelog

All notable changes to Facón will be documented here.

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
- King safety interpolated between middlegame and endgame tables based on remaining material

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
