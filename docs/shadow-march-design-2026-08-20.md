# Ray-marched sun shadows — Phase 4 design

**Date:** 2026-08-20 · **Status: S1 BUILT, awaiting its paired build. Code:**
`Source/VoxelEarthShaders/Private/VoxelShadowMarch.{h,cpp}` + `Shaders/VoxelShadowMarch.usf`
(new files only; no owned file touched). Offline check: `tools/voxel-check-shadowmarch-shader.ps1`
— both kernels compile on real dxc (no engine stubs; the file's only engine include is
`/Engine/Public/Platform.ush`, the `VoxelBrickPack.usf:24` precedent) and the source-0 guard
refuses as designed. **The new `.usf` is inert to the current binary** (no built shader references
it), so legs on the current binary are unaffected; it becomes live input at the first editor boot
after the paired build — C++ and shader must go into one build slot together. S1's leg set is §7;
`voxel.Shadow.MarchVerify` stays 0 in timing legs.
**Parent plan:** `docs/ray-marching-plan-2026-08-19.md` §6, §11-P4
**Measurement basis:** `docs/measurements/armA-drawpath-ceiling-2026-08-19.txt` (SHADOWS: SOLVED section, 23:45) —
shadowless p50 18.89 ms → shadowed 34.72 ms (**+15.83 ms, +83.8%**), quads/camera-gather 10.9M → 31.9M
(**S = 2.929**), within-arm spread 0.07 / 0.01 ms. One caveat carried from that record: the shadowed leg ran
with `r.Shadow.UseOctreeForCulling 0`, one replicate pair, and its hitch pattern (70.3% of frames over the
33.3 ms threshold) has not had its own diagnosis. +15.83 ms is real; do not treat 34.72 ms as a settled budget.

---

## 0. The design in one paragraph

A single full-screen compute pass, after scene depth is complete, fires **one sun ray per shaded pixel** —
origin reconstructed from the depth buffer, direction to the sun — through the resident brick pyramid, and
writes a per-pixel visibility mask. That mask is **multiplied into the sun's screen shadow mask through a
light function material** — an engine-supported injection point verified end-to-end in this 5.8 source
(§2). Because the rays start from the depth buffer and the multiply happens on the light itself, **every
opaque receiver in the frame — terrain, player proxy body, agents, debris ISMs, thrown items — gets terrain
shadows with zero per-material work**, and it composes correctly with whatever conventional shadows remain
(props still cast via CSM into the same mask; the product of two mostly-agreeing binary masks is their
intersection).

Two deliberate departures from plan §6, with reasons:

1. **Per shaded pixel, not per primary hit.** The plan's phrasing ("one secondary ray per primary hit")
   only shadows terrain pixels and leaves the receiver problem unsolved. Firing from the depth buffer costs
   nearly the same ray count, solves receivers as a side effect, and — the decisive discovery of this
   design pass — **decouples the shadow march from the primary marcher entirely**. It works today, against
   raster terrain, with `voxel.March 0`. Phase 4 stops being the deepest-integration piece and becomes a
   separable pass that can be measured against the shipping renderer this week.
2. **Promoted from v2 to now.** §6 deferred marched shadows behind a hidden-caster mesher because the
   receiver problem looked unsolved and shadows looked cheap (they were off). Both premises fell last
   night: the receiver problem has a no-fork solution (§2), and the raster price is measured at +15.83 ms.

---

## 1. Where the ray fires, and what it costs

### Frame anatomy

```
prepass / base pass                       scene depth + GBuffer complete
  (marcher arm: + PreRenderBasePass march, PostRenderBasePassDeferred emit)
PostRenderBasePassDeferred_RenderThread   [cs] SHADOW MARCH        (this design)
  (BasePassRendering.cpp:1152)                 read SceneDepth + GBufferA normal
                                               1 ray/pixel -> R8 visibility mask
                                               copy into persistent RT
... engine: RenderLights (DeferredShadingRenderer.cpp:3467) ...
    per-light: shadow projections -> ScreenShadowMaskTexture     (props' CSM)
    RenderLightFunction              -> multiplies OUR mask in   (LightRendering.cpp:2379)
    RenderLight                      -> sun x shadow mask -> SceneColor
```

- **Own pass, never inside the primary march loop.** Three reasons. (a) Receivers: the primary loop only
  knows terrain pixels. (b) Wave shape: a SIMD wave costs its slowest lane; appending a second dependent
  ray to a loop that is already latency-bound extends exactly the lanes that are already the tail. A
  separate dispatch re-converges every wave at the surface. (c) Territory: `VoxelMarch.usf` and
  `VoxelMarchRenderer.*` are owned and under active edit; this design adds files beside them, not lines
  inside them.
- **Hook and ordering.** Same hook the marcher's emit uses. Extensions are sorted by
  `ISceneViewExtension::GetPriority()` (SceneViewExtension.h:237,346); the marcher leaves the default 0, so
  the shadow extension returns a negative priority to run after it. In the first measured step this is moot
  — with `voxel.March 0` scene depth is complete after the base pass regardless.
