# Voxel GI as a GPU volume texture — design

Written 2026-07-25 from reading the light field, the GI subsystem, the pooled
vertex factory and the D3D12 RHI. `file:line` references were current at the time
of writing and are worth re-checking before leaning on one.

> **STATUS, corrected 2026-07-29. This SHIPPED.** The header used to read *"Design
> only; nothing here is implemented yet"*, which was true on the day it was
> written and false from the first landed step. `VoxelGIVolume.h/.cpp` exist,
> `VoxelQuadVertexFactory.ush` carries the `GIUVW` interpolant (`:55`) and samples
> the volume per pixel with multi-step probe fallback, and `voxel.GI.Volume`
> defaults to **1**. Steps 0, 1 and 2 of §7 are done and annotated in place.
>
> **It shipped as Scheme A, NOT the Scheme B this document recommends.** Two RGBA8
> volumes, `VolumePos = (Vis[+X], Vis[+Y], Vis[+Z], v)` and `VolumeNeg =
> (Vis[−X], Vis[−Y], Vis[−Z], v)` — two textures, still one sample per face, since
> a face reads exactly one of them chosen by the sign of its normal
> (`VoxelGIVolume.h:34-39`). Step 3 measured the choice and Scheme B lost: it
> **missed this design's own RMS bar by 2.6×, and was worst on exactly the cave
> walls the feature exists for**. §2's "Recommended default" for Scheme B is
> therefore superseded by measurement — which is what §2 asked for. The
> recommendation was wrong; the method that overturned it was the one this
> document specified.
>
> **And it shipped at Dim 192, not N=256** — see §2's dimensions table, corrected
> in place.
>
> Everything else here — the premultiplied-validity trick, the pool-space
> precision rule, the row-pitch and upload constraints, the re-centring design,
> the risks — held up and is still the reference. Read it as a design record with
> known outcomes, not as a proposal.

The point of this is **not** parity. Today a dig re-meshes a chunk partly just to
refresh baked GI; the whole re-shade phase exists to push new irradiance into
vertex colours. A GPU-sampled field deletes that coupling: lighting updates
become texel uploads and geometry updates stay geometry updates. That is the
prize, and it is independent of everything else left in G4.

## 0. Three findings that change the shape of the work

**No material asset change is required.** Not for per-vertex, and not for
per-pixel either. `M_VoxelTerrain` computes `BaseColor = albedo * VertexColor.G
* DebugTint`, and the pooled vertex factory owns both ends of the vertex-colour
pipe — it writes `Intermediates.Color` in `GetVertexFactoryIntermediates` and
`Result.VertexColor` in `GetMaterialPixelParameters`. So the GI term can be
folded into `.g` at either frequency entirely inside `VoxelQuadVertexFactory.ush`
and its `.h`/`.cpp`. This is the single largest cost reduction available here,
and it corrects `gpu-g4-parity-plan.md`'s original claim that every remaining
item needed a graph change.

**Prerequisite nobody had written down: under `voxel.Stream.GPU 1` the light
field is EMPTY.** `UVoxelGISubsystem::NotifyChunkMeshUpdated` is called from
exactly one site — `UVoxelChunkComponent::SetChunkQuads` — and the pooled
streaming branch returns before any `UVoxelChunkComponent` is created. No chunk
is voxelized, no brick is solved. **Every later verification step would compare
an empty field against an empty field and pass.** This is step 0.

**The `Static`-not-`Dynamic` buffer trap has no volume-texture analogue, but a
different one does.** `RHIUpdateTexture3D` stages the exact region and issues
`CopyTextureRegion` honouring the destination offsets (`D3D12Texture.cpp`
`RHIUpdateTexture3D` → `BeginUpdateTexture3D_Internal` →
`EndUpdateTexture3D_Internal`). No renaming, no ignored offset — genuinely
unlike the buffer lock path, and verified rather than assumed. The real trap is
that the staging row pitch is `Align(Width * BlockBytes, 256)`: an 8-texel-wide
RGBA8 brick stages **16 KB to move 2 KB**. Merge dirty bricks into wide X-runs
before uploading.

## 1. What the field is today

`VoxelLightField.h`:

