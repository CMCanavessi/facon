# Changelog
<!-- Last modified: 2026-04-11 11:53 -->

All notable changes to Facón will be documented here.

---

## [1.3] "Yunque" — 2026-04-11

### Pre-work bug fixes

- **NMP depth floor** (`search.cpp`): the NMP recursive call was passing `depth - 1 - NMP_REDUCTION` without a floor. At `depth == NMP_MIN_DEPTH` (3) this produces -1, which with the corrected `depth == 0` condition no longer enters quiescence — causing infinite recursion and a search hang at depth 4+. Fixed: `std::max(0, depth - 1 - NMP_REDUCTION)`.
- **`depth <= 0` → `depth == 0`** (`search.cpp`): prerequisite for LMR. With reductions, a node can be called with a reduced depth of exactly 0; `depth == 0` enters quiescence precisely at that point. Negative depths are no longer silently absorbed — they cause a detectable hang. All reduction call sites use `std::max(0, reduced_depth)`.

### Infrastructure

- **Centralized version system** (`CMakeLists.txt`, `src/version.h.in`, `src/version.rc.in`): `PROJECT_VERSION` and `FACON_CODENAME` in `CMakeLists.txt` are the single source of truth. CMake generates `version.h` at build time via `configure_file`. Binary name, startup banner, UCI `id name`, and Windows version resource all derive from these two variables. To release: set `FACON_CODENAME "Yunque"` and recompile. `src/version.rc` (static file from 1.2) is deleted — replaced by `src/version.rc.in`.
- **`bitboard.cpp` init message gated** (`bitboard.cpp`): "Initializing magic bitboards... done." now only prints when stderr is a terminal (`isatty()`). Suppressed when launched by GUI or fastchess.
- **`perft` command** (`uci.cpp`, `uci.h`): `perft N` counts leaf nodes to depth N from the current position and reports total nodes and elapsed time. `perft divide N` breaks the count down per first legal move. Bulk-counting optimization at depth 1. Verified: startpos depth 5 = 4,865,609.
- **Build directory naming** (`CMakeLists.txt`): quick start updated from `build` to `build-linux` / `build-windows`.

### Search

- **LMR (Late Move Reductions)** (`search.cpp`, `search.h`): moves searched after the first `LMR_MIN_MOVES` (3) legal moves at depth ≥ `LMR_MIN_DEPTH` (3) are searched at reduced depth if quiet and not in check. Formula: `reduction = log(depth) × log(move_number) / LMR_DIVISOR` (2.25). Re-searched at full depth if the reduced search raises alpha.
- **History heuristic** (`search.cpp`, `search.h`): `history_[color][from][to]` incremented by `depth²` on beta cutoffs. Replaces flat `ORDER_QUIET=0` score for quiet move ordering. Capped at `HISTORY_MAX` (50,000). Reset each search.
- **Aspiration windows** (`search.cpp`, `search.h`): iterative deepening searches with a ±`ASP_WINDOW` (50cp) window around the previous score from depth 4+. On fail-low or fail-high, widens only in the failing direction and doubles delta. Full window used for depth < 4 and mate scores.
- **Aspiration window fail-low fix** (`search.cpp`): the original fail-low handler set `beta_asp = (alpha_asp + beta_asp) / 2`, squeezing the upper bound. On re-search, TT and LMR interactions can cause the score to rebound above the narrowed ceiling, triggering a spurious fail-high and forcing a third search (yo-yo effect). Fixed: on fail-low, widen alpha only; leave beta unchanged. Standard implementation.
- **Aspiration window verbosity** (`search.cpp`): fail-low and fail-high events emit `info string ASP fail-{low|high} depth D score S window [α, β] delta D` lines, consistent with TM extension/reduction verbosity.

### Evaluation

- **Pawn structure** (`eval.cpp`, `eval.h`): five terms via bitboard operations — isolated (−15cp), doubled (−15cp), backward (−12cp), passed (rank-scaled: 0/0/10/20/35/55/80cp), connected (+8cp). All computed symmetrically for both colors.
- **Mopup insufficient material guard** (`eval.cpp`): K+B vs K and K+N vs K are theoretical draws. Both positions exceed `MOPUP_THRESHOLD` (300cp) and previously activated mopup. `mopup_eval()` now returns 0 when the strong side has exactly one minor piece and no pawns.

### Time Management

