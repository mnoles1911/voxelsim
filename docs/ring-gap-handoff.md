# Ring gaps at LOD boundaries — handoff for a dedicated investigation

Written 2026-07-27 at the end of the GPU programme, after the first hands-on
test on **real terrain**. This is the one symptom that programme never explained,
and it is now the most valuable open problem in the project.

Everything below is evidence, not theory, except where it says otherwise.

---

## CLOSED: root-caused and fixed, 2026-07-27 (night)

Everything below this section is the ORIGINAL handoff, from earlier the same
day. It is kept as-is for the record, with inline notes marking where it was
superseded — do not rewrite the historical content itself.

The ring gap is **root-caused and fixed**. Full data, every leg, every commit:
`docs/measurements/ring-gap-2026-07-27.txt`.

Three stacked causes, each confirmed by an instrumented leg, not inferred:

1. **`ReplacementCovered` treated an absent replacement record as covered.**
   The handoff's inner-hysteresis lead below was the right NEIGHBOURHOOD — the
   retention mechanism it points at already existed — but the actual hole was
   narrower than "no hysteresis on the inner edge": a chunk with no replacement
   record at all was being scored as already-covered and evicted anyway. Fixed
   in `0cf37dd` / refined in `e225d50`.
2. **Entry-scan / refill staleness** left deterministic, PERMANENT no-record
   rings at annulus edges once the anchor pinned — not just a transient
   in-flight gap, a hole that never filled in. Fixed by the quiescent-anchor
   gate on the stale-scan refill in `e225d50`.
3. **Pending-unload records blocked re-admission** when a footprint re-entered
   its annulus (a scan-before-park race): the chunk was mid-eviction, the
   camera receded, and the entry pass SKIPPED the re-desired key as
   "already tracked" because the dying record still existed — then the record
   parked and vanished after the level's last scan, leaving no trigger to
   re-admit it. Fixed by resurrecting pending-unload chunks the entry pass
   re-desires (the unload is cancelled and the still-resident geometry reused),
   `508a2fa`, with the GPU fork's per-frame cost bounded alongside it in
   `f029b13`.

Converged coverage holes (`voxel.Stream.CoverageVerify=1`, read after a 60 s
stationary linger) are now **0 on both meshers**, across repeated instrumented
legs, real terrain, under motion. See the RESURRECTION table in the measurement
file.

**The retention-cap lead in this handoff was tested and was NOT the visible
driver.** Leg C1 ran with `LodRetentionMs=20000` — capRel (the cap-driven
release) was 0 throughout the flight phase, and the holes were UNCHANGED. The
cap can release a chunk, but it was not the thing producing the gap.

**The headless repro problem is closed.** "Why this was never reproduced
headlessly" below correctly diagnosed the flat-terrain spawn bug and proposed
`-VoxelSpawnAt=-84480,53760` as the fix to try. That worked. The gap is now a
**one-command scripted flight**, no manual flying required:

```powershell
& 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' 'D:\voxelsim\ue-project\VoxelEarth.uproject' `
    -dx12 -sm6 -log '-VoxelSpawnAt=-84480,53760' `
    -VoxelPerfFlight=line -VoxelPerfHeading=0 -VoxelPerfPreflightSec=90 `
    -VoxelPerfRun=120 -VoxelPerfSpeed=20 -VoxelPerfLingerSec=60 `
    -VoxelPerfLogInterval=2 -ExecCmds="voxel.Stream.CoverageVerify 1" `
    -abslog="D:\voxelsim\ue-project\Saved\Logs\ringgap.log"
