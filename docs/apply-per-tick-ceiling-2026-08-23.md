# Apply is not cost-bound. It is cap-bound, and the cap already counts itself.

2026-08-23 late. Follows `docs/50k-budget-2026-08-23.md`'s correction ("the game
thread is only 25% busy … find and raise the per-tick bound first").

**No new leg and no new code were needed to find this.** It was already in
`Saved/ahead-on.log`, on the `Voxel apply stages` line, which has printed the
drain loop's exit attribution since Wave S0.

---

## 1. The reading

Every window of `Saved/ahead-on.log`, joined against `Voxel tick budget` and
`Voxel job flow` for the same window (all three read together — the standing
rule):

    win ticks tickMs  %wall  recomp  disp   apply | qEmpty wall cntCap drnCap | drained | appl/tick us/chunk
      1    59 2481.6  49.6%  1426.0 667.8   162.5 |      1    9     49      0 |    9764 |   165.5    16.6
      2   103 1304.0  26.1%   306.0 126.5   372.9 |      0   36     67      0 |   13873 |   134.7    26.9
      3   107 1233.3  24.7%   303.1 106.1   343.9 |      0   30     77      0 |   15816 |   147.8    21.7
      4    94 1217.7  24.4%   306.5 251.5   265.0 |      0   23     71      0 |   14054 |   149.5    18.9
      5    95 1043.7  20.9%   299.6  87.0   281.0 |      0   25     70      0 |   14174 |   149.2    19.8
      6   106 1265.3  25.3%   405.3 114.6   306.8 |      0   29     77      0 |   15473 |   146.0    19.8
      7   107 1367.4  27.4%   499.2 107.8   327.4 |      0   29     78      0 |   16028 |   149.8    20.4
      8   105 1448.7  29.0%   554.6 120.3   336.8 |      0   27     78      0 |   15697 |   149.5    21.5
      9   107 1396.5  27.9%   560.0 244.7   404.5 |      0   37     70      0 |   14648 |   136.9    27.6
     10   125 1356.6  27.1%   346.7 414.7   568.6 |      0   70     55      0 |   12979 |   103.8    43.8
     11   114 1072.3  21.5%   432.3 314.1   324.2 |      0   33     81      0 |   16407 |   143.9    19.8
     12   183  110.2   2.2%    37.9  17.1    37.2 |    153    0     30      0 |    5820 |    31.8     6.4   <- fill ending
     13+  201   10.9   0.2%     0.0   1.5     0.2 |    201    0      0      0 |       0 |     0.0     0.0   <- settled linger

The exit counts sum exactly to `ticks` in every row, so this is a complete
census of why the loop stopped.

### Three facts

**`queueEmpty = 0` in every filling window.** The drain loop never once ran out
of results while the world was filling. **The producer is not the limit.**
"Results are not arriving" is refuted for this configuration.

**`countCap` is 65-76% of all tick exits.** That exit is `Applied >= MaxApplies`
— `voxel.Stream.MaxAppliesPerFrame`, **192**. A `countCap` tick delivers exactly
192 applies, so ~93% of every window's drained chunks come out of ticks that
stopped because of that number. Throughput = `192 x tickRate`; at the observed
~21 Hz that predicts 4,032/s and the measurement is ~3,163/s.

**The game thread is 21-29% of wall.** There is 3-4x of headroom the pipeline is
not taking, because a **count ceiling — not a cost** — is stopping it.

### The arithmetic

50,000/s at 21 Hz needs **2,381 applies per tick**. The ceiling is **192**.
**Twelve times short.**

And it is the throttle rather than the safety rail its own cvar help text says
it is:

> "As of the 2026-07-24 streaming-speed pass this is a **safety ceiling, not the
> steady-state throttle** — the loop drains until `voxel.Stream.ApplyBudgetMs` of
> wall-clock is spent (or the queue empties, or this ceiling is hit)."

