"""Author the large marine species: sharks, whales, dolphins, tuna, carp.

One-off, and it refuses to overwrite a spec that already exists -- see
`tools/seedspec.py`.

WHERE THE NUMBERS COME FROM. Every proportion below is a published median,
converted into this generator's parameters; `docs/marine-megafauna-research.md`
has the sources and the conversion. The ones that mattered most:

  * Fluke span is NOT a constant fraction of body length. It runs 0.215 (blue
    whale) to 0.350 (right whale), with the odontocetes at 0.22-0.30 -- so a
    humpback and a blue whale are genuinely different animals, not one shape at
    two sizes. Woodward, Winn & Fish 2006, n up to 448.
  * Cetacean fineness ratio (length / max diameter) sits between 4.2 and 6.5
    across every species measured, a spread of under 25% over a 6.8x range of
    body size. One body profile with per-species tuning is the right shape of
    solution, and this file is that.
  * A shark's upper caudal lobe is longer than its lower, by about 3:1 in a
    requiem shark and only 1.1:1 in a great white.

THE LATTICE IS PER SPECIES AND IT SCALES WITH THE ANIMAL. See `resolution_cm`
on each spec and the rule in the module docstring of `tools/fishprobe.py`: a
big animal needs MORE voxels of length than a small one, because the features
that identify it are a smaller fraction of its length. A blue whale's dorsal
fin is 1.0-1.4% of its body.

    python tools/seed_marine.py
    python tools/seed_marine.py --force
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(kind: str, res: str, **over):
    changes = {
        "kind": kind,
        "resolution_cm": res,
        "variation.amount": 1.0,
        "variation.height": 0.12,
        "variation.shape": 0.10,
        "variation.proportion": 0.14,
        "detail.entity_class": "detail",
        # A whale is not a shoal animal and it is not cheap to meet twice.
        "detail.school_min": 1,
        "detail.school_max": 1,
        "fish.fin_min_vox": 2.0,
    }
    changes.update(over)
    return changes


def cet(**over):
    """A cetacean's shared shape: horizontal fluke, barrel trunk that becomes a
    blade at the wrist, flippers, a blowhole, and no pelvic or anal fin."""
    d = {
        "fish.caudal_plane": "horizontal",
        "fish.caudal_shape": "forked",
        "fish.section": 2.5,          # the trunk really is a barrel: girth/pi
        "fish.section_tail": 1.3,     # the tailstock really is a blade
        "fish.belly": 0.50,
        "fish.pelvic": 0.0,
        "fish.anal_height": 0.0,
        "fish.barbels": 0,
        "fish.pattern": "none",
        "fish.eye": 1.0,
        "fish.blowhole": 1.0,
        "materials.fish_eye": "skin_dark",
        "biomes.ocean": 1.0,
    }
    d.update(over)
    return d


SPECIES = {
    # --- cetaceans ----------------------------------------------------------
    "bottlenose-dolphin": (
        "3 m, 5 cm — the baseline cetacean: fluke, flippers, falcate dorsal",
        base("cetacean", "5", name="bottlenose-dolphin",
             notes="The reference cetacean. Fluke span 0.236-0.253 of body "
                   "length, dorsal fin 10.8%, flippers 11-13%, fineness ratio "
                   "5.15 — every one of those a published median. At 5 cm it "
                   "is 60 voxels long, which is where a falcate dorsal fin "
                   "starts having a shape rather than a height.\n\n"
                   "IT NOW WEARS A CAPE, which is the dark back reaching down "
                   "onto the flank under the dorsal fin and lifting again "
                   "behind it. Every delphinid described in the literature has "
                   "the same curve — 'lowest below the dorsal fin, passes "
                   "dorsally about halfway between the dorsal fin and the "
                   "flukes' (Perrin 1998) — and nobody anywhere has published "
                   "how far DOWN it reaches. 0.22 of body depth is traced from "
                   "reference photographs and it is what this animal can hold: "
                   "at 5 cm it is 12 voxels deep, so the cape dips two, which "
                   "is the floor. A bottlenose is the shallowest animal in the "
                   "library that can carry one at all.",
             **cet(**{
                 "fish.length_m": 3.0, "fish.depth_ratio": 0.195,
                 "fish.width_ratio": 0.95, "fish.depth_at": 0.40,
                 "fish.fullness": 2.6, "fish.snout": 0.30, "fish.peduncle": 0.28,
                 "fish.width_follow": 1.35, "fish.head_frac": 0.20,
                 "fish.caudal_len": 0.13, "fish.caudal_span": 1.25,
                 "fish.caudal_fork": 0.30,
                 "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.42,
                 "fish.dorsal_len": 0.16, "fish.dorsal_height": 0.55,
                 "fish.pectoral": 0.55, "fish.pectoral_aspect": 0.55,
                 "fish.back_frac": 0.46, "fish.belly_frac": 0.30,
                 "fish.field_curve": "cape", "fish.curve_at": 0.50,
                 "fish.curve_amount": 0.22,
                 "materials.fish_back": "skin_dark",
                 "materials.fish_flank": "skin_silver",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_dark",
                 "biomes.ocean": 1.0, "biomes.beach": 0.4,
                 "placement.abundance": 0.3, "placement.spacing_m": 40.0,
                 "detail.despawn_m": 120.0, "detail.school_min": 2,
                 "detail.school_max": 8, "detail.school_radius_m": 25.0,
                 "detail.water": "ocean", "detail.depth_min_m": 0.0,
                 "detail.depth_max_m": 30.0, "detail.min_water_depth_m": 6.0,
                 "detail.per_100m2": 0.05,
             })),
    ),
    "orca": (
        "7 m, 5 cm — the eye patch, the tall dorsal, the white belly",
        base("cetacean", "5", name="orca",
             notes="The most identifiable animal in the sea, and almost all of "
                   "it is colour. Fineness ratio 4.81, the closest to optimal "
                   "of any cetacean measured.\n\n"
                   "THE DORSAL FIN WAS RE-AUTHORED ON 2026-08-13 and this note "
                   "is the reason. It used to say 'this is authored male' — "
                   "and it was more than male: 1.30 of body depth is 27% of "
                   "body length, and the published bull is 22-26%. Nothing on "
                   "disk said which animal was being drawn. With `fish.sex` "
                   "the authored number is the average of the two sexes and "
                   "the ratio splits them: 0.92, ratio 1.55, so `male` gives "
                   "24% of body length and `female` 15%. Length 7.0 m and the "
                   "flipper reach 0.60 are UNCHANGED — a 1.14 length ratio "
                   "puts 7.5 m and 6.6 m either side of the authored animal, "
                   "which brackets the published Type A means of 7.3 and 6.4, "
                   "so there was nothing to correct. Clark & Odell 1999 (n=10 "
                   "males, 20 females) found flipper length and dorsal height "
                   "are the ONLY two proportionally dimorphic measurements on "
                   "this animal; the per-sex flipper fractions are quoted "
                   "everywhere and sourced nowhere, so 1.60 here is traced "
                   "from photographs and is marked as such.\n\n"
                   "THE VENTRAL FLAME is the white belly throwing a blaze up "
                   "the flank behind the middle of the animal, which is why an "
                   "orca reads as two white shapes from the side and not one. "
                   "Morin et al. 2024 describe the ventral field as having "
                   "'lobes extending up and back along the tail stock' with 'a "
                   "crisp border' — the second half of that is the argument "
                   "for a hard boundary rather than a fade. No published "
                   "number exists for where the flame starts, how high it "
                   "reaches, or how narrow the panel gets between the "
                   "flippers; 0.78 of body length and 0.34 of body depth are "
                   "traced from reference photographs and are marked as such.\n\n"
                   "The eye patch is drawn as a lozenge because a lozenge is "
                   "what it is, but the '21.8 x 5.9 cm on a 6 m animal' this "
                   "note used to quote HAS NO SOURCE — see "
                   "docs/marine-marking-research.md §5. The published figure is "
                   "a ratio: patch length is 0.37-0.41 of the blowhole-to-"
                   "dorsal-fin distance on the large-patched Antarctic types "
                   "(Durban et al. 2016, n=19).",
             **cet(**{
                 "fish.length_m": 7.0, "fish.depth_ratio": 0.21,
                 "fish.width_ratio": 0.95, "fish.depth_at": 0.38,
                 "fish.fullness": 2.4, "fish.snout": 0.40, "fish.peduncle": 0.26,
                 "fish.width_follow": 1.40, "fish.head_frac": 0.20,
                 "fish.caudal_len": 0.12, "fish.caudal_span": 1.20,
                 "fish.caudal_fork": 0.32,
                 "fish.dorsal_shape": "sail", "fish.dorsal_start": 0.38,
                 "fish.dorsal_len": 0.14, "fish.dorsal_height": 0.92,
                 "fish.pectoral": 0.60, "fish.pectoral_aspect": 0.75,
                 "fish.eye_patch": 2.0,
                 "fish.sex_length": 1.14, "fish.sex_dorsal": 1.55,
                 "fish.sex_pectoral": 1.60,
                 "fish.back_frac": 0.62, "fish.belly_frac": 0.30,
                 "fish.field_curve": "flame", "fish.curve_at": 0.78,
                 "fish.curve_amount": 0.34,
                 "fish.pattern": "saddle", "fish.pattern_scale": 0.10,
                 "fish.pattern_strength": 0.10,
                 "materials.fish_back": "skin_dark",
                 "materials.fish_flank": "skin_dark",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_dark",
                 "materials.fish_pattern": "skin_silver",
                 "materials.fish_patch": "skin_pale",
                 "biomes.ocean": 1.0, "biomes.beach": 0.2,
                 "placement.abundance": 0.08, "placement.spacing_m": 300.0,
                 "detail.despawn_m": 250.0, "detail.school_min": 1,
                 "detail.school_max": 4, "detail.school_radius_m": 60.0,
                 "detail.water": "ocean", "detail.depth_min_m": 0.0,
                 "detail.depth_max_m": 60.0, "detail.min_water_depth_m": 15.0,
                 "detail.per_100m2": 0.004,
             })),
    ),
    "common-dolphin": (
        "2.4 m, 2 cm — the hourglass: the cape's V and the flank blaze meeting",
        base("cetacean", "2", name="common-dolphin",
             notes="The species the shaped-boundary work was done for. Its "
                   "criss-cross is not a marking painted on a flank — it is "
                   "the dark cape dipping under the dorsal fin while the pale "
                   "ventral field throws a blaze up to meet it, so the flank "
                   "is pinched to a waist and shows as two patches. That is "
                   "`fish.field_curve` set to `hourglass`, and Perrin 1972 "
                   "says it is right: the four colours of a common dolphin are "
                   "the overlap of TWO shapes, not four regions — 'the buff "
                   "thoracic patch represents the colour yielded by the "
                   "pigment of the cape alone, the grey flank patch that of "
                   "the dorsal field overlay alone, and the black dorsalmost "
                   "area that of the combined effect'. So the flank forward of "
                   "the waist is drawn buff and the flank behind it grey, and "
                   "the criss-cross is the animal's ONE marking rather than "
                   "something laid on top of it. The patch is drawn in "
                   "`plume_rufous` and not in a tan: measured against the "
                   "silver flank beside it, a buff is a value contrast of 1.05 "
                   "and the readability floor is 1.5, so the right hue would "
                   "have been present in the voxels and invisible in the "
                   "water.\n\n"
                   "AUTHORED AT 2 cm AND NOT THE 5 cm THE OTHER DOLPHINS USE, "
                   "because the identifying feature is the waist and a waist "
                   "is a boundary that has to MOVE. Measured with "
                   "`tools/fishprobe.py --marks`: at 5 cm the animal is 7 "
                   "voxels deep and the cape dips ONE voxel, which is a ragged "
                   "line and not a curve; at 2.5 cm it is 14 deep and dips 3; "
                   "at 2 cm it is 18 deep and dips 4 with a 7-voxel blaze. "
                   "2.5 cm meets the three-voxel rule with nothing to spare "
                   "and the variation draw moves body depth by a tenth either "
                   "way, so 2 cm it is — a tier the library already authors "
                   "at. This is the lattice rule applied for the first time to "
                   "a COLOUR feature rather than to a fin.\n\n"
                   "The waist sits under the dorsal fin because that is where "
                   "the cape's lowest point is on every delphinid described: "
                   "'the ventral margin of the cape dips over the eye, is "
                   "lowest below the dorsal fin, passes dorsally about halfway "
                   "between the dorsal fin and the flukes' (Perrin 1998). The "
                   "dorsal fin origin is 44-46% of total length over 90 "
                   "measured animals (Heyning & Perrin 1994). The flank blaze "
                   "is a NORTH ATLANTIC animal — Pacific short-beaked common "
                   "dolphins do not carry one. Fluke span 0.295 of body length "
                   "and dorsal fin 8.9%, both Pavlov 2021.",
             **cet(**{
                 "fish.length_m": 2.4, "fish.depth_ratio": 0.19,
                 # Female-biased: 189.5 cm male against 180.1 cm female, n=28 and 37.
                 "fish.sex_length": 1.05,
                 "fish.width_ratio": 0.92, "fish.depth_at": 0.38,
                 "fish.fullness": 2.6, "fish.snout": 0.26, "fish.peduncle": 0.26,
                 "fish.width_follow": 1.38, "fish.head_frac": 0.18,
                 "fish.caudal_len": 0.12, "fish.caudal_span": 1.55,
                 "fish.caudal_fork": 0.30,
                 "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.40,
                 "fish.dorsal_len": 0.15, "fish.dorsal_height": 0.47,
                 "fish.pectoral": 0.74, "fish.pectoral_aspect": 0.50,
                 "fish.back_frac": 0.40, "fish.belly_frac": 0.26,
                 "fish.field_curve": "hourglass", "fish.curve_at": 0.50,
                 "fish.curve_amount": 0.30,
                 "materials.fish_back": "skin_dark",
                 "materials.fish_flank": "skin_silver",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_dark",
                 # The thoracic patch, and it is a plumage material because no
                 # skin colour can do the job. `plume_buff` is the right HUE
                 # for a tan patch and measures a value contrast of 1.05
                 # against the silver flank beside it -- under the 1.5 floor,
                 # which means present in the voxels and invisible in the
                 # water. `plume_rufous` is the same warm field two steps
                 # darker, at 2.52. See forge/materials.py.
                 "materials.fish_pattern": "plume_rufous",
                 "biomes.ocean": 1.0, "biomes.beach": 0.3,
                 "placement.abundance": 0.35, "placement.spacing_m": 30.0,
                 "detail.despawn_m": 120.0, "detail.school_min": 4,
                 "detail.school_max": 24, "detail.school_radius_m": 30.0,
                 "detail.water": "ocean", "detail.depth_min_m": 0.0,
                 "detail.depth_max_m": 40.0, "detail.min_water_depth_m": 8.0,
                 "detail.per_100m2": 0.06,
             })),
    ),
    "humpback-whale": (
        "14 m, 10 cm — the flippers, which are a third of the animal",
        base("cetacean", "10", name="humpback-whale",
             notes="Flippers 30.8% of body length and 7.3% wide — a chord "
                   "ratio of about 0.24, statistically longer than its body "
                   "length predicts, and the most recognisable limb in the "
                   "sea. Fluke span 0.341 of body length, against a blue "
                   "whale's 0.215. Stocky: volumetric coefficient 12.8 where a "
                   "blue whale is 5.7.",
             **cet(**{
                 "fish.length_m": 14.0, "fish.depth_ratio": 0.24,
                 # Female-biased: 13.0 m male against 13.9 m female; the ONLY visible difference.
                 "fish.sex_length": 0.94,
                 "fish.width_ratio": 0.90, "fish.depth_at": 0.34,
                 "fish.fullness": 2.2, "fish.snout": 0.52, "fish.peduncle": 0.24,
                 "fish.width_follow": 1.45, "fish.head_frac": 0.28,
                 "fish.caudal_len": 0.11, "fish.caudal_span": 1.45,
                 "fish.caudal_fork": 0.28,
                 "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.62,
                 "fish.dorsal_len": 0.12, "fish.dorsal_height": 0.16,
                 "fish.pectoral": 1.20, "fish.pectoral_aspect": 0.24,
                 "fish.back_frac": 0.70, "fish.belly_frac": 0.26,
                 "materials.fish_back": "skin_dark",
                 "materials.fish_flank": "skin_dark",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_pale",
                 "biomes.ocean": 1.0,
                 "placement.abundance": 0.05, "placement.spacing_m": 800.0,
                 "detail.despawn_m": 400.0, "detail.school_max": 3,
                 "detail.school_radius_m": 120.0,
                 "detail.water": "ocean", "detail.depth_min_m": 0.0,
                 "detail.depth_max_m": 80.0, "detail.min_water_depth_m": 25.0,
                 "detail.per_100m2": 0.001,
             })),
    ),
    "blue-whale": (
        "25 m, 10 cm — the largest animal, and a dorsal fin 1% of it",
        base("cetacean", "10", name="blue-whale",
             notes="The species that decides the lattice rule. Its dorsal fin "
                   "is 1.0-1.4% of body length — 25-35 cm on a 25 m animal — "
                   "and it is exactly what separates a blue from a fin from a "
                   "sei. Faithfully proportioned at any lattice this library "
                   "can afford, it is under half a voxel; `fish.fin_min_vox` "
                   "is the only reason it exists at all. Fluke span 0.215 and "
                   "flippers 0.132, both the smallest of any whale measured, "
                   "and fineness ratio 6.37, the slenderest.",
             **cet(**{
                 "fish.length_m": 25.0, "fish.depth_ratio": 0.157,
                 "fish.width_ratio": 1.00, "fish.depth_at": 0.36,
                 "fish.fullness": 2.0, "fish.snout": 0.34, "fish.peduncle": 0.20,
                 "fish.width_follow": 1.50, "fish.head_frac": 0.26,
                 "fish.caudal_len": 0.08, "fish.caudal_span": 1.35,
                 "fish.caudal_fork": 0.26,
                 "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.74,
                 "fish.dorsal_len": 0.07, "fish.dorsal_height": 0.09,
                 "fish.pectoral": 0.60, "fish.pectoral_aspect": 0.30,
                 "fish.back_frac": 0.55, "fish.belly_frac": 0.24,
                 "fish.pattern": "mottle", "fish.pattern_scale": 0.02,
                 "fish.pattern_strength": 0.30,
                 "materials.fish_back": "skin_blue",
                 "materials.fish_flank": "skin_silver",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_blue",
                 "materials.fish_pattern": "skin_pale",
                 "biomes.ocean": 1.0,
                 "placement.abundance": 0.02, "placement.spacing_m": 2000.0,
                 "detail.despawn_m": 400.0, "detail.school_max": 2,
                 "detail.school_radius_m": 200.0,
                 "detail.water": "ocean", "detail.depth_min_m": 0.0,
                 "detail.depth_max_m": 100.0, "detail.min_water_depth_m": 40.0,
                 "detail.per_100m2": 0.0005,
             })),
    ),
    "sperm-whale": (
        "16 m, 10 cm — a quarter of it is a square head, and no dorsal fin",
        base("cetacean", "10", name="sperm-whale",
             notes="The head is a quarter to a third of the animal and it is "
                   "square, which no other whale's is; this is the species the "
                   "`snout` slider goes to 0.9 for. No dorsal FIN — a hump and "
                   "a row of knuckles on the caudal third — so `dorsal_shape` "
                   "is `ridge`, set far back. Blowhole at the very front of "
                   "the head and offset to the left in life; drawn centred, "
                   "because one voxel of offset would read as a mistake.\n\n"
                   "RE-AUTHORED FROM 16 m TO 13.3 m ON 2026-08-13, for the "
                   "same reason the whale shark moved and in the opposite "
                   "direction: 16 m is a bull sperm whale and the cows are "
                   "11 m. This is the most size-dimorphic cetacean there is. "
                   "13.3 m is the geometric mean, `male` gives 16 and `female` "
                   "11, and the animal got cheaper rather than dearer.",
             **cet(**{
                 "fish.length_m": 13.3, "fish.sex_length": 1.45,
                 "fish.depth_ratio": 0.20,
                 "fish.width_ratio": 0.92, "fish.depth_at": 0.22,
                 "fish.fullness": 2.6, "fish.snout": 0.90, "fish.peduncle": 0.26,
                 "fish.width_follow": 1.30, "fish.head_frac": 0.30,
                 "fish.caudal_len": 0.10, "fish.caudal_span": 1.30,
                 "fish.caudal_fork": 0.24,
                 "fish.dorsal_shape": "ridge", "fish.dorsal_start": 0.62,
                 "fish.dorsal_len": 0.26, "fish.dorsal_height": 0.10,
                 "fish.pectoral": 0.35, "fish.pectoral_aspect": 0.70,
                 "fish.section": 2.8, "fish.section_tail": 1.4,
                 "fish.back_frac": 0.60, "fish.belly_frac": 0.18,
                 "fish.pattern": "mottle", "fish.pattern_scale": 0.04,
                 "fish.pattern_strength": 0.18,
                 "materials.fish_back": "skin_brown",
                 "materials.fish_flank": "skin_brown",
                 "materials.fish_belly": "skin_brown",
                 "materials.fish_fin": "skin_brown",
                 "materials.fish_pattern": "skin_pale",
                 "biomes.ocean": 1.0,
                 "placement.abundance": 0.03, "placement.spacing_m": 1500.0,
                 "detail.despawn_m": 400.0, "detail.school_max": 5,
                 "detail.school_radius_m": 150.0,
                 "detail.water": "ocean", "detail.depth_min_m": 0.0,
                 "detail.depth_max_m": 200.0, "detail.min_water_depth_m": 60.0,
                 "detail.per_100m2": 0.0008,
             })),
    ),
    "beluga": (
        "4.5 m, 5 cm — white, with NO dorsal fin at all",
        base("cetacean", "5", name="beluga",
             notes="The null case, and worth having for it: a cetacean with no "
                   "dorsal fin (a low ridge on the rear half), no markings and "
                   "no countershading, all white. Everything that identifies "
                   "it is the outline plus the absence of a fin. Belugas are "
                   "born grey and turn white at seven to nine years; this is "
                   "an adult. Fluke span 0.234, flipper planform square rather "
                   "than tapered, which is not modelled.",
             **cet(**{
                 "fish.length_m": 4.5, "fish.depth_ratio": 0.20,
                 # Female-biased: 483 cm male against 386 cm female, n=130 and 166.
                 "fish.sex_length": 1.25,
                 "fish.width_ratio": 0.98, "fish.depth_at": 0.36,
                 "fish.fullness": 2.8, "fish.snout": 0.52, "fish.peduncle": 0.30,
                 "fish.width_follow": 1.20, "fish.head_frac": 0.18,
                 "fish.caudal_len": 0.12, "fish.caudal_span": 1.25,
                 "fish.caudal_fork": 0.30,
                 "fish.dorsal_shape": "ridge", "fish.dorsal_start": 0.50,
                 "fish.dorsal_len": 0.34, "fish.dorsal_height": 0.06,
                 "fish.pectoral": 0.45, "fish.pectoral_aspect": 0.85,
                 "fish.section": 2.8, "fish.section_tail": 1.4,
                 "fish.back_frac": 0.0, "fish.belly_frac": 0.0,
                 "materials.fish_back": "skin_pale",
                 "materials.fish_flank": "skin_pale",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_pale",
                 "biomes.ocean": 0.8,
                 "placement.abundance": 0.06, "placement.spacing_m": 200.0,
                 "detail.despawn_m": 180.0, "detail.school_min": 2,
                 "detail.school_max": 10, "detail.school_radius_m": 40.0,
                 "detail.water": "ocean", "detail.depth_min_m": 0.0,
                 "detail.depth_max_m": 40.0, "detail.min_water_depth_m": 10.0,
                 "detail.per_100m2": 0.01,
             })),
    ),
    # --- sharks and large fish ---------------------------------------------
    "great-white-shark": (
        "4.5 m, 5 cm — nearly symmetric tail, abrupt countershading",
        base("fish", "5", name="great-white-shark",
             notes="A lamnid, so its tail is only 1.1:1 upper-to-lower and "
                   "reads as almost symmetric — the opposite of what people "
                   "draw. Measured over five lamnid species with no allometry "
                   "within or between them: head 29% of total length, first "
                   "dorsal 10%, caudal span 24%, pectorals 19%. The "
                   "countershading boundary is described in the literature as "
                   "ABRUPT, which is why the back and belly fractions nearly "
                   "meet.",
             **{
                 "fish.length_m": 4.5, "fish.depth_ratio": 0.175,
                 # Female-biased: mature 350-410 cm male against 450-500 cm female.
                 "fish.sex_length": 0.8,
                 "fish.width_ratio": 0.62, "fish.depth_at": 0.36,
                 "fish.fullness": 2.6, "fish.snout": 0.34, "fish.peduncle": 0.16,
                 "fish.belly": 0.48, "fish.width_follow": 1.50,
                 "fish.section": 2.3, "fish.section_tail": 1.5,
                 "fish.head_frac": 0.29,
                 "fish.caudal_shape": "forked", "fish.caudal_len": 0.20,
                 "fish.caudal_span": 1.45, "fish.caudal_fork": 0.55,
                 "fish.caudal_upper": 0.10,
                 "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.30,
                 "fish.dorsal_len": 0.14, "fish.dorsal_height": 0.62,
                 "fish.dorsal2_height": 0.10, "fish.dorsal2_start": 0.72,
                 "fish.dorsal2_len": 0.07,
                 "fish.anal_height": 0.10, "fish.anal_len": 0.08,
                 "fish.pectoral": 0.95, "fish.pectoral_aspect": 0.55,
                 "fish.pelvic": 0.10, "fish.eye": 1.0,
                 "fish.back_frac": 0.56, "fish.belly_frac": 0.40,
                 "fish.pattern": "none",
                 "materials.fish_back": "skin_dark",
                 "materials.fish_flank": "skin_silver",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_silver",
                 "materials.fish_pattern": "skin_dark",
                 "materials.fish_eye": "skin_dark",
                 "biomes.ocean": 1.0, "biomes.beach": 0.2,
                 "placement.abundance": 0.05, "placement.spacing_m": 400.0,
                 "detail.despawn_m": 180.0, "detail.school_max": 1,
                 "detail.water": "ocean", "detail.depth_min_m": 1.0,
                 "detail.depth_max_m": 80.0, "detail.min_water_depth_m": 12.0,
                 "detail.per_100m2": 0.003,
             }),
    ),
    "tiger-shark": (
        "4 m, 5 cm — a requiem shark: 3:1 tail, and the bars",
        base("fish", "5", name="tiger-shark",
             notes="A carcharhiniform, so the tail is strongly heterocercal — "
                   "the upper lobe is 31% of total length against the lower's "
                   "10%, about 3:1, and that asymmetry is the whole silhouette "
                   "difference from a lamnid. The bars run from just behind "
                   "the head to the caudal peduncle (0.20 to 0.85 of the body) "
                   "and fade with age; no source gives a count, so seven is a "
                   "construction, chosen because five to seven is what reads "
                   "at eighty voxels.",
             **{
                 "fish.length_m": 4.0, "fish.depth_ratio": 0.17,
                 # Female-biased: 406 cm male against 464 cm female, n=420.
                 "fish.sex_length": 0.88,
                 "fish.width_ratio": 0.70, "fish.depth_at": 0.32,
                 "fish.fullness": 2.6, "fish.snout": 0.46, "fish.peduncle": 0.22,
                 "fish.belly": 0.48, "fish.width_follow": 1.35,
                 "fish.section": 2.4, "fish.section_tail": 1.6,
                 "fish.head_frac": 0.24,
                 "fish.caudal_shape": "pointed", "fish.caudal_len": 0.28,
                 "fish.caudal_span": 1.30, "fish.caudal_upper": 0.62,
                 "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.30,
                 "fish.dorsal_len": 0.14, "fish.dorsal_height": 0.55,
                 "fish.dorsal2_height": 0.16, "fish.dorsal2_start": 0.66,
                 "fish.dorsal2_len": 0.08,
                 "fish.anal_height": 0.14, "fish.anal_len": 0.08,
                 "fish.pectoral": 0.75, "fish.pectoral_aspect": 0.60,
                 "fish.pelvic": 0.10, "fish.eye": 1.0,
                 "fish.back_frac": 0.44, "fish.belly_frac": 0.34,
                 "fish.pattern": "bars", "fish.pattern_count": 7,
                 "fish.pattern_width": 0.30,
                 "materials.fish_back": "skin_brown",
                 "materials.fish_flank": "skin_brown",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_brown",
                 "materials.fish_pattern": "skin_dark",
                 "materials.fish_eye": "skin_dark",
                 "biomes.ocean": 0.9, "biomes.beach": 0.3,
                 "placement.abundance": 0.06, "placement.spacing_m": 300.0,
                 "detail.despawn_m": 160.0, "detail.school_max": 1,
                 "detail.water": "ocean", "detail.depth_min_m": 1.0,
                 "detail.depth_max_m": 60.0, "detail.min_water_depth_m": 8.0,
                 "detail.per_100m2": 0.004,
             }),
    ),
    "whale-shark": (
        "9 m, 5 cm — a checkerboard of pale spots on a broad flat head",
        base("fish", "5", name="whale-shark",
             notes="The pattern is described in the literature as a "
                   "CHECKERBOARD: pale spots and stripes, irregular in front "
                   "of the pectorals and regular rows behind them. No source "
                   "anywhere gives a spot diameter or spacing, so the scale "
                   "here is a construction — about 6-10 cm spots on a 9 m "
                   "animal, which is 1-2 voxels at 5 cm, chosen so they land "
                   "ON the lattice rather than under it. Head truncated and "
                   "flattened with the mouth at the very front, which the low "
                   "snout depth and high width ratio between them give.\n\n"
                   "RE-AUTHORED FROM 9 m TO 11.1 m ON 2026-08-13. The 9 m it "
                   "carried is the MALE figure: whale sharks run 8-9 m male "
                   "against 14.5 m female, the largest sexual size difference "
                   "of any animal in this library, and nothing on disk said "
                   "which one was being drawn. 11.1 m is the geometric mean of "
                   "the two, so `male` gives 8.5 and `female` 14.5 and "
                   "`unsexed` is a whale shark rather than a bull whale shark. "
                   "It costs voxels — the animal is 23% longer and so nearly "
                   "twice the solid — and it is still the right number.",
             **{
                 "fish.length_m": 11.1, "fish.sex_length": 0.59,
                 "fish.depth_ratio": 0.20,
                 "fish.width_ratio": 0.95, "fish.depth_at": 0.30,
                 "fish.fullness": 2.8, "fish.snout": 0.72, "fish.peduncle": 0.24,
                 "fish.belly": 0.46, "fish.width_follow": 1.30,
                 "fish.section": 2.8, "fish.section_tail": 1.5,
                 "fish.head_frac": 0.22,
                 "fish.caudal_shape": "forked", "fish.caudal_len": 0.24,
                 "fish.caudal_span": 1.40, "fish.caudal_fork": 0.40,
                 "fish.caudal_upper": 0.45,
                 "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.46,
                 "fish.dorsal_len": 0.14, "fish.dorsal_height": 0.50,
                 "fish.dorsal2_height": 0.14, "fish.dorsal2_start": 0.74,
                 "fish.dorsal2_len": 0.08,
                 "fish.anal_height": 0.14, "fish.anal_len": 0.08,
                 "fish.pectoral": 0.90, "fish.pectoral_aspect": 0.50,
                 "fish.pelvic": 0.10, "fish.eye": 1.0,
                 "fish.back_frac": 0.60, "fish.belly_frac": 0.34,
                 "fish.pattern": "spots", "fish.pattern_count": 22,
                 # 0.02 is the parameter's FLOOR, not a choice. The estimate
                 # from the literature is 0.006-0.012 of body length, and the
                 # draft asked for 0.012 and was silently clamped up. At 180
                 # voxels 0.02 is a spot about four voxels across, which is
                 # what the lattice can actually hold -- the honest reading is
                 # that a real whale shark's spots are finer than this asset
                 # can express, so they are drawn at the smallest size that
                 # reads instead of at the smallest size that is true.
                 "fish.pattern_scale": 0.02,
                 "materials.fish_back": "skin_dark",
                 "materials.fish_flank": "skin_dark",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_dark",
                 "materials.fish_pattern": "skin_pale",
                 "materials.fish_eye": "skin_dark",
                 "biomes.ocean": 0.8,
                 "placement.abundance": 0.04, "placement.spacing_m": 600.0,
                 "detail.despawn_m": 250.0, "detail.school_max": 2,
                 "detail.school_radius_m": 80.0,
                 "detail.water": "ocean", "detail.depth_min_m": 0.0,
                 "detail.depth_max_m": 50.0, "detail.min_water_depth_m": 20.0,
                 "detail.per_100m2": 0.002,
             }),
    ),
    "scalloped-hammerhead": (
        "3.5 m, 5 cm — the cephalofoil, and the eyes out on its tips",
        base("fish", "5", name="scalloped-hammerhead",
             notes="The species the head-span parameter was added for, and the "
                   "reason it had to be added: a hammerhead's head is the "
                   "SHALLOWEST part of the animal and by a long way the "
                   "widest, and the body loft derives width from depth, so no "
                   "setting of any existing slider could produce one. Until "
                   "this, typing `hammerhead` gave a shark with hammerhead "
                   "proportions and an ordinary head.\n\n"
                   "Every proportion below is the scalloped hammerhead "
                   "holotype measured under Compagno's scheme, as a percentage "
                   "of total length: cephalofoil width 30, head 24, first "
                   "dorsal height 13 with a base of 9, second dorsal height 2, "
                   "anal 3, pectoral anterior margin 11, pre-first-dorsal 28, "
                   "pre-second-dorsal 60, and a caudal fin whose upper margin "
                   "is 31 against the lower's 10 — the 3.1:1 heterocercy that "
                   "separates a requiem shark's tail from a great white's.\n\n"
                   "THE EYES COME OUT ON THE TIPS FOR FREE. The eye is drawn "
                   "on the outermost occupied voxel at its station, and the "
                   "station sits inside the hammer, so widening the head "
                   "carries the eye out with it — which is where a "
                   "hammerhead's eyes actually are. `tools/fishprobe.py "
                   "--head` measures it, because a feature nobody had to write "
                   "is a feature nobody would notice breaking.",
             **{
                 "fish.length_m": 3.5, "fish.depth_ratio": 0.13,
                 # Female-biased: no Sphyrna figure retrieved; the carcharhiniform cross-species mean.
                 "fish.sex_length": 0.89,
                 "fish.width_ratio": 0.62, "fish.depth_at": 0.34,
                 "fish.fullness": 2.6, "fish.snout": 0.20, "fish.peduncle": 0.20,
                 "fish.belly": 0.48, "fish.width_follow": 1.45,
                 "fish.section": 2.3, "fish.section_tail": 1.5,
                 "fish.head_frac": 0.24, "fish.head_width": 0.38,
                 "fish.caudal_shape": "pointed", "fish.caudal_len": 0.31,
                 "fish.caudal_span": 1.35, "fish.caudal_upper": 0.62,
                 "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.28,
                 "fish.dorsal_len": 0.09, "fish.dorsal_height": 1.00,
                 "fish.dorsal2_height": 0.16, "fish.dorsal2_start": 0.60,
                 "fish.dorsal2_len": 0.03,
                 "fish.anal_height": 0.23, "fish.anal_len": 0.06,
                 "fish.pectoral": 0.85, "fish.pectoral_aspect": 0.55,
                 "fish.pelvic": 0.10, "fish.eye": 1.0,
                 "fish.back_frac": 0.50, "fish.belly_frac": 0.34,
                 "fish.pattern": "none",
                 "materials.fish_back": "skin_brown",
                 "materials.fish_flank": "skin_silver",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_brown",
                 "materials.fish_pattern": "skin_dark",
                 "materials.fish_eye": "skin_dark",
                 "biomes.ocean": 1.0, "biomes.beach": 0.3,
                 "placement.abundance": 0.08, "placement.spacing_m": 250.0,
                 "detail.despawn_m": 160.0, "detail.school_min": 1,
                 "detail.school_max": 6, "detail.school_radius_m": 40.0,
                 "detail.water": "ocean", "detail.depth_min_m": 1.0,
                 "detail.depth_max_m": 80.0, "detail.min_water_depth_m": 10.0,
                 "detail.per_100m2": 0.006,
             }),
    ),
    "reef-shark": (
        "1.6 m, 2 cm — the small common shark, black-tipped fins",
        base("fish", "2", name="reef-shark",
             notes="The shark a player will actually meet. Blacktips are not "
                   "plain black tips — every fin carries a black cap with a "
                   "pale sub-band under it, and there is a white flank band "
                   "running forward from above the anal fin. Neither is "
                   "modelled: a per-fin two-part tip needs the fin colour to "
                   "vary along the fin, which the generator does not do. The "
                   "flank band is, as a stripe.",
             **{
                 "fish.length_m": 1.6, "fish.depth_ratio": 0.16,
                 # Female-biased: 139 cm male against 157 cm female.
                 "fish.sex_length": 0.87,
                 "fish.width_ratio": 0.66, "fish.depth_at": 0.34,
                 "fish.fullness": 2.6, "fish.snout": 0.32, "fish.peduncle": 0.20,
                 "fish.belly": 0.48, "fish.width_follow": 1.40,
                 "fish.section": 2.3, "fish.section_tail": 1.5,
                 "fish.head_frac": 0.24,
                 "fish.caudal_shape": "pointed", "fish.caudal_len": 0.26,
                 "fish.caudal_span": 1.25, "fish.caudal_upper": 0.58,
                 "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.32,
                 "fish.dorsal_len": 0.13, "fish.dorsal_height": 0.60,
                 "fish.dorsal2_height": 0.14, "fish.dorsal2_start": 0.68,
                 "fish.dorsal2_len": 0.08,
                 "fish.anal_height": 0.12, "fish.anal_len": 0.08,
                 "fish.pectoral": 0.75, "fish.pectoral_aspect": 0.60,
                 "fish.pelvic": 0.10, "fish.eye": 1.0,
                 "fish.back_frac": 0.46, "fish.belly_frac": 0.30,
                 "fish.pattern": "stripe", "fish.pattern_pos": 0.36,
                 "fish.pattern_width": 0.10,
                 "materials.fish_back": "skin_brown",
                 "materials.fish_flank": "skin_silver",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_dark",
                 "materials.fish_pattern": "skin_pale",
                 "materials.fish_eye": "skin_dark",
                 "biomes.ocean": 0.8, "biomes.beach": 0.5,
                 "placement.abundance": 0.25, "placement.spacing_m": 60.0,
                 "detail.despawn_m": 90.0, "detail.school_max": 3,
                 "detail.school_radius_m": 25.0,
                 "detail.water": "reef", "detail.depth_min_m": 0.5,
                 "detail.depth_max_m": 30.0, "detail.min_water_depth_m": 3.0,
                 "detail.per_100m2": 0.05,
             }),
    ),
    "bluefin-tuna": (
        "2.2 m, 2 cm — the crescent tail and the knife-thin wrist",
        base("fish", "2", name="bluefin-tuna",
             notes="Thunniform: the whole animal is a device for holding a "
                   "high-aspect crescent tail steady. The wrist is the "
                   "thinnest of any fish here, which is what the very high "
                   "width falloff is for. Real bluefin carry seven to ten "
                   "yellow finlets behind the second dorsal and three keels on "
                   "the peduncle; neither is modelled — at 110 voxels a finlet "
                   "is one voxel and a row of them reads as a serrated edge, "
                   "which is worse than a clean one.",
             **{
                 "fish.length_m": 2.2, "fish.depth_ratio": 0.26,
                 "fish.width_ratio": 0.62, "fish.depth_at": 0.38,
                 "fish.fullness": 3.4, "fish.snout": 0.30, "fish.peduncle": 0.10,
                 "fish.belly": 0.50, "fish.width_follow": 1.90,
                 "fish.section": 2.2, "fish.section_tail": 1.4,
                 "fish.head_frac": 0.26,
                 "fish.caudal_shape": "forked", "fish.caudal_len": 0.16,
                 "fish.caudal_span": 0.95, "fish.caudal_fork": 0.72,
                 "fish.dorsal_shape": "spiny", "fish.dorsal_start": 0.30,
                 "fish.dorsal_len": 0.20, "fish.dorsal_height": 0.42,
                 "fish.dorsal2_height": 0.20, "fish.dorsal2_start": 0.60,
                 "fish.dorsal2_len": 0.08,
                 "fish.anal_height": 0.22, "fish.anal_len": 0.10,
                 "fish.pectoral": 0.55, "fish.pectoral_aspect": 0.55,
                 "fish.pelvic": 0.12, "fish.eye": 1.0,
                 "fish.back_frac": 0.40, "fish.belly_frac": 0.34,
                 "fish.pattern": "none",
                 "materials.fish_back": "skin_blue",
                 "materials.fish_flank": "skin_silver",
                 "materials.fish_belly": "skin_pale",
                 "materials.fish_fin": "skin_yellow",
                 "materials.fish_pattern": "skin_dark",
                 "materials.fish_eye": "skin_dark",
                 "biomes.ocean": 1.0,
                 "placement.abundance": 0.3, "placement.spacing_m": 30.0,
                 "detail.despawn_m": 120.0, "detail.school_min": 3,
                 "detail.school_max": 20, "detail.school_radius_m": 30.0,
                 "detail.water": "ocean", "detail.depth_min_m": 1.0,
                 "detail.depth_max_m": 120.0, "detail.min_water_depth_m": 20.0,
                 "detail.per_100m2": 0.08,
             }),
    ),
    "mirror-carp": (
        "0.9 m, 2 cm — the big lake fish, deep-bodied and orange-bronze",
        base("fish", "2", name="mirror-carp",
             notes="The large freshwater one, and the counterweight to "
                   "`golden-carp`: same family, twice the length, and authored "
                   "at 2 cm rather than 1 because 90 cm at 1 cm is 90 voxels "
                   "of a fish whose smallest feature is a barbel. Wild carp "
                   "run a length-to-depth ratio of about 4 and domesticated "
                   "ones 3.2-4.8; this is 3.3. Dorsal fin base runs the last "
                   "two-thirds of the back, which is the family's field mark.",
             **{
                 "fish.length_m": 0.90, "fish.depth_ratio": 0.30,
                 "fish.width_ratio": 0.46, "fish.depth_at": 0.40,
                 "fish.fullness": 3.4, "fish.snout": 0.42, "fish.peduncle": 0.32,
                 "fish.belly": 0.54, "fish.width_follow": 1.15,
                 "fish.section": 2.0, "fish.section_tail": 1.8,
                 "fish.head_frac": 0.24,
                 "fish.caudal_shape": "forked", "fish.caudal_len": 0.20,
                 "fish.caudal_span": 1.15, "fish.caudal_fork": 0.36,
                 "fish.dorsal_shape": "sail", "fish.dorsal_start": 0.34,
                 "fish.dorsal_len": 0.42, "fish.dorsal_height": 0.28,
                 "fish.anal_height": 0.24, "fish.anal_len": 0.12,
                 "fish.pectoral": 0.35, "fish.pectoral_aspect": 1.0,
                 "fish.pelvic": 0.18, "fish.barbels": 2, "fish.barbel_len": 0.05,
                 "fish.eye": 1.0, "fish.fin_thick": 2,
                 "fish.back_frac": 0.34, "fish.belly_frac": 0.26,
                 "fish.pattern": "mottle", "fish.pattern_scale": 0.10,
                 "fish.pattern_strength": 0.22,
                 "materials.fish_back": "skin_olive",
                 "materials.fish_flank": "skin_orange",
                 "materials.fish_belly": "skin_yellow",
                 "materials.fish_fin": "skin_brown",
                 "materials.fish_pattern": "skin_dark",
                 "materials.fish_eye": "skin_dark",
                 "biomes.grassland": 0.6, "biomes.temperate_forest": 0.5,
                 "biomes.savanna": 0.3,
                 "placement.abundance": 0.2, "placement.spacing_m": 20.0,
                 "detail.despawn_m": 70.0, "detail.school_max": 4,
                 "detail.school_radius_m": 10.0,
                 "detail.water": "lake", "detail.depth_min_m": 0.5,
                 "detail.depth_max_m": 8.0, "detail.min_water_depth_m": 2.0,
                 "detail.per_100m2": 0.4,
             }),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "marine specs")
    written = 0
    for name, (blurb, changes) in SPECIES.items():
        s, rep = sm.patch(sm.default_spec(), changes)
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          label=name, width=22):
            written += 1
        print(f"  {'':<22} {blurb}")
    print(f"\n{written} of {len(SPECIES)} written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
