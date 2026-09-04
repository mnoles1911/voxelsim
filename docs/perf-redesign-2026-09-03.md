# In-game FPS: the post-resolution portfolio

## Context

Owner directive: raise in-game speed/FPS by refactoring or redesigning pipeline
parts. Resolution reduction is OFF the table (owner decision, twice). Honest
current state at the owner's real config (2560x1440 out, TSR from 1552x873,
ZTight on, cruise apply cap on), flying 20 m/s at the kept spawn:
**p50 10.8 ms (~93 fps), p95 14.2 ms (~70 fps)**, hitches 0.2%. The p95 frame
is GPU-bound end-to-end. The 720p-era decomposition (never yet repeated at
owner res): marcher ~65% of steady GPU; the p95 step is ~72% streaming worldgen
kernels (WlVoxelize/WlColumn/PoolWrite) + ~29% marcher. Goal: p95 ≤ 10 ms
(>100 fps) without touching resolution. Fog stays disabled in legs (owner:
"fog off / don't care").

## The graveyard (verified constraints — do not re-propose)

- Per-ray cost is immovable; cost is ray-count linear ±2%. Per-STEP tests never
  pay (segments are 20-40 cells). No emptiness may be inferred from RESIDENCY —
  sky air is resident-with-records since the buried-skip retirement
  (docs/rungprobe-refuted-2026-08-29.md: 8.43% skip vs 95% predicted, 30x
  slower). No skipping the retry ladder on level-L evidence.
- Refuted with receipts: beam pre-pass, 4^3 in-loop block occupancy, sky-mark
  tStart, ZCut source swap, RungProbe, apply-side burst smoothing (v3 engaged
  fully, p95 unmoved — the correct layer is the GPU dispatch side).
- voxelsim-marcher-cost-is-ray-count and rungprobe-refuted are the authorities;
  check any new idea against them before building.

## Phase 0 — Map the real frame (one leg, half a day)

Run the CSV GPU attribution at the OWNER config for the first time (all prior
CSV legs were 720p): `tools\voxel-run-flight-leg.ps1` (now defaults to
2560x1440 + `-VoxelForceInternal=1552x873`) with `-csvGpuStats` +
`voxel.DeferExec 112 CsvProfile FRAMES=9000` per the recipe in
docs/gpu-tail-split-2026-08-27.md §8, then `python tools/csv-gpu-attrib.py`.
Deliverable: FAST/SLOW table at owner res — sizes TSR-at-1440p (predicted
2.5-4 ms vs the 1.26 measured at 720p output), the marcher (~7.4 predicted),
and the streaming step. **Every later phase's expected win is re-sized against
this table before building.**

## Phase 1 — Quick arms (days; each ships or dies on one A/B leg)

1. **Arm the built-but-never-flown all-air bit** (`voxel.March.IndexAnySolid`,
   VoxelMarchChunkIndex.cpp:289; refine kernel VoxelMarchIndexRefineMain
   already exists with audit + poison red-arm). Protocol per its own doc block:
   poison leg first (`voxel.March.IndexAnySolidPoison` must produce
   wrongClear>0 AND a broken capture), then audit leg (wrongClear=0 over
   nonzero checked), then flight A/B at owner config. Engagement proof:
   census word cellIndexEmpty leaves zero for the first time. Expected
   +0.2-0.4 ms steady (its own doc caps the expectation — the 73%-wasted-fetch
   headline is MALL-cached, not DRAM). Ship default-on if clean.
2. **TSR diet A/B** (config only, no voxel code): candidates from
   ue-project/Config/DefaultEngine.ini — `r.TSR.ThinGeometryDetection 1→0`
   (voxel terrain has almost no thin geometry), `r.TSR.History.SampleCount 32→
   lower`, plus engine-default cost knobs (R11G11B10 history, rejection AA
   quality) sized by Phase 0's real TSR number. One flight leg per candidate
   set; owner judges a same-pose capture pair for image cost. Expected
   0.5-1.5 ms if TSR at 1440p is as heavy as output-pixel scaling suggests.

## Phase 2 — Async-compute overlap (the p95 step killer; ~1 week)

**No pass in this codebase uses async compute** (grep: zero ERDGPassFlags::
AsyncCompute in Source) — the streaming worldgen/mesh kernels serialize with
the marcher on the graphics queue, and they ARE the p95 step (+1.9 ms, 72%).
Move the worldgen compute chain (voxelize/column/classify/pack — the passes
enqueued from VoxelGpuMeshJobManager.cpp DispatchBatch :4519) to
`ERDGPassFlags::AsyncCompute` so it overlaps the marcher.