It was raised 64 -> 192 on 2026-07-27 to reach 1,040 chunks/s, when an apply was
expensive enough that 192 filled the 6 ms budget. Apply is now ~0.021 ms, so 192
costs ~4 ms and the count hits first. **It is a cap sized for a system that no
longer exists** — the pattern this session has now hit six times.

---

## 2. The three ceilings bind in order, so raising one alone buys almost nothing

| # | ceiling | value | binds at | why |
|---|---|---|---|---|
| 1 | `voxel.Stream.MaxAppliesPerFrame` | 192 | **~4,000/s — now** | `countCap` is 65-76% of exits |
| 2 | `voxel.Stream.ApplyBudgetMs` | 6.0 ms | **~6,000/s** | apply is 2.5-3.9 ms/tick today, *under* 6, which is exactly why `wallClock` is the minority exit. Raise (1) and this governs: 6 ms / 0.021 ms = 286/tick |
| 3 | `kMaxResultDrainsPerFrame` | 1024 | **~21,500/s** | 1024 x 21 Hz. **A `constexpr` with no cvar at all** — a leg that hits it has no knob to turn and needs a rebuild to move |

Only after all three does per-chunk cost decide anything: at 2,381 chunks/tick,
0.021 ms/chunk is **50 ms/tick = 105% of a 21 Hz game thread**. That is the 50k
budget's "apply must roughly halve" restated per tick, and it is the *fourth*
thing to fix, not the first.

A fifth lever is worth as much as any of them: **tick rate**. 94-125 ticks per
5 s is about 21 Hz. Every doubling halves the per-tick cap required.

---

## 3. What shipped (new files only)

`VoxelApplyBatch.{h,cpp}`, three latched switches. **All three default to
returning exactly what the caller already had**, so with no switch present the
drain loop is byte-identical.

| switch | overrides | absent |
|---|---|---|
| `-VoxelApplyCap=N` | `voxel.Stream.MaxAppliesPerFrame` | the cvar |
| `-VoxelApplyBudgetMs=F` | `voxel.Stream.ApplyBudgetMs` | the cvar |
| `-VoxelApplyDrainCap=N` | `kMaxResultDrainsPerFrame` | 1024 |

**Latched, not cvars-via-`-ExecCmds`.** The cvars *are* settable, but `-ExecCmds`
lands after streaming has begun (`tools/voxel-capture.ps1:114`), so a cold-fill
leg driven that way measures a blend of 192 and the new value and its
time-to-settle is uninterpretable.

**No new traffic counter was added**, because `Voxel apply stages` is already
exactly the right instrument. One new line is printed once per session so that
"the switch is on" and "the switch did not parse" stop being the same log:

    Voxel apply caps: maxApplies=2048 (cvar 192) budgetMs=40.00 (cvar 6.00) drainCap=8192 (shipped 1024)

### Failing readings, both ways

| reading | verdict |
|---|---|
| `Voxel apply caps:` line **missing** | the hooks were not applied / module not called. Nothing here ran. |
| `-VoxelApplyCap=2048`, `countCap` still 65-76%, appl/tick still ~150 | **the override did not latch.** FAIL. |
| `countCap` -> 0, `wallClock` now dominant | **working.** `ApplyBudgetMs` is now the governor. Expected first result; raise `-VoxelApplyBudgetMs` next. |
| `drainCap > 0` | **working**, and 1024 is now binding. Raise `-VoxelApplyDrainCap`. |
| **`queueEmpty > 0` during a FILLING window** | **the success condition and the stop signal.** The drain has caught up with the producers; apply is no longer the bound. Hand back to dispatch/generation. |
| appl/tick rises but `drained/s` does **not** | the caps were not the bound after all. **Revert; do not keep raising.** |

Trap, from this same log: windows 13+ read `queueEmpty=201` with `drained=0`.
That is the settled post-fill linger, not a starved drain. `grep | tail -1`
lands there and has produced four retractions. Read every window; confirm
`drained > 0`.

---

## 4. THE HOOKS — `VoxelWorldSubsystem.cpp`, against `0a71fed`

