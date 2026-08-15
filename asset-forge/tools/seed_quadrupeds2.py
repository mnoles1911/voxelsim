"""The rest of the land animals, biome by biome.

`tools/seed_quadrupeds.py` shipped the first twenty-four -- the owner's own list
plus the species that each exercise one mechanism nothing else touches. This is
the remainder of the `gen: quadruped` rows in `docs/biomes/*.md`: a hundred-odd
species across all ten biomes, and the point of it is coverage rather than
mechanism. Every mechanism these use is already proven; what is new is that a
grassland now has a badger and a hare and a marmot in it rather than a fox.

WHAT EVERY ROW HAS TO SAY, AND WHY
----------------------------------
`resolution_cm` is chosen by the house rule and NOT copied from the biome file:
"the coarsest voxel size at which the smallest identifying feature is still
about three voxels across" (`forge/kinds.py:29-58`). Each row names the feature
that decided it, because a lattice with no reason attached is a lattice the
next person will change. Where this file disagrees with a biome file's Lattice
column -- and it does about twenty times -- the row says so and says why: those
columns are the document's own admission of an estimate (`README.md` §8), and
`tools/quadprobe.py --lattice` is what settles it.

THE TWENTY-CENTIMETRE FLOOR. The owner's minimum species size is 20 cm of real
length, and it is not a lattice: below it there are too few voxels to carry
identity at any lattice this project has. Eleven species here are genuinely
smaller than that -- a mole, a dart frog, a lemming -- and each is AUTHORED UP
with the arithmetic in its own `notes`. That note is the whole point. A minnow
was nearly "corrected" back to 7 cm once, and the note is what stops it.

FOUR SPECIES ARE NOT HERE AND THE REASON IS GEOMETRY, NOT EFFORT.
`quad.mark` offers bars, spots, saddle, flankstripe, dapple and blotch. A
LEOPARD'S ROSETTE IS AN ANNULUS -- a dark ring with a tawny centre -- and a
GIRAFFE'S RETICULATION IS A PARTITION into plates separated by lines. Neither
is any setting of the six. So `leopard`, `jaguar`, `snow-leopard` and
`reticulated-giraffe` are left unauthored rather than shipped with the wrong
coat, and §5 of this file's report says so. A CHEETAH IS NOT IN THAT GROUP: its
spots are solid and round, which is exactly `spots`, and it is authored below.

THE PROBE CAUGHT FOURTEEN INVISIBLE BOUNDARIES ON THE FIRST RUN, which is
this project's signature failure and the reason `tools/quadprobe.py --read`
exists: a colour with no value contrast against what it sits on. Forty birds
shipped that way on the last pass. Thirteen of the fourteen were the pale
UNDERSIDE against the back -- five of them the same colour on both sides, a
countershading boundary at a contrast ratio of exactly 1.00, which is a
boundary that does not exist. All thirteen were fixed by moving the belly, and
the fourteenth by moving the ANIMAL: see `ocellated-lizard`, whose blue ocelli
could not be kept on a green body at any blue in the palette.

SIZES ARE APPROXIMATE AND NOTHING HERE IS CITED. Every figure is the
approximate typical adult from the biome file it came from, and those files say
in so many words (`docs/biomes/README.md` §8) that none of it is measured or
sourced -- because this project has already shipped a fabricated citation. So
no row here quotes a source, and where a number matters it is written as the
estimate it is.

    python tools/seed_quadrupeds2.py            # write the ones that do not exist
    python tools/seed_quadrupeds2.py --force    # overwrite tuned specs with drafts

`--force` DISCARDS TUNING. `tools/seedspec.py` refuses to overwrite an existing
spec without it, because a seed script is the DRAFT a species started from and
what is on disk is the result of tuning it.
"""
from __future__ import annotations

import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"

# The same starting point `seed_quadrupeds.py` uses, repeated rather than
# imported: a seed script is a record of what a species was authored FROM, and
# a base that can change under it from another file is a base that makes the
# record wrong.
BASE = {
    "kind": "quadruped",
    "resolution_cm": "2",
    "quad.stance": "standing",
    "quad.section": 2.2,
    "quad.eye": 1,
    "quad.ear_shape": "pointed",
    "materials.quad_horn": "beak_horn",
    "materials.quad_eye": "skin_dark",
    "herd.entity_class": "detail",
    "herd.cover": "any",
    "variation.amount": 1.0,
    "variation.height": 0.09,
    "variation.proportion": 0.14,
    "variation.shape": 0.12,
}

SPECIES: list[tuple[str, str, dict]] = []


def add(name: str, notes: str, **patch) -> None:
    SPECIES.append((name, notes, patch))


# ===========================================================================
# DEER, AND THE ONE ANTELOPE THAT LOOKS LIKE ONE
#
# The shipped tranche has a moose (palmate), a red deer stag (branched, rack
# authored thicker than life) and a fallow deer (palmate at a quarter of the
# size). What it has NO example of is a deer with no headgear at all, which is
# most of the deer a player will actually meet, and none of the small ones.
# ===========================================================================

add(
    "red-deer-hind",
    "THE SAME SPECIES AS `red-deer-stag` AND A DELIBERATELY SEPARATE ASSET. "
    "`quad.sex` would give a hind from the stag's spec -- `sex_horn` is 0 there, "
    "which means the female carries no antlers at all rather than short ones -- "
    "so this file could have skipped the row. It does not, and the reason is "
    "the lattice: the stag sits at 2 cm ONLY to hold a rack, and a hind has no "
    "rack to hold. Drawn from the stag's spec she would cost the voxels of a "
    "feature she does not have. At 5 cm she is 38 voxels of head-body, her "
    "slimmer neck and the pale rump patch both still read, and she is a "
    "quarter of the asset.\n\n"
    "LATTICE: 5 cm. The smallest thing that identifies a hind is the rump patch "
    "boundary at roughly 20 cm on a 1.9 m animal -- four voxels, past the rule. "
    "`02-grassland.md` and `03-temperate-forest.md` both recommend 5 cm for "
    "this row and both are right.",
    resolution_cm="5",
    **{
        "quad.length_m": 1.90,
        "quad.shoulder_h": 0.60,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.21,
        "quad.head_frac": 0.12,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 40.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.40,
        "quad.width": 0.50,
        "quad.chest": 0.98,
        "quad.waist": 0.94,
        "quad.rump": 1.00,
        "quad.belly": 0.50,
        # The whole visible difference from the stag, and it is one row: a neck
        # at 0.48 against his 0.72, with no mane on it.
        "quad.neck_thick": 0.48,
        "quad.neck_taper": 0.66,
        "quad.mane": 0.0,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.08,
        "quad.ear_width": 0.58,
        "quad.ear_deg": 56.0,
        "quad.horn_shape": "none",
        "quad.tail_len": 0.08,
        "quad.tail_thick": 0.16,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.15,
        "quad.hock": 0.38,
        "quad.under": 0.26,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "edge",
        "herd.size_min": 3,
        "herd.size_max": 20,
        "herd.spread_m": 60.0,
        "herd.despawn_m": 320.0,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 180.0,
        "biomes.temperate_forest": 0.9,
        "biomes.grassland": 0.6,
        "biomes.taiga": 0.5,
        "biomes.tundra_alpine": 0.3,
    },
)

add(
    "roe-deer",
    "The small deer, and a genuinely different SHAPE from the red deer rather "
    "than a smaller one: short in the body, high in the rump -- hips visibly "
    "above the withers -- with a bright white rump patch and a rack of only "
    "three points a side.\n\n"
    "LATTICE: 2 cm. The antler is the feature and it is a short one: a roe "
    "beam is roughly 2 cm thick on a 1.2 m animal, which is one voxel at 2 cm "
    "and nothing at 5. `quad.horn_thick` is therefore authored at 0.24 against "
    "a life-size figure near 0.10, so the beam comes out around 3 voxels. THIS "
    "IS THE CLASS (b) FIX from `docs/biomes/README.md` §6, the same one "
    "`red-deer-stag` uses and for the same reason, and it must not be "
    "'corrected' back: at a life-size thickness the render is a doe.",
    **{
        "quad.length_m": 1.20,
        "quad.shoulder_h": 0.58,
        # HIPS ABOVE THE WITHERS. The one row that makes a roe a roe at any
        # distance, and the opposite of every other deer in the library.
        "quad.hip_h": 1.10,
        "quad.trunk_frac": 0.53,
        "quad.neck_frac": 0.20,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 44.0,
        "quad.head_deg": -8.0,
        "quad.depth": 0.38,
        "quad.width": 0.50,
        "quad.chest": 0.94,
        "quad.waist": 0.92,
        "quad.rump": 1.10,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.44,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.11,
        "quad.ear_width": 0.48,
        "quad.ear_deg": 62.0,
        "quad.horn_shape": "branched",
        "quad.horn_len": 0.18,
        "quad.horn_thick": 0.24,
        "quad.horn_spread": 0.40,
        "quad.horn_tines": 3,
        # Effectively tail-less, which is why the rump patch reads at all.
        "quad.tail_len": 0.03,
        "quad.tail_thick": 0.16,
        "quad.tail_deg": -60.0,
        "quad.leg_thick": 0.14,
        "quad.hock": 0.42,
        "quad.under": 0.30,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "plume_white",
        "quad.sex_horn": 0.0,
        "herd.cover": "edge",
        "herd.size_min": 1,
        "herd.size_max": 5,
        "herd.spread_m": 30.0,
        "herd.per_hectare": 0.6,
        "herd.flee_m": 120.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.6,
        "biomes.taiga": 0.4,
    },
)

add(
    "sika-deer",
    "A compact dark deer that carries its spots into adulthood, with a white "
    "rump patch bordered in black -- which is a marking sitting against a "
    "marking, and the only place in this file that happens. The border is "
    "drawn as the dark body itself rather than as a second mark, because "
    "`quad.mark` is ONE marking on the flank and a second one is not "
    "available; the black edge is the coat the patch is cut out of.\n\n"
    "LATTICE: 2 cm, AGAINST `03-temperate-forest.md`'s recommendation of 5, "
    "and the disagreement was measured rather than argued. 5 cm was authored "
    "first and built at 401 voxels: a 1.4 m animal at 5 cm is 28 voxels of "
    "head-body, which is the exact trap `alpaca` records in its own notes -- "
    "the bottom of the whole library's range, below every fish in it -- and "
    "the spots at a real 4 cm were under one voxel on top of that. At 2 cm the "
    "body is 70 voxels and a spot is 2, which is above the two-voxel floor and "
    "below the three-voxel rule: the spots are still COARSER THAN LIFE through "
    "`mark_width`, exactly the compromise `fallow-deer` records, but the "
    "animal is no longer eight boxes.",
    **{
        "quad.length_m": 1.40,
        "quad.shoulder_h": 0.61,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.20,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 38.0,
        "quad.head_deg": -8.0,
        "quad.depth": 0.40,
        "quad.width": 0.52,
        "quad.chest": 1.00,
        "quad.waist": 0.94,
        "quad.rump": 1.02,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.50,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.48,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.085,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 56.0,
        "quad.horn_shape": "branched",
        "quad.horn_len": 0.30,
        "quad.horn_thick": 0.20,
        "quad.horn_spread": 0.50,
        "quad.horn_tines": 4,
        "quad.tail_len": 0.14,
        "quad.tail_thick": 0.16,
        "quad.tail_deg": -44.0,
        "quad.leg_thick": 0.15,
        "quad.hock": 0.38,
        "quad.under": 0.24,
        "quad.mark": "dapple",
        "quad.mark_count": 14,
        "quad.mark_width": 0.20,
        "quad.mark_strength": 0.65,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "plume_white",
        "quad.sex_horn": 0.0,
        "quad.sex_length": 1.15,
        "herd.cover": "forest",
        "herd.size_min": 2,
        "herd.size_max": 14,
        "herd.spread_m": 45.0,
        "herd.per_hectare": 0.6,
        "herd.flee_m": 120.0,
        "biomes.temperate_forest": 0.9,
        "biomes.grassland": 0.3,
    },
)

add(
    "white-tailed-deer",
    "THE TAIL IS THE SPECIES AND IT IS THE ONE POSE THIS GENERATOR CANNOT "
    "AUTHOR HALFWAY. A white-tail is identified by a broad tail held straight "
    "up showing pure white underneath as it flees -- so the tail is drawn "
    "raised, at `tail_deg` 80, wide and white-tipped. That is a fleeing animal "
    "standing still, which is a small dishonesty and worth naming: a grazing "
    "white-tail carries the tail down and is then a plain tan deer. Both are "
    "reachable from this spec by one field, and the raised one is authored "
    "because it is the one anybody recognises.\n\n"
    "LATTICE: 5 cm. The raised tail is 25-30 cm on a 1.8 m animal, five voxels "
    "at 5 cm, comfortably past the rule -- which is unusual here: on most deer "
    "the lattice is set by something small and on this one it is set by the "
    "largest feature the animal has.",
    resolution_cm="5",
    **{
        "quad.length_m": 1.80,
        "quad.shoulder_h": 0.58,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.53,
        "quad.neck_frac": 0.21,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 40.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.38,
        "quad.width": 0.50,
        "quad.chest": 0.96,
        "quad.waist": 0.92,
        "quad.rump": 1.04,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.46,
        "quad.head_size": 0.92,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.10,
        "quad.ear_width": 0.52,
        "quad.ear_deg": 58.0,
        "quad.horn_shape": "branched",
        "quad.horn_len": 0.28,
        "quad.horn_thick": 0.19,
        "quad.horn_spread": 0.62,
        "quad.horn_tines": 4,
        "quad.tail_len": 0.18,
        "quad.tail_thick": 0.34,
        "quad.tail_taper": 0.85,
        "quad.tail_deg": 80.0,
        "quad.tail_tip": 0.60,
        "quad.leg_thick": 0.14,
        "quad.hock": 0.40,
        "quad.under": 0.30,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "plume_white",
        "quad.sex_horn": 0.0,
        "quad.sex_length": 1.15,
        "herd.cover": "edge",
        "herd.size_min": 2,
        "herd.size_max": 12,
        "herd.spread_m": 50.0,
        "herd.despawn_m": 320.0,
        "herd.per_hectare": 0.8,
        "herd.flee_m": 150.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.5,
        "biomes.taiga": 0.3,
    },
)

add(
    "elk-wapiti",
    "The largest branched-antler deer, and a bigger animal than the red deer "
    "in every direction: a dark neck mane against a pale body, a pale rump "
    "patch, and a rack that sweeps BACK along the spine rather than up. That "
    "backward sweep is `horn_curl` doing the work `horn_spread` does on a "
    "fallow deer, and it is what separates the two racks at a distance.\n\n"
    "LATTICE: 5 cm, and this is a DEPARTURE from what the shipped stag does, "
    "made deliberately. A red deer at 2 m had to go to 2 cm to hold a rack. An "
    "elk is 2.4 m and its beam is proportionally heavier -- roughly 5 cm at "
    "the base against a red deer's 3-4 -- so at 5 cm the beam is one voxel and "
    "the tines nothing. `horn_thick` is therefore authored at 0.20 against a "
    "life-size figure near 0.09, which puts the beam near 3 voxels at 5 cm. "
    "The rack is THICKER THAN LIFE and that is the point; do not correct it "
    "back, and do not move the animal to 2 cm to fix it -- a 2.4 m elk at 2 cm "
    "is 120 voxels of body for a feature that a thickness field already buys.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.40,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 0.96,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.23,
        "quad.head_frac": 0.12,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 36.0,
        "quad.head_deg": -12.0,
        "quad.depth": 0.42,
        "quad.width": 0.54,
        "quad.chest": 1.06,
        "quad.waist": 0.94,
        "quad.rump": 1.00,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.76,
        "quad.neck_taper": 0.58,
        "quad.mane": 0.30,
        "quad.head_size": 0.92,
        "quad.muzzle_depth": 0.55,
        "quad.muzzle_width": 0.46,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.50,
        "quad.ear_deg": 54.0,
        "quad.horn_shape": "branched",
        "quad.horn_len": 0.46,
        "quad.horn_thick": 0.20,
        "quad.horn_spread": 0.45,
        "quad.horn_curl": 0.75,
        "quad.horn_tines": 6,
        "quad.tail_len": 0.06,
        "quad.tail_thick": 0.16,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.15,
        "quad.hock": 0.36,
        "quad.under": 0.20,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "skin_dark",
        "quad.sex_horn": 0.0,
        "quad.sex_mane": 2.0,
        "quad.sex_length": 1.20,
        "herd.cover": "edge",
        "herd.size_min": 3,
        "herd.size_max": 30,
        "herd.spread_m": 80.0,
        "herd.despawn_m": 380.0,
        "herd.per_hectare": 0.4,
        "herd.flee_m": 200.0,
        "biomes.temperate_forest": 0.9,
        "biomes.taiga": 0.6,
        "biomes.grassland": 0.4,
        "biomes.tundra_alpine": 0.3,
    },
)

add(
    "reindeer",
    "THE ONLY DEER IN THE LIBRARY WHERE BOTH SEXES CARRY ANTLERS, which is "
    "`quad.sex_horn` held at 1.0 on a deer -- every other one here uses 0. "
    "Also the pale neck ruff, the short muzzle and the splayed hooves, which "
    "is `quad.foot` at 1.7 and is a real proportion rather than decoration: a "
    "reindeer's hoof spreads to carry it on snow.\n\n"
    "LATTICE: 5 cm, against the biome files' 2 cm, and the disagreement is "
    "worth stating. `07-taiga.md` and `08-tundra-alpine.md` both flag this row "
    "for the FORWARD BROW TINE -- a flattened blade that hangs over the face "
    "and is the one thing that says reindeer rather than red deer. At a real "
    "3-4 cm it fails the rule at 5 cm AND at 2 cm, which is class (b): there "
    "is no lattice at which it reads at life size. So it is authored oversize "
    "instead, as a palmate rack -- `horn_shape` PALMATE, not branched -- "
    "because a palm is a flat blade and a flat blade is exactly what a brow "
    "tine is, and palmate survives 5 cm where round tines do not. The rack is "
    "therefore SIMPLER THAN LIFE and the brow tine is READABLE, and that trade "
    "is the whole reason this row is not at 2 cm.",
    resolution_cm="5",
    **{
        "quad.length_m": 1.90,
        "quad.shoulder_h": 0.58,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.20,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 30.0,
        "quad.head_deg": -16.0,
        "quad.depth": 0.42,
        "quad.width": 0.54,
        "quad.chest": 1.02,
        "quad.waist": 0.94,
        "quad.rump": 0.98,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.72,
        "quad.neck_taper": 0.62,
        "quad.dewlap": 0.30,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.50,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 0.72,
        "quad.ear_deg": 46.0,
        "quad.horn_shape": "palmate",
        "quad.horn_len": 0.36,
        "quad.horn_thick": 0.18,
        "quad.horn_spread": 0.70,
        "quad.horn_curl": 0.50,
        "quad.horn_tines": 4,
        "quad.tail_len": 0.07,
        "quad.tail_thick": 0.18,
        "quad.tail_deg": -46.0,
        "quad.leg_thick": 0.17,
        "quad.hock": 0.34,
        # The splayed hoof.
        "quad.foot": 1.70,
        "quad.under": 0.30,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "plume_buff",
        # BOTH SEXES. The row this species exists to demonstrate.
        "quad.sex_horn": 1.0,
        "quad.sex_length": 1.15,
        "herd.cover": "open",
        "herd.size_min": 6,
        "herd.size_max": 120,
        "herd.spread_m": 200.0,
        "herd.despawn_m": 450.0,
        "herd.per_hectare": 1.2,
        "herd.flee_m": 150.0,
        "biomes.tundra_alpine": 1.0,
        "biomes.taiga": 0.8,
    },
)

add(
    "siberian-musk-deer",
    "A deer with NO ANTLERS AT ALL and two long downward tusks instead, on an "
    "arched back whose hindquarters stand well above its shoulders. It is the "
    "clearest test in the library that `quad.horn_shape` SPIKE points where it "
    "is told rather than only upward: the tusks are a pair of spikes with a "
    "near-zero spread and a full downward carriage, and if the headgear code "
    "had assumed 'up' this species would have been unbuildable.\n\n"
    "LATTICE: 1 cm. The tusks are the feature and they are thin -- roughly "
    "1 cm across on a 0.85 m animal -- so three voxels wants 0.3 cm, which no "
    "lattice here reaches. This is class (b): the tusk is authored THICKER "
    "THAN LIFE at `horn_thick` 0.16 against a life figure near 0.05, giving "
    "about 2 voxels at 1 cm. `07-taiga.md` recommends 1 cm for exactly this "
    "row and flags it. At 2 cm the tusks are one voxel and the animal is a "
    "hornless doe with a hunched back.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.85,
        "quad.shoulder_h": 0.58,
        # Rump far above the withers -- more extreme than the roe deer, and the
        # silhouette a musk deer is known by.
        "quad.hip_h": 1.22,
        "quad.trunk_frac": 0.55,
        "quad.neck_frac": 0.18,
        "quad.head_frac": 0.14,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 42.0,
        "quad.head_deg": -14.0,
        "quad.depth": 0.40,
        "quad.width": 0.50,
        "quad.chest": 0.90,
        "quad.waist": 0.94,
        "quad.rump": 1.16,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.46,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.38,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.14,
        "quad.ear_width": 0.46,
        "quad.ear_deg": 66.0,
        "quad.horn_shape": "spike",
        "quad.horn_len": 0.12,
        "quad.horn_thick": 0.16,
        "quad.horn_spread": 0.06,
        "quad.horn_curl": 0.90,
        "quad.tail_len": 0.05,
        "quad.tail_thick": 0.20,
        "quad.tail_deg": -55.0,
        "quad.leg_thick": 0.14,
        "quad.hock": 0.55,
        "quad.under": 0.28,
        "quad.mark": "dapple",
        "quad.mark_count": 10,
        "quad.mark_width": 0.14,
        "quad.mark_strength": 0.45,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_buff",
        "materials.quad_horn": "plume_white",
        "quad.sex_horn": 0.0,
        "herd.cover": "forest",
        "herd.size_max": 2,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 90.0,
        "biomes.taiga": 0.9,
        "biomes.tundra_alpine": 0.3,
    },
)

add(
    "pronghorn",
    "The fastest animal on the list and it looks it: very slender, very "
    "leggy, tan with TWO bold white throat bands and a white rump, and short "
    "forked horns. The two throat bands are the field mark and they are the "
    "one thing this generator cannot give it -- `quad.mark` is a single "
    "marking on the FLANK, and a throat band is neither. They are left out "
    "rather than faked as flank bars, which would put stripes on the wrong "
    "half of the animal. What carries the species instead is the white rump "
    "and the countershading boundary, which is `flankstripe`.\n\n"
    "LATTICE: 2 cm. The horn prong is the smallest identifying feature at "
    "roughly 4 cm on a 1.4 m animal, so three voxels wants 1.3 cm and 2 cm "
    "gives 2. Under the rule and accepted, because the alternative is 1 cm and "
    "a 140-voxel body for a prong.",
    **{
        "quad.length_m": 1.40,
        "quad.shoulder_h": 0.64,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.22,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 44.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.36,
        "quad.width": 0.46,
        "quad.chest": 1.02,
        "quad.waist": 0.88,
        "quad.rump": 0.98,
        "quad.belly": 0.46,
        "quad.neck_thick": 0.48,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.38,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.10,
        "quad.ear_width": 0.48,
        "quad.ear_deg": 64.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.16,
        "quad.horn_thick": 0.20,
        "quad.horn_spread": 0.40,
        "quad.horn_curl": 0.35,
        "quad.tail_len": 0.08,
        "quad.tail_thick": 0.16,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.11,
        "quad.hock": 0.36,
        "quad.under": 0.42,
        "quad.mark": "flankstripe",
        "quad.mark_width": 0.10,
        "quad.mark_strength": 0.9,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "skin_brown",
        "quad.sex_horn": 0.35,
        "herd.cover": "open",
        "herd.size_min": 3,
        "herd.size_max": 30,
        "herd.spread_m": 90.0,
        "herd.despawn_m": 400.0,
        "herd.per_hectare": 0.9,
        "herd.flee_m": 200.0,
        "biomes.grassland": 1.0,
        "biomes.desert": 0.3,
    },
)

add(
    "saiga-antelope",
    "AN ORDINARY ANTELOPE UNDER AN ABSURD NOSE, and the nose is the only thing "
    "anyone will look at. It is drawn as a muzzle that is deep, wide and "
    "DROOPED -- `muzzle_depth` 1.10, `muzzle_width` 0.95 and `muzzle_drop` "
    "0.85, which are the three highest muzzle numbers in this file -- so the "
    "snout hangs over the mouth as a bulb rather than tapering to it. That is "
    "the same mechanism the moose uses for its overhanging lip, pushed "
    "further, and it is the reason `quad.muzzle_drop` was worth having as a "
    "separate row from depth.\n\n"
    "LATTICE: 2 cm. The nose is 12-15 cm on a 1.3 m animal, six voxels at "
    "2 cm; the horns at 3 cm across are the thing that does not survive and "
    "are authored thicker, as elsewhere in this file.",
    **{
        "quad.length_m": 1.30,
        "quad.shoulder_h": 0.56,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.18,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.15,
        "quad.neck_deg": 34.0,
        "quad.head_deg": -14.0,
        "quad.depth": 0.40,
        "quad.width": 0.54,
        "quad.chest": 1.00,
        "quad.waist": 0.96,
        "quad.rump": 1.00,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.52,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 1.10,
        "quad.muzzle_width": 0.95,
        "quad.muzzle_drop": 0.85,
        "quad.jaw": 0.20,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.06,
        "quad.ear_width": 0.70,
        "quad.ear_deg": 50.0,
        "quad.horn_shape": "spike",
        "quad.horn_len": 0.20,
        "quad.horn_thick": 0.18,
        "quad.horn_spread": 0.28,
        "quad.tail_len": 0.08,
        "quad.tail_thick": 0.16,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.13,
        "quad.hock": 0.34,
        "quad.under": 0.34,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "materials.quad_horn": "plume_white",
        "quad.sex_horn": 0.0,
        "herd.cover": "open",
        "herd.size_min": 5,
        "herd.size_max": 80,
        "herd.spread_m": 150.0,
        "herd.despawn_m": 400.0,
        "herd.per_hectare": 1.4,
        "herd.flee_m": 180.0,
        "biomes.grassland": 1.0,
        "biomes.desert": 0.4,
    },
)

add(
    "dorcas-gazelle",
    "A small desert gazelle, and the species that shows `quad.under` and "
    "`flankstripe` working together at the small end: fawn above, white below, "
    "with a clean dark line exactly on the boundary between them. The horns "
    "curve back and then forward, which is `horn_curl` past 0.6 on a CURVE.\n\n"
    "LATTICE: 2 cm. The side stripe is roughly 4 cm on a 0.95 m animal, two "
    "voxels at 2 cm -- above the floor, under the rule -- and 1 cm would be "
    "95 voxels for a line. The horn rings, which are a real field mark, do not "
    "survive at either and are left out rather than drawn as noise.",
    **{
        "quad.length_m": 0.95,
        "quad.shoulder_h": 0.63,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.22,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 46.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.36,
        "quad.width": 0.46,
        "quad.chest": 0.94,
        "quad.waist": 0.90,
        "quad.rump": 1.00,
        "quad.belly": 0.46,
        "quad.neck_thick": 0.42,
        "quad.head_size": 0.92,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.36,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.12,
        "quad.ear_width": 0.44,
        "quad.ear_deg": 60.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.22,
        "quad.horn_thick": 0.14,
        "quad.horn_spread": 0.40,
        "quad.horn_curl": 0.68,
        "quad.tail_len": 0.16,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.55,
        "quad.tail_deg": -52.0,
        "quad.tail_tip": 0.35,
        "quad.leg_thick": 0.11,
        "quad.hock": 0.36,
        "quad.under": 0.36,
        "quad.mark": "flankstripe",
        "quad.mark_width": 0.10,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "quad.sex_horn": 0.55,
        "herd.cover": "open",
        "herd.size_min": 2,
        "herd.size_max": 16,
        "herd.spread_m": 60.0,
        "herd.per_hectare": 0.7,
        "herd.flee_m": 140.0,
        "biomes.desert": 1.0,
        "biomes.savanna": 0.4,
        "biomes.grassland": 0.2,
    },
)