- **Quadratic extension scaling** (`search.cpp`, `search.h`): `extend_time()` calls in `go()` pre-scale the factor by `(depth² / EXTENSION_FULL_DEPTH²)`. PV changes at depth 2–9 have near-zero effect; extensions at the engine's operating depth (14+) apply the full factor. `EXTENSION_FULL_DEPTH = 18`.
- **`accumulated_ext_` cap removed** (`timeman.cpp`, `timeman.h`): the old 2.0× cap caused near-zero quadratic factors at low depths to consume the extension budget before real extensions at depth 14+ could fire. The soft limit is now bounded only by the hard limit.
- **`extend_time()` guard** (`timeman.cpp`): `if (factor <= 1.0) return` — a factor ≤ 1.0 would shrink the soft limit silently. Use `reduce_time()` instead.
- **`reduce_time()`** (`timeman.h`, `timeman.cpp`): mirrors `extend_time()`. Multiplies the soft limit by a factor < 1.0, floored at `MIN_TIME_MS`. Triggers: mate found (×0.05, one-shot), forced move (×0.1), PV+score stable ≥ 7 iterations at depth > 12 (×0.4, one-shot).
- **`mate_reduction_applied_`** (`search.h`, `search.cpp`): one-shot guard for the "mate found" reduction. Without it, `is_mate_score()` is true on every subsequent iteration, collapsing the soft limit exponentially (×0.05^N).
- **`stable_iters_`** (`search.h`, `search.cpp`): counter for consecutive stable iterations (same PV move, score change < 3cp at depth > 12). Threshold raised from 3 to 7; easy-move factor changed from ×0.6 to ×0.4.
- **`cancel_easy_move()`** (`timeman.h`, `timeman.cpp`): reverses a prior easy-move reduction before applying a PV change or score drop extension. Multiplies soft limit by 1/factor. Does not touch `accumulated_ext_`.
- **Emergency hard limit** (`timeman.h`, `timeman.cpp`, `search.cpp`, `search.h`): when a depth ≥ 25 iteration completes, `TM.raise_emergency_limit()` raises the hard limit from 33.3% to 50% of raw remaining clock. Fires once per move. Allows genuinely deep, complex positions to use more time than the normal cap permits.

### Infrastructure / Bugfixes

- **Race condition in `cmd_ucinewgame()`** (`uci.cpp`): `TT.clear()` (which uses `std::memset`) could race with `TT.probe()` / `TT.store()` in the search thread if "ucinewgame" arrived while searching. Fixed by joining the search thread before clearing the TT and resetting the board.
- **`move_to_san()` castling check/mate** (`board.cpp`): the castling branch returned immediately (`"O-O"` / `"O-O-O"`) before reaching the check/checkmate detection block. Castling moves that deliver check or mate were missing the `+` or `#` suffix. Fixed by setting `san` instead of returning, so the check/mate annotation runs for all move types.
- **`seen[]` guard mismatch** (`search.cpp`): the PV repetition detection array was enlarged to `MAX_GAME_HISTORY + MAX_PLY + 2` (1154 slots) in 1.2, but the insertion guard still used `MAX_GAME_HISTORY + MAX_PLY` (1152), stopping 2 entries short. Updated the guard to match the array size.
- **Mopup insufficient material guard — implementation** (`eval.cpp`): the guard documented in 1.3 (K+B vs K and K+N vs K return 0 from `mopup_eval()`) was described in `eval.h` but never implemented in `eval.cpp`. Added the check: if the strong side has exactly two pieces (king + one minor), `mopup_eval()` returns 0.

---

## [1.2] "Rojo Vivo" — 2026-03-14

**+33.5% over 1.1** (870.0/1040 = 83.7% vs same 26-opponent field at 2min+1sec; Ordo **~1700**)

### Pre-work fixes

- **`unmake_move()` hash corruption** (`board.cpp`): `hash = st.hash` was placed *before* the piece operations (`move_piece`, `put_piece`, `remove_piece`), each of which XOR the hash incrementally. The restored hash was immediately re-corrupted by those ops, causing the board hash to differ from the saved pre-move value after every unmake. Fixed by moving `hash = st.hash` to the last line of `unmake_move()`, after all piece operations. Latent bug since 1.0 — affected TT hit rates, repetition detection, and PV display. The alternating root hash visible between iterative deepening iterations was a direct symptom.
- **`make_null_move()` full_move_number corruption** (`board.cpp`): `make_null_move()` never incremented `full_move_number`, but `unmake_null_move()` decremented it when `side_to_move == BLACK` after the flip. Every null move by Black corrupted the move counter. Fixed: added `if (side_to_move == WHITE) full_move_number++;` in `make_null_move()` after the side flip.
- **Double `generate_all_moves()` in TT probe path** (`search.cpp`): when a TT hit produced a valid `tt_move`, the code generated moves once to validate the TT move, then generated again for the main move loop. Eliminated by moving a single generation before the TT probe and reusing it. ~2-5% nps improvement.
- **Static linking Windows Defender false positive** (`CMakeLists.txt`): the global `-static` flag produced fully-static PE binaries that triggered Windows Defender's cryptominer heuristic. Replaced with selective static linking (`-static-libgcc`, `-static-libstdc++`, `-Wl,-Bstatic,-lwinpthread,-Bdynamic`).
- **Windows version resource** (`src/version.rc`, `CMakeLists.txt`): new file with `FileDescription "Facon Chess Engine"` and `CompanyName "Carlos M. Canavessi"`. Shows metadata in Explorer → Properties → Details.

