# Natural arches: what the research says, what I built, and what I recommend

**Status: re-author `hero-arch-colossal`. Do not abandon the generator.**

This document originally recommended abandoning procedural arches. That was
wrong, and the correction is worth stating first because it changes what to do.

I had only ever looked at the 90 m arch. Rendering the small ones was the test I
should have run before recommending anything: `desert-arch` at 3.5–5 m produces
genuinely convincing arches on three seeds of six — mass above the opening, a
curved roof, legs that widen downward, a chunky rock rather than a hoop. The
generator can already do this. `hero-arch-colossal` is authored at `rock.arch`
0.9 and `rock.flatten` 2.3: the tallest blank in the library with the largest
hole asked of it, which leaves no rock around the opening. Rebuilt at
`rock.arch` 0.55 and `rock.flatten` 1.5 it reads as a substantial mass with a
hole through it. `rock.arch` 0.7 / `flatten` 1.7 goes back toward a hoop, so the
lower pair is the better of the two tested.

Measured on the 0.55/1.5 rebuild at 20 cm, seeds 1–4: daylight 21,495–31,060 px,
and **zero voxels of rock behind the opening** — the hole is clear through. The
dark lump that appears inside the opening in the renders is the far leg seen at
the camera's angle, not something blocking it.

The research below stands, and so does the prototype and everything learned from
it. What has changed is that it is not needed to fix this asset.

## The problem

The owner reviewed six seeds of `hero-arch-colossal` and said they were not
natural at all. He is right, and the renders say why. Every seed is a ribbon of
roughly constant thickness bent into a hoop, standing free in the air, with
knobbly legs no thicker than the roof. Nothing in nature is shaped like that.

## What a real arch is

A real arch is **a hole in a wall**, not a hoop.

Sandstone fractures along parallel joints into thin vertical **fins**. Water
works into the weakest bed on each flank and cuts an **alcove** — a recess at
one height, driving inward while the beds above and below barely move. Two
alcoves on opposite flanks meet, and the fin is perforated. Blocks then spall
off the ceiling and the opening widens.

That history leaves three things our arches do not have:

1. rock **above** the span, because the opening is cut into a wall that
   continues upward;
2. legs that **widen downward** into the fin, rather than tapering like a hoop;
3. a span that is thickest at the abutments, following the load path.