add(
    "klipspringer",
    "THE FIRST SPECIES IN THE LIBRARY AUTHORED FOR BARE ROCK, which only "
    "became possible when the owner added `quadruped` to that biome's `hosts` "
    "tuple (`forge/biomes.py`, 2026-08-15) -- the 35-degree gate is the angle "
    "of repose for LOOSE MATERIAL and has never been the angle at which a "
    "hoofed animal loses footing.\n\n"
    "A tiny antelope that stands on the very TIPS of its hooves, which gives "
    "it an arched back and a stance nothing else here has. `quad.foot` is held "
    "DOWN at 0.45 rather than up: a tiptoe stance is a small foot on a long "
    "leg, and the arch comes from a rump carried above the withers. Straight "
    "spike horns and oversized round ears finish it.\n\n"
    "THE HORNS ARE AUTHORED THICKER THAN LIFE, AND THE PROBE IS WHY. At a life-size `horn_thick` near 0.22 the spikes are under two voxels at 2 cm and `tools/quadprobe.py --parts` reported horn-R, then horn-L, attached to the skull at a CORNER ONLY -- no joint, a part that falls off the rig. 0.46 gives a real overlap. This is the class (b) fix again and it is the only place in the file where the RIG rather than the eye forced it.\n\n"
    "LATTICE: 2 cm. The horns are 8-10 cm on a 0.85 m animal -- four voxels at "
    "2 cm -- and the speckled coat, which is the other field mark, is finer "
    "than any lattice here and is left out rather than drawn as dirt.",
    **{
        "quad.length_m": 0.85,
        "quad.shoulder_h": 0.63,
        "quad.hip_h": 1.14,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.18,
        "quad.head_frac": 0.15,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 46.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.40,
        "quad.width": 0.50,
        "quad.chest": 0.94,
        "quad.waist": 0.96,
        "quad.rump": 1.10,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.46,
        "quad.head_size": 0.98,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.38,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.10,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 58.0,
        "quad.horn_shape": "spike",
        "quad.horn_len": 0.15,
        "quad.horn_thick": 0.46,
        "quad.horn_spread": 0.24,
        "quad.tail_len": 0.06,
        "quad.tail_thick": 0.18,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.12,
        "quad.hock": 0.40,
        # Tiptoe: a SMALL foot on a long leg.
        "quad.foot": 0.45,
        "quad.under": 0.24,
        "materials.quad_back": "skin_olive",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "skin_olive",
        "materials.quad_tail": "skin_olive",
        "materials.quad_mark": "skin_dark",
        "quad.sex_horn": 0.0,
        "herd.cover": "rock",
        "herd.size_min": 1,
        "herd.size_max": 2,
        "herd.per_hectare": 0.4,
        "herd.flee_m": 60.0,
        "biomes.bare_rock": 0.9,
        "biomes.savanna": 0.5,
        "biomes.tundra_alpine": 0.2,
    },
)


# ===========================================================================
# CATTLE, SHEEP AND GOATS -- the horn shapes, and the hump
#
# `quad.horn_shape` has five real settings and the shipped tranche used three
# of them once each. This section uses all five repeatedly, which is the only
# way to find out whether they are five shapes or one shape with a slider.
# ===========================================================================

add(
    "european-bison",
    "THE WISENT, and the whole point of it is that it is the American bison "
    "with three numbers changed: taller, longer-legged, less humped and less "
    "shaggy. `hump` 0.24 against the American's 0.36, `cape` 0.18 against "
    "0.30, `shoulder_h` 0.66 against 0.58. Nothing else differs. If the two "
    "cannot be told apart on a contact sheet then `quad.hump` and `quad.cape` "
    "are decoration rather than parameters, and that is worth knowing.\n\n"
    "LATTICE: 5 cm. 2.9 m of head-body is 58 voxels, and the identifying "
    "features -- the hump relief and the cape boundary -- are both tens of "
    "centimetres. The horns at roughly 8 cm at the base are 1.6 voxels and "
    "are drawn at the one-voxel floor, which is the same accepted loss "
    "`american-bison` records: the horns are not what identifies a bison.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.90,
        "quad.shoulder_h": 0.66,
        "quad.hip_h": 0.92,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": -8.0,
        "quad.head_deg": -18.0,
        "quad.depth": 0.44,
        "quad.width": 0.62,
        "quad.chest": 1.08,
        "quad.waist": 0.94,
        "quad.rump": 0.84,
        "quad.belly": 0.52,
        "quad.section": 2.8,
        "quad.hump": 0.24,
        "quad.hump_at": 0.20,
        "quad.neck_thick": 0.88,
        "quad.neck_taper": 0.70,
        "quad.head_size": 1.10,
        "quad.muzzle_depth": 0.68,
        "quad.muzzle_width": 0.76,
        "quad.jaw": 0.55,
        "quad.mane": 0.22,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.045,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 30.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.10,
        "quad.horn_thick": 0.36,
        "quad.horn_spread": 1.05,
        "quad.horn_curl": 0.50,
        "quad.tail_len": 0.22,
        "quad.tail_thick": 0.16,
        "quad.tail_taper": 0.45,
        "quad.tail_deg": -58.0,
        "quad.tail_tuft": 0.55,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.28,
        "quad.fore_bend": 0.14,
        "quad.foot": 1.10,
        "quad.under": 0.18,
        "quad.cape": 0.18,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "skin_dark",
        "materials.quad_horn": "skin_pale",
        "quad.sex_length": 1.18,
        "herd.cover": "edge",
        "herd.size_min": 3,
        "herd.size_max": 30,
        "herd.spread_m": 90.0,
        "herd.despawn_m": 400.0,
        "herd.per_hectare": 0.35,
        "herd.flee_m": 80.0,
        "biomes.temperate_forest": 0.8,
        "biomes.grassland": 0.6,
        "biomes.taiga": 0.4,
    },
)

add(
    "wood-bison",
    "The third bison, and the largest: a triangular forequarter hump standing "
    "much taller than the hips, the head carried very low, and a shaggy cape "
    "over the shoulders and FORELEGS against a smooth rear. Against the "
    "American bison the difference is the hump's shape rather than its size -- "
    "`hump_at` 0.12 puts it forward, over the shoulder rather than behind it, "
    "which is what makes the profile a triangle instead of a dome.\n\n"
    "LATTICE: 5 cm, 58 voxels of head-body, same reasoning as the other two.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.90,
        "quad.shoulder_h": 0.58,
        "quad.hip_h": 0.80,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.13,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": -26.0,
        "quad.head_deg": -30.0,
        "quad.depth": 0.48,
        "quad.width": 0.68,
        "quad.chest": 1.14,
        "quad.waist": 0.92,
        "quad.rump": 0.74,
        "quad.belly": 0.52,
        "quad.section": 2.8,
        "quad.hump": 0.44,
        "quad.hump_at": 0.12,
        "quad.neck_thick": 0.94,
        "quad.neck_taper": 0.72,
        "quad.head_size": 1.18,
        "quad.muzzle_depth": 0.70,
        "quad.muzzle_width": 0.80,
        "quad.jaw": 0.55,
        "quad.mane": 0.34,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.04,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 28.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.11,
        "quad.horn_thick": 0.38,
        "quad.horn_spread": 1.10,
        "quad.horn_curl": 0.58,
        "quad.tail_len": 0.20,
        "quad.tail_thick": 0.16,
        "quad.tail_taper": 0.45,
        "quad.tail_deg": -60.0,
        "quad.tail_tuft": 0.60,
        "quad.leg_thick": 0.27,
        "quad.hock": 0.26,
        "quad.fore_bend": 0.14,
        "quad.foot": 1.10,
        "quad.under": 0.16,
        "quad.cape": 0.38,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "skin_brown",
        "materials.quad_horn": "skin_pale",
        "quad.sex_length": 1.20,
        "herd.cover": "edge",
        "herd.size_min": 3,
        "herd.size_max": 40,
        "herd.spread_m": 100.0,
        "herd.despawn_m": 400.0,
        "herd.per_hectare": 0.4,
        "herd.flee_m": 80.0,
        "biomes.taiga": 1.0,
        "biomes.grassland": 0.3,
        "biomes.tundra_alpine": 0.2,
    },
)

add(
    "cape-buffalo",
    "THE BOSS -- a helmet of fused horn across the forehead that the two horns "
    "drop out of, down, and then hook up from. That fused plate is drawn with "
    "`horn_spread` near 1.6 and `horn_curl` 0.85 on a CURVE: the pair start "
    "almost touching over the skull and travel a long way round. It is the "
    "widest headgear in the file and it is the reason `horn_spread` runs to 2 "
    "rather than stopping at 1.\n\n"
    "LATTICE: 5 cm. 2.8 m is 56 voxels and the boss is 40-50 cm across, eight "
    "to ten voxels.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.80,
        "quad.shoulder_h": 0.55,
        "quad.hip_h": 0.94,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": -12.0,
        "quad.head_deg": -24.0,
        "quad.depth": 0.46,
        "quad.width": 0.66,
        "quad.chest": 1.10,
        "quad.waist": 0.98,
        "quad.rump": 0.96,
        "quad.belly": 0.54,
        "quad.section": 2.7,
        "quad.hump": 0.14,
        "quad.hump_at": 0.18,
        "quad.neck_thick": 0.90,
        "quad.head_size": 1.10,
        "quad.muzzle_depth": 0.70,
        "quad.muzzle_width": 0.82,
        "quad.jaw": 0.50,
        "quad.ear_shape": "fan",
        "quad.ear_len": 0.06,
        "quad.ear_width": 1.20,
        "quad.ear_deg": 12.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.22,
        "quad.horn_thick": 0.30,
        "quad.horn_spread": 1.60,
        "quad.horn_curl": 0.85,
        "quad.tail_len": 0.30,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.40,
        "quad.tail_deg": -62.0,
        "quad.tail_tuft": 0.60,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.28,
        "quad.foot": 1.10,
        "quad.under": 0.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_grey",
        "materials.quad_horn": "skin_brown",
        "quad.sex_length": 1.15,
        "herd.cover": "open",
        "herd.size_min": 5,
        "herd.size_max": 150,
        "herd.spread_m": 150.0,
        "herd.despawn_m": 450.0,
        "herd.per_hectare": 1.0,
        "herd.flee_m": 60.0,
        "biomes.savanna": 1.0,
        "biomes.grassland": 0.3,
    },
)

add(
    "forest-buffalo",
    "The same animal at two thirds the size and a different colour, with the "
    "horns sweeping BACK rather than out -- `horn_spread` 0.45 against the "
    "Cape buffalo's 1.60, which is the single row that separates them -- and "
    "long fringed ears that are most of the head in silhouette.\n\n"
    "LATTICE: 5 cm. 2.2 m is 44 voxels; the ear fringe, which is the real "
    "field mark, is finer than the lattice and is carried as ear SIZE instead.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.20,
        "quad.shoulder_h": 0.52,
        "quad.hip_h": 0.96,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": -10.0,
        "quad.head_deg": -22.0,
        "quad.depth": 0.46,
        "quad.width": 0.64,
        "quad.chest": 1.08,
        "quad.waist": 0.98,
        "quad.rump": 0.96,
        "quad.belly": 0.54,
        "quad.section": 2.6,
        "quad.neck_thick": 0.86,
        "quad.head_size": 1.08,
        "quad.muzzle_depth": 0.66,
        "quad.muzzle_width": 0.78,
        "quad.jaw": 0.50,
        "quad.ear_shape": "fan",
        "quad.ear_len": 0.075,
        "quad.ear_width": 1.30,
        "quad.ear_deg": 8.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.13,
        "quad.horn_thick": 0.28,
        "quad.horn_spread": 0.45,
        "quad.horn_curl": 0.70,
        "quad.tail_len": 0.30,
        "quad.tail_thick": 0.14,
        "quad.tail_deg": -60.0,
        "quad.tail_tuft": 0.55,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.28,
        "quad.under": 0.0,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_rufous",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "skin_dark",
        "materials.quad_horn": "skin_brown",
        "quad.sex_length": 1.15,
        "herd.cover": "forest",
        "herd.size_min": 3,
        "herd.size_max": 20,
        "herd.spread_m": 50.0,
        "herd.despawn_m": 300.0,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 70.0,
        "biomes.rainforest": 1.0,
        "biomes.savanna": 0.3,
    },
)

add(
    "blue-wildebeest",
    "FRONT-HEAVY AND AWKWARD, which is the species: high humped shoulders "
    "falling away to low hips, a long blunt cow-like head hung below the line "
    "of the back, a black beard and a long black tail. The dark vertical bars "
    "on the forequarter are `quad.mark` BARS with a low count and a "
    "`mark_strength` under 1, so they stay a suggestion on the shoulder rather "
    "than becoming a zebra.\n\n"
    "LATTICE: 2 cm. The bars are the smallest identifying feature at roughly "
    "5 cm on a 2.2 m animal, so three voxels wants 1.7 cm and 2 cm gives 2.5 "
    "-- the same arithmetic that put the tiger and the zebra here. At 5 cm the "
    "bars are one voxel and the animal is a grey cow.",
    **{
        "quad.length_m": 2.20,
        "quad.shoulder_h": 0.64,
        "quad.hip_h": 0.80,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.17,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": -4.0,
        "quad.head_deg": -26.0,
        "quad.depth": 0.42,
        "quad.width": 0.52,
        "quad.chest": 1.10,
        "quad.waist": 0.90,
        "quad.rump": 0.80,
        "quad.belly": 0.50,
        "quad.hump": 0.22,
        "quad.hump_at": 0.14,
        "quad.neck_thick": 0.74,
        "quad.neck_taper": 0.72,
        "quad.mane": 0.30,
        "quad.dewlap": 0.45,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.62,
        "quad.muzzle_width": 0.70,
        "quad.jaw": 0.45,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.06,
        "quad.ear_width": 0.60,
        "quad.ear_deg": 40.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.14,
        "quad.horn_thick": 0.26,
        "quad.horn_spread": 1.15,
        "quad.horn_curl": 0.60,
        "quad.tail_len": 0.42,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.80,
        "quad.tail_deg": -60.0,
        "quad.leg_thick": 0.15,
        "quad.hock": 0.32,
        "quad.under": 0.0,
        "quad.mark": "bars",
        "quad.mark_count": 7,
        "quad.mark_width": 0.14,
        "quad.mark_strength": 0.70,
        "materials.quad_back": "plume_slate",
        "materials.quad_belly": "plume_slate",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "plume_slate",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "skin_dark",
        "materials.quad_horn": "skin_dark",
        "quad.sex_horn": 1.0,
        "quad.sex_length": 1.12,
        "herd.cover": "open",
        "herd.size_min": 8,
        "herd.size_max": 400,
        "herd.spread_m": 300.0,
        "herd.despawn_m": 500.0,
        "herd.per_hectare": 2.5,
        "herd.flee_m": 120.0,
        "biomes.savanna": 1.0,
        "biomes.grassland": 0.4,
    },
)

add(
    "wild-yak",
    "THE HAIR SKIRT IS THE SILHOUETTE, and it is the one thing on this animal "
    "that is not a body proportion: a curtain of hair hanging from the flanks "
    "and legs almost to the ground. There is no skirt parameter, so it is "
    "drawn as the flanks themselves -- `width` 0.86 and `belly` 0.72, the "
    "widest and deepest underside in the file -- with the marking colour taken "
    "well up the flank by `under` inverted through a dark body and a pale "
    "back. It reads as a yak from any distance and it is NOT a skirt: it is a "
    "very wide animal. That is a real departure and it is written here so that "
    "nobody looks for the skirt code.\n\n"
    "LATTICE: 5 cm. 3.0 m is 60 voxels, the largest here after the elephants, "
    "and the horns at 15 cm sweep are 3 voxels.",
    resolution_cm="5",
    **{
        "quad.length_m": 3.00,
        "quad.shoulder_h": 0.60,
        "quad.hip_h": 0.86,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.13,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": -18.0,
        "quad.head_deg": -26.0,
        "quad.depth": 0.50,
        "quad.width": 0.86,
        "quad.chest": 1.10,
        "quad.waist": 1.04,
        "quad.rump": 0.90,
        "quad.belly": 0.72,
        "quad.section": 3.0,
        "quad.hump": 0.30,
        "quad.hump_at": 0.16,
        "quad.neck_thick": 0.94,
        "quad.head_size": 1.10,
        "quad.muzzle_depth": 0.66,
        "quad.muzzle_width": 0.78,
        "quad.jaw": 0.50,
        "quad.mane": 0.24,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.035,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 26.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.16,
        "quad.horn_thick": 0.26,
        "quad.horn_spread": 1.30,
        "quad.horn_curl": 0.55,
        "quad.tail_len": 0.30,
        "quad.tail_thick": 0.30,
        "quad.tail_taper": 0.90,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.26,
        "quad.hock": 0.24,
        "quad.foot": 1.10,
        "quad.under": 0.0,
        "quad.cape": 0.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_grey",
        "materials.quad_horn": "plume_buff",
        "quad.sex_length": 1.25,
        "herd.cover": "open",
        "herd.size_min": 3,
        "herd.size_max": 40,
        "herd.spread_m": 120.0,
        "herd.despawn_m": 450.0,
        "herd.per_hectare": 0.4,
        "herd.flee_m": 150.0,
        "biomes.tundra_alpine": 1.0,
        "biomes.bare_rock": 0.3,
    },
)

add(
    "muskox",
    "A low blocky ox buried in a floor-length coat, with short legs barely "
    "visible under it and a helmet of fused horn across the forehead that "
    "hooks down and then out. The coat and the yak's hair skirt are the same "
    "problem solved the same way -- a very wide, very deep body rather than a "
    "curtain -- but the muskox goes further: `shoulder_h` 0.48 with `belly` "
    "0.76 puts the underside close to the ground and the legs nearly "
    "disappear, which is exactly the read.\n\n"
    "LATTICE: 5 cm. 2.2 m is 44 voxels; the pale saddle boundary is 30-40 cm "
    "and six to eight voxels, and the horn boss is four.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.20,
        "quad.shoulder_h": 0.48,
        "quad.hip_h": 0.92,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": -20.0,
        "quad.head_deg": -22.0,
        "quad.depth": 0.52,
        "quad.width": 0.82,
        "quad.chest": 1.12,
        "quad.waist": 1.06,
        "quad.rump": 1.00,
        "quad.belly": 0.76,
        "quad.section": 3.0,
        "quad.hump": 0.22,
        "quad.hump_at": 0.20,
        "quad.neck_thick": 0.96,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.60,
        "quad.muzzle_width": 0.74,
        "quad.jaw": 0.45,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.03,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 20.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.14,
        "quad.horn_thick": 0.34,
        "quad.horn_spread": 1.40,
        "quad.horn_curl": 0.90,
        "quad.tail_len": 0.04,
        "quad.tail_thick": 0.20,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.22,
        "quad.foot": 1.10,
        "quad.under": 0.0,
        "quad.cape": 0.30,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_buff",
        "materials.quad_horn": "plume_buff",
        "quad.sex_length": 1.20,
        "herd.cover": "open",
        "herd.size_min": 4,
        "herd.size_max": 24,
        "herd.spread_m": 60.0,
        "herd.despawn_m": 400.0,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 90.0,
        "biomes.tundra_alpine": 1.0,
    },
)

add(
    "alpine-ibex",
    "THE SECOND BARE-ROCK SPECIES, and the one the owner's `hosts` change was "
    "really for. A compact goat with heavier forequarters than hind and thick "
    "backswept horns curving in a single arc to two thirds of the body's "
    "length -- the longest headgear relative to body size in the whole file at "
    "`horn_len` 0.62.\n\n"
    "The transverse knobs along the horn are a real field mark and they are "
    "NOT drawn: they are 3-4 cm ridges on a horn that is itself only three "
    "voxels thick, so at 5 cm they would be nothing and at 2 cm they would be "
    "one voxel of noise along an edge. Left out rather than faked.\n\n"
    "LATTICE: 5 cm. 1.5 m is 30 voxels of head-body, which is at the bottom of "
    "the band and would normally send this animal to 2 cm -- but the horns are "
    "1.0 m long, so the ASSET is 50 voxels across and the feature that "
    "identifies it is the largest thing on it. This is the one place in the "
    "file where the body's voxel count is the wrong number to read.",
    resolution_cm="5",
    **{
        "quad.length_m": 1.50,
        "quad.shoulder_h": 0.60,
        "quad.hip_h": 0.94,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.15,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 22.0,
        "quad.head_deg": -12.0,
        "quad.depth": 0.42,
        "quad.width": 0.56,
        "quad.chest": 1.08,
        "quad.waist": 0.94,
        "quad.rump": 0.90,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.68,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.44,
        "quad.jaw": 0.35,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.07,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 40.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.62,
        "quad.horn_thick": 0.13,
        "quad.horn_spread": 0.22,
        "quad.horn_curl": 0.45,
        "quad.tail_len": 0.10,
        "quad.tail_thick": 0.20,
        "quad.tail_deg": -40.0,
        "quad.leg_thick": 0.16,
        "quad.hock": 0.34,
        "quad.under": 0.24,
        "quad.mark": "saddle",
        "quad.mark_width": 0.10,
        "quad.mark_strength": 0.85,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "skin_brown",
        "materials.quad_horn": "skin_brown",
        # Female ibex carry short horns, not none -- which is the middle case
        # `sex_horn` exists for and the only species in this file that uses it.
        "quad.sex_horn": 0.30,
        "quad.sex_length": 1.25,
        "herd.cover": "rock",
        "herd.size_min": 2,
        "herd.size_max": 20,
        "herd.spread_m": 50.0,
        "herd.per_hectare": 0.6,
        "herd.flee_m": 70.0,
        "biomes.bare_rock": 1.0,
        "biomes.tundra_alpine": 0.9,
    },
)

add(
    "chamois",
    "The other rock species, and a deliberately different build from the ibex "
    "beside it: slender and light where the ibex is stocky, with SHORT "
    "vertical horns hooked sharply back only at the very tip. That hook is "
    "`horn_curl` 0.30 on a short SPIKE rather than a CURVE -- the curl runs "
    "out at the end of a mostly straight shaft, which is the shape, and if "
    "curl had been a whole-length arc this species would have needed a sixth "
    "horn setting.\n\n"
    "The white face split by a bold black eye-to-muzzle stripe is the other "
    "field mark and it is not drawn: `quad.mark` puts one marking on the "
    "FLANK, and a face stripe is not a flank. The dark dorsal stripe is what "
    "is authored instead, as a SADDLE.\n\n"
    "LATTICE: 2 cm. `08-tundra-alpine.md` and `09-bare-rock.md` both flag this "
    "row and both are right: the horn hook is 3-4 cm on a 1.2 m animal, two "
    "voxels at 2 cm, and at 5 cm the horn is a straight one-voxel spike and "
    "the animal is a goat.",
    **{
        "quad.length_m": 1.20,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.18,
        "quad.head_frac": 0.15,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 34.0,
        "quad.head_deg": -8.0,
        "quad.depth": 0.38,
        "quad.width": 0.50,
        "quad.chest": 0.98,
        "quad.waist": 0.92,
        "quad.rump": 0.98,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.52,
        "quad.head_size": 0.92,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.10,
        "quad.ear_width": 0.48,
        "quad.ear_deg": 60.0,
        "quad.horn_shape": "spike",
        "quad.horn_len": 0.16,
        "quad.horn_thick": 0.16,
        "quad.horn_spread": 0.16,
        "quad.horn_curl": 0.30,
        "quad.tail_len": 0.08,
        "quad.tail_thick": 0.18,
        "quad.tail_deg": -46.0,
        "quad.leg_thick": 0.13,
        "quad.hock": 0.38,
        "quad.under": 0.26,
        "quad.mark": "saddle",
        "quad.mark_width": 0.08,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_white",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "skin_dark",
        "materials.quad_horn": "skin_dark",
        "quad.sex_horn": 1.0,
        "herd.cover": "rock",
        "herd.size_min": 2,
        "herd.size_max": 16,
        "herd.spread_m": 45.0,
        "herd.per_hectare": 0.7,
        "herd.flee_m": 90.0,
        "biomes.bare_rock": 1.0,
        "biomes.tundra_alpine": 0.9,
    },
)

add(
    "bighorn-sheep",
    "THE ONLY HEADGEAR IN THE LIBRARY THAT CURLS A FULL CIRCLE, and the "
    "reason `horn_curl` runs all the way to 1.0. A ram's horns are massive "
    "enough to read as a solid block beside the head rather than as a pair of "
    "lines, which is the one case where a coarse lattice HELPS: at 5 cm a horn "
    "26 cm thick at the base is five voxels and needs no authoring-up at all. "
    "That is the opposite of the red deer, and the two beside each other are "
    "the clearest statement of what the three-voxel rule actually costs.\n\n"
    "LATTICE: 5 cm. 1.6 m is 32 voxels of head-body -- low, and accepted for "
    "the same reason as the ibex: the horns are the asset and they are huge.",
    resolution_cm="5",
    **{
        "quad.length_m": 1.60,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 0.94,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.15,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 24.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.44,
        "quad.width": 0.58,
        "quad.chest": 1.08,
        "quad.waist": 0.96,
        "quad.rump": 0.94,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.74,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.48,
        "quad.jaw": 0.35,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.055,
        "quad.ear_width": 0.60,
        "quad.ear_deg": 36.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.36,
        "quad.horn_thick": 0.42,
        "quad.horn_spread": 0.55,
        "quad.horn_curl": 1.00,
        "quad.tail_len": 0.06,
        "quad.tail_thick": 0.20,
        "quad.tail_deg": -44.0,
        "quad.leg_thick": 0.17,
        "quad.hock": 0.32,
        "quad.under": 0.22,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "plume_white",
        "materials.quad_horn": "plume_buff",
        "quad.sex_horn": 0.40,
        "quad.sex_length": 1.20,
        "herd.cover": "rock",
        "herd.size_min": 3,
        "herd.size_max": 24,
        "herd.spread_m": 60.0,
        "herd.per_hectare": 0.6,
        "herd.flee_m": 90.0,
        "biomes.bare_rock": 0.9,
        "biomes.tundra_alpine": 0.9,
        "biomes.desert": 0.2,
    },
)

add(
    "barbary-sheep",
    "THE THROAT FRINGE IS THE SILHOUETTE -- a long curtain of hair hanging "
    "from the throat and down between the forelegs to the knee. It is drawn as "
    "`quad.dewlap` at 1.05, the highest in the file, which is the same "
    "mechanism the moose's bell uses taken nearly to its ceiling. The horns "
    "sweep out and back in a wide arc.\n\n"
    "LATTICE: 2 cm, and the two biome files disagree with each other about "
    "this row -- `05-desert.md` says 2 cm and `09-bare-rock.md` says 5. 2 cm "
    "wins because the fringe is what identifies the animal and its lower edge "
    "is a 5-8 cm boundary on a 1.4 m body: three voxels wants 1.7 cm. At 5 cm "
    "the fringe merges into the chest and the animal is a plain tan sheep.",
    **{
        "quad.length_m": 1.45,
        "quad.shoulder_h": 0.64,
        "quad.hip_h": 0.92,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.15,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 24.0,
        "quad.head_deg": -12.0,
        "quad.depth": 0.44,
        "quad.width": 0.56,
        "quad.chest": 1.10,
        "quad.waist": 0.94,
        "quad.rump": 0.92,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.72,
        "quad.dewlap": 1.05,
        "quad.head_size": 0.98,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.46,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.06,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 38.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.34,
        "quad.horn_thick": 0.22,
        "quad.horn_spread": 0.85,
        "quad.horn_curl": 0.70,
        "quad.tail_len": 0.14,
        "quad.tail_thick": 0.18,
        "quad.tail_taper": 0.60,
        "quad.tail_deg": -42.0,
        "quad.leg_thick": 0.17,
        "quad.hock": 0.32,
        "quad.under": 0.20,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "materials.quad_horn": "skin_brown",
        "quad.sex_horn": 0.55,
        "quad.sex_length": 1.20,
        "herd.cover": "rock",
        "herd.size_min": 2,
        "herd.size_max": 20,
        "herd.spread_m": 55.0,
        "herd.per_hectare": 0.6,
        "herd.flee_m": 90.0,
        "biomes.desert": 0.9,
        "biomes.bare_rock": 0.8,
        "biomes.savanna": 0.2,
    },
)

