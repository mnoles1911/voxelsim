# Eight animals have no ears, and the code returns early to say so

Found 2026-08-16 while checking what the lattice change
(`docs/quadruped-lattice-decision.md`) had disturbed. **It disturbed nothing
here — every number below was already true.** The move only got a gate to
notice, which is the entire argument for having the gate.

---

## 1. What the probe said

`tools/quadprobe.py --stance --parts --caps` went from *0 things to look at* to
*1*:

    alpine-ibex   FAIL: no joint (corner contact only): ear-L

`forge/quadruped.py::_ears` is explicit that this must never happen:

> Every ear STARTS INSIDE THE SKULL, one radius in, for the same reason the neck
> does: an overlap by construction is the only join that survives, and
> `forge.parts.joints` reports no joint at all for a part touching its parent
> only at a corner.

Measured, `alpine-ibex` at its old 5 cm and its new 2 cm, variation off, seed 1,
voxels tagged per part:

| part | @5 cm | @2 cm | ratio |
|---|---|---|---|
| body | 702 | 10,729 | 15.3× |
| horn-L | 56 | 779 | 13.9× |
| leg-L | 47 | 455 | 9.7× |
| head | 39 | 477 | 12.2× |
| **ear-L** | **1** | **1** | **1.0×** |
| **ear-R** | **1** | **0** | **0.0×** |

Everything scales with the lattice by 10–15×. The ears do not scale at all, and
one of them disappears. **The ibex had one voxel of ear at 5 cm too** — the
lattice change did not break the ear, it removed the last voxel that was hiding
the fact that there wasn't one.

Not the headgear, which was the obvious suspect on a species with 62 cm horns:
built with `quad.headgear` forced to `none`, the counts are identical (1 and 0).

## 2. The systemic version, which is bigger and older

`ear_v = quad.ear_len × length_v` is a length in VOXELS, and `_ears` opens with

    if p["ear"] == "none" or p["ear_v"] < 1.0:
        return

so a species whose ear works out to less than one voxel **gets no ears and no
diagnostic**. Across the 115 quadrupeds that ask for ears:

* **8 species draw no ear at all** (`ear_v < 1.0`): `norway-lemming` 0.30,
  `european-souslik` 0.66, `north-american-porcupine` 0.70,
  `european-hedgehog` 0.75, `antelope-ground-squirrel` 0.80,
  `black-tailed-prairie-dog` 0.88, `arctic-ground-squirrel` 0.90,
  `eurasian-beaver` 0.90.
* **37 species have ears under three voxels long**, which is this project's own
  threshold for a feature being identifiable at all.
* Median across the library is 4.5 voxels, and the top of the range is fine —
  `black-tailed-jackrabbit` 14.3, `african-bush-elephant` 15.6.

**None of the eight is a species this lattice change touched.** Seven are on
1 cm or 2 cm and were authored that way; the shortfall is `quad.ear_len` being
a fraction of body length on animals whose ears are small fractions of very
short bodies. A prairie dog is 35 cm long and its ears round to nothing.

This is the project's signature failure in its purest form: a parameter is
authored, validated, saved, built and rendered, and the mechanism it names
returns before drawing anything. `docs/quadruped-proportion-research.md` says
ears "ARE the species" on a hare or a fennec — and on a hedgehog, ours are not
there at all.

## 3. What has NOT been decided

**No fix is applied here**, deliberately, because the three candidates are
different arguments and one of them is the owner's:

1. **Floor the ear at the readable minimum** rather than returning — draw
   `max(ear_v, LIMB_MIN_VOX)` so a small-eared animal gets a small readable ear.
   Cheapest, and it is the same move the limb radius floor already makes. It
   also makes a lemming's ears too big for a lemming, which is exactly the
   complaint that `quad.leg_thick`'s floor produced and took two passes to
   unpick (`docs/quadruped-limb-regression.md` §2). A floor that lies is how
   that dead row happened.
2. **Refuse at author time** — make `selftest` fail a spec whose `ear_v` is
   under one voxel, so the species is re-authored or moved to a finer lattice.
   Honest, and it converts eight silent absences into eight decisions.
3. **Move the eight to a finer lattice.** Six are already on 1 cm, which is the
   finest on the menu, so this cannot reach them. It is not a general answer.

My recommendation is **2 for the eight, and nothing for the 37** until somebody
looks at a render: three voxels is a rule of thumb about identifiability, and an
ear at 2.5 voxels on a badger may well be fine.

Separately, the ibex's *single-voxel* ear at `ear_v` 5.25 is not explained by
the early return — the ear is long enough and is drawn — so `_ears`'s
rasterisation is losing it somewhere between the root inset and the cone width.
That is a real investigation and it has not been done.
