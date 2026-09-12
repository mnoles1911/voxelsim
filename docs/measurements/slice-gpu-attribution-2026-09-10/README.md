# Slice GPU attribution: the understory cost is geometry, not pixels (captures 34 and 35)

> **CORRECTION, 2026-09-12.** Text below that calls 48 m "the shipping default"
> or "the default ring" is WRONG. `kDefaultRingMeters` is **256.0** in
> `VoxelDetailAssetSubsystem.cpp:512`, with no config override, and its own
> comment records that it was raised deliberately after the owner reported the
> slopes looking bare from the vista. 48 m was the *validation harness*
> parameter default, which every capture then passed on the command line — so
> runs at 48 m were measuring a configuration the game does not use. The
> harnesses now default to 256 to match. What this changes: the 256 m figures
> throughout are the SHIPPING configuration, not an experiment.



Two captures added on 2026-09-10 to settle a question the existing data could not answer, plus the
noise floor that every single-pair claim in the plan should be read against.

## Capture 34 — the repeatability floor

Identical configuration to capture 26 (256 m ring, size cull off). Run purely as a repeat.

| counter | capture 26 | capture 34 | difference |
|---|---|---|---|
| GPU total | 40.739 | 40.659 | −0.2% |
| GPU/Basepass | 14.852 | 14.754 | −0.7% |
| GPU/RenderVelocities | 14.943 | 14.969 | +0.2% |
| GPU/VoxelMarch | 9.239 | 9.276 | +0.4% |
| Game thread | 23.117 | 23.769 | +2.8% |
| Frame time | 42.355 | 42.101 | −0.6% |

GPU counters reproduce to within 0.7%; the game thread is looser at 2.8%.

It was launched to measure resolution scaling by requesting 2560x1440, and it did not do that:
`-ResX`/`-ResY` are inert in this harness, and the internal view stayed at 832x468. That is the same
behaviour recorded for the flight leg. The run is kept as the repeatability control it accidentally
became.

## Capture 35 — the decisive experiment

`r.ScreenPercentage=33` instead of the project default 65, passed via `-dpcvars` because the cvar is
latched at startup. Engagement proven by the log's own view line: 423x238 = 100,674 pixels against
832x468 = 389,376, so **3.87 times fewer shaded pixels with identical geometry, instances and draw
calls**.

| counter | 65% (mean of captures 26 and 34) | 33% (capture 35) | change |
|---|---|---|---|
| GPU/Basepass | 14.803 | 14.724 | −0.5% |
| GPU/RenderVelocities | 14.956 | 14.916 | −0.3% |
| GPU/VoxelMarch | 9.258 | 5.143 | −44% |
| GPU/TemporalSuperResolution | 0.331 | 0.286 | −14% |
| GPU total | 40.699 | 36.326 | −11% |
| DrawCall/Basepass | 173 | 173 | unchanged |

**The understory passes did not move**: 29.76 ms to 29.64 ms, a 0.4% change against a 3.87x pixel
reduction, well inside the noise floor above. The understory cost is entirely geometry — vertex
processing, primitive assembly and instance handling. It is not shading, not overdraw, not
resolution, and not a consequence of the masked material disabling early depth rejection.

That last point is worth stating plainly because the opposite was the natural reading: the material
is masked, the depth prepass measures 0.009 ms across three draw calls, and so every layer of
overlapping grass really is shaded. It simply is not where the time goes.

The marcher scales with pixels, but not linearly. **Correction, same day:** this record first read
the two points as "3.71 ms fixed plus 14.25 ns per pixel". That is refuted by the terrain-only flight
leg, where the marcher costs 5.04 ms in total at 921,600 pixels, which cannot contain a 3.71 ms fixed
term. A two-point fit through a curve manufactures an intercept. Per pixel the three known points are
5.47 ns (flight leg), 23.78 ns (forest 65%) and 51.09 ns (forest 33%): **per-ray cost rises as rays
get fewer**, which is a latency signature, not a constant. A four-point sweep is needed to
characterise it properly, and the harness now supports it.

## What follows

Because the workload is geometry-bound, the levers are the ones that reduce submitted geometry:
the traversal count (`RenderVelocities` is the depth pass for vertex-deforming geometry at the default
`r.VelocityOutputPass=0`, not a spare pass, so the target is collapsing two traversals into one), the
LOD chain (coarsest LOD is a median 74% of LOD0 triangles; the whole chain saves 29% library-wide;
16 of 339 models have no LODs), size culling (21 ms, already built, drops 50 of the submitted
components), and Nanite, which is explicitly disabled and exists for this exact shape of problem.

Anything aimed at pixels, overdraw, early-Z or resolution is refuted for this scene by capture 35.

## Scope

One site, one fixture, walking at 2.2 m/s, 256 m ring. The geometry-versus-pixel conclusion should
hold anywhere the same material and instancing are used, but the absolute milliseconds are this
scene's. Nothing here is a claim about the 48 m default ring, where the understory is roughly
1.8 ms of GPU and the marcher dominates instead.
