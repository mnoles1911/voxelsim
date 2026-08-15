"""Author the first tranche of land animals.

THE OWNER'S OWN LIST FIRST -- kangaroo, deer, squirrel, wild boar, stag, fox,
wild hare, gorilla, zebra, antelope, alpaca, moose, bison, monkey, tiger, large
lizards -- and then the species from `docs/biomes/*.md` that exercise a
mechanism nothing else in the tranche does: a sprawling lizard, a bipedal
meerkat, a spiral-horned kudu, a sloping-backed hyena.

WHAT EVERY ROW HAS TO SAY, AND WHY
----------------------------------
`resolution_cm` is chosen by the house rule and NOT copied from the biome file:
"the coarsest voxel size at which the smallest identifying feature is still
about three voxels across" (`docs/marine-megafauna-research.md` §5.2). Each row
below names the feature that decided it and the arithmetic, because a lattice
with no reason attached is a lattice the next person will change.

Every feature size here is an APPROXIMATE TYPICAL ADULT FIGURE, the same
standing this project's biome files take: `docs/biomes/README.md` §8 says in so
many words that none of those sizes is measured or sourced, that they are good
enough to choose a voxel lattice and not good enough to quote as fact, and that
the reason for the caution is that this project HAS shipped a fabricated
citation. So nothing here cites anything. Where a number matters it is written
as the estimate it is, and `tools/quadprobe.py --lattice` prints what each
species actually comes out at so the choice can be checked against a render
rather than against this comment.

    python tools/seed_quadrupeds.py            # write the ones that do not exist
    python tools/seed_quadrupeds.py --force    # overwrite tuned specs with drafts

`--force` DISCARDS TUNING. `tools/seedspec.py` refuses to overwrite an existing
spec without it, because a seed script is the DRAFT a species started from and
what is on disk is the result of tuning it -- and re-running one has already
silently reverted a finished asset in this repo once.
"""
from __future__ import annotations

import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"

# Shared starting point for a land animal. Everything below is a delta from
# this, so a row is the DIFFERENCES between a species and a generic mammal --
# which is both shorter to read and the only form in which two species can be
# compared by eye.
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
    # Every land animal in this tranche is authored with variation ON. A herd of
    # identical zebra is the single most obvious failure this library can ship,
    # and it is far more obvious than a forest of similar oaks -- you can see
    # eight of them at once and they are the same shape.
    "variation.amount": 1.0,
    "variation.height": 0.09,
    "variation.proportion": 0.14,
    "variation.shape": 0.12,
}

# name -> (notes, patch). Ordered by the group they belong to rather than
# alphabetically, so the differences within a family are visible in one screen.
SPECIES: list[tuple[str, str, dict]] = []


def add(name: str, notes: str, **patch) -> None:
    SPECIES.append((name, notes, patch))


# --- macropods: the bipedal stance ------------------------------------------

add(
    "red-kangaroo",
    "THE FIRST BIPEDAL ANIMAL IN THE LIBRARY, and the one the stance parameter "
    "exists for. It stands on two hind legs and a heavy tail as a tripod, with "
    "small forelimbs held clear of the ground at the chest.\n\n"
    "The tail's carriage angle is NOT authored here. In the bipedal stance the "
    "generator solves it so the tip lands on the ground, because a third leg "
    "that does not reach the floor is not a third leg; `quad.tail_deg` below "
    "is ignored and `tools/quadprobe.py --stance` prints the solved angle and "
    "the gap that is left.\n\n"
    "LATTICE: 2 cm. The ears decide it. They are a long flat blade about 14 cm "
    "long and 5-6 cm wide on a 1.4 m animal, and the width is the smaller "
    "figure, so three voxels across it wants 1.7-2 cm. At 5 cm -- which is what "
    "`01-beach.md` and `02-grassland.md` recommend for the eastern grey -- the "
    "ear is one voxel wide and the animal loses the feature that most says "
    "kangaroo after the stance itself. 2 cm puts the whole animal at 70 voxels "
    "of head-body, which is mid-range for this library.",
    resolution_cm="2",
    **{
        "quad.length_m": 1.40,
        "quad.stance": "bipedal",
        # Hip LOW and shoulder HIGH, and the trunk long enough to span the gap:
        # this is what makes the animal sit up at about 65 degrees rather than
        # lying along the ground. Authored at 0.72 / 0.44 over a trunk of 0.54
        # the tilt came out at 48 degrees and the render was a lizard.
        "quad.shoulder_h": 0.86,
        "quad.hip_h": 0.36,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 72.0,
        "quad.head_deg": -8.0,
        "quad.depth": 0.46,
        "quad.width": 0.60,
        "quad.chest": 0.80,
        "quad.waist": 0.95,
        # HINDQUARTERS HEAVIER THAN THE CHEST, which is the other half of the
        # silhouette and the opposite of every other animal in this file.
        "quad.rump": 1.30,
        "quad.belly": 0.55,
        "quad.hump": 0.0,
        "quad.neck_thick": 0.42,
        "quad.neck_taper": 0.62,
        "quad.head_size": 0.85,
        "quad.muzzle_depth": 0.55,
        "quad.muzzle_width": 0.50,
        "quad.jaw": 0.35,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.15,
        "quad.ear_width": 0.40,
        "quad.ear_deg": 74.0,
        "quad.ear_back": 0.30,
        "quad.horn_shape": "none",
        # The counterweight. Thicker at the base than the animal's own neck,
        # which is a real fact about the species and is why `tail_thick` runs
        # as high as it does.
        "quad.tail_len": 0.70,
        "quad.tail_thick": 0.68,
        "quad.tail_taper": 0.24,
        "quad.tail_arc": -0.08,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.88,
        "quad.foot": 2.10,
        "quad.fore_bend": 0.55,
        "quad.fore_reach": 0.34,
        "quad.under": 0.34,
        "quad.mark": "none",
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        # A red kangaroo is the strongest sexual dimorphism in this tranche:
        # the male is rusty red and much larger, the female blue-grey.
        "quad.sex_length": 1.30,
        "herd.cover": "open",
        "herd.size_min": 2,
        "herd.size_max": 14,
        "herd.spread_m": 45.0,
        "herd.per_hectare": 0.6,
        "herd.flee_m": 45.0,
        "biomes.desert": 0.8,
        "biomes.grassland": 0.7,
        "biomes.savanna": 0.4,
        "biomes.beach": 0.2,
    },
)

add(
    "eastern-grey-kangaroo",
    "The same stance as the red kangaroo and a different animal: greyer, "
    "shorter-faced, less upright, and a coastal and woodland-edge species where "
    "the red is an animal of open arid country.\n\n"
    "LATTICE: 2 cm, for the ears, exactly as the red kangaroo. Held at 5 cm to "
    "match the biome files it would be a grey lozenge on two legs.",
    resolution_cm="2",
    **{
        "quad.length_m": 1.30,
        "quad.stance": "bipedal",
        "quad.shoulder_h": 0.80,
        "quad.hip_h": 0.38,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 64.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.46,
        "quad.width": 0.62,
        "quad.chest": 0.82,
        "quad.waist": 0.96,
        "quad.rump": 1.24,
        "quad.belly": 0.55,
        "quad.neck_thick": 0.44,
        "quad.head_size": 0.88,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.48,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.145,
        "quad.ear_width": 0.42,
        "quad.ear_deg": 72.0,
        "quad.tail_len": 0.70,
        "quad.tail_thick": 0.64,
        "quad.tail_taper": 0.24,
        "quad.leg_thick": 0.23,
        "quad.hock": 0.86,
        "quad.foot": 2.00,
        "quad.fore_reach": 0.36,
        "quad.under": 0.36,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "skin_pale",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "quad.sex_length": 1.25,
        "herd.cover": "edge",
        "herd.size_min": 2,
        "herd.size_max": 10,
        "herd.per_hectare": 0.7,
        "herd.flee_m": 40.0,
        "biomes.grassland": 0.8,
        "biomes.temperate_forest": 0.4,
        "biomes.beach": 0.5,
        "biomes.savanna": 0.3,
    },
)