Why the semantics allow it: a meshing chunk is INVISIBLE to rays until its
index delta lands, and "the index learns about the chunk on the next Flush" is
already the shipped contract (VoxelBrickPool.cpp P3 block) — late completion
is indistinguishable from a slow worker. What must STAY on the graphics queue,
ordered against the marcher: the index delta upload, the free-pass record
zeroing, and the pool-write scatter into live arenas (these touch state rays
read). Design rule: async passes write only into not-yet-published allocations;
publication stays synchronous. RDG cross-queue transitions handle fencing;
budget one day for hazard-validation with `-rdgdebug` + the RECON probe as the
corruption tripwire (it reconciles records↔index↔pool to the digit).

Gates: p95 down with the SLOW-bucket streaming terms moving under the marcher
in the Phase-0-style CSV rerun; RECON stays 0/0/0; fill time unchanged; image
byte-comparable at the kept poses. Fallback if hazards prove nasty: **cruise
dispatch governor** — halve the promote batch cap when the results backlog is
cruise-sized (mirror of the shipped apply rate-cap at the correct layer; the
five split promoteExit counters make its engagement provable).

## Phase 3 — Temporal ray priming (highest ceiling, owner-approved risk)

Per-pixel: reproject last frame's hit distance (the marcher already writes
depth; camera delta known), start this frame's march at reprojected-T minus a
safety margin of k chunks, instead of walking the air column from the camera.
Legality vs the graveyard: not per-step (a per-RAY start bound), not
residency-derived (the renderer's own last-frame hits), ladder untouched.
It attacks exactly the measured cost center — segment ENTRY (~0.9 ms/ring at
sky, x14/ray): rays that prime past whole rings never pay their entries.

Correctness protocol (all default-off until every gate passes):
- The prime is a HINT: if no hit lands within a short window past the prime,
  RE-WALK the full segment from its true start and count it
  (`primeMissRewalk` census word, append-only discipline in
  VoxelMarchRenderer.h). Disocclusion/invalid reprojection → normal walk.
- Sky pixels (no prior hit) keep the ZTight path — priming is terrain-pixel
  only, which is where the down/horizon cost lives.
- Image gates: the established noise-floor A/B (same-pose captures, diff under
  the measured same-config floor), the moving pose-matched captures
  (tools/voxel-moving-capture.ps1, 2-6 cm pose match), and the standing
  SKY AND DOWN timing poses. tailLost-style regression counter must stay 0.
- Expected if it survives: 20-40% off the marcher at cruise (~1.5-3 ms), the
  only lever that big left. If it fails its falsifiers, it goes to the
  graveyard with receipts like everything else.

## Explicitly deferred (report only)

- Game-thread p99 tail: recompute split (fires 11% of frames, +3.7 ms) and the
  atlas prefetch whole-column scan — hitch smoothness, not p95; next wave.
- Fog cost: out of scope (owner).
- The marcher's missing shadows/GI, jitter (image quality, separate track).

## Files (by phase)

- P1: VoxelMarchChunkIndex.cpp (cvar default flip if shipped),
  ue-project/Config/DefaultEngine.ini (TSR).
- P2: VoxelEarthShaders/Private/VoxelGpuMeshJobManager.cpp (pass flags, queue
  split), VoxelGpuWorldGen*.cpp (pass flags), VoxelBrickPool.cpp (publication
  ordering audit only).
- P3: VoxelMarch.usf + VoxelBrickTraverse.ush (prime + rewalk),
  VoxelMarchRenderer.cpp/.h (prev-depth binding, census words, cvar).

## Verification (standing discipline)

Every arm: engagement proof before timing (a counter that can fail), image
before timing on renderer changes, sky+down poses for marcher gates, one
change per leg, `tools\voxel-perf-gate.ps1 -MaxP95Ms 10` as the finish line.
Composite target: 14.2 → ~9-10 ms p95 (TSR ~1 + async ~1.5 + anySolid ~0.3 +
priming ~2), each number a hypothesis its own leg confirms or kills. The plan
ships to docs/perf-redesign-2026-09-03.md at execution start (owner's standing
deliverable preference), and SCOREBOARD.md records every verdict including
nulls.

---

## Phase 0 RESULT (P0-OWNERMAP, 2026-09-03, view=1552x873 -> 2560x1440 verified)

FAST (steady) GPU = 9.95 ms of an 11.9 ms p50 frame:
  marcher 6.83 (69% of GPU) | TSR 1.39 | streaming 0.36 | scene misc ~1.4
FAST->SLOW step = +1.87 GPU (frame p95 15.2 on this leg; CSV overhead inflates
~0.7 vs the clean 14.2):
  marcher +0.94 (51%) | WlVoxelize+WlColumn +0.82 (44%) | rest crumbs
Alignment caveat: r=0.920 (<0.95), so SLOW splits are directional.

RE-SIZED CEILINGS: TSR diet ~0.5 ms max (1.39 measured, not 2.5-4 predicted);
async overlap ~0.9 ms at p95 (step shrank at owner res); anySolid 0.2-0.4;
**temporal priming is now the headline — the marcher is 69% of steady GPU**,
20-40% of 6.83 = 1.4-2.7 ms. Composite honest estimate lands ~10.5-11.5 p95
unless priming hits its high end; the portfolio order stands.

---

## Phase 2 design detail (locked before implementation)

Async candidates (generation only, write into unpublished allocations):
WlColumn -> WlVoxelize -> WlClassify -> WlStamp (the Rg* single-region chain
equivalently). STAY GRAPHICS-QUEUE, ordered before the marcher: WlClaim (arena
claims), PoolWrite (scatter into live arenas), the index delta upload, the
free-pass record zeroing. The seam is the claim: generation produces into
scratch/private ranges; the claim+write is the publication moment and stays
synchronous. If the current chain generates DIRECTLY into claimed arenas (no
scratch), the split moves one pass earlier and PoolWrite alone publishes --
resolve at implementation from AddRegionPasses' buffer wiring.

## Phase 3 design detail (locked before implementation)

- The marcher keeps its OWN previous-frame hit-distance buffer at internal res
  (1552x873 x fp32 ~= 5.4 MB), written at hit time (sky writes a far
  sentinel). No engine-history coupling.
- Prime lookup per ray: T_guess = prev same-pixel T; prevUV = project(prev
  view-proj, camPos + dir * T_guess); sample prev-T there with a 3x3 MIN
  gather (min-filter is the silhouette guard: at depth edges the prime
  falls back toward the NEARER surface, the conservative direction).
  Invalid prevUV / sky sentinel / teleport (origin snap jump) -> unprimed.
- Start = primed T minus a margin of 2 chunks at the owning ring's level.
  World is static and camera-only motion, so overshoot arises only from
  reprojection error; the min-gather + margin bound it.
- MANDATORY REWALK: a primed ray that reaches its segment end without a hit
  re-walks from the segment's true start (counted: PrimeRewalks census word,
  append-only). A primed ray that hits is trusted -- the hit is real geometry;
  the only false state a prime can create is a MISS, and the rewalk closes it.
  (A hit BEYOND a skipped nearer surface is prevented by the min-gather +
  margin; the image gates are the final arbiter.)
