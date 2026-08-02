# Scope: seeding the hydrology pyramid from the model's coarse map

**Goal.** Stop truncating catchments larger than the top superblock level.
Today `superblock_max_level` (default 1, 246 km) receives no inflow at its own
edges, so any river draining more than that arrives with **zero** upstream area
and carves nothing. The fix identified in `pipeline.py` — and marked UNWIRED —
is to seed the top level from terrain-diffusion's own coarse map.

This document scopes that work. It is not an implementation.

---

## What is already true (verified)

**The `parent=` mechanism works and is in production use.** `build_flow_superblock(..., parent=...)` already chains levels: `pregen` builds L1, then passes it as parent when building L0. Nothing new needs inventing in the injection path.

**A parent is a small interface.** `inject_edge_inflow` reads only four things
off the parent `FlowSuperblock`: `cell_m`, `origin_m`, `acc` (MFD accumulation,
m²) and `filled` (depression-filled elevation). Everything else on the dataclass
(`sx`, `sy`, `tiles_per_side`, `missing_tiles`, `inputs_fingerprint`) is
bookkeeping. **A synthetic parent is therefore constructible** — it does not
have to come from coarse tiles.

**The model's coarse map is the right source and the access idiom exists.**
`world.coarse[:, ci0:ci1, cj0:cj1]`, then
`(c[:-1] / (c[-1:] + 1e-8))[0]` and `sign(x) * x²` for metres — the canonical
form is `terrain_diffusion/inference/explorer/server.py:_coarse_channel`. One
coarse cell is **7.68 km** (confirmed independently: the `elev_gain` measurement
used "64×64 cells (492 km)"). It is infinite, and it touches neither the latent
nor the decoder.

**The comment about "internals this service does not have" is about the wrong
module.** `bake/pipeline.py` is deliberately provider-agnostic — it receives a
`CoarseFetch` callable and has no idea a diffusion model exists. That is a
design property worth keeping. But **`pregen.py` sits above both**: it holds the
provider *and* calls `build_flow_superblock`. That is the seam. No layering
violation is required.

## The design

Add a **model-backed top level** above the tile-backed pyramid:

    M   built from world.coarse       7.68 km/px    ZERO coarse tiles needed
    ^
    L1  built from coarse tiles        120 m/px     256 tiles
    ^
    L0  built from coarse tiles         30 m/px      16 tiles

`M` is a `FlowSuperblock` whose `filled` and `acc` come from a priority-flood
and MFD accumulation over a window of the model's coarse map, and whose
`cell_m` is 7,680. It becomes the `parent` of the top tile-backed level.

**The size of that window sets the largest resolvable catchment, at almost no
cost:**

| window | span | cost |
|---|---|---|
| 256² | 1,966 km | one coarse-stage inference |
| 512² | **3,932 km** | one coarse-stage inference |
| 1024² | 7,864 km | one coarse-stage inference |

512² at 7.68 km/px covers **3,932 km** — Mississippi-scale basins — and needs
**no coarse tiles at all**. Compare with buying one more *exact* level: L2
would resolve 983 km and require 4,096 coarse tiles (~6 GB) to exist before any
fine tile in the region can publish under the completeness gate.

That asymmetry is the whole argument. **Cap `superblock_max_level` at 1 and add
the model-backed parent** rather than raising the cap.

## The main risk, stated honestly

**Lateral misplacement at injection.** `inject_edge_inflow` deposits a parent
cell's whole accumulated area into the *lowest child cell inside that parent
cell's footprint*. Footprint sizes:

| parent → child | ratio | lateral error |
|---|---|---|
| L1 → L0 (today) | 120 m → 30 m, 4×4 | ≤120 m, and `HYDROLOGY_RESIDUALS #3` records that it "heals within a few hundred metres" |
| **M → L1 (proposed)** | 7,680 m → 120 m, **64×64** | **up to ~3.8 km** |

So a continental river could enter the domain **on the wrong side of a divide**,
which would look badly wrong rather than subtly wrong. This is the one thing
that could sink the approach, and it is not currently measurable — it needs the
experiment below.

Mitigations to evaluate, cheapest first:

1. **Snap to the child's own channel.** Instead of the lowest cell in the
   footprint, walk the child's filled surface downhill from the footprint and
   deposit in the child-resolved thalweg. Keeps mass exact, removes most of the
   lateral error.
2. **Inject only above a discharge threshold.** Small parent cells carry little
   area and add mostly noise at this ratio; the value is in the big rivers.
3. **Accept and measure.** Zero inflow is also wrong, and badly so. "Right
   amount, approximately right place" may beat "nothing" even at 3.8 km.

## Blockers

**This cannot be built or verified on this box.** The probe run today got 12.8 s
into pipeline construction and was then refused:

    ValueError: checkpoint_sha256 is 'UNPINNED' -- refusing to run inference
    against an unpinned checkpoint

and `./checkpoint` does not exist locally — the weights lived on the GPU pod,
which was destroyed. **This work needs pod time**, or at minimum the checkpoint
restored and its sha pinned. The refusal itself is correct doctrine and should
not be worked around.

## Test plan

1. **Determinism.** Same window, two processes ⇒ identical `M.acc`. The coarse
   stage is the model; confirm it is reproducible before anything depends on it.
2. **Conservation.** Total area injected into a child equals total through-flow
   leaving the parent across that boundary. `inject_edge_inflow` claims exact
   conservation; assert it across the new ratio.
3. **The real question — does it change the terrain, and for the better?**
   Bake one tile known to sit downstream of a large off-block catchment, with
   and without the parent. Compare `max_accumulation_km2`, `injected_inflow_km2`
   and `incision_p99_m`, and render the same-ground A/B. If the river appears in
   the wrong valley, mitigation 1 is required rather than optional.
4. **Completeness interaction.** `M` is built from the model, not from tiles, so
   it is never "incomplete" in the `missing_tiles` sense. Decide deliberately
   whether that should count as complete for the publish gate — it resolves the
   *truncation* problem but not the *exploration-order* one, and conflating them
   would silently reopen what the gate just closed.

## Estimate

Small if the injection is left alone (a `FlowSuperblock` factory plus wiring in
`pregen`); the uncertainty is entirely in mitigation 1 and in whether test 3
passes. Sequence it **after** pod access is restored, not before.
