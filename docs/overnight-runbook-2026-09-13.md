# Overnight runbook, 2026-09-13

The queue being worked through unattended, in order, with the gate each leg has
to clear and the command that runs it. Written so an interrupted session can
pick up at the first unticked line rather than reconstructing the plan.

**Box rules that apply to every line.** One leg at a time; a build and a leg may
not overlap in either direction; `Get-Process`, never `tasklist`, to ask whether
the box is free; and the walk/route harnesses refuse to start beside foreign GPU
load, which is correct — a timing capture taken next to a game or a video
measures contention. Counter-only and image-only legs are immune to that and can
run when timing legs cannot.

## State at the start of the night

- Nanite-on-by-default for the understory is **written and built but
  uncommitted**, with both bakes on disk from 2026-09-12
  (`temperate-default-nanite-1`, `temperate-default-nanite-off-1`). Verification
  never ran: the two walks were launched at 00:20 and produced nothing.
- The roof-probe and across-frames shortlist changes are committed (`c473b84`),
  default on, each with a control arm, both **unmeasured**.
- Baseline to beat, walk 57, frames 0:200: `RoofProbeMs` 2.944,
  `CollisionPrepareMs` 3.677, `CollisionPreparations` 1.000 (min = max = 1 over
  200 frames), GameThreadTime 12.537.

## The queue

### 1. The Nanite default, verified then committed

    .\tools\ecological-walk-validation.ps1 -AssetDirectory <fixture> `
      -Output <...>\walk-capture-59-default-nanite -SpawnAt '-154740,-81476' `
      -DetailMeshLOD -MarchDispatchIdentity -AllowPreviewDetailCache `
      -DetailMeshCache <...>\temperate-default-nanite-1\detail-bake.json

    (arm 60: the -off bake, plus -ExtraArgs '-VoxelNoDetailNanite')

**Gate:** arm 59 takes the Nanite path with NO flag on the command line, and arm
60 refuses it with one — proved by the cache identity accepting each bake, by
`nanite=1`/`nanite=0` in the logs, and by zero material-audit fallback lines.
Coverage is the risk that the owner's picture verdict does not cover: only the
BAKED path can produce Nanite meshes, so any cache miss keeps the traditional
proxy and a partial population is drawn both ways.

### 2. What is inside the submit bracket

    -ExtraArgs '-dpcvars=voxel.Stream.FrameAttribution=2'

**Gate:** the log carries `Voxel frame attribution SUBMIT-SPLIT`, and the six
parts sum to `subTotal`. Standing hypothesis is `SubRasterMs`; it can lose.
Diagnosis first, no fix built on this pass.

### 3. The two shortlist changes, measured

Three arms, one binary, same fixture and spawn:

| arm | cvars |
|---|---|
| control | `voxel.Collision.RoofProbeShortlist=0,voxel.Collision.ShortlistAcrossFrames=0` |
| roof only | `voxel.Collision.ShortlistAcrossFrames=0` |
| both (default) | none |

**Gate, and it is the one that matters:** `CollisionPreparations` stays at 1 per
frame on every arm. Prediction recorded before the run — the sweep rect is about
±4 voxels around the pawn's XY and the probe's covered rect is ±32 voxels around
the same column, so each request is contained in the other's cover whichever
ticks first, and neither ordering should force a second prepare.

### 4. The height pyramid

Image gate first — a renderer speedup is a claim about the picture, and this
project once shipped a −7.6% "win" that was the marcher deleting a mountain.

    .\tools\voxel-heightpyramid-image-ab.ps1 -Prefix hpimg-massif
    .\tools\voxel-heightpyramid-image-ab.ps1 -Prefix hpimg-forest -SpawnAt '-154740,-81476' -SpawnAltM 5

**Noise floor, measured 2026-09-13 at the massif:** 0.60% of pixels over 8/255,
mean delta 1.27/255, concentration 0.22 (0.10 is perfectly diffuse). A diffuse
floor of that size is healthy; the armed pair fails if it exceeds it by a margin
the owner would see, or if its difference is CONCENTRATED into a shape — an arc,
a ridge, a missing skyline — however small the mean.

Timing only after the images pass, and with hole statistics OFF, because they
permute the kernel being timed.

### 5. The speed the target is written for

- On foot, the fastest a player can move: `-VoxelWalkSpeedTier=7` (9.5 m/s).
- The target's own speed, which needs the fly pawn:
  `tools/voxel-run-flight-leg.ps1 -SpawnAt '-154740,-81476'` with
  `-VoxelPerfSpeed=20` and the ecology fixture passed through `-ExtraArgs`.

**Gate:** both measured with the receipt discipline, and the pawn named on every
row — 9.5 m/s on foot and 20 m/s flying are not the same experiment.

### 6. What the Nanite flip leaves open

Bake coverage becomes load-bearing once Nanite is the default. Re-check whenever
the species set or the bake scope changes: zero fallbacks is a property of THIS
bake at THIS site, not of the code.