- **Ray construction.** World position from SceneDepth, offset **one level-0 voxel along the GBufferA
  normal** (the acne knob — in voxel units, exact, not resolution-dependent; pre-registered in the gate,
  §5). Direction = sun. `TMax = min(voxel.Shadow.MarchReachM, exit of resident volume)`; step budget cvar.
  Sky pixels (far-plane depth) exit before the first step — 58.7% of pixels at the measurement site.
- **Traversal.** v0 calls the proven flat level-0 walk — `VoxelMarchTraverseBrick`
  (`Shaders/VoxelBrickTraverse.ush:1123`), included read-only, resources bound via the public
  `GetGlobalVoxelBrickPool()` / `GetGlobalVoxelMarchChunkIndex()` seams (`VoxelBrickPool.h:1034`,
  `VoxelMarchChunkIndex.h:262`). A shadow ray is an **any-hit** query — it needs no face, AO, or material —
  so a leaner local loop over `VoxelMarchBrickSolidAt` is the likely v1; v0 uses the verified entry point
  first, so the gate inherits seven clean comparisons instead of starting from zero. **This design does not
  build on the in-progress hierarchical skip**; when it lands, the shadow walk adopts it and the reach
  extends.
- **Reach in v0: default 64 m** (inside R0's level-0 residency, ~87.5 m). At the pinned +38° sun a 64 m ray
  clears ~40 m of relief — near-field self-shadowing and player-scale shadows, not mountain shadows. That
  limitation is stated on every capture until reach rides the pyramid.

### Cost, stated honestly

Per-hit cost on this structure is **unmeasured, and the first step exists to produce it.** The only
defensible arithmetic shape: `rays ≈ shaded pixels` (0.41 × 1.35M ≈ 0.56M at the measurement site, bounded
by internal resolution), `cost ≈ overhead + mean-steps × per-step`, where both factors on the *shadow* ray
population (short, upward, surface-launched) are unknown. The primary march's `0.13 + steps × 5.9 µs` is a
**dead number for this path** (flat 1-bit volume, per the memory record) and is not inherited. Plan §6's
"+1–2.5 ms" is a **prior, not a prediction**. Two measured facts do transfer: the frame absorbs march-pass
ms ~1:1 (Arm B, Δframe tracked Δmarch within 0.4 ms over a 5 ms range), and the pass is latency-bound, so
it does not contend with the primary march for throughput. Memory: one R8 transient + one persistent R8
render target at scene extent (~1.3 MiB each). No new residency — the pyramid is already resident for
primary rays.

**Interaction with the primary march budget: none structural.** Separate dispatch, separate step-budget
cvar, shared read-only SRVs. The coupling is additive frame time, which is exactly what the A/B measures.

---

## 2. The injection seam — the receiver problem, solved without a fork

Plan §6's correction stands: **a receiver's material cannot march its own shadow ray** — deferred sun is
resolved in `RenderLights` from the GBuffer times the light's screen shadow mask, and nothing a base-pass
material writes survives into that multiply. The fix is to write the multiply's *other operand*.

### The verified chain (UE 5.8 source, this checkout)

| link | evidence |
|---|---|
| Sun's shadow mask is a screen-space texture; CSM projections render into it | `LightRendering.cpp:1901,2354` |
| A **light function** renders into that same mask, multiplicatively, after shadows | `LightRendering.cpp:2379`; blend via `FProjectedShadowInfo::GetBlendStateForProjection` (`LightFunctionRendering.cpp:437`) |
| LF materials that sample **depth or world position are atlas-incompatible** and take this classic screen-space path — the routing self-selects | `bDrawLightFunction`, `LightRendering.cpp:1853`; `CanLightUsesAtlasForUnbatchedLight`, `LightRendering.cpp:1372-1390` |
| An LF **forces the screen mask to exist** — the VSM one-pass elision explicitly yields (`!(bDirectLighting && bDrawLightFunction)`) | `LightRendering.cpp:1887` |
| The LF pixel shader reconstructs world position from `LookupDeviceZ(ScreenUV)` — **the same depth the march pass read** — and populates `MaterialParameters.ScreenPosition = mul(world, TranslatedWorldToClip)` | `LightFunctionPixelShader.usf:31-51`, `LightFunctionCommon.ush:74` |

So a light function material whose emissive is `SampleLevel(VoxelSunShadowMask, ScreenPositionToBufferUV, 0).r`
multiplies our marched visibility into the sun for **every opaque pixel on screen**, composed with the CSM
mask that props still write. UV alignment is inherent — both sides derive from the same depth texture.

### Plumbing (all new files except two routed requests)

- **`FVoxelShadowMarchExtension`** (new, `VoxelEarthShaders/Private/VoxelShadowMarch.{h,cpp}`) — view
  extension: compute pass + copy into a persistent `UTextureRenderTarget2D` sized to scene extent
  (recreated on resize), registered external to RDG.
- **`VoxelShadowMarch.usf`** (new) — includes `VoxelBrickTraverse.ush` read-only.
- **`M_VoxelSunShadowLF`** — MaterialDomain=LightFunction, one TextureObjectParameter + one Custom node,
  **DisabledBrightness = 1 (fail-lit)**. Authored once in the owner's editor (MCP session; I cannot and do
  not launch editors).
