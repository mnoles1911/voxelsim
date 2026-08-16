# Two ladders, not one: what an animal looks like, and what it can do

The owner's requirement, 2026-08-16: animals spawn within 1–2 km, are killable,
will be animated with their own AI later — **and** there must be an intermediate
level of detail, so a deer on the far side of a valley is visible and moving
while the expensive parts are switched off.

That last sentence is the design. **Visual detail and simulation cost are two
separate ladders with two separate schedules**, and every number below says they
must be, because the distance at which an animal stops being worth *drawing* in
detail is nowhere near the distance at which it stops being worth *simulating*.

---

## 1. The visual ladder comes from optics, not taste

At 1080p and a 70° vertical field of view, one pixel subtends **1.1312 mrad**. A
voxel smaller than a pixel is detail nobody can receive, so the honest rule is:
*a voxel should be about a pixel.* That fixes every threshold.

Measured on `white-tailed-deer` (authored at 2 cm, variation off, seed 1):

| LOD | voxel | how it is made | voxels | of LOD0 | justified out to |
|---|---|---|---|---|---|
| 0 | 2 cm | generated | 16,742 | 100% | **18 m** |
| 1 | 5 cm | generated | 1,205 | 7.2% | 44 m |
| 2 | 10 cm | generated | 242 | 1.4% | 88 m |
| 3 | 20 cm | downsampled | 60 | 0.4% | 177 m |
| 4 | 50 cm | downsampled | 5 | 0.02% | 442 m |
| 5 | point | one lit voxel | 1 | — | beyond |

**The first row is the surprise: 2 cm detail is only justified to 18 metres.**
Every animal in the library is authored at its LOD0 master and is over-detailed
past arm's reach. This is not an argument against the lattice work of 2026-08-16
— it is what makes it the *master*, the thing the rest of the chain is derived
from.

LOD 0–2 come free from the generator: `resolution_cm` already offers 10/5/2.5/2/1
and every species builds at any of them. LOD 3–4 are past the menu, and they
should be **downsampled from the baked LOD2 grid** rather than added to the menu:
a coarser generated asset is a different-looking animal, while a downsample is
provably the same animal with less information. The 15%-occupancy majority-colour
reduction used to measure the table above is the whole algorithm.

At 442 m a deer is **five voxels**. Your animal across the valley at 800 m is a
1.7-pixel object. **The far ring costs nothing to draw.** Whatever the far ring
costs, it is not rendering.

## 2. The simulation ladder is already half-authored

| tier | what it has | what it costs |
|---|---|---|
| **S0 — actor** | animation, collision, AI, killable, replicated | full |
| **S1 — mover** | position, heading, gait phase. No collision, no AI, not killable | a few floats at 1–5 Hz |
| **S2 — group** | one herd: centre, count, one path | one record per herd |

The S0 boundary does not need inventing. `herd.despawn_m` was authored by hand
per species, and fitted across all 131 quadrupeds it comes out at

    despawn_m = 190 * length_m ^ 0.73        (R² = 0.86)

The authors were already scaling simulation range with body size and never wrote
it down. It gives a fox ~150 m, a deer ~255 m, an elephant ~700 m — which is
about right for "close enough to matter".

The S1 boundary is the pixel rule: an animal is drawable while it is at least one
pixel, i.e. out to `length ÷ 1.1312 mrad` — a deer to 1,326 m, an elephant to
5,304 m, capped by the owner's 2 km spawn radius.

**Two quantities, both already available, neither previously distinguished.** The
authored despawn is 3–5× shorter than the visibility limit, and that gap is
exactly the intermediate ring the owner asked for.

## 3. The size floor is free, which is the good news

Quadrupeds inside a 2 km radius with grassland weights, using the real densities
from `docs/wildlife-density-research.md`:

| body-length floor | species kept | animals in bubble | of those, visible at 800 m |
|---|---|---|---|
| none | 113 | **46,481** | 1,199 |
| 0.5 m | 93 | 5,805 | 1,199 |
| **0.8 m** | **73** | **1,287** | **1,199** |
| 1.5 m | 38 | 494 | 494 |

Going from no floor to 0.8 m **removes 45,194 animals and loses none that could
be seen**. Everything it drops is sub-pixel at that range: a 30 cm hyrax is under
a pixel beyond 265 m, and there are 72,000 of them in the bubble.

So the floor is not a compromise or a budget cut. It removes exactly the entities
that could never have been received, and it leaves ~1,300 far-ring movers in the
worst biome — of which ~1,200 are genuinely on screen. At 60 voxels each (LOD3)
that is 78,000 voxels total, less than three shipped trees.

## 4. The long shot, and how to keep it without paying for it

There is a real gameplay cliff in this design: if S1 is not killable, an animal
you can plainly see at 600 m cannot be shot, and the player feels the seam.

**Promote on demand.** A shot fired toward a far group promotes that one group to
S0 for the duration — collision, hit resolution, death. The identity is already
stable (the scatter's site id plus the index within the herd), so the animal that
was moving is the animal that is hit. You never pay for 1,300 killable deer; you
pay for the one the player aimed at, at the moment they aimed.

The same mechanism runs the other way when the player closes on a herd: S1 → S0
promotion must preserve identity, or the deer you stalked is not the deer you
meet. That is the same requirement, and one implementation.

## 5. What this does not settle

* **Where killability ends entirely.** Promotion-on-demand makes the far ring
  shootable in principle; whether a 1.8 km shot should connect is a design call,
  not an engineering one.
* **Movement authority at S1.** "Some NPC movement" is cheap if a herd moves as a
  unit and individuals carry a deterministic offset and gait phase. It stops
  being cheap the moment an S1 animal needs to path around terrain. v1 should
  move herds along precomputed drift, not navigate.
* **The downsampler is measured but not built.** The table above was produced by
  a throwaway reduction to price the chain. Building it properly means a
  `--lod` pass in `tools/export_banks.py` and LOD-aware bank loading.
* **Birds and fish are not costed here.** A sparrow is 15 cm and sub-pixel beyond
  130 m, so the size floor removes almost every bird from the far ring — which is
  probably right for a sparrow and plainly wrong for an eagle or a flock of
  cranes, and flocks in flight are the case this framework handles worst.
