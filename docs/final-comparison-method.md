# Final comparison: method

**What this document is for.** `tools/voxel-final-comparison.ps1` produces the project's
headline comparison — the quad raster path against the ray marcher, on frame time, loading,
VRAM and geometry submitted. This document states, for every number that harness can print,
**what it means and what it does not**. If a figure from that report is quoted anywhere, the
matching entry here travels with it.

The standard is not "impressive". It is **unimpeachable**. A number that has to be withdrawn
later is worth less than a smaller number that holds — this project has already withdrawn a
"3–14× VRAM saving", an "86× loading headroom", a "30× hitch increase" and a "1.5× frame rate
ceiling", each of which was arithmetically true and directionally wrong.

---

## How to run it

```powershell
tools\voxel-final-comparison.ps1                 # DRY RUN. Prints the plan. Launches nothing.
tools\voxel-final-comparison.ps1 -SelfTest       # Proves the parser. Launches nothing.
tools\voxel-final-comparison.ps1 -Execute        # Spends the legs, serialized.
tools\voxel-final-comparison.ps1 -VerifyOnly -Legs 'name=armId',...
```

**The default is a dry run and that is deliberate.** One UE editor per box, and the owner drives
it. The dry run prints the exact leg list, the exact cvar string each leg receives, and the
checks each leg will face — generated from the same table that would run them, so the plan
cannot drift from what runs.

`-Execute` refuses to start if an editor is already alive, if a `.usf` is newer than every built
game module, or if the self-test fails.

---

## The five refusals, and why each exists

This project's failure mode is not bad measurement. It is **instruments that report success
while measuring nothing** — seven were found on 2026-08-19 alone. The harness is built so that
cannot reach the final number.

### R1 — a leg that cannot prove its own configuration is void, not reported

Every arm declares the cvars that *define* it. The harness resolves the **last** write of each
one and **what set it**, then voids the leg if the value is not what the arm claimed.

Reading the last write is not optional, and reading only one log format is how it goes wrong.
UE writes cvars into a log in four different shapes, and in `Saved/p0-sh-quality-ctl.log` they
disagree with each other:

| line | shape | value |
|---|---|---|
| 673 | `LogConfig: Set CVar [[r.ShadowQuality:5]]` under `File [Scalability]` | 5 |
| 864 | the same, under a **second** scalability pass | 0 |
| 941 | the same, under `File [Engine]` — the project's `[ConsoleVariables]` block | 5 |
| 1807 | `r.ShadowQuality = "0"` — **no log category at all**, from `-ExecCmds`, 18 s later | **0** |

A scan for `Set CVar` — the obvious scan — stops at line 941 and reports 5. **That leg ran 0**,
and it is the control side of the shadow pair, so getting it wrong inverts the whole result.

The fourth shape is the one most easily missed and matters most:

```
LogConsoleManager: Warning: Setting the console variable 'X' with 'SetByCode'
was ignored as it is lower priority than the previous 'SetByConsole'. Value remains '1'
```

**A write you can see in the log did nothing.** The harness records refusals and reports the
value that actually remains.

*What R1 does not do:* it cannot prove a cvar nobody wrote. `NEVER WRITTEN IN THIS LOG` voids
the arm rather than falling back to the documented default — because "the default says so" is
exactly the standard of evidence that failed on `r.ShadowQuality`, and because three of this
codebase's cvar help strings currently state the wrong default (`voxel.GI.Enabled` says
"0 = off (default)" on the line below the line setting it to 1).

### R2 — two legs with different configuration fingerprints do not go in one table

Each leg gets a fingerprint built **from its own log**, not from the files on disk. It covers 28
cvars, 14 command-line dimensions that cannot be reached by `-ExecCmds` at all, and the build
that ran. Two legs may share a table only if they differ **exclusively** in the keys that axis
declared as its variable. Otherwise the table is refused and the differing keys are named.

This is the rule that stops `18.99 ms` and `34.72 ms` sitting in one column. Both are real.
**They describe different games** — shadows began working on 2026-08-19 and are now on by
default, at a cost of +15.83 ms at p50.

*What R2 does not do:* it does not prove two legs are comparable. It proves that nothing the
harness knows how to watch differs between them. A change nobody thought to fingerprint —
worldgen, a tile cache rebake, a driver update — passes silently. The build fingerprint is the
partial defence, and it only exists for legs this driver launched.

