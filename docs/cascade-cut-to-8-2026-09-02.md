# The cascade cut to 8 rings, the clipmap's return, and the two bugs still open

Owner directives executed this session: cut the 11-level cascade to 8 rings
(R0..R7, 8,192 m), kill R8-R10 code paths, restore the clipmap beyond, make it
READ AS VOXELS (virtual-ring voxelization, v2 approved), fill all rings in
< 60 s. The 65 km all-voxel experiment is over; VoxelCoords.h's kNumLevels
comment carries the short version of why.

## Shipped and verified

* 8-level constants everywhere, interlocked by static_asserts (a half-done
  edit does not compile). VisBuffer keeps the 4-bit split level field.
* Clipmap spawns by default again; -VoxelNoClipmap suppresses (control arm).
  Ocean plane sized to the CLIPMAP's 65.5 km reach, asserted against
  AVoxelClipmapActor::NumLevels -- not to the cascade, which would strand
  water at 10 km under 65 km of terrain.
* Virtual-ring voxelization in M_VoxelClipmap (create_clipmap_material.py):
  cell = 12.8 m at the seam doubling per band, terraced steps, axis-quantized
  normals, per-cell jitter mosaic + edge grid (v2), marcher-ambient parity.
  -VoxelClipmapVoxelize=0 = smooth control. OWNER APPROVED v1 > smooth, v2 > v1.
* Fill throughput: MaxAppliesPerFrame 192 -> 768; full ~58k-chunk fill in
  ~30 s (was ~147 s). AtlasPrefetchAhead default 8 (engagement unproven on
  parked legs -- prefetch fills the predicted-anchor crescent; verify moving).