add(
    "meerkat",
    "A second bipedal species and a deliberately different one: a 30 cm animal "
    "standing vertical on its hind legs with a thin straight tail as a prop, "
    "where the kangaroos are 1.3 m animals sitting back on a heavy one. If the "
    "stance only worked at kangaroo proportions this is what would show it.\n\n"
    "LATTICE: 1 cm. The dark eye patches and the faint back bands are both "
    "about 2 cm on a 30 cm animal, so nothing coarser holds them; at 1 cm the "
    "body is 30 voxels, which is the bottom of the band this library uses and "
    "the same place the small fish sit.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.30,
        "quad.stance": "bipedal",
        "quad.shoulder_h": 1.05,
        "quad.hip_h": 0.28,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.16,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 84.0,
        "quad.head_deg": 6.0,
        "quad.depth": 0.36,
        "quad.width": 0.66,
        "quad.chest": 0.92,
        "quad.waist": 0.96,
        "quad.rump": 0.98,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.52,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.46,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.035,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 20.0,
        "quad.tail_len": 0.68,
        "quad.tail_thick": 0.16,
        "quad.tail_taper": 0.45,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.55,
        "quad.foot": 1.10,
        "quad.fore_reach": 0.40,
        "quad.under": 0.42,
        "quad.mark": "bars",
        "quad.mark_count": 7,
        "quad.mark_width": 0.16,
        "quad.mark_strength": 0.55,
        "quad.tail_tip": 0.22,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "skin_pale",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "open",
        "herd.size_min": 4,
        "herd.size_max": 30,
        "herd.spread_m": 20.0,
        "herd.per_hectare": 3.0,
        "herd.flee_m": 15.0,
        "biomes.savanna": 0.9,
        "biomes.desert": 0.6,
        "biomes.grassland": 0.3,
    },
)

# --- dogs and cats -----------------------------------------------------------

add(
    "red-fox",
    "The default species for this kind in the browser, and chosen for that "
    "because everything about it is mid-range: a mid-sized quadruped standing "
    "on four legs with pointed ears, a long brush and no headgear.\n\n"
    "LATTICE: 2 cm. The ears are the smallest identifying feature at roughly "
    "5 cm across the base on a 0.7 m animal, which wants 1.7 cm; 2 cm gives 2.5 "
    "voxels, just under the rule and the coarsest tier that keeps them. The "
    "white brush tip is 10-12 cm and is comfortable at 2 cm.",
    **{
        "quad.length_m": 0.70,
        "quad.shoulder_h": 0.57,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.55,
        "quad.neck_frac": 0.15,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 12.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.34,
        "quad.width": 0.58,
        "quad.chest": 1.00,
        "quad.waist": 0.88,
        "quad.rump": 0.94,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.50,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.40,
        "quad.jaw": 0.30,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.105,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 78.0,
        "quad.ear_back": 0.10,
        # The brush: as thick as the body, carried low and straight, tipped
        # white. Three of the four rows on this animal that anyone will notice.
        "quad.tail_len": 0.62,
        "quad.tail_thick": 0.52,
        "quad.tail_taper": 0.80,
        "quad.tail_deg": -22.0,
        "quad.tail_tip": 0.20,
        "quad.leg_thick": 0.19,
        "quad.hock": 0.42,
        "quad.fore_bend": 0.22,
        "quad.foot": 0.85,
        "quad.under": 0.34,
        "quad.stocking": 0.30,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "edge",
        "herd.size_max": 2,
        "herd.per_hectare": 0.15,
        "herd.flee_m": 60.0,
        "biomes.temperate_forest": 0.9,
        "biomes.grassland": 0.8,
        "biomes.taiga": 0.6,
        "biomes.tundra_alpine": 0.3,
        "biomes.beach": 0.2,
    },
)

add(
    "grey-wolf",
    "A dog built for distance: deep narrow chest, long legs, straight bushy "
    "tail carried low, and a grizzled saddle over paler flanks. Against the fox "
    "beside it on a sheet the differences are all proportion -- longer legs, "
    "less tail, a heavier muzzle and a level back.\n\n"
    "LATTICE: 2 cm. The saddle boundary and the ears are both around 6 cm on a "
    "1.3 m animal, which wants 2 cm exactly.",
    **{
        "quad.length_m": 1.25,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 0.95,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.18,
        "quad.head_frac": 0.15,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 16.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.36,
        "quad.width": 0.54,
        "quad.chest": 1.05,
        "quad.waist": 0.86,
        "quad.rump": 0.92,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.62,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.44,
        "quad.jaw": 0.45,
        "quad.mane": 0.16,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.62,
        "quad.ear_deg": 80.0,
        "quad.tail_len": 0.40,
        "quad.tail_thick": 0.38,
        "quad.tail_taper": 0.65,
        "quad.tail_deg": -34.0,
        "quad.tail_tip": 0.22,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.40,
        "quad.fore_bend": 0.18,
        "quad.under": 0.36,
        "quad.cape": 0.30,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "plume_slate",
        "quad.sex_length": 1.10,
        "herd.cover": "any",
        "herd.size_min": 2,
        "herd.size_max": 8,
        "herd.spread_m": 40.0,
        "herd.per_hectare": 0.05,
        "herd.flee_m": 150.0,
        "biomes.taiga": 0.9,
        "biomes.temperate_forest": 0.7,
        "biomes.grassland": 0.5,
        "biomes.tundra_alpine": 0.5,
    },
)

add(
    "eurasian-lynx",
    "The species the TUFTED ear shape exists for: a short-bodied, very "
    "long-legged cat with a bobbed black-tipped tail and tall black ear tufts. "
    "It is also the clearest test that `quad.tail_len` reaches the bottom of "
    "its range -- at 0.14 the tail is a stump and the animal has to still read "
    "as a cat.\n\n"
    "LATTICE: 2 cm. The ear tuft is the smallest thing that matters at roughly "
    "5 cm of black tip on a 1 m animal.",
    **{
        "quad.length_m": 1.00,
        "quad.shoulder_h": 0.66,
        "quad.hip_h": 1.04,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.1,
        "quad.neck_deg": 10.0,
        "quad.depth": 0.34,
        "quad.width": 0.60,
        "quad.chest": 0.98,
        "quad.waist": 0.90,
        "quad.rump": 1.00,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.62,
        "quad.head_size": 1.15,
        "quad.muzzle_depth": 0.70,
        "quad.muzzle_width": 0.72,
        "quad.jaw": 0.40,
        "quad.ear_shape": "tufted",
        "quad.ear_len": 0.085,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 80.0,
        "quad.tail_len": 0.16,
        "quad.tail_thick": 0.30,
        "quad.tail_taper": 0.85,
        "quad.tail_deg": -20.0,
        "quad.tail_tip": 0.40,
        "quad.leg_thick": 0.22,
        "quad.hock": 0.38,
        "quad.foot": 1.30,
        "quad.under": 0.34,
        "quad.mark": "spots",
        "quad.mark_count": 14,
        "quad.mark_width": 0.10,
        "quad.mark_strength": 0.60,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "forest",
        "herd.size_max": 1,
        "herd.per_hectare": 0.02,
        "herd.flee_m": 120.0,
        "biomes.taiga": 0.9,
        "biomes.temperate_forest": 0.6,
        "biomes.tundra_alpine": 0.3,
    },
)