| | |
|---|---|
| cell size | **40 UU** (0.4 m, 4 voxels) |
| brick | 8³ cells = **320 UU**, exactly one level-0 chunk |
| directions | 6 (ambient cube) |
| container | `TMap<FIntVector, TUniquePtr<FVoxelLFBrick>>` — sparse, absolute-keyed |
| build radius | `voxel.GI.RadiusUU` 7000 |
| fade | 4800 → 6400 UU |
| cap | `voxel.GI.MaxBricks` 4096 |
| centre | `FieldCentreUU`, set every tick to the camera |

Per brick: `Opacity[512]` + a 3-level opacity pyramid (cone-march only),
`Vis[512*6]` (**the payload**), `AvgIrr`/`PrevAvgIrr` (Jacobi bounce only), and
`SolvedCells` as a bit array. Only `Vis` and `SolvedCells` need to reach the GPU.

`sizeof(FVoxelLFBrick)` is ~4.7 KB. Two in-tree figures are stale and predate
`PrevAvgIrr`: `VoxelGI.cpp`'s "~3.6 KB each" and `status.md`'s 8.2 MB at 1999
bricks. Corrected: **~9.4 MB** at the measured 1,999 resident bricks, **~19.2 MB**
at the 4096 cap.

The lattice is **absolute-world**, not centre-relative — `floor(WorldUU / 320)`
in double. The field is not a clipmap; `FieldCentreUU` only drives admission and
eviction. So re-centring costs nothing today because there is nothing to
re-centre. **A GPU volume introduces that cost for the first time** (§4).

Threading: structural mutation is game-thread under a write lock; `SolveBricks`
is a blocking `ParallelFor` under a read lock; sampling hoists one
`FVoxelLightField::FReadScope` across a whole proxy build. **Anything that reads
the field to build texel data must be on the game thread inside a `FReadScope`,
and hand the staged bytes to the render thread in an `ENQUEUE_RENDER_COMMAND`
payload. Do not read the field from the render thread.**

## 2. Encoding and sizing

### Directional, and it matters

`Vis` is 6 bytes per cell and `SampleIrradianceAtProbe` recombines with
`max(0, N·D)` weights. Every greedy-mesh normal is axis-aligned, so exactly one
slot is selected. The header is explicit that this is load-bearing — a floor and
the ceiling above it get correctly different answers from the same field.
Collapsing to scalar `AvgIrr` is a visible regression in exactly the caves this
feature exists for.

Seven numbers (6 slots + validity) and no 7-channel format, so:

- **Scheme A — 2 × RGBA8, 8 B/cell. ← THIS IS WHAT SHIPPED.** `(+X,+Y,+Z,v)` and
  `(−X,−Y,−Z,v)`, premultiplied by validity. Bit-exact for any normal. Two
  samples — and in practice **one**, because a face's normal sign selects exactly
  one of the two textures (`VoxelGIVolume.h:34-39`), which this bullet
  underestimated.
- **Scheme B — 1 × RGBA8, 4 B/cell. ~~Recommended default.~~ Measured and
  rejected** — 2.6× over the RMS bar below, worst on cave walls.
  `R = Vis[+Z]·v`, `G = Vis[−Z]·v`, `B = mean(±X,±Y)·v`, `A = v`. **Exact** for
  ±Z faces — the classes where directionality visually dominates — and the mean
  for the four horizontal classes. One sample, half the memory.
- **Scheme C — R8 scalar.** Loses floor/ceiling distinction. Fallback only.

**Decide by measurement, not argument.** A ~30-line harness in the style of
`-VoxelGIConverge` can walk every solved cell and report
`RMS(Vis[±X], Vis[±Y] − their mean)` in irradiance bytes. Under ~8/255 means
Scheme B is free quality. One run settles the 2× memory question before any GPU
code exists.

### Premultiplied validity

Storing `Vis·v` and `v` separately, then dividing in the shader, makes hardware
trilinear reproduce the CPU sampler's "skip unsolved taps and renormalize by the
surviving weight" **exactly and for free** — the same arithmetic, executed by
the texture unit. It also gives the miss case for free: `A ≈ 0` means no data,
which the shader turns into plain AO, matching the CPU's `return false` path.
This is the load-bearing trick in the whole design.