add(
    "mountain-goat",
    "White, shaggy, humped at the shoulder, with a long chin beard and short "
    "black horns -- and the only species in the file whose ONLY dark points "
    "are its horns, hooves and muzzle. That makes it the hardest palette in "
    "the file to keep readable: a white animal against white rock with three "
    "small black marks on it. The marks are `materials.quad_mark` at "
    "`skin_dark` against a `plume_white` body, which measures far above the "
    "readability floor and is why the choice is safe.\n\n"
    "THE EARS WERE ENLARGED BY MEASUREMENT. At `ear_len` 0.065 -- roughly life size, 10 cm on a 1.5 m goat, two voxels at 5 cm -- `tools/quadprobe.py --parts` reported ear-R attached at a CORNER ONLY. 0.09 by 0.72 gives a joint. The ears are therefore bigger than life, which on this species costs nothing: nobody identifies a mountain goat by its ears.\n\n"
    "LATTICE: 5 cm. 1.5 m is 30 voxels; the beard at 20 cm is four and the "
    "shoulder hump is the largest thing on the animal.",
    resolution_cm="5",
    **{
        "quad.length_m": 1.50,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 0.88,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.15,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 14.0,
        "quad.head_deg": -18.0,
        "quad.depth": 0.46,
        "quad.width": 0.62,
        "quad.chest": 1.10,
        "quad.waist": 0.96,
        "quad.rump": 0.88,
        "quad.belly": 0.56,
        "quad.section": 2.6,
        "quad.hump": 0.26,
        "quad.hump_at": 0.16,
        "quad.neck_thick": 0.76,
        "quad.dewlap": 0.55,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.44,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.09,
        "quad.ear_width": 0.72,
        "quad.ear_deg": 44.0,
        "quad.horn_shape": "spike",
        "quad.horn_len": 0.14,
        "quad.horn_thick": 0.20,
        "quad.horn_spread": 0.22,
        "quad.horn_curl": 0.35,
        "quad.tail_len": 0.08,
        "quad.tail_thick": 0.22,
        "quad.tail_deg": -40.0,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.30,
        "quad.under": 0.0,
        "quad.stocking": 0.10,
        "materials.quad_back": "plume_white",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_white",
        "materials.quad_leg": "plume_white",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "skin_dark",
        "materials.quad_horn": "skin_dark",
        "quad.sex_horn": 0.85,
        "quad.sex_length": 1.15,
        "herd.cover": "rock",
        "herd.size_min": 1,
        "herd.size_max": 10,
        "herd.spread_m": 45.0,
        "herd.per_hectare": 0.4,
        "herd.flee_m": 80.0,
        "biomes.bare_rock": 1.0,
        "biomes.tundra_alpine": 0.8,
    },
)

add(
    "addax",
    "The SPIRAL horn again, and the reason it is worth a second species after "
    "the kudu: an addax's spiral is TIGHT and near-vertical where a kudu's is "
    "open and swept back, and the two together are what shows `horn_curl` and "
    "`horn_spread` doing different jobs on the same shape. Pale near-white "
    "body, chestnut shoulder patch, dark facial X.\n\n"
    "The facial X is not drawn, for the reason every face mark in this file is "
    "not drawn: `quad.mark` puts one marking on the flank. The shoulder patch "
    "is authored instead, as a CAPE.\n\n"
    "LATTICE: 2 cm. `05-desert.md` flags this row and the horn is why -- 5 cm "
    "thick at the base on a 1.6 m animal, which is 2.5 voxels at 2 cm and one "
    "at 5. Both sexes carry horns, so `sex_horn` stays 1.0.",
    **{
        "quad.length_m": 1.60,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 0.96,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.19,
        "quad.head_frac": 0.14,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 32.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.42,
        "quad.width": 0.56,
        "quad.chest": 1.04,
        "quad.waist": 0.98,
        "quad.rump": 0.98,
        "quad.belly": 0.52,
        "quad.section": 2.6,
        "quad.neck_thick": 0.60,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.46,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.58,
        "quad.ear_deg": 52.0,
        "quad.horn_shape": "spiral",
        "quad.horn_len": 0.50,
        "quad.horn_thick": 0.13,
        "quad.horn_spread": 0.20,
        "quad.horn_curl": 0.85,
        "quad.tail_len": 0.20,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.60,
        "quad.tail_deg": -56.0,
        "quad.tail_tuft": 0.40,
        "quad.leg_thick": 0.16,
        "quad.hock": 0.30,
        "quad.under": 0.0,
        "quad.cape": 0.22,
        "materials.quad_back": "plume_white",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_white",
        "materials.quad_leg": "plume_white",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "skin_brown",
        "materials.quad_horn": "skin_dark",
        "quad.sex_horn": 1.0,
        "herd.cover": "open",
        "herd.size_min": 2,
        "herd.size_max": 15,
        "herd.spread_m": 70.0,
        "herd.per_hectare": 0.3,
        "herd.flee_m": 130.0,
        "biomes.desert": 1.0,
        "biomes.savanna": 0.2,
    },
)

add(
    "arabian-oryx",
    "The gemsbok's smaller white cousin, and the species `quad.stocking` was "
    "written for: bright white body with BLACK LEGS, and the boundary between "
    "them measured from the ground up rather than from each joint down -- "
    "which matters here more than anywhere, because at `stocking` 0.42 on a "
    "fore leg and a hind leg of different lengths, a per-limb fraction would "
    "put the black at two heights and the animal would look as though it were "
    "standing in a hole.\n\n"
    "LATTICE: 2 cm. The facial mask and the leg boundary are both 5-7 cm on a "
    "1.6 m animal, the same arithmetic as the gemsbok.",
    **{
        "quad.length_m": 1.60,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.21,
        "quad.head_frac": 0.14,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 34.0,
        "quad.head_deg": -12.0,
        "quad.depth": 0.42,
        "quad.width": 0.56,
        "quad.chest": 1.02,
        "quad.waist": 0.96,
        "quad.rump": 0.98,
        "quad.belly": 0.50,
        "quad.section": 2.6,
        "quad.neck_thick": 0.58,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.54,
        "quad.muzzle_width": 0.46,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.58,
        "quad.ear_deg": 56.0,
        "quad.horn_shape": "sweep",
        "quad.horn_len": 0.40,
        "quad.horn_thick": 0.09,
        "quad.horn_spread": 0.14,
        "quad.horn_curl": 0.35,
        "quad.tail_len": 0.24,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.65,
        "quad.tail_deg": -60.0,
        "quad.tail_tip": 0.50,
        "quad.leg_thick": 0.15,
        "quad.hock": 0.32,
        "quad.under": 0.0,
        "quad.stocking": 0.42,
        "materials.quad_back": "plume_white",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_white",
        "materials.quad_leg": "plume_white",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "skin_dark",
        "materials.quad_horn": "skin_dark",
        "quad.sex_horn": 1.0,
        "herd.cover": "open",
        "herd.size_min": 2,
        "herd.size_max": 14,
        "herd.spread_m": 70.0,
        "herd.per_hectare": 0.3,
        "herd.flee_m": 130.0,
        "biomes.desert": 1.0,
        "biomes.savanna": 0.2,
    },
)

add(
    "bongo",
    "A deep chestnut forest antelope, heavy in the body and short in the leg, "
    "with ten to fourteen thin white vertical stripes and open horns swept "
    "back. It is the third BARS species after the tiger and the zebra and the "
    "hardest of the three, because its stripes are the narrowest: the count is "
    "held at 12 with a low `mark_width`, and `docs/biomes/README.md` §6 warns "
    "that a band under two voxels with two voxels of gap merges into a wash.\n\n"
    "LATTICE: 2 cm. A stripe is 3-4 cm on a 2.2 m animal, which strictly wants "
    "1 cm and a 220-voxel body. Held at 2 cm with the stripes authored WIDER "
    "THAN LIFE so they survive -- the class (b) fix, exactly as "
    "`greater-kudu` records it, and written here so nobody narrows them back.",
    **{
        "quad.length_m": 2.20,
        "quad.shoulder_h": 0.55,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.19,
        "quad.head_frac": 0.14,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 32.0,
        "quad.head_deg": -12.0,
        "quad.depth": 0.44,
        "quad.width": 0.56,
        "quad.chest": 1.06,
        "quad.waist": 1.00,
        "quad.rump": 1.04,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.62,
        "quad.mane": 0.20,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.46,
        "quad.ear_shape": "fan",
        "quad.ear_len": 0.075,
        "quad.ear_width": 1.00,
        "quad.ear_deg": 30.0,
        "quad.horn_shape": "spiral",
        "quad.horn_len": 0.30,
        "quad.horn_thick": 0.16,
        "quad.horn_spread": 0.35,
        "quad.horn_curl": 0.35,
        "quad.tail_len": 0.26,
        "quad.tail_thick": 0.16,
        "quad.tail_taper": 0.60,
        "quad.tail_deg": -55.0,
        "quad.tail_tuft": 0.45,
        "quad.leg_thick": 0.16,
        "quad.hock": 0.32,
        "quad.under": 0.16,
        "quad.stocking": 0.18,
        "quad.mark": "bars",
        "quad.mark_count": 12,
        "quad.mark_width": 0.13,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "plume_white",
        "materials.quad_horn": "skin_dark",
        "quad.sex_horn": 1.0,
        "quad.sex_length": 1.15,
        "herd.cover": "forest",
        "herd.size_min": 2,
        "herd.size_max": 10,
        "herd.spread_m": 35.0,
        "herd.per_hectare": 0.3,
        "herd.flee_m": 110.0,
        "biomes.rainforest": 1.0,
    },
)

add(
    "okapi",
    "A GIRAFFE AT HALF SCALE AND WITH A COAT THE GENERATOR CAN ACTUALLY DRAW, "
    "which is why it is here and the reticulated giraffe is not: an okapi's "
    "white stripes are transverse BANDS on the rump and upper legs, and a "
    "band wrapping a cylinder is precisely `quad.mark` BARS. A giraffe's "
    "reticulation is a partition into plates and is no setting of anything "
    "this generator has.\n\n"
    "It is also the only species here where the marking has to STOP -- the "
    "stripes are on the hindquarters and legs only, not the flank. `mark` "
    "covers the flank, so what is authored is a low count over the rear half "
    "with `stocking` carrying the leg banding, and the forequarter left plain "
    "dark. That is an approximation and it is the honest read at 2 cm.\n\n"
    "LATTICE: 2 cm. A stripe is 4-6 cm on a 2.1 m animal: 2-3 voxels at 2 cm.",
    **{
        "quad.length_m": 2.10,
        "quad.shoulder_h": 0.72,
        # Forelegs longer than hind, so the back slopes down to the rump --
        # the giraffe proportion, at half scale.
        "quad.hip_h": 0.86,
        "quad.trunk_frac": 0.48,
        "quad.neck_frac": 0.26,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 52.0,
        "quad.head_deg": -14.0,
        "quad.depth": 0.40,
        "quad.width": 0.52,
        "quad.chest": 1.02,
        "quad.waist": 0.94,
        "quad.rump": 0.96,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.56,
        "quad.neck_taper": 0.62,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "fan",
        "quad.ear_len": 0.09,
        "quad.ear_width": 1.05,
        "quad.ear_deg": 44.0,
        "quad.horn_shape": "spike",
        "quad.horn_len": 0.06,
        "quad.horn_thick": 0.30,
        "quad.horn_spread": 0.30,
        "quad.tail_len": 0.24,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.55,
        "quad.tail_deg": -50.0,
        "quad.tail_tuft": 0.50,
        "quad.leg_thick": 0.15,
        "quad.hock": 0.30,
        "quad.under": 0.0,
        "quad.stocking": 0.40,
        "quad.mark": "bars",
        "quad.mark_count": 6,
        "quad.mark_width": 0.16,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_white",
        "materials.quad_horn": "skin_dark",
        "quad.sex_horn": 0.0,
        "herd.cover": "forest",
        "herd.size_max": 2,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 90.0,
        "biomes.rainforest": 1.0,
    },
)

add(
    "vicuna",
    "THE HONEST WILD CAMELID, and `02-grassland.md` and `08-tundra-alpine.md` "
    "both say so about the alpaca that is already shipped: the alpaca is "
    "domesticated and belongs beside a settlement, and this is the animal that "
    "belongs on an empty hillside. Slight, fine-legged, long-necked, cinnamon "
    "above and white below, with a bib of long white chest hair.\n\n"
    "The bib is drawn as `quad.dewlap` rather than as a marking, because a "
    "hanging mass of hair on the chest is geometry and a colour field on the "
    "chest is not -- and the dewlap then takes the underside colour, which is "
    "the white, for free.\n\n"
    "LATTICE: 2 cm, the same as the alpaca and for the same reason its notes "
    "record: the ears are 5 cm across on a 1.5 m animal, so three voxels wants "
    "1.7 cm, and 5 cm would put the whole animal on 30 voxels with the ears "
    "one voxel wide.",
    **{
        "quad.length_m": 1.50,
        "quad.shoulder_h": 0.60,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.46,
        "quad.neck_frac": 0.32,
        "quad.head_frac": 0.11,
        "quad.muzzle_frac": 0.11,
        "quad.neck_deg": 74.0,
        "quad.head_deg": -12.0,
        "quad.depth": 0.40,
        "quad.width": 0.54,
        "quad.chest": 0.94,
        "quad.waist": 0.96,
        "quad.rump": 0.98,
        "quad.belly": 0.48,
        "quad.section": 2.5,
        "quad.neck_thick": 0.38,
        "quad.neck_taper": 0.58,
        "quad.dewlap": 0.70,
        "quad.head_size": 0.85,
        "quad.muzzle_depth": 0.48,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.085,
        "quad.ear_width": 0.40,
        "quad.ear_deg": 84.0,
        "quad.tail_len": 0.12,
        "quad.tail_thick": 0.20,
        "quad.tail_taper": 0.55,
        "quad.tail_deg": -34.0,
        "quad.leg_thick": 0.13,
        "quad.hock": 0.30,
        "quad.under": 0.36,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "open",
        "herd.size_min": 4,
        "herd.size_max": 20,
        "herd.spread_m": 60.0,
        "herd.per_hectare": 0.8,
        "herd.flee_m": 120.0,
        "biomes.tundra_alpine": 1.0,
        "biomes.grassland": 0.3,
        "biomes.bare_rock": 0.2,
    },
)

add(
    "guanaco",
    "The other honest wild camelid: heavier than the vicuna, tawny with a grey "
    "head, a hard line to white underparts and NO chest bib. Against the "
    "vicuna beside it the whole difference is size, the head colour and one "
    "field turned off, which is what the alpaca's notes said a two-line change "
    "from that spec would look like -- so this is that change, made.\n\n"
    "LATTICE: 5 cm. 1.9 m is 38 voxels, and unlike the vicuna the ears are "
    "proportionally smaller relative to a larger body: 6 cm on 1.9 m is one "
    "voxel at 5 cm, which is the loss this lattice takes. It is taken because "
    "`02-grassland.md` and `08-tundra-alpine.md` both call this row 5 cm and "
    "because a guanaco's read is its neck line, not its ears -- but it is the "
    "weakest lattice call in this file and a render should settle it.",
    resolution_cm="5",
    **{
        "quad.length_m": 1.90,
        "quad.shoulder_h": 0.60,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.48,
        "quad.neck_frac": 0.30,
        "quad.head_frac": 0.11,
        "quad.muzzle_frac": 0.11,
        "quad.neck_deg": 76.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.44,
        "quad.width": 0.60,
        "quad.chest": 1.00,
        "quad.waist": 1.00,
        "quad.rump": 1.00,
        "quad.belly": 0.50,
        "quad.section": 2.5,
        "quad.neck_thick": 0.44,
        "quad.neck_taper": 0.60,
        "quad.head_size": 0.88,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.44,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.42,
        "quad.ear_deg": 82.0,
        "quad.tail_len": 0.12,
        "quad.tail_thick": 0.22,
        "quad.tail_taper": 0.55,
        "quad.tail_deg": -32.0,
        "quad.leg_thick": 0.16,
        "quad.hock": 0.30,
        "quad.under": 0.38,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "open",
        "herd.size_min": 3,
        "herd.size_max": 24,
        "herd.spread_m": 80.0,
        "herd.per_hectare": 0.7,
        "herd.flee_m": 140.0,
        "biomes.grassland": 0.9,
        "biomes.tundra_alpine": 0.9,
        "biomes.desert": 0.3,
    },
)

add(
    "przewalskis-horse",
    "THE ONLY WILD HORSE, and a stockier one than any riding animal: short "
    "legs, a heavy head, and a STIFF UPRIGHT BLACK MANE WITH NO FORELOCK. "
    "That mane is the field mark and it is the same `quad.mane` brush the "
    "plains zebra uses -- which is the point of building the two: a zebra and "
    "a wild horse share a silhouette and are told apart by a coat, and if the "
    "mane parameter only worked at zebra settings this would show it.\n\n"
    "The dark dorsal stripe is authored as a SADDLE at a narrow width; the "
    "faint leg barring is not, because at 5 cm a bar 4 cm wide is under a "
    "voxel and it is a faint mark on a real animal anyway.\n\n"
    "LATTICE: 5 cm. 2.1 m is 42 voxels and the mane is a 10-15 cm brush, two "
    "to three voxels. The zebra sits at 2 cm only because its STRIPES set the "
    "lattice, and this animal has none.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.10,
        "quad.shoulder_h": 0.60,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.20,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 32.0,
        "quad.head_deg": -20.0,
        "quad.depth": 0.44,
        "quad.width": 0.60,
        "quad.chest": 1.04,
        "quad.waist": 1.00,
        "quad.rump": 1.02,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.70,
        "quad.neck_taper": 0.66,
        "quad.mane": 0.36,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.62,
        "quad.muzzle_width": 0.52,
        "quad.jaw": 0.40,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.065,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 76.0,
        "quad.tail_len": 0.34,
        "quad.tail_thick": 0.22,
        "quad.tail_taper": 0.95,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.28,
        "quad.fore_bend": 0.12,
        "quad.under": 0.26,
        "quad.stocking": 0.22,
        "quad.mark": "saddle",
        "quad.mark_width": 0.07,
        "quad.mark_strength": 0.9,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_min": 3,
        "herd.size_max": 20,
        "herd.spread_m": 80.0,
        "herd.despawn_m": 400.0,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 150.0,
        "biomes.grassland": 1.0,
        "biomes.desert": 0.3,
    },
)


# ===========================================================================
# MEGAFAUNA -- and the one part of an elephant this generator does not have
#
# There is no trunk primitive. An elephant's trunk is a long tapering flexible
# rod off the front of the skull, which is a TAIL attached to the wrong end,
# and nothing in `forge/quadruped.py` will put one there. What IS available is
# a very long, very deep, heavily drooped MUZZLE, and that is what the two
# elephants and the tapir below use. It reads as a trunk in silhouette and it
# does not curl. That is a real limitation, it is recorded here once rather
# than three times, and a trunk is the obvious next mechanism if these renders
# are not good enough.
# ===========================================================================

add(
    "african-bush-elephant",
    "THE BIGGEST SILHOUETTE IN THE WORLD and the largest asset in this file: "
    "6 m of animal with enormous fanned ears reaching the shoulder, a high "
    "domed head, a hollow back and pillar legs.\n\n"
    "THE TRUNK IS A DROOPED MUZZLE. There is no trunk primitive (see this "
    "section's header). `muzzle_frac` 0.22 with `muzzle_drop` at the ceiling "
    "gives a heavy tapering mass hanging off the front of the skull, which is "
    "the right silhouette and the wrong articulation -- it cannot curl or "
    "reach. The tusks are `horn_shape` SPIKE with a full curl and a wide "
    "spread, mounted on the head like horns, which is anatomically the wrong "
    "bone and visually right.\n\n"
    "THE EARS ARE WHY `fan` EXISTS. `ear_width` 1.45 makes them wider than "
    "they are long, which no setting of BLADE reaches, and on this species "
    "they are a fifth of the whole animal.\n\n"
    "LATTICE: 5 cm. 6 m is 120 voxels of head-body, the largest in the file, "
    "and every feature on it is a metre across. Nothing here wants finer.",
    resolution_cm="5",
    **{
        "quad.length_m": 6.00,
        "quad.shoulder_h": 0.54,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.53,
        "quad.neck_frac": 0.08,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.22,
        "quad.neck_deg": 10.0,
        "quad.head_deg": -20.0,
        "quad.depth": 0.52,
        "quad.width": 0.72,
        "quad.chest": 1.08,
        "quad.waist": 1.10,
        "quad.rump": 1.06,
        "quad.belly": 0.56,
        "quad.section": 2.8,
        "quad.neck_thick": 1.05,
        "quad.neck_taper": 0.90,
        "quad.head_size": 1.30,
        "quad.muzzle_depth": 0.55,
        "quad.muzzle_width": 0.40,
        "quad.muzzle_drop": 1.00,
        "quad.jaw": 0.20,
        "quad.ear_shape": "fan",
        "quad.ear_len": 0.13,
        "quad.ear_width": 1.45,
        "quad.ear_deg": 18.0,
        "quad.ear_back": 0.35,
        "quad.horn_shape": "spike",
        "quad.horn_len": 0.22,
        "quad.horn_thick": 0.16,
        "quad.horn_spread": 0.30,
        "quad.horn_curl": 0.55,
        "quad.tail_len": 0.22,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.50,
        "quad.tail_deg": -70.0,
        "quad.tail_tuft": 0.60,
        # Pillars. The thickest limbs in the library.
        "quad.leg_thick": 0.45,
        "quad.hock": 0.10,
        "quad.fore_bend": 0.08,
        "quad.foot": 1.30,
        "quad.under": 0.0,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_grey",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_dark",
        "materials.quad_horn": "plume_white",
        "quad.sex_length": 1.15,
        # A herd of elephants is the strongest case in the library for an
        # animal that has to still be there when you turn round.
        "herd.entity_class": "persistent",
        "herd.cover": "open",
        "herd.size_min": 3,
        "herd.size_max": 20,
        "herd.spread_m": 120.0,
        "herd.despawn_m": 800.0,
        "herd.per_hectare": 0.2,
        "herd.flee_m": 60.0,
        "biomes.savanna": 1.0,
        "biomes.grassland": 0.3,
    },
)

add(
    "african-forest-elephant",
    "Half the bush elephant and a rounder one: a level or slightly ARCHED back "
    "where the bush elephant's is hollow, oval ears held flat against the neck "
    "rather than fanned out, and tusks that point straight DOWN instead of "
    "sweeping out. Three fields separate them and all three are visible at "
    "distance, which is the test of whether the parameters are real.\n\n"
    "LATTICE: 5 cm. 3.0 m is 60 voxels; the ears at 60 cm are twelve.",
    resolution_cm="5",
    **{
        "quad.length_m": 3.00,
        "quad.shoulder_h": 0.66,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.53,
        "quad.neck_frac": 0.09,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.20,
        "quad.neck_deg": 8.0,
        "quad.head_deg": -22.0,
        "quad.depth": 0.54,
        "quad.width": 0.74,
        "quad.chest": 1.06,
        "quad.waist": 1.12,
        "quad.rump": 1.04,
        "quad.belly": 0.58,
        "quad.section": 2.9,
        "quad.neck_thick": 1.02,
        "quad.neck_taper": 0.92,
        "quad.head_size": 1.25,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.38,
        "quad.muzzle_drop": 1.00,
        "quad.jaw": 0.20,
        "quad.ear_shape": "fan",
        "quad.ear_len": 0.10,
        "quad.ear_width": 1.15,
        "quad.ear_deg": 30.0,
        "quad.ear_back": 0.55,
        "quad.horn_shape": "spike",
        "quad.horn_len": 0.18,
        "quad.horn_thick": 0.14,
        "quad.horn_spread": 0.14,
        "quad.horn_curl": 0.90,
        "quad.tail_len": 0.24,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.50,
        "quad.tail_deg": -72.0,
        "quad.tail_tuft": 0.60,
        "quad.leg_thick": 0.44,
        "quad.hock": 0.10,
        "quad.fore_bend": 0.08,
        "quad.foot": 1.25,
        "quad.under": 0.0,
        "materials.quad_back": "plume_slate",
        "materials.quad_belly": "plume_slate",
        "materials.quad_head": "plume_slate",
        "materials.quad_leg": "plume_slate",
        "materials.quad_tail": "plume_slate",
        "materials.quad_mark": "plume_grey",
        "materials.quad_horn": "plume_white",
        "quad.sex_length": 1.15,
        "herd.entity_class": "persistent",
        "herd.cover": "forest",
        "herd.size_min": 2,
        "herd.size_max": 10,
        "herd.spread_m": 50.0,
        "herd.despawn_m": 500.0,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 70.0,
        "biomes.rainforest": 1.0,
    },
)

add(
    "white-rhinoceros",
    "A slab on four posts with a pronounced neck hump, a square wide muzzle "
    "carried close to the ground, and TWO horns -- a long one in front and a "
    "short one behind. The generator gives one PAIR of horns, side by side, "
    "and a rhino's are in line front-to-back: this is a real mismatch and the "
    "resolution is to author the pair with `horn_spread` at 0.0, so the two "
    "meet on the midline and read as one long nasal horn. THE SECOND HORN IS "
    "NOT DRAWN. It is the smaller of the two and it is behind the first from "
    "every angle a player sees; a second pair of horns is not available and "
    "faking it would put a horn on each cheek.\n\n"
    "LATTICE: 5 cm. 3.8 m is 76 voxels; the front horn at 60 cm is twelve.",
    resolution_cm="5",
    **{
        "quad.length_m": 3.80,
        "quad.shoulder_h": 0.47,
        "quad.hip_h": 0.96,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": -22.0,
        "quad.head_deg": -30.0,
        "quad.depth": 0.50,
        "quad.width": 0.70,
        "quad.chest": 1.10,
        "quad.waist": 1.08,
        "quad.rump": 1.06,
        "quad.belly": 0.56,
        "quad.section": 3.0,
        "quad.hump": 0.26,
        "quad.hump_at": 0.10,
        "quad.neck_thick": 1.05,
        "quad.neck_taper": 0.95,
        "quad.head_size": 1.15,
        "quad.muzzle_depth": 0.70,
        "quad.muzzle_width": 0.90,
        "quad.jaw": 0.40,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 0.70,
        "quad.ear_deg": 60.0,
        "quad.horn_shape": "spike",
        "quad.horn_len": 0.17,
        "quad.horn_thick": 0.30,
        "quad.horn_spread": 0.0,
        "quad.horn_curl": 0.30,
        "quad.tail_len": 0.18,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.50,
        "quad.tail_deg": -60.0,
        "quad.tail_tuft": 0.45,
        "quad.leg_thick": 0.38,
        "quad.hock": 0.16,
        "quad.fore_bend": 0.10,
        "quad.foot": 1.25,
        "quad.under": 0.0,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_grey",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_dark",
        "materials.quad_horn": "plume_buff",
        "quad.sex_length": 1.12,
        "herd.cover": "open",
        "herd.size_min": 1,
        "herd.size_max": 6,
        "herd.spread_m": 60.0,
        "herd.despawn_m": 500.0,
        "herd.per_hectare": 0.1,
        "herd.flee_m": 80.0,
        "biomes.savanna": 1.0,
        "biomes.grassland": 0.2,
    },
)

add(
    "hippopotamus",
    "A BARREL ON STUMPS, and the extreme of two rows at once: `shoulder_h` at "
    "0.40 is the lowest standing animal in the file, and `head_size` at 1.55 "
    "the largest head relative to its body. A hippo's head really is close to "
    "a third of it, and if `head_size` had topped out at 1.2 this species "
    "would have come out as a pig.\n\n"
    "The eyes and ears set on TOP of the skull are the other field mark and "
    "the generator places both on the sides. Not faked; the head's shape "
    "carries it instead.\n\n"
    "LATTICE: 5 cm. 3.5 m is 70 voxels and nothing on the animal is small.",
    resolution_cm="5",
    **{
        "quad.length_m": 3.50,
        "quad.shoulder_h": 0.40,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.06,
        "quad.head_frac": 0.22,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 0.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.56,
        "quad.width": 0.82,
        "quad.chest": 1.10,
        "quad.waist": 1.16,
        "quad.rump": 1.10,
        "quad.belly": 0.62,
        "quad.section": 3.2,
        "quad.neck_thick": 1.08,
        "quad.neck_taper": 1.00,
        "quad.head_size": 1.55,
        "quad.muzzle_depth": 0.85,
        "quad.muzzle_width": 1.10,
        "quad.jaw": 0.80,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.025,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 70.0,
        "quad.tail_len": 0.14,
        "quad.tail_thick": 0.16,
        "quad.tail_taper": 0.45,
        "quad.tail_deg": -55.0,
        "quad.leg_thick": 0.36,
        "quad.hock": 0.18,
        "quad.fore_bend": 0.12,
        "quad.foot": 1.20,
        "quad.under": 0.22,
        "materials.quad_back": "plume_slate",
        "materials.quad_belly": "skin_pale",
        "materials.quad_head": "plume_slate",
        "materials.quad_leg": "plume_slate",
        "materials.quad_tail": "plume_slate",
        "materials.quad_mark": "skin_dark",
        "quad.sex_length": 1.12,
        "herd.cover": "waterside",
        "herd.size_min": 2,
        "herd.size_max": 20,
        "herd.spread_m": 40.0,
        "herd.despawn_m": 450.0,
        "herd.per_hectare": 0.6,
        "herd.flee_m": 40.0,
        "biomes.savanna": 1.0,
        "biomes.rainforest": 0.4,
        "biomes.grassland": 0.2,
    },
)

