# W3, visible half: river channel carving

> **Still the reference for `channel.h`; its status lines are stale
> (2026-08-05).** The geometry, the three defects it fixed, the argument for not
> putting a channel term in `evalSurface`, and the wiring recipe are all still
> good. What has moved:
>
> * **`kWorldGenVersion` is now 23**, not the 10 this document says. That
>   sentence was true when written and is quoted elsewhere as if it still were.
> * **`channel.h` is still not wired into `evalSurface`.** That part has not
>   changed.
> * **The laws now have a second home and a different anchor.**
>   `terrain-service/terrain_service/bake/water.py` mirrors this file's Q8 fixed
>   point in floating point — same exponents (102/256 and 90/256), same caps,
>   same ¾ waterline — but re-anchored on a **perennial discharge of 10 L/s**
>   rather than on `kRiverAccumThresholdDefault`. That is a change of anchor,
>   not of law, and the bake is what actually decides where water goes today.
> * The `Q` values in this document's tables are **flow-accumulation units**, not
>   m³/yr. Do not compare them with the bake's discharge figures.
>
> How the water system works end to end: `docs/watershed-system-plan.md`.

**Status, 2026-07-29.** `voxelcore/channel.h` + `src/channel.cpp` landed:
discharge-driven channel geometry, a graded bed that descends strictly to the
sea, and a cut/fill cross-section whose banks hold water. Tested
(`tests/test_channel.cpp`, 16 cases) and measurable (`vxc_riverprobe`).
**Not yet wired into `evalSurface`** — see "The wiring bump" below for what
that costs and why it was deliberately not taken in this pass.

`kWorldGenVersion` is **untouched** (still 10). No golden moved, `worldgen.ush`
needed no mirror, no SPIR-V respin. The diff is three `CMakeLists.txt` lines
and four new files.

---

## What this is

The plan asks for "rivers = flow-accumulation routing, discharge from upstream
precip sets width/depth, amplifier carves channels"
(`voxel-earth-implementation-plan.md` §3.7 Generation; §3.1 step 3 says
"riverbed carving along flow lines"). The routing half already existed:
`rivernet.h` builds a D8 flow-accumulation segment graph with a per-segment
discharge, and its header comment reserves exactly this slot — "a later
W3-proper pass will read [this] to drive CA/SWE **and per-voxel channel
carving**".

This is that pass, terrain side only. It produces the bed; the rivernet↔water-CA
coupling fills it. It does not touch `rivernet.{h,cpp}` or `waterca.{h,cpp}`.

## Geometry, in one screen

Width and depth are power laws in discharge — Leopold & Maddock downstream
hydraulic geometry, with the same exponents the server bake already uses
(`terrain-amplification-plan.md` B2: width ∝ A^0.4; the design doc's depth
∝ A^0.3–0.4, midpoint 0.35), so a future wiring pass agrees with the bake
rather than fighting it:

    width = 1500 mm · (Q/Q_threshold)^0.40      depth = 300 mm · (Q/Q_threshold)^0.35

anchored at `kRiverAccumThresholdDefault`, the discharge at which rivernet
decides a river exists at all — so the reference channel is a channel at
initiation, and a headwater trickle can never come out as the same trench as a
major river. Measured across four decades:

| catchment | width | depth | ideal width | ideal depth |
|---|---|---|---|---|
| 1× threshold | 1500 mm | 300 mm | 1500 | 300 |
| 10× | 3744 | 670 | 3768 | 672 |
| 100× | 9376 | 1512 | 9464 | 1504 |
| 1000× | 23488 | 3400 | 23773 | 3366 |
| 10000× | 58880 | 7632 | 59716 | 7536 |

59 m wide and 7.6 m deep for a major river is a physically sensible river. The
law tracks the ideal power law to better than 1.6% and is strictly monotone,
computed through integer `log2Q8`/`exp2Q8` with compile-time mantissa tables —
no `pow`, no floats, CI job `float-ban` clean.

Cross-section, at perpendicular distance `d` from the reach centreline:

    cutTarget(d)  = bed                          for d <= width/2
                  = bed + (d - width/2) / 2      beyond            (2:1 bank)
    fillTarget(d) = min(cutTarget(d), bed + depth)

    carved = natural > cutTarget  -> cutTarget                (excavate)
             natural < fillTarget -> min(fillTarget, natural + 4 m)  (embank)
             otherwise            -> natural