### Dimensions

Cell = 40 UU, so `N³` covers `±N·20` UU.

| N | half-extent | RGBA8 (B) | 2×RGBA8 (A) |
|---|---|---|---|
| 192 | ±3840 UU | **28.3 MB** | 56.6 MB |
| 256 | ±5120 UU | **67.1 MB** | 134.2 MB |
| 320 | ±6400 UU (= `FadeEndUU`) | 131.1 MB | 262.1 MB |
| 352 | ±7040 UU (≈ `RadiusUU`) | 174.5 MB | 349.0 MB |

**Recommended at the time: N = 256, `PF_R8G8B8A8`, Scheme B → 67.1 MB.** Largest
tier under the geometry pool's own 112 MB, and ±5120 UU is close enough to the
existing fade that only a retune is needed (§7). Fallback N = 192 at 28.3 MB.

> **WHAT ACTUALLY SHIPPED: N = 192, Scheme A → 56.6 MB** (the 2×RGBA8 column of
> the row above). `voxel.GI.VolumeDim` defaults to **192**, `ECVF_ReadOnly`,
> clamped to [16,256] and rounded **down** to a multiple of 8 so the volume is a
> whole number of bricks (`VoxelGIVolume.cpp:38-43,66-67`). So the fallback
> dimension was taken and the recommended encoding was not — the encoding because
> step 3 measured Scheme B as 2.6× over the RMS bar, the dimension because 192 is
> where the Wave B Scheme A measurement was made and it covers ±3840 UU, enough
> to judge by eye.
>
> **Two consequences worth carrying forward, because neither is in this table.**
> First, there is an **equal-size CPU mirror** — `VolumeShadow` and
> `VolumeShadowNeg` (`VoxelGI.h:254-261`), the bytes `voxel.GI.VolumeCheck`
> compares against, so the real footprint is ~113 MB, not 56.6. Second, the
> **usable reach is well under the half-extent**: the fade clamp of risk 8 (which
> shipped, at `VoxelGI.cpp:970-986`) *slides* the band down to 1920–3520 UU at
> Dim 192, so GI is at full strength only to 19.2 m and gone by 35.2 m against a
> nominal ±38.4 m box. Any reach argument must use those numbers. See
> `docs/lighting-weather-plan.md` §5.2, which derives them.

**Full-coverage parity is not affordable — matching the 7000 UU build radius
with the exact ambient cube is 349 MB. Say so and do not attempt it.**

The density cost is real: steady state is ~1.02 M live cells against 16.78 M in
a dense 256³, so >90% of the volume is rock, sky, or unloaded, all reading
`A = 0`. That is the price of O(1) random access by world position.

If 67 MB is contested, the escalations in order are a **clipmap** (two N=160
levels — 40 UU to ±3200 and 160 UU to ±12800 — 32.8 MB total, *half* the memory
and 2.5× the reach, but needs an irradiance downsample the field does not have
today since its mips are opacity-only) and then **brick indirection** (a 32³
`R32_UINT` index volume plus an atlas; 8³ bricks must be stored as 10³ with a
replicated border because hardware trilinear cannot cross atlas tiles, so
~16.4 MB at the 4096 cap). **Neither is the starting point.**

## 3. Upload

Create with `FRHITextureCreateDesc::Create3D(...)`, `ETextureCreateFlags::ShaderResource`,
sampler `TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>`. On a
`Texture3D`, linear min/mag interpolates across all three axes; clamp on all
three makes an out-of-range lookup degrade to the edge rather than wrap.

**Do not set `ETextureCreateFlags::Dynamic` and do not use any lock/Map path.**
That is the texture-side analogue of `gpu-pool-rendering-notes.md` invariant 5,
and it is avoided by construction if you only ever call `UpdateTexture3D`.

Constraints found in the engine source:

- `BeginUpdateTexture3D_Internal` asserts `IsInParallelRenderingThread()` — all
  uploads inside `ENQUEUE_RENDER_COMMAND`.
- `EndUpdateTexture3D_Internal` asserts `GFrameNumberRenderThread == UpdateData.FrameNumber`
  — if you use the `Begin`/`End` pair to let a worker fill staging memory
  directly, both must land in the same render-thread frame. The one-shot
  `UpdateTexture3D` has no such hazard; start there.