add(
    "bengal-tiger",
    "The species the BARS marking exists for, and the reason that marking is "
    "the fish generator's 'vertical bars' rather than a second implementation: "
    "narrow black stripes wrapping a cylinder is exactly what a perch carries, "
    "floor rules and all.\n\n"
    "LATTICE: 2 cm. The stripes are the smallest identifying feature at roughly "
    "5 cm wide on a 2 m animal, so three voxels across one wants 1.7 cm and 2 cm "
    "gives 2.5. At 5 cm a stripe is one voxel and the animal is an orange cat.",
    **{
        "quad.length_m": 2.00,
        "quad.shoulder_h": 0.48,
        "quad.hip_h": 0.96,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.11,
        "quad.neck_deg": 8.0,
        "quad.depth": 0.38,
        "quad.width": 0.62,
        "quad.chest": 1.10,
        "quad.waist": 0.92,
        "quad.rump": 0.98,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.72,
        "quad.head_size": 1.20,
        "quad.muzzle_depth": 0.72,
        "quad.muzzle_width": 0.80,
        "quad.jaw": 0.50,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.045,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 55.0,
        "quad.tail_len": 0.62,
        "quad.tail_thick": 0.28,
        "quad.tail_taper": 0.80,
        "quad.tail_deg": -28.0,
        "quad.tail_arc": 0.25,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.36,
        "quad.foot": 1.30,
        "quad.under": 0.32,
        "quad.mark": "bars",
        "quad.mark_count": 18,
        "quad.mark_width": 0.17,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "skin_orange",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "skin_orange",
        "materials.quad_leg": "skin_orange",
        "materials.quad_tail": "skin_orange",
        "materials.quad_mark": "skin_dark",
        "quad.sex_length": 1.15,
        "herd.cover": "forest",
        "herd.size_max": 1,
        "herd.per_hectare": 0.01,
        "herd.flee_m": 100.0,
        "biomes.rainforest": 0.9,
        "biomes.temperate_forest": 0.3,
        "biomes.savanna": 0.2,
    },
)

add(
    "spotted-hyena",
    "The species that most needs a SLOPING BACK: heavy high shoulders falling "
    "away to low hindquarters. `quad.hip_h` at 0.74 is the whole animal, and it "
    "is the clearest demonstration in this tranche that the two height rows "
    "carry more signal than any colour row.\n\n"
    "LATTICE: 2 cm. The dark blotches are 4-6 cm on a 1.4 m animal.",
    **{
        "quad.length_m": 1.35,
        "quad.shoulder_h": 0.63,
        "quad.hip_h": 0.74,
        "quad.trunk_frac": 0.55,
        "quad.neck_frac": 0.17,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 6.0,
        "quad.head_deg": -14.0,
        "quad.depth": 0.40,
        "quad.width": 0.56,
        "quad.chest": 1.12,
        "quad.waist": 0.90,
        "quad.rump": 0.82,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.78,
        "quad.head_size": 1.10,
        "quad.muzzle_depth": 0.62,
        "quad.muzzle_width": 0.56,
        "quad.jaw": 0.85,
        "quad.mane": 0.28,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 70.0,
        "quad.tail_len": 0.26,
        "quad.tail_thick": 0.34,
        "quad.tail_taper": 0.75,
        "quad.tail_deg": -40.0,
        "quad.tail_tip": 0.35,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.40,
        "quad.under": 0.0,
        "quad.mark": "blotch",
        "quad.mark_count": 9,
        "quad.mark_width": 0.22,
        "quad.mark_strength": 0.85,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "open",
        "herd.size_min": 2,
        "herd.size_max": 12,
        "herd.per_hectare": 0.08,
        "herd.flee_m": 80.0,
        "biomes.savanna": 0.9,
        "biomes.grassland": 0.4,
        "biomes.desert": 0.2,
    },
)

# --- bovids and the hump -----------------------------------------------------

add(
    "american-bison",
    "The largest thing in this tranche and the species `quad.hump` exists for. "
    "Its forequarter hump stands roughly twice the height of its hips, it wears "
    "a shaggy dark cape over the shoulders against a bare rear, and it carries "
    "short pale horns curving out and up.\n\n"
    "LATTICE: 5 cm. `docs/biomes/README.md` §6 names the shoulder hump as this "
    "species' smallest identifying feature, and at roughly 50 cm of relief on a "
    "2.8 m animal that is 10 voxels at 5 cm -- comfortably past the rule. The "
    "horns are the thing that does NOT survive: about 8 cm at the base, which "
    "is 1.6 voxels, so they are drawn at the one-voxel floor and read as a "
    "shape rather than as a thickness. That is accepted rather than fixed by "
    "moving the whole animal to 2 cm, because the horns are not what identifies "
    "a bison and the hump is.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.80,
        "quad.shoulder_h": 0.58,
        "quad.hip_h": 0.86,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": -20.0,
        "quad.head_deg": -26.0,
        "quad.depth": 0.46,
        "quad.width": 0.66,
        "quad.chest": 1.10,
        "quad.waist": 0.94,
        "quad.rump": 0.78,
        "quad.belly": 0.52,
        "quad.section": 2.8,
        "quad.hump": 0.36,
        "quad.hump_at": 0.20,
        "quad.neck_thick": 0.90,
        "quad.neck_taper": 0.70,
        "quad.head_size": 1.15,
        "quad.muzzle_depth": 0.70,
        "quad.muzzle_width": 0.78,
        "quad.jaw": 0.55,
        "quad.mane": 0.30,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.045,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 30.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.12,
        "quad.horn_thick": 0.36,
        "quad.horn_spread": 1.10,
        "quad.horn_curl": 0.55,
        "quad.tail_len": 0.20,
        "quad.tail_thick": 0.16,
        "quad.tail_taper": 0.45,
        "quad.tail_deg": -60.0,
        "quad.tail_tuft": 0.55,
        "quad.leg_thick": 0.26,
        "quad.hock": 0.28,
        "quad.fore_bend": 0.14,
        "quad.foot": 1.10,
        "quad.under": 0.20,
        "quad.cape": 0.30,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "skin_brown",
        "materials.quad_horn": "skin_pale",
        "quad.sex_length": 1.20,
        "herd.cover": "open",
        "herd.size_min": 4,
        "herd.size_max": 60,
        "herd.spread_m": 120.0,
        "herd.despawn_m": 400.0,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 70.0,
        "biomes.grassland": 1.0,
        "biomes.taiga": 0.3,
        "biomes.temperate_forest": 0.2,
    },
)

