# Nanite on the understory: a null, and why

Three arms, same site, same route, same binary, back to back.
Site: temperate forest, spawn `-154740,-81476`, 256 m detail ring,
1280x720 at `r.ScreenPercentage=65`. Receipts passed on all three; no foreign
GPU load. Roughly 540 frames each.

| counter (median ms) | Nanite OFF | Nanite ON | velocity WPO off |
|---|---|---|---|
| FrameTime | 41.75 | 42.62 | 36.62 |
| GameThreadTime | 22.00 | 22.41 | 21.34 |
| RenderThreadTime | 41.52 | 42.37 | 36.36 |
| **GPUTime** | **40.51** | **41.24** | **35.32** |
| GPU/Basepass | 14.71 | 14.66 | 14.64 |
| GPU/RenderVelocities | 14.87 | 14.87 | 0.01 |
| GPU/Prepass | 0.01 | 0.01 | 9.86 |
| GPU/VoxelMarch | 9.51 | 9.34 | 9.07 |
| DrawCall/Basepass | 176 | 178 | 173 |

## Nanite: null, and slightly negative

**+0.73 ms of GPU time (+1.8%). Rejected.**

Nanite *engaged* in the sense that the runtime accepted the data: the
visibility buffer ran at 0.121 ms and 5.97 MB of Nanite pages streamed. It did
not engage in the sense that mattered. The two passes that draw the understory
— base pass and velocity — moved by 0.05 ms out of 29.58 ms, base pass draw
count went 176 to 178 rather than collapsing, and every instance stayed
dynamic. The plants were still drawn the traditional way; the Nanite cost was
added on top of, not instead of, the existing path.

The bake itself is sound and took four attempts to make so. The fallback was
silently decimating to 25% until `FallbackTarget` was set explicitly
(`Auto` ignores explicit settings), and the guard was checking for fallback
data rather than Nanite data. Both are fixed, the identity string carries
`nanite=%d`, and the bake now refuses to hand back a mesh without Nanite data
when `-VoxelDetailNanite` was asked for.

### The cause, and it means this arm never measured Nanite

**The plant material does not carry the `bUsedWithNanite` usage flag, so the
engine's material audit fails and the component silently falls back to the
traditional proxy.**

The capture's own log says it, 253 times, once per baked material instance:

    LogMaterial: Warning: Material /Game/Voxel/Generated/DetailPreview/
    temperate-authored-lods-full-14/M_aa14d626... missing usage flag Nanite!
    Default Material will be used in game.

Zero such lines in the control arm, which is what you would expect — the
control never asks for Nanite.

The mechanism is a single engine function, `ShouldCreateNaniteProxy`
(`NaniteResourcesHelper.h:155-194`), which has exactly six ways to return
false. Five are ruled out here: the blend mode is masked and masked is allowed,
the shading model is default-lit, it is not a sky material, `bDisallowNanite`
is never set anywhere in this project, and the mesh demonstrably has valid
Nanite data. The sixth is the material audit, and the audit fails on
`MATUSAGE_Nanite` alone.

Why it fails rather than fixing itself: in the editor, Unreal auto-sets a
missing usage flag and recompiles. Under `-game` — which is how every capture
in this archive runs — it takes the other branch, warns, and uses the default
material (`Material.cpp:1889-1945`). `create_detail_asset_material.py:64` sets
`used_with_instanced_static_meshes` and nothing else, and the baked material
instances inherit their parent's usage flags wholesale, so none of the 253
carry the bit.

Everything the audit *records* but does not reject on: per-instance custom
data, per-instance random, world position offset (the wind), vertex
interpolators, customized UVs, pixel depth offset, tessellation. None of them
were ever the problem.

**So the +0.73 ms is not a verdict on Nanite.** It is the cost of registering
Nanite resources and streaming 5.97 MB of pages while still drawing every plant
through the old path. The comparison has not been run.

### What running it properly costs

One line in `create_detail_asset_material.py`, then a re-author and re-save of
`M_VoxelDetailAsset`. The material asset is part of the detail cache identity,
so that forces a **full re-bake** before any capture can use the cache, plus a
cold shader-permutation compile on the first run.

One structural limit is worth knowing before spending that. There are two mesh
producers here. The baked/cached path (`bFastBuild=false`) goes through the
normal derived-data build and honours `NaniteSettings` — that is why the bake
guard passed. The runtime path (`CreateDetailStaticMesh`, `bFastBuild=true`)
never allocates Nanite resources at all, and in a non-editor build
`StaticMesh.cpp:8924` hard-asserts that fast build is the only option. **Any
plant that comes from a cache miss or the geometry fallback can never be
Nanite**, so a fully-Nanite understory needs 100% cache coverage; anything less
is a permanently mixed population with the traditional proxy still drawing half
of it.

Against that, size culling is already measured at −21 ms, needs no re-bake and
no shader compile, and is one flag. Nanite's case rests on the velocity pass
disappearing structurally rather than getting cheaper, which is worth up to
14.9 ms — real, but behind a re-bake and not yet demonstrated.

**On destructibility, which is the question that was asked first:** the
understory has no destruction path. Ground cover is not voxel-destructible. It
is *interactable* — chop a reed, the reed goes from placed to removed and a
loot item drops — and that is a whole-instance remove, which is exactly what a
Hierarchical Instanced Static Mesh already does and what Nanite does not care
about. So Nanite was a legitimate thing to try here. It simply did not pay.

## Velocity: the real number, and it is large

**-5.19 ms of GPU time (-12.8%) from `r.Velocity.EnableVertexDeformation=0`.**

This is the single biggest GPU win measured in this work.

Read the row carefully, because the naive reading is wrong. `RenderVelocities`
went 14.87 to 0.01 and `Prepass` went 0.01 to 9.86. That is *not* the pass
moving: with `r.VelocityOutputPass=0` (the default) velocity is rendered
*during* the depth pass, which splits the depth pass's own time into two
reported phases. Turning off vertex-deformation velocity collapses that split
and the combined figure falls from 14.88 to 9.86 — 5.02 ms of the 5.19 ms
total.

So the cost is specifically **motion vectors for wind-deformed vegetation**,
not velocity rendering in general.

**This is a visual trade, not a free win.** Those motion vectors are what let
TSR resolve moving foliage; without them, wind-blown leaves smear under
temporal upscaling. Per the settings-panel policy every visual-trade toggle is
a Settings row and the owner's verdict sets the default. This one is not yet
taken, and the screenshots to judge it have not been shot.

## What was retracted along the way

An earlier claim that the marcher had a "3.71 ms fixed cost" was a two-point
fit artefact and is retracted; a flight leg at 2.4x the pixels totalled 5.04
ms. A pose hypothesis for the forest's per-pixel cost was also refuted: the
forest is 4.3x the flight leg per pixel and the entire pose range only spans
0.82 to 4.16 ns/px.
