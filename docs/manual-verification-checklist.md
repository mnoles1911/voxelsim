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
| `voxel.Stream.GPU` | **0** | terrain renders on the per-chunk component path. The reason has changed: not "it drops geometry" (fixed) but that the pooled path costs ~0.15 ms **more** than the component path at a down-facing pose. The fix is compaction, which is not built. |
| `voxel.Stream.GPUCull` | **1** | pooled frustum cull is **on**, and its output is verified pixel-identical to the single full draw. The pool's cost now tracks what is on screen. |
| `voxel.Water.GPU` | 0 | water on the per-chunk path |
| `voxel.GI.Enabled` | **0** | voxel GI off. Measured cost of turning it on: free at the median (+0.6% p50) but **3.2× the hitches and +14.9% p95**, neither overlapping the off-baseline. |
| `voxel.GI.Volume` | 0 | GPU GI volume off. Additionally it has **no consumer** under `voxel.Stream.GPU 0` — only the pooled vertex factory samples it, so with the default above it changes nothing at all. See §6. |
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

**SUPERSEDED — do not test against the old expectation.** This used to say to
expect the pooled path to be *"~23% slower at p50, more when little is on
screen, because it has no per-chunk culling yet"*. The cull now exists, is on by
default, and is verified pixel-identical to the single full draw. **If you
measure against the old text you will conclude the cull did not work.**

What to expect now: the pooled path is **level with the component path at the
horizon**, and **~0.17 ms behind at a down-facing pose**. That remaining gap is
why `voxel.Stream.GPU` is still 0; the fix is draw compaction, which is not
built. So the interesting reading here is no longer "how much slower" but
**whether the two are close enough that you would not notice** — and, at a
down-facing pose, whether the pool still costs anything when almost nothing is
on screen.

If you also want to see the GI cost from §6, note it is **not** visible in a
still-camera `stat unit` reading: it is free at the median and lives entirely in
the tail (3.2× hitches, +14.9% p95). Watch for intermittent stutter over a
minute rather than a worse steady number.

## 4. Water (optional)

`voxel.Water.GPU 1` needs an actual cavern lake — the default spawn's water is an
ocean plane the pool does not touch. `-VoxelFloodTest=70` sets one up. Look for
translucent water at the right waterline with the cavern floor visible through
it.

## 5. Digging (optional but valuable)

- Dig straight down more than ~40 m. The shaft should stay solid all the way; a
  see-through hole below ~41.6 m is the bug fixed this wave, and a regression
  there matters.
- Save, quit, reload, and look at the same shaft. Saved worlds used to lose
  structures above the sky-band trim.

## 6. Voxel GI through the GPU volume (Wave B)

**Everything here needs `voxel.Stream.GPU 1`.** Only the pooled vertex factory
samples the volume; the per-chunk component path bakes GI into vertex colours and
never reads the texture. With `voxel.Stream.GPU 0`, `voxel.GI.Volume 1` allocates
nothing and changes nothing — that is by design, not a bug, and it is the single
easiest way to conclude "the GI volume does nothing".

Launch with:

```
-VoxelGIOn -dpcvars=voxel.Stream.GPU=1,voxel.GI.Volume=1,voxel.GI.VolumeDim=192
```

`voxel.GI.VolumeDim` is **startup only** (it sizes an allocation), which is why it
goes through `-dpcvars` rather than the console. Everything else on this list can
be toggled live now.

### 6a. The fade at the volume face — the one a harness cannot score

This is design risk 8 and it is the hardest kind of wrong to notice, because it
produces a *plausible* image. The volume covers ±38.4 m at `VolumeDim 192`;
beyond it GI must hand back to the mesher's plain AO **smoothly**.

1. Settle, then fly slowly outward in a straight line over open, uneven ground.
2. Watch for a **ring** — a circle centred on you where the ground brightness
   steps rather than ramps. It moves with you, which is what distinguishes it
   from a terrain feature.
3. `voxel.GI.Volume 0` / `1` toggles live now; flipping it at the suspect
   distance is the cheapest A/B.

The clamp that keeps the fade inside the volume logs its effect. Grep the log for
`VoxelGI volume params:` — if `fade=` differs from `cvars asked`, the clamp fired
and the fade you are looking at is not the one the cvars requested.

### 6b. Re-centring, under motion

Automated capture structurally cannot answer this: the volume re-centres over
~8 frames (~130 ms) and every screenshot in this repo is a settled, stationary
scene.

1. Settle, then fly **continuously** for a minute or so at normal speed.
2. Watch for a brief, whole-screen brightness flicker in the middle distance —
   not at a ring, not tied to a chunk boundary.
3. The log says exactly when to expect one: `re-centre BEGIN` … `re-centre COMMIT`
   pairs. If a flicker does not line up with one of those timestamps, it is not
   this.

What the code guarantees, so you know what you are looking for: the camera's own
neighbourhood is restaged in the *same* frame the origin swaps, so any artifact
should be in the **middle distance**, never underfoot. `voxel.GI.Debug 2` prints
`VOLUMERECENTRE transient:` with how many occupied texels were stale at the worst
moment and how close the nearest one got to the camera.

### 6c. Scheme B's horizontal error — worth one look in a cave

Measured, not suspected: the volume stores ±Z irradiance exactly and the four
horizontal directions as their **mean**. The error is bimodal — half the samples
are essentially exact, but p95 is ~52/255 and the worst is ~105/255, and it
concentrates on **side faces near vertical occluders**, which is most of a cave
wall. See the Wave B section of `docs/gpu-waves-plan.md` for the numbers.

Nothing automated can decide whether that is acceptable, because it is a
question about appearance. Go underground, look at a wall lit from one side, and
compare `voxel.GI.Volume 1` against `voxel.GI.Volume 0` with `voxel.GI.Enabled 1`
on the component path (`voxel.Stream.GPU 0`, restart) — the component path
evaluates the full ambient cube and is the reference. If the volume's cave walls
read flat or wrongly-lit next to it, Scheme A is worth its 2× memory and the
Wave B section records what that would cost.

### 6d. A dig should relight without re-meshing

`voxel.GI.VolumeDigTest` automates the numeric half (it logs `VOLUMEDIG` lines).
The visual half:

1. Settle, dig a short tunnel into a hillside, then **stand still and watch**.
2. The tunnel interior should **darken progressively over a second or two** as
   the solve lands, not pop from lit to dark in one frame, and not stay lit.
3. The one failure this is looking for: a dug tunnel that keeps its **pre-dig**
   lighting indefinitely. That is zero-on-revoxelize not reaching the texture.

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
