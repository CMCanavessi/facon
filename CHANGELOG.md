# Changelog
<!-- Last modified: 2026-05-26 08:07 -->

All notable changes to Facon will be documented here.

---

## [1.5] "Espiga" -- 2026-05-26

**Ordo ~2550, +~220 Elo over 1.4.** Gauntlet 3 (low-Ordo field): 800.5/1040 (77.0%, Ordo ~2550). Gauntlet 4 (high-Ordo field): 345.5/1040 (33.2%, Ordo ~2545). Combined G3+G4 Ordo: ~2550 (n=2080). CCRL 40/15 (n=20, preliminary): 2583.

### Critical Bug Fixes

- **`game_phase()` inversion** (`eval.cpp`): the function returned the opposite of its documented contract -- 0 in startpos (should have been 256 = middlegame) and 256 in pure endgame (should have been 0). Consumers (king_safety, pst_value for the king) were written assuming the documented contract, so they silently used the wrong tables since 1.1: king_safety returned 0 in the middlegame (zeroed by phase_mg=0), and pst_value used PST_KING_EG instead of PST_KING_MG from move 1, incentivizing centralizing the king from the opening. The fix inverts the counting: phase now starts at 0 and adds material present, instead of starting at TOTAL_PHASE and subtracting material absent. Consumers are unchanged. **Single largest source of strength gain in 1.5.**
- **Drawn pawnless endings -- bug acknowledged, fix attempted, fix reverted** (`eval.cpp`): when `evaluate()` detects a pawnless ending with material above `MOPUP_THRESHOLD` (300cp), it calls `mopup_eval` and adds the result to the score. `mopup_eval` correctly returns 0 for theoretically drawn endings (K+B vs K, K+N vs K, K+N+N vs K, K+B+B same-color vs K), but `evaluate()` was only ADDING that 0 to a score that already contained the strong side's material count (320cp for a lone knight, 660cp for two same-color bishops, etc.). The engine evaluates these drawn endings as decisive material advantages. An override that forced the final score to 0 in these cases was implemented and tested. Although the override was technically correct, extended gauntlet testing showed it cost ~30 Elo: the override propagated through search and demotivated the engine from reaching positions whose drawn final endings it could nevertheless often avoid in practice. The override was reverted. The bug is acknowledged and listed under Known Limitations; proper resolution requires material-signature endgame recognition or Syzygy tablebase probing, planned for a future version.

### Search

- **Late Move Pruning (LMP)** (`search.cpp`, `search.h`): at shallow depth in non-PV nodes, once enough legal quiet moves have been searched, additional quiet moves are pruned entirely. Per-depth thresholds in `LMP_TABLE`. Captures, promotions, and killers exempt. Combined with SEE-aware capture ordering for a single shipped iteration.
- **SEE-aware capture ordering** (`search.cpp`, `search.h`): captures are split into two ordering tiers. GOOD captures (SEE >= 0) score above killers and are tried before quiets. BAD captures (SEE < 0) score below history and are tried last. Replaces the single `ORDER_CAPTURE` constant. Shortcut: captures where attacker is no more valuable than victim (PxQ, NxR, etc.) skip the SEE call and are classified GOOD directly. Bench NPS decreased ~22% due to SEE computation on every uncertain capture, but Elo gain dominated.
- **Internal Iterative Reductions (IIR)** (`search.cpp`, `search.h`): when a non-PV node has no TT move at sufficient depth, the depth is reduced by 1 before searching. The next visit (after the reduced search stores a move in the TT) benefits from move ordering.
- **Countermove heuristic** (`search.cpp`, `search.h`): for each (piece, to-square) pair of the opponent's last move, the engine remembers the quiet move that caused a beta cutoff in the child node. Ordered after killers, before history. Requires passing `prev_move` through the negamax call chain.
- **Razoring** (`search.cpp`, `search.h`): at depth 1-2 in non-PV nodes, if `eval + RAZOR_MARGIN <= alpha`, drop into qsearch immediately (`RAZOR_MAX_DEPTH = 2`, `RAZOR_MARGIN = 250cp`). A position so far below alpha that a margin of pawn-and-rook still cannot reach it is unlikely to improve in a shallow search.