add(
    "dromedary-camel",
    "ONE HUMP, AND IT IS THE HUMP PARAMETER'S CEILING TEST. `quad.hump` was "
    "written for a bison's withers -- a muscular rise over the shoulder -- and "
    "a camel's is a fatty mound sitting further back and standing much "
    "higher. `hump` 0.62 at `hump_at` 0.34 is that, and it is the strongest "
    "evidence in the library that the row is a shape and not a bison-shaped "
    "special case.\n\n"
    "LATTICE: 5 cm. 3.0 m is 60 voxels; the hump is 60 cm of relief, twelve "
    "voxels, and the knobbed knees which are the other field mark are 10 cm "
    "and two -- present but not readable, which is accepted.",
    resolution_cm="5",
    **{
        "quad.length_m": 3.00,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.50,
        "quad.neck_frac": 0.27,
        "quad.head_frac": 0.11,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 62.0,
        "quad.head_deg": -16.0,
        "quad.depth": 0.44,
        "quad.width": 0.58,
        "quad.chest": 1.00,
        "quad.waist": 1.02,
        "quad.rump": 0.94,
        "quad.belly": 0.50,
        "quad.section": 2.6,
        "quad.hump": 0.62,
        "quad.hump_at": 0.34,
        "quad.neck_thick": 0.52,
        "quad.neck_taper": 0.60,
        "quad.dewlap": 0.35,
        "quad.head_size": 0.85,
        "quad.muzzle_depth": 0.56,
        "quad.muzzle_width": 0.46,
        "quad.muzzle_drop": 0.30,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.03,
        "quad.ear_width": 0.75,
        "quad.ear_deg": 50.0,
        "quad.tail_len": 0.20,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.50,
        "quad.tail_deg": -55.0,
        "quad.tail_tuft": 0.50,
        "quad.leg_thick": 0.16,
        "quad.hock": 0.28,
        "quad.foot": 1.40,
        "quad.under": 0.18,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "quad.sex_length": 1.12,
        "herd.cover": "open",
        "herd.size_min": 2,
        "herd.size_max": 14,
        "herd.spread_m": 80.0,
        "herd.despawn_m": 450.0,
        "herd.per_hectare": 0.3,
        "herd.flee_m": 70.0,
        "biomes.desert": 1.0,
        "biomes.savanna": 0.2,
    },
)

add(
    "lowland-tapir",
    "A BARREL THAT TAPERS TO A POINT AT THE FRONT, which is the one body "
    "profile in this file that runs the opposite way to every other mammal: "
    "`chest` 0.78 against `rump` 1.14, so the animal is narrow at the "
    "shoulder and wide at the hip. Every other quadruped here is chest-heavy. "
    "That plus an arched back, a short mobile trunk drawn as a drooped muzzle, "
    "and a stiff bristle crest along the neck.\n\n"
    "LATTICE: 5 cm. 2.0 m is 40 voxels; the neck crest at 10 cm is two, which "
    "is the one thing short at this lattice and is accepted because the "
    "animal's read is its wedge profile.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.00,
        "quad.shoulder_h": 0.50,
        "quad.hip_h": 1.06,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.15,
        "quad.muzzle_frac": 0.15,
        "quad.neck_deg": 6.0,
        "quad.head_deg": -18.0,
        "quad.depth": 0.48,
        "quad.width": 0.62,
        "quad.chest": 0.78,
        "quad.waist": 1.02,
        "quad.rump": 1.14,
        "quad.belly": 0.56,
        "quad.section": 2.8,
        "quad.neck_thick": 0.80,
        "quad.neck_taper": 0.72,
        "quad.mane": 0.28,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.58,
        "quad.muzzle_width": 0.42,
        "quad.muzzle_drop": 0.75,
        "quad.jaw": 0.30,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.055,
        "quad.ear_width": 0.70,
        "quad.ear_deg": 62.0,
        "quad.tail_len": 0.05,
        "quad.tail_thick": 0.18,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.26,
        "quad.fore_bend": 0.14,
        "quad.foot": 1.10,
        "quad.under": 0.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_white",
        "herd.cover": "waterside",
        "herd.size_max": 2,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 60.0,
        "biomes.rainforest": 1.0,
    },
)

# ===========================================================================
# CATS -- and the one coat the generator refuses
#
# `quad.mark` has spots, dapple and blotch: one field of blobs at three scales.
# A CHEETAH'S SPOTS ARE SOLID AND ROUND and are exactly that, so a cheetah is
# built below. A LEOPARD'S, A JAGUAR'S AND A SNOW LEOPARD'S ARE ROSETTES -- a
# dark annulus with a tawny centre -- and an annulus is not a blob at any
# scale. All three are left unauthored rather than shipped as spotted cats,
# because a leopard with the wrong coat is worse than no leopard.
# ===========================================================================

add(
    "lion",
    "THE MANE IS THE ANIMAL AND IT IS PRESENT ON ONE SEX. `quad.sex_mane` at "
    "3.0 is the highest in the library -- higher than the red deer stag's "
    "2.20 and the gorilla's 2.50 -- because a lioness has no mane whatever "
    "and a male's thickens the entire front of the silhouette. That is the "
    "same present-or-absent mechanism the stag's antlers use, applied to a "
    "field rather than a part.\n\n"
    "Otherwise plain: a deep-chested cat with a long level back, a tawny coat "
    "with no marking at all -- the only unmarked big cat -- and a black tail "
    "tuft, which is `tail_tuft` with the marking colour.\n\n"
    "LATTICE: 5 cm. 2.0 m is 40 voxels, and this is the rare case where a "
    "large cat does NOT need 2 cm: with no stripes and no spots there is "
    "nothing fine on the animal, and the mane at 30 cm is six voxels. The "
    "tiger sits at 2 cm only because its stripes set the lattice.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.00,
        "quad.shoulder_h": 0.55,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.57,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 10.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.40,
        "quad.width": 0.60,
        "quad.chest": 1.10,
        "quad.waist": 0.92,
        "quad.rump": 0.96,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.78,
        "quad.mane": 0.30,
        "quad.head_size": 1.20,
        "quad.muzzle_depth": 0.70,
        "quad.muzzle_width": 0.80,
        "quad.jaw": 0.50,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.04,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 55.0,
        "quad.tail_len": 0.52,
        "quad.tail_thick": 0.24,
        "quad.tail_taper": 0.60,
        "quad.tail_deg": -34.0,
        "quad.tail_arc": 0.20,
        "quad.tail_tuft": 0.55,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.34,
        "quad.foot": 1.30,
        "quad.under": 0.24,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "quad.sex_mane": 3.0,
        "quad.sex_length": 1.20,
        "herd.cover": "open",
        "herd.size_min": 2,
        "herd.size_max": 12,
        "herd.spread_m": 40.0,
        "herd.despawn_m": 350.0,
        "herd.per_hectare": 0.05,
        "herd.flee_m": 60.0,
        "biomes.savanna": 1.0,
        "biomes.grassland": 0.3,
        "biomes.desert": 0.2,
    },
)

add(
    "cheetah",
    "THE ONE SPOTTED BIG CAT THIS GENERATOR CAN HONESTLY DRAW. A cheetah's "
    "spots are small, solid and round -- not rosettes -- which is exactly "
    "`quad.mark` SPOTS, and it is why this species is here while the leopard, "
    "the jaguar and the snow leopard are not.\n\n"
    "Built like a greyhound and authored like one: the deepest, narrowest "
    "chest in the file (`depth` 0.42 against `width` 0.44), the smallest head "
    "of any big cat, and the longest legs. The black tear line from eye to "
    "mouth is the other field mark and is not drawn -- a face mark is not a "
    "flank mark, which is the same limit five other species in this file "
    "record.\n\n"
    "LATTICE: 1 cm, and `06-savanna.md` says 1 cm for this row for exactly "
    "the reason that turns out to be right. A spot is 2-3 cm on a 1.3 m "
    "animal: three voxels across the smaller of those wants 0.7 cm, so 1 cm "
    "gives 2-3 and 2 cm gives 1. At 2 cm the animal is a plain tan cat. The "
    "cost is a 130-voxel body, which is the largest voxel count in this file "
    "after the elephants -- and it is spent on the only thing that "
    "distinguishes the species.",
    resolution_cm="1",
    **{
        "quad.length_m": 1.30,
        "quad.shoulder_h": 0.65,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 20.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.42,
        "quad.width": 0.44,
        "quad.chest": 1.06,
        "quad.waist": 0.84,
        "quad.rump": 0.94,
        "quad.belly": 0.46,
        "quad.neck_thick": 0.56,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.55,
        "quad.muzzle_width": 0.55,
        "quad.jaw": 0.35,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 62.0,
        "quad.tail_len": 0.58,
        "quad.tail_thick": 0.24,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": -30.0,
        "quad.tail_tip": 0.18,
        "quad.leg_thick": 0.13,
        "quad.hock": 0.34,
        "quad.foot": 1.00,
        "quad.under": 0.28,
        "quad.mark": "spots",
        "quad.mark_count": 22,
        "quad.mark_width": 0.07,
        "quad.mark_strength": 0.9,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 3,
        "herd.despawn_m": 300.0,
        "herd.per_hectare": 0.03,
        "herd.flee_m": 90.0,
        "biomes.savanna": 1.0,
        "biomes.grassland": 0.3,
        "biomes.desert": 0.2,
    },
)

add(
    "caracal",
    "THE TUFTS ARE THE SPECIES, and they are longer relative to the ear than "
    "the lynx's: a caracal's black tips stand up almost as far again as the "
    "ear itself. `ear_shape` TUFTED with `ear_len` 0.11 on a 0.75 m animal is "
    "that. Otherwise a plain tawny long-legged cat with a short tail -- no "
    "marking at all, which makes the ears carry the whole read.\n\n"
    "LATTICE: 2 cm, and `05-desert.md` flags this row. The tuft is 5-6 cm on "
    "a 0.75 m animal -- three voxels at 2 cm, exactly the rule. At 5 cm it is "
    "one voxel and the animal is a small lion.",
    **{
        "quad.length_m": 0.75,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.57,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 14.0,
        "quad.depth": 0.34,
        "quad.width": 0.56,
        "quad.chest": 1.00,
        "quad.waist": 0.90,
        "quad.rump": 1.00,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.58,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.60,
        "quad.muzzle_width": 0.62,
        "quad.jaw": 0.35,
        "quad.ear_shape": "tufted",
        "quad.ear_len": 0.11,
        "quad.ear_width": 0.42,
        "quad.ear_deg": 84.0,
        "quad.tail_len": 0.34,
        "quad.tail_thick": 0.26,
        "quad.tail_taper": 0.75,
        "quad.tail_deg": -26.0,
        "quad.leg_thick": 0.19,
        "quad.hock": 0.36,
        "quad.foot": 1.20,
        "quad.under": 0.30,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "edge",
        "herd.size_max": 1,
        "herd.per_hectare": 0.03,
        "herd.flee_m": 90.0,
        "biomes.desert": 0.9,
        "biomes.savanna": 0.6,
        "biomes.grassland": 0.2,
    },
)

add(
    "bobcat",
    "A lynx at three quarters scale with the drama taken out: shorter tufts, a "
    "BARRED tail rather than a plain black-tipped one, and a spotted coat. "
    "Beside `eurasian-lynx` on a sheet it is the test of whether a small "
    "change in `ear_len` and one extra marking make a different species or the "
    "same one twice.\n\n"
    "LATTICE: 1 cm, against the lynx's 2. `03-temperate-forest.md` flags this "
    "row and the tail bars are why: they are 2-3 cm on a 0.8 m animal, so "
    "three voxels wants 0.8 cm. At 2 cm a bar is one voxel and the tail is a "
    "grey stump, which is the lynx. The cost is 80 voxels of body.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.80,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 1.04,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.11,
        "quad.neck_deg": 12.0,
        "quad.depth": 0.34,
        "quad.width": 0.58,
        "quad.chest": 0.98,
        "quad.waist": 0.90,
        "quad.rump": 1.00,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.60,
        "quad.head_size": 1.10,
        "quad.muzzle_depth": 0.66,
        "quad.muzzle_width": 0.68,
        "quad.jaw": 0.38,
        "quad.ear_shape": "tufted",
        "quad.ear_len": 0.055,
        "quad.ear_width": 0.60,
        "quad.ear_deg": 80.0,
        "quad.tail_len": 0.20,
        "quad.tail_thick": 0.30,
        "quad.tail_taper": 0.85,
        "quad.tail_deg": -22.0,
        "quad.tail_tip": 0.30,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.38,
        "quad.foot": 1.20,
        "quad.under": 0.32,
        "quad.mark": "spots",
        "quad.mark_count": 18,
        "quad.mark_width": 0.08,
        "quad.mark_strength": 0.65,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "forest",
        "herd.size_max": 1,
        "herd.per_hectare": 0.04,
        "herd.flee_m": 90.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.3,
        "biomes.desert": 0.2,
    },
)

add(
    "european-wildcat",
    "THE TAIL IS THE ONLY RELIABLE SEPARATOR from a domestic cat, and both "
    "biome files that list this species say so: blunt, thick, RINGED and "
    "black-tipped, rather than tapering. So the tail is authored heavy "
    "(`tail_thick` 0.44, `tail_taper` 1.00 -- no taper at all) with BARS as "
    "the marking and a black tip on top of them.\n\n"
    "LATTICE: 1 cm. The tail rings are 3 cm on a 0.6 m animal, so three voxels "
    "wants 1 cm exactly; 2 cm gives 1.5 and the rings merge into a grey wash. "
    "Both `02-grassland.md` and `03-temperate-forest.md` flag this row and "
    "both call it 1 cm.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.60,
        "quad.shoulder_h": 0.58,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.57,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 14.0,
        "quad.depth": 0.34,
        "quad.width": 0.58,
        "quad.chest": 1.00,
        "quad.waist": 0.92,
        "quad.rump": 0.98,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.58,
        "quad.head_size": 1.10,
        "quad.muzzle_depth": 0.60,
        "quad.muzzle_width": 0.62,
        "quad.jaw": 0.35,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.62,
        "quad.ear_deg": 74.0,
        "quad.tail_len": 0.55,
        "quad.tail_thick": 0.44,
        "quad.tail_taper": 1.00,
        "quad.tail_deg": -24.0,
        "quad.tail_tip": 0.20,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.40,
        "quad.foot": 1.00,
        "quad.under": 0.30,
        "quad.mark": "bars",
        "quad.mark_count": 11,
        "quad.mark_width": 0.12,
        "quad.mark_strength": 0.7,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "forest",
        "herd.size_max": 1,
        "herd.despawn_m": 150.0,
        "herd.per_hectare": 0.06,
        "herd.flee_m": 70.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.5,
        "biomes.taiga": 0.3,
    },
)

add(
    "sand-cat",
    "A HEAD WIDER THAN THE SHOULDERS, which no other cat in the library has "
    "and which is what `ear_deg` at 22 is for: the ears are set very low and "
    "very wide rather than upright, so the skull reads as a flat triangle. "
    "Pale sandy, faint dark leg bars, a short black-tipped tail.\n\n"
    "LATTICE: 1 cm. The leg bars are 2 cm on a 0.5 m animal and would want "
    "0.7 cm, so at 1 cm they are two voxels and are drawn faint (`stocking` "
    "carries them rather than a marking, and `mark_strength` is not used "
    "here). The EARS are what genuinely decides the lattice: 5 cm across on a "
    "0.5 m animal wants 1.7 cm, but at 2 cm the body would be 25 voxels, "
    "which is under the alpaca trap. 1 cm gives 50 and holds both.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.50,
        "quad.shoulder_h": 0.56,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.57,
        "quad.neck_frac": 0.13,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 10.0,
        "quad.depth": 0.32,
        "quad.width": 0.60,
        "quad.chest": 0.98,
        "quad.waist": 0.94,
        "quad.rump": 0.98,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.62,
        "quad.head_size": 1.20,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.70,
        "quad.jaw": 0.30,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.09,
        "quad.ear_width": 0.80,
        # Low and wide-set: the row this species exists to exercise.
        "quad.ear_deg": 22.0,
        "quad.tail_len": 0.40,
        "quad.tail_thick": 0.26,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": -30.0,
        "quad.tail_tip": 0.22,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.38,
        "quad.foot": 1.20,
        "quad.under": 0.34,
        "quad.stocking": 0.20,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "open",
        "herd.size_max": 1,
        "herd.despawn_m": 120.0,
        "herd.per_hectare": 0.04,
        "herd.flee_m": 50.0,
        "biomes.desert": 1.0,
    },
)


# ===========================================================================
# DOGS -- seven of them, and the ears are the whole argument
#
# A wolf, a coyote, a jackal and a dingo are the same animal at four sizes,
# and if this library cannot tell them apart it has one dog and six wasted
# specs. What separates them in the field is EAR SIZE RELATIVE TO HEAD, leg
# length and tail carriage -- three rows -- and the fennec fox at one end and
# the maned wolf at the other are the proof that those three rows have enough
# range in them to matter.
# ===========================================================================

add(
    "coyote",
    "A wolf at two thirds the size with proportionally LARGER ears and a "
    "narrower muzzle, and a tail carried straight down rather than level. "
    "`ear_len` 0.11 against the wolf's 0.075 on an animal two thirds as long "
    "is the whole difference, and it is the field mark.\n\n"
    "LATTICE: 2 cm. The ears are 8 cm on a 0.9 m animal, four voxels; the "
    "grizzled saddle boundary is 5 cm and two and a half.",
    **{
        "quad.length_m": 0.90,
        "quad.shoulder_h": 0.60,
        "quad.hip_h": 0.96,
        "quad.trunk_frac": 0.55,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 14.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.34,
        "quad.width": 0.52,
        "quad.chest": 1.02,
        "quad.waist": 0.86,
        "quad.rump": 0.92,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.54,
        "quad.head_size": 0.98,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.36,
        "quad.jaw": 0.35,
        "quad.mane": 0.10,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.11,
        "quad.ear_width": 0.56,
        "quad.ear_deg": 82.0,
        "quad.tail_len": 0.44,
        "quad.tail_thick": 0.40,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": -56.0,
        "quad.tail_tip": 0.18,
        "quad.leg_thick": 0.17,
        "quad.hock": 0.42,
        "quad.fore_bend": 0.20,
        "quad.under": 0.34,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 3,
        "herd.per_hectare": 0.12,
        "herd.flee_m": 90.0,
        "biomes.grassland": 1.0,
        "biomes.desert": 0.5,
        "biomes.temperate_forest": 0.4,
    },
)

add(
    "corsac-fox",
    "A steppe fox: paler, shorter in the ear and shorter in the leg than a red "
    "fox, with a smaller brush. It is the closest pair in the whole file -- "
    "beside `red-fox` almost every row is within ten per cent -- and it is "
    "here deliberately, because a library that cannot separate two foxes has "
    "learned something it needs to know before it ships forty mammals.\n\n"
    "LATTICE: 2 cm. Same arithmetic as the red fox: ears roughly 4 cm on a "
    "0.55 m animal, two voxels at 2 cm, under the rule and the coarsest tier "
    "that keeps them at all.",
    **{
        "quad.length_m": 0.55,
        "quad.shoulder_h": 0.54,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 12.0,
        "quad.depth": 0.34,
        "quad.width": 0.58,
        "quad.chest": 1.00,
        "quad.waist": 0.90,
        "quad.rump": 0.94,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.52,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.085,
        "quad.ear_width": 0.62,
        "quad.ear_deg": 74.0,
        "quad.tail_len": 0.52,
        "quad.tail_thick": 0.48,
        "quad.tail_taper": 0.78,
        "quad.tail_deg": -26.0,
        "quad.tail_tip": 0.18,
        "quad.leg_thick": 0.19,
        "quad.hock": 0.40,
        "quad.fore_bend": 0.22,
        "quad.under": 0.36,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 3,
        "herd.despawn_m": 150.0,
        "herd.per_hectare": 0.2,
        "herd.flee_m": 60.0,
        "biomes.grassland": 1.0,
        "biomes.desert": 0.5,
    },
)

add(
    "fennec-fox",
    "THE EARS ARE HALF THE ANIMAL'S HEIGHT, and this is the top of "
    "`quad.ear_len`'s useful range on a mammal: 0.30 of head-body, three times "
    "a red fox's. Broad triangular ears set close together on a sharp small "
    "muzzle, cream all over, with a black-tipped brush nearly as long as the "
    "body.\n\n"
    "THE NECK AND HEAD WERE THICKENED BY MEASUREMENT, NOT BY EYE. As first authored -- a sharp small muzzle on a thin neck, which is what the animal looks like -- `tools/quadprobe.py --parts` reported the HEAD attached to the neck at a CORNER ONLY, with no joint. A part touching its parent through one voxel corner is not a joint: it is a part that will come off the moment the rig moves. `neck_thick` 0.72 and `head_size` 1.15 give a real overlap. The fennec now has a slightly heavier neck than life and it has a head that stays on.\n\n"
    "LATTICE: 1 cm, and `05-desert.md` flags this row. The ear is the largest "
    "feature and is not the problem; the MUZZLE is -- a fennec's snout is "
    "3-4 cm on a 0.4 m animal, so three voxels wants 1.1 cm. At 2 cm the face "
    "is two voxels and the animal is an ear on a lump. The body at 1 cm is 40 "
    "voxels, which is comfortable.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.40,
        "quad.shoulder_h": 0.50,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.11,
        "quad.neck_deg": 14.0,
        "quad.depth": 0.32,
        "quad.width": 0.56,
        "quad.chest": 0.96,
        "quad.waist": 0.90,
        "quad.rump": 0.96,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.72,
        "quad.neck_taper": 0.85,
        "quad.head_size": 1.15,
        "quad.muzzle_depth": 0.40,
        "quad.muzzle_width": 0.32,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.30,
        "quad.ear_width": 0.70,
        "quad.ear_deg": 80.0,
        "quad.ear_back": 0.05,
        "quad.tail_len": 0.62,
        "quad.tail_thick": 0.50,
        "quad.tail_taper": 0.82,
        "quad.tail_deg": -24.0,
        "quad.tail_tip": 0.22,
        "quad.leg_thick": 0.17,
        "quad.hock": 0.42,
        "quad.fore_bend": 0.22,
        "quad.under": 0.40,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 4,
        "herd.despawn_m": 100.0,
        "herd.per_hectare": 0.3,
        "herd.flee_m": 30.0,
        "biomes.desert": 1.0,
    },
)

add(
    "arctic-fox",
    "THE OPPOSITE END OF THE SAME ROW: `ear_len` 0.055, the SMALLEST ear of "
    "any dog here, on a short-faced fox with short legs and an enormously "
    "fluffy coat. Beside the fennec fox it is the clearest statement in the "
    "library of what one parameter's range is worth -- the same skeleton, the "
    "same colours, and nobody would call them the same animal.\n\n"
    "The winter and summer coats are two genuinely different silhouettes on "
    "one skeleton, as `08-tundra-alpine.md` says. WINTER is authored -- all "
    "white, with the body fullness up -- because it is the one that reads "
    "against tundra and the one anybody pictures. Summer is `materials` and "
    "two fullness rows away.\n\n"
    "LATTICE: 2 cm. 0.55 m is 27 voxels of head-body, which is low; the ears "
    "at 3 cm are 1.5 voxels and that is the loss. 1 cm was considered and "
    "rejected -- the identifying feature here is the animal's ROUNDNESS, not "
    "any small part of it, so the voxels would buy nothing.",
    **{
        "quad.length_m": 0.55,
        "quad.shoulder_h": 0.52,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.13,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 10.0,
        "quad.depth": 0.42,
        "quad.width": 0.70,
        "quad.chest": 1.06,
        "quad.waist": 1.02,
        "quad.rump": 1.04,
        "quad.belly": 0.56,
        "quad.section": 2.4,
        "quad.neck_thick": 0.72,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.055,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 66.0,
        "quad.tail_len": 0.56,
        "quad.tail_thick": 0.60,
        "quad.tail_taper": 0.92,
        "quad.tail_deg": -20.0,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.36,
        "quad.fore_bend": 0.20,
        "quad.foot": 1.30,
        "quad.under": 0.0,
        "materials.quad_back": "plume_white",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_white",
        "materials.quad_leg": "plume_white",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "plume_grey",
        "herd.cover": "open",
        "herd.size_max": 3,
        "herd.despawn_m": 150.0,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 50.0,
        "biomes.tundra_alpine": 1.0,
        "biomes.taiga": 0.4,
        "biomes.bare_rock": 0.2,
    },
)

add(
    "golden-jackal",
    "Lean, sandy-grey, long in the leg, with a narrow muzzle, upright pointed "
    "ears and a SHORT black-tipped tail carried low -- the short tail is what "
    "separates it from every other dog in this file, all of which carry half "
    "their body length behind them.\n\n"
    "LATTICE: 2 cm. The black tail tip is 6 cm on a 0.85 m animal, three "
    "voxels, exactly the rule.",
    **{
        "quad.length_m": 0.85,
        "quad.shoulder_h": 0.55,
        "quad.hip_h": 0.96,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.15,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 14.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.34,
        "quad.width": 0.52,
        "quad.chest": 1.00,
        "quad.waist": 0.88,
        "quad.rump": 0.92,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.54,
        "quad.head_size": 0.96,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.36,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.095,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 84.0,
        "quad.tail_len": 0.30,
        "quad.tail_thick": 0.40,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": -50.0,
        "quad.tail_tip": 0.30,
        "quad.leg_thick": 0.16,
        "quad.hock": 0.40,
        "quad.fore_bend": 0.20,
        "quad.under": 0.32,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "any",
        "herd.size_max": 4,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 70.0,
        "biomes.desert": 0.9,
        "biomes.savanna": 0.7,
        "biomes.grassland": 0.5,
    },
)

add(
    "dingo",
    "A lean sandy-ginger dog with erect ears and a straight bushy tail carried "
    "low. Plain, which is the point: it is the one dog here with no marking, "
    "no cape, no stockings and no tail tip -- a shape with a single colour on "
    "it -- and it is the control against which the six patterned dogs beside "
    "it are judged.\n\n"
    "LATTICE: 2 cm. 0.9 m is 45 voxels; the ears at 9 cm are four and a half.",
    **{
        "quad.length_m": 0.90,
        "quad.shoulder_h": 0.61,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.15,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 16.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.34,
        "quad.width": 0.54,
        "quad.chest": 1.02,
        "quad.waist": 0.88,
        "quad.rump": 0.94,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.58,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.48,
        "quad.muzzle_width": 0.40,
        "quad.jaw": 0.38,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.10,
        "quad.ear_width": 0.58,
        "quad.ear_deg": 86.0,
        "quad.tail_len": 0.42,
        "quad.tail_thick": 0.42,
        "quad.tail_taper": 0.72,
        "quad.tail_deg": -40.0,
        "quad.leg_thick": 0.17,
        "quad.hock": 0.40,
        "quad.fore_bend": 0.20,
        "quad.under": 0.32,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "any",
        "herd.size_max": 5,
        "herd.per_hectare": 0.1,
        "herd.flee_m": 80.0,
        "biomes.desert": 0.8,
        "biomes.grassland": 0.6,
        "biomes.beach": 0.5,
        "biomes.savanna": 0.4,
    },
)

add(
    "african-wild-dog",
    "ENORMOUS ROUND EARS on a lean long-legged dog, a white-tipped tail, and a "
    "coat of large irregular patches that is different on every individual. "
    "`quad.mark` BLOTCH at a high `mark_count` and a low strength is the "
    "closest this generator gets to that, and it is a real fit rather than a "
    "compromise: a blotch field IS irregular patching, and because the mark "
    "field is seeded from the individual's hash, two dogs from consecutive "
    "seeds genuinely differ. That is this species' whole identity and the "
    "generator gives it for nothing.\n\n"
    "LATTICE: 2 cm. The ears are 12 cm on a 1.1 m animal, six voxels; the "
    "patches are 10-15 cm and five to seven.",
    **{
        "quad.length_m": 1.10,
        "quad.shoulder_h": 0.66,
        "quad.hip_h": 0.96,
        "quad.trunk_frac": 0.55,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 16.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.34,
        "quad.width": 0.50,
        "quad.chest": 1.00,
        "quad.waist": 0.84,
        "quad.rump": 0.90,
        "quad.belly": 0.46,
        "quad.neck_thick": 0.56,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.44,
        "quad.jaw": 0.40,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.11,
        "quad.ear_width": 0.95,
        "quad.ear_deg": 78.0,
        "quad.tail_len": 0.34,
        "quad.tail_thick": 0.42,
        "quad.tail_taper": 0.80,
        "quad.tail_deg": -32.0,
        "quad.tail_tip": 0.35,
        "quad.leg_thick": 0.14,
        "quad.hock": 0.40,
        "quad.fore_bend": 0.18,
        "quad.under": 0.0,
        "quad.mark": "blotch",
        "quad.mark_count": 12,
        "quad.mark_width": 0.26,
        "quad.mark_strength": 0.8,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_min": 4,
        "herd.size_max": 20,
        "herd.spread_m": 50.0,
        "herd.per_hectare": 0.1,
        "herd.flee_m": 90.0,
        "biomes.savanna": 1.0,
        "biomes.grassland": 0.3,
    },
)