### Search

- **Null Move Pruning (NMP)** (`search.cpp`, `search.h`): if the side to move can pass their turn and still cause a beta cutoff at reduced depth, the position is too good for the opponent and we prune. Guards: `do_null`, not in check, `ply > 0`, `depth >= NMP_MIN_DEPTH` (3), non-pawn material present (avoids zugzwang in pawn endings). Reduction: `NMP_REDUCTION = 3`. Zero-window search: `-negamax(board, -beta, -beta+1, depth-1-R, ply+1, false)`.
- **Triangular PV array** (`search.cpp`, `search.h`): replaced TT-based PV retrieval with an explicit `pv_table_[MAX_PLY][MAX_PLY]` / `pv_length_[MAX_PLY]`. PV is updated on every alpha raise by copying the child's line. PV is reset (`memset`) at the start of each iteration. Eliminates stale PV moves from previous iterations.
- **PV display — repetition detection** (`search.cpp`): the PV walk in `go()` seeds a `seen[]` array with all hashes from `board.history[0..history_ply-1]` (game history before the root) plus the root hash itself. After each `make_move()` in the walk, if the resulting hash appears in `seen[]` the move is not printed and the walk stops. Eliminates the `"PV continues after threefold repetition"` warning from GUIs.

### Evaluation

- **Mopup evaluation** (`eval.cpp`, `eval.h`): bonus for the winning side in pawnless endings where `|score| >= MOPUP_THRESHOLD` (300cp). Rewards pushing the losing king to the corner (`MOPUP_CORNER_WEIGHT=10` × Manhattan distance from center) and keeping kings close (`MOPUP_PROXIMITY_WEIGHT=5` × (14 − Manhattan distance)). Applied from the winning side's perspective.

### Infrastructure

- **UCI threading** (`uci.cpp`, `uci.h`): `cmd_go()` now launches the search in a dedicated `std::thread`. The UCI loop returns immediately and can process `stop` while search runs. `cmd_stop()` sets `TM.stop` and joins the thread. Previously `stop` was ignored while searching.
- **`isatty()`-gated output** (`uci.cpp`, `main.cpp`): the startup banner, `TT.print_info()`, and the interactive `> ` prompt are suppressed when stdin is not a terminal (i.e. when launched by a GUI or fastchess). Output is clean for automated use.
- **TT silent constructor** (`tt.cpp`, `tt.h`): `TT` is a global object initialized before `main()`. The constructor now calls `resize(mb, silent=true)` so no output is emitted during static initialization. `print_info()` is called explicitly from `main()` after the banner, gated behind `isatty()`. Eliminates the TT message appearing before the banner.
- **`probe_move()` removal** (`tt.h`, `tt.cpp`): dead code removed. The root no longer relies on TT probes for bestmove — `root_best_move_` is set directly in `negamax()`.

### Observability

- **currmove/currmovenumber** (`search.cpp`): at ply 0 with `depth > 1`, each root move is announced as it begins searching via `info currmove <move> currmovenumber <n>`. Provides real-time visibility into which root move is being evaluated.
- **New-best SAN info string** (`search.cpp`, `board.h`, `board.cpp`): whenever the best move at the root *changes relative to the previous completed iteration*, an `info string` is emitted: `info string new best: Nf3+ +0.31 -- move 3/20 depth 11 [0:01:23,450]`. Guarded by `prev_best_move_` member — without this, alpha resetting at the start of each depth would fire on every iteration even for the same move. Time formatted as `h:mm:ss,ms`.
- **Heartbeat** (`search.cpp`): if `HEARTBEAT_INTERVAL_MS` (5 minutes = 300 000 ms) have passed since the last output, two lines are emitted: a standard `info depth ... nodes ... nps ... time ... hashfull` line (so Arena updates its stats panel) followed by `info string still searching -- last completed depth N`. Previously combined into one line using a mixed `info ... string` format that Arena did not display.

### Time Management

