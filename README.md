# Facón Chess Engine

<p align="center">
  <img src="docs/img/Logo.png" alt="Facon Chess Engine" width="320"/>
</p>

A UCI-compliant chess engine written in C++17.

*by Carlos M. Canavessi*

![Version](https://img.shields.io/badge/version-1.2%20Rojo%20Vivo-8B0000)
![Language](https://img.shields.io/badge/language-C%2B%2B17-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)
![Protocol](https://img.shields.io/badge/protocol-UCI-green)
![Elo](https://img.shields.io/badge/Ordo%20Elo-~1690-yellow)

---

## About

Facón is a chess engine built from scratch in C++17, designed as a learning project and long-term development platform. The name comes from the *facón* — a traditional Argentine gaucho knife, forged by hand, raw and functional.

Each version carries a codename that follows the knife-making process: from rough rusty iron to a sharp, precise blade.

---

## Current Version: 1.2 "Rojo Vivo"

> *The iron is hot. Time to strike.*

The third release, focused on search pruning, endgame technique, and engine observability. Measured at **+330 Elo** over version 1.1 (Ordo ~1690, confirmed across two gauntlets totaling 2080 games).

### What's new in 1.2

- **Null Move Pruning (NMP)** — if the side to move can pass their turn and the position still exceeds beta at reduced depth, prune immediately. The single largest search improvement in Facón's history: +5.6 average depth over 1.1 at long time controls.
- **Triangular PV array** — replaced TT-based PV retrieval with an explicit table updated on every alpha raise. Eliminates stale PV lines and the "PV continues after threefold repetition" GUI warning.
- **Mopup evaluation** — in pawnless endings with a decisive advantage, rewards pushing the losing king toward corners and closing the king distance. Guides conversion of technically won positions.
- **UCI threading** — the search now runs in a dedicated thread. The engine can receive and process `stop` while searching. Previously `stop` was ignored while the search was running.
- **Time management overhaul** — fixed systematic time forfeits caused by an overly aggressive hard limit. `HARD_FACTOR` lowered from 3.0 to 2.0, `OVERHEAD_MS = 100` added, hard limit capped at `remaining/3`. Zero time forfeit losses across 2080 gauntlet games.
- **Observability** — `currmove/currmovenumber` output at the root; new-best move announced in SAN with score, depth, and timestamp; heartbeat every 5 minutes at long time controls.
- **Pre-work bug fixes** — `unmake_move()` hash corruption (latent since 1.0, affecting TT hit rates and repetition detection), `make_null_move()` full-move counter corruption, double move generation in the TT probe path.

---

## Version History

| Version | Codename   | Ordo Elo | Gain |
|---------|------------|----------|------|
| 1.2     | Rojo Vivo  | ~1690    | +330 vs 1.1 |
| 1.1     | Herrumbre  | ~1360    | +140 vs 1.0 |
| 1.0     | Óxido      | ~1220    | baseline |

Gauntlet methodology: 26 opponents, 40 games each (1040 total), 2min+1sec, balanced opening book. Ordo rating computed across all versions in a combined rating list.

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

The resulting binary (`facon-1.2` / `facon-1.2.exe`) is statically linked and has no external dependencies.

---

## Usage

Facón communicates via the UCI protocol. Any UCI-compatible GUI works: [Arena](http://www.playwitharena.de/), [Cute Chess](https://cutechess.com/), [Banksia](https://banksiagui.com/).

### Quick start
```
$ ./facon-1.2
uci
id name Facon 1.2 - Rojo Vivo
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
│   ├── eval.h/.cpp     — Material + PST + king safety + mopup evaluation
│   ├── tt.h/.cpp       — Transposition table
│   ├── timeman.h/.cpp  — Time management
│   ├── search.h/.cpp   — Negamax alpha-beta, NMP, iterative deepening
│   ├── uci.h/.cpp      — UCI protocol handler
│   ├── main.cpp        — Entry point
│   └── version.rc      — Windows version metadata
├── cmake/
│   └── windows-cross.cmake
├── docs/
│   ├── v1.0.md         — Technical documentation for v1.0
│   ├── v1.1.md         — Technical documentation for v1.1
│   └── v1.2.md         — Technical documentation for v1.2
├── CMakeLists.txt
├── CHANGELOG.md
└── README.md
```

---

## Planned improvements

- **Search**: LMR, history heuristic, aspiration windows, singular extensions
- **Evaluation**: pawn structure (isolated, doubled, passed pawns), mobility, open files, rook on 7th, bishop pair
- **Time management**: depth-scaled extensions, easy move reduction
- **Architecture**: Syzygy tablebase support, ponder, NNUE (long-term)

---

## Author

**Carlos M. Canavessi**

---

## Acknowledgements

- [Chess Programming Wiki](https://www.chessprogramming.org/) — reference for all chess engine techniques
- [CCRL](https://www.computerchess.org.uk/ccrl/) — computer chess rating list
