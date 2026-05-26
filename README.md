# Facón Chess Engine
<!-- Last modified: 2026-05-26 08:07 -->

<p align="center">
  <img src="assets/logos/facon-banner.png" alt="Facon Chess Engine" width="640"/>
</p>

A UCI-compliant chess engine written in C++17.

*by Carlos M. Canavessi*

![Version](https://img.shields.io/badge/version-1.5%20Espiga-8B0000)
![Language](https://img.shields.io/badge/language-C%2B%2B17-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)
![Protocol](https://img.shields.io/badge/protocol-UCI-green)
![Elo](https://img.shields.io/badge/Ordo%20Elo-~2550-yellow)

---

## About

Facón is a chess engine built from scratch in C++17, designed as a learning project and long-term development platform. The name comes from the *facón* — a traditional Argentine gaucho knife, forged by hand, raw and functional.

Each version carries a codename that follows the knife-making process: from rough rusty iron to a sharp, precise blade.

---

## Current Version: 1.5 "Espiga"

> *The tang. Hidden, but it's what holds the blade in your hand.*

The sixth release, focused on search refinement, a critical evaluation bug fix carried over from older versions, and infrastructure improvements. Measured at approximately **+220 Elo** over version 1.4 (Ordo ~2550). The bulk of the gain comes from fixing `game_phase()` that had been silently zeroing king-safety evaluation in the middlegame since version 1.1.

### What's new in 1.5

- **`game_phase()` inversion fix** — the function had been returning the inverse of its documented contract since 1.1, silently disabling king-safety in the middlegame and using the endgame king PST from move 1. Fix alone measured +224 Elo.
- **Late Move Pruning (LMP)** — skip late quiet moves at shallow depths in non-PV nodes when enough alternatives have already been searched without raising alpha.
- **SEE-aware capture ordering** — captures split into GOOD (SEE >= 0, before quiets) and BAD (SEE < 0, after quiets) tiers. Replaces the single MVV-LVA tier from 1.4.
- **Internal Iterative Reductions (IIR)** — non-PV nodes without a TT move at sufficient depth are reduced by 1 ply before searching; the next visit benefits from move ordering.
- **Countermove heuristic** — third quiet-move ordering tier after killers; remembers the refutation move per opponent (piece, to-square) pair.
- **Razoring** — at depth 1-2 in non-PV nodes, drop into qsearch if `eval + 250cp <= alpha`.
- **TT aging** — global generation counter packed into the TT entry's bound byte. Smarter replacement (depth + age) keeps the TT relevant across moves without losing meaningful old data.
- **Knight outpost refactor** — `KNIGHT_OUTPOST_REACHABLE` (10cp) vs `KNIGHT_OUTPOST_SUPPORTED` (25cp). Plus a fix for false-negative outpost detection on the knight's own rank.
- **Mopup insufficient material guard extended** — K+N+N vs K and K+B+B same-color vs K are now correctly detected as drawn endings; mopup is skipped.
- **`eval` UCI command** — non-UCI debug command that prints a per-component breakdown of the static evaluation for the current position.
- **`bench` rebalanced** — default depth raised from 15 to 18. Six positions replaced for more uniform time distribution; per-position labels added.
- **`cmd_setoption` Hash parse safety** — manual digit-parse instead of `std::stoi` (release builds use `-fno-exceptions`, which would crash on invalid input).
- **Build: MSVC support removed, `-fomit-frame-pointer` added** — the previous MSVC branch in the build system was non-functional (the source uses GCC/Clang builtins directly). Replaced with explicit `FATAL_ERROR` if the user attempts MSVC. Native Windows builds use MinGW-w64. `-fomit-frame-pointer` added to Release builds: ~+2.3% NPS at zero compatibility cost.
- **Final summary on aborted iteration** — when the search is interrupted mid-iteration (hard time limit, `stop` command), the engine now emits a final UCI `info depth` line and a human-readable `info string` summary right before `bestmove`, capturing total nodes / time / nps that would otherwise be lost between the last heartbeat and the bestmove.
- **Command-line `bench` mode** — `./facon-1.5 bench [verbose] [depth N]` runs the benchmark once and exits, without going through UCI handshake. Useful for automation and benchmark sweeps.
- **`currmove` output suppressed for first 2 seconds of search** — UCI `info currmove` lines from the root are now gated by elapsed time. At shallow depths each move at the root completes in well under a millisecond, producing hundreds of currmove lines per second; suppressing this volume reduces stdout pressure on GUI pipe readers without losing useful information.

---

## Version History

| Version | Codename   | Ordo Elo | Gain |
|---------|------------|----------|------|
| 1.5     | Espiga     | ~2550    | +220 vs 1.4 |
| 1.4     | Hoja       | ~2330    | +430 vs 1.3 |
| 1.3     | Yunque     | ~1900    | +200 vs 1.2 |
| 1.2     | Rojo Vivo  | ~1700    | +340 vs 1.1 |
| 1.1     | Herrumbre  | ~1360    | +140 vs 1.0 |
| 1.0     | Oxido      | ~1220    | baseline |

Gauntlet methodology: 26 opponents, 40 games each (1040 total), 2min+1sec, balanced opening book. Ordo rating computed across all versions in a combined rating list. Gauntlet field renewed at 1.4 (avg ~2310, range 2108-2504); 1.5 was tested against the same field (G3, Ordo ~2550) and against a higher-Ordo field (G4, avg ~2680, Ordo ~2545). Combined G3+G4 Ordo: ~2550.

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

The resulting binary (`facon-1.5` / `facon-1.5.exe`) is statically linked and has no external dependencies.

---

## Usage

Facón communicates via the UCI protocol. Any UCI-compatible GUI works: [Arena](http://www.playwitharena.de/), [Cute Chess](https://cutechess.com/), [Banksia](https://banksiagui.com/).

### Quick start
```
$ ./facon-1.5
uci
id name Facon 1.5 - Espiga
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

### Non-UCI commands

| Command | Description |
|---------|-------------|
| `eval`  | Print a per-component breakdown of the static evaluation for the current position. |
| `bench` | Run the benchmark on 10 hand-crafted positions; reports per-position nodes and total NPS. `bench verbose` for full search output, `bench depth N` to override the default depth (18). Also available as a command-line invocation (`./facon-1.5 bench`) which runs the benchmark and exits without entering the UCI loop. |
| `perft N` | Count leaf nodes to depth N from the current position. `perft divide N` for per-first-move breakdown. |
| `d` | Print the current board state. |

---

## Project Structure

```
facon/
├── src/
│   ├── types.h         — Core types: Square, Piece, Move, Bitboard, move_to_uci()
│   ├── bitboard.h/.cpp — Magic bitboards, attack tables
│   ├── board.h/.cpp    — Board state, make/unmake, Zobrist hashing, all_attackers_to()
│   ├── movegen.h/.cpp  — Pseudo-legal move generation (captures include quiet queen promotions)
│   ├── eval.h/.cpp     — Material + PST + king safety + mopup + pawn structure + positional + evaluate_verbose
│   ├── tt.h/.cpp       — Transposition table (depth-preferred replacement, generation/aging)
│   ├── timeman.h/.cpp  — Time management
│   ├── search.h/.cpp   — Negamax, LMR, NMP, SEE, futility, razoring, LMP, IIR, countermove, ID
│   ├── uci.h/.cpp      — UCI protocol handler, bench, eval, perft commands
│   ├── main.cpp        — Entry point
│   ├── version.h.in    — Version header template (CMake-generated)
│   └── version.rc.in   — Windows version resource template
├── cmake/
│   └── windows-cross.cmake
├── assets/
│   └── logos/
│       ├── facon-banner.png  — Repository banner (2:1)
│       └── Logo.svg          — Source vector logo
├── docs/
│   ├── v1.0.md         — Technical documentation for v1.0
│   ├── v1.1.md         — Technical documentation for v1.1
│   ├── v1.2.md         — Technical documentation for v1.2
│   ├── v1.3.md         — Technical documentation for v1.3
│   ├── v1.4.md         — Technical documentation for v1.4
│   └── v1.5.md         — Technical documentation for v1.5
├── CMakeLists.txt
├── CHANGELOG.md
└── README.md
```

---

## Author

**Carlos M. Canavessi**

---

## Acknowledgements

- [Chess Programming Wiki](https://www.chessprogramming.org/) — reference for all chess engine techniques
- [CCRL](https://www.computerchess.org.uk/ccrl/) — computer chess rating list