add(
    "maned-wolf",
    "ABSURDLY LONG BLACK LEGS UNDER A SMALL FOX-RED BODY, and it is the "
    "highest `shoulder_h` in the whole library at 0.95 -- an animal whose "
    "shoulder stands almost as high as it is long. If the row had topped out "
    "near a deer's 0.65 this species would have been unbuildable, which is why "
    "it is worth having.\n\n"
    "The black dorsal mane is `quad.mane` in the marking colour; the black "
    "legs are `stocking` taken high, and the two together are most of the "
    "animal.\n\n"
    "LATTICE: 2 cm. 1.0 m is 50 voxels; the mane at 8 cm is four and the "
    "stocking boundary at 6 cm is three.",
    **{
        "quad.length_m": 1.00,
        "quad.shoulder_h": 0.95,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 22.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.30,
        "quad.width": 0.46,
        "quad.chest": 0.94,
        "quad.waist": 0.86,
        "quad.rump": 0.90,
        "quad.belly": 0.44,
        "quad.neck_thick": 0.50,
        "quad.mane": 0.26,
        "quad.head_size": 0.96,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.36,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.13,
        "quad.ear_width": 0.58,
        "quad.ear_deg": 84.0,
        "quad.tail_len": 0.36,
        "quad.tail_thick": 0.42,
        "quad.tail_taper": 0.80,
        "quad.tail_deg": -34.0,
        "quad.tail_tip": 0.30,
        "quad.leg_thick": 0.11,
        "quad.hock": 0.32,
        "quad.fore_bend": 0.14,
        "quad.under": 0.24,
        "quad.stocking": 0.46,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 2,
        "herd.per_hectare": 0.05,
        "herd.flee_m": 110.0,
        "biomes.grassland": 1.0,
        "biomes.savanna": 0.4,
    },
)

add(
    "striped-hyena",
    "The second hyena, and the one that shows the sloping back is a SHAPE "
    "rather than a species: the same `hip_h` collapse the spotted hyena uses, "
    "on an animal with vertical BARS instead of blotches and a tall erectile "
    "mane along the whole spine. `quad.mane` at 0.60 is the longest dorsal "
    "crest in the file.\n\n"
    "LATTICE: 2 cm. The flank bars are 4-5 cm on a 1.1 m animal, so three "
    "voxels wants 1.4 cm and 2 cm gives 2.2 -- under the rule, above the "
    "floor, and the alternative is 110 voxels of body at 1 cm.",
    **{
        "quad.length_m": 1.10,
        "quad.shoulder_h": 0.64,
        "quad.hip_h": 0.72,
        "quad.trunk_frac": 0.55,
        "quad.neck_frac": 0.17,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 4.0,
        "quad.head_deg": -12.0,
        "quad.depth": 0.38,
        "quad.width": 0.52,
        "quad.chest": 1.08,
        "quad.waist": 0.88,
        "quad.rump": 0.80,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.72,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.58,
        "quad.muzzle_width": 0.50,
        "quad.jaw": 0.75,
        "quad.mane": 0.60,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.09,
        "quad.ear_width": 0.80,
        "quad.ear_deg": 74.0,
        "quad.tail_len": 0.30,
        "quad.tail_thick": 0.40,
        "quad.tail_taper": 0.80,
        "quad.tail_deg": -38.0,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.40,
        "quad.under": 0.0,
        "quad.mark": "bars",
        "quad.mark_count": 9,
        "quad.mark_width": 0.14,
        "quad.mark_strength": 0.9,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 2,
        "herd.per_hectare": 0.05,
        "herd.flee_m": 90.0,
        "biomes.desert": 0.9,
        "biomes.savanna": 0.6,
        "biomes.grassland": 0.3,
    },
)

add(
    "american-black-bear",
    "THE HUMP AND THE FACE PROFILE ARE THE ONLY SEPARATORS from a brown bear, "
    "and `03-temperate-forest.md` says exactly that. So this spec is the "
    "shipped `brown-bear` with `hump` at 0, `muzzle_drop` at 0 -- a straight "
    "face where the brown bear's is dished -- taller ears, and eighty per cent "
    "of the size. Nothing else differs, on purpose: if the two cannot be told "
    "apart then `quad.hump` and `quad.muzzle_drop` are not carrying what the "
    "brown bear's notes claim they carry.\n\n"
    "LATTICE: 5 cm. 1.6 m is 32 voxels, at the low end; accepted because the "
    "features here are the profile and the bulk and both are tens of "
    "centimetres. The ears at 12 cm are two and a half voxels, which is "
    "better than the brown bear manages at the same lattice and is the one "
    "place this species is ahead of it.",
    resolution_cm="5",
    **{
        "quad.length_m": 1.60,
        "quad.shoulder_h": 0.55,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.11,
        "quad.neck_deg": 4.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.50,
        "quad.width": 0.68,
        "quad.chest": 1.04,
        "quad.waist": 0.98,
        "quad.rump": 0.98,
        "quad.belly": 0.52,
        "quad.section": 2.6,
        "quad.hump": 0.0,
        "quad.neck_thick": 0.80,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.62,
        "quad.muzzle_width": 0.56,
        "quad.muzzle_drop": 0.0,
        "quad.jaw": 0.40,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.075,
        "quad.ear_width": 1.00,
        "quad.ear_deg": 58.0,
        "quad.tail_len": 0.05,
        "quad.tail_thick": 0.16,
        "quad.tail_deg": -40.0,
        "quad.leg_thick": 0.28,
        "quad.hock": 0.30,
        "quad.fore_bend": 0.30,
        "quad.foot": 1.50,
        "quad.under": 0.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_buff",
        "quad.sex_length": 1.20,
        "herd.cover": "forest",
        "herd.size_max": 2,
        "herd.per_hectare": 0.05,
        "herd.flee_m": 70.0,
        "biomes.temperate_forest": 1.0,
        "biomes.taiga": 0.5,
    },
)

add(
    "warthog",
    "A LOW GREY BARREL WITH THE HEAD TOO BIG FOR IT, and a thin tail carried "
    "STRAIGHT UP -- `tail_deg` at 85, the highest carriage in the library, and "
    "the row's own help text names this species for it. Upcurving tusks, "
    "paired facial warts that the lattice cannot hold, and a spiky mane.\n\n"
    "LATTICE: 2 cm. The tusks are 4 cm thick on a 1.3 m animal, two voxels; "
    "the warts are 3 cm and are not drawn. `06-savanna.md` recommends 2 cm and "
    "the tusks are why.",
    **{
        "quad.length_m": 1.30,
        "quad.shoulder_h": 0.54,
        "quad.hip_h": 0.90,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.20,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": -10.0,
        "quad.head_deg": -8.0,
        "quad.depth": 0.46,
        "quad.width": 0.58,
        "quad.chest": 1.10,
        "quad.waist": 0.96,
        "quad.rump": 0.86,
        "quad.belly": 0.54,
        "quad.section": 2.5,
        "quad.hump": 0.14,
        "quad.hump_at": 0.16,
        "quad.neck_thick": 0.90,
        "quad.neck_taper": 0.88,
        "quad.head_size": 1.30,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.62,
        "quad.jaw": 0.40,
        "quad.mane": 0.34,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.055,
        "quad.ear_width": 0.60,
        "quad.ear_deg": 60.0,
        "quad.horn_shape": "spike",
        "quad.horn_len": 0.10,
        "quad.horn_thick": 0.24,
        "quad.horn_spread": 0.45,
        "quad.horn_curl": 0.80,
        # Straight up, and running.
        "quad.tail_len": 0.32,
        "quad.tail_thick": 0.10,
        "quad.tail_taper": 0.40,
        "quad.tail_deg": 85.0,
        "quad.tail_tuft": 0.50,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.34,
        "quad.foot": 0.85,
        "quad.under": 0.0,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_grey",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_dark",
        "materials.quad_horn": "skin_pale",
        "quad.sex_length": 1.15,
        "herd.cover": "open",
        "herd.size_min": 2,
        "herd.size_max": 10,
        "herd.spread_m": 25.0,
        "herd.per_hectare": 0.8,
        "herd.flee_m": 60.0,
        "biomes.savanna": 1.0,
        "biomes.grassland": 0.3,
    },
)

add(
    "red-river-hog",
    "The most brightly coloured pig there is: rust-red with a WHITE CREST "
    "along the spine and long white tufts on the ear tips. The crest is "
    "`quad.mane` painted in the marking colour, which is the same field the "
    "boar uses for its dark bristle -- so the two pigs beside each other show "
    "the mane doing opposite jobs, dark on pale and pale on dark.\n\n"
    "The ear tufts are TUFTED ear shape, which was written for a lynx and "
    "happens to be exactly right here.\n\n"
    "LATTICE: 2 cm. The crest is 6-8 cm on a 1.3 m animal, three voxels; the "
    "white eye-ring, which is the third field mark, is 2 cm and is not drawn.",
    **{
        "quad.length_m": 1.30,
        "quad.shoulder_h": 0.54,
        "quad.hip_h": 0.92,
        "quad.trunk_frac": 0.57,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": -4.0,
        "quad.head_deg": -14.0,
        "quad.depth": 0.44,
        "quad.width": 0.56,
        "quad.chest": 1.08,
        "quad.waist": 0.96,
        "quad.rump": 0.90,
        "quad.belly": 0.54,
        "quad.section": 2.5,
        "quad.neck_thick": 0.84,
        "quad.neck_taper": 0.82,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.54,
        "quad.muzzle_width": 0.48,
        "quad.jaw": 0.40,
        "quad.mane": 0.30,
        "quad.ear_shape": "tufted",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.50,
        "quad.ear_deg": 66.0,
        "quad.tail_len": 0.22,
        "quad.tail_thick": 0.12,
        "quad.tail_taper": 0.55,
        "quad.tail_deg": -50.0,
        "quad.tail_tuft": 0.40,
        "quad.leg_thick": 0.19,
        "quad.hock": 0.34,
        "quad.foot": 0.85,
        "quad.under": 0.0,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_rufous",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "plume_white",
        "quad.sex_length": 1.12,
        "herd.cover": "forest",
        "herd.size_min": 3,
        "herd.size_max": 16,
        "herd.spread_m": 25.0,
        "herd.per_hectare": 0.6,
        "herd.flee_m": 60.0,
        "biomes.rainforest": 1.0,
    },
)


# ===========================================================================
# MUSTELIDS AND THE OTHER LOW CARNIVORES
#
# Ten species built out of one proportion: a body far longer than it is tall,
# on legs far shorter than anything else in the file. `shoulder_h` runs from
# 0.24 to 0.42 through this whole section against a deer's 0.60, and that one
# row is what makes a weasel a weasel.
# ===========================================================================

add(
    "european-badger",
    "VERY LOW, VERY WIDE, WEDGE-SHAPED, and the face stripes are the species "
    "-- which is exactly what this generator cannot draw. `quad.mark` puts one "
    "marking on the FLANK and a badger's black-and-white bands run "
    "front-to-back down the SKULL. Both biome files that list this species say "
    "the face stripes are the whole read, and they are not drawn.\n\n"
    "What is authored instead is the head in the white and the body in the "
    "grey, so the animal is a pale wedge on a grizzled body -- which is the "
    "right read from behind and above, and the wrong one from directly in "
    "front. THIS IS A KNOWN LOSS. A per-axis head marking is the feature that "
    "would fix it, and it would serve the badger, the chamois, the addax, the "
    "cheetah and the pronghorn, all of which record the same gap in this file.\n\n"
    "LATTICE: 2 cm. 0.8 m is 40 voxels; the stripes would be 4 cm and two "
    "voxels, so even with the mechanism this would be marginal.",
    **{
        "quad.length_m": 0.80,
        "quad.shoulder_h": 0.34,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.10,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": -8.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.42,
        "quad.width": 0.78,
        "quad.chest": 1.02,
        "quad.waist": 1.02,
        "quad.rump": 1.06,
        "quad.belly": 0.56,
        "quad.section": 2.7,
        "quad.neck_thick": 0.86,
        "quad.neck_taper": 0.82,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.34,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.03,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 60.0,
        "quad.tail_len": 0.18,
        "quad.tail_thick": 0.30,
        "quad.tail_taper": 0.60,
        "quad.tail_deg": -30.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.30,
        "quad.fore_bend": 0.26,
        "quad.foot": 1.30,
        "quad.under": 0.0,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "plume_white",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "forest",
        "herd.size_max": 3,
        "herd.despawn_m": 140.0,
        "herd.per_hectare": 0.2,
        "herd.flee_m": 40.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.7,
        "biomes.taiga": 0.3,
    },
)

add(
    "wolverine",
    "A SMALL BEAR THAT IS ACTUALLY A WEASEL: broad head, short powerful legs, "
    "huge feet, and a pale band sweeping along each flank from shoulder to "
    "tail. `quad.foot` at 1.9 is the largest in the file relative to limb "
    "thickness and it is a real fact about the animal -- a wolverine's paws "
    "carry it over snow that would drop a wolf.\n\n"
    "The flank band is `quad.mark` FLANKSTRIPE, which was written for a "
    "gemsbok's countershading boundary and turns out to be the right shape "
    "here too: a horizontal band at the flank line is what a wolverine's "
    "sweep is.\n\n"
    "LATTICE: 2 cm. 0.85 m is 42 voxels; the band is 6-8 cm and three to four.",
    **{
        "quad.length_m": 0.85,
        "quad.shoulder_h": 0.48,
        "quad.hip_h": 0.96,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 0.0,
        "quad.head_deg": -8.0,
        "quad.depth": 0.46,
        "quad.width": 0.66,
        "quad.chest": 1.08,
        "quad.waist": 1.00,
        "quad.rump": 1.00,
        "quad.belly": 0.56,
        "quad.section": 2.5,
        "quad.hump": 0.14,
        "quad.hump_at": 0.16,
        "quad.neck_thick": 0.86,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.50,
        "quad.jaw": 0.50,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.03,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 50.0,
        "quad.tail_len": 0.28,
        "quad.tail_thick": 0.52,
        "quad.tail_taper": 0.85,
        "quad.tail_deg": -22.0,
        "quad.leg_thick": 0.26,
        "quad.hock": 0.34,
        "quad.fore_bend": 0.28,
        "quad.foot": 1.90,
        "quad.under": 0.0,
        "quad.mark": "flankstripe",
        "quad.mark_width": 0.16,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "any",
        "herd.size_max": 1,
        "herd.per_hectare": 0.02,
        "herd.flee_m": 80.0,
        "biomes.taiga": 1.0,
        "biomes.tundra_alpine": 0.8,
        "biomes.bare_rock": 0.2,
    },
)

add(
    "pine-marten",
    "A long slender arboreal mustelid with a VERY bushy tail and a cream-"
    "yellow throat bib. The bib is the field mark and it is the one throat "
    "marking in this file that CAN be drawn: it is the underside colour taken "
    "unusually high up the neck through `quad.under`, which reaches the "
    "throat because the neck is part of the underside run. That is a real "
    "match rather than a workaround, and it is why the marten gets its bib "
    "where the badger does not get its face.\n\n"
    "LATTICE: 1 cm. 0.5 m of head-body is 50 voxels and the bib boundary is "
    "3-4 cm, three voxels. At 2 cm the body would be 25 -- under the alpaca "
    "trap -- and the bib one voxel.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.50,
        "quad.shoulder_h": 0.36,
        "quad.hip_h": 1.06,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 16.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.34,
        "quad.width": 0.52,
        "quad.chest": 0.96,
        "quad.waist": 0.96,
        "quad.rump": 1.00,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.62,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.38,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.80,
        "quad.ear_deg": 72.0,
        "quad.tail_len": 0.62,
        "quad.tail_thick": 0.58,
        "quad.tail_taper": 0.90,
        "quad.tail_deg": -14.0,
        "quad.tail_arc": 0.30,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.44,
        "quad.fore_bend": 0.26,
        "quad.under": 0.52,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "forest",
        "herd.size_max": 1,
        "herd.despawn_m": 100.0,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 30.0,
        "biomes.temperate_forest": 1.0,
        "biomes.taiga": 0.7,
    },
)

add(
    "fisher",
    "The marten's larger, darker, heavier-headed relative, with a long "
    "tapering tail and NO bib at all -- `under` at 0, where the marten's is "
    "0.52. Two rows and a size separate them, which is exactly the test worth "
    "running on a family of ten near-identical animals.\n\n"
    "LATTICE: 1 cm. 0.6 m is 60 voxels; nothing on the animal is finer than "
    "its ears at 3 cm, which is three voxels.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.60,
        "quad.shoulder_h": 0.38,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 12.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.36,
        "quad.width": 0.56,
        "quad.chest": 1.02,
        "quad.waist": 0.96,
        "quad.rump": 1.00,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.72,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.48,
        "quad.muzzle_width": 0.44,
        "quad.jaw": 0.45,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 66.0,
        "quad.tail_len": 0.66,
        "quad.tail_thick": 0.46,
        "quad.tail_taper": 0.50,
        "quad.tail_deg": -18.0,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.40,
        "quad.fore_bend": 0.26,
        "quad.under": 0.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_grey",
        "herd.cover": "forest",
        "herd.size_max": 1,
        "herd.despawn_m": 110.0,
        "herd.per_hectare": 0.08,
        "herd.flee_m": 40.0,
        "biomes.temperate_forest": 0.9,
        "biomes.taiga": 0.6,
    },
)

add(
    "stoat",
    "THE BLACK TAIL TIP IS THE SPECIES, and it is the only reliable way to "
    "tell a stoat from a weasel -- it stays black when the rest of the animal "
    "turns white in winter, which is exactly what `quad.tail_tip` was written "
    "for and the row's own help text says so.\n\n"
    "The summer coat is authored, chestnut over cream, because the black tip "
    "against chestnut and the black tip against white are both readable and "
    "the summer animal is the one that appears in more biomes.\n\n"
    "THE LEGS WERE THICKENED AND THE BODY RAISED, BY MEASUREMENT. Authored at the animal's real proportions -- `shoulder_h` 0.26, which is the lowest body in the file -- `tools/quadprobe.py --parts` reported BOTH HIND LEGS attached at a corner only AND the fore joints not forward of the hind ones, which is a rig with the shoulders behind the hips. At 0.32 with thicker limbs and a shallower hind fold it passes. A stoat that is very slightly less flat than life is the price of one that can be animated at all.\n\n"
    "LATTICE: 1 cm. 0.30 m is 30 voxels of head-body, the bottom of the band, "
    "and the tail tip is 3 cm and three voxels. This species is at the limit "
    "of the rule rather than inside it, exactly as `eastern-grey-squirrel` "
    "records for itself, and 2 cm would make the whole animal 15 voxels.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.30,
        "quad.shoulder_h": 0.32,
        "quad.hip_h": 1.06,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 18.0,
        "quad.head_deg": -2.0,
        "quad.depth": 0.28,
        "quad.width": 0.44,
        "quad.chest": 0.94,
        "quad.waist": 0.96,
        "quad.rump": 0.98,
        "quad.belly": 0.46,
        "quad.neck_thick": 0.66,
        "quad.neck_taper": 0.90,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.40,
        "quad.muzzle_width": 0.36,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 60.0,
        "quad.tail_len": 0.42,
        "quad.tail_thick": 0.34,
        "quad.tail_taper": 0.80,
        "quad.tail_deg": -16.0,
        "quad.tail_tip": 0.35,
        "quad.leg_thick": 0.26,
        "quad.hock": 0.32,
        "quad.fore_bend": 0.18,
        "quad.under": 0.44,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "any",
        "herd.size_max": 1,
        "herd.despawn_m": 70.0,
        "herd.per_hectare": 0.4,
        "herd.flee_m": 15.0,
        "biomes.grassland": 0.9,
        "biomes.temperate_forest": 0.9,
        "biomes.taiga": 0.5,
        "biomes.tundra_alpine": 0.3,
    },
)

add(
    "steppe-polecat",
    "A pale straw mustelid with DARK LEGS and a dark face mask -- the legs are "
    "`quad.stocking` taken very high, which on an animal whose legs are a "
    "fifth of its height means the marking colour reaches the belly line and "
    "the read is a pale animal on dark feet.\n\n"
    "The face mask is not drawn, for the reason the badger's stripes are not: "
    "`quad.mark` is a flank marking. This is the fifth species in the file to "
    "record that gap.\n\n"
    "LATTICE: 1 cm. 0.5 m is 50 voxels; the stocking boundary is 3 cm and "
    "three voxels.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.50,
        "quad.shoulder_h": 0.30,
        "quad.hip_h": 1.04,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 12.0,
        "quad.depth": 0.32,
        "quad.width": 0.50,
        "quad.chest": 0.96,
        "quad.waist": 0.96,
        "quad.rump": 0.98,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.70,
        "quad.head_size": 0.92,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.38,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.045,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 58.0,
        "quad.tail_len": 0.34,
        "quad.tail_thick": 0.40,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": -20.0,
        "quad.tail_tip": 0.30,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.38,
        "quad.fore_bend": 0.24,
        "quad.under": 0.30,
        "quad.stocking": 0.55,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 1,
        "herd.despawn_m": 90.0,
        "herd.per_hectare": 0.2,
        "herd.flee_m": 25.0,
        "biomes.grassland": 1.0,
        "biomes.desert": 0.3,
    },
)

add(
    "european-polecat",
    "The steppe polecat's darker forest cousin: a bandit face mask over pale "
    "underfur, which here means the reverse palette -- dark body, pale "
    "underparts showing through. Beside the steppe polecat it is one palette "
    "swap and two proportions, and the pair are the honest answer to whether "
    "two polecats are worth two specs.\n\n"
    "LATTICE: 1 cm. 0.45 m is 45 voxels.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.45,
        "quad.shoulder_h": 0.30,
        "quad.hip_h": 1.04,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 12.0,
        "quad.depth": 0.32,
        "quad.width": 0.50,
        "quad.chest": 0.98,
        "quad.waist": 0.96,
        "quad.rump": 1.00,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.70,
        "quad.head_size": 0.92,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.38,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.045,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 58.0,
        "quad.tail_len": 0.36,
        "quad.tail_thick": 0.42,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": -20.0,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.38,
        "quad.fore_bend": 0.24,
        "quad.under": 0.24,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "forest",
        "herd.size_max": 1,
        "herd.despawn_m": 90.0,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 25.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.4,
    },
)

add(
    "american-mink",
    "Uniform dark chocolate with a small white chin patch, slimmer than a "
    "polecat and glossier. It is the plainest mustelid in the file -- no "
    "mask, no bib, no stockings, no tail tip -- and it is here as the "
    "unmarked control for the six that carry something.\n\n"
    "LATTICE: 1 cm. 0.4 m is 40 voxels; the chin patch at 2 cm is two and is "
    "carried by `under` reaching the throat rather than by a marking.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.40,
        "quad.shoulder_h": 0.30,
        "quad.hip_h": 1.04,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 14.0,
        "quad.depth": 0.30,
        "quad.width": 0.48,
        "quad.chest": 0.96,
        "quad.waist": 0.96,
        "quad.rump": 0.98,
        "quad.belly": 0.46,
        "quad.neck_thick": 0.68,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.36,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.04,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 56.0,
        "quad.tail_len": 0.42,
        "quad.tail_thick": 0.40,
        "quad.tail_taper": 0.65,
        "quad.tail_deg": -18.0,
        "quad.leg_thick": 0.19,
        "quad.hock": 0.38,
        "quad.fore_bend": 0.24,
        "quad.under": 0.16,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_white",
        "herd.cover": "waterside",
        "herd.size_max": 1,
        "herd.despawn_m": 90.0,
        "herd.per_hectare": 0.2,
        "herd.flee_m": 25.0,
        "biomes.temperate_forest": 0.9,
        "biomes.grassland": 0.4,
        "biomes.taiga": 0.3,
    },
)

add(
    "european-otter",
    "A long low sinuous body with a THICK TAPERING TAIL that is a swimming "
    "organ rather than an ornament: `tail_thick` 0.70 with `tail_taper` 0.20, "
    "which is a heavy base running to a point and is the opposite of every "
    "brush in this file. Short legs, a broad flat muzzle, and the whole animal "
    "held close to the ground.\n\n"
    "LATTICE: 2 cm. 0.8 m is 40 voxels; the muzzle at 6 cm is three, which is "
    "the rule met, and a flat broad muzzle is what says otter rather than "
    "mink at any distance.",
    **{
        "quad.length_m": 0.80,
        "quad.shoulder_h": 0.28,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 6.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.34,
        "quad.width": 0.56,
        "quad.chest": 1.00,
        "quad.waist": 1.00,
        "quad.rump": 1.00,
        "quad.belly": 0.50,
        "quad.section": 2.5,
        "quad.neck_thick": 0.86,
        "quad.neck_taper": 0.95,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.36,
        "quad.muzzle_width": 0.72,
        "quad.jaw": 0.25,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.025,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 50.0,
        "quad.tail_len": 0.62,
        "quad.tail_thick": 0.70,
        "quad.tail_taper": 0.20,
        "quad.tail_deg": -10.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.32,
        "quad.fore_bend": 0.22,
        "quad.foot": 1.40,
        "quad.under": 0.36,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "waterside",
        "herd.size_max": 3,
        "herd.despawn_m": 120.0,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 40.0,
        "biomes.temperate_forest": 0.9,
        "biomes.beach": 0.6,
        "biomes.grassland": 0.5,
        "biomes.taiga": 0.4,
    },
)

add(
    "striped-skunk",
    "A WHITE CAP THAT SPLITS INTO TWO STRIPES DOWN THE BACK, over a huge plume "
    "tail. The stripes run FORE-AND-AFT, which is the one direction "
    "`quad.mark` BARS cannot go -- bars wrap the body transversely. So the "
    "stripes are drawn as a SADDLE: a pale field over the back, from the cap "
    "backwards. That gives a black animal with a white back and a white tail, "
    "which is the correct read at any distance and loses the split between "
    "the two stripes at close range.\n\n"
    "A LONGITUDINAL MARKING IS THE FEATURE THAT WOULD FIX IT. It is the same "
    "missing axis the badger's face stripes need, and this is the second "
    "species in the file blocked on it.\n\n"
    "LATTICE: 2 cm. 0.4 m is 20 voxels of head-body, at the very bottom of the "
    "band; the tail at 0.3 m carries the asset up to 35 and the saddle "
    "boundary is 3 cm and one and a half voxels. 1 cm was rejected because the "
    "marking that would justify it is not the one being drawn.",
    **{
        "quad.length_m": 0.40,
        "quad.shoulder_h": 0.34,
        "quad.hip_h": 1.06,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 4.0,
        "quad.head_deg": -8.0,
        "quad.depth": 0.40,
        "quad.width": 0.62,
        "quad.chest": 1.00,
        "quad.waist": 1.00,
        "quad.rump": 1.04,
        "quad.belly": 0.54,
        "quad.neck_thick": 0.80,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.36,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 62.0,
        "quad.tail_len": 0.75,
        "quad.tail_thick": 0.72,
        "quad.tail_taper": 0.95,
        "quad.tail_deg": 26.0,
        "quad.tail_arc": 0.45,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.36,
        "quad.fore_bend": 0.26,
        "quad.under": 0.0,
        "quad.mark": "saddle",
        "quad.mark_width": 0.24,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "plume_white",
        "herd.cover": "edge",
        "herd.size_max": 2,
        "herd.despawn_m": 90.0,
        "herd.per_hectare": 0.25,
        # It does not run. Nothing in this file has a shorter flight distance.
        "herd.flee_m": 10.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.5,
    },
)