add(
    "plains-zebra",
    "The first species in this library whose MARKING sets its lattice rather "
    "than its body doing so, which `docs/biomes/README.md` §7 predicted it "
    "would be.\n\n"
    "LATTICE: 2 cm. The stripes are the smallest identifying feature at "
    "roughly 5-8 cm wide on a 2.3 m animal; three voxels across the narrowest "
    "of those wants 1.7 cm, and 2 cm gives 2.5-4. At the 5 cm the body alone "
    "would be happy with, a stripe is one voxel and the animal is grey. The "
    "biome file flags this row as one of its weakest numbers, so it is worth "
    "checking against a render rather than against this note.",
    **{
        "quad.length_m": 2.30,
        "quad.shoulder_h": 0.56,
        "quad.hip_h": 0.97,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.22,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 34.0,
        "quad.head_deg": -18.0,
        "quad.depth": 0.42,
        "quad.width": 0.58,
        "quad.chest": 1.02,
        "quad.waist": 0.98,
        "quad.rump": 1.02,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.62,
        "quad.neck_taper": 0.62,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.62,
        "quad.muzzle_width": 0.50,
        "quad.jaw": 0.40,
        # The stiff upright brush, which is the one thing that separates a
        # zebra's outline from a pony's before the stripes are visible.
        "quad.mane": 0.34,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 76.0,
        "quad.tail_len": 0.24,
        "quad.tail_thick": 0.12,
        "quad.tail_taper": 0.55,
        "quad.tail_deg": -55.0,
        "quad.tail_tuft": 0.60,
        "quad.leg_thick": 0.17,
        "quad.hock": 0.30,
        "quad.fore_bend": 0.12,
        "quad.under": 0.0,
        "quad.mark": "bars",
        "quad.mark_count": 25,
        "quad.mark_width": 0.28,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "plume_white",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_white",
        "materials.quad_leg": "plume_white",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_min": 3,
        "herd.size_max": 40,
        "herd.spread_m": 90.0,
        "herd.despawn_m": 400.0,
        "herd.per_hectare": 0.8,
        "herd.flee_m": 90.0,
        "biomes.savanna": 1.0,
        "biomes.grassland": 0.5,
    },
)

add(
    "gemsbok",
    "The SWEEP horn: two very long, straight, near-parallel spears swept back "
    "over the spine. Both sexes carry them, so `quad.sex_horn` stays at 1.0 -- "
    "which is worth one spec on its own, because every other horned animal in "
    "this tranche uses that row to take the headgear away from the female and "
    "a reader could reasonably conclude that is what it is for.\n\n"
    "LATTICE: 2 cm. The black facial mask and the black side stripe are both "
    "5-7 cm on a 2 m animal.",
    **{
        "quad.length_m": 2.00,
        "quad.shoulder_h": 0.60,
        "quad.hip_h": 0.98,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.22,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 34.0,
        "quad.head_deg": -12.0,
        "quad.depth": 0.42,
        "quad.width": 0.56,
        "quad.chest": 1.02,
        "quad.waist": 0.96,
        "quad.rump": 1.00,
        "quad.belly": 0.50,
        "quad.section": 2.6,
        "quad.neck_thick": 0.58,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.55,
        "quad.muzzle_width": 0.45,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.60,
        "quad.ear_deg": 60.0,
        "quad.horn_shape": "sweep",
        "quad.horn_len": 0.42,
        "quad.horn_thick": 0.09,
        "quad.horn_spread": 0.16,
        "quad.horn_curl": 0.52,
        "quad.tail_len": 0.26,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": -62.0,
        "quad.tail_tip": 0.55,
        "quad.leg_thick": 0.16,
        "quad.hock": 0.32,
        "quad.under": 0.26,
        "quad.stocking": 0.26,
        "quad.mark": "flankstripe",
        "quad.mark_width": 0.14,
        "quad.mark_strength": 1.0,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_white",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        "quad.sex_horn": 1.0,
        "herd.cover": "open",
        "herd.size_min": 2,
        "herd.size_max": 18,
        "herd.per_hectare": 0.5,
        "herd.flee_m": 110.0,
        "biomes.desert": 0.8,
        "biomes.savanna": 0.7,
        "biomes.grassland": 0.3,
    },
)

add(
    "greater-kudu",
    "The SPIRAL horn: two and a half open turns, on a tall grey-brown antelope "
    "with thin white flank stripes and a throat fringe. Female kudu carry no "
    "horns at all, so `quad.sex_horn` is 0 -- the present-or-absent mechanism, "
    "not a scale.\n\n"
    "LATTICE: 2 cm. The white flank stripes are the smallest feature at 3-4 cm "
    "on a 2.3 m animal, which strictly wants 1 cm. Held at 2 cm and the stripes "
    "authored WIDER than life so they survive; this is the class (b) fix from "
    "`docs/biomes/README.md` §6 and it is written down here so nobody corrects "
    "the stripe width back.",
    **{
        "quad.length_m": 2.30,
        "quad.shoulder_h": 0.61,
        "quad.hip_h": 0.94,
        "quad.trunk_frac": 0.5,
        "quad.neck_frac": 0.25,
        "quad.head_frac": 0.12,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 40.0,
        "quad.head_deg": -8.0,
        "quad.depth": 0.42,
        "quad.width": 0.52,
        "quad.chest": 1.00,
        "quad.waist": 0.96,
        "quad.rump": 1.02,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.56,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.44,
        "quad.dewlap": 0.42,
        "quad.mane": 0.18,
        "quad.ear_shape": "fan",
        "quad.ear_len": 0.09,
        "quad.ear_width": 0.95,
        "quad.ear_deg": 34.0,
        "quad.horn_shape": "spiral",
        "quad.horn_len": 0.44,
        "quad.horn_thick": 0.11,
        "quad.horn_spread": 0.42,
        "quad.horn_curl": 0.62,
        "quad.tail_len": 0.22,
        "quad.tail_thick": 0.16,
        "quad.tail_taper": 0.65,
        "quad.tail_deg": -58.0,
        "quad.tail_tip": 0.40,
        "quad.leg_thick": 0.15,
        "quad.hock": 0.34,
        "quad.under": 0.24,
        "quad.mark": "bars",
        "quad.mark_count": 8,
        "quad.mark_width": 0.11,
        "quad.mark_strength": 0.95,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_slate",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "plume_white",
        "quad.sex_horn": 0.0,
        "quad.sex_length": 1.20,
        "herd.cover": "edge",
        "herd.size_min": 2,
        "herd.size_max": 10,
        "herd.per_hectare": 0.3,
        "herd.flee_m": 120.0,
        "biomes.savanna": 0.9,
        "biomes.grassland": 0.3,
    },
)

add(
    "impala",
    "Light, leggy and mid-sized, with a graded coat -- rich red-brown above, "
    "tan through the flanks, white below -- and lyre-shaped horns on the male "
    "only. The species that shows what `quad.under` alone does when there is no "
    "marking on top of it.\n\n"
    "LATTICE: 2 cm. The black tail-side stripes are about 3 cm on a 1.4 m "
    "animal, and are the one feature this lattice does not hold; they are left "
    "out rather than drawn at one voxel.",
    **{
        "quad.length_m": 1.40,
        "quad.shoulder_h": 0.64,
        "quad.hip_h": 1.02,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.22,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 42.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.38,
        "quad.width": 0.50,
        "quad.chest": 0.98,
        "quad.waist": 0.92,
        "quad.rump": 1.04,
        "quad.belly": 0.48,
        "quad.neck_thick": 0.46,
        "quad.head_size": 0.92,
        "quad.muzzle_depth": 0.48,
        "quad.muzzle_width": 0.40,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.10,
        "quad.ear_width": 0.50,
        "quad.ear_deg": 62.0,
        "quad.horn_shape": "curve",
        "quad.horn_len": 0.34,
        "quad.horn_thick": 0.09,
        "quad.horn_spread": 0.55,
        "quad.horn_curl": 0.45,
        "quad.tail_len": 0.22,
        "quad.tail_thick": 0.14,
        "quad.tail_taper": 0.60,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.13,
        "quad.hock": 0.38,
        "quad.under": 0.34,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_white",
        "materials.quad_mark": "skin_dark",
        "quad.sex_horn": 0.0,
        "herd.cover": "edge",
        "herd.size_min": 4,
        "herd.size_max": 50,
        "herd.spread_m": 70.0,
        "herd.per_hectare": 1.6,
        "herd.flee_m": 90.0,
        "biomes.savanna": 1.0,
        "biomes.grassland": 0.4,
    },
)

