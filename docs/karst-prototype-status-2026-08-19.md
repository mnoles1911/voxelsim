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

## Playability: measured against the engine's own player

`karst_playability.py`. The player numbers are read out of
`VoxelMovementTuning.h`, not guessed: a **box 0.6 m wide x 1.8 m tall** (1.2 m
crouched), **step-up 0.3 m**, jump ~1.25 m. That is almost exactly Minecraft's
player (0.6 x 1.8, 0.5 m step), so Minecraft's cave design rules transfer nearly
unchanged, and the two numbers that decide a passage are the same two in both:
**headroom and floor step**.

Classes are set by gradient against the step-up: **walk** <= 0.5 (0.3 m rise per
0.6 m of travel), **scramble** <= 2.0 (needs jumps), **shaft** above that.

| | value |
|---|---|
| walk / scramble / shaft, by length | **56.6% / 36.3% / 7.1%** |
| headroom | min 6.0 m, mean 7.4 m |
| walkable floor width | min 2.68 m, mean 3.30 m |
| surface entrances | 735 |
| **reachable on foot from an entrance** | **22.7% walking, 92.1% with jumps** |

**92.1% reachable with jumps is the headline: the network is explorable, not
scenery.** The walk/scramble/shaft mix is close to what a Minecraft cave feels
like, and 7% shafts is enough vertical drama to matter without isolating the
system.

**One finding that follows from the 5x directive and is worth reconsidering.**
Minimum headroom is **6 m** and minimum walkable floor is **2.7 m** — every
passage in the world is a hall. There are no crawls, no squeezes, no tight
connectors, because a uniform 5x scale removes the small end of the
distribution entirely. Tight passages are a large part of what makes caves read
as caves in Minecraft, and "one characteristic scale per feature class" is
exactly the complaint that got the old system rejected
(`docs/underground-system-plan.md`, the six tells). The fix is not to abandon 5x
— it is to make the radius distribution WIDE rather than shifted: keep the halls
at 5x and let a real fraction of passages stay at 1-2x so a player has to crouch
and squeeze between them. That is an owner decision, not a tuning one, because
it partly walks back the "uniformly generous" answer.

*Caveat on the radii:* they are currently sized from node degree as a stand-in
for discharge (r ~ Q^0.4 is the physical rule). The prototype does not carry Q
yet, so the radius VARIATION is not yet physical — only its range is.

## OWNER VERDICT, 2026-08-19: "the tunnel tubes look very perfect, straight, and not natural"

Correct, and now measured. **Tortuosity (path length / straight-line distance)
is mean 1.142, median 1.073, and 45.6% of chains are essentially straight
(<1.05). Surveyed karst conduits run about 1.3-2.0; a straight line is 1.0.**
`docs/images/karst/centreline-wander.png` shows it directly: straight spokes
radiating from a hub.

**This reproduces tell #3 of the six that got the OLD system rejected** --
"straight constant-radius capsules, one hash draw per edge". A Dijkstra graph of
capsules ships the same artefact as a hash lattice of capsules, with better
provenance. That was named as the killer risk in the plan and it has now
happened.

**Where the straightness actually comes from, and it is not the renderer.**

1. Voxel-level detail was the first thing I tried and it is NOT the fix.
   Midpoint subdivision with noise displacement, radius variation along the
   conduit and wall roughness all landed (`karst_voxel.py`), and they make the
   walls irregular and the width vary -- but the centreline stays straight. That
   is cosmetic jitter on an unchanged path.
2. **Every term in the edge cost is multiplied by segment length.** So the total
   is essentially `k x length` with `k` varying slowly, and the cheapest path is
   simply the SHORTEST one: Dijkstra returns a straight line by construction.
   Tested rather than argued -- dropping `w_dist` 1.0 -> 0.15 and sharpening the
   horizon term to a narrow band (`w_horizon` 400, exponent 3) moved passage
   density only 1.168 -> 1.249, i.e. the geological terms barely influence the
   route at all.
3. **Node spacing cannot express the meander.** At 120 m spacing a 400 m
   conduit is three nodes. A 30 m wiggle has nowhere to live.

**The fix, in the order it should be tried** (none of it done yet):

* Make the geological cost vary *per unit length* far more sharply than it does,
  so following a horizon or a joint is genuinely cheaper than going straight.
  The horizon term wants to be a narrow band, not a smooth ramp.
* Node spacing down to ~30-40 m so a route has somewhere to bend, accepting the
  Dijkstra cost (it is milliseconds today).
* Only then noise displacement, as a finish rather than as the mechanism.

**Acceptance target, stated before the attempt: mean tortuosity >= 1.3 with
under 10% of chains below 1.05.** That is a number the next attempt can be
measured against instead of another round of looking.

## FIXED: tortuosity 1.14 -> 1.30, straight chains 45.6% -> 0.4%

