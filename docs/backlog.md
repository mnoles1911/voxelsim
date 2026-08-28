> ## STANDING PERFORMANCE TARGETS (owner-set 2026-08-24 — maintain these)
>
> | metric | target | stock baseline 2026-08-24 |
> |---|---|---|
> | cold start to settle | **<= 5 s** | 45.5 s |
> | streaming throughput | **50,000 chunks/s** | 3,618/s |
> | frame p95 while MOVING >= 20 m/s | **< 10.00 ms (>100 fps)** | 44.00 ms (23 fps) |
> | steadiness while moving | **<= 0.10% stutters** (frame > 20 ms) | 31.4% |
>
> Priority order: **chunks/s, FPS, cold start.** Stationary does not count.
> Full scoreboard and the log line behind every figure: **`docs/SCOREBOARD.md`**.
> **An arm that improves cold start or throughput and worsens the moving p95 is
> not a win.** Report the STOCK number as the headline — armed configs are an
> upper bound on a setup the owner does not run.

# Backlog

One place for everything known-and-not-done. Written 2026-07-25 after the
worldgen v8 climate wave; **section 0 added 2026-07-28** after the streaming and
draw-path programme merged (PR #165).

Each item says what it is, why it matters, what it costs, and what unblocks it.
Items are grouped by **what kind of decision they need**, not by subsystem —
because the thing that stalls work here is usually "whose call is this", not
"where does the code live".

`docs/status.md` **stopped being written on 2026-07-29** and is now a historical
record, not the current one. Since then the chronological record is the merge
commit messages plus the dated document each programme leaves in `docs/` — water:
`docs/water-architecture.md`; assets: §10 below and the documents it cites. This
file is the forward-looking list. When an item lands, delete it here and say where
the result was written.

**Read in this order** (staleness reviewed 2026-08-19). §10 is the newest and
describes the world as it is now. §0 is the engine-performance front, but its
numbers are from 2026-07-28 and predate both the 192M pool and assets in the
world. §§1-7 predate August entirely; where this review falsified an item it now
says so inline.

---

## 0. ENGINE PERFORMANCE — the current front

### 0.0n A capture run that finishes its work never exits -- symptom bounded, cause open

`-VoxelLoadingShotAt` does not quit after its last shot when that shot lands
AFTER hand-off. The front end tears down at hand-off and takes whatever ends the
run with it, so the image IS produced and the process lives on forever.

Measured 2026-08-25, the expensive way: a `-Shot Loading -At '2,50'` run held the
box for **241 minutes** and cost another session two queued jobs. It was not
hung -- both shots fired, the log was still growing at 45 MB, and it had
rendered 1,577,156 settled frames at ~2.5 cores with `Responding=True`. It had
finished and was never told to stop.

`-At '1,5'` (both shots inside the loading screen) exits 0 cleanly. Anything past
the ~15 s gate does not.

**SYMPTOM IS BOUNDED, CAUSE IS NOT.** `tools/voxel-ui-capture.ps1` now runs with
a `-TimeoutSec` (default 600) and kills a run that outlives it, so this can no
longer eat a night. But the run still does not end itself, so anyone driving
`UnrealEditor-Cmd` directly -- a leg script, a CI job, a hand-typed command --
gets the original behaviour.

**`-VoxelMenuWatchdog` CANNOT BE THE FIX and it is worth understanding why.** It
is a FRONT-END mechanism, and the failure is precisely that the front end has
already torn down. A watchdog owned by the thing that goes away is silent in
exactly the case it exists for -- the same shape as an ini branch that never
resolves and a cvar that was never a cvar. Whatever ends these runs has to
outlive the front end.

### 0.0k FIXED 2026-08-25 -- menu AND loading screen photographed; the world is held until the view settles

Found 2026-08-25, first time `tools/voxel-ui-capture.ps1 -Shot Menu` has ever
been run. It has never produced an image: `ue-project/Saved/` contains no
`ui-capture-*.log` before today. Three runs, three fatals, exit code 3 every
time, no screenshot.

**The front end reaches its menu correctly.** Every run logs
`VoxelFrontEnd: active`, `menu font loaded`, and `0 save(s) listed` -- the
subsystem, the Macondo font, the background art and the save-list query all
work. The menu is built. What kills the run is the WORLD ticking underneath it.

**The mechanism.** In `EVoxelFrontEndState::Menu` the front end holds streaming
by leaving `ChunkOwner` null, so `UVoxelWorldSubsystem::Tick` returns on its
first line. Nothing holds the other world-touching tickers, and no pawn has
been placed yet -- so the player viewpoint is the world ORIGIN regardless of
`-VoxelSpawnAt`. Those tickers then query worldgen at (0,0), the fine tier has
no tile (-1,-1) baked on this box, and `FVoxelFineTileStreamer` is fatal by
design on a gate leak in an unattended run.

Two independent offenders confirmed by callstack, which is what makes this a
class and not a switch:

    UVoxelWaterSubsystem::Tick
      -> RefreshImplicitWater          (VoxelWaterSubsystem.cpp:6176)
      -> EnsureWorldgenColumn -> Amplifier::evalSurface -> gate leak

    AVoxelOceanActor::UpdateUnderwaterState   (VoxelOceanActor.cpp:321)
      -> IsUnderwaterAtWorld -> GetWaterFillAtWorld -> World<8>::materialAt
      -> gate leak

The log's own diagnosis is exact and worth quoting: *"The residency tick has
NEVER RUN (no ring centre), so nothing has been prefetched for any position."*
That is the deadlock stated plainly -- one subsystem is held, the others are
not, and the unheld ones depend on the held one's prefetch.

**What was tried, and why the workarounds are the wrong fix.**
`-VoxelSpawnAt=-61440,-61440` (the baked 2x2 centre) got the run further and
loaded `resident=4` tiles, but did not help: the camera is at the origin during
the menu, not at the spawn. `-VoxelWaterMarkerOnly=1` removed the water
subsystem offender and the ocean actor took its place. Chasing each one with a
switch is whack-a-mole, and every switch changes engine behaviour in a shot
whose whole purpose is to show what the front end really looks like.

**`-VoxelFineTileGateFatal=0` IS NOT THE ANSWER** even though the log offers it.
It answers elevation with sea level and yields a screenshot that looks right and
is not reproducible.

**FIXED in d073de6.** `VoxelFrontEnd::IsWorldHeldForMenu(World)` is the
predicate, DERIVED from `UVoxelWorldSubsystem::HasWorldSessionStarted()` -- the
same ChunkOwner the streamer already gates on -- rather than a flag the UI
module sets on state transitions. That choice paid for itself: a stored flag
would have needed all four callers known in advance, and only two were.

**THERE WERE FOUR, NOT TWO, AND THAT IS THE REUSABLE LESSON.** They surfaced one
at a time, each hidden behind the last -- silencing water revealed the ocean
actor, which revealed the sky rig, which revealed the clipmap veil probe:

    UVoxelWaterSubsystem::Tick -> RefreshImplicitWater
    AVoxelOceanActor::UpdateUnderwaterState
    UVoxelSkySubsystem::OnWorldBeginPlay -> SpawnRig
    AVoxelClipmapActor::IsCameraUnderRock

A fail-fast gate reports the FIRST caller, not the class, so it turns a class of
bugs into a QUEUE of bugs and the queue is invisible. This entry said "two
offenders, confirmed by callstack" and that was true and still wrong. Anything
of the shape "N things call X while X is unavailable" has to be enumerated from
the call graph, not discovered by repeated fatals.

Three of the four are early returns. The sky is not: `OnWorldBeginPlay` fires
once, so skipping it means no sky for the session -- its rig spawn is deferred
to the first unheld tick. The `Z=0` fallback already in `SpawnRig` was NOT
taken: that constant is documented for a world with no terrain subsystem at
all, which is permanent, and borrowing it for a temporary condition would leave
the rig referenced to sea level for the whole session afterwards.

NOT ATTEMPTED, deliberately: the tree carries another session's uncommitted
edits to `VoxelWorldSubsystem.cpp/.h` and `VoxelMarchRenderer.cpp`. Building
would compile their half-finished work and corrupt the DLL pair they are
measuring against.

**A SECOND FRAME-EXACT CAUSE, found when the loading screen still died after
the menu was fixed.** The hold released on `HasWorldSessionStarted()`, which
goes true the instant `StartWorldSession` runs -- but `BeginPlayerSession`
spawns and possesses the pawn on that SAME tick, and
`APlayerCameraManager` has not ticked yet, so `GetPlayerViewPoint()` still
reports the pre-possession world ORIGIN for the rest of that frame. Measured:
pawn spawned 04:13:04.200 frame 31, gate leak 04:13:04.201 frame 31.

I had this diagnosed as "the pawn is not placed yet" and was about to place it
earlier. The log said otherwise. The pawn is placed; the VIEW lags it by one
frame. The fix is `HasWorldSessionSettled()` -- session started AND
`GFrameCounter` moved on -- and deliberately not "does the controller have a
pawn", because it does, synchronously, which is why that test would have passed
while the view was still wrong.

`-Shot Loading` now exits 0 and produces images. Stages 2-6 of
`docs/front-end-local-verification-handoff.md` are UNBLOCKED, and the full menu
-> loading -> hand-off cycle is verified: gate closed after 15.01s (world
ready), music faded over 1.50s, handed off.
`tools/voxel-ui-capture.ps1 -Shot Menu` exits 0 and writes VoxelMenu00000.png
with no workaround switches -- and specifically without
`-VoxelFineTileGateFatal=0`, which the fatal itself offers and which would have
answered elevation with sea level and produced a picture that looks right and
is not reproducible.

---

### 0.0m FIXED -- the ini override path never resolved; now it does, proven in pixels

Open, 2026-08-25. `FVoxelMenuLayout` reads every layout number from
`ue-project/Config/DefaultVoxelUI.ini` at startup, and that file's own header
explains why it matters: *"there is no editor available to most people who will
want to nudge this screen ... the next best thing is that moving the title up
four pixels is a text edit and a relaunch, with no compiler."*

It may not work. Evidence, such as it is:

  * A capture run with `MainPanelHalfWidth=400.0` under a correct
    `[VoxelUI.Layout]` section produced a pixel-identical image, and the log
    had no `N value(s) overridden from DefaultVoxelUI.ini` line -- which
    `VoxelUITheme.cpp:182` emits whenever `OverrideCount > 0`.
  * No `Saved/Config/WindowsEditor/VoxelUI.ini` has ever been generated, across
    every run to date. A custom ini GConfig has actually registered would
    normally produce one.
  * The shipped file contains NO keys, only comments -- its header calls an
    empty file "the normal case". So nobody has ever exercised the path. This
    is not a regression; it has never been demonstrated.

**THE TEST THAT PRODUCED THE FIRST BULLET WAS CONTAMINATED, and by me.** I
created the probe with `cat >` without checking what was already at that path,
clobbering the tracked 1,411-byte file, and then deleted it afterwards as
though it were mine. Another session restored it from HEAD. The file WAS
present with correct content while the capture ran, so the null is not
explained by absence -- but a test run against a file I had just destroyed is
not evidence I would accept from anyone else.

**RESULT: ABSENT. The branch never resolved.** The unconditional diagnostic
printed `ini branch 'VoxelUI' ABSENT; 0 value(s) overridden` on its first
capture. Reading the bare name `"VoxelUI"` relied on GConfig resolving it to
`Config/DefaultVoxelUI.ini` by convention, and it does not -- so every getter
had returned false for every key since the file was added.

**FIXED** in the same pass: a full path through `FPaths::ProjectConfigDir()`
plus an explicit `GConfig->LoadFile`. The path alone is necessary and not
sufficient -- the getters call `FindOrLoadNoSafeReload`, and a config nobody
asked to load has no branch to find.

**Proven in pixels, not just in the log**, because the first confirmation was
the exact shape of null that had already fooled me twice: `TitleBoxWidth=900`
gave "1 value(s) overridden" and a byte-identical capture, which is CORRECT
(the title already fits in 720, so widening cannot move it) and is
indistinguishable from a no-op. Re-run with `TitleBoxWidth=400`, which must
clip: the title came back at centre-267, and 400 x 1.335 / 2 = 267 exactly.

The ABSENT case is now a **Warning**, not a Log. A silently-absent config branch
is what survived from the day the file was added until it was measured.

Related: 0.0l, whose diagnosis this probe was meant to serve and did not.

---

### 0.0l The menu TITLE is clipped at both ends, measured -- "VOXELMARK" renders as "OXELMAR"

Found in the first menu capture, 2026-08-25. NOT a font-load failure: the string
is `"VOXELMARK"` and `menu font loaded` is in every log.

**Measured off the capture, not eyeballed** -- my first read of the screenshot
was that it clipped, my first measurement said it fit, and the pixels settled
it. Gold title pixels in `VoxelMenu00000.png` span x = 933..1626 in a 2560-wide
image: centre-347 to centre+346. Symmetric to within one pixel, which is a
container clip and not a layout offset. 347 = 260 x 1.335, i.e. exactly
`MainPanelHalfWidth` at this capture's Slate layout scale.

**So the clipper is the `SBox` in `SVoxelMainMenu.cpp:285`,**
`.WidthOverride(L.MainPanelHalfWidth * 2.f)` = 520 local units. That number
came from the Godot original's `_main_panel` offsets (-260,-400)-(260,400),
where it governed the BUTTON column; in Slate the title is inside the same box
and gets clipped by it. Roughly a quarter more width is needed than the box
gives.

Note that measuring the TTF alone does not predict this. `MacondoSwashCaps` at
nominal 84 px measures 483 px advance / 518 px ink for the string, which fits
520 comfortably -- Slate lays it out materially wider than the raw face
metrics, so the only trustworthy measurement is the rendered capture.

ADR-0009 flagged the risk ("Macondo at 84 px will not lay out identically in
Slate and Godot; expect a few pixels of difference in title width and
centring"). It is real, and it is not a few pixels.

THE FIX IS A DESIGN CALL, not a mechanical one, which is why it is not already
made: either the title stops being constrained by the button panel (move it out
of that `SBox`, or give it its own wider one -- it should size itself rather
than inherit a number meant for buttons), or `TitleFontSize` comes down from 84.
The first preserves the intended look; the second changes it. Owner's call.
Nothing here should be tuned by arithmetic alone -- re-capture and look.

### 0.0j Chunks admit LEFT-TO-RIGHT and FAR-BEFORE-NEAR — FIXED: `NearestAdmit` default ON since 2026-08-24

**SUPERSEDED 2026-08-28 — the old status ("NOT YET IN ANY BINARY") went stale.**
The fix IS in every shipping binary: `NearestAdmit` shipped default ON on
2026-08-24 (`VoxelStreamAdmission::NearestAdmitEnabled`; the SCOREBOARD's
2026-08-24 legs ran it as the default), and R0 settle improved 15.8 -> 12.7 s
(**-20%**) at no fps cost. Chunks now admit nearest-first. `ViewBias` remains
authored, default off, and still never feeds eviction. The standing instruction
below — "do not treat a sighting of this symptom as the fix failing until a
binary containing `-VoxelNearestAdmit` has actually been run" — is MOOT: every
stock binary contains it, so a fresh sighting WOULD now be the fix failing and
should be measured (`admMeanM R0..R6`, `nearestAdmit` counter), not argued.
The diagnosis below is kept as history.

Owner-reported twice, most recently 2026-08-23 while the fix was still in merge:
*"chunks are still streaming and rendering in from a left to right basis in
front of player. that still feels wrong in terms of prioritization"*. **He is
right, and what he is seeing is expected — the fix has never been built.** It
conflicts with Phase 2 in three places inside `AddCandidate` and is being
integrated. Do not treat a sighting of this symptom as the fix failing until a
binary containing `-VoxelNearestAdmit` has actually been run.

**ROOT CAUSE, confirmed in code rather than inferred from the plot.** A gate
inside `AddCandidate` — `AdmissionsThisLevel >= Cap/4` — rejected everything past
the first **512** admissions **IN SCAN ORDER**. The cell sweep is row-major from
the southwest corner, so whenever a level's missing set exceeds 512 (cold start,
teleport, and every refill of a drained ring during flight) the admitted set is a
row-major strip beginning at the **far corner** of the sweep square, running
west→east. That is both halves of the complaint from one gate.

**THE SAME GATE IS THE THROUGHPUT COLLAPSE.** Budget is spent admitting far
slabs; `DropFarthestOverCap` then throws them straight back out; they are
re-collected on the next call. Net-zero work. This is what drives
`candidatesRejected` to ~1.1M per 5 s window while `dispatched` decays 16,227 →
9,512. An in-file note already said it plainly and nobody had connected it to the
visible symptom: *"each refill admitted Cap/4 = 512, and the truncation pass
threw ~500 straight back out."*

**WHAT WAS ALREADY CORRECT, so nobody re-fixes it.** The sort
(`SortPendingQueues`, distance-primary, with `BiasedSortKeySq` clamping interior
coarse chunks to their ring's inner radius), the truncation (`DropFarthestOverCap`
drops farthest), and the dispatch pick (ring-floor deficit, then nearest across
queue heads) are ALL proximity-correct. They faithfully ordered a queue that only
ever CONTAINED the far strip. The last stage did not win here — the first did.
**No stage anywhere consulted view direction.**

**THE FIX, authored, unbuilt:**
  * `-VoxelNearestAdmit` — the gate keeps its budget but commits the **nearest**
    Cap/4 rather than the first Cap/4, collected during the sweep and committed
    after it, sorted by the same key the queue stores. This makes enumeration
    order irrelevant rather than reordering the enumeration, which is also what
    makes it merge-safe against Phase 2's margin strips.
  * `-VoxelViewBias[=k]` — `key = BiasedSortKeySq * (1 + k(1-cos t)/2)`, layered
    inside `PrioritySortKeySq` so one number steers the cutoff, the sort, the
    truncation and the cross-level dispatch pick. Bounded and multiplicative, so
    distance dominates and no ring can starve by facing. **Never feeds
    eviction** — a 180 deg turn must not evict what is already resident.

**WHAT TO READ WHEN IT IS FINALLY RUN:** `admMeanM R0..R6` (mean 2D admit distance
per ring) — row-major truncation pins a budget-bound ring near its FAR edge from
the first window; nearest-first starts at the inner edge and walks outward. That
is the ordering itself, not a proxy. Plus `nearestAdmit` non-zero in treatment and
exactly 0 in control. Judge safety on PER-RING `loaded`/`pending`, never aggregate
throughput — level-primary ordering once measured "R3/R4 at 0 loaded chunks for
90 s" while the aggregate looked healthy.

### 0.0i The uncovered instrument: DIAGNOSED and FIXED (both halves, one cause)

Authored 2026-08-23, NOT YET BUILT OR MEASURED. The gate below is what the
next leg has to read; until it does, nothing here is a result.

**THE ORIGINAL SYMPTOMS,** from three headless captures at 120/400/1200 m
(`Saved/capture-zcut-alt*.log`, read with `tools/read-holes.sh`): `byLevel`
read `L0=<everything> L1..L5=0` in EVERY window of EVERY rung, and `byReason`
sat at 100% in ONE bucket per window, flipping between `never` and `unattrib`
with nothing streaming (`records/level R0[+0 -0 ev0]`, `jobsInFlight=0`) and
the picture on screen unchanged.

**THE LEAD IN THE PREVIOUS VERSION OF THIS ENTRY IS FALSIFIED.** It said
`annotWrites pending` freezing at 290,332 was the mechanism -- the annotation
writer dying and the reason bits going stale. It is not:

  * The freeze is normal. `annotWrites` counts ADMISSIONS; a settled world
    admits nothing. Same window, same log: `records/level R0[+0 -0 ev0]`.
  * The correlation does not hold in either direction. On the **alt400** rung
    the counter CLIMBS 27,661 -> 48,018 through windows reading
    `unattrib=100%`, and sits FROZEN through later windows reading
    `never=100%`. The 120 m rung's apparent coincidence was one rung's
    ordering read as a law.
  * `[ANNOTATION WRITER DISARMED ...]` printed **zero times** in all three
    logs, so `voxel.March.IndexGpuResident` was off throughout and the known
    P2 interaction is not the story either.

**THE ACTUAL CAUSE, one for both halves.** `uncovered` and its attribution
trigger on `bCrossedAbsentChunk` -- "the ray crossed a chunk that is not
resident" -- and **the streamed set is a thin SHELL around the surface**:
`resident0=28103` over a ~6,400-column ring-0 footprint is 4.4 chunks per
column. Everything above it (sky-band trim) and below it (buried skip) is
absent AND CORRECTLY SO, and the camera flies in that air. Ring boundaries
are horizontal CYLINDERS, so segment 0 starts AT THE CAMERA. Therefore:

  * the first absent chunk on essentially every ray is the air the camera is
    sitting in, at level 0 -- and "first crossing wins" hands the whole
    histogram to it. That is `byLevel L0=100%`, at all three altitudes, and
    it is why `attributed == uncovered` held EXACTLY in every window: a
    tautology, not a health check.
  * one index cell (the camera's own chunk) therefore supplies the reason for
    ~100% of attributed rays. That is why the split is all-or-nothing rather
    than a mixture, and why it flips wholesale between windows.
  * `uncovered` itself read **25.27% of ALL RAYS on a settled, stationary
    world**, unchanged window after window. Its doc comment's "known
    over-count" caveat was the entire signal.

**THE FIX (authored, in this branch).** A hole is not "a chunk that is not
there"; it is a GAP IN GROUND THE STREAMING SYSTEM HOLDS. So the breakdown
now triggers on a new flag, `bCrossedShellAbsent`, set only where the absent
chunk is FACE-ADJACENT to a resident one (`VoxelMarchAbsentTouchesShell`, six
index loads, early-out, level-2 permutation only). New counter `uncShell`
prints beside `uncovered` with the ratio, so the old counter's contamination
is a reading rather than an argument. `bCrossedAbsentChunk` is UNCHANGED --
the fallthrough gate keeps its own flag and the shipping arm is
byte-identical. A new `MarchIndexLevelPopulated` uniform (one bit per index
grid slot) short-circuits the shell test for a slot that streams NOTHING,
which is the only reason `-VoxelMaxRingLevel=0` can still be the proving run.

Two silent drops fixed alongside, both the same shape: the kernel's sentinel
guard was the literal `< 6u` and the CPU's sum loop stopped at 6 while the
enum has carried SEVEN level words since the 8 km ring landed, so every
level-6 attribution was dropped and printed as a SHORTFALL (i.e. as a shader
capture defect) instead. The bound is now pushed from the enum, and the perf
line prints L6.

**THE GATE, and state the failing reading for each:**
  * `byLevel` shows mass at MORE THAN ONE level on a normal flight. All at L0
    means the camera-air defect is back.
  * `byReason` does NOT collapse to one bucket in steady state.
  * `uncShell` < `uncovered`. **`100.00% of uncovered` means the shell test
    narrowed nothing** -- suspect `MarchIndexLevelPopulated == 0`.
    `uncShell == 0` across a flight with visible arcs means it narrowed too
    far. `tools/read-holes.sh` flags both.
  * `-VoxelMaxRingLevel=0` must move the mass OUT of L0 and push `never` to
    near-100% above L0. Expect it to pile at the first starved segment the
    ray enters, not to spread evenly across L1-L5.

**IS `uncovered` TRUSTWORTHY NOW? No, and it is not meant to be.** It is left
bit-identical on purpose so every historical leg stays comparable and the
level-1 arm is unchanged. `uncShell` is the number to quote; `uncovered` is
its denominator and nothing else.

**Run:** `tools/voxel-zcutoff-ladder.ps1` (three altitudes, `HoleStats 2`
armed), read with `tools/read-holes.sh Saved/capture-zcut-alt*.log`. Then the
certification leg with `-VoxelMaxRingLevel=0`.

### 0.0h The 10,814 gate leaks are a lake-sheet STARTUP-ORDER bug, not streaming — plus a live capture landmine

REWRITTEN TWICE on 2026-08-23, and both wrong versions are recorded here because
each was a different way of reading a frozen counter as a frozen system.

  * v1 called `gateLeaks=10814` a streaming defect ("the fine tier never follows
    the player"). Wrong.
  * v2 called it the expected cost of an unbaked world, on a tile size that was
    **off by 125x**. Also wrong, and worse, because it was confidently derived.

THE ARITHMETIC ERROR, because it is the reusable lesson. v2 read
`-VoxelSpawnAt=-61440` as Unreal units (-614.4 m) and concluded a tile was
122.88 m across and the baked patch was 370 m x 250 m. **`-VoxelSpawnAt` is in
METRES** (`VoxelEarthGameMode.cpp:61`); the anchor logs -6,144,000 UU = -61,440 m.
A fine tile is 8192 px x 1875 mm/px = **15.36 km** on a side. So the contiguous
baked block x in {-5,-4,-3}, y in {-5,-4} is **46 km x 31 km**, the 4 km ring
cascade sat inside four baked, resident tiles for the whole session, and the
player never flew over unbaked fine ground at all.

WHAT ACTUALLY HAPPENED. `AVoxelWaterSheetActor`'s first gather
(`VoxelWaterSheetActor.cpp:483-510`) ran with `CamUU = (0,0)` before the pawn had
a position:

    Lake sheets: scanning 4 fine tile(s) within 10000 m of (0, 0)

Those are tiles (0,0), (0,-1), (-1,0), (-1,-1) — the four meeting at the WORLD
ORIGIN, ~61 km from the player and genuinely never baked. It read 10,814
elevations out of them. 2.5 s later it re-gathered correctly at
(-6144000, -6144000) and found 476 basins. All 10,814 leaks are in the first 13
seconds: the log carries exactly ONE `+10814` delta line, in the first window,
and the counter never moves again across all 40 windows.

`resident=4` / `loaded=4` / `ringRadius=0` are all correct and healthy: a coarse
tile is 15.36 km and the anchor's entire session excursion was **1.56 km**,
so the player never crossed a tile boundary and there was nothing to prefetch.

WHAT A GATE LEAK ACTUALLY DOES, which nobody had established:
**it returns SEA LEVEL (elevation 0)** — `ReportGateLeak_Locked`
(`VoxelFineTileStreamer.cpp:874-975`) ends `return Sampler_.elevationMm(px,py)`,
which is 0 for a non-resident tile. It does NOT fall back to coarse and does NOT
suppress geometry. Geometry suppression is a separate mechanism entirely
(`VoxelWorldSubsystem.cpp:9981-9982`): a candidate whose footprint is not
resident is simply not admitted. So **gate leaks are never the meshing path** —
they are the ~100 non-meshing consumers that read `FVoxelWorldImpl::Voxels`
directly (collision, agents, water, HUD, `GetSurfaceHeightUU`). A leak means one
of those got a wrong elevation.

WHY THE LOG COULD NOT SHOW THIS: the fine-tier line had no way to distinguish
"streamer stuck" from "player stayed put". Fixed — it now also prints
`ringCentre=(x,y) ringMoves=N`, where the sentinel `(INT32_MIN, INT32_MIN)` means
`TickResidencyAndEviction` has never run (the genuinely-broken third state, which
used to look identical to the healthy one), and gate-leak messages now carry the
anchor tile and Chebyshev distance with the reading rule attached: distance 0-1
is a coverage gap, distance >1 means a caller sampled terrain the player is
nowhere near and no bake would prevent it.

**LIVE LANDMINE, fix this before it costs a night:** `tools/voxel-capture.ps1`
runs `-unattended` with `-VoxelFineTileDir` and does NOT pass
`-VoxelFineTileGateFatal=0`. Under `-unattended` the first gate leak is
`UE_LOG(Fatal)`. If the sheet actor's origin gather beats the pawn placement in a
`-game` run, **the capture dies**. `tools/bv12-river-captures.ps1:43` and
`tools/bv12-shoot-one.ps1:37` already pass that flag — someone hit this and
worked around it without diagnosing it.

TWO REAL FOLLOW-UPS:
  1. Make `AVoxelWaterSheetActor` skip its first gather until the camera has a
     real position. That deletes all 10,814 leaks at the source.
  2. Either add `-VoxelFineTileGateFatal=0` to `voxel-capture.ps1`, or fix (1) so
     the flag is not needed. Prefer (1).

Two things checked and RULED OUT along the way, recorded so nobody re-runs them:

  * March index aliasing — the toroidal grid has a live `AliasCollisions[]`
    counter and a one-shot complaint; the owner's log contains none.
  * A truncated cascade — his command line carried no `-VoxelMaxRingLevel`, so
    all six rings streamed to 4 km.

None of this explains "voxel terrain stops rendering below a certain z level".
`tools/voxel-zcutoff-ladder.ps1` is the instrument for that.

### 0.0f Night sky: no moon at all, and the ground is brighter at night than by day

**Owner-reported 2026-08-23** from a PIE session played through to nightfall. Two defects,
possibly one cause, and they should be investigated together before either is "fixed":

1. **No moon is visible at any point in the night.** Expected behaviour is a moon that moves
   across the night sky on its own arc. The stars and the night sky itself look right — the
   owner said so explicitly — so whatever is wrong is specific to the moon body, not to the
   night sky as a whole.
2. **The ground is lit brighter at night than during the day.** Not merely "too bright" —
   *inverted*. That is a strong signal: a moon/night light whose intensity is being applied
   without its day/night falloff, or a sky-light intensity that scales the wrong way through
   midnight, would produce exactly this.

**Why they are probably related and must be looked at as a pair:** a moon light that is
present and driving ground illumination but whose *body* is not being drawn would explain
both at once — invisible moon, and its light landing at full strength. Confirming or ruling
that out is the first move; if the moon light does not exist at all, then the night ground
brightness is a separate sky-light problem and the two split.

Start in `UVoxelSkySubsystem` (`ue-project/Source/VoxelEarth/VoxelSkySubsystem.cpp`), which
owns the day/night clock (`-VoxelTimeOfDay` / `-VoxelDate` / `-VoxelTimeScale`, and the
frozen-noon defaults every measurement leg pins). Note that legs run with `TimeScale 0` at
12:00, so **no headless measurement in this project has ever rendered a night sky** — this
is a class of defect the leg harness structurally cannot catch, which is worth remembering
beyond this item.

**Reproduce headlessly**: `tools/voxel-capture.ps1` with `-VoxelTimeOfDay=00:00` (and a
sweep across e.g. 20:00 / 00:00 / 04:00) rather than asking the owner to play to nightfall
again. Judged by eye, by the owner, on that capture set.

### 0.0g The sky/fog cvars do not take effect live

**Owner-reported 2026-08-23.** `voxel.Sky.FogDensity`, `voxel.Sky.FogHeightFalloff` and
`voxel.Sky.Fog` were each set in a live PIE session with **no visible change**. They are
almost certainly read once when the fog component is configured and never re-applied on
change.

This is the "switch that is armed and does nothing" shape, and it makes the fog untunable in
the session where you can actually see it. Fog is acceptable as it stands after the clipmap
was restored, so this is tuning ergonomics rather than urgent — but until it is fixed,
**nobody can trust a fog A/B taken by flipping these at runtime.**


### 0.0d Far clipmap renders white, and bright magenta at extreme distance

**Owner-reported 2026-08-23**, immediately after the clipmap was restored (it had
been suppressed by a `-VoxelNoClipmap` left in the launch script — see §0.0e for
that mistake). The heightfield that carries terrain from the ring cascade's edge
out to ~30 km draws with **no colour or terrain texture at all — flat white** —
and at the greatest distances turns **bright purple/pink**.

**Magenta is a strong signal, not a cosmetic one.** In UE it is the standard
"invalid / missing material" colour, so the far band is likely not merely
untextured but failing a lookup outright. White nearer in suggests the material
loads but its colour inputs are unset or unbound, which is a different fault from
the magenta — **treat them as two symptoms and confirm whether they share a
cause before fixing either.**

Start at `AVoxelClipmapActor` (`ue-project/Source/VoxelEarth/VoxelClipmapActor.cpp`):
it loads `/Game/Voxel/M_VoxelClipmap` at `:135-136` and assigns it at `:147-149`,
with a dynamic material instance and a snow band documented at `:71`. Check in
this order:

1. Does `StaticLoadObject` actually return the material at runtime? A null there
   would give UE's default, which is the white.
2. Are the biome/colour parameters being pushed to the MID at all (`:300`
   onwards), and are they pushed for every band or only the near ones?
3. What differs at the far bands specifically — a vertex-colour or texture
   coordinate that runs out of range, or a parameter that is only set within some
   radius. Magenta appearing ONLY at distance argues for a per-band difference
   rather than a global miss.

**Judged by eye, by the owner** — this is appearance, so matched captures at a
fixed pose looking at the horizon, not a metric.

#### DIAGNOSED 2026-08-23 — one cause, two symptoms, and it is not a lookup failure

**The white and the magenta ARE the same defect**, and the thing they share is
one number: the byte this codebase puts in `VertexColor.B` (temperature) and
`VertexColor.A` (precipitation) for every terrain vertex. The material paints
white when the temperature byte is low, and magenta when the temperature byte
AND the precipitation byte are both at the floor. Nothing is failing to load and
nothing is unbound.

**Magenta is not UE's missing-material colour here.** UE substitutes a flat
GREY when a material fails; this magenta is an authored parameter called
`WaterMarkerColor` (0.90, 0.00, 0.90), and it is present in the shipped
`M_VoxelClipmap.uasset` (its name table lists `WaterMarkerColor` alongside
`SnowColor`, `SnowlineLowMeters`, `BiomeLUT` and the rest, so the asset is the
current graph and it loaded fine). The last thing
`Tools/terrain_material_common.py` does to BaseColor is

```
is_marker = (1 - ramp(VertexColor.B, 0.02, 0.06))
          * (1 - ramp(VertexColor.A, 0.02, 0.06))
          *      ramp(VertexColor.R, 0.94, 0.98)
BaseColor = lerp(BaseColor, WaterMarkerColor, saturate(is_marker))
```

which decodes the `(R=1, B=0, A=0)` sentinel `VoxelQuadVertexFactory.ush` writes
for `MAT_WATERMARK` debug voxels. But `R` is 255 on every sky-facing land
surface (`VoxelClimate::BiomeTintForFace`), and `B`/`A` are climate — so any
ground that is cold enough AND dry enough impersonates the instrument. The
comment in that file predicted the collision ("ground at p1 temperature AND p1
precipitation ... would go magenta ... in a debug mode that is already not
shippable") — but only the WRITER is gated on the debug mode. The DECODER runs
in every frame of every run.

The white is the snow lerp — the only white anything in the graph can produce
(every other colour it makes tops out at 0.72 in any channel).

**Why now: the constants were fitted to a different world.** The remap window in
`VoxelClimateProbe.h` (`kTempU8Lo/Hi = 100/189`, `kPrecipU8Lo/Hi = 14/32`) and
the snowline in `terrain_material_common.py` (2700/2900 m) were measured on 25
tiles under provider `3e11cf157a836c70`, cool-temperate maritime, highest point
2897 m. `DefaultGame.ini` today points at provider `80b9ca451a23eae4` — **289
tiles, and a different climate**. Measured over all of it (33,234,574 land
pixels, elevation > 0):

| | old world (what the constants assume) | world configured today |
|---|---|---|
| raw precipitation u8 | p1 14, p99 32 | **p1 0, p50 6, p99 52, max 104** |
| raw temperature u8 | p1 100, p99 189 | p1 89, p50 181, p99 215, max 230 |
| highest land | 2897 m | **6331 m** |

So the precipitation window `[14, 32]` now clamps **72.9% of all land to byte
0**, and a snowline chosen to sit at 93% of the old world's maximum height sits
at 43% of this one's.

**What that does to the picture** (the full graph replayed on those tiles):

| ground | flat white (snow ≥ 0.9) | magenta (marker gate) | neither |
|---|---|---|---|
| all land | 12.4% | 4.5% | 85.9% |
| ≥ 1500 m | 44.2% | 20.9% | 50.8% |
| ≥ 2500 m | 87.7% | 51.2% | 8.7% |
| ≥ 3000 m | **100.0%** | 63.4% | **0.0%** |
| ≥ 4000 m | — | **82.5%** | — |

That is the owner's report line for line: white, turning magenta at the highest
ground. **It is not a per-band or per-distance effect at all** — distance only
selects for altitude, because a horizon vista is made of the tallest ground in
frame. The task's hint that "magenta only at distance argues for a per-band
difference" is disproved: every band runs the identical shader on identical
vertex-colour semantics, and level 3 simply contains the mountains.

Two more things fall out of the same measurement. The precipitation axis of
`T_VoxelBiomeLUT` carries information on only **16.7%** of this world's land
(72.9% pinned to the dry end, 10.4% to the wet end), and the temperature axis is
pinned at the hot end on a further 44.9% — so most of the non-snow vista is being
sampled from one corner of the LUT and has almost no biome variation left to
show. That is the "no terrain texture at all" half of the complaint, and it is
NOT fixed by anything below.

#### Fixed in code (no asset regeneration)

* **The magenta.** `VoxelClimate::SampleClimateAtWorldUU` now floors both emitted
  climate bytes at `kSentinelSafeFloorU8 = 20` (0.078, a 1.3× margin over the
  material's 0.06 gate), so terrain can no longer write the reserved sentinel.
  Costs the bottom 7.8% of both LUT axes — roughly a 6% shift along the LUT's
  dry→wet mix — and **does not touch snow at all** (that gate is at 0.16). The
  `MAT_WATERMARK` instrument is unaffected: the vertex factory writes a literal
  `0.0`, not a climate byte. `-VoxelClimateSentinelGuard=0` restores the old
  behaviour on the same binary for the A/B. Fixes the quad path too.
* **A knob for the white**, off by default. All four snow constants are
  ScalarParameters on the shipped asset, so `AVoxelClipmapActor` can move them
  through a per-level MID with no asset change: `-VoxelClipmapSnowlineLowM=`,
  `-VoxelClipmapSnowlineHighM=`, `-VoxelClipmapSnowTempMax=`,
  `-VoxelClipmapSnowTempFeather=`. **Pass none and the frame is byte-identical to
  before**, because no MID is created. Where the snowline belongs on a 6331 m
  world is a judgement by eye, not arithmetic — and 2700 m was chosen to match
  voxel-core's `MAT_SNOW` threshold, so moving one without the other splits the
  vista from the near field at the snowline.
* **`-VoxelClipmapColorCensus`** logs, per level rebuild, what the vertex colours
  just written will do in the material: % inside the marker gate, % at full snow
  by each route, and how much of each LUT axis is pinned. Turns "the vista is
  white" into numbers from the real run.
* `ApplyLevelMaterial` is now the single place a level's material is set. The old
  `UpdateDebugTint` **discarded** the MID whenever the ring debug layer was
  switched off, which was harmless with only `DebugTint` on it and would have
  silently reverted the snowline the moment a second parameter existed.
* The material-load check now logs on SUCCESS as well as failure. Ruling
  `StaticLoadObject` in was the first question asked of this defect and the log
  could not answer it.

#### Still open — needs an asset regeneration, so deliberately NOT done

Regenerating any of these must go through **`tools/voxel-sky-chain-regen.ps1`**,
which discovers every dependent of `MPC_VoxelSky` (`create_clipmap_material.py`
is in its ORDER list), rebuilds in order, and refuses the run if a pinned-pose
capture moved.

1. **Gate the debug-marker term properly.** The floor above stops terrain
   impersonating the sentinel, but the right fix is that the decode should not
   exist in a shipping material at all, or should key off something terrain can
   never produce. Touches `terrain_material_common.py`, so both
   `M_VoxelClipmap` and `M_VoxelTerrain`.
2. **Re-measure the climate remap window for THIS world** and regenerate
   `T_VoxelBiomeLUT` with matching axes in the same commit. The window and the
   LUT axes are two copies of the same numbers; `VoxelClimateProbe.h`'s
   `static_assert`s exist to stop one moving without the other. This is what
   would give the vista its biome colour back on the 83.3% of land whose
   precipitation is currently pinned to an axis end.
3. **Retune the snowline against voxel-core's `MAT_SNOW`**, once the owner has
   picked a number by eye with the runtime switches above. Both must move
   together or the near field and the vista disagree at the snowline.

#### The capture that shows it

The pose matters more than usual here, because the defect lives on ground above
2500 m: a vista with no mountains in it looks identical in both arms. This site
was picked off the configured tiles rather than guessed (the block-mean
elevation mosaic over all 289 tiles, then the lowest land in a ring around the
biggest massif). **Camera on 398 m ground at world (-9524, -40563) m; the massif
centred at (-31440, -42480) m, 22 km away, mean 5333 m over a 4 km box with a
6061 m peak; bearing 185°.** From 2500 m up, that summit sits ~8° above the
horizon, so a level camera frames the far band and the peaks together.

```powershell
# AFTER (shipped default: the sentinel guard is on)
tools\voxel-capture.ps1 -Name clipmap-0d-after `
    -SpawnAt -9524,-40563 -SpawnAltM 2500 -SpawnPitch 0 -SpawnYaw 185 `
    -SettleSec 180 -ExtraArgs "-VoxelClipmapColorCensus"

# BEFORE, same binary, same pose -- the magenta comes back
tools\voxel-capture.ps1 -Name clipmap-0d-before `
    -SpawnAt -9524,-40563 -SpawnAltM 2500 -SpawnPitch 0 -SpawnYaw 185 `
    -SettleSec 180 -ExtraArgs "-VoxelClimateSentinelGuard=0","-VoxelClipmapColorCensus"
```

The census lines land in the same log, so the picture arrives with its numbers.

To try a snowline by eye, add e.g.
`-ExtraArgs "-VoxelClipmapSnowlineLowM=3500","-VoxelClipmapSnowlineHighM=3800"`
to a third capture. That is a knob to turn, not a recommendation — and one
candidate is already **disproved**: scaling the old constants by the height
ratio (6331/2897) gives ~5900/6300 m, and only **0.005%** of this world's land
is above 5900 m, so that is not a higher snowline, it is no snow at all. The two
worlds differ in SHAPE, not just in maximum: this one's p99.9 is 4898 m. The
hypsometry, so the next person picks rather than derives — fraction of land
above each height: 2700 m 3.18%, 3000 m 1.79%, 3500 m 0.59%, 4000 m 0.35%,
4500 m 0.20%, 5000 m 0.08%.

### 0.0e A diagnostic flag shipped in the launch script for a session

`tools/voxel-launch-prototype.ps1` passed **`-VoxelNoClipmap`**, copied out of the
handoff's editor command line without noticing what it does: it SUPPRESSES the
far-terrain clipmap, and it exists purely as a control for "is that distant
terrain actually voxels?" The owner's first hands-on session therefore showed
terrain ending a few km out against a flat blue plane, and reported it as a bug
in the world. Fixed; it is now `-NoClipmap`, opt-in, with the reason at the call
site.

**The lesson worth keeping:** a command line copied from a measurement recipe is
not a command line for playing the game. Diagnostic switches in this project are
deliberately powerful (they suppress whole subsystems to make a control), and
several read as ordinary settings by name alone.


### 0.0b Coarse LOD browns out — the material mip discards thin surface layers

**Owner-reported 2026-08-23, and present since the ray marcher landed.** The near
ring renders "full" — every voxel a solid colour. Further rings are progressively
less white and more brown, in visible bands by LOD.

**Mechanism, verified in code, and it is two effects compounding:**

`downsampleBricks` (`voxel-core/include/voxelcore/mips.h:42-88`) takes a
**majority vote of the 8 child voxels**, ascending scan with strict `>`, so
**ties break to the LOWEST material id**. And the ids are `MAT_AIR = 0`,
**`MAT_ROCK = 2`**, `MAT_SNOW = 7`, `MAT_GRASS = 8`.

Snow and grass are thin surface caps; rock is the thick body underneath. In any
2x2x2 group straddling the surface, rock usually wins the vote outright — and on
a 4-4 tie it wins anyway because 2 < 7. Each level compounds the last. Distance
is LOD, so distance is brown.

A third, related effect worth checking at the same time: `solidThreshold = 4`
means a parent cell stays AIR unless at least 4 of its 8 children are solid,
which erodes a sloped surface by up to half a voxel per level.

**The fix is surface-preserving downsampling** — for the topmost solid cell of a
column, carry the child's surface material instead of the majority. Standard
technique; the work is in doing it without breaking two things:

1. **The GPU must agree.** `voxel.GPU.VerifyCoarse` proves the GPU coarse path
   bit-exact against the CPU one on columns, cells AND quads. An earlier audit
   reports the GPU coarse worldgen POINT-SAMPLES one representative column
   (`worldgen.ush:1804-1863`) rather than voting — **that reading is not
   personally verified and should be the first thing checked**, because if the
   two arms already disagree, the parity gate is not covering this and that is a
   finding in itself.
2. **Judge it by eye, not by a metric.** This is a colour/appearance defect; the
   owner judges screenshots. Matched captures at a fixed pose across LOD bands.

### 0.0c RESOLVED 2026-08-28 — the triangular LOD-ring gaps are gone; fallthrough default on

**Owner's verdict, 2026-08-28: solved by turning fallthrough on by default.**
Marcher fallthrough has been default-on since 2026-08-23
(`VoxelWorldSubsystem.cpp:4098`, "Fallthrough is useless without it"), and the
owner confirms the wedges are no longer present.

Kept below, struck through in spirit rather than deleted, because the three
REFUTED streaming hypotheses are still worth not re-running: skirt mask, coarse-
ring starvation, and hierarchical coverage were each tested with matched legs and
each came back null. The cause was in the MARCHER, not in streaming -- which is
why every streaming explanation failed.

<details><summary>Original entry (cause was elsewhere; hypotheses below are refuted)</summary>

### 0.0c Small black TRIANGULAR gaps at LOD ring boundaries — still open

**Owner-reported 2026-08-23**, after this session's ring fixes: "your recent
fixes have closed most of the ring gaps but there are still these few triangular
black gaps at LOD ring boundaries", concentric around the player.

**Why the shape matters, and why the streaming hypotheses may be the wrong tree.**
Three streaming explanations were tested with matched legs this session and ALL
were refuted (skirt mask, coarse-ring starvation, hierarchical coverage — see
`docs/gpu-streaming-architecture.md` §7). A *triangular wedge* at a boundary is
the shape a TRAVERSAL handoff error makes, not the shape a missing chunk makes:
a missing chunk is chunk-shaped (square, and the 2026-08-22 fix records exactly
that — "holes were exactly one chunk square"). A wedge is what you get when a ray
crosses between two differently-scaled grids and the t-interval handoff is
slightly wrong.

**So this may not be a residency problem at all**, which would explain why every
residency fix failed to move it. Note also that `uncovered` may NOT be counting
these: it requires the ray to have crossed a chunk with `bResident == false`, and
a wedge missed between two RESIDENT grids would slip past that test entirely.
**That is checkable and should be checked first** — if the owner can see gaps
while `uncovered` is near zero, they are two different defects and the 4%
flying-`uncovered` is a separate, real problem.

Start at `VoxelBrickTraverse.ush`'s ring walk and the segment handoff
(`t = R / |Dir.xy|`, the 2026-08-22 cylinder-vs-sphere fix), not at admission.


### 0.0a Marched sun shadows — TURNED OFF 2026-08-23, owner decision, revisit later

`voxel.Shadow.March` now defaults to **0**. Terrain has no sun shadows until this
comes back. Set it to 2 to restore them.

**Why.** It is the largest single frame-time item in the renderer. Matched 30 m/s
line legs changing ONLY this cvar:

| | frames | p50 | brickPacks |
|---|---|---|---|
| mode 2 (shadows) | 10,832 | 20.90 ms | 913,197 |
| mode 0 (off) | **20,811** | **7.76 ms** | 917,797 |

The frame rate **doubles**; it costs ~13 ms of a 20.7 ms frame on this box.

**Streaming was unaffected** (+0.5%), and that same leg is what killed the theory
that terrain generation and the marcher contend for the GPU — freeing half the
frame's GPU time moved streaming throughput by nothing.

**Revisit when** the GPU streaming programme (`docs/gpu-streaming-architecture.md`)
and the rest of the rendering pipeline are good. The reach dial
(`voxel.Shadow.MarchRayReachM`, 48/96/256/512) was never chosen — 512 is the
current cost and no one has measured whether 96 looks acceptable at a quarter of
it. That measurement is the obvious first move when this is picked back up, and it
was never taken.


Everything below this section predates the streaming programme — except §§8-10 —
and §6b in particular is stale and now says so.

> **Re-measure before quoting anything here. Reviewed 2026-08-19.** Two things
> changed underneath these numbers. The quad pool default went 80M → 192M
> (`036552f`), so "70% of 80M" no longer describes any run. And the world now
> composes environment assets into terrain chunks and draws ground cover as HISM
> instances out to 256 m (§10) — render-thread and GPU work that no leg below
> includes. The *conclusions* (render-thread bound, tail is GPU, game-thread work
> buys headroom not fps) are structural and are expected to hold; the
> milliseconds are not current.

### 0.0 NEW P0 (2026-08-22) — MAJOR REGRESSION: marcher streaming is slower than the quad mesher it replaced

**Owner-reported, in the editor, and it is the headline problem with the marcher
programme right now.** Three symptoms, believed to be one cause:

1. **Streaming is slower than the pre-marcher quad system.** Not "slower than
   hoped" — slower than what shipped before any of this work.
2. **Chunks are missing at LOD boundaries on initial load**, so the ring seams
   are visible as gaps rather than as a detail change.
3. **Flying at normal speed outpaces the streamer**, leaving holes that fill in
   behind the camera.

**Why this matters more than the draw-path win.** The marcher's headline is
**2.72x p50 / 2.41x p95** on the *draw path*. That number is real and was
measured on matched legs. But it buys frame time the owner cannot see while the
world he is flying through has holes in it. **A renderer that draws an
incomplete world quickly is not faster.** Every marcher-vs-quad comparison in
`docs/measurements/armA-drawpath-ceiling-2026-08-19.txt` measures frames, not
world completeness, so none of them would have caught this — and the standing
warning in that file about asking "what was the quad arm doing that the marcher
arm was not" applies to the *producer* side too, not just shadows and GI.

**What is measured so far** (`Saved/Logs/VoxelEarth.log`, 2026-08-22 16:30-16:32,
two PIE sessions back to back in one process):

| | session 1 | session 2 |
|---|---|---|
| index entries after 5 s | 9,675 | 1,531 |
| after 25 s | 65,522 | 26,106 |
| sustained fill rate | ~2,000-2,500 chunks/s | decelerates 1,979 → 880 → 300/s |
| streaming `tickMs` | 0.269 then collapses to **0.007** | **0.267-0.281 sustained** |

So there are **two distinct problems layered on top of each other**:

- **(a) A general streaming-throughput regression** vs the quad mesher. Not yet
  quantified against a pre-marcher build — that comparison does not exist and is
  the first thing to get.
- **(b) A second-PIE-session throughput collapse.** Session 2 fills at roughly a
  seventh of session 1's rate and its streaming tick never drops. Something is
  not being reset per world besides the pool. **This is separate from the
  teardown bug fixed on 2026-08-22** (see below) — that fix is verified working
  and did not address this.

**Already fixed, do not re-diagnose:** the brick pool and march chunk index were
never torn down with the UWorld, so a second PIE session inherited a saturated
index (`indexEntries=131047` of 131,072) full of orphans nobody owned, evicted
~200/5 s, and rendered pure sky. Three links: no teardown call,
`FVoxelBrickPool::Reset` and `FVoxelMarchChunkIndex::Detach` both existing with
**zero callers**, and `Cells.SetNumZeroed` being a no-op on re-attach because
`SetNumZeroed` only zeroes elements it adds. Fixed and gated —
`Voxel GPU teardown: released 71060 pool chunks and 71060 index entries (pool now
0, index now 0)`. **Session 2 now streams. It just streams badly**, which is (b).

**An open design question the fix raises.** The teardown now DISCARDS 71,060
chunks that were still valid geometry for the same world at the same location.
Re-adopting them into the new world's records instead of dropping them would make
a second Play near-instant rather than a full cold re-stream. That is an
optimisation, not a correctness fix, and it should not be attempted until (a) and
(b) are understood — but it is the reason session 2 starts from zero.

**Suggested first move, and it is a comparison not a hunt:** build the
pre-marcher quad configuration (`voxel.March 0`, `voxel.Terrain.RetireQuads 0`)
and run the same flight, reading chunks/s and the LOD-boundary behaviour. That
says whether (a) is the marcher's producer path or a change underneath both. The
producer switches are command line / ini only — `-ExecCmds` lands after streaming
has begun and will silently not apply (`VoxelBrickPool.cpp:286-304`).

**Do not read frame time as progress on this item.** The thing being fixed is
chunks per second and holes at boundaries.

#### Measured 2026-08-22, second pass — TWO limiters, and only one of them is the marcher's

Read from the same log. **The streaming tick reaches 95% of wall and 170 ms per tick**, which
is why flying outpaces it. But the dominant stage CHANGES as the world fills, and the two
halves have different owners:

**Cold fill — `recompute` dominates, and it is NOT marcher work.**

| window | tick % of wall | recompute |
|---|---|---|
| first 5 s | 90.0% | 2,815.7 ms |
| next | 65.1% | 2,128.0 ms |
| next | 51.8% | 1,193.5 ms |
| next | 25.3% | **85.2 ms** |

Essentially all of it is the level-0 entry loop: `recomputeMs=70.16` of which
`entryMs R0=69.96`, over `footprints R0=5088` — about **14 µs per footprint**, per tick.

The cause is `FootprintChunkZRangeCached` (`VoxelWorldSubsystem.cpp:8654`) **refusing to
memoize while the fine tier has not finished decoding the tile**. Its own comment explains
why that is correct: an entry computed while a 300 MB tile is still decoding is derived from
the sea-level fallback and, with no invalidation, would be wrong for the rest of the session.
So it recomputes every footprint every tick until residency flips.

**That behaviour landed in `1e5207b` (2026-08-17), "Per-layer mesher + admission counters;
z-range memo waits for residency" — a fine-tier correctness fix, not marcher code. It costs
the quad path exactly the same.** The decay to 85 ms above shows the memo does re-enable once
tiles land, so this is a cold-fill and fly-into-new-territory cost, not a permanent one.
**It is the leading explanation for "chunks missing at LOD boundaries on initial load".**

**Steady state — `dispatch` dominates, and this one IS the marcher's.**

| window | tick % of wall | dispatch | recompute | packs/s |
|---|---|---|---|---|
| 16:30:51 | 73.6% | 2,682.5 ms | 169.4 | 2,388 |
| 16:30:56 | 86.9% | 3,631.9 ms | 128.4 | 1,866 |
| 16:31:01 | **95.4%** | **4,365.0 ms** | 107.8 | **1,383** |

Throughput FALLS as dispatch grows. The stage brackets inside dispatch put nearly all of the
unattributed remainder in one place — `other` ≈ `gpuMgrTickMs` in every sample
(`other=4.19/4.73/4.95` against `gpuMgrTick=4.10/4.66/4.90`) — which is
**`FVoxelGpuMeshJobManager::Tick`, the brick-packing manager that only runs because
`voxel.GPU.BrickPack 1`**. `airProof`, `band`, `submit`, `pick` and `overlay` are all
sub-millisecond and are not the problem.

**So the A/B in the item above is still the right first move, but the prediction is now
specific rather than open:** turning the marcher off should remove most of the *dispatch* cost
and leave most of the *recompute* cost in place. If both drop, the z-range memo attribution is
wrong. If neither drops, both attributions are wrong and the cause is underneath.

**Not yet checked, and cheap:** whether `gpuMgrTick` is doing work proportional to the job
queue or re-scanning something per tick. That is one read of `VoxelGpuMeshJobManager::Tick`
and it is where this should resume.

### Where the engine is (2026-07-28)

Standard flight leg, shipped defaults, **2560x1440**:

| | value |
|---|---|
| p50 frame | 15.27 ms (**65.5 fps**) |
| p95 frame | 21.02 ms (47.6 fps) |
| max frame | 42.8 ms |
| hitch frames | 11–25 of ~16,000 |
| flight-phase holes (median) | ~57 |
| holes(final) / allocFail | 0 / 0 |
| chunks/s | ~1,074 |
| pool | 70% of 80M quads |

**Streaming is solved and is no longer the constraint.** 260.9 → ~1,074 chunks/s
across Waves S1–S4, zero permanent holes, and terrain now arrives ahead of the
camera (T4-1). Nothing in this section is a streaming item.

**The constraint is frame time, specifically its tail.** The median clears
60 fps; the 95th percentile does not, and there is ~1.5 ms of headroom against
the 16.7 ms budget before any gameplay system is added.

Full detail: `docs/measurements/session-summary-2026-07-28.txt`.

---

### 0.1 P0 — ANSWERED 2026-07-28: the frame is render-thread bound, the tail is GPU

Measured with `voxel.Stream.FrameAttribution 1` over two legs (~16,600 frames
each). Full numbers: `docs/measurements/frame-attribution-2026-07-28.txt`.

**A typical frame is the render thread.** render 13.49 ms against a 13.41 ms
frame, while the GAME thread spends 10.07 ms of it **waiting**. The voxel tick
contributes 0.47 ms. That retrospectively explains why cutting game-thread
publication from 2.94 ms to 0.055 ms moved nothing — the game thread had 10 ms
of slack to absorb it.

⇒ **Game-thread work buys headroom, never frame rate.** Anything aimed at fps
must reduce render-thread work.

**The tail is GPU.** The largest slow-frame delta is `renderWait` (+12.3 / +10.1
ms against a frame delta of +16.5 / +13.3) — the render thread blocked.

*The CPU voxel tick rises on slow frames (+5.1 ms) and was called "a passenger"
here. **The GPU capture partly reversed that:** the CPU tick is indeed a
passenger, but the GPU side of streaming — `WorldTick`, the meshing compute
passes — is **3.6–5.1 ms, 21–28% of the GPU frame**, and it IS on the critical
path. A frame with more terrain arriving has a longer GPU frame and therefore
more `renderWait`. See `docs/measurements/gpu-capture-2026-07-28.txt`.*

**Shadow cascades are not involved.** `shadowGather=0` throughout and ~1.03
gathers per frame — the "pool re-gathered 4–5× for shadows" hypothesis is dead.

**The cull walks the whole pool every frame:** 62,657 runs frustum-tested per
gather, of which only ~10,300 survive — and it scales with *resident* chunks, not
visible ones. *(An estimate of ~4.4 ms once stood here, derived from the 45–90 ns
per test recorded at Wave G. Measured, it is ~1.0 ms — see 0.1a. Do not use the
Wave G figure.)*

### 0.1a NEW P0 — Cheaper per-range binding (the emit, not the walk)

**Measured with `voxel.Stream.GPUCullTiming`.** The render thread's 13.72 ms:

| | per gather (~1/frame) |
|---|---|
| cull **walk** — 62,657 box tests | **~1.0 ms** |
| range **emit** — ~6,215 ranges | **~3.2 ms** |
| **not the voxel pool at all** | **~9.5 ms** |

**The ~4.4 ms walk estimate was wrong by 4×** — it multiplied 62,657 by the
45–90 ns/test recorded at Wave G; measured, it is ~17 ns. That hoist works far
better than its own comment claims.

So **the emit is the pool's real cost**, and its shape is one
`CreateUniformBufferImmediate` per range — exactly what the `kMaxRanges` comment
warned about.

**THE OBVIOUS FIX IS ALREADY FALSIFIED — read this before starting.** The
appealing version is to drop the per-range uniform buffer entirely by putting the
range's start in `FMeshBatchElement::BaseVertexIndex` and letting the shader
derive the quad from `SV_VertexID` alone. `VoxelQuadVertexFactory.ush` records
that this was tried and MEASURED:

> *SV_VertexID does not include the draw's base vertex on D3D12
> (`RHISupportsAbsoluteVertexID` is Vulkan-only), so a draw that starts at pool
> quad F still sees VertexId running from 0 … tiling the pool into N exact
> contiguous ranges drew only the first 1/N of it at N = 2, 8 and 64 while still
> paying for every quad.*

So every draw genuinely must be told its base explicitly on this RHI. The
remaining routes, neither cheap:

  - **per-instance vertex stream.** One entry per range holding `BaseQuad`, with
    each draw's `StartInstanceLocation` selecting its entry. Instance *fetch*
    honours the start location even though `SV_InstanceID` (like `SV_VertexID`)
    does not, which is the standard workaround. Needs vertex-factory stream work.
  - **cached multi-frame uniform buffers.** Keep a persistent ring sized to
    `kMaxRanges` and `RHIUpdateUniformBuffer` instead of creating ~6,215 per
    frame. Cheaper per range, but the single-frame lifetime exists precisely so a
    buffer is not rewritten while an in-flight draw still references it — the ring
    depth is the correctness argument and must be reasoned about, not guessed.

Either way this is vertex-factory work, not a call-site change, and it should be
planned as such.

*Interaction to remember:* the draw-path retune took ranges 1,023 → ~6,215 to
kill over-draw. Large net win (p50 30.59 → 15.15 ms), but it bought ~2.7 ms of
emit. If the per-range cost falls, the gap/ranges optimum moves and that sweep
should be re-run.

**Hierarchical cull is demoted** — it attacks the ~1 ms walk, not 4 ms. Real, no
visual cost, but rank it accordingly.

### 0.1c STRUCK — the depth prepass is not removable

Tested 2026-07-28, four arms: `docs/measurements/prepass-test-2026-07-28.txt`.
It was flagged as the largest and cheapest item on the 100 fps path. It is
neither.

`r.EarlyZPass 0` is ignored (still "Forced by Nanite"). `r.Nanite 0` does not
free it either — the forcing **hands over to DBuffer**. With both off it is still
5.708 ms. Two independent subsystems require a full depth prepass.

The fallback hypothesis — that the prepass was not earning its cost — dies too:
BasePass is 5.907–6.053 ms across all four arms, a 2.5% spread, so base-pass cost
is independent of it. GPU frame 17.25 → 17.08 ms. Nothing on offer.

⇒ **Realistic floor from the remaining items is ~12–13 ms (~80 fps), not 10.**
Reaching 10 ms needs the primitive count itself down — 17.8M per pass, drawn
twice — which points back at rendering distant rings as heightfield rather than
voxel geometry.

### 0.1b ANSWERED IN PART, 2026-08-28 — it is the FLOOR, not the tail; still unsplit by subsystem

**The method this entry asked for has been run.** Its own three steps were: a
per-frame time series rather than percentiles; establish whether slow frames are
game-, render- or GPU-bound ("`renderMs` cannot answer this today: it is a HITCH
field and only exists above the threshold"); then pick a fix. Steps 1 and 2 are
done -- `voxel.Stream.FrameAttribution 2` samples every frame, and the first
per-frame GPU clock this project has ever had went in on 2026-08-27.

**The answer to "is it the tail": NO. It is the floor.** Pooled over 1.63M fast
and 32.9k tail frames on the shipping default, 2560x1440:

    bucket   frame   render  renderWait    rhi  gameWait  gameMs   gpu
    PARKED    7.99     8.12       0.02    1.97      5.96    2.19   6.79
    FAST      7.50     7.61       0.23    1.79      4.96    2.60   6.41
    TAIL     19.70    11.30       3.14    6.11      4.82   16.71   8.39

**Render-thread BUSY barely moves: 7.38 ms fast to 8.16 ms tail, +0.78.** What
rises is `renderWait` (+2.91) and `rhi` (+4.32) -- the render thread waits more,
it does not work more. At the tail `gameMs` 16.71 exceeds `render` 11.30 and
`gpu` 8.39, so **the game thread is the critical path there** and this ~8 ms
render block is not what makes a bad frame bad.

**It IS the floor, and the floor is what blocks >100 fps.** Parked reads
frame 7.99 / render 8.12 / gpu 6.79 with a near-idle game thread -- a ~125 fps
ceiling with zero terrain streaming. Goal 3a (<10 ms p95) cannot be reached by
tail work alone once the tail is fixed; this block sets the bar.

**WHAT IS STILL OPEN, and it is the original question narrowed:** the block has
NOT been split by subsystem. The 2026-08-27 GPU split decomposed the RISE to p95
(streaming 73%, marcher 20%, draw path negative) but was framed against SLOW and
says nothing about the 6.79 ms parked GPU floor or the 8.12 ms parked render
thread. **The next step is one leg: the same `-csvGpuStats` + per-pass
`GPU/<name>` decomposition run on a PARKED leg**, which would name the floor the
same way the tail was named. `tools/csv-gpu-attrib.py` already reads it.

<details><summary>Original entry</summary>

### 0.1b NEW P0 — What is the other ~9.5 ms?

The largest single unexplained block in the frame, and **bigger than everything
the streaming programme has optimised put together.** The voxel pool is only
~4.2 ms of the 13.7 ms render thread; the rest is `AVoxelClipmapActor` (the 30 km
heightfield), the water subsystem and its pool, sky, and the base/post chain.

Already ruled out: anything pixel-proportional (resolution is free). Break it
down with UE's render-thread stats or a ProfileGPU capture **before** sizing any
work against it.

<details><summary>Superseded framing (kept — this is the hypothesis that was falsified)</summary>

**Open, and the obvious hypothesis is already falsified.**

p50 15.27 ms against p95 21.02 ms is a ~5.7 ms spread. The streaming tick was
blamed because `perTick` (5.995 ms, on ~32% of frames) matched that gap almost
exactly. Incremental pool runs then removed a third of the tick — `buildRuns`
2.94 → 0.055 ms per publication, tick 11.73% → 7.75% of wall — and **p50/p95 did
not move at all**. The match was a coincidence.

The cause is unknown. The next attempt must find it rather than assume:

1. Capture a per-frame time series, not percentiles, and correlate slow frames
   against what else happened on them (publication? unload burst? GPU harvest?
   shadow cascade?). The `Hitch frame:` line already carries a
   subsystem/elsewhere split — extend that to ALL frames, not just >33.3 ms ones.
2. Establish whether slow frames are game-thread, render-thread or GPU bound.
   `renderMs` cannot answer this today: it is a HITCH field (lesson 17) and only
   exists above the threshold.
3. Only then pick a fix.

**Worth:** the difference between "60 fps median" and "60 fps floor", which is
the difference between the stated goal being met and not.

</details>

---

### 0.2 RETIRED 2026-08-28 — MOOT under the marcher. Do not build this.

**This was scoped entirely against the QUAD path and that path no longer draws
the terrain.** The entry's own numbers say so: "17.8M primitives submitted per
pass, TWICE per frame", "the whole pool is one primitive with one draw call".
Under `voxel.March 1` the terrain is not primitives at all -- it is a ray march,
and `quads=0` on every leg. **A ray terminates at the first hit, so occluded
geometry costs exactly nothing. Occlusion culling is inherent to the renderer
that replaced the one this was designed for.**

**And the measurement agrees, which is the part that retires it rather than just
arguing it away.** The 2026-08-27 GPU split (`docs/gpu-tail-split-2026-08-27.md`)
decomposed every term of the GPU's rise from a typical frame to p95 across two
legs: streaming 73%, marcher 20%, and **the entire draw path -0.11 / -0.10 ms --
NEGATIVE.** There is no draw-path cost to reclaim on the frames that are slow.

**What replaced it.** The marcher's analogue of occlusion culling is EMPTY-SPACE
SKIPPING, and that programme has been run and is largely exhausted: `anySolid`
measured -0.13 ms parked but NEVER ARMED — `voxel.March.IndexAnySolid` still
defaults 0 (`VoxelMarchChunkIndex.cpp:289`); the code is committed, the default
was never flipped (corrected 2026-08-28; this line previously said "shipped") —
the height pyramid was built and RETIRED (479 missed rays),
ZCut was refuted for the horizon (0.00% of 3.3e9 decisions at pitch -10). See
[[voxelsim-marcher-cost-is-ray-count]] -- marcher cost is ray-count linear within
2% and per-ray cost is immovable, so the lever is ray count and geometry
visibility is not one of its terms.

**The one part that is NOT moot:** `AVoxelClipmapActor` (the 30 km heightfield)
and water are still real geometry. But the draw path measures negative, so
culling them would be optimising a term that does not rise. If that ever changes,
re-open against a measurement rather than against this entry.

<details><summary>Original scoping (kept for the horizon-culling analysis, which was sound for the quad path)</summary>

**Designed, not started. The largest remaining frame-time lever that costs no
visual quality.**

The cull is frustum-only. It already rejects 68% of chunks (41,946 of 62,119),
but most survivors are behind something: the GPU capture shows 17.8M primitives
submitted per pass, TWICE per frame, into an internal render target of 1552x873
(TSR upscales to 2560x1440) — roughly 11 triangles per rendered pixel.
Terrain occludes itself heavily — a ground-level camera sees a few hundred metres
of surface and nothing past the first ridge.

**UE's built-in occlusion cannot help.** Hardware occlusion queries are
per-PRIMITIVE, and ADR-0006 deliberately makes the whole pool one primitive with
one draw call. The renderer sees a single world-sized object and can only answer
"is the pool visible", which it always is.

Two approaches:

**(a) Horizon culling — cheaper, terrain-specific, and NOT SOUND FROM CHUNK
BOUNDS.** The appealing version is: build a per-bearing horizon-angle array from
the near chunks' `RunBounds` (which the cull already has on the render thread),
then reject any chunk whose angular extent falls entirely below it. Two passes
over ~10k in-frustum runs, no GPU readback, no frame of latency.

**It is wrong, and this was worked through on 2026-07-28 rather than discovered
in a screenshot.** A chunk's bounding box is not solid. Voxel terrain has caves,
arches, overhangs and thin spires, so a *near* chunk whose box reaches high does
not occlude anything — you can see straight past a spire, and straight through a
cave mouth. Occlusion derived from bounds is therefore not conservative: it hides
terrain the player can see, and a pooled primitive fails silently (ground rule 4),
so the symptom is missing landscape with nothing in any log.

To make it sound the horizon must come from something that is actually opaque —
the clipmap **heightfield** (`FootprintBandCache` / the surface-height columns),
not chunk bounds — and even then caves below the surface line break it. That
restricts it to "reject chunks entirely below the surface horizon", which is
close to what the buried skip already does at admission.

⇒ **Prefer (b).** Approach (a) is recorded here so it is not re-proposed as the
cheap option; its cheapness comes from using data that cannot answer the
question.

**(b) HZB occlusion — general, the real answer.** A compute pass tests each
chunk's bounds against the previous frame's hierarchical depth buffer and writes
a per-chunk visibility bit that `BuildCulledRanges` reads alongside the frustum
result. How UE's own GPU scene culling works; handles every camera pose. Costs
one frame of latency (standard, invisible at these rates) plus reprojection care.

Prerequisites and hazards:

- `ComputeRunBounds`/`RunBounds` already provide per-chunk world bounds on the
  render thread, so the input either approach needs exists.
- `RunBounds` is read by `GetDynamicMeshElements`, which runs **concurrently**
  across the camera view and every shadow cascade. A per-frame visibility buffer
  must not be written while a gather reads it — see the concurrency argument at
  `RebuildRunBounds`.
- **Shadow cascades must not use camera occlusion.** A chunk invisible to the
  camera can still cast a visible shadow. Gate on `bShadowGather`.
- Ground rule 4: a pooled primitive fails silently. Verify with a screenshot diff
  and a converged `CoverageVerify`, and add a debug mode that draws what
  occlusion rejected — the sibling of `GPUCullDebugAllVisible`.

**Worth:** unmeasured, but with the prepass struck (0.1c) this is now the
LARGEST remaining item — and uniquely, it cuts BOTH passes, since PrePass and
BasePass each submit the same 17.8M primitives. If half the in-frustum geometry
is occluded, both passes shrink together.

---

### 0.3 P2 — Deferred by owner decision (2026-07-28)

**S2-1 GPU hide pass — built, gated OFF, UNVERIFIED.**
`voxel.Stream.PoolGpuHide 0`; the pass, the pending-hide plumbing and
`UnmarkGpuHide` all ship and are inert at the default. Its forced probe went
through five rounds of harness bugs and still gives no trustworthy verdict. One
intermediate result was reported as "the probe caught a real bug" and is
**retracted** — the control, against the shipped CPU-shadow path, fails
identically. **Before trusting any verdict from that probe, verify it against a
known answer:** allocate one chunk, publish, read it back, assert the ids equal
that chunk's id. Full history:
`docs/measurements/s21-gpu-hide-probe-2026-07-28.txt`.

~~**S2-5 drop the CPU shadow — blocked on S2-1.**~~ **DONE 2026-08-18**
(`036552f`). The shadow was not dropped, it was **paged**: capacity now costs
VRAM only, the whole-array copy in `CreateSceneProxy` is gone, and the default
pool went 80M → 192M quads (`kPoolCapacityQuads`,
`VoxelWorldSubsystem.cpp:11650`) — which is what the asset-composed alpine vista
needed. S2-1 stayed gated off and did not block it. **Not yet seen in a rendered
frame at the new default** — that verification is §10b.

---

### 0.4 P3 — Geometry levers that cost visual quality

Take these only if 0.2 is exhausted and frame time is still short.

| item | saving | cost |
|---|---|---|
| **Per-chunk greedy meshing** — merging is per-brick (8³) not per-chunk (32³), so a flat 32×32 face becomes 16 quads instead of 1 | up to 16× on flat terrain in theory; far less in practice, since the merge key includes 4-corner AO | GPU mesher must match the CPU reference **bit-exactly** (`mesher.h`), gated by `VerifyAsyncMesh` and worldgen digest `6e893ab3679a8c81`. Both implementations change in lockstep, digest re-baselined. |
| **Cascade 6 rings → 5** (4 km → 2 km) | ~1/6 of resident chunks, ~7.8M quads | Draw distance. Not one line: `AVoxelClipmapActor` derives its entire vertex spacing from `RingPresets[kNumLevels-1].OuterMeters`. |
| **Coarser far rings** (32³ → 16³ at L4/L5) | halves far-ring quads, keeps draw distance | Distant detail. Biggest structural win, most work. |
| **Trim far-ring Z extent** | distant columns rarely need their full underground stack | Underground pop-in when descending at range. |

**Do not re-propose reducing per-ring quad counts as a bug fix.** Ring radii
double and chunk footprints double with them, so chunks-per-ring is constant **by
construction** — that is what a clipmap is. The flat L0–L5 distribution is
correct. (Proposed and withdrawn 2026-07-28.)

---

### 0.5 P4 — Speculation refinements (small)

- **DONE 2026-07-28 — `SpeculativeZTrim 1` and `SpeculativeMaxInFlight 16`.**
  Trimming one chunk from each end of the speculative band removes 98% of the
  zero-quad dispatches (183 → 3 per window) and lifts adopted-per-dispatch from
  46% to 85%. Both shipped as defaults.

  **But it bought no frame time, and that is the important part.** Cutting
  dispatches by 46% moved p50 by 0.08 ms. **Reducing GPU meshing work does not
  reduce frame time on this renderer** — the meshing compute evidently overlaps
  with rendering rather than sitting on the critical path, so removing it lets
  the GPU idle rather than shortening the frame.

  ⇒ This strikes "fewer mesh dispatches" from the 100 fps path, and it puts a
  question mark over **how much of the 18.4 ms GPU frame is serial at all**.
  Anything aimed at 100 fps has to establish that first, because the same
  overlap argument may apply to other GPU items on the list.

<details><summary>Original framing (kept — the reasoning that led here)</summary>

- **~187 speculative dispatches per window still return zero quads.**
  `dropOvertaken=0` and `dropPoolFull=0`, so it is all zero-quad results — and
  "zero quads" covers all-**solid** as well as all-air, which is why they cluster
  at both ends of the surface band and never the middle. Feeding the D6 band back
  from speculative results made the shared buried skip fire at all (0 → 51 per
  window) but end-to-end effect was small, because speculation is bounded by
  `SpeculativeMaxInFlight` rather than by candidate supply. Further reduction
  needs an **analytic** empty test at enumeration time. Bounded cost if never
  done: an air or buried chunk parks nothing, holds no pool range, evicts nothing.
</details>

- ~~**`voxel.Stream.VelocityLeadSec` still defaults to 0.**~~ **DONE** — default
  is now 2.0; T4-1 is on.

  <details><summary>original</summary>

  **`voxel.Stream.VelocityLeadSec` still defaults to 0.** T4-1 is confirmed (93%
  fewer flight-phase holes, no throughput/memory/game-thread cost) but ships off
  pending an owner call. Recommended default **2.0** — the effect saturates at
  1 s, so the cheapest setting is also the best.

  </details>

---

### 0.6 Standing rules for this area

1. **Read `docs/lessons-2026-07-27-s0-s1.md` first.** Seventeen lessons, most of
   them measurement failures where the number was arithmetically correct and
   answered a different question than the one asked.
2. **`frameMs`/`renderMs` are HITCH fields**, emitted only above a 33.3 ms
   threshold. Use the `VoxelPerfRun post-warmup` line for frame time.
3. **Run legs through `tools/voxel-run-flight-leg.ps1`**, summarise with
   `voxel-leg-summary.ps1` (refuses partial legs), audit with
   `voxel-audit-leg-overlap.ps1` (two legs sharing the box read exactly like a
   slow configuration).
4. **State the resolution.** The harness defaults to 1600x900; the target is 2K.
   Resolution is currently free because the renderer is geometry-bound — re-check
   after any change, because the moment something becomes pixel-bound every 900p
   measurement stops transferring.
5. **Never tune `GPUCullMergeGap` and `GPUCullMaxRanges` separately.** Swept
   alone, the winning value of each measures *worse* than the loser.
6. **Do not re-propose the falsified levers:** fork caps 16/32,
   `GpuMeshInFlight` 1024, `DispatchAfterDrain`, `AdmissionBandSkip`, ring
   cross-fade, slot-floor sweeps, ring-major dispatch, idle defrag (T3-6).
7. **A gate that no-ops and exits 0 is not a pass.** Several verification
   commands do exactly that when issued via `-ExecCmds` at startup.

---

## 1. Needs a decision or a spend, not engineering

**NVIDIA determinism leg.** The cross-vendor gate is verified bit-exact on the
AMD RX 7800 XT across all three modes, but the NVIDIA leg has never been run.
`docs/gpu-streaming-plan.md` already called it "the only unmet part of the
original gate", and worldgen v8's respin did not change that either way. Until
`tools/run-nvidia-digest.sh` runs against the current `.spv`, the cross-vendor
claim rests on one vendor. **Cost:** ~$5 and ~20 minutes on a rented box.
**Expected digests** are in `voxel-core/shaders/prebuilt/README.md`'s v8 respin
section.

~~**A second pregen in an arid region.**~~ **RESOLVED by regeneration, not by a
second pregen.** The orographic rain-shadow coupling plus the WorldClim
conditioning rebuild (§4, 2026-08-01) made the shipped world itself arid in
places: DESERT 9.74% of land, and both biomes now have real-tile coverage —
desert has **baked fine tiles** and a censused site, savanna a real interior
sample at s1 stride (`docs/biome-placement-survey.md`). The remaining arid gap is
authoring, not terrain: see §10a on roster starvation.

**Adopting the existing tile cache under the new identity.** Identity schema
v3 rolled every `provider_id`, so `pregen`/serving will not find
`terrain-diffusion-unlabeled-3e11cf157a836c70`. The game is unaffected
(`DefaultGame.ini` points at the directory by path and `TileGridSampler`
validates seed/scale, not identity). Set `provider_id_override` if the provider
should serve those tiles — that is exactly its sanctioned use.

---

## 2. Blocked on something else landing

**erosion-v7 (dendritic drainage).** ~340 lines of flow-accumulation valley
carving, parked. The version collision with v8 is trivial; the real blocker is
that the branch lands drainage CPU-only behind a flag defaulting ON, and PR #104
made `voxel.Stream.GPU` the default the same week. Under ADR-0006 display
geometry is GPU-generated while collision stays CPU, so drainage-on means up to
**5.6 m — 56 voxels — of solid-looking ground you fall through**. Full reasoning
in `docs/status.md`, "erosion-v7 is PARKED".

*Unblocks, cheapest first:* (a) land it with the flag defaulting OFF — default
output then equals `main` byte-for-byte, so no golden moves and no
`kWorldGenVersion` bump is needed at all; (b) port flow accumulation to a GPU
compute pass, which pairs naturally with the harness widening below.

**GPU harness cannot see real tiles.** *(Still open, confirmed 2026-08-19:
`gpu_harness.cpp:1963` still takes `SyntheticTileSampler&`.)* `gpu_harness.cpp`
and `VoxelGpuVerify.cpp` take a concrete `SyntheticTileSampler&`, not
`ITileSampler&`, so `vxc_gpu` can only ever exercise synthetic climate — never
the real-tile regime where v8's miscalibration actually lived. Mitigated for now
because v8 made the synthetic emission span every threshold, but that is a
mitigation, not a fix. **Worth doing before ADR-0006 makes the GPU path fully
authoritative.** Pure plumbing; the raster fill already goes through the virtual
interface.

**M1 gate min-spec-proxy re-run.** The 2026-07-24 numbers were taken at default
quality, not the historical min-spec protocol, so the gate row is uncoloured
pending a re-run. v8 changed terrain materially, so this wants redoing anyway.

---

## 3. Cheap and self-contained

**`tiff_export` round-trip smoke test.** `tools/make_conditioning.py`'s output is
validated against `tiff_export.CHANNEL_FILES` (five channels, float32, CRS, sane
ranges) but has never been round-tripped through the model, because a 200-cell
sketch upsamples 256x per axis into a 51200² raster. Run it once with a small
`--cells` (say 16 → 4096²) to prove the path end to end.

**Add a 3750 environment to `amplifier_surface_bound_adversarial`.** The test
crosses two pixel sizes, 30000 and 11250, and 11250 is *not* the scale-8 value —
that is 3750. The comment now says so. Adding a real 3750 environment is a new
environment plus a re-pin of a golden that is explicitly not worldgen output, so
it is a deliberate small change rather than a free rider.

**DONE — verified 2026-08-19.** `test_amplifier.cpp` now runs real 3750 *and*
1875 environments (`{kSeed, 3750}` at :591, and the `pixelMm[]` sweep at :792
crosses 30000/3750/1875); :869 records the digest re-pin those additions forced.

**Re-run `-VoxelMatHistogram` in engine.** v8's fix is verified by the CPU-side
top-voxel census (surface materials 12.4% → 100.00%), not by the quad census
that produced the original `MAT_ROCK 15% / MAT_SUBSOIL 85%` measurement in
`VoxelClimateProbe.h`. Re-run the switch and update that comment with real quad
numbers before anyone builds an id-keyed appearance rule on it.

~~**`VoxelEarth.Build.cs` hardcodes `build/voxel-core-msvc/voxelcore.lib`**~~
**DONE — verified stale 2026-07-28.** The module now probes
`Debug`/`RelWithDebInfo`/`Release`/`""` under `build/voxel-core-msvc` in a
deliberate order and errors with the full search list if none match, so
multi-config generators work without a manual copy.

**Shared `TileGridSampler` between `VoxelWorldSubsystem` and
`VoxelClimateProbe`.** Already flagged in `VoxelClimateProbe.h`. The two loaders
being copies is what caused the climate-vs-geometry divergence fixed in PR #113
— the tile-dir precedence is still a copy, and only the seed default is now
shared. Making both read one loader removes the class of bug rather than the
instance.

---

## 4. Larger, and worth scoping properly

**Natural language → world.** The design goal. The chain is already built except
the front end: `make_conditioning.py`'s `WorldSpec` is a small struct of named,
human-meaningful scalars, and everything downstream of it is that file plus
terrain-diffusion. Translating *"a big temperate continent with a dry interior
and a wet west coast"* into that struct is the only part that needs a language
model. `--dump-spec` already emits the resolved struct as JSON so a generated
spec can be inspected and edited before anything is generated.

*Two things to fix while doing it:* the round-trip is unproven (§3), and the
generated mountain ranges still read as drawn ribbons — deliberately not chased,
since elevation's `cond_snr` of 0.3 lets the model reinterpret them freely.

**Deserts require conditioning the precipitation channel.** Not a tuning
exercise. `synthetic_map.py` generates precipitation as an independent Perlin
field, never touched by elevation or distance from ocean — no orographic
rainfall, no rain shadow, no continentality. So no landmass scale produces a dry
interior. Either author the precipitation raster (`make_conditioning.py` does
this) or patch `synthetic_map.py` to couple it. See `docs/worldgen-levers.md` §2.

> **DONE 2026-08-01 — both halves.** `synthetic_map.py` was patched to couple
> precipitation to terrain (an orographic rain-shadow pass; the patch is
> `terrain-service/patches/terrain-diffusion-worldgen.patch`, parameters at
> `providers/diffusion.py:549-566`). Measured: correlation between the upwind
> barrier and the rainfall multiplier **−0.734**; mean multiplier **0.493**
> behind a barrier over 600 m against **1.393** with none. Separately, the
> conditioning statistics were rebuilt from the real WorldClim rasters — the
> previous file had used hand-written latitude formulas substituted when
> WorldClim was unreachable, which held precipitation to **7.8%** of its
> encodable range over land. The shipped world now measures **DESERT 9.74%,
> RAINFOREST 4.73%**, all eight mappable biomes non-zero. Note the rebuild alone
> was not enough: deserts only appeared after a monotone remap of the model's
> *output* in `adapt_raster_to_tile` (`3b511e3`, `56257c8`). See
> `docs/world-generation-architecture.md` §6.1–6.2.

**Narrow `diffusion.py`'s bio_12 quantization range** from 0..12000 to ~0..4000
mm/yr, and bio_1 from ±40 to ~±30 °C. Precipitation currently occupies 23 of 256
codes — 1 LSB is 47 mm/yr, coarser than the distinctions the biome thresholds
draw. This is the *root* fix for that; the physical-threshold work in v8 was the
consumer-side half. Changes tile bytes, so it needs a GPU rental and a
`provider_id` roll: **attach it as a rider to the next paid pregen**, not as its
own trip. Pleasant side effect: it would collapse `VoxelClimateProbe`'s remap to
near-identity and remove a calibration entirely.

**Scale-dependent slope thresholds.** `slopeMmPerPx` is proportional to
`pixelSizeMm`, so `kBiomeCliffSlopeMmPerPx`, the topsoil retention term, and
`slopeScaleQ10`/`microScaleQ10` in `amplifier.cpp` all mean different things at
scale 8 than at scale 1. Latent — only scale 1 has ever been generated — and
documented at the constant. Threading `pixelSizeMm` through `classifyBiome`
alone would be a half-fix at full CPU/GPU-mirror risk, so **do all three
together, before generating scale-8 tiles.**

**Restore per-material subsurface strata.** `VoxelClimateProbe`'s R channel is a
binary surface flag because thresholding a categorical material id through the
vertex-colour transform measured unreliable. The cost is that a cave wall is one
rock colour instead of bedrock/rock/gravel/subsoil/clay. v8 makes an id-keyed
rule viable again (surface materials now reach 100% of top voxels), but the
R-channel transform needs understanding first.

---

## 5. Remaining identity gaps

`DiffusionConfig.provider_id()` now covers checkpoint content, conditioning-data
content, world-shape kwargs, scale, channel mapping and the tile wire format.
Still outside it, in rough priority order:

1. **Execution environment.** `_load_pipeline` silently falls back to
   `device="cpu"` when no GPU is visible, so CPU- and GPU-generated tiles share
   a namespace; `torch`/cuDNN versions and TF32 flags are likewise absent.
   Doctrine §2.3 accepts cross-GPU non-determinism, but the id does not record
   which side of the CPU/GPU split a tile came from.
2. **`terrain_diffusion_version` defaults to `"UNRECORDED"`** and, unlike
   `UNPINNED`/`UNVERIFIED`, is neither refused before inference nor marked in
   the id.
3. **`pipeline.bind()`'s caching strategy.** `(x, y)` are not independent
   per-tile seeds; seamlessness comes from the tile store's cached context, so
   which neighbours are resident is process-history state that can influence
   output.

**`pregen --provider diffusion` still constructs the UNPINNED default config.**
Wire pinned-config selection into `app._make_provider` once a production
checkpoint is chosen.

---

## 6. Upstream (terrain-diffusion) issues we work around

**`drop_water_pct` silently does nothing.** `make_synthetic_map_factory` loads
`data/global/synthetic_map_stats.json` unconditionally and the cache is not
keyed on the parameters, so the land/ocean knob has no effect once that file
exists. `frequency_mult` escapes only because it also flows into the uncached
noise config. Delete the cache file to use `drop_water_pct`, and note that
recomputing it needs the WorldClim bio rasters present, not just
`etopo_10m.tif`. Details in `docs/worldgen-levers.md` §6.

**`python -m terrain_diffusion` imports the training stack** (`confection`),
which is not needed to run inference. Invoke the module function directly —
`tools/world_map.py`'s docstring shows how.

---

## 6a. RESOLVED 2026-07-26 (Wave C): the determinism gate is GREEN on both legs

**Both legs now print `6e893ab3679a8c81` and PASS bit-exact.** Two legs each,
same box, AMD Radeon RX 7800 XT:

| leg | toolchain | result | digest |
|---|---|---|---|
| `build/voxel-core-msvc/bench/vxc_gpu.exe` | DXC `cs_6_0` -> SPIR-V -> Vulkan | **PASS**, bit-exact, 8192 columns / 393216 cells / 6668 quads | `6e893ab3679a8c81` |
| `voxel.GPU.VerifyRegion` | UE `cs_6_6`/`6_8` -> DXIL -> D3D12 | **PASS**, bit-exact | `6e893ab3679a8c81` |

### The cause was VERSION SKEW inside one process, not the toolchain

The failing run compared a **worldgen v6 CPU reference** against a **worldgen v8
GPU kernel**. `voxelcore.lib` predated the v8 climate landing (`e25d563`, which
reached `main` at `2c7eb68`, 2026-07-25 19:28) while Unreal compiled the current
`worldgen.ush`. Nothing about DXIL, D3D12, `$Globals` packing or shader model was
ever involved.

**How that was established, rather than argued** (all on this box, 2026-07-26):

1. Built voxel-core at `2c7eb68^1` — `main` immediately before v8 — into an
   isolated worktree, and ran its `vxc_gpu` against its own committed SPIR-V:
   **PASS**, digest `f3c48a4df3e20e9a`. That is the pre-v8 baseline the four
   files listed below still quote, so the reconstruction is faithful.
2. Ran that same **v6 CPU** binary against the **current v8 SPIR-V**. It
   reproduces the recorded failure's values exactly:
   `cell(-64,-64,vz=11648): cpu=2 gpu=5` and `vz=11654: cpu=5 gpu=12`.
3. Ran the **reverse** pairing (current v8 CPU, pre-mirror v6 SPIR-V from
   `3fbf3f7^`). It produces the mirror image — `cpu=5 gpu=2`, `cpu=12 gpu=5` —
   which rules out "the shader was the stale half".
4. The recorded quad counts corroborate the direction: `3424 quads (cpu 3422)`
   is GPU-then-CPU, and 3422 is measured to be the **v6** count for that region
   while 3424 is the **v8** count.
5. No commit after the failure was recorded (`84b90fc`, 01:12) touched
   `worldgen.ush`, `shaders/prebuilt/`, `VoxelGpuWorldGen.cpp` or voxel-core's
   amplifier. Only the artifacts moved: `voxelcore.lib` was rebuilt at 02:00 and
   the editor relinked at 03:00. The gate has passed on every run since.

### Three wrong diagnoses, all kept, and what each one got wrong

1. **"worldgen v8 was never mirrored into the HLSL."** Wrong — the mirror
   (`3fbf3f7`) is faithful, as the bench proves. But the *class* was right: this
   was version skew. It looked for it in the source instead of in the link.
2. **"Floating-point contraction in DXIL flipped a layer comparison."**
   Impossible: `worldgen.ush` has no floating-point arithmetic anywhere (every
   operand is `int64_t`/`uint64_t`), and UE 5.8's D3D12 backend never translates
   `CFLAG_NoFastMath` (`D3DShaderCompiler.cpp:52-61`), so the proposed fix could
   not have been written either.
3. **"All 4,096 columns match, only cells differ."** Also wrong, and it is what
   sent both of the above hunting in the kernel. Under v6-CPU/v8-GPU the column
   fields `topsoilMm`/`subsoilMm` differ too (measured: `cpu=0 gpu=366`,
   `cpu=500 gpu=1232`), and `CompareRegion` prints a column's field mismatches
   *before* that column's cells. The quoted transcript was an excerpt with the
   `col(...)` lines dropped, and the conclusion was drawn from the excerpt.

### Guards added so this cannot present the same way again

* **`voxel.GPU.VerifyRegion` now pins a CPU-REFERENCE digest** of its own
  (`kExpectedCpuDigest`, `VoxelGpuVerify.cpp`), folded from
  `vxc::Amplifier`/`vxc::meshBrick` with no GPU involvement. A stale or
  mismatched `voxelcore.lib` now fails with "the linked voxelcore.lib is NOT the
  worldgen this gate is pinned to" instead of a list of per-cell materials. It
  also catches both sides moving *together*, which GPU-vs-CPU equality
  structurally cannot.
* **Mismatches are now counted and classified by stage** (column field / cell /
  quad) with the totals printed uncapped, and the capped list is labelled as an
  ordered subset that must not be quoted piecemeal. That is diagnosis 3's exact
  failure mode, closed.
* **`worldgen.ush` carries a compile-time version lock.**
  `VXC_WORLDGEN_VERSION_USH` must equal `vxc::kWorldGenVersion`, which
  `ModifyCompilationEnvironment` passes in as `VXC_WORLDGEN_VERSION_CPP`; a
  mismatch is a shader `#error` (verified by compiling with a deliberately wrong
  value). Because a define is part of the shader's DDC key, a version bump also
  forces a recompile instead of silently reusing the previous version's
  bytecode. Scope stated honestly in the file: this catches **source** skew, not
  a stale `.lib` — the header constant would still read 8. The two guards cover
  the two different faults.

### The historical entry, kept for the record

### An earlier version of this entry blamed worldgen v8. That was wrong.

The first diagnosis here said the v8 climate wave had changed CPU material rules
without mirroring them into the HLSL. **It had not.** The mirror exists and is
correct: `3fbf3f7` ("Step 2d: the worldgen.ush mirror + SPIR-V respin -- AMD leg
GREEN", 18:37) lands *after* `e25d563` ("Step 2 (CPU half)", 14:55), both are on
main, no CPU worldgen commit follows the mirror, and the bench passing bit-exact
proves the mirror is faithful. The wrong diagnosis is recorded rather than
deleted because it is the obvious one and the next person will reach for it too.

### Where the divergence actually is

From the in-engine failure (`voxel.GPU.VerifyRegion 0`):

```
[origin] 4096 columns, 196608 cells, 3424 quads (cpu 3422)
[origin] quad decode: 20544 vertices match the CPU reference exactly
    cell(-64,-64,vz=11648): cpu=2 gpu=5      MAT_ROCK    vs MAT_SUBSOIL
    cell(-64,-64,vz=11654): cpu=5 gpu=12     MAT_SUBSOIL vs MAT_PERMAFROST
```

- ~~**Columns match** (all 4,096) — `ColumnMain` agrees under both toolchains.~~
  **CORRECTED 2026-07-26: they did not.** The block above is an excerpt whose
  `col(-64,-64).topsoilMm` / `.subsoilMm` lines were dropped; the harness emits a
  column's field mismatches before that column's cells, so they were there. This
  single inferred sentence is what made the fault look confined to
  `VoxelizeMain`, and it produced the two wrong diagnoses that followed.
- **Cells differ**, material ids only, and in a consistent direction: the DXIL
  build's soil column sits one layer shallower than the CPU's. What the CPU calls
  rock, it calls subsoil; what the CPU calls subsoil, it calls the *surface*
  biome material.
- **The mesher is innocent** — quad decode matches exactly (20,544 vertices); the
  2-quad count difference is downstream of the cell mismatch.

~~So the fault is isolated to **`VoxelizeMain` as compiled by Unreal**. Given that
identical source is bit-exact under DXC/SPIR-V, the prime suspect is
**floating-point contraction**: an FMA or reassociation in UE's HLSL compilation
flipping a `<` at a layer boundary by one ULP.~~ **Both sentences are wrong.**
`worldgen.ush` contains no floating-point arithmetic to contract, and the two
sides of the comparison were different worldgen versions — see the resolution at
the top.

### ~~Next step~~ — superseded, and unbuildable as written

~~Compare the shader compilation flags UE uses for these kernels against the
standalone DXC invocation in `voxel-core/bench`.~~ Done, and they are not the
cause. For the record, since it is the obvious next reach: UE 5.8's D3D12 backend
maps only `PreferFlowControl`, `AvoidFlowControl` and `WarningsAsErrors`
(`D3DShaderCompiler.cpp:52-61`) and returns 0 for everything else, so
"force strict IEEE for `VoxelizeMain`" could not have been written at all. The
two real flag deltas — `cs_6_0` vs `cs_6_6`/`6_8`, and the explicit `cbuffer` vs
loose `$Globals` scalars — were both checked in Wave C and are both innocent:
DXC emits `$Globals` for the `VXC_UE` variant at byte-identical offsets to the
bench's `cbuffer` (`BrickZMin` at offset 44, 56 bytes, verified by disassembling
both), and the `cs_6_6` DXIL leg is now bit-exact over 393,216 cells.

### On the recorded digest — deliberately NOT updated

Four files quote `f3c48a4df3e20e9a` (`gpu-streaming-plan.md:63-64`,
`streaming-handoff.md:12,120`, `gpu-g2-draw-path.md:115`,
`voxel-core/shaders/prebuilt/README.md:378`). The current **bench** value is
`6e893ab3679a8c81` and it is green, so that one is a legitimate re-baseline
whenever someone wants it. The **Unreal** value `046b4a9f9c5e49b7` must not be
recorded as a baseline at all: it is the output of a build that disagrees with
the CPU, and blessing it would turn a loud failure into a silent one.

### ~~Related but separate~~: a stale prebuilt library — **THIS WAS THE CAUSE**

`build/voxel-core-msvc/voxelcore.lib` was built at 15:37 against an
`amplifier.cpp` last modified at 19:40, so UE builds on main were linking pre-v8
CPU worldgen. That is a real problem in its own right and gives the
`VoxelEarth.Build.cs` hardcoded-lib item below teeth. ~~It is **not** the cause
here — rebuilding voxel-core from scratch and relinking reproduces the identical
in-engine failure.~~

**Corrected 2026-07-26.** This paragraph had the answer in its first sentence and
then dismissed it. The dismissal rests on a claimed from-scratch rebuild that
does not reproduce today and left no transcript in the tree; every run since
`voxelcore.lib` was actually rebuilt (02:00) and the editor relinked (03:00) has
passed. The reconstruction at the top of this entry then confirms the direction
numerically. **The lesson worth keeping: a stale static library is not a
"separate" problem from a determinism-gate failure — it is one of its two most
likely causes, and it is the cheaper one to eliminate.** Eliminate it by rebuild,
never by argument.

### Consequences while the Unreal leg was red — now cleared

- ~~GPU-meshed terrain would differ from CPU-meshed terrain **in-engine**, so this
  blocks the GPU meshing programme (6b) on correctness.~~ Unblocked: Wave D's
  correctness precondition is met.
- **The NVIDIA leg (Wave C2) is now unblocked and still owed.** It has never been
  run; the cross-vendor claim rests on one AMD RX 7800 XT.
  `tools/run-nvidia-digest.sh` exists for a rented box. Not runnable from this
  machine — there is no NVIDIA GPU on it.
- **The min-spec-proxy M1 gate re-run (Wave C3) is still owed**, deliberately not
  attempted in Wave C: it is a 60 s frame-time measurement and this box was
  running three other build/editor agents concurrently. Ground rule 1 exists
  because numbers taken like that have already been retracted once. It also wants
  to land after Wave A's cull work, which changes what it measures.

---

## 6b. GPU streaming (ADR-0006), after G0-G5 landed

> **SUPERSEDED 2026-07-28 — see section 0.** Written before Waves S0-S4 and the
> draw-path work. Its central open question ("does the pool make frames faster?
> unmeasured") is now answered: p50 30.59 -> 15.2 ms at 2K and hitch frames
> 3,747 -> ~11 (`docs/measurements/drawpath-2k-2026-07-28.txt`). The fixed-camera
> harness it asks for was also built: `tools/voxel-run-flight-leg.ps1` plus the
> `VoxelPerfRun post-warmup` line give p50/p95/max over ALL frames. Kept below
> for the historical reasoning.

**Does the pool actually make frames faster? Unmeasured, and the harness cannot
currently say.** G5 flipped `voxel.Stream.GPU` on by default. The parity case is
solid (17.4% -> 4.3% of pixels differing, against a measured 1.1% noise floor)
and the mechanism is verified as a count (9,822 chunks / 8,813,242 quads as ONE
primitive and ONE draw, against 9,822 primitives). **The frame-time claim is
not** — an earlier p95/hitch table was retracted after twelve legs showed the
ranges overlapping, and a follow-up designed to remove streaming noise (anchor
stationary, cascade fully settled) still saw the component path render the
identical scene at 43 fps and 103 fps in two runs. Leading suspect is camera
orientation: the anchor is a position, and the pawn's yaw/pitch are neither
pinned nor logged. **Unblocks:** a fixed-camera perf harness that also LOGS the
pose. Start from `-VoxelFloodTest` / `Tools/capture_terrain_shots.ps1`. Until
then make no frame-time claim in either direction, including a negative one.
Full write-up in `docs/streaming-handoff.md`.

**GI volume steps 3-5 — LANDED (Wave B, 2026-07-26), with three corrections.** Steps 0-2 had landed: the pooled path feeds the light field,
the volume is sampled per pixel, and the encode matches the CPU sampler at
**0.000 mean error** (with a deliberate half-cell-shifted control to prove the
harness is not comparing a thing against itself). Steps 3-5 are now done; the
full write-up with every number and how it was measured is the Wave B section of
`docs/gpu-waves-plan.md`. The three things worth carrying here:

- **(3) Scheme A is BUILT and measured - the recorded bar was never passed.**
  `gpu-gi-volume-design.md:100-103` asks for an **RMS** under ~8/255. The figures
  on record (5.950, 6.165) are **mean-abs** compared against an RMS threshold.
  Measured RMS at the settled field is **20.4-21.1**, i.e. Scheme B misses the
  actual bar by **2.6x** and always did. The distribution is bimodal (p50 ~ 0.02,
  p95 ~ 52, max 105) because Scheme B stores the four horizontal directions as
  their mean: exact where they agree, worst where they disagree - a side face
  beside a vertical occluder, i.e. most of a cave wall. The two transcripts that
  disagreed in the tree are also reconciled: two legs on one build agree to
  +/-0.17 bytes, so the harness is precise and the figure tracks how settled the
  field was (1,947 vs 2,212 resident bricks). Quote the brick count beside the
  error or the number means nothing.
  Section 2's **"two samples"** cost for Scheme A **does not apply to this mesh**.
  Scheme A stores (+X,+Y,+Z,v) and (-X,-Y,-Z,v); section 2 itself establishes that
  every greedy-mesh normal is axis-aligned, so a face needs exactly **one** of the
  two textures. Scheme A costs memory and nothing else. And the configuration that
  settles it is missing from the sizing table: **Scheme A at N=192 is 56.6 MB,
  less VRAM and less RAM than Scheme B at N=256 (67.1 MB), which the design
  already recommends** - exact walls everywhere, at the price of 13 m of reach.
  **Measured after the switch**, same harness/scene/sample count: horizontal
  mean, RMS, p95, p99 and max all **0.000**, fraction over the bar **0.0000**,
  with the negative control still at 1.525 mean / 58.8 max so the zeros are
  load-bearing rather than a harness comparing a thing against itself. Memory
  54.0 MiB at Dim 192 - the design table's "56.6 MB" is the same quantity in
  decimal, not a discrepancy.
- **(5) The retirable cost does not exist, and this is the third statement of
  this item.** The roadmap said "stop re-meshing to refresh lighting on the
  pooled path"; `gpu-waves-plan.md` corrected that to "the component path's 5×5×5
  re-shade plus the quad subdivision". Both are wrong in the same direction:
  under `voxel.Stream.GPU 1` no level-0 `UVoxelChunkComponent` is ever
  constructed, and GI is level-0-only, so **both costs are already exactly zero
  on the path that has a volume to sample**. What was real and is now fixed:
  solved pooled bricks were still being pushed onto `RefreshQueue` to be popped
  and discarded, which is what forced step 0's 8× pop cap. **Wave B's prize is a
  capability, not a saving** — on the pooled renderer, baked per-vertex GI does
  not exist at all.
- **`voxel.GI.Volume 1` has no consumer under `voxel.Stream.GPU 0`,** which is
  the default. Only the pooled vertex factory samples the volume, so shipping the
  volume on is **coupled to Wave A's outcome**, not independent of it - the
  execution plan lists A and B as disjoint, which is true of their FILES and not
  of their shipping. The useful consequence: **GI can ship today without either**
  - the component path's CPU bake already works, so `voxel.GI.Enabled 1` with
  `voxel.GI.Volume 0` is a shipping configuration now, and that is what Wave B
  recommends. The volume is correct-and-off, waiting on a renderer default.
  Making the COMPONENT path sample the volume was considered and REJECTED: it
  needs a material-graph change (the one thing the design was built to avoid), a
  UVolumeTexture wrapper, per-chunk custom primitive data rewritten on every
  re-centre, and it would make the material graph a THIRD copy of a shade formula
  whose existing two copies had already drifted in four places.

- **NEW AND NOT ROOT-CAUSED: the light field is effectively empty under motion.**
  2,212 resident bricks settled and stationary; **0-12** for the whole of a 90 s
  `-VoxelPerfFlight=surface` run at ~20 m/s, with `pendingVox=0(+0 pooled)`
  throughout while streaming loaded normally around it (R0 loaded=3131). A player
  is moving most of the time, so if this holds, voxel GI is largely absent in
  play and every screenshot of the feature - all settled and stationary by house
  style - has measured the one case where it works. It also blocks verifying the
  volume's re-centring on the scripted flight, which is how it was found.
  Candidates not yet separated: the build-radius rejection in the voxelize drain,
  eviction outrunning the 16-chunk-per-frame budget, or the ingest hook not
  firing for most pooled applies. **The two cheap legs that split it:** brick
  count during the flight, and ~5 s after coming to rest. Recovering on stop
  means throughput/priority; not recovering means bricks are never requested -
  the same fork as the owner's ring-gap symptom.

Closed by the same wave: the X-run merge on a dig's contiguous neighbourhood and
zero-on-revoxelize / zero-on-evict now have a harness rather than an argument
(`voxel.GI.VolumeDigTest`), `voxel.GI.Volume`'s "read per frame" claim is true
rather than aspirational, and the volume texture is no longer allocated for
sessions that never enable GI (it was a `TGlobalResource` built in `InitRHI` —
67 MB at the recommended shipping size, charged to everyone).

**Per-chunk debug tints — the last G4 item, and the only one that still needs the
material asset.** Storage is already solved (`ChunkParams.w` is free). The route
is a `float4` `TexCoords` interpolant with the tint packed in `.zw`. **The
decision that makes it safe:** encode identity as ZERO, not white — the component
path supplies only a `float2` texture coordinate, so `TexCoord0.zw` arrives as
zero there regardless of the graph, and a naive unpack treating 0 as black would
render every component-path chunk black the moment the material is regenerated.
Debug-only, so its absence costs nothing in play.

**CORRECTED 2026-07-26 — identity-as-zero is unsafe, for the opposite reason.**
Superseded text kept above, per §6a's convention. A material asking for texture
coordinate 1 gets **texture coordinate 0** on the component path, not zero:
`FLocalVertexFactory` clamps the request to the mesh's UV count and clamping
duplicates (`LocalVertexFactory.ush:729-730`, `:737`), and the mesh has one UV
set because `VoxelChunkComponent.cpp:634` takes `InitFromDynamicVertex`'s default
`NumTexCoords = 1`. The value is the world-planar UV wrapped to 32 m, so ±32 of
position-dependent garbage would have been multiplied into the **default**
renderer's BaseColor. The path that does deliver zero is the pooled one. Measured
with a probe material adding `EmissiveColor = abs(TexCoord1) * 0.05`: 30.91% of
pixels differing at >8/255 on the component path against a 3.58% same-run floor,
nothing on the pooled path. **Corrected encoding:** a sentinel range — both paths
`fmod` UVs to a 32 m period, so nothing can leave (−32, 32); store `tint + 1000`
and treat anything under ~100 as identity, which is correct on both paths with
one graph and no switch node. Full write-up in `docs/gpu-waves-plan.md` Wave E.

**`voxel.Stream.AdmissionBandSkip` is off, and should stay off** until two things
are checked: its edit veto uses `EditedFootprintMinZ` where the dispatch site
uses `ChunkHasEditedBrick`, and the argument that they agree is reasoning rather
than measurement; and frame rate collapsed with it on (279 -> 89 ticks/5 s) for
reasons never explained. It reaches ~4% of the waste it was aimed at anyway (89
skips against 2,186), so there is little to gain.

**R0 to 128 m is now unblocked** — `RingPresets` became a runtime accessor
(`GetRingPresets()`), overridable via `-VoxelRingInnerMeters=` /
`-VoxelRingOuterMeters=`. Moving R0 itself is still an open call, and the
`+9.2%` resident-chunk cost of the seam-padding fix is the thing to weigh it
against.

**Ring cross-fade: do not build it for the pooled path.** Re-tested after the
seam fix gave the annuli their overlap band, and it still produces see-through
patches at ring boundaries. The G0 checklist listed it first; it is the one item
on that list that should not be built. Reasoning and the likely root cause (both
rings fading simultaneously across the shared band rather than crossing over) in
`docs/gpu-g4-parity-plan.md`.

---

## 7. Measured and CLOSED — do not re-litigate

Recorded so these are not re-attempted. Each was measured, not argued.

**The ~600 chunks/s pooled plateau (2026-07-27).** CLOSED at 797.0 chunks/s with
converged **holes = 0** — the first time the pooled arm completed the world at
the adopted 128 m / 4 km cascade. Cause was the per-chunk `PushUpdatesToProxy`
(98–99% of per-apply cost, rising to 2.1 ms/apply under flight), which backed up
the result queue until 42% of drained results were discarded as stale. Fixed by
batching publication once per tick, plus an unload budget raised from a value
sized for the old throughput. Full ladder:
`docs/measurements/s1-close-2026-07-27.txt`.

**"Results are not ARRIVING" as a reading of the plateau.** FALSE, and it was an
artefact of `LastAppliedFrac` dividing by the count cap while the drain loop
breaks on a wall clock. `queueEmpty` was 0 in every streaming window measured —
results arrived faster than they could be applied. Do not re-derive producer-side
work from that sentence.

**T1-3, the per-apply `Amplifier::column` / column-keyed params cache.** STRUCK.
`SampleChunkParamsForPool` measures 0.002–0.004 ms per apply — **0.2% of an
apply** — at every point in every leg, against `poolAdd`'s 98–99%. §1c names it
as an aggravator and it is not one. Caching it removes nothing.

**T3-6 idle pool defrag, as a response to the batching-era allocation failures.**
NOT NEEDED. 16,903 free runs and a 72× collapse in largest-free-run at equal
total free looks exactly like first-fit pathology; it was residency ballooning
because `MaxUnloadsPerFrame` was still 24, a value sized for a 260 chunks/s
pipeline. Raising it took `allocFail` 26,763 → 0 and holes 257 → 0. (Defrag may
still be wanted for other reasons; it is not the fix for this.)

**`MaxAppliesPerFrame` at 192 as a regression.** WRONG, and recorded here
because it was nearly entered as fact. A leg measuring 749 chunks/s / 179 holes
had shared the box with a second editor for 86 s; re-run alone on the same
binary it gave 1,033-1,048 with holes 0, a +30% win over 794. The default is now
192. A contended leg looks exactly like a slow configuration, and a plausible
mechanism had already been written to explain it.

**Handle recycling as a THROUGHPUT lever.** `Allocations` is append-only and
`BuildChunkRuns` walks it, giving a 3.72× walk:emit ratio by end of flight — but
cost tracks `emit`, not `walk`. Cutting the walk 45% bought 2%; `BuildChunkRuns`
is dominated by `Runs.Sort()` over live runs. Keep the recycling as an
unboundedness fix (`allocsEver` −60%, and it grows all session otherwise); do not
expect chunks/s from it.

**Sea level as a design lever.** Not a generation input at all: z=0 is inherited
from ETOPO's datum and hardcoded downstream (tile format, `biome.h`'s coastal
band, `caves.h`'s implicit ocean, the water system). Lowering it does not help
regardless — inland reach is *exactly* 123 km from 0 m through −500 m, because
the new coastline runs parallel to the old one, so the apron widens and no
interior deepens. It also destroys beaches (2.3% → 0.2% at −100 m).

**Seed selection, for continents or deserts.** A seed picks a realization, not a
process. The default sketch is Perlin FBm quantile-matched to Earth's histogram,
and Perlin is isotropic, so every seed is an archipelago: inland reach 69–92 km
across four seeds against a 246 km measurement ceiling.

**`frequency_mult[0]` for a habitable continental interior.** It does enlarge
landmasses (inland reach 123 → 192 km, largest landmass 197k → 315k km²) but the
interiors are not usable: elevation climbs monotonically inland to a mean of
2240 m at 100–200 km, 66% above the treeline, while precipitation barely moves.
The model learned Earth's hypsometry, where large landmasses have high interiors.
It buys **alpine plateau, not continental interior.** Leave it at its default;
use conditioning instead.

**`kBiomeTreelineBaseMm` (900 m at 0 °C).** Low-sensitivity and visually
undecidable: a 4× change moves the alpine share only 48.6% → 35.0%, and
in-engine captures at 300/900/1800 m are indistinguishable at both low ground
(below treeline everywhere) and summit (above it everywhere). It only moves a
boundary on mid-elevation slopes. Left at 900 m, which is physically defensible.
Revisit only if the world reads too bare in normal play.

---

## 8. ENVIRONMENT ASSETS (asset-forge) — added 2026-08-11

> **LARGELY SUPERSEDED BY §10 — reviewed 2026-08-19. Read §10 first.** The
> premise here ("forty-two species, none of them in the world") is two programmes
> out of date: **828 species** are authored, the engine composes them into terrain
> chunks on both the CPU and GPU paths, and the 2026-08-17/19 programme placed,
> censused and mapped them across every land biome. What survives is the
> asset-forge measurement discipline — the rock findings and the seven
> measurement traps at the end of this section, which are still the rules for
> judging a shape. Items found closed by the review are struck below.

Everything below is either the last mile of getting assets into the world or a
decision about who owns a number. Context: `asset-forge/README.md`,
`docs/tree-asset-generator-plan.md`.

~~**The sequenced plan lives in `docs/asset-forge-plan.md`**~~ — that plan's five
phases are done or overtaken, and it now carries a banner saying so. The current
forward list is §10.

**The shader palette — DONE.** *(Table 2026-08-11; the wiring landed with the
asset programme — `VoxelQuadVertexFactory.ush:874` carries palette RGB on
`TexCoords[3]/[4]` with `isAsset`, and the material graph does the
`lerp(biomeAlbedo, paletteRGB, isAsset)` recommended below. Kept for the
reasoning.)* ADR-0008 made
`vxc::kMaterialPalette` the one definition of what a material looks like.
asset-forge already read a generated copy; the renderer read nothing, so what a
designer approved in the forge and what the game drew were two different
answers.

Done, and verifiable without an editor:
`ue-project/Shaders/VoxelMaterialPalette.ush` is **generated** from the header by
`ue-project/Tools/gen_material_palette_ush.py` — all 26 materials, three face
classes each, sRGB converted to linear once at generation, plus the ADR's
evaluation (`VoxelMaterialColor(mat, faceClass, voxel)`: voxel-keyed hash tint
and the trilinear patch term, no lighting). `tools/compile-shaders.ps1` now runs
two checks on it: `--check` fails if the .ush has drifted from the header, and
DXC compiles `VoxelMaterialPaletteTest.usf` to **both** ADR-0001 targets. Both
guards were proved to fire — a perturbed colour is named with its line, and
moving `MAT_BARK_PALE` back up among the woods reproduces the historical
"out of step with the enum at index 19". Neither needs the editor, which is the
point: one editor per box is a hard rule, so a check that needs it is a check
nobody runs.

**What is left is the wiring, and it needs a decision as well as the box.** The
hook is named in `VoxelQuadVertexFactory.ush` (the `bMarker` branch: "the
smallest possible instance of the thing biomes need next: per-material
appearance in the pooled renderer... the id survives here"). Two constraints
meet there. `VertexColor` is full — R surface flag, G ambient occlusion, B and A
climate — and `TexCoords[1]/[2]` are already spoken for by the local-lights plan
(`docs/sky-and-local-light-plan.md`), so the palette needs `TexCoords[3]/[4]` at
pixel rate and a `M_VoxelTerrain` graph change to read them. The graph is a
generated artifact (`Tools/create_voxel_material.py`), so that edit is code, but
running it is a `-run=pythonscript` commandlet and therefore editor-bound.
**Recommended shape:** feed the palette only where the biome graph has no
answer — `BaseColor = lerp(biomeAlbedo, paletteRGB, isAsset)` — which leaves
terrain's climate-driven colour untouched and is inert until assets are actually
in the world. Do NOT replace the biome path wholesale as a first step; it cannot
be verified in the same motion and it is the only appearance path that currently
works.

**Also found — SINCE FIXED.** `ue-project/Tools/terrain_palette.py` *was* a
**second palette**, 16 entries, stopping at `MAT_WATERMARK`, and its own header
called it "single source of truth". Its RGB column is now **generated** from
`materialpalette.h` by `gen_material_palette_ush.py` and checked by
`compile-shaders.ps1`; the file owns only the UE-side `BIOME_TINT` policy and its
header now says so. Original entry kept for the reasoning. The ten asset materials (bark, heartwood, deadwood, six leaf types,
pale bark) have no appearance on the UE side at all. It is not urgent only
because no asset is in the world yet. When the wiring above lands, its RGB
column should be generated from the header too, leaving it to own just the
UE-side `BIOME_TINT` policy, which genuinely is not the engine's business.

~~**UE wiring for asset streaming. BLOCKED on the editor box.**~~ **DONE** —
built and verified in-editor across the 2026-08-15/19 programme: composition at
every LOD level on both the CPU and GPU paths, plus exact per-footprint
admission. Original entry:
The voxel-core half is designed and largely written — `assetplacement.h` gives a
provable upper bound on how high an asset reaches, which is the thing that lets
the streaming admission path keep skipping chunks it can prove empty. What is
left is the UE module: getting a baked asset into the volume the marcher reads,
and confirming a crown lands in the chunks the bound said it would. Neither half
of that can be *verified* without the editor, and one editor per box is a hard
rule here — two capture sessions on one machine killed each other's frames for
hours and read exactly like a slow configuration. Another session holds it.
**Unblocks:** the box, nothing else.

**Placement: an asset-forge panel, or `assetplacement.h`? — DECIDED, and the
answer was compile-the-spec.** asset-forge authors the intent (per-kind ×
per-biome densities, explicit allowlists, named rule overrides, in the web app's
Placement panel); `tools/export_manifest.py` compiles it into the VXM2 manifest;
the engine reads that as its `AssetLayer` numbers. One number, two readers — the
panel-vs-header split never grew. See `docs/placement-spec-schema.md`. Original
entry: The one thing that still matters while it waits: **neither side may
grow its own version of the other's numbers in the meantime.** A spacing
authored in a panel and a spacing typed into an `AssetLayer` is the Appendix's
failure with a new subject, and the deferral makes that more likely, not less,
because both halves stay half-built.

Right now it is neither, twice. asset-forge writes a `placement` group per
species — `abundance`, `spacing_m`, `cluster`, slope and elevation gates, biome
weights — that **no code reads**. voxel-core has `assetplacement.h`, which does
the per-chunk deterministic scatter and the bound, and gets its numbers from an
`AssetLayer` struct that nothing fills in from a spec. The two are not
alternatives so much as two halves that have never been introduced: the spec is
a designer's *intent* for a species, the header is the engine's *mechanism*, and
the open question is only which one owns each number and how the intent reaches
the mechanism. **Decide before either grows its own version of the other's
numbers** — a spacing authored in a panel and a spacing typed into an
`AssetLayer` is the Appendix's failure with a new subject. The cheap answer is
to compile the spec's placement group into `AssetLayer` at bake time so there is
one number with two readers; it is written here as a decision because choosing
the panel instead is a legitimate call and it changes what gets built.

**Review the 16 legacy rock species against the newer mechanisms.** The rock
generator gained real geology on 2026-08-10 — bedding, joint sets, columns,
corestones, veins, clasts, tafoni, flutes, pans, exfoliation, arches and
caprock — and the eighteen species authored during and after that wave use it.
The sixteen that predate it do not: ten of them (`granite-boulder`,
`standing-stone`, `summit-tor`, `glacial-erratic`, `river-cobble`,
`alpine-scree`, `limestone-slab`, `cliff-fall-block`, `desert-mesa-block`,
`mossy-forest-boulder`) run **no** mechanism at all, and six more
(`basalt-colonnade`, `exfoliating-dome`, `fractured-outcrop`,
`jointed-granite-tor`, `banded-sandstone-ledge`, `tafoni-sandstone`) run one or
two of them. That is not automatically wrong — a river cobble genuinely has no
joints to show — but every one of those sixteen was tuned around a weathering
pass that was removing **20 voxels from a stone of 90,000**, so their shapes
were won with `rough` and cut planes standing in for erosion that never ran.
Worth a pass to see which are now saying the wrong thing about their own rock
type. **Judge it with `tools/waistprobe.py`, NOT with `rockmech.py`** — and not
with voxel counts. `rock.build` measures the finished stone and rescales it to
the authored size, so a mechanism that plainly works lands within 0.1% of where
it started and reports "changes nothing"; five did exactly that.

That refit also breaks `rockmech.py` in the opposite direction, which was found
on 2026-08-11 and is the more dangerous half. Because the stone is rescaled, any
change in MASS reappears as a shift of the whole surface, so the divergence
metric reads large whether or not the mechanism did anything shaped. Measured
with the erosion scaling deliberately disabled, `caprock` still scored 43.5% and
`notch` 22.2% at 9 m. **`rockmech.py` showing "no dead sliders" was never
evidence that the mechanisms worked**, and it is what let a size-blind `erode` —
removing a constant ~4.75 voxel layers whether the stone was 3 m or 13 m, so
14.3% of a boulder and 3.5% of a hero — survive a full pass of the harness.
What discriminates is a measurement the refit cannot launder: retreat depth in
voxels against known size, and `waistprobe.py`'s cross-section-against-height
with its overhang number and its SEVERED check.

~~**Rocks have no allocation guard.**~~ **DONE 2026-08-11.** An over-ambitious
`rock.size_m` used to surface as a bare numpy `MemoryError` from whichever
temporary happened to be unlucky, which points at a line of arithmetic instead
of at the spec that caused it. `forge/rock.py` now sizes the working set before
allocating anything — six whole-grid float32 arrays are live at the peak — and
raises a `MemoryError` naming the spec, the resolution, the grid it would need
and the ceiling it broke (`MAX_WORKING_GB`, 20 GB; a 90 m hero at 10 cm sits at
about 12).

### The measurement traps in this repo, so the next person does not rediscover them

Every one of these was hit, twice in some cases, and each cost a round of
editing correct code.

1. **Strip colour before judging shape.** `MAT_ROCK` and `MAT_BEDROCK` render
   near-black on the dark contact sheet, so only the lit top faces show and a
   round boulder reads as a flat wedge. `tools/shapecheck.py` forces every solid
   voxel to one bright material; use it before believing a shape is wrong.
2. **A pass that runs is not a pass that does anything.** The rock weathering
   pass removed 0.44% of a surface while its slider said 30%, for the entire
   life of the module, because blurred uniform noise is a narrow bell and
   min/max rescaling does not flatten it. Nothing errored. Always measure what
   a pass removed, not that it ran.
3. **Voxel count is not a valid A/B for rocks.** `rock.build` fits the finished
   stone to the authored size, so anything that changes erosion gets scaled back
   out. Use shape divergence — crop both to their occupied box, `|A xor B| /
   |A or B|` — which is what `tools/rockmech.py` reports.
4. **Count the thing you are changing.** Tuning foliage by leaf-voxel count
   hides the whole question: the acacia's canopy went from 96 clumps to 271 and
   *down* in leaf voxels, which is precisely the change that was wanted. Clump
   count and clump radius are the measurement; voxels are a side effect.
5. **One seed is not a comparison.** `variation.amount` is doing its job, so the
   same spec at three seeds is 7.0–9.8 m tall. Any A/B on a single seed is
   reading that spread. Render the same seeds on both sides, and keep an
   untouched species in the frame as a control — that is what proves the change
   is yours and not a baseline someone else moved under you.
6. **A check that fires on the normal case teaches people to ignore checks.**
   The 256-voxel size flag fired on ten of forty-two species and made "keep all
   clean" skip every large tree. It was removed and the export was made to split
   instead.
7. **Gate on authored values, never on an internal correction factor.** A rock
   stage gated on the *scaled* size worked once and then switched itself off for
   every attempt after the fitting loop corrected downward.

---

## Appendix: the recurring failure mode

Three separate bugs this week were the same shape — **two copies of one
calibration drifting apart**:

* `biome.h`'s thresholds vs the encoding the tiles actually used (worldgen v8);
* `VoxelClimateProbe`'s remap window vs `gen_terrain_textures.py`'s LUT axes;
* `VoxelClimateProbe`'s tile loader vs `VoxelWorldSubsystem`'s (PR #113) — which
  had drifted in *two* independent ways and was self-reporting in the startup
  log the entire time.

The fixes that stuck were the ones that removed the second copy rather than
re-synchronising it: thresholds derived from `climate.h` at compile time, the
seed default taken from the subsystem rather than re-typed, `world_map.py`
parsing `biome.h` instead of hardcoding it. **Prefer deriving over copying, and
when a copy is genuinely unavoidable, make it fail loudly rather than quietly.**

## W3 rivers — PARKED 2026-07-29, blocked on the terrain pipeline

Matt's call, and the evidence supports it: **do not resume river water until
worldgen actually carves riverbeds and generates basins.**

**What is built and merged (all inert):**
- `channel.h`/`channel.cpp` — riverbed geometry from discharge. Verified
  standalone via `vxc_riverprobe`: a real river from +130.6 m to −99.9 m,
  monotone every step, 0 gaps across 571,500 centreline columns, 0.20% bank
  leakage against a <1% guard. **Consumed by nothing.**
- `rivercouple.h`/`.cpp` — discharge → CA water, sustained flux → channel
  promotion, ocean as sink. `kDivertChannel` is no longer a stub. Behind
  `voxel.Water.Rivers`, **default false**.

**Why it is parked.** Run in engine, the coupling works exactly as designed —
443 segments, 9,894,505 fill units injected, ledger exact to the unit
(`storage + outlets + toCA == injected`), 3.69M units delivered into the CA.
And it looks wrong: disconnected puddles scattered along the drainage lines
rather than continuous rivers. The cause is not the coupling. **There are no
riverbeds.** `ChannelField` is referenced nowhere in `amplifier.cpp`,
`world.h` or `VoxelWorldSubsystem.cpp`, so discharge lands on unmodified
hillside and pools wherever the ground happens to dip.

The carving pass avoided a `kWorldGenVersion` bump by not wiring itself in.
That was reported as a clean win; it was also precisely why the feature does
not work.

**What unblocks it, in order** *(reviewed 2026-08-19)*:
1. The amplifier and bake pipeline settle (the other session owns this).
2. Wire `ChannelField` into worldgen output. **Costs a `kWorldGenVersion` bump
   (now 28, so 28→29), golden re-pins, and an HLSL mirror change in lockstep** —
   there is no free route, because "free" was exactly what not-wiring-it bought.
   *Still true: `ChannelField` is referenced only by `channel.h`/`channel.cpp`.*
3. ~~Basin detection → ponds and lakes.~~ **DONE, by another route.** The water
   re-architecture (2026-08-09 onward) ships the baked basin registry,
   `FBasinLedger`, lake sheets that rise/spill/drain, and a GPU PBF solver for
   near-field water; the baked river plane was retired from the near-field draw
   in Phase 5 (`7925cb6`). **Re-read `docs/water-architecture.md` before resuming
   W3 at all** — it may have removed the reason this is parked, or the need for
   it.

**Collision warning.** `origin/claude/erosion-v7` took the version-bump route
for drainage carving — 341 lines into `amplifier.cpp`, goldens re-pinned across
five files — and is **permanently unmergeable**. The other session touched
`amplifier.cpp` ×8, `worldgen.ush` ×5 and the **binary** `.spv` prebuilts ×4 in
24 hours; binary conflicts do not merge. Coordinate before entering worldgen.

**Also parked:** the `swe.h` §5 lateral-spill gap (an SWE-owned pool cannot
spill into a lower CA-owned neighbour) and ADR-0007's depth term. Both need
`kSweVersion` 1→2 and a re-pin of `0x61523E585CF7B782`; ADR-0007 argues for
deciding them together.

---

## 9. WATER BUGS FROM THE 2026-08-13 PLAYTEST

Reported by the owner while testing wind waves, ripples and throwables. Neither
is diagnosed; both are written down while the observation is fresh, with what is
already known that bears on them. **Both still open — confirmed 2026-08-19; no
commit since the playtest touches either.**

### 9.1 P1 — Digging a voxel UNDER water crashes the frame rate and unloads nearby water

**What was seen.** A single left click destroying one voxel beneath the water
surface "crashed game performance and slowed things down to a crawl", and at the
same time caused "a shader rendering issue with water such that near water around
the edit just unloaded or became transparent".

**Why it is probably one bug and not two.** An edit under water invalidates two
things at once: the terrain mesh for the affected chunk, and the water that was
resting on it. The near-field water and the lake sheet are separate draw paths
(docs/water-architecture.md), and the sheet's extent is derived from the baked
basin rather than from live geometry, so an edit can put them briefly out of
agreement. "Became transparent" is what water with no volume behind it looks
like -- consistent with the water surface surviving while whatever it was
occluding went away, or with an implicit-water brick being dropped and not
rebuilt.

**What is already known that bears on it.**
- The water CA owns mobilized bricks and `implicitFillAt` reads 0 there
  (VoxelWaterSubsystem.cpp, the ownership partition). A dig that mobilizes a
  region hands it from the implicit path to the CA, and that hand-off is the
  obvious suspect for both the stall and the disappearance.
- Edits are replayed from the edit log on load, so this is reproducible from a
  saved world rather than only live -- which makes it capturable at a pinned
  pose.
- The stall being immediate and severe points at synchronous work on the game
  thread, not at streaming: streaming shows up as a hitch that recovers.

**First moves, cheapest first.** Reproduce at a pinned pose with
`stat unit` and `voxel.Debug 1` up; watch whether DRAW or GAME time moves.
Then `voxel.Water.MeshImplicitLakes 1` (default 0) to see whether the near-field
voxel path is involved at all. Then the CA's own counters -- this project's rule
is that every stage emits a ran-flag, and the water CA has them.

### 9.2 P2 — The wave field's own crest speed has never been judged separately

The wind field's timescales were slowed 8-15x on 2026-08-13 (weather.h) after
the owner rejected the default three times. That addressed how fast the wind
CHANGES. It did not address how fast a crest TRAVELS at a fixed wind, which is
`omega0` in water_wave_graph.py and is currently 1.59 m/s for the 5 m base octave
against a real deep-water value of 2.79 m/s -- i.e. already slower than physical.
If the surface still reads as too fast with a frozen wind, the fault is not there
and the next suspect is `WindPatchDriftFrac` (0.5, so the calm/choppy patch field
sweeps at half the wind speed -- 2.5 m/s at the 5 m/s default, which crosses a
50 m view in twenty seconds).

---

## 10. ASSET PLACEMENT + ASSET FORGE — after the 2026-08-17/19 programme (merged, `aab8f54`)

54 commits. What landed is in the merge message and `docs/status.md`; this is
only what did **not**. Grouped, as this file is, by whose call each one needs.

### 10a. Needs the owner, not engineering

**CURATION HAS NEVER BEEN EXERCISED.** 828 species, all `approved` by the
grandfather clause, **zero human verdicts**. The gate ships (`354339b`), the UI
ships (Asset Library tab: approve/reject/draft plus per-seed toggles), and
`tools/library.py` reports the split. Nothing else can substitute — only the
owner can say which of a species' four baked seeds are worth placing. Best done
now that the 3D inspector shows an asset beside its placement contract.
*Cost: an evening of looking. Unblocks: a library that means something.*

**RAINFOREST AND INLAND ROCKS ARE ROSTER GAPS, NOT DENSITY BUGS.** Rainforest
carries 4 weighted tree species and measured 53.7 trees/ha — below grassland.
Inland rocks sit at 1.5–5/ha against the beach's 132. Per-biome density
deliberately did NOT touch either (`003b136`): they are starved, not saturated,
and thinning a starved biome makes it worse. The fix is authoring — weight more
species into those biomes, or generate them.

**TERRAIN DRAMA — HELD BY OWNER DECISION.** `docs/terrain-relief-measurements.md`
(`77aef42`) measured this world's 2 km wall score at **1,964 m max** (Milford
Sound is 1,683) and 10 km relief at 5,610 m — the amplitude ceiling is already
DEM-class. Two real gaps: **abundance** (world p99 10 km relief is 2,740 m
against that 5,610 max; 6 of 289 tiles exceed a 1,500 m wall; exactly ONE
sea-to-summit wall exists in the world) and **grammar** (slope p99 saturates at
50–58 degrees in every region measured — the checkpoint's own ceiling, so no
U-floors, no benches, no vertical faces). Levers can plausibly buy abundance;
only regional DEM conditioning buys grammar. `docs/dem-reference-library.md`
(`dcae423`) has 12 candidate regions with bboxes, licensing and three routes
priced. Paused by the owner until placement judgement finishes.

**BARE-ROCK-ONLY SPECIES NOW HAVE NO HOST.** v27 removed BARE_ROCK from dry land
(owner's call); species weighted only for it can never place. Re-weight toward
alpine/scree, or accept them as submarine-only.

### 10b. Blocked on something else landing

**VERIFICATION CAPTURES FOR THE LAST THREE LANDINGS.** All merged, none seen
in-world: the 192M pool default (`036552f` — every capture that night needed
`-VoxelPoolCapacityQuads`; the default should now carry the alpine vista's 173M
unaided), adaptive detail budgets (`e30bb46` — grep `VoxelDetailAssets: CONVERGED`
for time-to-first-full-ring against that night's 591–656 s, and the count of
`Hitch frame` lines must not rise), and the v28 per-biome densities in a
rendered frame rather than a census. Blocked only on the editor, which is free.

**GPU COARSE BYTE-PARITY HARNESS — WRITTEN, NEVER RUN.** `04dbdfe` documents the
exact procedure (CPU `FCoarseChunkGridSampler` plus `MeshChunkBricks` against
`RunRegionBlocking`; compare cells in dispatch columns 7..40, then quads through
`VoxelChunkQuadsIdentical`; levels 1–5, all four yaws). Byte parity is the
acceptance bar for coarse GPU composition and is currently asserted, not measured.

**PER-CLASS OVERLAYS ARE STALE.** `bake-out/biome-survey/` overlays predate the
flower/reed bank bake (`37591e0`), so they under-draw every biome by the 14–38%
that was invisible. Regenerate with `vxc_assetprobe --overlay <base>` then
`asset-forge/tools/place_overlay.py`.

### 10c. Cheap and self-contained

**`voxel-capture.ps1` DEFAULTS `-FineProviderId` TO EMPTY.** That default is the
fall-through that produced three fatal gate leaks in one night: the dir comes
from the command line, the id falls through to the ini, and the cross-product
has never existed on disk. `terrain-service/tools/resolve_fine_namespace.py`
answers it (rc 2 = nothing baked, rc 3 = ambiguous);
`docs/fine-tile-provider-identity.md` section 7 has the drop-in snippet. **Make
the dir/id pair atomic**: refuse a run that supplies one without the other.

**REDUNDANT COARSE COPIES, ~870 MB.** (Was: *~35 GB of stale tile namespaces* --
the fine-namespace half of that was done 2026-08-19, 21.29 GB -> 6.47 GB.) As of
2026-08-21 `D:/voxelsim/tile-cache` is the single authoritative root and holds
coarse + fine + superblocks together. `D:/vox-trunk-cache` and `D:/vox-wet-cache`
still each hold a byte-identical 289-tile coarse `s1` tier (435 MB each) plus
~20 superseded fine bake namespaces. Keep `...-b19d281fd` (bake_ver 28, current).
**Scan for junctions before any recursive delete** -- this repo has the scar
(`windows-junction-recursive-delete-hazard`), and `vox-wet-cache` in particular
holds many namespaces that older measurement docs still cite by path, so read
`docs/water-wet-country-*.md` before removing anything there.

**THE BIOME MAP SHOULD COME FROM `vxc_climateprobe`, NOT A SECOND REPO'S WEB
SERVICE.** `terrain-service/tools/world_map.py` needs the terrain-diffusion
explorer running (torch, diffusers, rasterio, infinite-tensor — eight dependency
clusters), produces a coarse *preview* whose percentages disagree sharply with
the engine's own census, and duplicates classification by parsing `biome.h`.
`vxc_climateprobe` already classifies every column through the engine's own
classifier off baked tiles; a colour-keyed PNG mode there would be
authoritative, service-free and unable to drift. Note the proper
`terrain-service/tools/worldmaps/` set (heightmap, biomes+provinces,
temperature, vista index) IS the standing deliverable and does work — it needs
`D:/terrain-diffusion/.venv`, which its README documents.

**FLOWER AND REED SPECIES NEED THEIR OWN CURATION PASS.** 477 banks were baked in
one command after being invisible for their entire existence — nobody has ever
looked at them.

### 10d. Larger, worth scoping

**COARSE-LEVEL GPU ASSET STAMPING IS PARTIAL.** The gather kernel ships and the
fork serves coarse chunks, but a grid too tall for span packing (SizeZ > 4095)
still falls back to the CPU, and `ValidateRegionRequest` refuses instances on any
level it does not implement.

**DETAIL ASSETS DO NOT REACT TO EDITS.** A dug-out column keeps its ground cover
until the group leaves the 256 m ring and re-resolves. Terrain-lattice assets get
this right through the overlay; cover accepting a staleness window is the v1
trade, recorded in `docs/detail-asset-rendering.md`.

**PER-BIOME WATER KIND IS CARRIED, NOT SERVED.** The schema accepts `water_kind`
per rule (the owner's "must be near FRESH water" case), but the bake-28 distance
plane is salinity-blind — it answers "how far to water", not "to what kind".
Same precedent as `water_max` before bake-28: the gate exists the day the channel
does.

**ANIMALS.** 382 species (bird 127, fish 106, quadruped 131, cetacean 18) place
correctly and are structurally excluded from rendering by owner decision — they
need animation first. `assetdetail.h`'s group scatter (herds, shoals, flocks) is
built and has no consumer. 251 of them still have no density source (the
PanTHERIA work covered mammals; birds, fish and cetaceans were blocked on a
web-search budget).

### 10e. The recurring failure mode, fourth instance

This programme lost the most hours to **an instrument measuring a world the
engine was not running** — four separate times: a probe on synthetic climate; a
veto reading an empty debug channel; the engine calling the old column binding in
five places while the probe called the new one; and a stale `vxc_gpu.exe`
reporting a *plausible* CPU/GPU divergence that was really 16-hour-old CPU code
against a current shader. The countermeasures shipped — engine-bound counters
with `--json`/`--compare`, trunk-anchor overlays, and `assetColumnChannelsAt`
living on the world object so no composition path can opt out by omission — but
the discipline is the fix: **`ls -la` the probe binary and `grep -c` the accessor
on both sides before quoting any number.** See the appendix above and
`voxelsim-instrument-must-run-the-engine-binding` in the session memory.

## 11. FRONT END (main menu + loading screen) — added 2026-08-22

Landed: a 1:1 clone of the Mira-Thal / *Voxelmark* front end as pure C++ Slate
in a new `VoxelEarthUI` module, with named saves behind CONTINUE and LOAD GAME.
See `docs/front-end-plan.md` for what to run and
`docs/adr/0009-slate-front-end-and-committed-ui-art.md` for why it is built the
way it is.

**Nothing below has been compiled or photographed.** The environment this was
written in has no UE 5.8 install, and CI cannot compile the module either
(`ue-build.yml` is gated off — 30 GB engine, 14 GB runner disk). The two lints
that DO run in CI pass. Treat the first item as blocking.

0. **Read `docs/front-end-local-verification-handoff.md` first.** It sequences
   the items below, names the four API shapes most likely to fail a first
   build, and flags one bug class (`DoesSupportWorldType`) that would compile
   silently and stop working — in the pre-existing `UVoxelWorldSubsystem` too.
1. **Build it, then run the capture set.** `tools\voxel-ui-capture.ps1 -Shot Menu`
   and its siblings. First contact with a compiler will find things; the module
   is ~3,500 lines of Slate that has never seen one.
2. **The colour probe (R1).** `FLinearColor(FColor)` decodes sRGB and Slate
   re-encodes on output; whether `#f0c14b` survives that round trip is an
   assumption. `voxel.UI.SRGBTint` A/Bs it at the single conversion site.
   **Nothing should call this port pixel-exact until this runs.**
3. **The gate-ring measurement.** `GateMaxRing = 3` is reasoned, not measured.
   `foreach ($n in 1..5) { tools\voxel-ui-capture.ps1 -Shot GateSweep -GateRing $n -MaxHold 180 }`,
   into `docs/measurements/front-end-gate-<date>.txt`. Until then the constant's
   comment says "hypothesis" and should keep saying it.
4. **The non-regression diff.** A world capture taken through
   `-VoxelMenuAutoStart` against the same capture without the front end, at the
   documented within-session noise floor. This is the one that protects the
   archive.
5. **Font metrics (R2).** Macondo at 84 px will not lay out identically in
   Slate and Godot. Expect a few pixels in title width and vertical centring;
   `Config/DefaultVoxelUI.ini` is where to correct it without a rebuild.
6. **Two API forms a read-only audit could not settle**, both cheap to check
   the moment there is an engine to check against. `FSlateDrawElement::MakeLines`
   is passed `TArray<FVector2D>` (`SVoxelHourglass.cpp`) on the grounds that
   the float-precision overload is the newer of the two; and
   `UWorldSubsystem::DoesSupportWorldType(const EWorldType::Type)` is the
   signature both `UVoxelFrontEndSubsystem` and the pre-existing
   `UVoxelWorldSubsystem` override. If the latter has been superseded by the
   `const UWorld*` form, both still COMPILE and both silently stop being
   called -- so it is worth one look, and if it is wrong it is wrong in the
   older file too.

**Deferred, deliberately, with their seams left open:**

- **Pause menu.** The seam is `EVoxelFrontEndState::Playing`; it needs a
  `Paused` state, an Escape binding on `AVoxelEarthPlayerController`, and an
  `SVoxelPauseMenu` reusing `FVoxelUIStyle` and `SVoxelMenuButton` unchanged.
  Its SAVE dialog binds to the existing `VoxelSave::Write`. Until it lands,
  saves are created through `voxel.SaveGame`.
- **Settings screen.** Currently a placeholder panel shaped like HELP and
  CREDITS. The Godot original persists to two files (`settings.json` and
  `graphics.json`); consolidating them is worth doing at the same time.
- **Menu music.** The three source WAVs are ~124 MB and one exceeds GitHub's
  per-file limit. The handoff hook is stubbed; the contract is adopt at
  BeginLoad, fade to −40 dB over 1.5 s at hand-off.
- **Per-save seeds.** `Seed` is baked into the amplifier in `Initialize`, before
  `Impl` exists, so a save from another seed cannot be opened — it is listed as
  unloadable with the reason shown. Lifting this means extracting a
  `ConstructImplForSeed()` out of `Initialize`, which also unlocks a seed picker
  on NEW GAME.
- **The Mira-Thal branding.** The menu says VOXELMARK and its tips describe
  Roland, Lethe's Draught and the Aelorin. That was the explicit 1:1 brief; all
  of it is in `VoxelUIStrings.cpp` so re-authoring is a single-file edit.