add(
    "alpaca",
    "The owner asked for an alpaca by name, so an alpaca is what this is -- but "
    "it is worth saying plainly rather than quietly substituting: the alpaca is "
    "DOMESTICATED, and `02-grassland.md` records that the honest wild entries "
    "for this clade are the guanaco and the vicuna. Placed in the world it will "
    "read as a farm animal on a wild hillside unless something puts it near a "
    "settlement. The guanaco is the same build with a longer neck, a grey head "
    "and a harder line to a white belly, and is a two-line change from here if "
    "the wild version is wanted instead.\n\n"
    "LATTICE: 2 cm. The ears decide it at roughly 12 cm long and 5 cm wide, "
    "and it is the WIDTH that counts -- three voxels across it wants 1.7 cm.\n\n"
    "5 cm was tried first, on the reasoning that nothing about an alpaca is "
    "small, and it put a 1.4 m animal on 28 voxels of head-body -- the bottom "
    "of the whole library's range, below every fish in it, and with the ears "
    "one voxel wide. This is the general trap in the coarsest-lattice rule: it "
    "is the coarsest at which the FEATURE still reads, and reading the body's "
    "size instead gives an animal made of eight boxes.",
    resolution_cm="2",
    **{
        "quad.length_m": 1.40,
        "quad.shoulder_h": 0.62,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.48,
        "quad.neck_frac": 0.3,
        "quad.head_frac": 0.12,
        "quad.muzzle_frac": 0.1,
        "quad.neck_deg": 70.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.46,
        "quad.width": 0.66,
        "quad.chest": 1.00,
        "quad.waist": 1.02,
        "quad.rump": 1.00,
        "quad.belly": 0.52,
        "quad.section": 2.5,
        "quad.neck_thick": 0.44,
        "quad.neck_taper": 0.60,
        "quad.head_size": 0.90,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.46,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.085,
        "quad.ear_width": 0.45,
        "quad.ear_deg": 82.0,
        "quad.tail_len": 0.14,
        "quad.tail_thick": 0.22,
        "quad.tail_taper": 0.55,
        "quad.tail_deg": -30.0,
        "quad.leg_thick": 0.17,
        "quad.hock": 0.30,
        "quad.under": 0.30,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "herd.cover": "open",
        "herd.size_min": 3,
        "herd.size_max": 16,
        "herd.per_hectare": 1.2,
        "herd.flee_m": 30.0,
        "biomes.tundra_alpine": 0.8,
        "biomes.grassland": 0.6,
    },
)

# --- deer: the antler question ----------------------------------------------

add(
    "moose",
    "The PALMATE rack, and the species `docs/biomes/README.md` §6 says to build "
    "before any round-tined deer: flat blades 10-15 cm across survive a coarse "
    "lattice where round tines do not. Also the humped withers, the very long "
    "legs, the pendulous bell under the throat and the drooping muzzle -- four "
    "separate mechanisms on one animal.\n\n"
    "LATTICE: 5 cm. The palm blade at 10-15 cm is 2-3 voxels, which is the rule "
    "met at the coarsest tier that meets it. This is the species that decides "
    "whether the palmate/branched split was worth making, and the answer should "
    "come from a render of this beside `red-deer-stag`.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.70,
        "quad.shoulder_h": 0.70,
        "quad.hip_h": 0.88,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.18,
        "quad.head_frac": 0.14,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 24.0,
        "quad.head_deg": -26.0,
        "quad.depth": 0.42,
        "quad.width": 0.52,
        "quad.chest": 1.06,
        "quad.waist": 0.92,
        "quad.rump": 0.88,
        "quad.belly": 0.50,
        "quad.hump": 0.30,
        "quad.hump_at": 0.14,
        "quad.neck_thick": 0.68,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.85,
        "quad.muzzle_width": 0.60,
        "quad.muzzle_drop": 0.55,
        "quad.jaw": 0.55,
        "quad.dewlap": 0.55,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 40.0,
        "quad.horn_shape": "palmate",
        "quad.horn_len": 0.30,
        "quad.horn_thick": 0.22,
        "quad.horn_spread": 0.95,
        "quad.horn_tines": 4,
        "quad.tail_len": 0.05,
        "quad.tail_thick": 0.18,
        "quad.tail_deg": -55.0,
        "quad.leg_thick": 0.17,
        "quad.hock": 0.34,
        "quad.under": 0.18,
        "quad.stocking": 0.0,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "plume_grey",
        "quad.sex_horn": 0.0,
        "quad.sex_length": 1.15,
        "herd.cover": "forest",
        "herd.size_max": 2,
        "herd.per_hectare": 0.06,
        "herd.despawn_m": 350.0,
        "herd.flee_m": 90.0,
        "biomes.taiga": 1.0,
        "biomes.temperate_forest": 0.5,
        "biomes.tundra_alpine": 0.3,
    },
)

add(
    "red-deer-stag",
    "The BRANCHED rack, and the species the biome survey says has no honest "
    "lattice: at life size a red deer's main beam is 3-4 cm and a tine tip 1-2, "
    "so at 5 cm the antlers disappear and the stag becomes a hind, and at 2 cm a "
    "beam is two voxels and a tine one -- still under the three-voxel rule.\n\n"
    "SO THE RACK IS AUTHORED THICKER THAN LIFE, AND THIS NOTE IS WHY. "
    "`quad.horn_thick` here is 0.13 against a life-size figure near 0.07; the "
    "beam comes out around 3 voxels instead of 1.5. That is the class (b) fix "
    "recorded in `docs/biomes/README.md` §6 -- author the feature above life "
    "size and write down the reason -- and it is already house practice for "
    "four birds and a clownfish. DO NOT 'correct' this back to a life-size "
    "thickness: the render at that setting is a hind.\n\n"
    "LATTICE: 2 cm. 2.0 m of head-body at 2 cm is 100 voxels, which is where "
    "the tine tips reach one voxel and exist at all.",
    **{
        "quad.length_m": 2.00,
        "quad.shoulder_h": 0.63,
        "quad.hip_h": 0.96,
        "quad.trunk_frac": 0.51,
        "quad.neck_frac": 0.24,
        "quad.head_frac": 0.12,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 38.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.40,
        "quad.width": 0.52,
        "quad.chest": 1.04,
        "quad.waist": 0.94,
        "quad.rump": 1.00,
        "quad.belly": 0.50,
        # The maned neck of a stag in rut, which is a real proportion
        # difference from the hind and not only a colour one.
        "quad.neck_thick": 0.72,
        "quad.neck_taper": 0.55,
        "quad.mane": 0.22,
        "quad.head_size": 0.92,
        "quad.muzzle_depth": 0.52,
        "quad.muzzle_width": 0.44,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.075,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 58.0,
        "quad.horn_shape": "branched",
        "quad.horn_len": 0.42,
        "quad.horn_thick": 0.13,
        "quad.horn_spread": 0.60,
        "quad.horn_tines": 5,
        "quad.tail_len": 0.08,
        "quad.tail_thick": 0.16,
        "quad.tail_deg": -50.0,
        "quad.leg_thick": 0.15,
        "quad.hock": 0.38,
        "quad.under": 0.24,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_brown",
        "materials.quad_leg": "skin_brown",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_dark",
        # PRESENT ON THE MALE ONLY. A hind has no antlers, which is a part that
        # is absent and not a small one -- `quad._sex_present` is the mechanism
        # and this is the row that exercises it.
        "quad.sex_horn": 0.0,
        "quad.sex_mane": 2.20,
        "quad.sex_length": 1.15,
        "herd.cover": "edge",
        "herd.size_min": 1,
        "herd.size_max": 12,
        "herd.spread_m": 60.0,
        "herd.despawn_m": 320.0,
        "herd.per_hectare": 0.25,
        "herd.flee_m": 180.0,
        "biomes.temperate_forest": 0.9,
        "biomes.taiga": 0.6,
        "biomes.grassland": 0.4,
        "biomes.tundra_alpine": 0.3,
    },
)