### R3 — a metric whose meaning changes between arms is labelled in the cell, or omitted

Two counters on this project change meaning between arms, and both have already produced a
wrong conclusion:

- **`loaded=` counts geometry publication.** Under `voxel.Brick.SuppressQuadMesh 1` chunks
  publish nothing, so it collapsed to **3,243** against 50,504 elsewhere — while the brick pool
  filled normally to 87,753. The pipeline was healthy; the instrument was measuring a different
  quantity. Cold fill derived from it therefore **cannot cross the loading pair at all**, and
  the harness reports it for the control side only, saying so in the cell.
- **`hitches` counts frames over a fixed 33.3 ms** (`VoxelDebug.h:419`). At p50 18.85 that is
  1.4% of frames; at p50 34.72 it is **70.3%**. It is a step function of the median, not a
  measure of variance — and p50 34.72 against p95 35.38 is a 0.66 ms spread, which cannot have
  70% outliers. This was briefly written up as a "30× hitch increase … a playability problem"
  and retracted. The harness labels it `DEGENERATE` and omits it whenever p50 approaches the
  threshold.

The reason prints **beside the figure**, never in a footnote, because a caveat one scroll away
from its number is a caveat that will be dropped when the number is quoted.

### R4 — totals are total-over-count, never a sum of per-item means

`voxel.Brick.Stats` prints a `THIS ARM'S TOTAL` line that sums three per-chunk means whose
counts differ by 870×. On the Phase 5 arm it read **1.127 ms/chunk** where `total_ms / count` is
**0.389** — a 3× inflation that **reversed the verdict**. It failed in the favourable-looking
direction only by luck; had it inflated the control instead, nobody would have recomputed it.

The harness parses each term's `(ms, count)` separately and never reads a printed total. The
same rule governs the gather census: cumulative quads over cumulative gathers, **differenced
across the settled window only** — because the raw cumulative includes the empty pre-stream
pool, and a single window is a statement about a phase rather than a configuration.

### R5 — nothing is read before it settles, and settle must be proved

Settle is the **first** `Voxel streaming:` window that is simultaneously idle
(`jobsInFlight=0 pendingJobs=0`) **and at the run's peak `loaded=`**.

The peak clause is the one that costs a result if dropped. Idle alone is not convergence: the
GPU meshing fork produces genuine mid-fill lulls, and legs "settled" at `loaded=40,615` against
a true final 43,328 — making the fork look ~12% faster while being scored at 94% of the work.
**A lull is not a finish line.**

Two further guards: some *earlier* window must have had work in flight (otherwise "0 jobs" may
mean nothing had started, and two identical reads of a world that never filled agree perfectly),
and windows must *continue* after the settle point (otherwise the settled state was never held
long enough to read).

---

## What each number means, and what it does not

### Frame time — `p50 frame (post-warmup)`

**Means:** the median frame time in milliseconds over the post-warmup window of a static, pinned
pose at `-61440,-61440`, 2560×1440, sun frozen at 12:00 / 03-20, with the quad raster path
(`voxel.March 0`) against the marcher drawing from the brick pool (`voxel.March 2` +
`voxel.March.Source 1`). Replicated, with the observed spread printed.

**Does not mean:**
- **Not the frame players will see.** A stale `sg.*` preset — ResolutionQuality, ViewDistance,
  PostProcess and Effects all at 0 — has been in force for *every leg this project has ever
  taken*: `r.ViewDistanceScale 0.4`, material quality Low, `r.DetailMode 0`, no volumetric fog,
  no ambient occlusion. Every A/B survives this because both arms shared it. **No absolute
  number does.**
- **Not a claim that the marcher wins.** As of 2026-08-19 it does not. See "What is not
  measurable yet" below.
- **Not the tail.** `p95` is printed and explicitly not compared: it has disagreed with *itself*
  by 3.8 ms inside a single leg (24.52 on the full-run line, 28.31 on the post-warmup line).
  A metric that cannot reproduce within one leg cannot resolve a difference between two.

**The bar a delta must clear.** The rig's within-config spread is **0.18 ms (~1%)**, measured
over four equivalent legs. The harness requires a delta to clear the greater of that floor and
twice the worst within-arm spread the legs actually produced. Below it, the output reads
`UNRESOLVED` and **no direction is printed** — a small number with a confident sign is exactly
what gets withdrawn later.