## The three things that were wrong and got measured

**1. The bed datum.** The first cut referenced the bed to the tile-pixel
elevation rivernet routes on. On the 30 m tier the amplifier's own detail
swings the ground tens of metres either side of that, so a 300 mm headwater
channel ended up floating in mid-air or buried. The bank test reported **8107
leaking sides**. Referencing the *amplified* surface instead (`IChannelSurface`,
`channelSurfaceOf(amplifier)`) fixed the datum.

**2. Continuity is not free.** With the tile datum, strict downstream descent
was a theorem (D8 edges strictly descend, discharge is non-decreasing, so
`elevation - depth` strictly descends). Against the amplified surface it is
not, so the build does one explicit downstream pass:

    bed[n] = min(surface(n) - depth(n), bed[upstream] - minDrop(reach))

The topological order is free — D8 always targets a strictly lower tile pixel,
so descending tile elevation visits every node after all its contributors.
Sills in the way are cut through, which is what a river does; `sillsCut` and
`maxCutBelowSurfaceMm` report how often and how deep so an over-aggressive cut
shows up as a number rather than as a canyon someone notices later.

**3. The fill has to run past the rim.** Stopping the fill at the rim leaves a
knife edge — the rim is at `bed + depth`, but if ground falls away immediately
outside it the water pours straight over. That was **7763 leaking sides**.
Continuing the fill outward at rim height until natural ground overtakes it (an
embankment that daylights) closes the wetted perimeter. `kChannelMaxFillMm`
bounds it by *height*, so on gentle ground it reaches as far as it needs to and
on a cliff edge it stops and is counted rather than building an aqueduct.

The bank test itself was also wrong at first: casting rays perpendicular from a
centreline compares a column 40 m out against the water line of the part of the
channel it started from, not the nearer, lower part it is actually next to, and
invented leaks. Stated **locally** — wherever the design profile puts ground at
or above the local water line, the carved ground must be there too — it has no
such coupling.

## Measured, on real terrain

`vxc_riverprobe --synthetic 20260719 192 --origin -800 -96` (a 5.76 km window
straddling a coastline):

| | |
|---|---|
| nodes / segments / outlets | 2358 / 1905 / 258 |
| bed strictly descending | **YES** |
| centreline columns walked | 571,500 |
| **gaps** | **0** |
| **columns left above their own bed** | **0** |
| bank columns swept | 5,467,427 |
| below the water line | 10,783 (**0.20%**) |
| reaches at/below sea level | 1609 / 1905 |
| outlets stranded above sea level | 56 / 258 |

The 0.20% residue is where ground falls away by more than the 4 m fill bound
inside the embankment footprint — deliberately left short rather than grown
into an aqueduct. `test_channel.cpp` guards it at under 1%.

The 56 stranded outlets are almost entirely **region-window artifacts, not
defects**: rivernet routes only inside its bounds, so any chain that leaves the
5.76 km window simply terminates there. On an inland window every outlet is
"stranded", correctly — no river reaches the sea inside a box with no sea in it.
The probe says so explicitly rather than reporting a false failure.

Cross-compiler determinism: the channel digest is byte-identical between MSVC
and clang (`f1f442bbedcfce69` on the 96-px window), as the doctrine requires.

## The showcase fixture

    vxc_riverprobe --synthetic 20260719 192 --origin -800 -96 --profile

picks the longest chain that starts on dry land and ends below sea level and
prints its long profile. Currently a 21-node river:

    i    bed_mm     Q         w_mm   d_mm
    0    130598     32741     1708   335     <- headwater, +130.6 m
    ...
    10   1683       111818    2784   516     <- last node above sea level
    11   -11416     132140    2976   546     <- crosses z=0
    ...
    20   -99871     355824    4416   776     <- mouth, -99.9 m, open to the ocean

Bed falls monotonically at every step; the channel grows 1708×335 mm →
4416×776 mm as discharge grows 10.9×. The mouth is strictly below sea level, so
the ocean connects into the channel instead of being separated from it by a lip.

## Why this is not a term in `evalSurface`

A channel's existence at (x, y) depends on square kilometres of upstream
terrain, so it is not a point function. `evalSurface` and its HLSL mirror in
`voxel-core/shaders/worldgen.ush` are built entirely out of point functions of
(seed, x, y) plus a bounded tile stencil, and the cross-vendor determinism gate
covers `evalSurface`, `materialAt` *and* meshed bricks. Carving inside
`evalSurface` therefore means shipping the segment graph to the GPU as a new
per-region buffer.