### Evaluation

- **Knight outpost refactor** (`eval.cpp`, `eval.h`): the single `KNIGHT_OUTPOST = 20cp` constant is replaced with two graduated bonuses. `KNIGHT_OUTPOST_REACHABLE = 10cp` for knights on squares safe from pawn attack but not pawn-supported. `KNIGHT_OUTPOST_SUPPORTED = 25cp` for knights both safe AND supported by a friendly pawn (firmly anchored).
- **Knight outpost forward_mask fix** (`eval.cpp`): the forward_mask used to disqualify outposts incorrectly included the knight's own rank, causing false-negatives when an enemy pawn sat on the same rank in an adjacent file. Pawns capture diagonally forward, so same-rank enemy pawns cannot attack the knight. Fix: forward_mask is now strictly ahead of the knight. Guarded against undefined behavior (1ULL << 64) for the edge case of a white knight on rank 8.

### Transposition Table

- **TT aging** (`tt.cpp`, `tt.h`, `search.h`): each search bumps a global generation counter (`current_generation`, wraps at 64). Replacement policy uses `(age * 4) + depth_loss`: an entry one generation old is treated as 4 plies shallower for replacement purposes. Allows fresh shallow results to replace deep stale ones from earlier positions. TT entry size unchanged at 16 bytes; generation is packed into the bound byte as `gen_bound = (generation << 2) | bound`.

### Time Management

- **`go infinite`/analysis idempotency** (`timeman.cpp`, `uci.cpp`): `start()` with no time limits sets `infinite = true`, avoiding spurious time-management activation in analysis mode.

### Infrastructure

- **`bench` default depth raised from 15 to 18** (`uci.cpp`): more meaningful per-position measurements. Bench signature: 299,881,540 nodes at depth 18 (deterministic, used as no-regression check).
- **`bench` position rebalancing** (`uci.cpp`): 6 of 10 positions replaced for better time distribution. At depth 18, per-position share went from 0.01%-38.85% (1.4 set) to 2.7%-16.7% (much more uniform). Each position now carries a label describing the search feature it stresses.
- **`bench verbose` flag** (`uci.cpp`): when present, full search output (info lines, currmove, TM messages) is emitted. Default (quiet) mode prints only per-position summary and total.
- **`bench` `TM.infinite = true` during benchmark** (`uci.cpp`): bench now forces infinite TM during the benchmark, so node counts remain reproducible across TM tuning experiments.
- **`bench` short-circuit on mate-in-1** (`search.cpp`): the bench positions occasionally trigger forced-mate sequences at depth 1 -- previously bench would log a "depth 0" line and continue at full TC. Now exits the iteration loop cleanly when a forced mate is found at depth 1.
- **`eval` command** (`uci.cpp`, `eval.cpp`, `eval.h`): new UCI command that prints a per-component breakdown of `evaluate()` for the current position. Useful for tuning and debugging. Calls `evaluate_verbose()` which reproduces `evaluate()` exactly but accumulates each term separately.
- **`evaluate_verbose()` label fixes** (`eval.cpp`): the "game_phase" line in `eval` output said "0 = endgame, 256 = middlegame", which was misleading because phase reflects non-pawn material remaining and does not distinguish opening from middlegame. Corrected to "0 = bare kings, 256 = full non-pawn material". The "skipped" reason strings for the mopup component were also clarified.
- **Mopup insufficient material guard extended** (`eval.cpp`): the guard for theoretically drawn pawnless endings now covers K+N+N vs K and K+B+B same-color vs K, in addition to the K+B vs K and K+N vs K cases already handled in 1.4. Without these the strong side's material exceeded `MOPUP_THRESHOLD` and corner-chasing activated in drawn positions, causing the engine to refuse draws and wander. K+B+B with opposite-colored bishops continues to trigger mopup correctly (winning endgame).
- **`cmd_setoption()` Hash parse safety** (`uci.cpp`): replaced `std::stoi()` with the same manual digit-parse pattern used by `cmd_perft()`. Release builds are compiled with `-fno-exceptions`, so `std::stoi` on a non-numeric value would call `std::terminate()` and crash the engine. Defensive against malformed setoption commands like `setoption name Hash value abc`.
- **Final summary on aborted iteration** (`search.cpp`): when the search is interrupted mid-iteration (hard time limit hit, `stop` command), several minutes of search activity could elapse silently between the last heartbeat and the `bestmove` output. The search now emits a final `info depth` line (raw UCI format, parseable) and an `info string aborted while searching at depth N -- M nodes, h:mm:ss,ms, M nps -- using score from depth N-1: ±Xcp` line (human readable) right before `bestmove`, capturing total work done. Only emitted when the iteration in progress was actually interrupted -- no change when the search ends cleanly at iteration boundary.
- **Command-line `bench` mode** (`main.cpp`, `uci.cpp`, `uci.h`): the binary can now be invoked as `./facon-1.5 bench [verbose] [depth N]` to run the benchmark once and exit, without going through the UCI handshake. Useful for automated NPS regression tests, CI runs, and benchmark sweeps over compilation flags. Banner and TT info are still printed when stdin is a terminal. Unknown command-line arguments produce a usage message and exit code 1.
- **Score sign convention cleanup** (`search.cpp`): the `+` prefix on the AW fail-low/high info strings and the "new best" info string is now applied only when the score is strictly positive (previously used `>= 0`, which produced `+0cp` for exact-zero scores). Zero scores now print as `0cp` without sign, consistent with the new summary line and most other engines.
- **`currmove` output gated by elapsed time** (`search.cpp`): UCI `info depth N currmove M currmovenumber K` lines from the root are now suppressed during the first 2 seconds of each search. At shallow depths (5-12) each root move completes in well under a millisecond, producing hundreds of currmove lines per second. This volume of stdout traffic can saturate the GUI's pipe-reader event loop in some setups, with knock-on effects on observed search throughput. After 2 seconds, `currmove` output resumes normally -- by that point the per-move time is typically long enough that each currmove line is useful rather than spam. Heartbeats, "new best" lines, and final iteration summaries are unaffected.