- **Assignment** — a new game-side component finds the sun (`TActorIterator<ADirectionalLight>`) and calls
  `SetLightFunctionMaterial(MID)`. No edit to `VoxelSkySubsystem.cpp` (owned elsewhere); **routed heads-up
  to its owner**: if that subsystem ever assigns a light function or respawns the sun actor, this seam
  needs to hear about it.
- **Sun direction into the compute pass** — read from the light component on the game thread each frame and
  enqueued. Registered trap: this is a *derived* copy of the direction the light itself shades with
  ([[derived-not-verified-detaches]]); the gate's mutation arm (§5) checks the pair agree.

### Reachability verdict: **REACHABLE, no engine fork.** Residual risks, named

1. **`r.LightFunctionQuality=0` lives in `[ShadowQuality@0]`** (`BaseScalability.ini:131-133`) — **the
   same scalability block whose staleness silently zeroed shadows for the project's entire measurement
   history.** The channel this design ships through is gated by the trap we just climbed out of. Required:
   pin `r.LightFunctionQuality=1` in the existing `DefaultEngine.ini [ConsoleVariables]` block (line 211;
   config edit is outside my territory — exact line supplied, routed to you), and the runtime gate reads
   the cvar's **last** write, per the recorded rule.
2. **Atlas routing is decided by material analysis** the game module cannot read back. The observable check
   instead: the mask-forced-to-0.5 mutation arm (§5) — if the screen does not visibly dim, the injection is
   not live, whatever the flags say.
3. **What the LF does NOT reach** — the engine's own comment (`LightRendering.cpp:1379-1382`): translucent,
   **water**, volume fog, clustered, and Lumen sample light functions **only from the atlas**, which our
   (atlas-incompatible) material is never in. Consequences in the receiver matrix below. Volumetric fog
   additionally evaluates directional LFs in **light space** (`VolumetricFogLightFunction.cpp`), where a
   screen-space sample would read garbage — `r.VolumetricFog.LightFunction=0` keeps fog on shadow maps.

### Receiver matrix

| receiver | terrain shadows come from | state |
|---|---|---|
| terrain (marched or raster-drawn) | marched mask | covered |
| player proxy body, `VoxelAgentSubsystem`, `VoxelDebris` ISMs, `VoxelThrownItem` | marched mask (they are opaque GBuffer pixels) | covered, zero per-receiver work |
| props onto terrain / each other | surviving CSM (they remain casters) | unchanged |
| water (SLW), ribbon actors | conventional shadows only — LF cannot reach them | **open**: either keep a hidden L0 terrain caster inside water reach (±25.6 m, [[voxelsim-near-field-water-reach]]) or accept the loss; owner judges captures (§3) |
| volumetric fog | shadow maps only | terrain god-rays degrade as terrain leaves cascades; near-field caster mitigates; owner judges |

---

## 3. What retires, what survives

| item | verdict |
|---|---|
| 4 cascade gathers over the pool: 21.0M quads/gather, the +15.83 ms | **retires** when terrain leaves shadow casting — this is the prize, and it must be *gone, not moved* (plan P4 rule) |
| `voxel.Stream.GPUShadowMaxLevel` | **demoted to the transition knob** (cap terrain casting while the march covers the near field), then retires — unless the water mitigation keeps an L0-only caster, in which case it survives at 0 permanently |
| `voxel.Sky.ShadowUpdateHz` | **cost basis retires; cvar survives** while any CSM remains (props). It exists to cap cascade re-render cost from sun motion; a marched shadow tracks the sun per frame at zero incremental cost. With prop-only cascades its setting stops mattering at current scales |
| shadow-map texel budget (`r.Shadow.MaxCSMResolution=2048`, 10 cascades pinned in DefaultEngine.ini:212-214) | **shrinks**: cascades stop needing to cover 4 km of terrain; `DynamicShadowDistanceMovableLight` can drop to prop scale — cheaper *and* sharper prop shadows |
| CSM machinery itself | **survives** — props as casters, water and fog as receivers |
| hidden-caster mesher idea (plan §6 v1: `SetCastHiddenShadow`, L0–L1) | **shrinks to the water mitigation**: L0 only, within ~26 m, if the owner wants terrain-on-water shadows before Phase 5. Double-darkening where CSM and march overlap is near-idempotent (product of agreeing binary masks); the disagreement band is CSM's bias vs the march's exactness, visible in captures, owner judges |

**Zero-code measurement available before any of this is built** (§7 step 0): a `voxel.Stream.GPUShadowMaxLevel`
sweep + `-VoxelShadowDistance` sweep at the standard pose = the cascade cost curve by ring level = the
exact refund schedule, and the standing price of the water-mitigation config.

---

## 4. Correctness gates — calibrated against measured noise, never zero

A marched shadow has no byte-comparable reference. Three gates, plus the mutation arms that prove each one
can fail.