```

90 s pinned at spawn while the cascade warms, 120 s flying at 20 m/s along
+X, 60 s pinned again — read the converged `Voxel coverage holes=` line from
the linger phase. Add `-VoxelNoGpuMesh` for the CPU mesher. See the new-switches
list at the bottom of `docs/measurements/ring-gap-2026-07-27.txt` for what each
flag does.

---

## What the owner sees

Concentric gaps at LOD ring boundaries while flying. Present before this
programme, still present after it.

## What has been RULED OUT, with the test that ruled it out

These are the expensive hypotheses. Do not re-run them.

| ruled out | test | result |
|---|---|---|
| **The GPU mesher** | `-VoxelNoGpuMesh`, real terrain, same anchor | gap still present, "same if not worse" |
| **The ring skirt** | `-VoxelNoRingSkirt` | gap "pretty much unchanged" |
| **The GPU renderer** | gap predates `voxel.Stream.GPU` being on at all | present throughout |

So it is **not a meshing bug and not a rendering bug**. Both producers and both
renderers show it. It is a **streaming / residency / admission** problem.

Two side observations from the same session, worth keeping:

- CPU mesher had **fewer hitches** than the GPU fork (consistent with the GPU
  round-trip tail).
- CPU chunk loading was **anecdotally much slower** than the GPU fork. That is
  the first observation of the GPU mesher's throughput advantage on REAL
  terrain; every measured number in `docs/measurements/` was taken on flat
  fallback (see "Why this was never reproduced" below).

---

## THE LEAD: the inner ring edge has no hysteresis

`VoxelWorldSubsystem.cpp`, the eviction test in `RecomputeDesiredSet`:

```cpp
const double UnloadOuterUU = Preset.OuterMeters * 100.0 * UnloadRingMultiplier; // 1.25x
const double InnerUU       = Preset.InnerMeters * 100.0;                        // 1.0x
const bool bBeyondOuter = DistSq > FMath::Square(UnloadOuterUU);
const bool bInsideInner = LevelKey.Level > 0 && DistSq < FMath::Square(InnerUU);
```

The **outer** edge unloads at `1.25 x Outer` — a 25% slack band, so a chunk
leaving a ring outward stays resident well past the boundary while its coarser
replacement loads.

The **inner** edge unloads at exactly `Inner`, with no slack at all. The header
says so plainly and defers the reason: *"hysteresis on the outer edge only — see
RecomputeDesiredSet for why the inner edge has none in this wave."*

And the annuli **abut exactly**: `Outer[L] == Inner[L+1]`, asserted as an
invariant.

So consider a chunk column crossing a boundary `B = Outer[L] = Inner[L+1]`
**inward** (the camera approaching):

1. At `r < B`, ring L+1 drops its coarse chunk **immediately** — no slack.
2. Ring L must now supply a finer chunk covering the same ground.
3. That finer chunk **does not exist yet**. It needs a dispatch, a mesh, and a
   drain — tens of milliseconds at best, and it queues behind everything else.

Between (1) and (3) that ground is covered by **nothing**. That is a hole at a
ring boundary, appearing only while moving, in a ring shape, on the side the
camera is approaching.

This predicts every observed property:

- **mesher-independent** — it is an eviction/admission race, not geometry
- **skirt-independent** — the skirt adds a retaining wall to a chunk that
  exists; this chunk is absent
- **motion-dependent** — only boundary crossings trigger it
- **fills in if you stop** — the finer chunk eventually arrives

**Status: STRONG HYPOTHESIS, NOT CONFIRMED.** Nobody has instrumented it.

> **SUPERSEDED 2026-07-27 (night):** confirmed as the right neighbourhood, wrong
> exact mechanism. It was not a missing hysteresis band on the inner edge; it
> was `ReplacementCovered` scoring an absent replacement record as covered.
> Two further causes stacked on top (entry-scan staleness, a scan-before-park
> race). See the closed-out section at the top of this file.

### How to confirm or kill it in one run

Log, per frame, every chunk that leaves the desired set via `bInsideInner`, with
its level and the radius it was at. Then check whether a gap sighting coincides
with a burst of inner-edge evictions whose replacement chunks are not yet
resident. If it does, this is the bug.

### If confirmed, the shape of the fix

Give the inner edge the same hysteresis the outer edge has — but **inverted**:
keep a coarse chunk until the camera is *comfortably* inside the boundary, e.g.
unload at `Inner x 0.8` rather than `Inner`. That creates a deliberate overlap
band where both the coarse and fine chunk are resident.

Two things to watch, both already on record:

- Overlap costs residency. `docs/gpu-roadmap-remaining.md` records the seam
  padding fix at **+9.2% resident chunks**; this would add more.
- **Do not reach for the ring cross-fade.** E3 tested it after the seam fix and
  it *still* produced see-through patches. It is explicitly closed as
  do-not-build. Overlapping residency is not the same thing as cross-fading.

---

## Why this was never reproduced headlessly — and why that is not evidence

The scripted 20 m/s flight "does not produce them on current main". **That
finding should be discarded.**

Until 2026-07-27 every headless run spawned at world origin `(0,0)`, and the
loaded tiles cover **X [-122880, -46080] m, Y [15360, 92160] m**. Origin is 46 km
outside them. `TileGridSampler` answered every elevation query with the
missing-tile sea-level default — 1,366,512 of them in a single session — so the
whole world was a **flat plane**.

On a flat plane a level-0 chunk and a level-1 chunk of the same ground render
almost identically. **A temporarily-missing chunk between two flat planes is
invisible.** The one symptom that most needed real terrain is the one thing that
was never tested on it.

Fixed in `299004c`: the spawn is now checked against tile coverage and a miss is
a loud Error naming the exact `-VoxelSpawnAt` to use. Any log older than
2026-07-27 was taken on fake terrain and its performance numbers describe a world
with roughly half the geometry (5.4 M resident quads vs 10.4 M on real tiles).

**First thing to try:** a scripted flight from
`-VoxelSpawnAt=-84480,53760`. It may reproduce on the first attempt, which would
convert this from a manual-only symptom into a headless one — and everything
gets easier from there.

> **CONFIRMED 2026-07-27 (night):** it reproduced. See the one-command recipe
> in the closed-out section at the top of this file, and
> `docs/measurements/ring-gap-2026-07-27.txt` for every leg.

---

## Reproduction recipe (manual)

> Superseded for reproduction purposes by the scripted one-command flight in
> the closed-out section at the top of this file, which also reads the
> converged hole count for you instead of requiring an eyeball "stop moving"
> check. This manual recipe is kept for hands-on investigation.

```powershell
& 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' 'D:\voxelsim\ue-project\VoxelEarth.uproject' `
    -dx12 -sm6 -log '-VoxelSpawnAt=-84480,53760' `
    -VoxelPerfLogInterval=2 -ExecCmds="voxel.Debug 1" `
    -abslog="D:\voxelsim\ue-project\Saved\Logs\ringgap.log"
