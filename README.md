# Facón Chess Engine

<p align="center">
  <img src="Logo.png" alt="Facon Chess Engine" width="320"/>
</p>

A UCI-compliant chess engine written in C++17.

*by Carlos M. Canavessi*

![Version](https://img.shields.io/badge/version-1.1%20Herrumbre-8B4513)
![Language](https://img.shields.io/badge/language-C%2B%2B17-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)
![Protocol](https://img.shields.io/badge/protocol-UCI-green)
![Elo](https://img.shields.io/badge/estimated%20elo-~1360-yellow)

---

## About

Facón is a chess engine built from scratch in C++17, designed as a learning project and long-term development platform. The name comes from the *facón* — a traditional Argentine gaucho knife, forged by hand, raw and functional.

Each version carries a codename that follows the knife-making process: from rough rusty iron to a sharp, precise blade.

---

## Current Version: 1.1 "Herrumbre"

> *Still rusty. But now it holds an edge.*

The second release, focused on search improvements and stability. Measured at **+120 Elo** over version 1.0 (gauntlet testing vs field Elo ~1358, 1040 games at 2min+1sec).

### What's new in 1.1

- **Killer move heuristic** — quiet moves that caused a beta cutoff are stored per-ply and tried before other quiet moves in sibling nodes. Improves move ordering significantly in quiet positions where MVV-LVA has no effect.
- **King safety** — penalty for enemy pieces attacking the king zone. Grows quadratically with the number of attackers and scales with game phase (middlegame only).
- **Dynamic time management** — replaced the fixed time budget with a soft/hard limit model. The soft limit is extended when the PV move changes between iterations (×1.5) or the score drops significantly (×1.25).
- **seldepth tracking** — maximum depth reached including quiescence search, reported in the UCI info line each iteration.
- **Stability fixes** — abort flag ensures all `unmake_move()` calls execute on timeout; `root_best_move_` is always set from the actual move loop at the root, never from a TT probe; `is_legal()` now checks piece ownership before the expensive board copy, eliminating ghost moves from stale TT entries.

---

## Version History

| Version | Codename  | Elo gain |
|---------|-----------|----------|
| 1.1     | Herrumbre | +120 vs 1.0 (~1360) |
| 1.0     | Óxido     | baseline (~1240) |

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

### Linux (optimized for your CPU, not distributable)
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DNATIVE=ON
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

The resulting binary (`facon-1.1` / `facon-1.1.exe`) is statically linked and has no external dependencies.

---

## Usage

Facón communicates via the UCI protocol. Any UCI-compatible GUI works: [Arena](http://www.playwitharena.de/), [Cute Chess](https://cutechess.com/), [Banksia](https://banksiagui.com/).

### Quick start
```
$ ./facon-1.1
uci
id name Facon 1.1 - Herrumbre
id author Carlos M. Canavessi
option name Hash type spin default 16 min 1 max 1024
uciok
isready
readyok
position startpos
go movetime 2000
info depth 1 seldepth 1 score cp 30 nodes 20 nps 0 time 0 hashfull 0 pv b1c3
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
│   ├── eval.h/.cpp     — Material + PST + king safety evaluation
│   ├── tt.h/.cpp       — Transposition table
│   ├── timeman.h/.cpp  — Time management
│   ├── search.h/.cpp   — Negamax alpha-beta, iterative deepening
│   ├── uci.h/.cpp      — UCI protocol handler
│   └── main.cpp        — Entry point
├── cmake/
│   └── windows-cross.cmake
├── docs/
│   ├── v1.0.md         — Technical documentation for v1.0
│   └── v1.1.md         — Technical documentation for v1.1
├── CMakeLists.txt
├── CHANGELOG.md
└── README.md
```

---

## Future Plans

Facón is under active development. Planned improvements include:

- **Search**: null move pruning, history heuristic, late move reductions (LMR), aspiration windows
- **Evaluation**: incremental PST updates, pawn structure, open files, mobility
- **Architecture**: NNUE evaluation, multithreading (Lazy SMP), ponder support
- **Endgame**: Syzygy tablebase support

---

## Author

**Carlos M. Canavessi**

---

## Acknowledgements

- [Chess Programming Wiki](https://www.chessprogramming.org/) — reference for all chess engine techniques
- [CCRL](https://www.computerchess.org.uk/ccrl/) — computer chess rating list