add(
    "raccoon",
    "THE MASK AND THE RINGS ARE THE ENTIRE SPECIES, and this generator has "
    "exactly one of them. The ringed tail is `quad.mark` BARS -- transverse "
    "bands wrapping a cylinder, which is what a raccoon's tail is -- and the "
    "eye mask is a face marking, which is not available. Both biome files "
    "listing this species flag it.\n\n"
    "The mask is approximated by painting the HEAD in the dark marking colour "
    "against a grizzled grey body, so the animal reads as grey with a dark "
    "face. That is the right silhouette and the wrong detail: a real raccoon's "
    "mask is a band across the eyes on a pale face. Recorded rather than "
    "hidden.\n\n"
    "LATTICE: 2 cm. The tail rings are 4 cm on a 0.5 m animal, two voxels -- "
    "under the rule. 1 cm would give four and is what `01-beach.md` implies by "
    "flagging the row; it is held at 2 cm because at 1 cm this becomes a "
    "50,000-voxel asset for a very common species, and the rings survive at "
    "two voxels where a 3 cm stripe would not.",
    **{
        "quad.length_m": 0.50,
        "quad.shoulder_h": 0.42,
        "quad.hip_h": 1.06,
        "quad.trunk_frac": 0.57,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 6.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.42,
        "quad.width": 0.62,
        "quad.chest": 1.00,
        "quad.waist": 1.00,
        "quad.rump": 1.06,
        "quad.belly": 0.54,
        "quad.neck_thick": 0.78,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.36,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.06,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 66.0,
        "quad.tail_len": 0.52,
        "quad.tail_thick": 0.56,
        "quad.tail_taper": 0.85,
        "quad.tail_deg": -18.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.40,
        "quad.fore_bend": 0.28,
        "quad.foot": 1.20,
        "quad.under": 0.28,
        "quad.mark": "bars",
        "quad.mark_count": 8,
        "quad.mark_width": 0.18,
        "quad.mark_strength": 0.9,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "skin_pale",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "edge",
        "herd.size_max": 4,
        "herd.despawn_m": 100.0,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 20.0,
        "biomes.temperate_forest": 1.0,
        "biomes.beach": 0.6,
        "biomes.grassland": 0.4,
    },
)

add(
    "coati",
    "THE TAIL IS CARRIED STRAIGHT UP, which is `tail_deg` at 84 on a tail "
    "longer than the body -- a vertical banded flagpole over a low animal. "
    "Nothing else in this file does that; the warthog's raised tail is a third "
    "of the length and a tenth of the thickness. Plus the long flexible "
    "UPTURNED snout, which is `muzzle_drop` at 0 with a long `muzzle_frac` and "
    "a head angled UP -- the only positive `head_deg` in the file.\n\n"
    "LATTICE: 2 cm. 0.6 m is 30 voxels; the tail bands are 5 cm and two and a "
    "half voxels.",
    **{
        "quad.length_m": 0.60,
        "quad.shoulder_h": 0.42,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.55,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.17,
        "quad.neck_deg": 18.0,
        "quad.head_deg": 20.0,
        "quad.depth": 0.36,
        "quad.width": 0.54,
        "quad.chest": 0.98,
        "quad.waist": 0.98,
        "quad.rump": 1.02,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.70,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.36,
        "quad.muzzle_width": 0.28,
        "quad.jaw": 0.20,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 70.0,
        "quad.tail_len": 1.05,
        "quad.tail_thick": 0.34,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": 84.0,
        "quad.tail_arc": -0.10,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.42,
        "quad.fore_bend": 0.26,
        "quad.under": 0.24,
        "quad.mark": "bars",
        "quad.mark_count": 9,
        "quad.mark_width": 0.16,
        "quad.mark_strength": 0.8,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "forest",
        "herd.size_min": 3,
        "herd.size_max": 20,
        "herd.spread_m": 25.0,
        "herd.per_hectare": 1.5,
        "herd.flee_m": 20.0,
        "biomes.rainforest": 0.9,
        "biomes.beach": 0.5,
        "biomes.savanna": 0.3,
    },
)

add(
    "small-indian-mongoose",
    "Very low, very long, short-legged, uniform grizzled brown, with a "
    "tapering tail -- no marking of any kind. `01-beach.md` flags this row and "
    "also flags what it is: INTRODUCED almost everywhere it is common, which "
    "is worth knowing before it is scattered across a world's coastline. Its "
    "biome weights are held low for that reason rather than for any lattice "
    "one.\n\n"
    "LATTICE: 1 cm. 0.35 m is 35 voxels of head-body. At 2 cm it would be 17, "
    "which is below every fish in the library, and there is no feature "
    "argument to fall back on because the animal has no features -- which is "
    "precisely why the body's own size has to set the lattice here.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.35,
        "quad.shoulder_h": 0.26,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.15,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 10.0,
        "quad.depth": 0.30,
        "quad.width": 0.48,
        "quad.chest": 0.96,
        "quad.waist": 0.98,
        "quad.rump": 0.98,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.72,
        "quad.neck_taper": 0.92,
        "quad.head_size": 0.88,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.34,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.035,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 50.0,
        "quad.tail_len": 0.85,
        "quad.tail_thick": 0.36,
        "quad.tail_taper": 0.35,
        "quad.tail_deg": -12.0,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.38,
        "quad.fore_bend": 0.22,
        "quad.under": 0.20,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "edge",
        "herd.size_max": 2,
        "herd.despawn_m": 70.0,
        "herd.per_hectare": 0.4,
        "herd.flee_m": 15.0,
        "biomes.beach": 0.6,
        "biomes.grassland": 0.3,
        "biomes.rainforest": 0.2,
    },
)


# ===========================================================================
# RODENTS AND LAGOMORPHS -- where the twenty-centimetre floor bites
#
# Seven of the eighteen species below are genuinely smaller than the owner's
# 20 cm minimum, and every one of them is authored UP to 0.20 with the
# arithmetic in its own notes. THAT NOTE IS THE POINT. The floor is a
# real-world length and not a lattice: at 1 cm -- the finest tier this project
# has -- a 13 cm lemming is thirteen voxels of body, and thirteen voxels
# cannot carry a face, a leg and a coat pattern at once. Nineteen species in
# the library already do this and each says so; a minnow was nearly
# "corrected" back to 7 cm once, and the note is what stopped it.
# ===========================================================================

add(
    "alpine-marmot",
    "AUTHORED IN THE UPRIGHT POSE, because `08-tundra-alpine.md` says in so "
    "many words that the upright pose is how the animal is recognised. That is "
    "`quad.stance` BIPEDAL on a fat short-legged rodent -- the third species "
    "in the library to use it after the two kangaroos and the meerkat, and the "
    "one furthest from what the stance was written for: no counterweight tail, "
    "no long hind foot, just a barrel sitting on its haunches.\n\n"
    "LATTICE: 1 cm. 0.5 m is 50 voxels. `02-grassland.md` says 1 cm and "
    "`08-tundra-alpine.md` says 2, and 1 cm wins because the ears are 2 cm on "
    "this animal -- at 2 cm they are one voxel and a marmot with no ears is a "
    "loaf of bread.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.50,
        "quad.stance": "bipedal",
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 0.42,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.10,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 76.0,
        "quad.head_deg": 4.0,
        "quad.depth": 0.44,
        "quad.width": 0.72,
        "quad.chest": 1.02,
        "quad.waist": 1.06,
        "quad.rump": 1.10,
        "quad.belly": 0.58,
        "quad.section": 2.4,
        "quad.neck_thick": 0.90,
        "quad.neck_taper": 0.95,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.04,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 46.0,
        "quad.tail_len": 0.34,
        "quad.tail_thick": 0.34,
        "quad.tail_taper": 0.60,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.52,
        "quad.fore_reach": 0.40,
        "quad.foot": 1.20,
        "quad.under": 0.34,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "rock",
        "herd.size_min": 2,
        "herd.size_max": 12,
        "herd.spread_m": 30.0,
        "herd.despawn_m": 120.0,
        "herd.per_hectare": 2.0,
        "herd.flee_m": 25.0,
        "biomes.tundra_alpine": 1.0,
        "biomes.grassland": 0.5,
        "biomes.bare_rock": 0.4,
    },
)

add(
    "european-souslik",
    "A slimmer, smaller ground squirrel standing VERTICAL at a burrow mouth "
    "-- more upright than the marmot beside it, `neck_deg` 86 against 76, "
    "which is as near straight up as the row goes. Sandy with faint pale "
    "flecks, which are 1 cm on the real animal and are drawn as a thin DAPPLE "
    "rather than left out: at 1 cm they are one voxel each, which is the "
    "two-voxel floor missed, and they are authored at low strength so they "
    "read as texture rather than as spots.\n\n"
    "THE SIT WAS LOWERED BY MEASUREMENT. Authored first at `shoulder_h` 0.78 "
    "-- as near vertical as the row goes, which is what the biome row's "
    "'standing vertical' describes -- `tools/quadprobe.py --stance` measured "
    "the tail tip 3.0 voxels off the ground and reported NO TRIPOD. In the "
    "bipedal stance the generator solves the tail's angle so the tip reaches "
    "the floor, and a tail too short to get there is not a third leg. 0.62 "
    "with a longer tail measures 1.0 voxels and passes. The animal now sits "
    "back rather than standing bolt upright, which is a small loss against "
    "the row's wording and the honest one: the alternative was a tripod with "
    "a leg missing.\n\n"
    "LATTICE: 1 cm. 0.22 m is 22 voxels, the smallest body in this file "
    "outside the authored-up group and the same size as the library's "
    "smallest fish.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.22,
        "quad.stance": "bipedal",
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 0.42,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 86.0,
        "quad.head_deg": 6.0,
        "quad.depth": 0.38,
        "quad.width": 0.64,
        "quad.chest": 0.96,
        "quad.waist": 1.00,
        "quad.rump": 1.02,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.76,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.03,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 40.0,
        "quad.tail_len": 0.34,
        "quad.tail_thick": 0.28,
        "quad.tail_taper": 0.60,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.50,
        "quad.fore_reach": 0.42,
        "quad.foot": 1.10,
        "quad.under": 0.36,
        "quad.mark": "dapple",
        "quad.mark_count": 14,
        "quad.mark_width": 0.08,
        "quad.mark_strength": 0.35,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "open",
        "herd.size_min": 3,
        "herd.size_max": 25,
        "herd.spread_m": 25.0,
        "herd.despawn_m": 90.0,
        "herd.per_hectare": 4.0,
        "herd.flee_m": 15.0,
        "biomes.grassland": 1.0,
        "biomes.desert": 0.2,
    },
)

add(
    "black-tailed-prairie-dog",
    "Stouter and tanner than the souslik, with a SHORT BLACK-TIPPED TAIL, "
    "standing at a mound. Three ground squirrels in one file is deliberate: "
    "they are the same silhouette at three sizes with three different tails, "
    "and the tail is the only thing separating them.\n\n"
    "THE SIT IS THE LOWEST OF THE FOUR GROUND SQUIRRELS, AND IT WAS MEASURED "
    "RATHER THAN CHOSEN. `tools/quadprobe.py --stance` failed this species "
    "twice: at `shoulder_h` 0.70 the tail tip was 6.0 voxels off the ground "
    "and at 0.56 it was still 2.8, both reported as NO TRIPOD. The obvious fix "
    "-- a longer tail -- IS NOT AVAILABLE HERE, because the short black-tipped "
    "tail is the one thing separating this animal from the souslik beside it; "
    "lengthening it to reach the floor would have turned it into that species. "
    "So the ANIMAL moved instead: 0.46 with the hips carried higher, which "
    "measures 0.2 voxels and passes. It now sits back on its haunches rather "
    "than standing erect, and that is the trade -- the pose is less upright "
    "and the species is still itself.\n\n"
    "LATTICE: 1 cm. 0.35 m is 35 voxels; the black tip is 4 cm and four.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.35,
        "quad.stance": "bipedal",
        "quad.shoulder_h": 0.46,
        "quad.hip_h": 0.48,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.10,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 80.0,
        "quad.head_deg": 4.0,
        "quad.depth": 0.42,
        "quad.width": 0.70,
        "quad.chest": 1.00,
        "quad.waist": 1.04,
        "quad.rump": 1.06,
        "quad.belly": 0.56,
        "quad.neck_thick": 0.86,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.025,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 40.0,
        "quad.tail_len": 0.28,
        "quad.tail_thick": 0.32,
        "quad.tail_taper": 0.70,
        "quad.tail_tip": 0.45,
        "quad.leg_thick": 0.23,
        "quad.hock": 0.50,
        "quad.fore_reach": 0.42,
        "quad.foot": 1.10,
        "quad.under": 0.36,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_min": 5,
        "herd.size_max": 40,
        "herd.spread_m": 40.0,
        "herd.despawn_m": 100.0,
        "herd.per_hectare": 6.0,
        "herd.flee_m": 20.0,
        "biomes.grassland": 1.0,
        "biomes.desert": 0.2,
    },
)

add(
    "arctic-ground-squirrel",
    "The fourth of the upright ground squirrels and the tundra one: stocky, "
    "tawny, with pale spotting on the back and a short furred tail.\n\n"
    "LATTICE: 1 cm. 0.30 m is 30 voxels; the back spots are 1-2 cm and are "
    "drawn at the one-to-two voxel floor as a low-strength DAPPLE, which is "
    "the same accepted loss the souslik records.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.30,
        "quad.stance": "bipedal",
        "quad.shoulder_h": 0.72,
        "quad.hip_h": 0.40,
        "quad.trunk_frac": 0.57,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 80.0,
        "quad.head_deg": 4.0,
        "quad.depth": 0.42,
        "quad.width": 0.70,
        "quad.chest": 1.00,
        "quad.waist": 1.02,
        "quad.rump": 1.04,
        "quad.belly": 0.56,
        "quad.neck_thick": 0.84,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.03,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 42.0,
        "quad.tail_len": 0.36,
        "quad.tail_thick": 0.40,
        "quad.tail_taper": 0.75,
        "quad.leg_thick": 0.23,
        "quad.hock": 0.50,
        "quad.fore_reach": 0.42,
        "quad.foot": 1.10,
        "quad.under": 0.34,
        "quad.mark": "dapple",
        "quad.mark_count": 16,
        "quad.mark_width": 0.08,
        "quad.mark_strength": 0.40,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "open",
        "herd.size_min": 2,
        "herd.size_max": 16,
        "herd.spread_m": 30.0,
        "herd.despawn_m": 90.0,
        "herd.per_hectare": 3.0,
        "herd.flee_m": 15.0,
        "biomes.tundra_alpine": 1.0,
        "biomes.taiga": 0.4,
    },
)

add(
    "antelope-ground-squirrel",
    "AUTHORED AT 0.20 m AGAINST A REAL 0.16, which is the owner's 20 cm floor. "
    "The arithmetic: at 1 cm a 16 cm animal is sixteen voxels of head-body, "
    "and it has to carry a white flank stripe, a face, four legs and an arched "
    "tail in that. Twenty voxels is the floor because below it the parts stop "
    "being distinguishable from one another. Do NOT 'correct' this back to "
    "0.16 -- the animal at that size is a lozenge.\n\n"
    "The tail held ARCHED OVER THE BACK LIKE A PARASOL is the species and it "
    "is `tail_arc` at 0.95, higher than the grey squirrel's 0.85 and the "
    "highest in the library.\n\n"
    "LATTICE: 1 cm. The white flank stripe is 1 cm on the real animal and is "
    "authored WIDER than life through `mark_width` so it lands on two voxels "
    "at the authored size.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.shoulder_h": 0.40,
        "quad.hip_h": 1.08,
        "quad.trunk_frac": 0.57,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 26.0,
        "quad.head_deg": 2.0,
        "quad.depth": 0.40,
        "quad.width": 0.62,
        "quad.chest": 0.96,
        "quad.waist": 0.98,
        "quad.rump": 1.04,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.74,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.04,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 50.0,
        "quad.tail_len": 0.55,
        "quad.tail_thick": 0.48,
        "quad.tail_taper": 0.90,
        "quad.tail_deg": 30.0,
        "quad.tail_arc": 0.95,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.48,
        "quad.fore_bend": 0.30,
        "quad.under": 0.32,
        "quad.mark": "flankstripe",
        "quad.mark_width": 0.12,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "skin_pale",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "plume_white",
        "herd.cover": "open",
        "herd.size_max": 3,
        "herd.despawn_m": 70.0,
        "herd.per_hectare": 2.0,
        "herd.flee_m": 10.0,
        "biomes.desert": 1.0,
        "biomes.grassland": 0.2,
    },
)

add(
    "red-squirrel",
    "The grey squirrel's opposite number and the pair are worth having: rust-"
    "red instead of grey, with EAR TUFTS the grey has none of, and a tail "
    "carried in a tighter arch. `ear_shape` TUFTED is the whole difference "
    "besides the palette.\n\n"
    "LATTICE: 1 cm, and both `03-temperate-forest.md` and `07-taiga.md` flag "
    "this row. The tufts are 2 cm on a 0.22 m animal, two voxels at 1 cm, "
    "which is under the rule and the finest tier available -- exactly the "
    "limit `eastern-grey-squirrel` records for its ears. THE JOINT CAPS WILL "
    "BE OFF on this animal for the same reason they are off on the grey: its "
    "legs come out under three voxels thick, and `tools/quadprobe.py --caps` "
    "is what confirms that is a decision rather than an accident.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.22,
        "quad.shoulder_h": 0.42,
        "quad.hip_h": 1.10,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.20,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 30.0,
        "quad.head_deg": 0.0,
        "quad.depth": 0.44,
        "quad.width": 0.60,
        "quad.chest": 0.94,
        "quad.waist": 0.96,
        "quad.rump": 1.06,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.70,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "tufted",
        "quad.ear_len": 0.11,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 84.0,
        "quad.tail_len": 0.90,
        "quad.tail_thick": 0.56,
        "quad.tail_taper": 0.95,
        "quad.tail_deg": 26.0,
        "quad.tail_arc": 0.95,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.55,
        "quad.fore_bend": 0.35,
        "quad.under": 0.38,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "forest",
        "herd.size_max": 2,
        "herd.despawn_m": 70.0,
        "herd.per_hectare": 3.0,
        "herd.flee_m": 8.0,
        "biomes.taiga": 1.0,
        "biomes.temperate_forest": 0.8,
    },
)

add(
    "eastern-chipmunk",
    "AUTHORED AT 0.20 m AGAINST A REAL 0.15, the owner's 20 cm floor. At 1 cm "
    "a 15 cm animal is fifteen voxels and it has to carry FIVE dark stripes "
    "down the back -- five bands with four gaps in fifteen voxels is under two "
    "voxels apiece and merges into a wash, which "
    "`docs/biomes/README.md` §6 records as the way a band field fails. At "
    "0.20 m the stripes land on two voxels each. Do not shrink it back.\n\n"
    "The stripes run FORE-AND-AFT, which `quad.mark` cannot do -- BARS wrap "
    "the body transversely -- so what is authored is a dark SADDLE over the "
    "back and the individual stripes are lost. Third species in this file "
    "blocked on a longitudinal marking, after the badger and the skunk.\n\n"
    "LATTICE: 1 cm, which `03-temperate-forest.md` also flags.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.shoulder_h": 0.40,
        "quad.hip_h": 1.08,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 28.0,
        "quad.head_deg": 2.0,
        "quad.depth": 0.42,
        "quad.width": 0.62,
        "quad.chest": 0.96,
        "quad.waist": 0.98,
        "quad.rump": 1.04,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.74,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.06,
        "quad.ear_width": 0.75,
        "quad.ear_deg": 78.0,
        "quad.tail_len": 0.60,
        "quad.tail_thick": 0.36,
        "quad.tail_taper": 0.80,
        "quad.tail_deg": 34.0,
        "quad.tail_arc": 0.40,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.50,
        "quad.fore_bend": 0.32,
        "quad.under": 0.34,
        "quad.mark": "saddle",
        "quad.mark_width": 0.20,
        "quad.mark_strength": 0.8,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "forest",
        "herd.size_max": 2,
        "herd.despawn_m": 60.0,
        "herd.per_hectare": 3.0,
        "herd.flee_m": 6.0,
        "biomes.temperate_forest": 1.0,
    },
)

add(
    "siberian-flying-squirrel",
    "AUTHORED AT 0.20 m AGAINST A REAL 0.16, the owner's 20 cm floor -- the "
    "same arithmetic the antelope ground squirrel records.\n\n"
    "THE MEMBRANE IS NOT DRAWN, and that is the honest position rather than a "
    "compromise. A patagium is a sheet of skin between wrist and ankle: "
    "spread, it makes the animal a flat rectangle, and there is no sheet "
    "primitive in `forge/quadruped.py` at all. What is authored is the "
    "PERCHED animal, where the membrane is a baggy fold along the flank -- "
    "which is a very wide body with a loose flank line, and that IS "
    "expressible: `width` 0.86 with `under` taken high. The spread pose is a "
    "genuinely different asset and needs a mechanism nothing here has.\n\n"
    "LATTICE: 1 cm. The enormous black eyes are the field mark at 1 cm across "
    "on the real animal, so `quad.eye` is set to 2 -- a two-voxel radius patch "
    "-- which is oversize and deliberate. `07-taiga.md` flags this row.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.shoulder_h": 0.34,
        "quad.hip_h": 1.06,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.10,
        "quad.head_frac": 0.20,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 20.0,
        "quad.head_deg": 2.0,
        "quad.depth": 0.36,
        "quad.width": 0.86,
        "quad.chest": 0.98,
        "quad.waist": 1.04,
        "quad.rump": 1.02,
        "quad.belly": 0.50,
        "quad.section": 2.8,
        "quad.neck_thick": 0.78,
        "quad.head_size": 1.10,
        "quad.muzzle_depth": 0.40,
        "quad.muzzle_width": 0.38,
        "quad.eye": 2,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 66.0,
        "quad.tail_len": 0.70,
        "quad.tail_thick": 0.44,
        "quad.tail_taper": 0.90,
        "quad.tail_deg": 8.0,
        "quad.tail_arc": 0.20,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.46,
        "quad.fore_bend": 0.30,
        "quad.under": 0.44,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "forest",
        "herd.size_max": 2,
        "herd.despawn_m": 60.0,
        "herd.per_hectare": 1.0,
        "herd.flee_m": 10.0,
        "biomes.taiga": 1.0,
        "biomes.temperate_forest": 0.3,
    },
)

add(
    "eurasian-beaver",
    "THE TAIL IS UNMISTAKABLE AND NOTHING ELSE NEEDS TO BE -- a broad flat "
    "scaly paddle held horizontally. It is drawn as the widest, flattest tail "
    "in the library: `tail_thick` 0.95 with `tail_taper` 1.10, which is a tail "
    "that gets WIDER toward the tip, and a carriage angle of zero. Nothing "
    "else in this file has a taper above 1.\n\n"
    "LATTICE: 2 cm. 0.9 m of head-body is 45 voxels and the paddle at 30 cm "
    "wide is fifteen. Both biome files listing this species say 2 cm.",
    **{
        "quad.length_m": 0.90,
        "quad.shoulder_h": 0.34,
        "quad.hip_h": 1.04,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.09,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 4.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.46,
        "quad.width": 0.68,
        "quad.chest": 1.00,
        "quad.waist": 1.06,
        "quad.rump": 1.08,
        "quad.belly": 0.58,
        "quad.section": 2.6,
        "quad.neck_thick": 0.92,
        "quad.neck_taper": 0.98,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.48,
        "quad.muzzle_width": 0.48,
        "quad.jaw": 0.40,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.02,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 50.0,
        "quad.tail_len": 0.36,
        "quad.tail_thick": 0.95,
        "quad.tail_taper": 1.10,
        "quad.tail_deg": 0.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.34,
        "quad.fore_bend": 0.24,
        "quad.foot": 1.50,
        "quad.under": 0.0,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "waterside",
        "herd.size_min": 2,
        "herd.size_max": 6,
        "herd.spread_m": 20.0,
        "herd.despawn_m": 140.0,
        "herd.per_hectare": 0.4,
        "herd.flee_m": 30.0,
        "biomes.temperate_forest": 1.0,
        "biomes.taiga": 0.8,
        "biomes.grassland": 0.3,
    },
)

add(
    "capybara",
    "A BLUNT BRICK OF A RODENT, and the squarest body in the file: `section` "
    "at 3.4 is the highest squareness anywhere in the library, which is what "
    "turns the trunk's cross-section from an oval into a rectangle. Square "
    "blocky head, flat muzzle, NO VISIBLE TAIL -- `tail_len` 0.0, the only "
    "mammal here with none at all -- and short legs.\n\n"
    "LATTICE: 2 cm. 1.1 m is 55 voxels; nothing on the animal is smaller than "
    "its ears at 6 cm, which is three voxels.",
    **{
        "quad.length_m": 1.10,
        "quad.shoulder_h": 0.46,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.08,
        "quad.head_frac": 0.20,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 8.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.46,
        "quad.width": 0.66,
        "quad.chest": 1.02,
        "quad.waist": 1.04,
        "quad.rump": 1.06,
        "quad.belly": 0.56,
        "quad.section": 3.4,
        "quad.neck_thick": 0.98,
        "quad.neck_taper": 1.00,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.62,
        "quad.jaw": 0.45,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 56.0,
        "quad.tail_len": 0.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.30,
        "quad.fore_bend": 0.18,
        "quad.foot": 1.20,
        "quad.under": 0.0,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "waterside",
        "herd.size_min": 3,
        "herd.size_max": 20,
        "herd.spread_m": 30.0,
        "herd.per_hectare": 1.2,
        "herd.flee_m": 40.0,
        "biomes.rainforest": 1.0,
        "biomes.savanna": 0.3,
    },
)

add(
    "north-american-porcupine",
    "A HUNCHED DARK MASS COVERED IN QUILLS, and `03-temperate-forest.md` says "
    "the quills are a SURFACE and not geometry. That is exactly right and it "
    "is what this spec does: there is no quill primitive, so the animal is "
    "drawn as a high-backed hunched body -- `hump` 0.34 at `hump_at` 0.40, "
    "which is a rise over the MIDDLE of the back rather than over the "
    "shoulder, and is the only spec in the file to put the hump there -- with "
    "a coarse pale DAPPLE over the dark coat standing in for the light quill "
    "tips.\n\n"
    "LATTICE: 2 cm. 0.7 m is 35 voxels; the quill-tip dapple is authored at "
    "the two-voxel floor, which is what a texture wants.",
    **{
        "quad.length_m": 0.70,
        "quad.shoulder_h": 0.42,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.09,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": -4.0,
        "quad.head_deg": -14.0,
        "quad.depth": 0.50,
        "quad.width": 0.70,
        "quad.chest": 0.98,
        "quad.waist": 1.06,
        "quad.rump": 1.02,
        "quad.belly": 0.58,
        "quad.section": 2.4,
        "quad.hump": 0.34,
        "quad.hump_at": 0.40,
        "quad.neck_thick": 0.94,
        "quad.head_size": 0.85,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.44,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.02,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 44.0,
        "quad.tail_len": 0.28,
        "quad.tail_thick": 0.56,
        "quad.tail_taper": 0.75,
        "quad.tail_deg": -20.0,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.32,
        "quad.fore_bend": 0.28,
        "quad.foot": 1.30,
        "quad.under": 0.0,
        "quad.mark": "dapple",
        "quad.mark_count": 24,
        "quad.mark_width": 0.10,
        "quad.mark_strength": 0.55,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "forest",
        "herd.size_max": 1,
        "herd.despawn_m": 100.0,
        "herd.per_hectare": 0.2,
        "herd.flee_m": 15.0,
        "biomes.temperate_forest": 1.0,
        "biomes.taiga": 0.4,
    },
)

add(
    "norway-lemming",
    "AUTHORED AT 0.20 m AGAINST A REAL 0.13, the owner's 20 cm floor, and this "
    "is the largest authoring-up ratio in the file -- more than half again. "
    "The arithmetic: THE PATTERN IS THE WHOLE READ, as `08-tundra-alpine.md` "
    "says, and it is a bold black-and-yellow-brown blotching. At 1 cm a 13 cm "
    "animal is thirteen voxels, so a blotch field over it is three or four "
    "blobs of two voxels each and the pattern is noise. At 0.20 m it is twenty "
    "voxels and the blotches land on three. Do not shrink it back; a lemming "
    "without its pattern is a mouse.\n\n"
    "LATTICE: 1 cm. Ears hidden in the fur (`ear_len` 0.015), almost no tail, "
    "a blunt face -- there is nothing else on the animal to set a lattice by.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.shoulder_h": 0.38,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.08,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 8.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.44,
        "quad.width": 0.70,
        "quad.chest": 1.00,
        "quad.waist": 1.04,
        "quad.rump": 1.02,
        "quad.belly": 0.56,
        "quad.section": 2.3,
        "quad.neck_thick": 0.98,
        "quad.neck_taper": 1.00,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.40,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.015,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 40.0,
        "quad.tail_len": 0.06,
        "quad.tail_thick": 0.30,
        "quad.tail_deg": -20.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.40,
        "quad.fore_bend": 0.26,
        "quad.under": 0.30,
        "quad.mark": "blotch",
        "quad.mark_count": 7,
        "quad.mark_width": 0.30,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_min": 1,
        "herd.size_max": 8,
        "herd.spread_m": 15.0,
        "herd.despawn_m": 50.0,
        "herd.per_hectare": 8.0,
        "herd.flee_m": 5.0,
        "biomes.tundra_alpine": 1.0,
        "biomes.taiga": 0.3,
    },
)