- **G-S1 — replay gate (structural, the strong one).** Every Nth pixel, the pass records
  `(origin, dir, verdict, steps)` to a UAV. A verify path re-walks those exact rays on the **reference
  walker** — the same flat brick traversal the primary marcher certified against (seven consecutive clean
  1.35M-ray comparisons) — and compares hit/miss. **The noise floor is measured every frame, not assumed:**
  reference vs an entry-point-nudged (1e-6) copy of itself — entry-point, not segment-scale, because §2c
  measured the segment-scale perturbation understating the floor and the entry nudge is where accumulated-t
  divergence originates. Grades `AGREES / AT REFERENCE NOISE FLOOR / DISAGREES`; **refuses to report at
  zero samples** (the zeroed-counter trap); prints totals as total/count, never summed means.
- **G-S2 — cross-renderer statistics (owner-facing, not pass/fail).** Marched mask vs CSM mask, same pose,
  same sun. Disagreement is *expected* — CSM's bias and peter-panning against the march's exactness — and
  concentrated at shadow edges. Pre-registered before the first run: interior disagreement (outside a
  dilated edge mask derived from the CSM mask) must sit under a bar **calibrated from a CSM-vs-CSM
  replicate pair run in the same session** — the raster's own mask-to-mask noise is the denominator, per
  the project's hard-won rule. The 1-voxel normal offset band is exempted in writing before the run, not
  after the first failure.
- **G-S3 — shadowed-fraction scalar.** Fraction of shaded pixels in shadow, per leg, logged. A regression
  canary, cheap enough to run always.

**What makes each gate fail — stated now, because tonight produced four checks that could not fail:**
sun vector deliberately rotated 5° in the verify arm → G-S1 must report DISAGREES; brick SRVs deliberately
unbound (zeros = air = all lit) → G-S3 must flag an implausible fraction; mask forced to 0.5 in mode 2 →
the screen must visibly dim (proves the LF multiply is live end-to-end). All three mutation arms run once
per configuration change, and their results are recorded next to the gate's.

Per [[voxelsim-owner-judges-screenshots]]: A/B captures with conditions go to the owner; the gates certify
consistency with the volume, not the look.

---

## 5. Phasing within Phase 4

| step | what | produces |
|---|---|---|
| **S0** | zero-code cvar sweeps (owner-run) | cascade cost by ring level; noise band replicates |
| **S1** | mode-1 pass, **new files only**, no injection, no material, raster terrain | **the ms number** (§7) |
| **S2** | LF material + assignment + mode 2 | injected shadows; mutation arm 3; owner captures near-field |
| **S3** | terrain out of cascades (`GPUShadowMaxLevel` cap), march covering near field | the refund arm: is +15.83 ms *gone, not moved*; G-S2 captures |
| **S4** | reach extension over the landed hierarchical skip; soft edges via cone jitter into TSR history (off in v0 — binary visibility keeps G-S1 binary) | far shadows; look pass |

Cvars: `voxel.Shadow.March 0/1/2` (off / march-to-scratch + stats / injected),
`voxel.Shadow.MarchReachM` (64), `voxel.Shadow.MarchBudget`, `voxel.Shadow.MarchVerify` (G-S1 sample-N),
`voxel.Shadow.MarchStats`.

---

## 6. Seam requests (routed through the owner)

1. **Marcher owner:** my new `.usf` includes `VoxelBrickTraverse.ush` read-only and calls
   `VoxelMarchTraverseBrick(OriginLocalUU, DirWorld, TMinUU, TMaxUU, ConeSlopeUU)` plus the loose-global
   binding names (`VoxelBrickDesc/Occ/Mat/ChunkTable`, `MarchChunkIndex`, `MarchIndexDimChunks`,
   `MarchBrickOriginVoxel`). I need signature-change notice, not a freeze. An any-hit entry point would be
   welcome later; **not needed for S1**.
2. **VoxelSkySubsystem owner:** heads-up that a light function material will be assigned to the sun from
   outside; tell me if the subsystem ever touches `SetLightFunctionMaterial` or respawns the sun.
3. **Config edit (outside my territory):** add `r.LightFunctionQuality=1` to the existing
   `[ConsoleVariables]` block at `ue-project/Config/DefaultEngine.ini:211`, beside the r.ShadowQuality pin
   and for the identical reason.
