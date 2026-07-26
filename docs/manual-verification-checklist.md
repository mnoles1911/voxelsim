# Manual verification checklist (PIE)

Written 2026-07-26 at the end of the GPU streaming wave. Everything here is a
thing **headless capture cannot answer** — either because it needs a moving
camera, a human eye, or a judgement about whether something "looks right".

Read the note at the bottom on why the automated method missed the ring-gap
symptom; it is the reason this file exists.

## Before you start

Current defaults, so you know what you are looking at without setting anything:

| cvar / switch | default | meaning |
|---|---|---|
| `voxel.Stream.GPU` | **0** | terrain renders on the per-chunk component path |
| `voxel.Stream.GPUCull` | **0** | pooled frustum cull is off (it still drops geometry) |
| `voxel.Water.GPU` | 0 | water on the per-chunk path |
| `voxel.GI.Enabled` | 0 | voxel GI off |
| `voxel.GI.Volume` | 0 | GPU GI volume off |
| `-VoxelCoarseGrid` | on | coarse chunks use the flat-grid fast path |
| `-VoxelL0BrickSkip` | on | level-0 skips provably-empty bricks |

**Give the world time to settle.** The full 2 km cascade takes roughly 10–30 s
from spawn depending on machine. `voxel.Debug 1` shows a HUD; the world is
settled when `pending` is 0 on every ring and `jobs` reads 0.

---

## 1. The ring gaps — the one I could not reproduce

**This is the priority.** You reported concentric ring gaps while flying; my
20 m/s scripted flight on current main does not produce them, and the run where
I thought I saw them turned out to be a build linking a stale library.

1. Enter PIE, `voxel.Debug 1`, let the world settle.
2. Fly the way you normally do — the speed and direction that shows it.
3. **When you see a gap, stop moving and hold still.**
4. Note from the HUD/log: which ring boundary it sits at, and whether it **fills
   in** within a few seconds of stopping.

What the answer means:

- **Fills in when you stop** → streaming cannot keep up at your speed. It is a
  throughput/priority problem and the per-ring `loaded`/`pending` numbers in the
  log at that moment are the evidence I need.
- **Does NOT fill in when you stop** → it is not a speed problem at all. It is an
  admission or residency bug — a chunk that is never requested. Very different
  fix, and much more likely to be reproducible.

Either way, please grab the log (`ue-project/Saved/Logs/VoxelEarth.log`) — the
`Voxel rings:` and `Voxel ring dispatch:` lines around the moment are what
distinguish the two.

Also worth trying: fly the same path with `voxel.Stream.GPU 1`. If the gaps
differ between the two renderers, that localises it to rendering rather than
streaming.

## 2. Terrain appearance (the parity work)

Set `voxel.Stream.GPU 1`, restart PIE, settle.

- **Hillside risers** — every vertical step face should read as blended turf, not
  pink/tan. This was genuinely broken and is the one visual fix I am confident in
  (17.4% → 4.3% of pixels differing from the component path).
- **Cave floors** — go underground. Floors must **not** be grass-green. Same fix,
  other half.
- **A/B it**: `voxel.Stream.GPU 0`, restart, same spot. The two should be
  indistinguishable apart from a faint biome-tint gradient (the pooled path
  samples climate per chunk rather than per quad — known and accepted).

## 3. Frame cost, with the camera held still

The number I could never measure reliably in automation. Hold the camera
**completely still** and pointed at a dense view, then read `stat unit`.

1. `voxel.Stream.GPU 0` → note Frame / Draw / GPU.
2. Quit, `voxel.Stream.GPU 1`, return to the **same spot and same view
   direction**, settle, read again.

Holding the pose fixed is the whole trick — it is what my headless harness could
not do, and without it the run-to-run spread swamps the difference. Two identical
runs at an unpinned pose gave me 43 fps and 103 fps on the same settled scene.