**Acceptance target met** (mean >= 1.3, straight < 10%). Settings:
`--node-spacing-m 80 --gamma 1.05 --deadends 20 --wander 3.0 --piece-m 5
--wander-z 0.10`. Density 2.04 km/km^2, still inside the 5x band.
`docs/images/karst/centreline-fixed.png`.

**Three things were wrong, and only the third one mattered.**

1. **Horizons were 2D.** `incept` was the stratigraphic field sampled at the
   SURFACE and stored as a 2D array, then indexed by `[my, mx]` regardless of
   the node's z -- so a conduit at 1,900 m and one at 2,100 m in the same column
   got identical horizon cost. The term carried no depth information and could
   not make a route follow a bedding plane. Fixed (`inception_at`, evaluated in
   3D at the edge's own z, one field shared by both stages). **It changed
   almost nothing on its own: 1.098 mean, 67.7% straight.** Worth fixing because
   it was simply wrong, not because it was the cause.
2. **The cost weights cannot bend a route, and this is mathematics rather than
   tuning.** Every term is multiplied by segment length, so the total is
   `k x length` with `k` varying slowly, and shortest-path over a smooth cost
   field is near-straight. Swept 3x3 over spacing and weights: mean tortuosity
   moved 1.082 -> 1.156, nowhere near 1.3. **Stop trying to weight your way to
   sinuosity.**
3. **Sinuosity has to be ADDED, not optimised for** -- which is what the
   reference method's midpoint subdivision does and what Minecraft's noise
   carving does. I had implemented it in the VOXELISER only, and judged it by
   eye at 900 m zoom where a 6 m meander is invisible. Moved into the network
   stage, where the skeleton that actually gets baked and shipped lives, and
   measured instead of eyeballed.

**The trade it forces, measured: sinuosity costs walkability.**

| wander_z | tortuosity | straight | walk-reachable | with jumps |
|---|---|---|---|---|
| 0.05 | 1.304 | 0.4% | 13.2% | 55.8% |
| **0.10** | **1.30** | **0.4%** | **12.0%** | **~55%** |
| 0.30 | 1.314 | 0.3% | 9.7% | 50.0% |
| 0.60 | 1.343 | 0.2% | 7.0% | 37.6% |

Vertical displacement is not a taste parameter. At 0.6 it puts a several-metre
rise and fall into every 5 m piece, which is a rollercoaster, not a cave. Real
passages meander in PLAN far more than they undulate in PROFILE, because a
phreatic tube follows the water table and a vadose canyon follows the hydraulic
gradient, and both are smooth in z.

**AND IT FORCES A CORRECTION TO AN EARLIER NUMBER.** The 92.1% foot-reachable
figure reported before was an artefact of coarse segments: gradient averaged
over 217 m hides local steepness the player actually walks into. At a realistic
5 m piece length the honest figure is **~55%**. The earlier number was measured
correctly and meant less than it looked like -- the same averaging trap
`voxelsim-terracing-measure-plateau-area` already records.

**One cost to carry into the format design:** subdivision takes the skeleton
from 2,880 to 80,820 segments, ~28x. The `.vxkn` budget in the plan (~280 KB per
tile) was sized on unsubdivided nodes and needs re-deriving -- or the wander
needs to be applied at read time from a seed rather than stored, which is the
cheaper answer and worth designing for.

## Owner decisions applied, and the four follow-ups

**Radius distribution: WIDE, accepted.** Radii are now **log-uniform from 0.8 m
to 9.0 m** rather than a linear ramp between 3.0 and 7.5. Log-uniform is the
point: discharge across a karst network spans orders of magnitude and r ~ Q^0.4,
so a linear ramp is middle-heavy and makes everything a hall. Both tiles now
measure a **1.97 m mean radius against a 9 m maximum** -- most passage small, a
few large, which is what real caves and Minecraft both have.

**The 0.8 m floor is a playability limit, not taste.** A tube of radius r has a
walkable floor about 0.894r wide with 1.79r of headroom over it, so the player's
0.6 m box and 1.2 m crouch both bottom out at r = 0.67 m. 0.8 m is that plus
margin: a 1.6 m tube you crouch through. Measured minimum headroom is now
**1.60 m** -- crouch-only passages exist, which is the texture that was missing.

*One bug found and fixed on the way:* the radius model normalised junction order
by each tile's own min/max, which is unstable when degree is nearly constant --
it gave a 1.79 m mean on one tile and **8.92 m on another** (every passage a
hall) because dividing by a near-zero range amplifies the leftover noise. Now an
absolute mapping: degree 1 is a tip, 4+ is a trunk confluence, and those mean the
same thing on every tile.

**1. Hierarchical branching -- done.** Dead-ends no longer all attach to whichever
node happens to be central. A node already branched-to is penalised, which
spreads junctions along the trunk the way a tributary joins a river.

**2. The unserved region -- NOT DONE.** Still the one open item of the four.

**3. A tile with ponors -- done, and the path had never run.** Tile `-7_-5`
carries **629 terminal basins**; every previous run was on a tile whose 165
basins are all overflowing, so the terminal-basin sink path -- the strongest
evidence the world carries -- had never once executed. It works: 1,710 sinks,
629 of them ponors, density 4.48 km/km^2, 79/19/2 walk/scramble/shaft.
*Worth noting:* spring-headwater agreement is **5.7%** on this tile against 29.7%
on the alpine one. Wetter, flatter, shallower water table. Not yet explained.

**4. The `.vxkn` budget -- re-derived, and the answer is: do not store the
wander.** Subdivision takes the skeleton from 2,880 to 80,820 segments, ~28x,
which would take the ~280 KB/tile estimate to roughly 8 MB and make the sidecar
a third of a tile. But the wander is a pure function of `(seed, position)` --
it is `subdivide()` over a value-noise field, with no global state. **Store the
un-subdivided skeleton and apply the wander at READ time from the seed.** The
format then keeps its ~280 KB, the client reconstructs identical geometry, and
the sinuosity costs nothing on the wire. This is a format-design decision that
had to be made before the format is written rather than after.

## The four closed, and the killer risk is dead

**1. THE BLEND QUESTION IS ANSWERED, AND THE FILLET CAN BE DROPPED.** This was
the risk named as able to kill the project. Re-run on SINUOUS conduits (it was
meaningless on straight ones -- "a smooth junction between two straight pipes is
still two straight pipes"), and against a blend radius that rounds junctions
instead of inflating them:

| blend k | blended vs hard volume | hard-vs-blended IoU |
|---|---|---|
| 0.5 | 1.03x | **0.971** |
| 1.0 | 1.09x | 0.916 |
| 2.0 | 1.28x | 0.778 |
| 4.0 | **1.93x** | 0.517 |

The k=4.0 figure quoted earlier was never a smoothness measurement -- at that
radius smooth-min inflates the conduit by **93% of its own volume**, so it was
comparing a tube against a fatter tube. At a radius that only rounds junctions,
**a hard union already matches smooth-min to within 3%.**

The reason is subdivision: at 5 m pieces adjacent capsules overlap so heavily
that their union is smooth by construction. The crease that motivated the whole
fillet mitigation was an artefact of 217 m segments meeting at sharp angles.
**Ship the hard union with its early-out; do not build fillet nodes.** That
removes a bake-time step, an extra primitive class, and the only part of the
geometry plan that had no bounded cost.

**2. THE UNSERVED REGION IS LEGITIMATE GEOLOGY, NOT A SUPPRESSION ARTEFACT.**
Conduit coverage within 300 m is 47.6% (alpine) and 86.2% (ponor), and the
unserved ground is simply HIGHER:

| | served | unserved | delta |
|---|---|---|---|
| alpine, elevation | 2,312 m | 2,649 m | **+337 m** |
| ponor, elevation | 183 m | 706 m | **+524 m** |
| ponor, water-table depth | 10.9 m | 33.1 m | +22 m |

High massifs stand above the local base level, have no springs, and therefore
grow no conduits. That is what real karst does -- caves cluster near base level
-- so the empty high ground is the model working, not failing. **Nothing to fix.**

**3. THE HEADROOM CHECK IS SPLIT.** "Passable" (1.2 m, crouched) is the bar;
"standing" (1.8 m) is a texture statistic. Both tiles: **94% stand upright, 6%
crouch-only**, minimum 1.60 m. The flat NO it used to print was the wide radius
distribution working, reported as a regression.

*6% is on the low side for the crawl texture the wide distribution was chosen
for.* It follows arithmetically from log-uniform 0.8-9.0 m, where only ~5% of
the range sits below a 0.9 m radius. If more squeezes are wanted, the
distribution has to be shifted down, not widened further.

**4. THE SPRING/HEADWATER GAP IS A CRITERION LIMIT, AND IT IS WORTH KNOWING.**
Agreement was 29.7% on the alpine tile and 5.7% on the ponor one. Cause:

| | alpine | ponor |
|---|---|---|
| water-table depth, mean | 48.4 m | 14.0 m |
| cells with the table within 1 m of the surface | 9.8% | **47.4%** |

The spring test is "the water table meets the surface in a valley where a
horizon daylights". On wet flat ground the table is at the surface across nearly
HALF the tile, so the test stops discriminating and suppression then picks 40
resurgences out of 2,411 near-arbitrarily. **The criterion only works where the
water table is deep.** The fix is to require the table to EMERGE -- a convergence
or gradient condition -- rather than merely to be near the surface. Not done, and
it is the highest-value correctness item left in the field stage.

## Next, in order

1. **Owner looks at the map and sections** in `docs/images/karst/` and says
   whether the shape reads. Density is in band; shape is his call.
1b. **Owner decides the radius DISTRIBUTION** — uniform 5x (no crawls, current)
   vs. wide (halls at 5x, a real fraction of squeezes at 1-2x). See the
   playability section.
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