add(
    "lesser-egyptian-jerboa",
    "AUTHORED AT 0.20 m AGAINST A REAL 0.10, which is double life size and the "
    "largest departure anywhere in this file. The arithmetic and the "
    "justification: at 1 cm a 10 cm animal is TEN voxels of head-body, and "
    "this species is bipedal with enormous hind legs, a head nearly as big as "
    "the body, and a tail twice the body length ending in a black-and-white "
    "flag. Ten voxels cannot carry a stance, let alone a two-tone tuft. At "
    "0.20 m the body is twenty voxels and the tail is forty, and the flag "
    "lands on four. `05-desert.md` flags this row for exactly this. DO NOT "
    "CORRECT IT BACK.\n\n"
    "It is the second bipedal rodent in the library after the meerkat and the "
    "opposite kind: the meerkat stands vertical on a thin prop, and a jerboa "
    "sits back with the tail trailing as a counterweight -- which is the "
    "kangaroo's arrangement at a fiftieth of the mass.\n\n"
    "LATTICE: 1 cm.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.stance": "bipedal",
        "quad.shoulder_h": 0.76,
        "quad.hip_h": 0.34,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.10,
        "quad.head_frac": 0.24,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 70.0,
        "quad.head_deg": 4.0,
        "quad.depth": 0.42,
        "quad.width": 0.62,
        "quad.chest": 0.94,
        "quad.waist": 0.98,
        "quad.rump": 1.10,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.72,
        "quad.head_size": 1.20,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.40,
        "quad.eye": 2,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.10,
        "quad.ear_width": 0.70,
        "quad.ear_deg": 74.0,
        "quad.tail_len": 1.55,
        "quad.tail_thick": 0.22,
        "quad.tail_taper": 0.60,
        "quad.tail_tuft": 0.85,
        "quad.tail_arc": -0.10,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.90,
        "quad.foot": 2.20,
        "quad.fore_reach": 0.32,
        "quad.under": 0.40,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 3,
        "herd.despawn_m": 60.0,
        "herd.per_hectare": 1.5,
        "herd.flee_m": 10.0,
        "biomes.desert": 1.0,
    },
)

add(
    "european-rabbit",
    "Shorter-eared, rounder and shorter-legged than a hare, with a WHITE TAIL "
    "FLASH -- which is `tail_tip` on a short upturned tail and is what a "
    "player actually sees, because a rabbit is nearly always going away.\n\n"
    "Beside `european-hare` it is the clearest ear comparison in the library: "
    "0.11 of head-body against the hare's 0.20, on an animal two thirds the "
    "size.\n\n"
    "LATTICE: 1 cm. 0.40 m is 40 voxels; the ears are 2.5 cm wide, so three "
    "voxels wants 0.8 cm and 1 cm is the finest tier there is -- at the limit "
    "of the rule, the same place the hare and the grey squirrel sit.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.40,
        "quad.shoulder_h": 0.40,
        "quad.hip_h": 1.14,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 20.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.44,
        "quad.width": 0.62,
        "quad.chest": 0.94,
        "quad.waist": 0.98,
        "quad.rump": 1.10,
        "quad.belly": 0.54,
        "quad.neck_thick": 0.68,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.11,
        "quad.ear_width": 0.42,
        "quad.ear_deg": 76.0,
        "quad.ear_back": 0.20,
        "quad.tail_len": 0.14,
        "quad.tail_thick": 0.34,
        "quad.tail_taper": 0.75,
        "quad.tail_deg": 30.0,
        "quad.tail_tip": 0.55,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.66,
        "quad.fore_bend": 0.28,
        "quad.foot": 1.30,
        "quad.under": 0.36,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "plume_white",
        "herd.cover": "edge",
        "herd.size_min": 2,
        "herd.size_max": 10,
        "herd.spread_m": 20.0,
        "herd.despawn_m": 90.0,
        "herd.per_hectare": 2.5,
        "herd.flee_m": 25.0,
        "biomes.grassland": 1.0,
        "biomes.temperate_forest": 0.6,
        "biomes.beach": 0.3,
    },
)

add(
    "black-tailed-jackrabbit",
    "EVEN LONGER EARS THAN THE BROWN HARE -- `ear_len` 0.26 on a 0.55 m animal "
    "against the hare's 0.20 on 0.65, which is a much bigger ear on a smaller "
    "body -- plus a black-topped tail and a sandy grey coat.\n\n"
    "LATTICE: 1 cm, and `02-grassland.md` flags this row. The ears are 3 cm "
    "wide, so three voxels wants 1 cm exactly.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.55,
        "quad.shoulder_h": 0.40,
        "quad.hip_h": 1.20,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 22.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.40,
        "quad.width": 0.56,
        "quad.chest": 0.90,
        "quad.waist": 0.94,
        "quad.rump": 1.12,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.62,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.26,
        "quad.ear_width": 0.34,
        "quad.ear_deg": 74.0,
        "quad.ear_back": 0.30,
        "quad.tail_len": 0.16,
        "quad.tail_thick": 0.30,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": -26.0,
        "quad.tail_tip": 0.55,
        "quad.leg_thick": 0.15,
        "quad.hock": 0.82,
        "quad.fore_bend": 0.30,
        "quad.foot": 1.60,
        "quad.under": 0.40,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 3,
        "herd.despawn_m": 120.0,
        "herd.per_hectare": 1.0,
        "herd.flee_m": 45.0,
        "biomes.desert": 1.0,
        "biomes.grassland": 0.8,
    },
)

add(
    "cape-hare",
    "Long erect ears longer than the head, very long folded hind legs, sandy-"
    "buff with a black-and-white tail. `05-desert.md` flags this row and says "
    "the ears alone identify it, which is the same claim `european-hare` makes "
    "and is why the two need different numbers rather than different names: "
    "this one's ears are 0.22 of head-body against the brown hare's 0.20, on "
    "an animal three quarters the size.\n\n"
    "LATTICE: 1 cm. Ears 3 cm wide on a 0.5 m animal; three voxels wants 1 cm.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.50,
        "quad.shoulder_h": 0.40,
        "quad.hip_h": 1.18,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 24.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.40,
        "quad.width": 0.56,
        "quad.chest": 0.92,
        "quad.waist": 0.94,
        "quad.rump": 1.12,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.62,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.22,
        "quad.ear_width": 0.32,
        "quad.ear_deg": 80.0,
        "quad.ear_back": 0.10,
        "quad.tail_len": 0.14,
        "quad.tail_thick": 0.30,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": -28.0,
        "quad.tail_tip": 0.50,
        "quad.leg_thick": 0.15,
        "quad.hock": 0.80,
        "quad.fore_bend": 0.30,
        "quad.foot": 1.55,
        "quad.under": 0.40,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 3,
        "herd.despawn_m": 110.0,
        "herd.per_hectare": 0.8,
        "herd.flee_m": 40.0,
        "biomes.desert": 1.0,
        "biomes.savanna": 0.5,
        "biomes.grassland": 0.3,
    },
)

add(
    "mountain-hare",
    "SHORTER EARS THAN ANY OTHER HARE HERE and black tips that stay black in "
    "every season -- `08-tundra-alpine.md` calls the tips the one constant "
    "mark, which is the same argument the stoat's black tail tip carries and "
    "the same mechanism: a marking on a part that does not change colour.\n\n"
    "The WINTER coat is authored, white with black ear tips, because that is "
    "the animal against snow and the one the biome files describe first.\n\n"
    "LATTICE: 1 cm, against `08-tundra-alpine.md`'s 2 and with "
    "`07-taiga.md`'s 1. The ear tip is 3 cm on a 0.55 m animal: three voxels "
    "wants 1 cm, and at 2 cm the tip is one voxel of black on a white animal, "
    "which is the exact defect the probes exist to catch.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.55,
        "quad.shoulder_h": 0.42,
        "quad.hip_h": 1.16,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 20.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.44,
        "quad.width": 0.62,
        "quad.chest": 0.96,
        "quad.waist": 1.00,
        "quad.rump": 1.10,
        "quad.belly": 0.54,
        "quad.neck_thick": 0.68,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.14,
        "quad.ear_width": 0.40,
        "quad.ear_deg": 78.0,
        "quad.ear_back": 0.20,
        "quad.tail_len": 0.12,
        "quad.tail_thick": 0.34,
        "quad.tail_taper": 0.75,
        "quad.tail_deg": -26.0,
        "quad.leg_thick": 0.17,
        "quad.hock": 0.78,
        "quad.fore_bend": 0.28,
        "quad.foot": 1.60,
        "quad.under": 0.0,
        "materials.quad_back": "plume_white",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_white",
        "materials.quad_leg": "plume_white",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 3,
        "herd.despawn_m": 110.0,
        "herd.per_hectare": 1.0,
        "herd.flee_m": 40.0,
        "biomes.tundra_alpine": 1.0,
        "biomes.taiga": 0.8,
        "biomes.bare_rock": 0.2,
    },
)

add(
    "pika",
    "AUTHORED AT 0.20 m AGAINST A REAL 0.18, the owner's 20 cm floor -- the "
    "smallest departure in the file, one voxel at 1 cm, and recorded anyway "
    "because a floor with exceptions that go unwritten is not a floor.\n\n"
    "A ROUND TAIL-LESS BALL, and `08-tundra-alpine.md` puts it exactly right: "
    "it looks like a hamster, not a rabbit. So the ears are ROUND rather than "
    "blade, `tail_len` is 0, there is no visible neck, and the body is nearly "
    "as wide as it is long. Beside the four hares in this section it is the "
    "proof that `ear_shape` is doing real work -- same family, same size "
    "class, and nobody would put them together.\n\n"
    "LATTICE: 1 cm. The round ears are 2 cm across, two voxels, under the rule "
    "and the finest tier available.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.shoulder_h": 0.44,
        "quad.hip_h": 1.04,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.07,
        "quad.head_frac": 0.21,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 16.0,
        "quad.head_deg": 0.0,
        "quad.depth": 0.48,
        "quad.width": 0.76,
        "quad.chest": 1.02,
        "quad.waist": 1.04,
        "quad.rump": 1.04,
        "quad.belly": 0.58,
        "quad.section": 2.2,
        "quad.neck_thick": 1.00,
        "quad.neck_taper": 1.05,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.10,
        "quad.ear_width": 1.00,
        "quad.ear_deg": 62.0,
        "quad.tail_len": 0.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.44,
        "quad.fore_bend": 0.28,
        "quad.under": 0.32,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "rock",
        "herd.size_max": 3,
        "herd.despawn_m": 60.0,
        "herd.per_hectare": 3.0,
        "herd.flee_m": 10.0,
        "biomes.tundra_alpine": 1.0,
        "biomes.bare_rock": 0.5,
    },
)


# ===========================================================================
# THE ONES THAT ARE NOT SHAPED LIKE ANYTHING ELSE
# ===========================================================================

add(
    "european-hedgehog",
    "A SPINE-COVERED DOME, and `02-grassland.md` says plainly that the spines "
    "are a surface texture and not geometry. So the animal is a dome: "
    "`hip_h` and `shoulder_h` near equal on a body wider than it is long, with "
    "a small pointed face poking out of the front and the legs almost "
    "invisible. The spines are a coarse DAPPLE in a paler colour over the "
    "back, which is texture rather than structure and is the honest read.\n\n"
    "LATTICE: 1 cm. 0.25 m is 25 voxels. The face -- the only non-dome part "
    "of the animal -- is 4 cm long, four voxels; at 2 cm it would be two and "
    "the hedgehog would be a rock.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.25,
        "quad.shoulder_h": 0.36,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.07,
        "quad.head_frac": 0.21,
        "quad.muzzle_frac": 0.16,
        "quad.neck_deg": 4.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.56,
        "quad.width": 0.86,
        "quad.chest": 1.04,
        "quad.waist": 1.10,
        "quad.rump": 1.08,
        "quad.belly": 0.60,
        "quad.section": 2.0,
        "quad.neck_thick": 1.00,
        "quad.neck_taper": 0.80,
        "quad.head_size": 0.75,
        "quad.muzzle_depth": 0.34,
        "quad.muzzle_width": 0.30,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.03,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 50.0,
        "quad.tail_len": 0.04,
        "quad.tail_thick": 0.20,
        "quad.tail_deg": -30.0,
        "quad.leg_thick": 0.16,
        "quad.hock": 0.34,
        "quad.fore_bend": 0.24,
        "quad.under": 0.16,
        "quad.mark": "dapple",
        "quad.mark_count": 26,
        "quad.mark_width": 0.09,
        "quad.mark_strength": 0.6,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "edge",
        "herd.size_max": 2,
        "herd.despawn_m": 60.0,
        "herd.per_hectare": 0.8,
        "herd.flee_m": 6.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.8,
    },
)

add(
    "european-mole",
    "AUTHORED AT 0.20 m AGAINST A REAL 0.15, the owner's 20 cm floor. At 1 cm "
    "a 15 cm mole is fifteen voxels of a cylinder with no neck, no visible "
    "eyes and two shovel forepaws -- and the paws are the entire species. "
    "Fifteen voxels of body puts a forepaw at two and it disappears into the "
    "shoulder. At 0.20 m the paw is four. Do not shrink it back.\n\n"
    "THE ONLY ANIMAL IN THE LIBRARY WITH `quad.eye` SET TO 0, which is what "
    "that setting exists for and the parameter's own help text names a mole "
    "for it. Also the largest `quad.foot` in the file at 2.6 -- the shovel "
    "paws, turned outward by the SPRAWLING stance so they face the way they "
    "dig.\n\n"
    "LATTICE: 1 cm.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.22,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.05,
        "quad.head_frac": 0.20,
        "quad.muzzle_frac": 0.15,
        "quad.neck_deg": 0.0,
        "quad.head_deg": 0.0,
        "quad.depth": 0.44,
        "quad.width": 0.66,
        "quad.chest": 1.04,
        "quad.waist": 1.06,
        "quad.rump": 1.00,
        "quad.belly": 0.52,
        "quad.section": 2.0,
        "quad.neck_thick": 1.05,
        "quad.neck_taper": 1.00,
        "quad.head_size": 0.75,
        "quad.muzzle_depth": 0.32,
        "quad.muzzle_width": 0.28,
        # No eye at all.
        "quad.eye": 0,
        "quad.ear_shape": "none",
        "quad.tail_len": 0.14,
        "quad.tail_thick": 0.26,
        "quad.tail_taper": 0.50,
        "quad.tail_deg": 20.0,
        "quad.leg_thick": 0.26,
        "quad.hock": 0.26,
        "quad.foot": 2.60,
        "quad.under": 0.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_grey",
        "herd.cover": "any",
        "herd.size_max": 1,
        "herd.despawn_m": 40.0,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 4.0,
        "biomes.grassland": 1.0,
        "biomes.temperate_forest": 0.6,
    },
)

add(
    "nine-banded-armadillo",
    "A JOINTED ARMOUR SHELL IN BANDS, which is `quad.mark` BARS used as "
    "STRUCTURE rather than as colour -- the bands wrap a cylinder, which is "
    "exactly what the marking does, and on this animal they are the shell "
    "segments rather than a coat pattern. It is the only species in the file "
    "where the marking is meant to read as geometry, and whether that works is "
    "a render question.\n\n"
    "Large ears and a long tapering armoured tail finish it.\n\n"
    "LATTICE: 2 cm. 0.5 m is 25 voxels of head-body -- low, and the bands at "
    "3 cm are 1.5 voxels, which is under the two-voxel floor. 1 cm was the "
    "alternative and was rejected: at 1 cm the bands are three voxels and "
    "correct, but the whole point of the animal is a hard shell read at "
    "distance, and a 50-voxel armadillo costs eight times the asset of a "
    "25-voxel one for a feature nobody sees past ten metres. THIS IS THE "
    "WEAKEST LATTICE CALL IN THE FILE ALONGSIDE THE GUANACO'S and a render "
    "should settle it.",
    **{
        "quad.length_m": 0.50,
        "quad.shoulder_h": 0.34,
        "quad.hip_h": 1.04,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.09,
        "quad.head_frac": 0.19,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 0.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.48,
        "quad.width": 0.68,
        "quad.chest": 1.00,
        "quad.waist": 1.02,
        "quad.rump": 1.06,
        "quad.belly": 0.54,
        "quad.section": 2.3,
        "quad.neck_thick": 0.90,
        "quad.head_size": 0.85,
        "quad.muzzle_depth": 0.38,
        "quad.muzzle_width": 0.30,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.09,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 72.0,
        "quad.tail_len": 0.80,
        "quad.tail_thick": 0.42,
        "quad.tail_taper": 0.25,
        "quad.tail_deg": -14.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.34,
        "quad.fore_bend": 0.24,
        "quad.foot": 1.30,
        "quad.under": 0.0,
        "quad.mark": "bars",
        "quad.mark_count": 9,
        "quad.mark_width": 0.16,
        "quad.mark_strength": 0.85,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "edge",
        "herd.size_max": 2,
        "herd.despawn_m": 90.0,
        "herd.per_hectare": 0.4,
        "herd.flee_m": 15.0,
        "biomes.grassland": 1.0,
        "biomes.savanna": 0.4,
        "biomes.temperate_forest": 0.3,
    },
)

add(
    "giant-anteater",
    "A LONG TUBULAR HEAD WITH NO VISIBLE EYE OR EAR, and an enormous flag of a "
    "tail nearly as long as the animal. The head is `muzzle_frac` 0.30 -- the "
    "longest snout share in the library, close to the parameter's ceiling of "
    "0.35 -- with `muzzle_depth` and `muzzle_width` both very low, which turns "
    "the muzzle from a box into a tube. `quad.eye` is 0, the second and last "
    "species in the file to use it after the mole, and the row's help text "
    "names this animal for it.\n\n"
    "The black wedge edged white across the shoulder is `quad.cape` in the "
    "marking colour: a dark field over the FRONT of the animal, which is "
    "exactly the shape.\n\n"
    "LATTICE: 2 cm. 1.2 m of head-body is 60 voxels; the snout is 40 cm and "
    "twenty; the white edging of the shoulder wedge is 4 cm and two.",
    **{
        "quad.length_m": 1.20,
        "quad.shoulder_h": 0.44,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.50,
        "quad.neck_frac": 0.08,
        "quad.head_frac": 0.12,
        "quad.muzzle_frac": 0.30,
        "quad.neck_deg": -10.0,
        "quad.head_deg": -22.0,
        "quad.depth": 0.44,
        "quad.width": 0.54,
        "quad.chest": 1.00,
        "quad.waist": 1.00,
        "quad.rump": 1.04,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.86,
        "quad.neck_taper": 0.60,
        "quad.head_size": 0.70,
        "quad.muzzle_depth": 0.24,
        "quad.muzzle_width": 0.20,
        "quad.jaw": 0.0,
        "quad.eye": 0,
        "quad.ear_shape": "none",
        # The flag.
        "quad.tail_len": 0.70,
        "quad.tail_thick": 0.90,
        "quad.tail_taper": 0.85,
        "quad.tail_deg": -12.0,
        "quad.tail_arc": 0.20,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.30,
        "quad.fore_bend": 0.28,
        "quad.foot": 1.40,
        "quad.under": 0.0,
        "quad.cape": 0.34,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_grey",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 1,
        "herd.despawn_m": 150.0,
        "herd.per_hectare": 0.08,
        "herd.flee_m": 40.0,
        "biomes.rainforest": 0.9,
        "biomes.savanna": 0.5,
        "biomes.grassland": 0.3,
    },
)

add(
    "brown-throated-sloth",
    "A HANGING SHAPE, NOT A STANDING ONE -- `04-rainforest.md` opens the row "
    "with that and it is the whole problem. `quad.stance` offers standing, "
    "sprawling and bipedal; there is no suspended stance and no branch for one "
    "to hang from, so a hanging sloth is not authorable here at all.\n\n"
    "WHAT IS AUTHORED IS THE SLOTH ON THE GROUND, which is a real thing it "
    "does and a famously bad one: SPRAWLING, with forelimbs far longer than "
    "hind (`fore_reach` 1.22, higher than the gorilla's 1.15 and the highest "
    "in the library), a rounded body, a flat round face and a shaggy coat. "
    "THIS IS A DEPARTURE FROM THE ROW AS WRITTEN and it is recorded here "
    "rather than quietly substituted. A suspended stance plus an attachment "
    "point is what the hanging version needs.\n\n"
    "LATTICE: 2 cm. 0.6 m is 30 voxels; the flat round face at 10 cm is five.",
    **{
        "quad.length_m": 0.60,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.26,
        "quad.hip_h": 0.88,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.09,
        "quad.head_frac": 0.22,
        "quad.muzzle_frac": 0.11,
        "quad.neck_deg": 10.0,
        "quad.head_deg": 0.0,
        "quad.depth": 0.48,
        "quad.width": 0.70,
        "quad.chest": 1.02,
        "quad.waist": 1.04,
        "quad.rump": 1.00,
        "quad.belly": 0.56,
        "quad.section": 2.1,
        "quad.neck_thick": 0.86,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.40,
        "quad.muzzle_width": 0.52,
        "quad.jaw": 0.20,
        "quad.ear_shape": "none",
        "quad.tail_len": 0.06,
        "quad.tail_thick": 0.24,
        "quad.tail_deg": -20.0,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.40,
        "quad.fore_bend": 0.40,
        "quad.fore_reach": 1.22,
        "quad.foot": 1.50,
        "quad.under": 0.0,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_grey",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "forest",
        "herd.size_max": 1,
        "herd.despawn_m": 80.0,
        "herd.per_hectare": 0.3,
        "herd.flee_m": 5.0,
        "biomes.rainforest": 1.0,
    },
)

add(
    "rock-hyrax",
    "LOOKS LIKE A RODENT AND IS NOT ONE, which `09-bare-rock.md` says and "
    "which the spec cannot express -- so what it expresses instead is the "
    "shape: guinea-pig-proportioned, tail-less, blunt-faced, short-legged, "
    "with small round ears. Third species authored for bare rock, after the "
    "klipspringer and the ibex.\n\n"
    "LATTICE: 2 cm. 0.5 m is 25 voxels; the ears at 3 cm are 1.5, which is "
    "the loss. 1 cm was rejected because a hyrax has no fine feature to spend "
    "it on -- the animal IS its outline.",
    **{
        "quad.length_m": 0.50,
        "quad.shoulder_h": 0.40,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.60,
        "quad.neck_frac": 0.07,
        "quad.head_frac": 0.20,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 10.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.46,
        "quad.width": 0.70,
        "quad.chest": 1.00,
        "quad.waist": 1.04,
        "quad.rump": 1.02,
        "quad.belly": 0.56,
        "quad.section": 2.3,
        "quad.neck_thick": 1.00,
        "quad.neck_taper": 1.00,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.46,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 54.0,
        "quad.tail_len": 0.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.36,
        "quad.fore_bend": 0.22,
        "quad.foot": 1.20,
        "quad.under": 0.26,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "rock",
        "herd.size_min": 3,
        "herd.size_max": 24,
        "herd.spread_m": 20.0,
        "herd.despawn_m": 90.0,
        "herd.per_hectare": 4.0,
        "herd.flee_m": 15.0,
        "biomes.bare_rock": 1.0,
        "biomes.savanna": 0.4,
        "biomes.desert": 0.3,
    },
)

add(
    "virginia-opossum",
    "Pale grizzled grey with a pointed pink snout, bare black ears and a LONG "
    "NAKED PREHENSILE TAIL -- which is drawn as the thinnest, least tapering "
    "tail in the file (`tail_thick` 0.22, `tail_taper` 0.30) in the pale "
    "marking colour, so it reads as bare skin against fur. That colour split "
    "between tail and body is the field mark and it is one material row.\n\n"
    "LATTICE: 2 cm. 0.45 m is 22 voxels of head-body, low; the tail carries "
    "the asset to 40. The bare ears at 4 cm are two voxels.",
    **{
        "quad.length_m": 0.45,
        "quad.shoulder_h": 0.38,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.09,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.15,
        "quad.neck_deg": 8.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.42,
        "quad.width": 0.62,
        "quad.chest": 1.00,
        "quad.waist": 1.00,
        "quad.rump": 1.02,
        "quad.belly": 0.54,
        "quad.neck_thick": 0.82,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.36,
        "quad.muzzle_width": 0.28,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 70.0,
        "quad.tail_len": 0.85,
        "quad.tail_thick": 0.22,
        "quad.tail_taper": 0.30,
        "quad.tail_deg": -16.0,
        "quad.tail_arc": 0.20,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.40,
        "quad.fore_bend": 0.26,
        "quad.under": 0.30,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "skin_pale",
        "materials.quad_head": "plume_white",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_pale",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "edge",
        "herd.size_max": 2,
        "herd.despawn_m": 80.0,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 12.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.4,
    },
)

# ===========================================================================
# PRIMATES
# ===========================================================================

add(
    "chimpanzee",
    "Lighter than the gorilla and built the same way: arms longer than legs, "
    "knuckle-walking, `fore_reach` above 1. Against `western-lowland-gorilla` "
    "on a sheet the differences are a flat back rather than a humped one, a "
    "pale bare face against matte black, and EARS THAT STAND CLEAR OF THE "
    "HEAD -- `ear_len` 0.06 against the gorilla's 0.035, which on a smaller "
    "animal is nearly double in proportion and is the field mark.\n\n"
    "LATTICE: 2 cm. 1.3 m is 65 voxels; the ears at 8 cm are four and the "
    "pale face at 15 cm is seven.",
    **{
        "quad.length_m": 1.30,
        "quad.shoulder_h": 0.60,
        "quad.hip_h": 0.88,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.10,
        "quad.head_frac": 0.20,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 22.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.48,
        "quad.width": 0.66,
        "quad.chest": 1.08,
        "quad.waist": 0.98,
        "quad.rump": 0.92,
        "quad.belly": 0.54,
        "quad.section": 2.4,
        "quad.hump": 0.0,
        "quad.neck_thick": 0.84,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.56,
        "quad.muzzle_width": 0.56,
        "quad.jaw": 0.60,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.06,
        "quad.ear_width": 0.95,
        "quad.ear_deg": 20.0,
        "quad.tail_len": 0.0,
        "quad.leg_thick": 0.26,
        "quad.hock": 0.52,
        "quad.fore_bend": 0.16,
        "quad.fore_reach": 1.12,
        "quad.foot": 1.30,
        "quad.under": 0.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_grey",
        "quad.sex_length": 1.15,
        "herd.cover": "forest",
        "herd.size_min": 3,
        "herd.size_max": 25,
        "herd.spread_m": 40.0,
        "herd.per_hectare": 0.3,
        "herd.flee_m": 40.0,
        "biomes.rainforest": 1.0,
        "biomes.savanna": 0.2,
    },
)

add(
    "mandrill",
    "THE MARK IS THE FACE, AND THE FACE IS THE ONE PLACE THIS GENERATOR "
    "CANNOT MARK. A mandrill is a red stripe down the middle of the muzzle "
    "with deep blue ridged flanges either side, and `quad.mark` puts one "
    "marking on the FLANK. This is the sixth species in this file to hit that "
    "wall and the one where it costs the most.\n\n"
    "What is authored: the HEAD material is `skin_red` against a grizzled "
    "olive body, so the animal reads as a stocky grey-brown ape with a "
    "brilliant red face. The blue flanges are lost. A per-region head marking "
    "-- the same feature the badger, the chamois, the cheetah, the pronghorn "
    "and the addax all need -- would give them back, and this is the strongest "
    "single case for it in the library.\n\n"
    "LATTICE: 1 cm, and `04-rainforest.md` says 1 cm. The face stripe is 3 cm "
    "on a 0.8 m animal, so three voxels wants 1 cm -- and that is the lattice "
    "for a feature that is not being drawn, which is worth being honest "
    "about. It is held at 1 cm anyway because the muzzle ridging is real "
    "geometry at that size and disappears at 2.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.80,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 0.86,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.16,
        "quad.neck_deg": 18.0,
        "quad.head_deg": -8.0,
        "quad.depth": 0.46,
        "quad.width": 0.64,
        "quad.chest": 1.10,
        "quad.waist": 0.96,
        "quad.rump": 0.90,
        "quad.belly": 0.52,
        "quad.hump": 0.16,
        "quad.hump_at": 0.12,
        "quad.neck_thick": 0.80,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.60,
        "quad.muzzle_width": 0.40,
        "quad.jaw": 0.85,
        "quad.mane": 0.20,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.03,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 30.0,
        "quad.tail_len": 0.10,
        "quad.tail_thick": 0.24,
        "quad.tail_taper": 0.55,
        "quad.tail_deg": 60.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.48,
        "quad.fore_bend": 0.28,
        "quad.foot": 1.10,
        "quad.under": 0.0,
        "materials.quad_back": "skin_olive",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_red",
        "materials.quad_leg": "skin_olive",
        "materials.quad_tail": "skin_olive",
        "materials.quad_mark": "plume_buff",
        "quad.sex_length": 1.30,
        "quad.sex_mane": 2.0,
        "herd.cover": "forest",
        "herd.size_min": 4,
        "herd.size_max": 40,
        "herd.spread_m": 40.0,
        "herd.per_hectare": 1.0,
        "herd.flee_m": 35.0,
        "biomes.rainforest": 1.0,
    },
)

