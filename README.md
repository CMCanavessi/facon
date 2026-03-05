# Facón Chess Engine

A UCI-compliant chess engine written in C++17.

*by Carlos M. Canavessi*

![Version](https://img.shields.io/badge/version-1.0%20Óxido-8B4513)
![Language](https://img.shields.io/badge/language-C%2B%2B17-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)
![Protocol](https://img.shields.io/badge/protocol-UCI-green)
![Elo](https://img.shields.io/badge/estimated%20elo-~1450--1500-yellow)

---

## About

Facón is a chess engine built from scratch in C++17, designed as a learning project and long-term development platform. The name comes from the *facón* — a traditional Argentine gaucho knife, forged by hand, raw and functional.

Each version carries a codename that follows the knife-making process: from rough rusty iron to a sharp, precise blade.

---

## Current Version: 1.0 "Óxido"

> *Rusty iron. Unpolished. But it cuts.*

The first public release. A complete, functional UCI chess engine built on solid foundations:

- Bitboard representation with magic bitboards for sliding piece attacks
- Negamax alpha-beta search with iterative deepening
- Quiescence search to resolve tactical sequences at leaf nodes
- Transposition table (16 MB default, configurable) with Zobrist hashing
- Material + Piece-Square Table evaluation with middlegame/endgame interpolation
- Draw detection: threefold repetition and 50-move rule
- Full UCI protocol support
- Time management with Fischer clock support

Estimated strength: **~1450–1500 Elo** (CCRL pending)

---

## Build

### Requirements
- C++17 compiler (GCC 10+ or Clang 12+)
- CMake 3.16+

### Linux
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Windows (cross-compile from Linux)
```bash
sudo apt install mingw-w64
mkdir build-windows && cd build-windows
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/windows-cross.cmake \
  -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The resulting binary (`facon-1.0` / `facon-1.0.exe`) is statically linked and has no external dependencies.

---

## Usage

Facón communicates via the UCI protocol. Any UCI-compatible GUI works: [Arena](http://www.playwitharena.de/), [Cute Chess](https://cutechess.com/), [Banksia](https://banksiagui.com/).

### Quick start
```
$ ./facon-1.0
uci
id name Facon 1.0 - Oxido
id author Carlos M. Canavessi
option name Hash type spin default 16 min 1 max 1024
uciok
isready
readyok
position startpos
go movetime 2000
info depth 1 score cp 50 nodes 21 nps 0 time 0 hashfull 0 pv b1c3
...
bestmove b1c3
```

### Supported UCI options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `Hash` | spin | 16 | Transposition table size in MB (1–1024) |

---

## Project Structure

```
facon/
├── src/
│   ├── types.h         — Core types: Square, Piece, Move, Bitboard
│   ├── bitboard.h/.cpp — Magic bitboards, attack tables
│   ├── board.h/.cpp    — Board state, make/unmake, Zobrist hashing
│   ├── movegen.h/.cpp  — Pseudo-legal move generation
│   ├── eval.h/.cpp     — Material + PST evaluation
│   ├── tt.h/.cpp       — Transposition table
│   ├── timeman.h/.cpp  — Time management
│   ├── search.h/.cpp   — Negamax alpha-beta, iterative deepening
│   ├── uci.h/.cpp      — UCI protocol handler
│   └── main.cpp        — Entry point
├── cmake/
│   └── windows-cross.cmake
├── docs/
│   └── v1.0.md         — Detailed technical documentation for v1.0
├── CMakeLists.txt
├── CHANGELOG.md
└── README.md
```

---

## Future Plans

Facón is under active development. Planned improvements include:

- **Search**: null move pruning, killer moves, history heuristic, late move reductions (LMR), aspiration windows
- **Evaluation**: incremental PST updates, king safety, pawn structure, open files, mobility
- **Architecture**: NNUE evaluation, multithreading (Lazy SMP), ponder support
- **Endgame**: Syzygy tablebase support

---

## Author

**Carlos M. Canavessi**

---

## Acknowledgements

- [Chess Programming Wiki](https://www.chessprogramming.org/) — reference for all chess engine techniques
- [CCRL](https://www.computerchess.org.uk/ccrl/) — computer chess rating list
