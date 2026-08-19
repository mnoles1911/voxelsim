# Karst Phase 0 prototype — what works, what does not, 2026-08-19

Three tools, end to end, on real baked tiles. **The field layer works. The
routing layer does not yet produce karst.** Written before the next session so
nobody re-derives the same three days.

## Runs end to end

`karst_prototype.py` → `karst_network.py` → `karst_render.py`, on tile `-4_-4`
(alpine, bake_ver 28, seed 20260719, 236 km²):

| stage | cost | output |
|---|---|---|
| fields | 105 s | elevation, water table, inception horizons, fracture fabric, sinks, springs |
| network | 0.1 s | 36 systems routed, 230 segments, 81 km passage |
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

## What is wrong

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

1. Sweep node spacing × gamma together; target ≥1.5 km/km² before judging shape.
2. Move `suppress_springs` into the field stage once the separation is settled.
3. Terminal basins contributed **zero** sinks on this tile (all 165 of its
   basins are overflowing) — run the field stage on a tile that has ponors, or
   the basin sink path is untested.
4. Only then the blend question, in C++ (`vxc_karstprobe`) — it is the risk that
   can kill the project and it cannot be answered here.

**Do not show the current map to the owner as a proposal.** It is evidence that
the pipeline runs, not that the geology reads.