- `D3D12.UseUpdateTexture3DComputeShader` defaults to 0. The compute path has
  different alignment behaviour; if anyone flips it, re-verify.
- **Row pitch is 256-aligned.** Per-brick uploads waste 8×. Group the frame's
  dirty bricks by `(brickY, brickZ)`, sort by `brickX`, coalesce runs (tolerate
  small gaps — uploading a 3-brick gap beats a second barrier). A dig dirties a
  5×5×5 brick neighbourhood: 25 calls of 40×8×8 instead of 125 calls of 8³.

`RefreshQueue`/`RefreshSet` already carries exactly the right work. Replace the
re-shade drain in the subsystem tick with a texel upload, keeping the budget-cvar
shape (`voxel.GI.MaxBrickUploadsPerFrame`, ~64).

**Three other sites must write texels, and missing any leaves stale light:**

| Event | Texel action |
|---|---|
| brick solved | write `Vis·v, v` |
| brick re-voxelized, not yet solved | **zero it** — else a dug tunnel keeps its pre-dig lighting until the solve lands |
| brick evicted | **zero it** |

Zeroing is safe precisely because of the validity channel: `A = 0` means "no
data" and falls back to plain AO, not to black.

## 4. Re-centring

The volume must follow the camera; the CPU field does not have this problem
because it is absolute-keyed (§1).

- **Toroidal addressing** — only new slabs upload, but hardware trilinear bleeds
  across the wrap plane, and that plane *sweeps through the interior* as the
  origin scrolls: a 40 UU artifact plane moving across the world. Fixing it means
  manual 8-tap `Load()` with wrapped integer coords — correct, expensive, fiddly.
- **Double-buffer** — 2× VRAM. Rejected at 67 MB.
- **Dead zone + staged re-upload — recommended.** Re-centre only when the camera
  leaves a central box (±N·5 UU = ±1280 UU at N=256); re-upload every resident
  brick at its new address over ~8 frames; swap the origin uniform **only on the
  frame the last upload lands**. The dead zone guarantees the old volume still
  covers the camera throughout. ~2000 bricks × 2 KB ≈ 4 MB per re-centre, and at
  20 m/s the camera crosses the dead zone every ~1.3 s — ~3 MB/s. No seam, no
  extra VRAM.

**Snap the volume origin to a whole brick (320 UU) multiple, computed in
double**, so brick uploads always land on texel-aligned 8³ boxes and the texel
lattice coincides exactly with the field's cell lattice. No resampling, ever.

## 5. Binding and sampling

The factory's uniform buffer is built once in `InitRHI` and the header warns
that changing an input afterwards needs a re-init. Do not perturb that — this is
a renderer that fails silently. **Add a second uniform buffer** holding the
volume texture, sampler, origin in pool space, inverse size, and the
strength/floor/fade scalars. Bind it alongside `VoxelVF` in
`GetElementShaderBindings`; because the pool is dynamic-relevance that runs every
frame, so it can simply be rebuilt whenever the origin moves.

Two rules already paid for in this module:

- **Members are reached through the struct** — `VoxelGIVol.GIVolume`, never a
  loose `Texture3D GIVolume;` in the `.ush`. A loose declaration compiles
  perfectly and reads zeros forever. Symptom: uniformly-lit terrain that looks
  *almost* right.
- **No member may be null.** Bind `GBlackVolumeTexture->TextureRHI` and
  `Enabled = 0` when GI is off.

### Per-pixel, not per-vertex

The CPU path compensates for per-vertex GI by **subdividing greedy quads**
(`voxel.GI.MaxQuadSpanVoxels`). The pooled path structurally **cannot** — every
quad is exactly 6 vertices reconstructed from `SV_VertexID`, and subdividing
would multiply the pool's quad count against its 112 MB budget. So per-vertex on
the pool is strictly worse than per-vertex on the component path.

Per-pixel is also *better than what it replaces*: full-rate GI **and** it deletes
the quad-subdivision cost, which is where GI's median cost actually lives.