- **`extend_time()` reason parameter** (`timeman.h`, `timeman.cpp`): `extend_time(double factor, const char* reason = "")` now accepts an optional reason string. When non-empty, emits `info string TM extend xF (reason) -- soft Xms -> Yms [h:mm:ss,ms]`. Call sites: `TM.extend_time(1.5, "PV change")` and `TM.extend_time(1.25, "score drop")`.
- **`start()` allocation report** (`timeman.cpp`): on `start()`, emits soft/hard limits and the effective clock (after overhead deduction) for the current move.
- **Time forfeit fix** (`timeman.cpp`): engine was losing a large fraction of gauntlet games on time due to three compounding issues. Fix: `OVERHEAD_MS = 100` subtracted from remaining clock upfront before all limit calculations; `HARD_FACTOR` lowered 3.0 → 2.0; `SAFETY_FACTOR` lowered 0.95 → 0.90; hard_limit capped at `remaining / 3` (was `/ 2`); `should_stop()` triggers `STOP_GRACE_MS = 100` before the hard limit to guarantee bestmove delivery before GUI clock expires.

### Post-release bugfixes

- **Quiescence time check** (`search.cpp`): `quiescence()` checked `stats_.nodes & 2047` to decide when to call `TM.should_stop()`, but `stats_.nodes` is only incremented in `negamax()`. During deep tactical sequences with many recursive qsearch calls without returning to negamax, the condition was frozen and the time check never fired. Fixed by switching to `stats_.qnodes & 2047`.
- **Nodes / NPS reporting** (`search.cpp`, `search.h`): all UCI output (end-of-iteration info line, heartbeat, new-best) reported `stats_.nodes`, which only counts negamax nodes. `stats_.qnodes` (quiescence nodes) was never included. In tactical positions qsearch can generate 5–10× more nodes than the main search, causing reported node count and NPS to be severely underestimated. Fixed by reporting `stats_.nodes + stats_.qnodes` everywhere.
- **PV seen[] array size** (`search.cpp`): the repetition-detection array in the PV walk was declared `seen[MAX_GAME_HISTORY + MAX_PLY]` (1152 slots). In the worst case the seed pass fills up to 1025 slots and the walk adds up to 128 more, requiring 1153. The existing overflow guard prevented an actual out-of-bounds write but silently skipped registering the last position, creating a theoretical blind spot. Fixed by declaring `seen[MAX_GAME_HISTORY + MAX_PLY + 2]`.

---

## [1.1] "Herrumbre" — 2026-03-11

**+140 Elo over 1.0** (Ordo 1360, gauntlet 1040 games at 2min+1sec vs field avg ~1358)

### Search
- **Killer move heuristic**: quiet moves that cause a beta cutoff are stored per-ply (two slots per node) and tried before other quiet moves in sibling nodes. Improves move ordering in quiet positions where MVV-LVA has no effect.
- **Dynamic time management**: replaced the fixed time budget with a soft/hard limit model. The soft limit is extended when the PV move changes between iterations (×1.5) or the score drops by ≥30 centipawns (×1.25). The hard limit acts as an absolute ceiling.
- **seldepth tracking**: maximum depth reached including quiescence search is tracked in `stats_.seldepth` and reported in the UCI info line each iteration.

### Evaluation
- **King safety**: penalty for enemy pieces attacking the king zone (king square plus adjacent squares). Attack weights: Knight=2, Bishop=2, Rook=3, Queen=5 (queens counted twice). Penalty grows quadratically with total weight and scales with game phase (middlegame only).

### Stability
- **Abort flag** (`abort_search_`): when the hard time limit is hit inside the search, a flag is set and every level of the call stack returns immediately. Ensures all `unmake_move()` calls execute, leaving the board consistent.
- **Root best move tracking** (`root_best_move_`): best move at ply 0 saved directly inside `negamax()`. Eliminates `bestmove 0000` outputs.
- **`is_legal()` piece ownership check**: verifies the from-square contains a piece belonging to the side to move before the expensive board copy. Catches ghost moves from stale TT entries.
- **TT move validation**: TT moves verified against generated move list before use for ordering. Hash collisions cannot produce garbage in the sort.
- **TT pollution prevention**: if `abort_search_` is set when returning from a child node, result discarded without TT store.
- **`safe_move` pre-seed**: first legal move stored before the ID loop as last-resort fallback. Guarantees `bestmove` is never `0000`.

### Cleanup
- `probe_move()` removed from `tt.h`/`tt.cpp` — dead code.
- Non-ASCII characters removed from all source files.

---

## [1.0] "Óxido" — 2026-03-05

Initial public release.

### Engine
- Bitboard board representation with redundant piece array for O(1) square queries
- Magic bitboards for sliding piece attack generation
- Zobrist hashing with incremental updates on make/unmake
- Full make/unmake move stack supporting any game length
- Draw detection: threefold repetition and 50-move rule

### Move Generation
- Complete pseudo-legal move generation for all piece types
- Separate capture-only generator for quiescence search
- Handles all special moves: castling, en passant, promotions

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
- CMake build system, C++17
- Linux native build (GCC/Clang)
- Windows cross-compilation via MinGW-w64
- Statically linked Windows binary