- Gates: DECISIONS NOT NANOSECONDS -- VoxelMarch ms at SKY AND DOWN AND
  HORIZON from ProfileGPU/CSV; noise-floor same-pose image A/B; pose-matched
  moving captures; PrimeRewalks small and PrimedRays large (both must move);
  uncovered/substituted must not move against control.

## Phase 2 implementation note (2026-09-03, code landed, unbuilt)

Implemented behind `voxel.GPU.AsyncGen` (default 0, byte-identical off). One
deviation from the locked design's letter, forced by the engine and worth
knowing before validating: "RDG cross-queue transitions handle fencing" is
true but per-BUILDER -- FRDGBuilder finalises every external buffer back to
the graphics pipe at its own epilogue (RenderGraphBuilder.cpp,
AddLastBufferTransition), so AsyncCompute passes in the standalone flush
builder would be fence-sandwiched between scene N-1 and scene N even WITH the
one-tick defer: a guaranteed null. The landed shape therefore stashes the
generation window and injects the five Wl* stages -- AsyncCompute -- into the
SCENE renderer's own graph via a post-opaque render delegate (fork at
prologue, overlap spans the frame incl. the marcher; the scene epilogue join
costs max(0, gen - scene) and hands the arenas back to graphics for free).
Publication defers one flush exactly as designed: the Claim runs at the HEAD
of the next worklist flush graph, reading the ring/control/args state the
previous flush left (pass-add order closes ring-wrap and arena-reuse; arenas
stay single-buffered). Index adds and frees of claim-pending slots defer with
it (pool-side hold + cancel-on-death). Engagement: the `[gpu-worklist]
asyncGen` window line (def/scene/serialFb/idxDef/freesHeld/deferMisses) plus
the one-time EFFECTIVE=0/1 arm log. RECON: stolen/marked stay hard zero;
`empty` may transiently show up to one promote batch mid-flight and must
drain at quiescence (comment on FVoxelBrickPool::DeferGpuIndexAddForPendingClaim).

## Phase 1 log

- Poison (P1-ANYSOLID-POISON): wrongClear=24,322,128 = checked exactly;
  cellIndexEmpty fired 239M (first time ever); uncovered 39.8% -> 56.2%.
  The audit CAN fail, loudly, and the image agrees. RED ARM PASSES.