### Build

- **MSVC support removed** (`CMakeLists.txt`): the previous MSVC branch was non-functional because `bitboard.h` uses GCC/Clang builtins (`__builtin_popcountll`, `__builtin_ctzll`, `__builtin_clzll`) directly without wrappers. Keeping the dead code gave a false sense of portability. The branch has been removed and MSVC is now rejected at configure time with `FATAL_ERROR`. Native Windows builds should use MinGW-w64, either directly on Windows or via cross-compile from Linux.
- **`-fomit-frame-pointer` added to Release builds** (`CMakeLists.txt`): frees `%rbp` as a general-purpose register in the hot search/eval paths. Measured ~+2.3% NPS over the previous flag set on Ryzen 7 1700 (Zen 1). Sole flag change from a deliberate sweep over various GCC tuning options -- other candidates (`-march=x86-64-v3`, `-Ofast`, `-frename-registers`) were rejected due to either compatibility cost or no measurable benefit.


---

## [1.4] "Hoja" -- 2026-04-24

**Ordo ~2330, +430 Elo over 1.3.** Gauntlet 3: 530.5/1040 (51.0%) against 26 opponents (avg ~2310). Self-play vs 1.3: +545 Elo (95.85%, 1120 games). NPS improvement: +53% (Linux), +92% (Windows) over 1.3.

### Search

