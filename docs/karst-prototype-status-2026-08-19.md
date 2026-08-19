# Karst Phase 0 prototype — what works, what does not, 2026-08-19

Three tools, end to end, on real baked tiles. **The field layer works, and after
amplification the routing layer produces branchwork at 1.90 km/km^2 -- inside the
owner's 5x band.** The sections below are in the order they were found, because
the first diagnosis was right about the symptom and wrong about the cause, and
that is the part worth not repeating.

## Runs end to end

`karst_prototype.py` → `karst_network.py` → `karst_render.py`, on tile `-4_-4`
(alpine, bake_ver 28, seed 20260719, 236 km²):

| stage | cost | output |
|---|---|---|
| fields | 105 s | elevation, water table, inception horizons, fracture fabric, sinks, springs |
| network | 0.4 s | 35 systems routed, 2,080 segments, 449 km passage |
| render | 6 s | map over hillshade + three cross-sections |

## What is right, with evidence

* **Elevation decodes correctly.** 1,048.8 – 3,551.7 m across the tile, and the
  hillshade in `-4_-4-karst-map.png` shows coherent alpine ridge-and-valley
  structure. (It did not at first — see the 100× note below.)
* **The water table behaves like a water table.** In
  `-4_-4-karst-sections.png` it sits tens to hundreds of metres below ridge
  crests and rises to meet the surface in every valley floor. That is Tóth-style
  regional flow, and it is the field the whole vadose/phreatic distinction
  depends on. Depth: mean 48.4 m, p90 113.8 m, max 298.3 m.
* **Conduits descend toward valley floors** and sit at plausible depths (mean
  81 m, p90 109 m below surface).
* **Springs correlate with the bake's own headwaters at 29.7%** (within 3 cells).
  Weak, but the cross-check exists and runs automatically — a stream appearing
  at full discharge from nowhere is what a resurgence *is*.

## UPDATE, same day: amplification was the missing piece

**Passage density 0.34 -> 1.90 km/km^2, and the map now reads as branchwork.**
The diagnosis below ("fragments, not systems") was right about the symptom and
wrong about the cause. Two things were found, in order.

**1. The two levers interact, and the sweep proves it.** Node spacing and gamma,
swept jointly at 38 springs (density in km/km^2):

| spacing | gamma 1.05 | gamma 2.0 | gamma 4.0 | mean segment |
|---|---|---|---|---|
| 225 m | 0.543 | 0.343 | 0.254 | 338-385 m |
| **120 m** | **0.656** | 0.496 | 0.377 | 198-215 m |
| 75 m | 0.624 | 0.553 | 0.459 | 90-94 m |
| 50 m | 0.569 | 0.502 | 0.415 | 76-79 m |

Density peaks at **120 m spacing, not the finest** -- 50 m gives more than twice
the segments (1,697 vs 720) and *less* passage, because the extra segments are
short and the prune eats them. Sweeping either lever alone would have picked the
wrong value, exactly as backlog section 0.6 records for
`GPUCullMergeGap`/`GPUCullMaxRanges`.

**2. Sink-to-spring shortest paths give the TRUNK and nothing else.** That is
what "fragments" actually was. A real cave is mostly not trunk: it is
tributaries, blind alleys and abandoned loops, which is why the reference
scenes carry explicit Waypoint and Deadend key points -- their "superimposed"
scene is 1 sink and 2 springs against **eleven** deadends, and the spongework
scene ~50. The paper's `Amplify` step was in my research notes and not in the
code. Implemented (route extra corridor nodes to the nearest node ALREADY on the
skeleton, which is what makes a branch a branch rather than a second trunk):

| dead-ends/system | segments | density km/km^2 | mean segment | p90 depth |
|---|---|---|---|---|
| 0 | 720 | 0.656 | 215 m | 250 m |
| 15 | 1,596 | **1.467** | 217 m | 257 m |
| 25 | 2,080 | **1.903** | 217 m | 258 m |
| 40 | 2,662 | 2.451 | 217 m | 259 m |