4. **One editor task at S2:** author `M_VoxelSunShadowLF` (MCP session in the owner's editor).

Build discipline, restated as commitment: I announce before the shader tree changes; the `.usf` and its
C++ land together; one build slot requested; no leg until "tree matches binary" is confirmed.

---

## 7. The smallest first step that produces one measured number

**S1 — the mode-1 pass.** New files only (`VoxelShadowMarch.{h,cpp,usf}`); no injection, no material asset,
no owned files, no dependency on the in-flight skip work; runs against **raster terrain** (`voxel.March 0`).

The leg set, all owner-run through the standard harness, standard site/pose, shadows ON (the shipping
default as of last night):

| leg | config | reads |
|---|---|---|
| C0a, C0b | `voxel.Shadow.March 0`, replicate pair | **the noise band, run not assumed** |
| M1a, M1b | `voxel.Shadow.March 1`, reach 64 m, replicate pair | pass cost = M1 − C0 p50, judged against the C0 band |
| — | both legs also log: pass ms from an `RQT_AbsoluteTime` ring (`VoxelFluidRender.cpp:470-495` precedent), rays fired / active / hit, step histogram (mean and p95, printed total/count) | the per-step cost model **for this ray population on this structure** |

Deliverable sentence the step is designed to let us write: *"sun shadow rays march at level 0 over the near
ring in X.XX ms at the standard pose (band ±Y.YY), mean S steps per active ray"* — with the control run in
the same session. G-S1 replay gate and its sun-vector mutation arm are part of S1, so the first number
lands already certified against the reference walker.

What S1 does **not** claim: any visual result (mode 1 draws nothing), any refund (terrain still casts), any
far-field reach. Those are S2/S3 and each carries its own control.

---

## 8. S1 MEASURED (2026-08-20) — and the startedInside diagnosis

**The cost number:** `s1-shadow-c0b` 34.86 ms → `s1-shadow-m1b` 40.82 ms, **+5.96 ms**, against a pass
census of **gpuMs 5.397** (windows 5.391/5.392/5.397, max 5.57 after warmup) — the frame absorbs the pass
~1:1, matching Arm B's behaviour for the primary march. Budget never bound (`exhausted=0`). The march cost
is measured, tight, and replicated. **Replacement is still a projection** (~15.83 − 5.96 ≈ 9.9 ms) until
the march-on/CSM-off arm runs: `voxel.Shadow.March 1` + `voxel.Stream.GPUShadowMaxLevel -1` (every pool
chunk is above the cap → zero shadow quads; note the four gather *walks* still run — the cvar's own text
says `shadowGathers>0 with shadowQuads=0` — so that arm slightly UNDERSTATES the true retirement).

**The alarming field, decomposed.** `inside/f = 328,288` = 53% of marched rays. Two readings from the same
census that constrain it:

- `hit% = 59.2` **includes** startedInside (the traversal sets `bHit` when the origin is solid). True
  marching hits are ~6.2% of marched — a plausible shadow fraction at a 38° sun. **The walk is sane; the
  origins are the problem.**
- Non-resident chunks read as AIR in this traversal (`VoxelBrickTraverse.ush`, "outside the resident set
  is air"), so inside means the origin voxel is **genuinely solid in the bricks** — geometry, not residency.

**Mechanism hypothesis — the pixel footprint crosses the voxel size at range.** One pixel subtends
`TSurf × coneSlope ≈ TSurf × 2/873` UU at the internal resolution: ~0.7 voxels at 32 m, ~1.5 at 64 m.
Amplified terrain is rough at exactly voxel scale, so beyond ~30 m the depth sample is one point on
sub-pixel staircase relief and a reconstructed origin lands inside that relief roughly half the time. The
marched population is area-weighted toward range, so ~53% is quantitatively consistent. A fixed 1-voxel
offset cannot fix an error that grows with distance.

**Pre-registered predictions, stated before the legs run:**

1. The new inside-by-distance census bands (near < 16 m / mid 16–32 / far 32–64 at reach 64): the
   footprint hypothesis predicts **near ≈ 0 and far carries the population**; a systematic depth/normal
   bias predicts all three bands populated alike.
2. The offset sweep (`MarchNormalOffsetVoxels` 2, 4): **partial collapse only**, roughly one distance
   shell per doubling — it cannot reach zero, because the error grows with distance.
3. The fix candidate, `voxel.Shadow.MarchPullbackPx` (new, default 0 = the measured S1 configuration):
   pull the origin toward the camera by N pixel footprints before the normal offset — any point on the
   camera ray in front of the depth sample is air **by visibility**, and the shift is sub-pixel on screen
   by construction. At 1.5: **inside collapses in all bands** (residual = genuine concavities), true hit%
   stays ~6–8%, gpuMs approximately unchanged. If inside does NOT collapse at 1.5, the hypothesis is
   wrong and the next suspect is the depth/normal chain itself.

**Diagnosis leg set (D-legs), after the paired build:** D1 = M1 rerun (new census prints the bands — the
discriminator, zero config change); D2 = offset sweep 2/4; D3 = `MarchPullbackPx 1.5`; R1 = the
replacement arm above. Captures for the owner wait until inside is understood — a mask where half the
near field reads shadowed is a fast wrong answer, not a feature.

### 8a. D1/D3 RAN — the footprint hypothesis is FALSIFIED. Recorded, not erased.

```
D1  inside/f=328,288  near=209,577  mid=101,622  far=17,089     (prediction was near ~ 0, far-heavy)
D3  pullback 1.5 px: inside 328,288 -> 302,792 (-7.8%, uniform across bands), gpuMs 5.491 -> 5.630
```

Near carries 64%, monotonically decreasing with distance — the **inverse** of a footprint effect. And
the pullback result is stronger than a miss: for a point on any visible face the view ray has a
component off that face, so even the near band's 2.7 UU pullback rescues every boundary-ambiguity
inside. Only ~8% were rescued, uniformly. **~92% of insides are buried deeper than a ~2-voxel view-ray
shift — these origins are well inside solid, worst where geometry is closest.** Meanwhile the marcher's
own depth gate certified bricks ≈ raster along view rays to <0.5 voxel within 25.6 m — the same range
where the insides concentrate. That contradiction is not resolvable by more theory.

Two candidates eliminated by reading: water columns in bricks (**there is no MAT_WATER** — the material
enum is closed, `amplifier.h:492`; water never enters Cells) and non-residency (non-resident is AIR in
this traversal). The remaining candidate space is discriminated by instruments, not argument.

### 8b. The diagnosis instruments (built, awaiting the paired build) — leg D4

`voxel.Shadow.MarchDiag 1` (default 0 — the timing configuration is untouched) adds, for every inside
ray: **per-band denominators** (`shadowbands:` prints inside/marched as a RATE per band — the field D1
could not answer), a **burial-depth histogram** (first air voxel straight above the origin: 1/2/3/4+),
an **inside-voxel material split** (BEDROCK/ROCK = interior stratigraphy vs surface materials), and a
**64-record example dump** per frame (`shadowdiag: exN ...` — pixel, TSurf, decoded GBuffer normal,
N·L, material, burial, band, and the WORLD level-0 voxel in voxels and metres).

**Pre-registered discriminations, stated before D4 runs:**

| observation | reading |
|---|---|
| burial mostly **1**, surface materials | boundary/off-by-one class: origins sit exactly one voxel under first air — lattice-scale disagreement (normal aim, quantisation, rounding) |
| burial mostly **4+/15**, BEDROCK/ROCK | deep-interior class: the world-position→voxel mapping is wrong somewhere — decided by the offline cross-check below |
| per-band RATES roughly equal (raw ordering explained by ray counts) | the mechanism is uniform and D1's near-dominance was the missing denominator — the pose reading |
| near RATE >> mid/far RATES | genuinely near-specific — pose geometry (steep look-down) or a term that shrinks with distance |
| example normals not axis-like on terrain | the GBuffer normal chain is the suspect after all |

**The dump's real purpose is the offline arbiter:** each record carries the world voxel, so a handful
can be checked against the CPU reference world with the sanctioned vxc probes (never a Python rebuild —
[[voxelsim-never-rebuild-ground-in-python]]). Brick says solid + CPU world says solid ⇒ the
reconstruction is wrong. Brick says solid + CPU world says air ⇒ the bricks are wrong — which the
primary marcher's gate would then also need to explain. That fork is the whole diagnosis.

### 8c. D4 RAN — every standing hypothesis dead, and the mechanism is now cornered

```
shadowbands: marched/f near=389,174 mid=193,700 far=36,749
             inside RATE near=53.9%  mid=52.5%  far=46.5%      <- EQUAL. D1's near-dominance
                                                                  was the missing denominator
burial histogram, 128 sampled records: burial=1, ALL 128
```

**Withdrawn, not adjusted:** my deep-burial inference from the pullback ("~92% buried deeper than a
2-voxel shift") was wrong — burial is uniformly exactly one voxel. The pullback's 8% rescue rate is
explained by incidence angle, not depth (see below). The coordinator's pose hypothesis (near genuinely
worse) is falsified by the equal rates. The footprint hypothesis was already dead (§8a).

**The cornered mechanism: the one-voxel offset jumps OVER the only voxel that is provably air.** The
voxel adjacent to a visible face — the one the camera saw the face *through* — is guaranteed air by
visibility. An offset of exactly 1.0 voxel from a point ON the face lands the origin at that air
voxel's FAR boundary, i.e. on the surface of whatever is next. On amplified 10 cm terrain, "whatever is
next" is the neighbouring micro-column, solid at that height about half the time. This predicts every
observation at once: rates ~50% and distance-free (geometry statistics, not precision), burial exactly 1
(the origin lands in a surface-shell voxel whose top is exposed), pullback rescuing only ~8% (its
along-normal component is `2.7 UU × cos(incidence)`; most faces are seen at grazing incidence), and no
brick/raster disagreement anywhere — bricks, raster and reconstruction are all correct; the offset
arithmetic overshoots.

Two sub-models, distinguished by the dump records' normals (requested, not yet seen):
(a) exact axis normals, side-face overshoot into the next column; (b) material-tilted normals on top
faces, whose lateral component (`10 × sin(tilt)` UU at offset 1.0) lands in the adjacent bump.

**Pre-registered predictions for the offset sweep, stated while the 1.5 leg is in flight:**

| offset (voxels) | coin-flip model (coordinator) | overshoot model (this section) |
|---|---|---|
| 1.0 (measured) | ~50% inside | ~50% × P(next voxel solid) |
| **1.5 (running)** | **collapses in all bands** | **does NOT collapse — flat to WORSE** (1.5 voxels from a side face is the *middle* of the next column's voxel; the coin's ambiguity is replaced by certainty) |
| **0.5 (next, cvar-only)** | — | **collapses in all bands** — the origin sits at the CENTER of the seen-through air voxel, the one place visibility proves is empty. Residual = true concavities + sub-model (b)'s lateral term |

If 1.5 does not collapse and 0.5 does: fix = default `MarchNormalOffsetVoxels` to 0.5 (never an integer)
— one default change, no shader edit. If 0.5 collapses only partially and the dump shows tilted
normals: sub-model (b) is live and the robust fix is snapping the offset direction to N's dominant axis
(one shader line, needs a build). If neither collapses, the reconstruction chain returns as suspect and
the offline vxc cross-check of the dump records is the next move.

### 8d. R1 MEASURED — the replacement number is real

```
CSM cascades on,  march off:  p50 34.86 ms
CSM terrain off,  march on:   p50 24.97 ms      -9.89 ms  (projection said ~9.9)
terrain shadow quads per gather: 21,021,619 -> 54
```

Recorded caveats: the four gather walks still run (`GPUShadowMaxLevel -1` culls quads, not gathers), so
the true retirement is slightly larger; and mode 1 injects nothing — R1 is a frame-time measurement, not
a picture. The visual arm (S2 injection + captures, owner judges) waits on the startedInside fix.

### 8e. ROOT CAUSE FOUND — the GBuffer read was a black system dummy. Fixed.

The offset-sweep legs (1.0 → 53.9%, 1.5 → worse incl. far 64.3%, 0.5 → mixed) falsified BOTH standing
models, and the dump survey settled it: **128 of 128 records, across frames, byte-identical
`N = (-0.58, -0.58, -0.58)` = `normalize(2·(0,0,0) − 1)`** — not a wrong normal, NO normal, laundered by
a normalize into a plausible unit vector. Verified against the 5.8 source:

- the UB handed to `PostRenderBasePassDeferred` is built with **SceneDepth only**
  (`DeferredShadingRenderer.cpp:2499`); unset GBuffer slots fall back to **`SystemTextures.Black`**
  (`SceneTextures.cpp:1108`) — NON-NULL, so the null-check was a check that could not fail;
- the tempting repair, `FXRenderingUtils::GetOrCreateSceneTextureUniformBuffer(..., GBufferA)`,
  **ignores the requested setup mode** whenever the scene has a UB (`FXRenderingUtils.cpp:109-118`);
- the primary marcher's emit hit the identical defect the same night and its fix block
  (`VoxelMarchRenderer.cpp`, "WHERE THE TARGETS COME FROM") is the precedent this fix now follows.

Every D-leg observation reconciles: constant `NdotL=0.06` (one vector against a fixed sun),
`backface=0` (one vector separates nothing), ~50% inside at burial exactly 1 (a diagonal offset clears
0.58 voxels per axis and exits no voxel), rates distance-free, and no offset scalar along a fixed wrong
direction able to fix anything.

**The fix (in tree, awaiting paired build):** GBufferA is taken from the hook's `RenderTargets` MRT
bindings — the set the base pass actually drew through — at the slot from
`FSceneTexturesConfig::Get().GBufferBindings[GBL_Default]` (the marcher's fix), then **cross-checked by
the engine's own debug name and by extent against scene depth** (rejects any 1×1 dummy), with a one-time
attestation log (slot, name, extent, format) and a hard, counted, log-once decline on any mismatch. Plus
an always-on census canary: `nonAxisN/f` counts marched rays whose decoded normal has no dominant axis —
on quad terrain it must sit ~0, and against a black dummy it reads 100% in the first window. **No
axis-snapping anywhere** — snapping a constant produces a constant and masks precisely this defect.
`MarchNormalOffsetVoxels` defaults to **0.5** with the derivation in its help text: with real face
normals, the half-voxel puts the origin at the center of the voxel the camera saw the face through —
guaranteed air by visibility; an exact 1.0 lands on that voxel's far boundary. Never an integer.

**COST NUMBERS NEED ONE RE-MEASUREMENT, stated before anyone quotes them onward:** every march cost so
far (gpuMs ~5.4, frame +5.96, and R1's 24.97) was measured with **53% of marched rays terminating at
step zero** (startedInside is an instant exit). Post-fix those rays march for real; backface rays newly
exit early instead. Net direction unknown — likely up. The measured numbers remain true of their
configuration; the shipping configuration is the post-fix one.

**Pre-registered for the post-fix leg set (F-legs):**

| leg | expectation |
|---|---|
| boot log | `GBufferA bound from MRT slot N, name='GBufferA', extent=<scene extent>` — extent NOT 1×1 |
| F1 = M1 rerun | `nonAxisN/f ≈ 0` (<1% of marched); `backface/f > 0` at last; inside rate collapses to low single digits in ALL bands at offset 0.5; `hit%` falls from 59.2 to a true shadow fraction (~5–10% at this pose, from D1's own arithmetic: 59.2 − 53.0); **new gpuMs recorded as THE cost number** |
| F2 = R1 rerun | the replacement saving re-measured with real origins |
| F3 = diag leg | dump records show varied, axis-like normals; burial histogram of the residual insides names what is left (true concavities expected) |

If F1's inside rate does not collapse, the reconstruction chain returns as suspect and the offline vxc
cross-check of dump world-voxels is the next move — that instrument is already built.

### 8f. F1/F2 LANDED — every pre-registered criterion met on the first run

```
F1: GBufferA bound from MRT slot 1, name='GBufferA', extent=1552x880, format=A2B10G10R10
    nonAxisN/f=0.0   backface=194,028   inside RATE 0.8/1.7/1.6%  (66x collapse)
    hit%=3.4   gpuMs 6.502 (up from 5.397 as pre-flagged -- THE cost number)
F2: p50 26.01 ms

SHIPPING TABLE (all caveats recorded in the measurement file):
  shadowless        18.89 ms
  CSM cascades      34.86 ms   +15.83
  marched, correct  26.01 ms   + 7.12
  REPLACEMENT SAVING  8.85 ms  -- the feature is 2.22x cheaper marched
```

Residual inside at 0.8–1.7% is characterisable (F3's residue histogram queued, not blocking).

---

## 9. S2 — the light-function injection (BUILT, awaiting material + build + legs)

**Code (no shader change — the tree is untouched since the F-round build):**

- `Tools/create_sunshadow_lf_material.py` (new) — authors `/Game/Voxel/M_VoxelSunShadowLF`, the
  family idiom (checked connects, recompile, save, read-back verification). Three load-bearing choices
  documented in its header: the **WorldPosition atlas anchor** (a Custom node's HLSL is opaque to the
  material analyzer — `HLSLMaterialTranslator.cpp:1769` infers atlas compatibility from flags only real
  expression nodes set, and an atlas-routed LF evaluates in light space where the screen-UV sample is
  garbage; compiling WorldPosition sets `bUsesVertexPosition` at `:6276` and forces the classic path);
  the **white fail-lit default texture**; and the **parameter-name contract** `VoxelSunShadowMask`
  (mirrored as `kSunShadowMaskParamName` — `SetTextureParameterValue` on a missing name is a silent
  no-op, so one grep must always find both files).
- `VoxelShadowMarch.{h,cpp}` — mode 2 is real: the render thread publishes the buffer extent, copies
  the mask into a persistent `UTextureRenderTarget2D` (created `InitCustomFormat(PF_R8)` explicitly —
  the `RTF_R8` enum maps to PF_G8, `TextureRenderTarget2D.h:50`, and would fail the copy's format
  match); the subsystem loads the checked-in material, wires an MID via `SetLightFunctionMaterial`, and
  sets **DisabledBrightness = 1** (the engine default 0.5 would read as a permanent half-shadow when
  the LF fades — the mutation arm's signature arriving as a bug). Resize is unpublish-first/republish-
  next-tick so the render thread never derefs a resource mid-recreation. Missing material = declined
  and said, never run-as-mode-1: `shadowinject: declinedNoTarget` counts every frame. Mode leaving 2 —
  including the mode-0 early return — unwires the sun.
- `voxel.Shadow.MarchMutateInjection 1` — the injection mutation arm: fills the target with 0.5
  instead of the mask. **Direct sun must read half-dark everywhere; a normal-looking frame means the
  injection is not live**, whatever the counters say.

**Sequencing (owner's box):**

1. One-time editor commandlet: `UnrealEditor-Cmd.exe <uproject> -run=pythonscript
   -script=ue-project/Tools/create_sunshadow_lf_material.py -unattended -nop4 -nosplash` — then
   **check in `Content/Voxel/M_VoxelSunShadowLF.uasset`**. The asset is generated-once-and-tracked,
   per the coordinator's constraint; regeneration is idempotent (delete-and-rebuild).
2. Build slot: **cpp-only** — `VoxelShadowMarch.{h,cpp}` changed, the `.usf` did not; the offline
   check still passes on the unchanged tree.
3. Legs, in this order:

| leg | config | reads |
|---|---|---|
| **I0 (mutation)** | mode 2 + `MarchMutateInjection 1` | capture MUST show direct sun at half everywhere; census `shadowinject: mutated>0` with the inline warning. Run FIRST — no capture is trusted before the chain is proven able to fail |
| **Cal-A/Cal-B** | `March 0`, cascades on, two captures same pose | imgdiff of the pair = the raster's own replicate noise — **the G-S2 calibration bar, run not assumed** |
| **V1** | mode 2 + `GPUShadowMaxLevel -1` | the replacement LOOK. imgdiff vs Cal-A, judged against the Cal bar; captures + conditions to the owner — per the standing rule, no verdict from us |
| census watch | — | boot: `GBufferA bound...` + `INJECTION LIVE`; steady: `shadowinject: copies` ≈ frames, `declinedNoTarget` ≈ 0 after warmup |

**Stated on every V1 capture (the look-limitations of this stage, not defects):** 64 m reach — casters
beyond it cast nothing; hard edges (no penumbra until cone jitter lands); water, translucency and
volumetric fog receive no marched shadows (the atlas-only LF systems — `LightRendering.cpp:1379`);
props still cast via CSM unchanged.