**The identity check rides this pair.** `mode2frame − mode0frame` must equal
`marchMs + emitMs + scratchMs`. It closed to **0.137 ms (2.5%)** against the 0.18 ms floor,
which retires a whole class of doubt — hidden barriers, un-timed clears, pass setup, descriptor
churn — without enumerating them. If it stops closing, something is being paid that no bracket
names, and the frame-time row is not trustworthy however clean it looks.

### Loading — `worker cost per chunk` and `packs/s`

**Means:** worker-thread milliseconds per chunk delivered to the pool, computed as
`(mesh_ms + fill_ms + pack_ms) / pack_count`. The denominator is the pack count because every
chunk reaching the pool is packed exactly once, whereas mesh and fill are mutually exclusive
across the two arms — and the mesh term on the suppression arm covers 96 game-thread *edit*
chunks that are not the arm at all. `packs/s` is the pool's own first-to-last-pack throughput,
and it is the one throughput figure that means the same thing on both arms.

**Does not mean:**
- **Not chunks/s, and not wall-clock cold fill.** Removing apply cost removes *one* serial stage
  of several. The recorded 2.49× is worker time per chunk; converting it into a streaming rate
  is unmeasured.
- **Cold-fill seconds do not cross this pair** (see R3), and are quantised to the 5 s census
  interval at both ends, so differences under ~10 s are not resolvable *at all*. `35.4 s` against
  `35.6 s` was correctly reported as "indistinguishable at 5-second resolution", not as
  "0.2 s slower".
- **The suppression arm renders the world empty.** It is a stopwatch arm. Never a screenshot arm,
  and never shipped at 1.

### VRAM at settle — `CONTENT` and `COMMIT`, and never crossed

**Means:** two rows that must not be combined.

- **CONTENT** — resident quad bytes (`residentQuads × 8 B` at settle) against resident brick MiB
  from `voxel.Brick.Stats`. **Both pools are resident in the same leg**, because both producers
  currently run, so this is the same frame, the same ground and the same instant. It is the
  strongest comparison this project has, and it needs no arm switch.
- **COMMIT** — the quad pool's capacity in MB against the brick pool's committed MiB. These are
  *reservations chosen by their authors*. A commit ratio is a sizing decision, not a
  representation result.

**Does not mean:** an earlier draft of the ray-marching plan claimed a **3–14× VRAM refund** by
comparing brick *resident* against the quad pool's *committed* 2,197 MB — roughly 3× the
cascade's own quad content. Most of the apparent refund was allocator slack. The measured
format saving on identical ground is **1.74×**. The harness emits the two rows separately and
says so in both cells.

Two further limits: resident bytes bound the live set **only while `evictions = 0`**, and the
harness flags the row if that stops holding. And this pose is not the saturating one — the
shipping default refused **34,937 chunks** at the temperate pose, which is a different leg and
is not measured here.

### Geometry submitted — `quads per camera gather`

**Means:** camera plus shadow quads submitted per camera gather, as cumulative quads over
cumulative gathers differenced across the settled window. The "new" arm suppresses quad
submission with `voxel.Stream.GPUCullDebugDrawNothing 3` while the marcher draws.

**The zero is a measured zero, not an absence.** Both suppression paths in
`VoxelGpuPoolComponent` call `RecordGather` *before* `continue`, deliberately, so a suppressed
gather appears in the census as a gather that submitted nothing. `cameraGathers = 0` would mean
the instrument stopped running — the census is silent at its default
`voxel.Stream.GPUCullStatsPeriod 0` — and the harness distinguishes the two rather than
reporting either as success.

**Does not mean:** the quad path is **not retired**; that is P4. This arm is the Phase 4 *shape*
on today's binary, and the report says `EMULATED`. The quad pool is still resident and still
costs its VRAM in this configuration. Like the loading suppression arm, **it is a counter arm,
never a screenshot arm** — the raster path is submitting nothing and the marcher is writing to
scratch, so the visible image is not the game.