VS side computes the probe position pushed out along the face normal (matching
the CPU's first probe offset — the normal is constant across the quad, so
offsetting here is exact and free) and hands the PS a UVW in `[0,1]` on
`TEXCOORD1`, which is free. Carrying UVW rather than a position keeps magnitudes
small and makes the fade distance recoverable without a second interpolant.

PS side samples, divides by `A`, selects the ambient-cube slot from the world
normal's `z`, and multiplies into `VC.g` (which is already AO).

**The formula must match the CPU term for term**, because `voxel.Stream.GPUMaxLevel`
puts both renderers in one frame; a divergence appears as a lighting seam at the
ring boundary. The CPU reference is:

```
Fade    = 1 - clamp((Dist(WorldPos, GICentreUU) - FadeStart) / (FadeEnd - FadeStart), 0, 1)
Ambient = lerp(AmbientFloor, 1, clamp(Irr, 0, 1))
Weight  = clamp(Strength * Fade, 0, 1)
ShadeG  = AoNorm * lerp(1, Ambient, Weight)      // miss => ShadeG = AoNorm
```

and `Decoded.AmbientOcclusion` is already `AoNorm` on the same 0..1 scale.

### Half-texel convention

The CPU treats the solved value as living at the **cell centre** — it probes at
`(X+0.5)·40` and the sampler computes `P/40 - 0.5` before flooring. Texel `i`
must therefore be world `VolumeOrigin + (i+0.5)·40`, which is exactly what
`UVW = (Pos - VolumeOrigin) / (N·40)` gives with hardware trilinear. **No
half-texel offset is needed** — but only because the origin is brick-snapped to
the field lattice (§4). Get that wrong and everything shifts half a cell.

### Pass cost

`GetMaterialPixelParameters` runs in every pass compiling this factory, and the
material is **Masked**, so the depth pass runs it too — one extra `Texture3D`
sample there for no benefit. Measure before optimising.

## 6. Precision — the exact space

`gpu-pool-rendering-notes.md` invariant 4: at ~8.4 M UU float32's ULP is 1.0 UU.
A 40 UU cell would quantise to 40 steps, and computing `WorldPos - VolumeOrigin`
in float32 at that magnitude is catastrophic cancellation.

**Compute the volume coordinate in POOL-PRIMITIVE SPACE. Nothing else.** The
pool component carries the 8.4 M UU offset in its double-precision transform;
`OriginInPool = OriginRelative - GpuPoolRebase` is done in double and narrowed
once; `Intermediates.Position` is already in that space, and the big offset only
re-enters at `TransformLocalToTranslatedWorld` for rasterisation.

So `Intermediates.Position` is the correct and only input. Supply the origin in
the same space, subtracting in double on the game thread and narrowing once. At
the cascade's 2048 m reach the ULP is then ~0.015 UU — 2,600× headroom against a
40 UU cell.

**Prohibited:** passing the world-space origin as `FVector3f`; reconstructing a
world position in the shader from `Primitive.LocalToWorld`; using translated
world space (camera-relative, moves every frame — the volume does not).

**Also assert** that the pool transform stays a pure translation. That is what
makes the world normal usable directly for slot selection. If anyone rotates or
scales the pool, this breaks silently.

## 7. Staged plan

House style: absolute `.uproject` path, `-game -unattended -sm6`, fixed
`-VoxelSpawnAt`, screenshot at **25–30 s**, compared against a control run with
byte-identical arguments. Discard the first run after any build.