- Audit (P1-ANYSOLID-AUDIT): checked=24,183,522 wrongClear=0;
  cellIndexEmpty=169M (engaged); uncovered 39.83% = pose baseline (terrain
  intact). Refine kernel conservative as documented (cleared only 16,899
  proven-air cells; refused 291M uncertain). GREEN ARM PASSES.
- Flight A/B: pending (P1-ANYSOLID-FLIGHT vs control OWNERRES-1440D
  p50 10.807 / p95 14.225, same binary+pose+harness).

- Flight A/B (P1-ANYSOLID-FLIGHT): p50 10.635 (-0.17) p95 13.995 (-0.23) vs
  control 10.807/14.225 -- inside the predicted 0.2-0.4 band, consistent both
  percentiles. SHIP default-on (flip rides the next build).
- TSR ThinGeometryDetection 0 (P1-TSR-THIN0): p50 10.392 p95 13.754 -- a
  further -0.24 both percentiles (~17% of TSR's 1.39). Image sent for owner
  verdict (VoxelVerify00654); tree-close caveat stated. Ship on verdict only.
- Phase 1 running total: p95 14.225 -> 13.754 (-0.47) from two cheap arms.

## Standing policy (owner, 2026-09-04)

Every VISUAL-trade toggle this programme produces ships as a player-facing
row in the SETTINGS panel (pattern: VoxelGraphicsUserSettings + a
BuildSettingsPanel row), with the owner's session verdict as the default.
Internal scheduling changes with no image difference (e.g. async overlap) do
not get rows. Shipped so far: Fine Detail Smoothing (default OFF). Queued on
its gates: Temporal Ray Priming (row lands with its ship decision).

## Gate 2-3 verdicts (2026-09-04 late)

Gate 1 control (new build, arms off): 10.282/13.628 -- no refactor drift.
Gate 2 AsyncGen: EFFECTIVE=1; def=8,927 scene=19,337 serialFb=0 deferMisses=0;
RECON digit-identical (stolen/empty/marked 0, ok counts exact); flight
10.339/13.43 (p95 -0.20). SHIPS.
Gate 3 TemporalPrime: vista engagement primed=7.5M/window rewalks=0.19%,
uncovered/substituted unmoved, image clean (00662); DOWN pose primed=69.7M
rewalks=0 uncovered=0.0000%, image clean; falsifier VACUOUS (HeightPyramid
consulted=0 -- the arm it rides never engaged; likely Init-latched, noted
honestly, primary gates stand); full-stack flight (async+prime):
**p50 9.495 / p95 12.284** -- prime worth -0.84/-1.15 under motion.

EVENING SCOREBOARD (owner config, 20 m/s):
  baseline            10.807 / 14.225
  + anySolid          10.635 / 13.995   (ships, default flipped)
  + TSR thin off      10.392 / 13.754   (ships, Settings row live)
  + AsyncGen          10.339 / 13.43    (ships pending default flip)
  + TemporalPrime      9.495 / 12.284   (owner verdict pending -> row)
Net: p50 -1.31 ms (~105 fps median), p95 -1.94 ms (~81 fps). Goal 3 gap
now 2.3 ms, all marcher.

## SHIPPED (owner approval 2026-09-04: "I approve all of your work made this
## far. Screenshots looked good")

- voxel.March.TemporalPrime default 1 (ship record at the cvar).
- voxel.GPU.AsyncGen default 1 (ship record at the global).
- Settings row 2: "Faster Terrain Drawing" (VoxelGraphicsUserSettings +
  SVoxelMainMenu), default ON, persisted, applied at boot.
- Shipping configuration now = the full evening stack: p50 9.5 ms
  (~105 fps median) / p95 12.3 ms (~81 fps) moving at the owner's config.
- Builds ride the clipmap-v3 lane's next build (concurrent, disjoint files).

## Clipmap voxelization v3 (2026-09-04, owner-requested rider)

Geometric terracing: clipmap vertex heights snap to the band's voxel cell
(round-to-nearest, matching voxel-core's centre-rule so the seam has no
step-down ledge); shading normals + T-junction stitch over QUANTIZED heights
(crack-free by construction -- Chebyshev bands land on level boundaries);
one C++ derivation feeds mesh + material (pushed as MID params, warn-on-drift
vs asset defaults); shader v3 strengthens the cubic read (riser 6.0, jitter
0.22, edge 0.30, posterize 3) and switches to Chebyshev-from-origin. Same
-VoxelClipmapVoxelize switch gates both halves. Ladder verified live in-log:
"seam 8192 m, cells 12.8/25.6/51.2/102.4 m per band, geometry TERRACED".
Honest limits: painted terraces still carry steep far faces (vert spacing is
40x cell); silhouette steps are sub-pixel at extreme range. Captures
00670/00672/00674 OWNER-APPROVED 2026-09-04 ("Those look dope"). v3 ships.