**`S`, the shadow multiplier**, is reported on the raster side: total ÷ camera quads per camera
gather, measured at **2.929** — the raster path resubmits 21.0M extra quads across four cascade
passes on top of the camera's 10.9M. That a marcher would instead pay one secondary ray per
primary hit against an already-resident pyramid is a **design property, not a measurement**,
until a shadowed marcher arm runs.

---

## What is not measurable yet — and the report says so

These are printed in the report's own `UNMEASURED` section, by name, so they can be struck off
individually. They are listed rather than omitted: an omission reads as an oversight, a listed
gap reads as a boundary.

1. **The marcher does not beat the raster path, and cannot yet.** At the last matched pair it
   *added* 5.4 ms at p50 (19.72 → 25.23). It cannot win until hierarchical empty-space skipping
   lands. Every marcher cost measured so far is a **dense walk**: level 0 only, no cone LOD, no
   ring transitions, no mip pyramid.
2. **The 9.19× skip ratio the whole case rests on has never been measured on the real
   structure.** It came from a two-level mip over the flat 512³ occupancy volume — a different
   structure, different cell sizes, different restart behaviour. The brick pyramid it is quoted
   for has never been walked with skipping at all. The cost model `0.13 + steps × 5.9 µs` is
   dead for the same reason.
3. **Everything is level 0.** The chunk index is level-0 only by construction; R0 spans 0–128 m.
   No number here describes a ray crossing a ring boundary, and none describes the 4 km cascade.
4. **The marcher's frame-time tail is unknown, not fine.**
5. **Absolute frame times describe Low settings**, not the shipping game.
6. **Loading is measured as worker cost, not end to end.**

---

## Self-test: how the instrument is proved before it is trusted

`-SelfTest` runs the parser against 2026-08-19 legs whose answers are already written down in
`docs/measurements/armA-drawpath-ceiling-2026-08-19.txt`. It launches nothing and needs no
editor. It is a **precondition of `-Execute`**, not a separate chore: a parser that mis-reads a
leg whose answer is known will mis-read one whose answer is not, and will do it silently.

Ten positive cases check recorded figures reproduce. **Six negative cases matter more** — they
check the driver *refuses* what a naive parser would happily report:

| case | the driver must |
|---|---|
| `p0-sh-quality-ctl` / `r.ShadowQuality` | read **0** from an uncategorised runtime echo at line 1807, not 5 from the last `Set CVar` at 941 |
| `p2-suppressmesh-r1` / `loadedChunks` | refuse the metric, because quad-mesh suppression changed what it counts |
| `p0-sh-nooctree-r1` / `hitches` | label it `DEGENERATE` against a p50 of 34.72 |
| `p0-sh-quality-ctl` vs `p0-sh-nooctree-r1` | **refuse a shared table** and name `r.ShadowQuality` as the reason |
| `p3b1-src1-r1` / `marchMs` | refuse it as a cost result — 1,138 of 21,340 tiles drawn is wasted stepping, not an indirection price |
| `p5-phase5-r1` / worker total | compute **0.389**, not the **1.127** the log itself prints |

Current state: **16 pass, 0 fail**, including an end-to-end case that reproduces the identity
check's 0.137 ms residual from five separately parsed inputs.

---

## Traps carried forward from the harness this one builds on

- **The deferred capture.** `-ExecCmds` fires at startup and would profile frame 1 of an empty
  world — which it reports as a successful capture. That has cost three separate runs. The
  capture is deferred to preflight plus half the run.
- **The sun must be pinned.** A leg compares two things taken at different instants of the same
  run. A moving sun means the two halves describe different lighting. The harness voids any leg
  whose measured sun drift exceeds 0.01°, and records the pose as well as the drift — two frozen
  legs at *different hours* are no more comparable than a frozen and a moving one.
- **A partial leg reads as a slow configuration.** Completeness is checked against the leg's own
  command line, not a hardcoded window count, so changing the profile cannot make the check
  quietly permissive.
- **Contention is mutual.** `voxel-audit-leg-overlap.ps1` runs after every executed set: the
  launch guard checks liveness at *start* and cannot speak for a run that was already finishing.
  If two legs overlapped, **both** are void.
- **The shader tree and the built binary must agree.** A complete `.usf` with unbuilt matching
  C++ is as fatal as a half-finished one — it killed a 250 s leg after 9 s on a global shader
  compile error. This driver does not build; it refuses to start a ~1 hour serialized run in
  that state and says why.