**Step 0 — feed the field from the pooled path. ✅ DONE (PR #105).** A pooled
ingest queue in the GI subsystem, called from the pooled branch of
`ApplyMeshResult` where the origin and the CPU-meshed quads are both already in
hand, draining against the same per-frame budget. Measured: both paths settle at
**2212 bricks / 10.1 MB** — the same number, not merely the same order, since
both consume the same CPU mesher output for the same level-0 chunk set and the
field is absolute-keyed. Before it, the pooled run reported `bricks=0`.

One thing that had to come with it, and would have bitten silently: the re-shade
drain bounds *refreshes* but not *pops*, and a miss deliberately does not consume
the refresh budget (that is what makes the per-component dedupe free). On the
pooled path every pop misses, so unbounded it walks `RemoveAt(0)` through the
whole ~2000-entry queue every frame — O(n^2) memmove producing nothing. Pops are
now capped at 8x the refresh budget, which never binds on the component path.

**Step 1 — resource + known pattern + per-pixel sample, no real data. DONE.**
Volume, second uniform buffer, `TEXCOORD1` interpolant and the per-pixel fold
into `.g`, plus `voxel.GI.VolumeTest` to fill a per-brick checkerboard.

*Result:* the checker renders. Measured as percentage of pixels differing by
more than 8/255, at a fixed anchor: **28.6% and 28.3%** against two independent
`voxel.GI.Volume 0` control runs, with a **1.83%** same-binary repeat-run noise
floor. So the volume is created, bound, reachable **from the pixel shader**,
correctly addressed, and pool-space precision is adequate -- and the off path
sits at the noise floor.

Two things the picture showed that are worth knowing before step 2. At N=64 the
volume only covers +/-1280 UU, and `AM_Clamp` means everything beyond that
samples the edge texels, which reads as large smeared bands rather than as a
hard cutoff -- easy to mistake for a bug in the addressing when it is the
sampler doing exactly what it was asked. And the fade constants are derived from
the volume extent, so they shrink with `voxel.GI.VolumeDim`; step 4 and risk 8
are the same issue seen from the other end.

*(Original plan for this step, kept for the reasoning:)* Fill the volume with a
world-space checkerboard (`A=255`, RGB alternating per brick).
*Why:* this is the "does the same pool draw known-good data?" rung of the
diagnostic ladder. A crisp 3.2 m checker aligned to chunk boundaries proves
creation, binding, PS reachability, UVW mapping, origin, and pool-space precision
**in one screenshot**. Uniform shading instead means the buffer reaches the VS
but not the PS — the "compiles perfectly, reads zeros" failure. Add a
`VoxelGI volume:` log line mirroring `VoxelGpuPool upload:`/`placement:` before
you need it.

**Step 2 — real data. DONE.** Encode under `FReadScope` (Scheme B), X-run merge,
zero-on-revoxelize, zero-on-evict, uploads driven off a `VolumeUploadQueue` in
the subsystem tick.

*Result (numeric, primary).* `voxel.GI.VolumeCheck`, at `-VoxelSpawnAt=-84480,53760`
with `voxel.GI.VolumeDim=192`, 20,000 samples over 1,947 resident bricks / 852
inside the volume:

```
VOLUMECHECK: cells=6814 meanAbsErr=0.000 maxAbsErr=0.000
horizontal (+-X/+-Y):  cells=13186 meanAbsErr=5.950 maxAbsErr=101.988
control (half-cell X): cells=6814  meanAbsErr=0.591 maxAbsErr=44.500
signal (+-Z irradiance): mean=31.4 sd=46.5 min=0.0 bytes
volumeMiss=0 fieldFirstProbeMiss=0
```

Pass bar was ±Z mean < 2/255. **Exactly 0 is the correct answer, not a
suspicious one:** with validity premultiplied, both sides evaluate the same
float expression over the same 8 bytes with the same weights — §2.2's claim that
hardware trilinear reproduces the CPU sampler *exactly* rather than
approximately, measured.

Two supporting lines exist because a harness comparing a thing against itself
also reports 0.000. The **control** repeats the comparison with the volume probe
displaced half a cell in X — the smallest addressing mistake the origin snap
exists to prevent — and comes out at 0.591/44.500. The **signal** line reports
the spread of the values the error was measured over, because an unoccluded cell
solves to exactly 1.0 and a uniformly-lit field would score 0.000 for any
encoding at all; sd = 46.5 bytes says there was something to get wrong.

The horizontal number is Scheme B's documented cost, not a defect, and it is the
measurement step 3 decides A vs B on.

*Two things worth knowing before step 3 or 4.* The X-run merge is measured at
**1.4 bricks per run in steady state** — because the round-robin re-solve
delivers bricks in `TMap` iteration order, so a 64-brick batch out of ~2,000
scattered residents rarely contains X-adjacent pairs. It is the *dig* case (a
contiguous 5×5×5 neighbourhood) the merge was designed for and that case is
untested here. And the origin is **static**: set once from the field centre at
the first upload and never moved, so at `VolumeDim` 64 only 36 of 1,947 resident
bricks were inside the volume and even at 192 only 852 were. Step 4 is what
makes the volume follow the camera.

*Verify (visual, secondary):* A/B from the underground camera. Caves are where
the difference lives; the ground camera is a poor discriminator — the surface
capture at 40 s shows healthy terrain and no artifacts, which is a smoke test
and not evidence of anything else.

**Step 3 — pick the encoding by measurement** (§2), now against a real streamed
field.

**Step 4 — re-centring.** Verify on the scripted flight with shots at 15/30/45 s:
no lighting pop, and the origin uniform changing on exactly one frame per
re-centre.

**Step 5 — retire baked GI on the pooled path.** The deliverable: a dig updates
lighting through solve+upload only, with pool `UpdateChunk` calls limited to the
chunks whose *geometry* changed rather than the 5×5×5 lighting neighbourhood.

## 8. Risks

Read `gpu-pool-rendering-notes.md` first. The ones that bite here:

1. **A pooled primitive fails silently.** Budget the log line before you need it
   and work the diagnostic ladder rather than reasoning from code — that table
   records two occasions where reasoning gave a confident wrong answer and a
   control settled it in one run.
2. **Uniform-buffer members through the struct**, never loose globals (§5).
3. **No null members** (§5).
4. **The buffer `Static`/`Dynamic` trap has no texture analogue — do not "fix"
   it.** The real one is the 256-byte staged row pitch (§0, §3).
5. **Uploads on the render thread only** (§3).
6. **Never compute a world position in the shader** (§6). The failure is not a
   crash; it is quantisation and cancellation noise that reads as shimmer under
   camera motion.
7. **Two renderers coexist.** Verify with `voxel.Stream.GPUMaxLevel 0`, which
   puts the potential lighting seam directly in front of the camera.
8. **The fade must be retuned.** `FadeEndUU` 6400 exceeds any affordable
   half-extent; at N=256 the sampler would clamp at the volume face and GI would
   cut off hard at 51.2 m instead of fading. Set `FadeEnd < N·20` with margin
   (e.g. 3600/4600). Getting this wrong produces a *plausible* image with a hard
   lighting ring — the hardest kind of wrong to notice.
9. **Adding to a uniform-buffer layout forces a shader recompile** of every
   permutation using this factory. Expected — but combine it with "discard the
   first run after any build" or you will read the recompile as a regression.
10. **The field is client-side only and outside the determinism boundary.**
    Nothing here may feed worldgen, the edit log, replication, or the digest.
11. **Do not A/B through scalability cvars.**

## 9. Proposed cvars

*Defaults below were the proposal. Two have since moved:* `voxel.GI.Volume` is
**1** and `voxel.GI.VolumeDim` is **192** (`VoxelGIVolume.cpp:14-43`). The rest
shipped as written.

| Name | Default | Purpose |
|---|---|---|
| `voxel.GI.Volume` | ~~0~~ **1** | master switch; prefer a `-VoxelGIVolume` switch for A/B |
| `voxel.GI.VolumeDim` | ~~64~~ **192** | per-axis texels, startup only. Clamped to [16,256] and rounded down to a multiple of 8 so the volume is a whole number of bricks. Read-only, so an override needs `-dpcvars=voxel.GI.VolumeDim=<n>` |
| `voxel.GI.MaxBrickUploadsPerFrame` | 64 | replaces the chunk-refresh budget on the pooled path |
| `voxel.GI.VolumeRecentreCells` | 64 | dead-zone half-width (step 4, not implemented) |
| `voxel.GI.VolumeCheck` | 0 | numeric field-vs-volume equivalence harness. Non-zero arms it; the value is the sample count (1 = 4096). Runs once, re-armable by setting it back to 0 |
| `voxel.GI.VolumeCheckSettleSeconds` | 20 | delay after the first texel upload before the harness runs |
| `voxel.GI.VolumeDebugVis` | 0 | 1 raw irradiance, 2 validity, 3 checkerboard (keep step 1's pattern as a permanent ladder rung) |