Segment length and depth are **unchanged** across that range, which is the check
that amplification adds branches rather than distorting the geometry.

**Current settings: spacing 120 m, gamma 1.05, 25 dead-ends, spring separation
1,500 m -> 1.90 km/km^2**, inside the 5x band. `docs/images/karst/` carries the
map and sections at those settings.

**The owner judges the map, not my reading of it**
(`voxelsim-owner-judges-screenshots`). What I can say without judging: it is
dendritic, systems converge on resurgences, and depth shading varies within
single systems. Two things I would want him to look at specifically -- a large
unserved region in the centre-right of the tile, which is either a legitimately
non-karst massif or an artefact of spring suppression, and the "spidery" hubs
where many short spokes meet one node, which is naive amplification showing
through and is not how real caves branch.

## What was wrong before amplification

**The network is fragments, not systems.** 230 segments averaging 286 m,
passage density **0.34 km/km²** — roughly 1× real-world mapped-cave density
against the owner's 5× target, and visually the map shows short disconnected
pieces rather than branchwork converging on resurgences.

Two causes identified, one fixed:

1. **Spring over-detection — fixed, and it was the big one.** The field stage
   flags every cell where a horizon daylights at the water table in a valley,
   which on real terrain is a contiguous *ribbon*, not a point: 1,461 candidates
   over 236 km² (6/km²) against 937 sinks. With as many springs as sinks every
   system gets ONE sink, and a one-sink system has no confluence and no
   branchwork. `suppress_springs` (non-maximum suppression by catchment,
   1,500 m separation) takes it to 38 springs (0.16/km²), which took density
   from 0.02 to 0.34 km/km² and routed systems from 10 to 36. **The suppression
   belongs in the field stage next to `find_springs`; it lives in the network
   stage while the separation is being tuned, because the field stage costs
   105 s a run.**
2. **Still open: paths are short and heavily pruned.** The γ-skeleton prune
   at γ=2.0 drops most candidate edges, and corridor sampling at 225 m node
   spacing gives few nodes to route through. The next lever is node spacing
   against gamma, swept together — they interact and sweeping either alone will
   mislead (the same trap as `GPUCullMergeGap`/`MaxRanges`, backlog §0.6).

## Two decode traps, recorded so they are not repeated

* **`quant` is a CODE, not a multiplier.** `tile_codec.QUANT_MM` maps 1 → 100 mm.
  Multiplying by the code gives a silent **100×** error that still decodes to
  plausible terrain — it reported 25 m of relief across a 15 km alpine tile,
  which is only obviously wrong if you know the region. This repo has already
  retracted one 100× decode error (`bb83002`); same trap, different field.
* **`HeadEntry.px` is the (x, y) pair**, not an x.

## Next, in order

1. **Owner looks at the map and sections** in `docs/images/karst/` and says
   whether the shape reads. Density is in band; shape is his call.
2. **Hierarchical amplification.** The current step connects each dead-end to
   the nearest skeleton node, which makes spidery hubs. Real caves branch
   hierarchically — a tributary joins a trunk, not a star centre. Weighting the
   connection toward *downstream* skeleton nodes is the cheap first attempt.
3. **The unserved region.** A large area of the tile carries no conduits.
   Establish whether that is a legitimately non-karst massif (deep water table,
   no springs) or an artefact of spring suppression, before tuning anything else.
4. Move `suppress_springs` into the field stage now the separation is settled.
5. **The basin sink path has never run.** Terminal basins contributed zero sinks
   here — all 165 of this tile's basins are overflowing. Run the field stage on
   a tile with ponors.
6. Only then the blend question, in C++ (`vxc_karstprobe`). It is the risk that
   can kill the project and it cannot be answered from a skeleton.

**What the map is and is not.** It is evidence that the method produces
karst-shaped networks from this world's own hydrology at a defensible density.
It is not evidence about how any of it looks as voxels, which is a different
question with its own probe.