Expect the pooled path to be **slower** here (~23% at p50, more when little is on
screen) because it has no per-chunk culling yet. That is known, measured, and why
it is not the default.

## 4. Water (optional)

`voxel.Water.GPU 1` needs an actual cavern lake — the default spawn's water is an
ocean plane the pool does not touch. `-VoxelFloodTest=70` sets one up. Look for
translucent water at the right waterline with the cavern floor visible through
it.

### 4a. Water frame cost — the number automation could not take (added Wave E)

Same shape as item 3, same reason, and it is now the *only* thing blocking a
verdict on the water pool. Headless legs were held rather than run: up to four UE
sessions were live on the box at once, and a contended frame-time number is worse
than no number because it looks like a result.

If you have the machine to yourself, this takes ten minutes and settles it:

1. Launch with `-VoxelFloodTest=70` once and note the pose it prints
   (`VoxelFloodTest [lake/static] camera: ACTUAL (...)`). At the reference seed it
   is `(42030, 21000, 96062) rot(pitch −25, yaw 0)`.
2. `voxel.Water.GPU 0`, get to that pose, hold **completely still**, let it
   settle, read `stat unit` — Frame / Draw / GPU.
3. Quit, `voxel.Water.GPU 1`, same pose, settle, read again.
4. Twice each, alternating, and treat the two same-config readings as the noise
   floor. If they straddle the difference, there is no difference to report.

The pooled path's whole claim is ~2,200 water primitives collapsing to **one**,
so Draw is the number to watch, not Frame.

### 4b. Water translucency — one sort key for the whole world (added Wave E)

The pool draws all water as one primitive, so there is one translucent sort key
for every water surface in the world, and triangles blend in pool-allocation
order. The argument that this is invisible is narrow and worth an eye: it holds
only because base colour and opacity are constant and there is no refraction.

At the cavern lake, with something in view where **two water surfaces overlap in
depth** (looking along the lake at a shallow angle, or across a lower pool
through the near shore), A/B `voxel.Water.GPU 0` and `1`. They should be
indistinguishable. If they are not, the pool needs per-region sort keys before
any of W5's fill-fraction shading, foam, caustics or refraction can land.

## 5. Digging (optional but valuable)

- Dig straight down more than ~40 m. The shaft should stay solid all the way; a
  see-through hole below ~41.6 m is the bug fixed this wave, and a regression
  there matters.
- Save, quit, reload, and look at the same shaft. Saved worlds used to lose
  structures above the sky-band trim.

## 6. Per-chunk debug tints, when they land (added Wave E)

Not built — it needs a vertex-factory change this wave did not own. When it does
land, the thing to check is **the renderer that is not being demoed**: the tint
rides a second texture coordinate, and the component path does not supply one, so
it receives a *duplicate of texture coordinate 0* rather than zero (measured, see
`docs/gpu-waves-plan.md` Wave E). A wrong encoding shows up as component-path
terrain repainted in red/green bands that reset every 32 m — the same signature
the probe produced deliberately.

So: with debug tints **off**, `voxel.Stream.GPU 0` terrain must look exactly as
it does today. That is the regression to watch, not whether the tint itself
works.

---

## Why this file exists

Every screenshot I took was a **stationary camera on a settled world**. That
cannot show a movement-induced gap, which is exactly the symptom you reported. I
verified terrain was fully loaded in each capture (`jobsInFlight=0
pendingJobs=0`) and concluded the pictures were sound — and they were, for the
question they could answer. They simply could not answer yours.

Two other traps this wave, both worth knowing before trusting any number:

- **A stale `voxelcore.lib`** silently links an out-of-date generator and
  produces a different world. It sent two investigations into the wrong
  subsystem, and one of my own conclusions to you was drawn from a run affected
  by it. The build now warns; if you see that warning, rebuild voxel-core before
  believing anything.
- **Single runs lie.** Two identical configurations regularly differ more than
  the effect being measured. Nothing here should be concluded from one run.