add(
    "fallow-deer",
    "Mid-sized, boldly white-spotted on chestnut, with palmate antlers. Beside "
    "the moose it shows the same rack shape at a quarter of the size; beside "
    "the red deer stag it shows palmate against branched at nearly the same "
    "body length.\n\n"
    "LATTICE: 2 cm, and this is a compromise that should be said out loud. The "
    "PALM is happy at 5 cm -- that is the whole point of a palmate rack. The "
    "SPOTS are about 3 cm on a 1.5 m animal and want 1 cm, which would make the "
    "body 150 voxels. 2 cm splits the difference: the palm is 5-7 voxels across "
    "and the spots are drawn at roughly 2 voxels through `quad.mark_width`, "
    "which is above the two-voxel floor and below the three-voxel rule. The "
    "spots are therefore COARSER THAN LIFE on purpose.",
    **{
        "quad.length_m": 1.50,
        "quad.shoulder_h": 0.63,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.52,
        "quad.neck_frac": 0.22,
        "quad.head_frac": 0.13,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": 40.0,
        "quad.head_deg": -8.0,
        "quad.depth": 0.40,
        "quad.width": 0.52,
        "quad.chest": 1.00,
        "quad.waist": 0.94,
        "quad.rump": 1.02,
        "quad.belly": 0.50,
        "quad.neck_thick": 0.52,
        "quad.head_size": 0.92,
        "quad.muzzle_depth": 0.50,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.085,
        "quad.ear_width": 0.55,
        "quad.ear_deg": 55.0,
        "quad.horn_shape": "palmate",
        "quad.horn_len": 0.34,
        "quad.horn_thick": 0.16,
        "quad.horn_spread": 0.70,
        "quad.horn_tines": 4,
        "quad.tail_len": 0.14,
        "quad.tail_thick": 0.14,
        "quad.tail_deg": -46.0,
        "quad.tail_tip": 0.35,
        "quad.leg_thick": 0.14,
        "quad.hock": 0.38,
        "quad.under": 0.26,
        "quad.mark": "dapple",
        "quad.mark_count": 16,
        "quad.mark_width": 0.22,
        "quad.mark_strength": 0.75,
        "materials.quad_back": "plume_rufous",
        "materials.quad_belly": "skin_pale",
        "materials.quad_head": "plume_rufous",
        "materials.quad_leg": "plume_rufous",
        "materials.quad_tail": "plume_rufous",
        "materials.quad_mark": "plume_white",
        "quad.sex_horn": 0.0,
        "quad.sex_length": 1.15,
        "herd.cover": "edge",
        "herd.size_min": 3,
        "herd.size_max": 24,
        "herd.spread_m": 50.0,
        "herd.per_hectare": 0.7,
        "herd.flee_m": 120.0,
        "biomes.temperate_forest": 0.9,
        "biomes.grassland": 0.5,
    },
)

# --- pigs, bears, primates ---------------------------------------------------

add(
    "wild-boar",
    "A wedge: heaviest at the shoulder and falling away to low hips, head "
    "carried below the line of the back, short legs, and a bristle crest along "
    "the spine. It is the clearest case in this tranche of a species whose "
    "whole identity is the two height rows plus one crest.\n\n"
    "LATTICE: 2 cm. The bristle crest is 8-10 cm on a 1.4 m animal, so three "
    "voxels wants 3 cm and 2 cm is the coarsest tier at or under it. "
    "`02-grassland.md` recommends 5 cm and `07-taiga.md` recommends 2; at 5 cm "
    "the crest is two voxels and the tusks vanish entirely.",
    **{
        "quad.length_m": 1.40,
        "quad.shoulder_h": 0.60,
        "quad.hip_h": 0.82,
        "quad.trunk_frac": 0.58,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.13,
        "quad.neck_deg": -6.0,
        "quad.head_deg": -16.0,
        "quad.depth": 0.46,
        "quad.width": 0.58,
        "quad.chest": 1.12,
        "quad.waist": 0.94,
        "quad.rump": 0.80,
        "quad.belly": 0.54,
        "quad.section": 2.5,
        "quad.hump": 0.20,
        "quad.hump_at": 0.16,
        "quad.neck_thick": 0.86,
        "quad.neck_taper": 0.78,
        "quad.head_size": 1.00,
        "quad.muzzle_depth": 0.55,
        "quad.muzzle_width": 0.48,
        "quad.jaw": 0.45,
        "quad.mane": 0.26,
        "quad.ear_shape": "pointed",
        "quad.ear_len": 0.065,
        "quad.ear_width": 0.65,
        "quad.ear_deg": 62.0,
        "quad.tail_len": 0.20,
        "quad.tail_thick": 0.12,
        "quad.tail_taper": 0.60,
        "quad.tail_deg": -55.0,
        "quad.tail_tuft": 0.35,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.34,
        "quad.foot": 0.85,
        "quad.under": 0.18,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_grey",
        "quad.sex_length": 1.15,
        "herd.cover": "forest",
        "herd.size_min": 2,
        "herd.size_max": 14,
        "herd.spread_m": 30.0,
        "herd.per_hectare": 0.4,
        "herd.flee_m": 70.0,
        "biomes.temperate_forest": 1.0,
        "biomes.grassland": 0.5,
        "biomes.taiga": 0.4,
        "biomes.beach": 0.2,
    },
)