```

Note the **single quotes** around `-VoxelSpawnAt` — PowerShell splits an
unquoted comma list into an array and the switch never arrives. This programme
was bitten by comma-splitting three times: twice in PowerShell and once in
`FParse::Value`, whose `FString` overload stops at `,` by default and silently
truncated every `-VoxelRing*` switch to its first entry (fixed in PR #136).

Then fly, and **when you see a gap, stop moving**. Whether it fills in splits
throughput from admission and they need different fixes.

## The log lines that matter

- `Voxel rings:` — per-ring `loaded` / `pending` at the moment of the gap
- `Voxel ring dispatch:` — what each ring was given
- `Voxel anchor:` — where the camera was
- `Voxel tile grid: anchor ... is INSIDE the loaded tile coverage` — **confirm
  this appears**, or the run is on fake terrain again
- `Voxel cold-band throttle` — `markTimeouts` must be 0; non-zero means a
  launch path produced no result, which strands a whole (X,Y) column and would
  look like a gap

## Things that look relevant and are not

- **`voxel.Stream.AdmissionBandSkip`** — off, and should stay off. Reaches ~4%
  of the waste it targets and frame rate collapsed with it on.
- **Ring cross-fade** — closed, see above.
- **`kRingCapShare` / `kRingSlotFloorDefault`** — a floor sweep of `{0,2,3,4,4}`
  once collapsed throughput from 49,179 chunks to 558. Change one at a time.
  Also note the sweep on record was run *before* the `FParse` comma fix, so its
  legs may all have parsed to the default — treat those numbers as unproven.

## Ground rules that still apply

`docs/gpu-waves-plan.md` carries 14 of them. The two most relevant here:

1. **Never conclude from a single run.** Two identical configurations have
   repeatedly differed more than the effect being measured.
2. **A lull is not a finish line.** `jobsInFlight=0 pendingJobs=0` occurs
   transiently mid-fill; settle means loaded is unchanged for several consecutive
   samples *and* nothing is in flight *and* nothing is pending.
   `tools/voxel-run-leg.ps1` implements this — use it rather than writing another
   wait loop, which is exactly how the rule got bypassed the second time.