- **Check extension** (`search.cpp`): `if (in_check) depth++` before depth==0 qsearch entry. Forces check evasions to be resolved at full depth. +30 Elo.
- **Static Exchange Evaluation (SEE)** (`search.cpp`, `board.h`, `board.cpp`): full exchange simulation using `all_attackers_to(sq, occ)` with x-ray discovery. Captures with SEE < 0 pruned in qsearch. Shortcut: skip SEE call when victim >= attacker (always winning).
- **Reverse futility pruning (RFP)** (`search.cpp`, `search.h`): at depth 1-3, if `eval - 100*depth >= beta`, prune the node entirely. Like NMP without the null search.
- **Move-level futility pruning** (`search.cpp`, `search.h`): at depth 1-2, skip quiet moves if `eval + 150*depth <= alpha`. At least one move always searched.
- **LMR table precalculated** (`search.h`, `search.cpp`, `main.cpp`): `LMR_table[MAX_PLY][MAX_MOVES]` initialized at startup replaces per-move `log(depth)*log(move)/LMR_DIVISOR` in the hot path.
- **Move scores computed once** (`search.cpp`): parallel scores array computed O(n), selection sort uses precomputed scores. Eliminates O(n^2) `move_score()` calls. NPS: +9%.
- **Make/unmake legality** (`search.cpp`): `is_legal()` (which copies ~700 bytes of Board per pseudo-legal move) replaced with make, king-in-check, unmake in negamax and quiescence hot paths. NPS: +130%.
- **Quiet queen promotions in qsearch** (`movegen.cpp`, `movegen.h`): `generate_captures()` now includes queen-promotion pushes (pawn to 8th rank without capturing). A free queen was previously invisible to qsearch.
- **Forced move instant-play** (`search.cpp`): when only one legal move exists at root, play immediately with zero search time. Emits minimal `info depth 0` line for GUI compatibility.

### Evaluation

- **Piece mobility** (`eval.cpp`, `eval.h`): pseudo-legal squares per piece (excluding own pieces and enemy pawn attacks). Knight 4cp, bishop 5cp, rook 2cp, queen 1cp per square.
- **Open/semi-open files** (`eval.cpp`, `eval.h`): rook on open file (no pawns) +20cp, semi-open (no friendly pawns) +10cp.
- **Rook on 7th rank** (`eval.cpp`, `eval.h`): +20cp for rook on the opponent's 2nd rank.
- **Bishop pair** (`eval.cpp`, `eval.h`): +30cp when both bishops present.
- **Knight outposts** (`eval.cpp`, `eval.h`): +20cp for knights on ranks 4-6 (relative) that cannot be attacked by enemy pawns.


## [1.4] "Hoja" -- 2026-04-24

**Ordo ~2330, +430 Elo over 1.3.** Gauntlet 3: 530.5/1040 (51.0%) against 26 opponents (avg ~2310). Self-play vs 1.3: +545 Elo (95.85%, 1120 games). NPS improvement: +53% (Linux), +92% (Windows) over 1.3.

### Search

- **Check extension** (`search.cpp`): `if (in_check) depth++` before depth==0 qsearch entry. Forces check evasions to be resolved at full depth. +30 Elo.
- **Static Exchange Evaluation (SEE)** (`search.cpp`, `board.h`, `board.cpp`): full exchange simulation using `all_attackers_to(sq, occ)` with x-ray discovery. Captures with SEE < 0 pruned in qsearch. Shortcut: skip SEE call when victim >= attacker (always winning).
- **Reverse futility pruning (RFP)** (`search.cpp`, `search.h`): at depth 1-3, if `eval - 100*depth >= beta`, prune the node entirely. Like NMP without the null search.
- **Move-level futility pruning** (`search.cpp`, `search.h`): at depth 1-2, skip quiet moves if `eval + 150*depth <= alpha`. At least one move always searched.
- **LMR table precalculated** (`search.h`, `search.cpp`, `main.cpp`): `LMR_table[MAX_PLY][MAX_MOVES]` initialized at startup replaces per-move `log(depth)*log(move)/LMR_DIVISOR` in the hot path.
- **Move scores computed once** (`search.cpp`): parallel scores array computed O(n), selection sort uses precomputed scores. Eliminates O(n^2) `move_score()` calls. NPS: +9%.
- **Make/unmake legality** (`search.cpp`): `is_legal()` (which copies ~700 bytes of Board per pseudo-legal move) replaced with make, king-in-check, unmake in negamax and quiescence hot paths. NPS: +130%.
- **Quiet queen promotions in qsearch** (`movegen.cpp`, `movegen.h`): `generate_captures()` now includes queen-promotion pushes (pawn to 8th rank without capturing). A free queen was previously invisible to qsearch.
- **Forced move instant-play** (`search.cpp`): when only one legal move exists at root, play immediately with zero search time. Emits minimal `info depth 0` line for GUI compatibility.