add(
    "brown-bear",
    "Bulky, with a pronounced muscular shoulder hump, a dished face, small "
    "round ears set wide and low, a very short tail, and a flat-footed walk. "
    "The hump is the same mechanism the bison uses at half the strength, which "
    "is the point of having it as a parameter rather than as a bison-shaped "
    "special case.\n\n"
    "LATTICE: 5 cm. Nothing on a brown bear is small: the identifying features "
    "are the hump, the face profile and the bulk, all of them tens of "
    "centimetres. The ears at 10 cm are 2 voxels, which is the one thing this "
    "lattice is short on.",
    resolution_cm="5",
    **{
        "quad.length_m": 2.00,
        "quad.shoulder_h": 0.52,
        "quad.hip_h": 0.90,
        "quad.trunk_frac": 0.6,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.17,
        "quad.muzzle_frac": 0.11,
        "quad.neck_deg": 2.0,
        "quad.head_deg": -12.0,
        "quad.depth": 0.50,
        "quad.width": 0.70,
        "quad.chest": 1.08,
        "quad.waist": 0.96,
        "quad.rump": 0.94,
        "quad.belly": 0.52,
        "quad.section": 2.6,
        "quad.hump": 0.30,
        "quad.hump_at": 0.14,
        "quad.neck_thick": 0.82,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.70,
        "quad.muzzle_width": 0.62,
        "quad.muzzle_drop": 0.20,
        "quad.jaw": 0.45,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.05,
        "quad.ear_width": 1.00,
        "quad.ear_deg": 48.0,
        "quad.tail_len": 0.05,
        "quad.tail_thick": 0.16,
        "quad.tail_deg": -40.0,
        "quad.leg_thick": 0.28,
        "quad.hock": 0.30,
        "quad.fore_bend": 0.30,
        "quad.foot": 1.50,
        "quad.under": 0.0,
        "materials.quad_back": "skin_brown",
        "materials.quad_belly": "skin_brown",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_brown",
        "materials.quad_mark": "skin_dark",
        "quad.sex_length": 1.20,
        "herd.cover": "forest",
        "herd.size_max": 2,
        "herd.per_hectare": 0.03,
        "herd.flee_m": 80.0,
        "biomes.taiga": 1.0,
        "biomes.temperate_forest": 0.6,
        "biomes.tundra_alpine": 0.4,
    },
)

add(
    "western-lowland-gorilla",
    "THE ONE ANIMAL IN THIS TRANCHE WHOSE FORELIMBS ARE LONGER THAN ITS HIND "
    "LEGS, which is why `quad.fore_reach` runs above 1.0 rather than stopping "
    "there. A knuckle-walking ape carries its shoulders well above its hips and "
    "reaches the ground with its arms, and if the reach parameter had been "
    "capped at 'touches the floor' this would have been unbuildable.\n\n"
    "LATTICE: 2 cm. The pale saddle on an adult male and the conical crest of "
    "the skull are the features, both around 15 cm on a 1.4 m head-body -- so "
    "5 cm would strictly do. Held at 2 cm because a gorilla's face is the thing "
    "a player will look at and at 5 cm the head is 6 voxels.",
    **{
        "quad.length_m": 1.40,
        "quad.stance": "standing",
        "quad.shoulder_h": 0.66,
        "quad.hip_h": 0.72,
        "quad.trunk_frac": 0.6,
        "quad.neck_frac": 0.1,
        "quad.head_frac": 0.2,
        "quad.muzzle_frac": 0.1,
        "quad.neck_deg": 20.0,
        "quad.head_deg": -6.0,
        "quad.depth": 0.52,
        "quad.width": 0.72,
        "quad.chest": 1.14,
        "quad.waist": 0.98,
        "quad.rump": 0.86,
        "quad.belly": 0.56,
        "quad.section": 2.4,
        "quad.hump": 0.24,
        "quad.hump_at": 0.10,
        "quad.neck_thick": 0.92,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.62,
        "quad.muzzle_width": 0.62,
        "quad.jaw": 0.70,
        "quad.mane": 0.14,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.035,
        "quad.ear_width": 0.90,
        "quad.ear_deg": 25.0,
        "quad.tail_len": 0.0,
        "quad.leg_thick": 0.30,
        "quad.hock": 0.55,
        "quad.fore_bend": 0.12,
        # Arms longer than legs: the reach goes past the floor and the trunk
        # rides on them, which is what knuckle-walking looks like.
        "quad.fore_reach": 1.15,
        "quad.foot": 1.35,
        "quad.under": 0.0,
        "quad.cape": 0.34,
        "materials.quad_back": "skin_dark",
        "materials.quad_belly": "skin_dark",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "skin_dark",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "plume_grey",
        "quad.sex_length": 1.25,
        "quad.sex_mane": 2.50,
        "herd.cover": "forest",
        "herd.size_min": 2,
        "herd.size_max": 12,
        "herd.spread_m": 25.0,
        "herd.per_hectare": 0.12,
        "herd.flee_m": 40.0,
        "biomes.rainforest": 1.0,
    },
)

add(
    "chacma-baboon",
    "A dog-like muzzle on a monkey's body, with high shoulders and a tail "
    "carried in a sharp kink -- up and then down. The kink is `quad.tail_arc` "
    "at its useful extreme and is the only place in this tranche that row does "
    "something a carriage angle could not.\n\n"
    "LATTICE: 2 cm. The bare dark face and the heavy brow are 6-8 cm on a 0.9 m "
    "animal.",
    **{
        "quad.length_m": 0.90,
        "quad.shoulder_h": 0.72,
        "quad.hip_h": 0.86,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.14,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 18.0,
        "quad.head_deg": -10.0,
        "quad.depth": 0.44,
        "quad.width": 0.62,
        "quad.chest": 1.08,
        "quad.waist": 0.94,
        "quad.rump": 0.90,
        "quad.belly": 0.52,
        "quad.hump": 0.16,
        "quad.hump_at": 0.12,
        "quad.neck_thick": 0.72,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.55,
        "quad.muzzle_width": 0.42,
        "quad.jaw": 0.65,
        "quad.mane": 0.16,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.035,
        "quad.ear_width": 0.85,
        "quad.ear_deg": 35.0,
        "quad.tail_len": 0.72,
        "quad.tail_thick": 0.20,
        "quad.tail_taper": 0.50,
        "quad.tail_deg": 42.0,
        "quad.tail_arc": -0.80,
        "quad.leg_thick": 0.20,
        "quad.hock": 0.50,
        "quad.fore_bend": 0.30,
        "quad.foot": 1.10,
        "quad.under": 0.0,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_dark",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "plume_buff",
        "materials.quad_mark": "skin_brown",
        "quad.sex_length": 1.25,
        "herd.cover": "any",
        "herd.size_min": 5,
        "herd.size_max": 60,
        "herd.spread_m": 60.0,
        "herd.per_hectare": 2.0,
        "herd.flee_m": 25.0,
        "biomes.savanna": 0.9,
        "biomes.grassland": 0.4,
        "biomes.bare_rock": 0.0,
    },
)

# --- small mammals: the fine end of the lattice ------------------------------