add(
    "black-howler-monkey",
    "A SQUARE HEAVY HEAD ON A COMPACT BLACK BODY, with a prehensile tail "
    "carried in a curl and longer than the animal. The head is the species: a "
    "howler's throat and jaw are swollen into a box, which is `jaw` at 1.0 -- "
    "the parameter's ceiling, and the only spec in the library to reach it -- "
    "with `muzzle_depth` high and `head_size` up.\n\n"
    "The curled tail is `tail_arc` 0.75 on a tail of 1.15 body lengths, which "
    "is the second longest in the file after the jerboa's.\n\n"
    "LATTICE: 1 cm, which `04-rainforest.md` flags. 0.6 m is 60 voxels; the "
    "jaw box is 8 cm and eight. At 2 cm the head is fifteen voxels and the "
    "swelling that makes it a howler is three.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.60,
        "quad.shoulder_h": 0.56,
        "quad.hip_h": 0.94,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.10,
        "quad.head_frac": 0.22,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 20.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.44,
        "quad.width": 0.62,
        "quad.chest": 1.04,
        "quad.waist": 0.98,
        "quad.rump": 0.98,
        "quad.belly": 0.52,
        "quad.section": 2.3,
        "quad.neck_thick": 0.86,
        "quad.head_size": 1.20,
        "quad.muzzle_depth": 0.78,
        "quad.muzzle_width": 0.60,
        "quad.jaw": 1.00,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.025,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 26.0,
        "quad.tail_len": 1.15,
        "quad.tail_thick": 0.34,
        "quad.tail_taper": 0.55,
        "quad.tail_deg": 40.0,
        "quad.tail_arc": 0.75,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.50,
        "quad.fore_bend": 0.30,
        "quad.fore_reach": 1.05,
        "quad.foot": 1.10,
        "quad.under": 0.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_buff",
        "quad.sex_length": 1.20,
        "herd.cover": "forest",
        "herd.size_min": 3,
        "herd.size_max": 15,
        "herd.spread_m": 30.0,
        "herd.per_hectare": 0.8,
        "herd.flee_m": 25.0,
        "biomes.rainforest": 1.0,
    },
)

# ===========================================================================
# REPTILES AND AMPHIBIANS -- the sprawling stance, eight more times
#
# `water-monitor` shipped the sprawling stance and proved it. What this
# section adds is range: a 2 m crocodile and a 20 cm lizard, a squat frog and
# a whip-tailed monitor, all on the same attachment change.
# ===========================================================================

add(
    "nile-crocodile",
    "THE LARGEST SPRAWLING ANIMAL IN THE LIBRARY, and a real test of the "
    "stance at scale: `water-monitor` is 0.75 m of head-body and this is "
    "2.0 m, with limbs that have to leave a flank three times as deep and "
    "still reach a floor three times as far down.\n\n"
    "The double row of raised scutes along the tail is drawn as a MANE -- "
    "`quad.mane` is a dorsal crest and a crocodile's scute ridge is exactly "
    "that, and it is the only place in the library where the mane parameter is "
    "used on something with no hair. That is a genuine reuse rather than a "
    "fudge: the row makes a raised ridge along the spine and does not care "
    "what it is made of.\n\n"
    "LATTICE: 5 cm. 2.0 m of head-body is 40 voxels and the tail carries the "
    "asset to 90; the scute ridge is 10 cm of relief, two voxels, which is the "
    "one thing short at this lattice. 2 cm was rejected because a 4.5 m "
    "asset at 2 cm is 225 voxels long and the animal is mostly one shape.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.00,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.16,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.50,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.20,
        "quad.muzzle_frac": 0.16,
        "quad.neck_deg": 0.0,
        "quad.head_deg": 0.0,
        "quad.depth": 0.28,
        "quad.width": 1.05,
        "quad.chest": 0.98,
        "quad.waist": 1.02,
        "quad.rump": 0.96,
        "quad.belly": 0.42,
        "quad.section": 3.0,
        "quad.neck_thick": 0.92,
        "quad.neck_taper": 0.95,
        "quad.mane": 0.30,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.34,
        "quad.muzzle_width": 0.62,
        "quad.jaw": 0.55,
        "quad.eye": 1,
        "quad.ear_shape": "none",
        "quad.tail_len": 1.20,
        "quad.tail_thick": 0.80,
        "quad.tail_taper": 0.06,
        "quad.tail_deg": -2.0,
        "quad.leg_thick": 0.26,
        "quad.hock": 0.28,
        "quad.foot": 1.20,
        "quad.under": 0.30,
        "materials.quad_back": "skin_olive",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_olive",
        "materials.quad_leg": "skin_olive",
        "materials.quad_tail": "skin_olive",
        "materials.quad_mark": "skin_dark",
        "materials.quad_horn": "skin_dark",
        "quad.sex_length": 1.25,
        "herd.cover": "waterside",
        "herd.size_max": 4,
        "herd.spread_m": 30.0,
        "herd.despawn_m": 300.0,
        "herd.per_hectare": 0.2,
        "herd.flee_m": 25.0,
        "biomes.savanna": 1.0,
        "biomes.rainforest": 0.5,
        "biomes.grassland": 0.2,
    },
)

add(
    "spectacled-caiman",
    "The crocodile at half the size and flatter still, with a broad snout and "
    "a bony ridge between the eyes -- which is drawn as the `mane` crest "
    "carried forward onto the skull rather than as a separate part. Beside the "
    "Nile crocodile it is the test of whether one sprawling reptile at two "
    "sizes is two animals.\n\n"
    "LATTICE: 2 cm. 0.9 m of head-body is 45 voxels; the eye ridge is 4 cm and "
    "two voxels. `04-rainforest.md` says 2 cm.",
    **{
        "quad.length_m": 0.90,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.16,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.50,
        "quad.neck_frac": 0.13,
        "quad.head_frac": 0.21,
        "quad.muzzle_frac": 0.16,
        "quad.neck_deg": 0.0,
        "quad.head_deg": 0.0,
        "quad.depth": 0.26,
        "quad.width": 1.10,
        "quad.chest": 0.98,
        "quad.waist": 1.02,
        "quad.rump": 0.96,
        "quad.belly": 0.40,
        "quad.section": 3.2,
        "quad.neck_thick": 0.94,
        "quad.neck_taper": 1.00,
        "quad.mane": 0.24,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.30,
        "quad.muzzle_width": 0.70,
        "quad.jaw": 0.50,
        "quad.ear_shape": "none",
        "quad.tail_len": 1.15,
        "quad.tail_thick": 0.78,
        "quad.tail_taper": 0.07,
        "quad.tail_deg": -2.0,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.28,
        "quad.foot": 1.15,
        "quad.under": 0.30,
        "quad.mark": "bars",
        "quad.mark_count": 8,
        "quad.mark_width": 0.12,
        "quad.mark_strength": 0.6,
        "materials.quad_back": "skin_olive",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_olive",
        "materials.quad_leg": "skin_olive",
        "materials.quad_tail": "skin_olive",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "waterside",
        "herd.size_max": 5,
        "herd.spread_m": 20.0,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 20.0,
        "biomes.rainforest": 1.0,
        "biomes.savanna": 0.3,
    },
)

add(
    "nile-monitor",
    "A WHIP TAIL TWICE THE BODY LENGTH -- `tail_len` 1.55, the longest in the "
    "library and effectively the parameter's ceiling -- on a sprawling lizard "
    "with a long neck and rows of yellow spots. Against the shipped "
    "`water-monitor` it is longer-necked, longer-tailed and spotted rather "
    "than banded.\n\n"
    "LATTICE: 2 cm. The spot rows are 4 cm on a 0.6 m head-body, two voxels; "
    "`06-savanna.md` says 2 cm.",
    **{
        "quad.length_m": 0.60,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.20,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.46,
        "quad.neck_frac": 0.26,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 4.0,
        "quad.head_deg": 0.0,
        "quad.depth": 0.30,
        "quad.width": 0.92,
        "quad.chest": 0.92,
        "quad.waist": 1.00,
        "quad.rump": 0.92,
        "quad.belly": 0.44,
        "quad.section": 2.6,
        "quad.neck_thick": 0.56,
        "quad.neck_taper": 0.82,
        "quad.head_size": 0.82,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.46,
        "quad.jaw": 0.30,
        "quad.ear_shape": "none",
        "quad.tail_len": 1.55,
        "quad.tail_thick": 0.66,
        "quad.tail_taper": 0.06,
        "quad.tail_deg": -4.0,
        "quad.tail_arc": -0.15,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.30,
        "quad.foot": 1.10,
        "quad.under": 0.32,
        "quad.mark": "spots",
        "quad.mark_count": 20,
        "quad.mark_width": 0.09,
        "quad.mark_strength": 0.9,
        "materials.quad_back": "skin_olive",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_olive",
        "materials.quad_leg": "skin_olive",
        "materials.quad_tail": "skin_olive",
        "materials.quad_mark": "skin_yellow",
        "herd.cover": "waterside",
        "herd.size_max": 2,
        "herd.per_hectare": 0.2,
        "herd.flee_m": 25.0,
        "biomes.savanna": 0.9,
        "biomes.rainforest": 0.4,
        "biomes.grassland": 0.2,
    },
)

add(
    "desert-monitor",
    "The third monitor and the shortest-tailed: grey-yellow with faint dark "
    "cross-bands, a thick tapering tail rather than a whip, and the whole "
    "animal held closer to the ground than either of the others.\n\n"
    "LATTICE: 2 cm. 0.45 m of head-body is 22 voxels, low; the tail carries "
    "the asset to 55. The cross-bands are 4 cm and two voxels. `05-desert.md` "
    "says 2 cm.",
    **{
        "quad.length_m": 0.45,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.18,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.50,
        "quad.neck_frac": 0.22,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 2.0,
        "quad.depth": 0.30,
        "quad.width": 0.96,
        "quad.chest": 0.94,
        "quad.waist": 1.00,
        "quad.rump": 0.94,
        "quad.belly": 0.44,
        "quad.section": 2.7,
        "quad.neck_thick": 0.62,
        "quad.neck_taper": 0.84,
        "quad.head_size": 0.85,
        "quad.muzzle_depth": 0.48,
        "quad.muzzle_width": 0.50,
        "quad.ear_shape": "none",
        "quad.tail_len": 1.30,
        "quad.tail_thick": 0.74,
        "quad.tail_taper": 0.12,
        "quad.tail_deg": -4.0,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.30,
        "quad.foot": 1.10,
        "quad.under": 0.34,
        "quad.mark": "bars",
        "quad.mark_count": 10,
        "quad.mark_width": 0.12,
        "quad.mark_strength": 0.6,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "open",
        "herd.size_max": 1,
        "herd.despawn_m": 90.0,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 25.0,
        "biomes.desert": 1.0,
    },
)

add(
    "marine-iguana",
    "A FLATTENED TAIL LONGER THAN THE BODY, a blunt square snout, a low spiny "
    "dorsal crest, lying flat on black rock. The crest is `quad.mane` again -- "
    "the third reptile in this section to use it, which settles that the row "
    "is a dorsal ridge and not hair.\n\n"
    "LATTICE: 2 cm. 0.6 m of head-body is 30 voxels; the crest spines are "
    "3-4 cm and are drawn as a continuous ridge at two voxels rather than as "
    "individual spikes, which is what the lattice allows. `01-beach.md` says "
    "2 cm.",
    **{
        "quad.length_m": 0.60,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.18,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 2.0,
        "quad.head_deg": 0.0,
        "quad.depth": 0.32,
        "quad.width": 0.88,
        "quad.chest": 0.96,
        "quad.waist": 1.00,
        "quad.rump": 0.94,
        "quad.belly": 0.44,
        "quad.section": 2.5,
        "quad.neck_thick": 0.76,
        "quad.neck_taper": 0.90,
        "quad.mane": 0.26,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.60,
        "quad.jaw": 0.35,
        "quad.ear_shape": "none",
        "quad.tail_len": 1.15,
        "quad.tail_thick": 0.68,
        "quad.tail_taper": 0.10,
        "quad.tail_deg": -2.0,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.30,
        "quad.foot": 1.20,
        "quad.under": 0.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_rufous",
        "herd.cover": "rock",
        "herd.size_min": 3,
        "herd.size_max": 30,
        "herd.spread_m": 20.0,
        "herd.per_hectare": 3.0,
        "herd.flee_m": 8.0,
        "biomes.beach": 1.0,
        "biomes.bare_rock": 0.4,
    },
)

add(
    "spiny-tailed-lizard",
    "A SQUAT FLAT BODY WITH A SHORT THICK TAIL RINGED IN SPIKES, which is the "
    "opposite of every other lizard in this section: their tails are whips "
    "and this one's is a club. `tail_len` 0.70 with `tail_thick` 0.85 and "
    "`tail_taper` 0.55 is that -- short, heavy, barely tapering -- and the "
    "spike rings are drawn as BARS in the horn colour, which reads as a "
    "banded club rather than as individual spikes. The spikes themselves are "
    "1 cm and are not geometry at any lattice here.\n\n"
    "LATTICE: 1 cm. 0.25 m of head-body is 25 voxels; the tail rings are 1.5 "
    "cm and are drawn at the two-voxel floor. `05-desert.md` says 1 cm.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.25,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.20,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.11,
        "quad.head_frac": 0.20,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 2.0,
        "quad.depth": 0.30,
        "quad.width": 1.05,
        "quad.chest": 1.00,
        "quad.waist": 1.04,
        "quad.rump": 1.00,
        "quad.belly": 0.44,
        "quad.section": 3.0,
        "quad.neck_thick": 0.92,
        "quad.neck_taper": 1.00,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.66,
        "quad.jaw": 0.30,
        "quad.ear_shape": "none",
        "quad.tail_len": 0.70,
        "quad.tail_thick": 0.85,
        "quad.tail_taper": 0.55,
        "quad.tail_deg": -2.0,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.28,
        "quad.foot": 1.10,
        "quad.under": 0.24,
        "quad.mark": "bars",
        "quad.mark_count": 9,
        "quad.mark_width": 0.13,
        "quad.mark_strength": 0.8,
        "materials.quad_back": "skin_yellow",
        "materials.quad_belly": "skin_olive",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "rock",
        "herd.size_max": 2,
        "herd.despawn_m": 60.0,
        "herd.per_hectare": 0.6,
        "herd.flee_m": 12.0,
        "biomes.desert": 1.0,
        "biomes.bare_rock": 0.3,
    },
)

add(
    "ocellated-lizard",
    "A LARGE LIZARD WITH ROWS OF BLUE EYE-SPOTS ALONG THE FLANK, and the "
    "ocelli are the species.\n\n"
    "THE ANIMAL MOVED, NOT THE MARKING, AND HERE IS THE MEASUREMENT. Authored "
    "first as `plume_cyan` ocelli on a `skin_green` body, "
    "`tools/quadprobe.py --read` measured that pair at 1.60 against a floor of "
    "1.80 -- FAINT, which on this species means the animal ships with no "
    "spots. Nothing blue in the palette clears the floor against green: "
    "`skin_blue` is DARKER than `skin_green` and measures 1.66 the other way. "
    "So the BODY was darkened from `skin_green` to `skin_olive`, which "
    "measures 2.85 against the same cyan, and the blue ocelli survive. That is "
    "the `lilac-breasted-roller` precedent -- move the animal when moving the "
    "colour would destroy the species -- and a real ocellated lizard is "
    "olive-green as often as it is grass-green, so the departure is small. DO "
    "NOT put the body back to `skin_green`: the spots go with it.\n\n"
    "LATTICE: 1 cm. The eye-spots are 1.5 cm on a 0.25 m head-body, which is "
    "1.5 voxels at 1 cm -- under the floor, and authored WIDER than life "
    "through `mark_width` so they land on two. `02-grassland.md` says 1 cm.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.25,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.18,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 2.0,
        "quad.depth": 0.28,
        "quad.width": 0.90,
        "quad.chest": 0.96,
        "quad.waist": 1.00,
        "quad.rump": 0.94,
        "quad.belly": 0.44,
        "quad.section": 2.6,
        "quad.neck_thick": 0.76,
        "quad.neck_taper": 0.90,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.48,
        "quad.muzzle_width": 0.56,
        "quad.jaw": 0.35,
        "quad.ear_shape": "none",
        "quad.tail_len": 1.50,
        "quad.tail_thick": 0.60,
        "quad.tail_taper": 0.08,
        "quad.tail_deg": -2.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.30,
        "quad.foot": 1.10,
        "quad.under": 0.28,
        "quad.mark": "spots",
        "quad.mark_count": 16,
        "quad.mark_width": 0.10,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "skin_olive",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_olive",
        "materials.quad_leg": "skin_olive",
        "materials.quad_tail": "skin_olive",
        "materials.quad_mark": "plume_cyan",
        "herd.cover": "rock",
        "herd.size_max": 2,
        "herd.despawn_m": 60.0,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 12.0,
        "biomes.grassland": 1.0,
        "biomes.bare_rock": 0.3,
    },
)

add(
    "sand-lizard",
    "AUTHORED AT 0.22 m OF HEAD-BODY AGAINST A REAL 0.09, which is well over "
    "double and needs the arithmetic said out loud. The owner's floor is 20 cm "
    "of real length; at 1 cm a 9 cm lizard is NINE voxels of head-body, and it "
    "has to carry a pale dorsal band, four sprawled limbs and a head. Nine "
    "voxels is a matchstick. At 0.22 m the body is 22 voxels and the dorsal "
    "band lands on two. `02-grassland.md` flags this row. Do not shrink it "
    "back.\n\n"
    "The male's spring green flanks are the field mark and are authored as the "
    "default, because a brown lizard 22 voxels long is indistinguishable from "
    "a twig and a green one is not. That is a departure from 'typical adult' "
    "toward 'recognisable', and it is deliberate.\n\n"
    "LATTICE: 1 cm.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.22,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.18,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 2.0,
        "quad.depth": 0.30,
        "quad.width": 0.92,
        "quad.chest": 1.00,
        "quad.waist": 1.02,
        "quad.rump": 0.96,
        "quad.belly": 0.44,
        "quad.section": 2.5,
        "quad.neck_thick": 0.82,
        "quad.neck_taper": 0.95,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.52,
        "quad.ear_shape": "none",
        "quad.tail_len": 1.20,
        "quad.tail_thick": 0.56,
        "quad.tail_taper": 0.10,
        "quad.tail_deg": -2.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.30,
        "quad.foot": 1.05,
        "quad.under": 0.30,
        "quad.mark": "saddle",
        "quad.mark_width": 0.12,
        "quad.mark_strength": 0.9,
        "materials.quad_back": "skin_green",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_green",
        "materials.quad_leg": "skin_green",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "open",
        "herd.size_max": 2,
        "herd.despawn_m": 50.0,
        "herd.per_hectare": 1.0,
        "herd.flee_m": 8.0,
        "biomes.grassland": 1.0,
        "biomes.desert": 0.3,
        "biomes.beach": 0.3,
    },
)

add(
    "viviparous-lizard",
    "AUTHORED AT 0.20 m OF HEAD-BODY AGAINST A REAL 0.06, which is more than "
    "three times life size and is the largest ratio anywhere in this library. "
    "The arithmetic: at 1 cm a 6 cm lizard is SIX voxels of body. Six voxels "
    "is one voxel of head, three of trunk and two of hip, with no room for a "
    "leg to leave the flank -- it would not be an animal, it would be a "
    "pebble. The owner's 20 cm floor exists exactly for this case. DO NOT "
    "CORRECT IT BACK; there is no lattice at which a 6 cm lizard is an asset.\n\n"
    "It is worth saying what that costs: at 0.20 m this lizard is the same "
    "authored size as the sand lizard beside it, and the real animals differ "
    "by a third. Both are at the floor, so the floor has flattened a real "
    "difference. Colour and proportion carry the separation instead -- this "
    "one is brown with a pale dorsal band and stockier.\n\n"
    "LATTICE: 1 cm. `03-temperate-forest.md` flags this row.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.18,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 2.0,
        "quad.depth": 0.32,
        "quad.width": 0.96,
        "quad.chest": 1.02,
        "quad.waist": 1.04,
        "quad.rump": 0.98,
        "quad.belly": 0.44,
        "quad.section": 2.5,
        "quad.neck_thick": 0.88,
        "quad.neck_taper": 1.00,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.54,
        "quad.ear_shape": "none",
        "quad.tail_len": 1.05,
        "quad.tail_thick": 0.58,
        "quad.tail_taper": 0.12,
        "quad.tail_deg": -2.0,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.30,
        "quad.foot": 1.05,
        "quad.under": 0.30,
        "quad.mark": "saddle",
        "quad.mark_width": 0.14,
        "quad.mark_strength": 0.8,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_buff",
        "herd.cover": "edge",
        "herd.size_max": 2,
        "herd.despawn_m": 45.0,
        "herd.per_hectare": 1.2,
        "herd.flee_m": 6.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.4,
        "biomes.taiga": 0.3,
    },
)

add(
    "fire-salamander",
    "GLOSSY BLACK WITH IRREGULAR BRIGHT YELLOW BLOTCHES, which is `quad.mark` "
    "BLOTCH at full strength on the highest-contrast pair in the whole "
    "library: `skin_yellow` on `skin_dark`. Short legs, a blunt head, no ears, "
    "and the sprawling stance with the belly on the ground.\n\n"
    "At exactly 0.20 m it sits ON the owner's floor rather than under it, "
    "which is worth noting because it means the animal is authored at life "
    "size and nothing was bent for it -- the only amphibian here of which that "
    "is true.\n\n"
    "LATTICE: 1 cm. 0.20 m is 20 voxels; the blotches are 2 cm and two voxels, "
    "which is the two-voxel floor met and the three-voxel rule missed. "
    "Accepted, because the blotch is a colour and not a shape and the contrast "
    "carries it.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.16,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.10,
        "quad.head_frac": 0.20,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 0.0,
        "quad.depth": 0.32,
        "quad.width": 0.90,
        "quad.chest": 1.00,
        "quad.waist": 1.04,
        "quad.rump": 1.00,
        "quad.belly": 0.44,
        "quad.section": 2.2,
        "quad.neck_thick": 0.92,
        "quad.neck_taper": 1.00,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.44,
        "quad.muzzle_width": 0.62,
        "quad.jaw": 0.20,
        "quad.eye": 2,
        "quad.ear_shape": "none",
        "quad.tail_len": 0.85,
        "quad.tail_thick": 0.62,
        "quad.tail_taper": 0.20,
        "quad.tail_deg": -2.0,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.26,
        "quad.foot": 1.00,
        "quad.under": 0.0,
        "quad.mark": "blotch",
        "quad.mark_count": 10,
        "quad.mark_width": 0.24,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "skin_yellow",
        "herd.cover": "forest",
        "herd.size_max": 2,
        "herd.despawn_m": 45.0,
        "herd.per_hectare": 0.8,
        "herd.flee_m": 4.0,
        "biomes.temperate_forest": 1.0,
        "biomes.taiga": 0.2,
    },
)

add(
    "common-frog",
    "AUTHORED AT 0.20 m AGAINST A REAL 0.09, the owner's 20 cm floor. At 1 cm "
    "a 9 cm frog is nine voxels, and the whole read of a frog is a CROUCH -- "
    "folded hind legs bunched above the line of the back, bulging eyes on top "
    "of the head -- which is a relationship between four parts. Nine voxels "
    "has no room for a relationship. `03-temperate-forest.md` flags the row. "
    "Do not shrink it back.\n\n"
    "THE CROUCH IS `hock` AT 1.0, the parameter's ceiling and the only spec in "
    "the library to reach it: the hind leg folds so far that the knee stands "
    "above the hip, which is exactly a sitting frog. Sprawling stance, no "
    "tail, no ears, and `quad.eye` at 2 for the bulge.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.26,
        "quad.hip_h": 1.30,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.04,
        "quad.head_frac": 0.24,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 0.0,
        "quad.head_deg": 8.0,
        "quad.depth": 0.44,
        "quad.width": 0.90,
        "quad.chest": 0.94,
        "quad.waist": 1.02,
        "quad.rump": 1.12,
        "quad.belly": 0.52,
        "quad.section": 2.0,
        "quad.neck_thick": 1.05,
        "quad.neck_taper": 1.05,
        "quad.head_size": 1.10,
        "quad.muzzle_depth": 0.40,
        "quad.muzzle_width": 0.85,
        "quad.jaw": 0.55,
        "quad.eye": 2,
        "quad.ear_shape": "none",
        "quad.tail_len": 0.0,
        "quad.leg_thick": 0.22,
        # The crouch.
        "quad.hock": 1.00,
        "quad.fore_bend": 0.45,
        "quad.foot": 1.60,
        "quad.under": 0.34,
        "quad.mark": "blotch",
        "quad.mark_count": 8,
        "quad.mark_width": 0.20,
        "quad.mark_strength": 0.6,
        "materials.quad_back": "skin_olive",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_olive",
        "materials.quad_leg": "skin_olive",
        "materials.quad_tail": "skin_olive",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "waterside",
        "herd.size_min": 1,
        "herd.size_max": 8,
        "herd.spread_m": 10.0,
        "herd.despawn_m": 45.0,
        "herd.per_hectare": 4.0,
        "herd.flee_m": 4.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.5,
        "biomes.taiga": 0.3,
    },
)

add(
    "poison-dart-frog",
    "AUTHORED AT 0.20 m AGAINST A REAL 0.045, which is more than four times "
    "life size and the largest departure in this library by a wide margin. "
    "The arithmetic: at 1 cm a 4.5 cm frog is FOUR AND A HALF VOXELS. That is "
    "not a small asset, it is not an asset -- four voxels cannot hold a head, "
    "a body and a folded leg, let alone the two or three black bands that "
    "`04-rainforest.md` says are the whole point of the species. At 0.20 m the "
    "body is 20 voxels and the bands land on three.\n\n"
    "THE SIZE IS THE COST OF HAVING THE SPECIES AT ALL. A 20 cm dart frog is "
    "four times life size and will read as a large frog, which is wrong and is "
    "the lesser wrong: the alternative is no dart frog. This is exactly the "
    "trade the owner's floor was set to force, and it is recorded here so "
    "nobody 'corrects' it and quietly removes the animal.\n\n"
    "LATTICE: 1 cm. Flat saturated colour on one bright ground -- "
    "`skin_orange` over `skin_dark` bands, which is a contrast ratio far above "
    "the readability floor and is the point of the species.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.20,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.28,
        "quad.hip_h": 1.28,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.04,
        "quad.head_frac": 0.26,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 0.0,
        "quad.head_deg": 14.0,
        "quad.depth": 0.46,
        "quad.width": 0.86,
        "quad.chest": 0.96,
        "quad.waist": 1.02,
        "quad.rump": 1.10,
        "quad.belly": 0.52,
        "quad.section": 2.0,
        "quad.neck_thick": 1.05,
        "quad.neck_taper": 1.05,
        "quad.head_size": 1.10,
        "quad.muzzle_depth": 0.42,
        "quad.muzzle_width": 0.90,
        "quad.jaw": 0.60,
        "quad.eye": 2,
        "quad.ear_shape": "none",
        "quad.tail_len": 0.0,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.95,
        "quad.fore_bend": 0.45,
        "quad.foot": 1.50,
        "quad.under": 0.0,
        "quad.mark": "bars",
        "quad.mark_count": 3,
        "quad.mark_width": 0.24,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "skin_orange",
        "materials.quad_belly": "skin_orange",
        "materials.quad_head": "skin_orange",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_orange",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "forest",
        "herd.size_min": 1,
        "herd.size_max": 5,
        "herd.spread_m": 8.0,
        "herd.despawn_m": 40.0,
        "herd.per_hectare": 3.0,
        "herd.flee_m": 3.0,
        "biomes.rainforest": 1.0,
    },
)


def main(argv: list[str]) -> int:
    force = seedspec.parse_force(argv)
    seedspec.announce(force)
    SPECS.mkdir(parents=True, exist_ok=True)
    written = 0
    for name, notes, patch in SPECIES:
        s = sm.default_spec()
        s, _ = sm.patch(s, BASE)
        s, rep = sm.patch(s, {"name": name, "notes": notes, **patch})
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          width=28):
            written += 1
    print(f"\n{written} of {len(SPECIES)} land-animal specs written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
