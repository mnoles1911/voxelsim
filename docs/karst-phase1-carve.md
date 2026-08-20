# Karst Phase 1 — the carve, the removal, and the mirror

**Opened 2026-08-19, after Phase 0 merged (PR #230).** Phase 0 answered what a
Python prototype can answer; everything below needs integer C++, the shader
mirror and a version bump, and none of it can be settled by more prototyping.

## What Phase 0 settled, so Phase 1 does not re-litigate it

| decision | settled by |
|---|---|
| **Hard union of capsules, NO fillet nodes** | measured: at 5 m subdivision hard union matches smooth-min at **IoU 0.971**. The crease was an artefact of 217 m segments meeting at sharp angles. |
| **Wander applied at READ time from the seed, not stored** | subdivision costs 28x the segments (2,880 → 80,820); the wander is a pure function of `(seed, position)`. Storing it takes the sidecar from ~280 KB to ~8 MB per tile. |
| **Sinuosity is added, not optimised for** | every cost term multiplies by length, so shortest-path over a smooth field is near-straight. Swept 3x3: 1.082 → 1.156 against a 1.3 target. |
| **Radii log-uniform 0.8–9.0 m** | linear is middle-heavy and makes everything a hall. 0.8 m is a playability floor: the player's 0.6 m box and 1.2 m crouch both bottom out at r = 0.67 m. |
| **Conduits anchored at ABSOLUTE z** | horizons and the water table are absolute surfaces; a depth-draped conduit climbs over every ridge it passes under. `amplifier.h:117-119` has the precedent. |
| **Hillside mouths routed INTO the network, not made outlets** | as outlets they grow stub systems that touch a hillside — holes leading nowhere. |

## The work, in dependency order

### 1. `voxel-core/include/voxelcore/karst.h` — the carve

Same shape as the passes it replaces: a per-column reduction to a small segment
list, then `dz*dz < marginSq` per voxel. Integer-only.

```
struct KarstSeg  { int32 marginSq; int32 axisZMm; int16 rVertCm; uint8 kind; uint8 floorDropDm; }
struct KarstColumn { int32 count; int32 minZMm, maxZMm; KarstSeg segs[kMaxKarstSegs]; }
```

`minZMm`/`maxZMm` are the column's carve band and give a **two-compare early-out**
that today's depth-space caves cannot do. Guard order copies `caveCarveAt`:
sea level, roof, bedrock margin — three independent refusals, so a mistuned
constant cannot punch through the world's floor.

**The segment cap is enforced by the ENCODER, not hoped for by a test.** Today
`kMaxCaveSegs` = 12 truncates silently (`caves.h:450`) and safety rests on a test
measuring that it never binds — which only works because the current lattice has
bounded degree. An irregular network has no such bound. The bake rasterises
segment footprints, merges near-parallel overlaps to a fixed point, and **fails
loudly** if it will not converge. A baked network can be validated; a hashed one
cannot.

### 2. The removal

Delete `caves.h`, `caverns.h`, `test_caves.cpp`, `test_caverns.cpp`, and the
mirrored passes at `worldgen.ush:2047-2680`. ~2,600 lines.

Retire hash channels 20–25 and 30–31 in `hash_channel_registry.h`. **Never reuse
them** — the registry exists because a double-allocation once shipped as a bug.

**Two things must be solved rather than deleted:**

* `VoxelFootprintBand.h:174-220` derives the vertical streaming footprint by
  unrolling the cave carve's own bounds. Remove the caves and the underground
  band has no definition. Needs a conservative bound shipped with the conduit
  data — and note Phase 0's interval-band result was **null on today's world**,
  so the band's payoff cannot be demonstrated until this lands.
* `amplifier.cpp:1345-1392`'s `static_assert`ed depth envelopes (42.8 m / 91 m)
  are what make the all-solid admission skip provable. A prevalent deep network
  breaks that proof by construction.

### 3. `worldgen.ush` — the mirror, and the real risk

Every carve input today is derivable from `(seed, vx, vy, surfaceMm)`. A carve
that reads a **baked table** has no precedent in the mirror, and a silent CPU/GPU
divergence is a desync vector under ADR-0006.

Phase 0's design note stands: this is *easier* than the raster case, not harder —
the uploaded bytes are identical on both sides, with no resampling and no window
phase. **Upload the whole tile's karst section (~1.1 MB) rather than windowing
it**, and the `kRasterCavernMarginMm` failure class is designed out rather than
guarded against.

**Prove it on a toy case before designing the wire format around it.**

### 4. Version and gates

`kWorldGenVersion` 28 → 29 with a `core.h` changelog entry; edit logs and
savegames invalidated (routine here). Goldens `cave_layer` and `cavern_layer`
die; `amplifier_deep_column_golden_digest` and the `vxc_gpu` digests move. Seven
prebuilt DXIL modules respun. `vxc_bench --digest` re-pinned on gcc/clang/MSVC.

**No tile re-bake in Phase 1** — the network is generated per region at load, at
reduced scale, purely to prove the carve and the mirror. The bake is Phase 2.

## Acceptance

* `ctest` green; `vxc_bench --digest` matches across three compilers; `vxc_gpu`
  reports bit-exact CPU/GPU after the mirror rewrite.
* `vxc_caveprobe` re-run against the new carve — it reads whatever `materialAt`
  produces and needs no changes. **This is where the interval-band question gets
  its real answer.**
* `vxc_volumeprobe` re-run and the delta handed to the ray-marching session
  before this lands: their P2 VRAM gate is ±20% of a census that our caves are
  inside whether we tell them or not.
* **Rebuild every `vxc_*` before quoting any number.** Stale probes have faked
  results here four times.

## Carried forward from Phase 0, not blocking

* The spring criterion only discriminates where the water table is deep (29.7%
  agreement on the alpine tile against 5.7% on the wet one). Wants an *emergence*
  condition rather than proximity-to-surface. Highest-value field-stage fix.
* Crouch-only passage is 6% of the network, which is on the low side for the
  crawl texture the wide distribution was chosen for. Shifting the radius
  distribution down, not widening it further, is the lever.
* Radii are sized from junction degree as a stand-in for discharge. The physical
  rule is r ∝ Q^0.4 and the bake already carries Q.