* Three latent lifts: worklistCoarseScale's ladder ended one level short
  (L7 got scale 64); the worklist "malformed" test flagged healthy R7 records
  (>6u); frame-origin snap was 1024 voxels ("aligned at every level" -- false
  for L6/L7's 2048/4096-voxel chunks) -> now derived, 4096.
* NEW INSTRUMENT: per-ring-level HIT census ("Voxel march hits byLevel"),
  the hit-side twin of the miss attribution. This is what finally localised
  the gap; keep it forever.

## OPEN BUG 1 -- R6/R7 land EXACTLY ZERO hits (the gap ring)

Plan view from 8,000 m: solid voxel disc to EXACTLY 2,048 m (the R5|R6 seam),
nothing to the clipmap at 8,192 m. hits byLevel: L0=0 L1=9 L2=15k L3=48k
L4=188k L5=658k L6=0 L7=0. Chunks resident (L6~5k, L7~2k entries), reach check
passes (8 rings, budget 3328, worst ring 1109), capRays=0, stale 0% at ground.
The kill is INSIDE the R6/R7 segment walk. The U-shape (L0/L1 also ~dead at
steep poses) and the TOPOGRAPHIC disc rim (owner-spotted: the rim follows
terrain height => reach is a RAY-T SPHERE of 8,192 m, not an XY cylinder;
also why terrain below vanishes above ~10 km altitude) are constraints any
mechanism must explain. A Fable trace of VoxelBrickTraverse.ush's segment
interval derivation is the active lead.

FALSIFIED ON THE WAY (all engagement-proven): vertical keep (twice), air-skip,
admission cutoff/refill quantization (partial only; the 5 s parked backstop
shipped but its gate FAILED and it has no engagement counter -- treat as
unproven), rect-bound admission (kept: correct on its own terms), coarse
fallback, kFirstCoarseLevel routing, ocean occlusion, clipmap occlusion,
step-budget caps, snap alignment (fixed, necessary, insufficient).

## RESOLVED -- the gap ring / dead vista: an unchecked DECLINE SENTINEL in the rect-bound admission fix

The whole chain, end to end: the rect-bound rewrite of ComputeFootprintChunkZRange
called surfaceLower/UpperBoundMm WITHOUT checking their documented decline
sentinels (kSurfaceBoundDeclined = INT64_MAX / lower = INT64_MIN; core.h's own
comment warns of "a caller that forgets to check"). The bounds decline on
oversized footprints -- L6/L7's 204.8/409.6 m rects, while L5's 102.4 m stays
under -- so EVERY outer-ring chunk was admitted with a sentinel-derived Z in
the billions (measured on real keys: Z = 1,030,792,138), filed in the index
torus at (garbage & 127), and the marcher, asking at the real altitude, never
found one in 10^8 probes. Fix: check both sentinels, corner-sampling fallback
(cannot decline). Verified same leg: L7 keys Z=3..6, slots 6/7 real fetches
12.9M/21.8M (their first ever), vista hits 0 -> 7,976,620, image continuous
from near voxels through the band into the voxelized clipmap.

HOW IT HID: it shipped inside the fix for the ORIGINAL swiss cheese, one build
before any instrument could see it -- no printer showed index keys, index slot
counts above 5, per-level hits, or per-slot fetch verdicts. All four
instruments now exist (plus -VoxelDumpIndexKeys), and each would have caught
it in minutes. Four 7-era short printers were found on the way; count
initializers by hand, and print EVERY slot, always.

THE INSTRUMENT LADDER THAT CRACKED IT (keep this method): per-level hit census
-> localised the dead levels; per-slot fetch verdicts -> separated benign
torus-alias stale from the real signal; index-key dump -> showed the garbage Z
in one line. Three instruments, each an order of magnitude tighter.

## SUPERSEDED -- the deterministic-dead-vista section (kept for the method lesson)

Late finding that reframes the night: on the current binary the vista pose
(1,500 m / pitch -7) draws NOTHING -- hits=0, deterministic across identical
reruns (CAP-AI/AJ) -- while the plan pose (8,000 m / -89) counts millions on
the SAME binary (CAP-AB). Eliminated by legs: ZTight (off), the clipmap
(suppressed -- still 0), the hit-census instrumentation (fully reverted --
still 0). The bisect SELF-CONTRADICTED: the only build inside the bracket was
exonerated by full revert, so either an earlier non-zero reading was misread,
there are two overlapping causes, or a non-code state changed (the material
regen chain ran twice in the bracket window, incl. a module relink, and
RT_VoxelRipple assets were deleted twice). Hit history at the pose:
2.6M -> 10.3M -> 820k -> 0 forever. A fresh-context trace agent is running a
mechanical first-divergence diff of the alive-vs-dead logs.
LESSON RECORDED: run the determinism pair BEFORE any bisect.

## OPEN BUG 2 -- ZTight wedges when armed at boot

voxel.March.ZTight armed in-session: 256 -> 65 ms frame, 15x fewer probes,
stale 70.8% -> 0.00% (CAP-S). Armed as compiled default OR +10 s defer:
hits=0 for entire legs, non-recovering (CAP-T/U/V). Default returned to 0;
legs arm it explicitly. Root cause unknown; the S-vs-T/U/V delta also spans
the applies-768/prefetch-8 defaults build, and CAP-V's discriminator flags
did not provably apply. CAP-AC re-tests the cvar path on the newest binary.

## Why the editor feels permanently heavy

Nothing streams after the first minute; the load is the marcher re-walking
every ray through kilometres of ABSENT space each frame (a hit ends a ray
early -- a filled world is a cheaper world). Both open bugs feed it: the gap
keeps rays walking, and the wedge keeps the Z-slab gate off-default.
In-session relief today: `voxel.March.ZTight 1` in the console.

## Standing acceptance test (owner's words)

"LOAD ALL LOD RINGS": the plan view (SpawnAltM 8000, pitch -89) must show
voxels to the clipmap edge with no gap ring, at a frame cost that holds.

## The checkerboard residue: mis-keyed L7 brick jobs (RESOLVED, 2026-09-02)

After the sentinel fix and quadrant recursion, the owner still saw missing
surface squares (blue lake-sheet squares at 1500 m; dense flat grey slabs at
2600 m). The instrument ladder that cracked it, in order, each leg falsifying a
theory:

1. **Presence bitmaps** (`-VoxelDumpIndexKeys`): every L6/L7 column through the
   annulus held ≥1 resident record — not missing columns.
2. **Surface-completeness AUDIT**: per-column fresh
   `ComputeFootprintChunkZRange` vs memoized range vs per-Z `ChunkRecords`
   residency. All clean (L6 1,281 / L7 1,257 columns, 0 missing, 0 memo
   mismatch) — streaming admission COMPLETE; ruled out the scan-revisit,
   stale-memo, and wrong-bound classes in one leg. (The census `byReason
   never` at this pose is dominated by above-horizon sky rays crossing
   un-streamed air cells and is NOT a terrain-hole signal.)
3. **Counter divergence**: pool `perLevel` L6 = 14,910 vs index
   `slot6Entries` = 14,435. Counters lie here (phantom pool slots), so:
4. **RECON probe**: per record at L3..L7, `FindChunkSlot` (the brick pool's
   authority — NOT the record's `PoolSlot` field, which is the quad-path pool
   and INDEX_NONE on the marcher path) vs the index cell via `DebugProbeKey`,
   with `DebugFindSlotOwner` reverse lookup. Verdict: L6 stolen=439..485, and
   every stolen cell's owner sat at exactly (X+128, Y+128, Z) — keys in the
   L7 annulus coordinate range, filed as L6.

**Root cause**: `VoxelGpuMeshJobManager.cpp` keyed brick jobs with
`Clamp(BrickRegion.CoarseLevel, 0, 6)` — a stale ceiling from the 7-level
world (its own comment records the identical bug shipping once before as a
stale 5). Every L7 GPU brick job was keyed L6 with L7 coordinates: ~3,000
phantom L6 pool entries, aliasing onto true L6 index cells 128 chunks away
(`&127` collision) and overwriting them. Rays drew flat 26-km-distant content
over the near rings — the slabs. The true L7 keys read no-geometry (3,677
records), so the far ring was starved too. Validation missed it because the
`CoarseLevel > 6` refusal only applies to regions carrying asset instances,
and content SCALE was already derived correctly, so the world was plausible
with no error anywhere.

**Fix**: all three stale ceilings now derive from
`FVoxelMarchChunkIndex::kLevels - 1` — `BrickKey.Level`, the asset
`AnchorShift` clamp, and the `VoxelGpuVerify` gate ceiling (which had been
leaving L7 unprovable).

**Verified to the digit** (CAP-AW-KEYFIX, kept spawn -56940,-56610, 2600 m,
pitch -4): records = ok = pool = index at L6 (11,598) and L7 (9,988); stolen
0; noGeom 0; real L7 marcher fetches +39%; the slabs gone at the identical
pose. Owner verdict pending on the capture.

The AUDIT and RECON probes stay in the `-VoxelDumpIndexKeys` block: RECON is
the standing instrument for the whole "pool and index disagree" class.

## ZTight: wedge autopsy closed, DEFAULT ON (2026-09-02 evening)

The cold/deferred-arm wedge (CAP-T/U/V: armed-by-default legs drew nothing,
hits=0, non-recovering) is closed WITHOUT a ZTight change: it was the two
index-corruption bugs above (admission sentinel; L7-keyed-as-L6 brick jobs)
starving the per-level resident-Z slab reduction. On the fixed binary the
same deferred-arm path (CAP-AY-ZTIGHT, cvar 1 at boot + the 10 s arm defer)
renders a full correct vista, pixel-comparable to the off arm.

`voxel.March.ZTight` default 0 -> 1. The 10 s arm defer is KEPT (the proven
timing; arming at t=0 exactly has never been proven and the first seconds are
fill anyway). The A/B saving this ships: 256 -> 65 ms at the vista.

Measurement note for future perf reads: the capture legs' `frameMs=` lines
only print frames over the 33.3 ms threshold (a hitch log) and the editor's
background throttle pins idle frames at exactly 400.00 ms -- neither is a
steady-state FPS statement. Steady-state numbers come from -VoxelPerfRun's
JSON via tools/check-perf-run.py / tools/voxel-perf-gate.ps1.

## Evening wave: altitude fixes + instrumentation debt + honest perf (2026-09-02)

Three parallel lanes, one build, all verified on-box:

**Altitude (marcher)**: per-axis march-volume Z (half-extent 13,516.8 m, XY
unchanged) + ladder-on-truncation with a per-rung vertical clamp
(4088 voxels/level — also closes a latent unbounded 13-bit Z pack wrap).
Verified: CAP-AZ-DOWN (2.6 km straight down, previously truncating) uncovered
= 0.0000%; CAP-BA-PLANVIEW (10 km plan) full disc, smooth cover beyond rings;
CAP-BB-VISTA unchanged vs the approved shot. `substituted` at altitude is
high BY DESIGN now (depth draws coarser); its falls-to-zero reading applies
at ground poses only. zladder census words exist but the capture legs do not
print the stats block — grep gap, use voxel.March.Stats or HoleStats fold.

**Instrumentation**: promoteExit split into five truthful counters summing to
ticks (falsifier warning wired); admit->visible p50/p95/max window line with
unstamped=0 proof; -VoxelRecomputeCensus emitter matching the existing
reader. check-perf-run.py exit-code contract fixed (vacuous pass -> exit 2,
inadmissible-timing and frameCount=0 guards); tools/voxel-perf-gate.ps1 is
the one-command post-leg gate; CI job wired into .github/workflows/ci.yml.

**Perf, steady-state (FLIGHT-POSTFIX, 20 m/s line flight at the kept spawn,
120 s, 28,301 frames, rendered 1280x720, ZTight default-on)**:
p50 8.78 ms (~114 fps), p95 11.93 ms, post-warmup p95 11.67 ms (~86 fps),
post-warmup hitches 48/28,062 frames (0.17%), 638,780 chunks loaded,
avg 5,323 chunks/s, admit->visible p50 0.04 s while cruising.
Gate: PASS at the 33.3 ms M1 bar. Goal 3 (>100 fps moving = p95 < 10 ms)
is 1.7 ms away. Parked-vista history: 246 ms (pre-fix, ZTight off) -> this.

Remaining: clipmap seam softening (owner-judged, editor-bound); Goal-3 gap.

## Goal-3 pass: the apply-burst lever is a NULL; the gap is a product decision

Fresh CSV GPU attribution (GOAL3-ATTRIB, shipping defaults, kept spawn,
20 m/s): steady GPU 7.75 ms (marcher 5.04, TSR 1.26); the FAST->SLOW step is
+2.24 ms GPU of which ~72% is the worldgen kernels (WlVoxelize +0.67,
WlColumn +0.63, PoolWrite +0.13), and slow frames applied 247 chunks vs 19.

Three iterations of apply smoothing followed, ending in a PROVEN null:
  v1 backlog/6 spread -- never engaged (smoothCap=0): a crossing is a
     standing river (backlog 600-4,100), not a one-shot burst;
  v2 rate cap 64/frame, storm bypass 2,000 -- never engaged: the waves
     exceed 2,000 and took the bypass (componentsApplied=768);
  v3 storm threshold 10,000 (cold fill runs ~90k) -- ENGAGED FULLY
     (smoothCap to 247/window, applies capped at exactly 64, throughput
     held at 5,323 chunks/s)... and p95 DID NOT MOVE (11.65 vs 11.55).

Conclusion: the apply burst CORRELATED with slow frames but did not cause
them. The worldgen GPU cost rides the mesh DISPATCH batches upstream, and
the p95 frame is GPU-bound end-to-end: marcher 5.7 + streaming 1.9 + TSR
1.25 + scene ~1.1 = 10.0 ms GPU at SLOW. Frame p95 < 10 ms therefore needs
GPU p95 ~8.5, and no streaming-side smoothing can produce it while the
marcher holds 5.0-5.7 ms -- the ray-count lever (internal resolution /
upscale ratio) is the remaining honest path, and it is an owner product
decision (voxelsim-marcher-cost-is-ray-count: cost is ray-linear within 2%,
per-ray knobs spent).

The v3 rate cap SHIPS (cruiseCap=64, stormBacklog=10,000): it engaged at
zero measured cost, bounds worst-case single-frame applies 768 -> 64, and
its engagement counter (smoothCap) plus armed line keep it honest. All legs
rendered 1280x720 internal (harness GameUserSettings) -- the owner's real
config is somewhat heavier; treat these p95s as lower bounds.

Standing at close: p50 8.8 ms (~114 fps), p95 11.5-11.7 ms (~86 fps),
hitches 0.15%, fill <60 s intact, admit->visible p50 0.04 s.