add(
    "eastern-grey-squirrel",
    "THE TAIL IS THE SILHOUETTE, which is what makes this species worth "
    "building early: it is the only test that `quad.tail_thick` and "
    "`quad.tail_arc` together can make a tail that is more of the animal than "
    "the animal is.\n\n"
    "LATTICE: 1 cm. The ears are about 2.5 cm on a 26 cm body, so three voxels "
    "wants 0.8 cm and 1 cm is the finest tier available -- this species is at "
    "the limit of the rule rather than comfortably inside it. At 1 cm the body "
    "is 26 voxels, the same size as the smallest fish in the library.\n\n"
    "AND THE JOINT CAPS ARE OFF ON THIS ANIMAL, which is worth knowing. Its "
    "legs come out about 2 voxels thick, under the three-voxel floor the owner "
    "set for the cap spheres, so it ships with plain joints. That is the "
    "correct behaviour -- a cap here would be most of the leg -- and "
    "`tools/quadprobe.py --caps` is what confirms it is a decision rather than "
    "an accident.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.26,
        "quad.shoulder_h": 0.42,
        "quad.hip_h": 1.10,
        "quad.trunk_frac": 0.54,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.2,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 30.0,
        "quad.head_deg": 0.0,
        "quad.depth": 0.44,
        "quad.width": 0.62,
        "quad.chest": 0.94,
        "quad.waist": 0.96,
        "quad.rump": 1.06,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.70,
        "quad.head_size": 1.05,
        "quad.muzzle_depth": 0.48,
        "quad.muzzle_width": 0.44,
        "quad.ear_shape": "round",
        "quad.ear_len": 0.10,
        "quad.ear_width": 0.60,
        "quad.ear_deg": 82.0,
        # The plume: nearly as long as the body, barely tapering, and arched
        # over the back. `tail_taper` near 1 IS the plume -- there is no
        # separate switch for it, because that would be a second way to spell
        # a number that already exists.
        "quad.tail_len": 0.92,
        "quad.tail_thick": 0.60,
        "quad.tail_taper": 0.95,
        "quad.tail_deg": 20.0,
        "quad.tail_arc": 0.85,
        "quad.leg_thick": 0.18,
        "quad.hock": 0.55,
        "quad.fore_bend": 0.35,
        "quad.foot": 1.00,
        "quad.under": 0.38,
        "materials.quad_back": "plume_grey",
        "materials.quad_belly": "skin_pale",
        "materials.quad_head": "plume_grey",
        "materials.quad_leg": "plume_grey",
        "materials.quad_tail": "plume_grey",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "forest",
        "herd.size_max": 3,
        "herd.despawn_m": 70.0,
        "herd.per_hectare": 4.0,
        "herd.flee_m": 8.0,
        "biomes.temperate_forest": 1.0,
        "biomes.taiga": 0.3,
    },
)

add(
    "european-hare",
    "THE EARS ARE THE SPECIES, and this is the animal `quad.ear_shape` = blade "
    "was written for: a long flat black-tipped paddle standing well clear of "
    "the skull. It also carries the deepest hind-leg fold in the tranche after "
    "the kangaroo.\n\n"
    "LATTICE: 1 cm. The ears are 3-4 cm wide on a 0.65 m animal, so three "
    "voxels across one wants 1-1.3 cm. `03-temperate-forest.md` flags this row "
    "with a warning for exactly this reason. At 2 cm the ear is two voxels wide "
    "and the animal is a rabbit; at 1 cm the body is 65 voxels, which is "
    "comfortable.",
    resolution_cm="1",
    **{
        "quad.length_m": 0.65,
        "quad.shoulder_h": 0.38,
        "quad.hip_h": 1.20,
        "quad.trunk_frac": 0.56,
        "quad.neck_frac": 0.12,
        "quad.head_frac": 0.18,
        "quad.muzzle_frac": 0.14,
        "quad.neck_deg": 22.0,
        "quad.head_deg": -4.0,
        "quad.depth": 0.42,
        "quad.width": 0.58,
        "quad.chest": 0.92,
        "quad.waist": 0.94,
        "quad.rump": 1.14,
        "quad.belly": 0.52,
        "quad.neck_thick": 0.62,
        "quad.head_size": 0.95,
        "quad.muzzle_depth": 0.46,
        "quad.muzzle_width": 0.42,
        "quad.ear_shape": "blade",
        "quad.ear_len": 0.20,
        "quad.ear_width": 0.30,
        "quad.ear_deg": 72.0,
        "quad.ear_back": 0.45,
        "quad.tail_len": 0.14,
        "quad.tail_thick": 0.28,
        "quad.tail_taper": 0.70,
        "quad.tail_deg": -30.0,
        "quad.leg_thick": 0.16,
        "quad.hock": 0.80,
        "quad.fore_bend": 0.30,
        "quad.foot": 1.60,
        "quad.under": 0.40,
        "materials.quad_back": "plume_buff",
        "materials.quad_belly": "plume_white",
        "materials.quad_head": "plume_buff",
        "materials.quad_leg": "plume_buff",
        "materials.quad_tail": "skin_dark",
        "materials.quad_mark": "skin_dark",
        "herd.cover": "open",
        "herd.size_max": 3,
        "herd.despawn_m": 120.0,
        "herd.per_hectare": 1.2,
        "herd.flee_m": 40.0,
        "biomes.grassland": 1.0,
        "biomes.temperate_forest": 0.5,
        "biomes.beach": 0.2,
    },
)

# --- reptiles: the sprawling stance ------------------------------------------

add(
    "water-monitor",
    "THE SPRAWLING STANCE, and the reason it is a choice rather than an angle: "
    "this animal's limbs leave the FLANK and reach out sideways before they "
    "reach down, with the belly a few centimetres off the ground. No setting of "
    "a limb attached under the body produces that, because it is the attachment "
    "point that moves.\n\n"
    "It is also the widest asset in the tranche seen from above, which is a "
    "real test of the bounding box: a sprawled limb reaches further out in y "
    "than the animal is deep in z.\n\n"
    "LATTICE: 2 cm. The pale spot rows across the back are 4-5 cm on a 0.75 m "
    "head-body animal.",
    **{
        "quad.length_m": 0.75,
        "quad.stance": "sprawling",
        "quad.shoulder_h": 0.20,
        "quad.hip_h": 1.00,
        "quad.trunk_frac": 0.5,
        "quad.neck_frac": 0.22,
        "quad.head_frac": 0.16,
        "quad.muzzle_frac": 0.12,
        "quad.neck_deg": 4.0,
        "quad.head_deg": 0.0,
        "quad.depth": 0.30,
        "quad.width": 0.95,
        "quad.chest": 0.94,
        "quad.waist": 1.00,
        "quad.rump": 0.92,
        "quad.belly": 0.45,
        "quad.section": 2.6,
        "quad.neck_thick": 0.62,
        "quad.neck_taper": 0.80,
        "quad.head_size": 0.85,
        "quad.muzzle_depth": 0.48,
        "quad.muzzle_width": 0.50,
        "quad.jaw": 0.30,
        "quad.ear_shape": "none",
        "quad.eye": 1,
        "quad.tail_len": 1.45,
        "quad.tail_thick": 0.72,
        "quad.tail_taper": 0.08,
        "quad.tail_deg": -4.0,
        "quad.tail_arc": -0.10,
        "quad.leg_thick": 0.24,
        "quad.hock": 0.30,
        "quad.foot": 1.10,
        "quad.under": 0.34,
        "quad.mark": "bars",
        "quad.mark_count": 11,
        "quad.mark_width": 0.12,
        "quad.mark_strength": 0.85,
        "materials.quad_back": "skin_olive",
        "materials.quad_belly": "plume_buff",
        "materials.quad_head": "skin_olive",
        "materials.quad_leg": "skin_olive",
        "materials.quad_tail": "skin_olive",
        "materials.quad_mark": "skin_yellow",
        "materials.quad_horn": "skin_dark",
        "herd.cover": "waterside",
        "herd.size_max": 2,
        "herd.per_hectare": 0.2,
        "herd.flee_m": 25.0,
        "biomes.rainforest": 0.9,
        "biomes.savanna": 0.4,
        "biomes.beach": 0.3,
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
                          width=26):
            written += 1
    print(f"\n{written} of {len(SPECIES)} land-animal specs written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