So the non-local work happens **once per region** in `ChannelField::build()`,
and is baked into a bounded spatial index; `sampleAt()` afterwards is an O(1)
point query over a handful of candidate reaches — the same shape as a tile
fetch, and mirrorable as such. This is the precedent `carrier.h`,
`detail_rill.h` and `detail_bedding.h` all set: land the bounded, tested
geometry first, wire it in one deliberate bump later.

Three things made taking the bump in this pass the wrong call:

1. **The GPU binding table is full.** Vulkan bindings 0–10 are all consumed
   (`worldgen.ush:1073-1100`); a new SRV lands at 11, plus `FillRasterWindow`,
   `FVoxelGpuRegionRequest`, `ValidateRegionRequest`, the RDG upload, three
   per-pass `FParameters`, and three hardcoded descriptor-layout arrays in
   `gpu_harness.cpp`.
2. **Another session is mid-flight in exactly those files.** In the 24 h before
   this branch: `amplifier.cpp` ×8, `worldgen.ush` ×5, `test_amplifier.cpp` ×6,
   and the **binary** `.spv` prebuilts ×4. Binary conflicts do not merge.
3. **There is precedent for how that ends.** `origin/claude/erosion-v7` took the
   bump route — 341 lines into `amplifier.cpp`, HLSL mirror, SPIR-V respin,
   goldens re-pinned across five test files. It finished, never merged, and is
   now unmergeable; `core.h:40-43` still carries the note about the version
   number it stranded.

## The wiring bump, when it is taken

Everything below is additive; nothing here needs redesign.

1. `Amplifier` gains an optional `const ChannelField*`, built per streamed
   region and cached alongside the tile memo. `evalSurface` (or better,
   `column()`, so the bed can also set `surfaceMat`) applies
   `field->surfaceMm(vx, vy, natural)` — one function, already the single
   definition the applicator, the tests and the probe all share.
2. Bed materials: `kChannelBedMaterial` (`MAT_GRAVEL`) for the top
   `kChannelBedLinerMm` of the bed, `kChannelBankMaterial` (`MAT_CLAY`) for
   built embankment. Both IDs already exist; `MAT_CLAY`'s comment in `core.h`
   reserves it for "future floodplain/riverbank biomes".
3. GPU mirror: upload the reach list + index as one SRV at binding 11, filled
   in `FillRasterWindow` (the header says that is the one place the rule may
   live). `channelTargetMm`, `log2Q8`/`exp2Q8` and the mantissa tables are
   straight integer transcriptions.
4. Bump `kWorldGenVersion` and `VXC_WORLDGEN_VERSION_USH`, respin SPIR-V
   (`tools/compile-shaders.ps1`), regenerate goldens
   (`tools/regen-goldens.ps1`), re-pin `kExpectedCpuDigest`
   (`VoxelGpuVerify.cpp:63`), and fold `kChannelVersion` into it.
5. Batch it with whatever else is pending — the plan's own advice, since every
   bump invalidates saved edit logs.

**Alternative worth evaluating first:** the baked `.vxtl` v2 **flow plane**
already exists and is shipped on every production tile (`docs/vxtl-v2-format.md`
§6: one `uint8` per fine pixel, log₂ flow accumulation plus channel/bank/
deposition bits), decodable via `FineTile::decodeFlowBlock` — and today has
**zero consumers**. If the bed can be driven from that raster instead of from
the segment graph, it becomes a genuine point function of tile data, the GPU
mirror is a texture fetch rather than a graph upload, and the bump gets much
cheaper. What it cannot give on its own is the *graded, monotone* long profile
this module builds, which is what makes water actually flow rather than pool —
so the likely end state is the flow plane for where channels are and this
module's profile pass for how deep.

## Coordination note

The bed geometry is deliberately keyed off **rivernet's own segment ids and
discharges**. Whatever the rivernet↔water-CA coupling puts water into,
`ChannelField` has already cut a bed for, at the same place, with a water line
it can query directly: `ChannelSample::waterLineMm` (bed + ¾ depth) is the
elevation the banks are built to contain, and `bedMm` / `rimMm` bracket it.