Sources: [USGS, Geology of Arches National
Park](https://www.usgs.gov/geology-and-ecology-of-national-parks/geology-arches-national-park);
[Utah Geological
Survey](https://geology.utah.gov/map-pub/survey-notes/glad-you-asked/glad-you-asked-why-natural-arches-in-utah/);
[Eos, "The Delicacy of Arches"](https://eos.org/features/the-delicacy-of-arches).

## Why ours cannot look right, mechanically

`forge.rock._arch` **cuts** a parametric opening out of a finished stone. The
shape of the hole is authored, so the rock has no say in it — and rock having a
say is the entire reason real arches look the way they do.

The published mechanism is a negative feedback between stress and erosion.
Bruthans et al., ["Sandstone landforms shaped by negative feedback between
stress and erosion", *Nature Geoscience* 7, 597–601
(2014)](https://www.nature.com/articles/ngeo2209): in sandstone, load-bearing
rock weathers **slower**. Under vertical stress the grains dissolve and
re-precipitate at their contacts — "fabric interlocking" — dropping the porosity
and locking the fabric. Past a critical stress, erosion effectively stops. So
erosion removes whatever is *not* carrying load, which pushes the load onto what
remains, which locks it. The shape converges on the load path. An arch is a
thrust line, exposed.

Yang, Jain, Cordonnier, Cani, Wang and Benes, ["Arenite: A Physics-based
Sandstone Simulator", *ACM TOG* 44(4), SIGGRAPH
2025](https://dl.acm.org/doi/10.1145/3731201) builds this into a simulator. Two
findings matter more here than the method does:

- Their ablation is blunt: **"without considering stress, this method cannot
  create realistic arches and arcades."** That is our exact failure.
- Their initial condition for an arch is not an arch. It is *"a rectangular
  cuboid posed on two disjoint supports"*, with layered erodibility and a
  lateral wind. The arch is what is **left**. They also note rough initial
  shapes barely matter — *"even a cube may erode into a realistic sandstone
  pillar."*

Their erodibility rule is a hard switch:  `E = k_s` if `trace(σ) > I`, else
`k_w`, with `k_w ≫ k_s`. Viability decays as `∂b/∂t = −E(σ)·(W + F)` for wind
`W` and fluvial `F`.

## What I built, and the three bugs worth keeping

`tools/archgrow.py` is a cheap stand-in for that: erosion gated by a discrete
**load routing** instead of a material-point stress solve. Every solid voxel
hands its weight to whatever solid is beneath it, spreading sideways when
nothing is. It runs coarse — Arenite's own arch is a 128³ grid, and the shape of
an arch does not need a 5 cm lattice to be decided.

Three things were wrong, each of which destroyed the mechanism completely, and
each of which is worth knowing if anyone picks this up again:

1. **A re-evaluated threshold cannot converge.** Taking the lock threshold as a
   quantile of the *current* load locks the same fraction of whatever is left,
   forever, so there is no state in which erosion stops. Measured: a 96-wide fin
   ran from 85,702 solid voxels to 8. The threshold stands in for a material
   property and must be set once and held.
2. **Erosion must be side-weighted.** Weighting every exposed face equally eats
   the fin down from its flat top, which removes the overburden — and the
   overburden was the load, so nothing locks. It is a death spiral and it is
   also wrong about rock: the top of a fin sheds water and case-hardens; the
   flanks are what retreat.
3. **Locking must latch.** Recomputing it fresh each step means a voxel locks,
   stops eroding with its viability half spent, unlocks when the load finds a
   better path, and dies in a few steps. Fabric interlocking is a change *to the
   rock*, not a switch that flips back.

With all three fixed, the process converges properly — solid settles at ~55k
voxels with ~54k interlocked, instead of running to zero.

## Why the prototype is shelved rather than finished

It converges, and it does not look like an arch. It looks like a block with a
square hole: the flanks thin roughly evenly, the load concentrates into what is
left, and the rock locks solid while the opening is still the notch it started
as. Raising the bed contrast so one weak bed becomes an alcove did not change
it. Closing the gap means moving toward what Arenite actually does — a real
stress solve, wind as a transported medium, fluvial routing — which is a
SIGGRAPH paper's worth of work on a GPU, and we are numpy on a CPU.

And re-authoring the spec turned out to fix the asset, so the expensive route is
not needed to get a hero arch.

## Recommendation

1. **DONE — `hero-arch-colossal` is re-authored at `rock.arch` 0.55,
   `rock.flatten` 1.5** (was 0.9 and 2.3). Sizes come out 41–62 m rather than
   90, because the size fit targets the longest dimension and lowering
   `flatten` moves that from the vertical to the horizontal. The owner was
   asked about this directly and ruled that **90 m is not a requirement —
   "flex it as needed"** (2026-08-13), so the height is left where the shape
   wants it rather than compensated for with `rock.size_m`.
2. **Bake the chosen seed once and place it from the library.** This is one
   rock, in one place, in one world. A generator earns its keep when you need
   many instances that differ; nothing needs to rebuild this asset per-seed, and
   its cost (a 24 GB working set, 20 minutes, a CI leg that runs it at a quarter
   resolution to fit) is only paid because it is treated as a species.
3. **Fix the real defect, which is not the shape.** Three of six `desert-arch`
   seeds and one of six `hero-natural-arch` seeds ship with no through-hole at
   all — daylight of 0, 3 and 6 px. The carve reports success on those seeds. An
   arch spec that produces an arch half the time is a bug worth its own item,
   and `tools/archprobe.py` already instruments every guard that can cause it.
4. **Keep `tools/archgrow.py` and this document.** The stress-erosion feedback
   is the right mechanism for hoodoos, pedestals and alcoves as well as arches.
   If we ever want those procedurally, the three bugs above are already paid for
   — and the next step there is a real stress solve, not more tuning of the load
   proxy, scoped as its own piece of work.