Three one-line replacements in `FVoxelWorldImpl::DrainResults`. The include is
already present (line 14, from the `-VoxelApplyFast` wave).

**Hook A — line 21993.** Replace:

```cpp
	const int32 MaxApplies = VoxelDebug::GetStreamMaxAppliesPerFrame();
```

with:

```cpp
	const int32 MaxApplies = VoxelApplyFast::AppliesPerTickCap(VoxelDebug::GetStreamMaxAppliesPerFrame());
```

**Hook B — line 22006.** Replace:

```cpp
	const double ApplyBudgetSeconds = double(VoxelDebug::GetStreamApplyBudgetMs()) / 1000.0;
```

with:

```cpp
	const double ApplyBudgetSeconds =
		VoxelApplyFast::ApplyBudgetSeconds(double(VoxelDebug::GetStreamApplyBudgetMs()) / 1000.0);
```

*(Order matters and is already correct: Hook A runs before Hook B in the
function, and `ApplyBudgetSeconds` is what emits the one-shot caps line once all
three effective values are known.)*

**Hook C — line 22021.** Replace:

```cpp
	constexpr int32 kMaxResultDrainsPerFrame = 1024;
```

with:

```cpp
	const int32 kMaxResultDrainsPerFrame = VoxelApplyFast::DrainsPerTickCap();
```

*(`constexpr` -> `const`. It is only ever read in the loop condition and in the
exit attribution at the end of the function; neither needs a constant
expression. Keeping the name means no other line changes.)*

---

## 5. The legs

`tools/voxel-run-leg.ps1` (cold-fill driver; do not pass `-VoxelPerfFlight`).
Read with `tools/leg-summary.sh`, **every window**, never `grep | tail -1`.

| leg | log | `-ExtraArgs` |
|---|---|---|
| **L1** control on the new binary | `cap-ctl.log` | *(none)* |
| **L2** lift ceiling 1 only | `cap-a.log` | `-VoxelApplyCap=4096` |
| **L3** lift 1+2 | `cap-ab.log` | `-VoxelApplyCap=4096`, `-VoxelApplyBudgetMs=40` |
| **L4** lift 1+2+3 | `cap-abc.log` | `-VoxelApplyCap=4096`, `-VoxelApplyBudgetMs=40`, `-VoxelApplyDrainCap=16384` |
| **L5** all three + the sampler fix | `cap-abc-fast.log` | as L4, plus `-VoxelApplyFast=3` |

All with `-ClearEditLog -BudgetSec 300`, e.g.

```
pwsh -File tools/voxel-run-leg.ps1 -LogPath D:\voxelsim\Saved\cap-a.log -ClearEditLog -BudgetSec 300 -ExtraArgs @('-VoxelApplyCap=4096')
```

**L2 is deliberately one switch.** Two caps in series make a throughput number
unattributable to either — the trap this codebase already documents for
`GpuMeshInFlight` — and these are three caps in series.

### Gates

- **G1 (traffic).** `Voxel apply caps:` present with the expected values; on L2,
  `countCap` share drops below 10% of exits. Unchanged `countCap` = FAIL.
- **G2 (throughput).** `drained/s` per filling window: L1 ~3,200/s -> L4
  >= 12,000/s (a 3.75x, the headroom the 25%-busy thread implies). Below 6,000/s
  means a ceiling outside these three binds, and it must be named before more
  tuning.
- **G3 (occupancy).** `%wall` rises from 21-29% toward 60-80%. Flat `%wall` with
  flat throughput is the "caps were not the bound" reading — revert.
- **G4 (correctness).** `holes=0`, `evictions=0`, settle time strictly *down*,
  no `allocFail`. A bigger per-tick apply burst is a render-thread hitch risk by
  design (the documented trade); `p95` frame time is the number to watch, and if
  it becomes unacceptable the fix is a higher tick rate, not a lower cap.
- **G5 (control identity).** L1 reproduces `ahead-on.log`'s 65-76% `countCap` and
  ~150 appl/tick within noise.
