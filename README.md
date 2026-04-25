# Facón Chess Engine

<p align="center">
  <img src="docs/img/Logo.png" alt="Facon Chess Engine" width="320"/>
</p>

A UCI-compliant chess engine written in C++17.

*by Carlos M. Canavessi*

![Version](https://img.shields.io/badge/version-1.4%20Hoja-8B0000)
![Language](https://img.shields.io/badge/language-C%2B%2B17-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)
![Protocol](https://img.shields.io/badge/protocol-UCI-green)
![Elo](https://img.shields.io/badge/Ordo%20Elo-~2330-yellow)

---

## About

Facón is a chess engine built from scratch in C++17, designed as a learning project and long-term development platform. The name comes from the *facón* — a traditional Argentine gaucho knife, forged by hand, raw and functional.

Each version carries a codename that follows the knife-making process: from rough rusty iron to a sharp, precise blade.

---

## Current Version: 1.4 "Hoja"

> *The blade. Shaped on the anvil, now with an edge.*

The fifth release, focused on search pruning, positional evaluation, and speed optimization. Measured at **+430 Elo** over version 1.3 (Ordo ~2330, gauntlet 1040 games at 2min+1sec). NPS improvement: +53% (Linux), +92% (Windows) over 1.3.

### What's new in 1.4

- **Static Exchange Evaluation (SEE)** — simulates full capture sequences with x-ray discovery. Losing captures pruned in quiescence search.
- **Check extension** — extends search depth by 1 when in check. Ensures critical evasions are resolved at full depth.
- **Futility pruning** — at shallow depths, skip quiet moves when static evaluation plus a margin is below alpha. Also reverse futility pruning at the node level.
- **Piece mobility** — knights, bishops, rooks, and queens score bonuses per pseudo-legal square available. Mobility area excludes own pieces and enemy pawn attacks.
- **Positional bonuses** — open/semi-open file rook bonuses, rook on 7th rank, bishop pair, knight outposts.
- **Make/unmake legality** — the ~700 byte board copy in `is_legal()` replaced with make/check/unmake in the search hot path. NPS: +130%.
- **Depth-preferred TT replacement** — shallow entries no longer evict deeper entries for different positions.
- **Progressive easy-move** — cumulative x0.95 reduction per stable iteration, cancellable on PV change or score drop.
- **AW fail-high time extension** — proportional to the number of aspiration window fail-highs, up to x1.50.
- **Forced move instant-play** — only one legal move = play immediately with zero search time.
- **`bench` command** — 10 hand-crafted positions for NPS measurement and regression testing.

---

## Version History

| Version | Codename   | Ordo Elo | Gain |
|---------|------------|----------|------|
| 1.4     | Hoja       | ~2330    | +430 vs 1.3 |
| 1.3     | Yunque     | ~1900    | +200 vs 1.2 |
| 1.2     | Rojo Vivo  | ~1700    | +340 vs 1.1 |
| 1.1     | Herrumbre  | ~1360    | +140 vs 1.0 |
| 1.0     | Oxido      | ~1220    | baseline |

Gauntlet methodology: 26 opponents, 40 games each (1040 total), 2min+1sec, balanced opening book. Ordo rating computed across all versions in a combined rating list. Gauntlet field renewed at 1.4 (avg ~2310, range 2108-2504).

---

## Build

### Requirements
- C++17 compiler (GCC 10+ or Clang 12+)
- CMake 3.16+

### Linux
```bash
mkdir build-linux && cd build-linux
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

The resulting binary (`facon-1.4` / `facon-1.4.exe`) is statically linked and has no external dependencies.

---

## Usage

Facón communicates via the UCI protocol. Any UCI-compatible GUI works: [Arena](http://www.playwitharena.de/), [Cute Chess](https://cutechess.com/), [Banksia](https://banksiagui.com/).

### Quick start
```
$ ./facon-1.4
uci
id name Facon 1.4 - Hoja
id author Carlos M. Canavessi
option name Hash type spin default 16 min 1 max 1024
uciok
isready
readyok
position startpos
go movetime 2000
info depth 1 seldepth 1 score cp 68 nodes 41 nps 0 time 0 hashfull 0 pv e2e4
...
bestmove e2e4
```

### Supported UCI options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `Hash` | spin | 16 | Transposition table size in MB (1-1024) |

---

## Project Structure

```
facon/
├── src/
│   ├── types.h         — Core types: Square, Piece, Move, Bitboard, move_to_uci()
│   ├── bitboard.h/.cpp — Magic bitboards, attack tables
│   ├── board.h/.cpp    — Board state, make/unmake, Zobrist hashing, all_attackers_to()
│   ├── movegen.h/.cpp  — Pseudo-legal move generation (captures include quiet queen promotions)
│   ├── eval.h/.cpp     — Material + PST + king safety + mopup + pawn structure + positional
│   ├── tt.h/.cpp       — Transposition table (depth-preferred replacement)
│   ├── timeman.h/.cpp  — Time management
│   ├── search.h/.cpp   — Negamax, LMR, NMP, SEE, futility pruning, aspiration windows, ID
│   ├── uci.h/.cpp      — UCI protocol handler, bench command
│   ├── main.cpp        — Entry point
│   ├── version.h.in    — Version header template (CMake-generated)
│   └── version.rc.in   — Windows version resource template
├── cmake/
│   └── windows-cross.cmake
├── docs/
│   ├── v1.0.md         — Technical documentation for v1.0
│   ├── v1.1.md         — Technical documentation for v1.1
│   ├── v1.2.md         — Technical documentation for v1.2
│   ├── v1.3.md         — Technical documentation for v1.3
│   └── v1.4.md         — Technical documentation for v1.4
├── CMakeLists.txt
├── CHANGELOG.md
└── README.md
```

---

## Planned improvements

- **Search**: IID, singular extensions, countermove heuristic, razoring, LMP
- **Evaluation**: full king attack by rays, pawn shelter/storm, piece tropism, tempo, space, endgame recognition (material signature dispatch)
- **Transposition table**: aging (generation counter), smarter replacement
- **Speed**: incremental evaluation (PST in make/unmake), Texel tuning of all HCE weights
- **Architecture**: Syzygy tablebases, ponder, Lazy SMP multithreading, NNUE (long-term)

---

## Author

**Carlos M. Canavessi**

---

## Acknowledgements

- [Chess Programming Wiki](https://www.chessprogramming.org/) — reference for all chess engine techniques
- [CCRL](https://www.computerchess.org.uk/ccrl/) — computer chess rating list