### Evaluation

- **Piece mobility** (`eval.cpp`, `eval.h`): pseudo-legal squares per piece (excluding own pieces and enemy pawn attacks). Knight 4cp, bishop 5cp, rook 2cp, queen 1cp per square.
- **Open/semi-open files** (`eval.cpp`, `eval.h`): rook on open file (no pawns) +20cp, semi-open (no friendly pawns) +10cp.
- **Rook on 7th rank** (`eval.cpp`, `eval.h`): +20cp for rook on the opponent's 2nd rank.
- **Bishop pair** (`eval.cpp`, `eval.h`): +30cp when both bishops present.
- **Knight outposts** (`eval.cpp`, `eval.h`): +20cp for knights on ranks 4-6 (relative) that cannot be attacked by enemy pawns.

### Transposition Table

- **Depth-preferred replacement** (`tt.cpp`, `tt.h`): `store()` refuses to overwrite a deeper entry for a different position. Overwrites only if slot is empty, same position, or new depth >= stored depth.
- **TTEntry::depth int8_t to uint8_t** (`tt.h`, `tt.cpp`): `MAX_PLY` is 128, which overflows int8_t. Depth 128 would store as -128, corrupting probe comparisons.

### Time Management

- **MOVES_TO_GO 30 to 25** (`timeman.cpp`): ~20% more time per move.
- **Progressive easy-move** (`search.cpp`, `search.h`): x0.95 per stable iteration (>=5 at depth >=10), cumulative. Cancelled on PV change or score drop >=30cp. Replaces one-shot x0.40.
- **AW fail-high time extension** (`search.cpp`): `1.0 + 0.10 * count * depth_scale`, capped at x1.50. More fail-highs = more time to resolve the window.
- **EXTENSION_FULL_DEPTH 18 to 15** (`search.h`): full extension factor reached earlier, balanced by progressive easy-move.
- **Time cap relaxed** (`timeman.cpp`): `remaining/3` to `remaining*2/5`. More permissive, especially in endgames.
- **Mate reduction guard** (`search.cpp`): `reduce_time("mate found")` only fires when `last_score > 0`. Being mated no longer triggers time reduction.
- **soft_stop reorder** (`search.cpp`): TM extensions now always run before checking soft_stop().
- **go depth X fix** (`timeman.cpp`): depth-limited search without clock now sets `infinite=true`. Previously activated TM fallback.
- **movestogo UCI parameter** (`timeman.h`, `timeman.cpp`, `uci.cpp`): parsed from `go ... movestogo N`, used instead of hardcoded constant.

### Infrastructure

- **bench command** (`uci.cpp`, `uci.h`, `search.h`): 10 hand-crafted positions, depth 15 default. Reports per-position nodes + total NPS. Quiet by default; `bench verbose` shows full search output.
- **all_attackers_to()** (`board.h`, `board.cpp`): returns all attackers of both colors using parameterized occupancy. Used by SEE for x-ray discovery.
- **move_to_uci() deduplication** (`types.h`, `search.cpp`, `uci.cpp`): identical static functions moved to types.h as shared inline.
- **FACON_DEBUG build mode** (`CMakeLists.txt`, `search.h`, `search.cpp`): `cmake -DFACON_DEBUG=ON` enables LMR/NMP/TT diagnostic counters. Zero cost in release builds.
- **Dev codename detection** (`CMakeLists.txt`): handles dev, dev-rc1, dev-rc2, etc.


---

## [1.3] "Yunque" — 2026-04-11

**Ordo ~1900, +200 Elo over 1.2.** Gauntlet 2: 572.0/1040 (55.0%) against 26 opponents (avg ~1870). Self-play vs 1.2: +240 Elo (1000 games).

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

## [1.2] "Rojo Vivo" — 2026-03-23

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
