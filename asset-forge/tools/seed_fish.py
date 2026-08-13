"""Author the first ten fish species.

One-off, and it refuses to overwrite a spec that already exists — see
`tools/seedspec.py` for why (a seed script silently reverted a finished hero
back to its draft values, and the only reason it was recoverable is that a
backup happened to be seconds old).

WHAT THIS SET IS FOR. Ten species chosen to span the reference screenshot
rather than to cover ichthyology: an orange-and-white reef fish with bars, two
olive-green freshwater fish, a brown speckled bottom fish, a small pale round
one, something long and eel-like, something bright, and a big predator. The
point of the set is that no two of them can be mistaken for each other AT
TWENTY VOXELS, which is a much stronger constraint than looking different in a
drawing.

NO SPECIES IS UNDER 20 cm. Owner decision, 2026-08-13: rather than add a 5 mm
lattice tier for the small reef fish, enlarge the species. Two of these are
therefore bigger than the animal -- `clown-anemonefish` most obviously -- and
both say so in their own `notes`, because that note is the only thing standing
between a future reader and a well-meant "correction" back to life size.

Every one is authored at **1 cm**. See `docs/fish-shape-research.md`: at the
5 cm asset lattice a 30 cm trout is six voxels long and there is no fish there
at all, and at 2 cm it is fifteen, which cannot carry a forked tail and an eye.
1 cm nests 10:1 in the terrain lattice and 5:1 in the asset lattice, both whole
numbers, and a whole fish is a few hundred voxels — a shoal of forty is one
tenth of a single oak.

    python tools/seed_fish.py
    python tools/seed_fish.py --force     # revert them all to these drafts
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(**over):
    """A spec with every fish default, then the species' own values."""
    changes = {
        "kind": "fish",
        "resolution_cm": "1",
        # A fish is small and cheap, so it can afford to vary widely; a shoal of
        # identical animals is the single most obvious tell that something is
        # generated. These three are the ones `fish._params` actually reads.
        "variation.amount": 1.0,
        "variation.height": 0.18,
        "variation.shape": 0.14,
        "variation.proportion": 0.20,
        "detail.entity_class": "detail",
    }
    changes.update(over)
    return changes


# name -> (blurb, changes)
SPECIES = {
    # --- fresh water --------------------------------------------------------
    "brown-trout": (
        "the baseline river fish: fusiform, olive-brown, spotted, adipose fin",
        base(
            name="brown-trout",
            notes="Fusiform, the shape most fish are. Spotted rather than "
                  "striped, and the adipose fin is the thing that tells a "
                  "salmonid from every other slim brown fish at this size.",
            **{
                "fish.length_m": 0.30, "fish.depth_ratio": 0.25,
                "fish.width_ratio": 0.52, "fish.depth_at": 0.41,
                "fish.fullness": 3.0, "fish.snout": 0.34, "fish.peduncle": 0.30,
                "fish.belly": 0.54, "fish.width_follow": 1.20,
                "fish.section": 2.1, "fish.head_frac": 0.26,
                "fish.caudal_shape": "forked", "fish.caudal_len": 0.18,
                "fish.caudal_span": 1.05, "fish.caudal_fork": 0.20,
                "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.36,
                "fish.dorsal_len": 0.18, "fish.dorsal_height": 0.34,
                "fish.adipose": True,
                "fish.anal_height": 0.30, "fish.anal_len": 0.14,
                "fish.pectoral": 0.28, "fish.pelvic": 0.18,
                "fish.eye": 1.0,
                "fish.back_frac": 0.38, "fish.belly_frac": 0.26,
                "fish.pattern": "spots", "fish.pattern_count": 14,
                "fish.pattern_scale": 0.05,
                "materials.fish_back": "skin_olive",
                "materials.fish_flank": "skin_brown",
                "materials.fish_belly": "skin_pale",
                "materials.fish_fin": "skin_brown",
                "materials.fish_pattern": "skin_dark",
                "materials.fish_eye": "skin_dark",
                "biomes.temperate_forest": 0.9, "biomes.taiga": 0.6,
                "biomes.tundra_alpine": 0.35, "biomes.grassland": 0.3,
                "placement.abundance": 0.5, "placement.spacing_m": 4.0,
                "detail.despawn_m": 45.0, "detail.school_min": 1,
                "detail.school_max": 5, "detail.school_radius_m": 4.0,
                "detail.water": "river", "detail.depth_min_m": 0.3,
                "detail.depth_max_m": 3.0, "detail.min_water_depth_m": 0.5,
                "detail.per_100m2": 2.0,
            },
        ),
    ),
    "river-perch": (
        "deep olive body, seven dark bars, tall spiny dorsal",
        base(
            name="river-perch",
            notes="Compressiform, and BARRED. Vertical bars are the weed-bed "
                  "and reef pattern: they break the outline against vertical "
                  "structure, which is exactly where a perch sits.",
            **{
                "fish.length_m": 0.22, "fish.depth_ratio": 0.32,
                "fish.width_ratio": 0.42, "fish.depth_at": 0.36,
                "fish.fullness": 4.2, "fish.snout": 0.34, "fish.peduncle": 0.26,
                "fish.belly": 0.52, "fish.width_follow": 1.30,
                "fish.section": 1.7, "fish.head_frac": 0.28,
                "fish.caudal_shape": "forked", "fish.caudal_len": 0.18,
                "fish.caudal_span": 1.00, "fish.caudal_fork": 0.18,
                "fish.dorsal_shape": "spiny", "fish.dorsal_start": 0.30,
                "fish.dorsal_len": 0.30, "fish.dorsal_height": 0.50,
                "fish.anal_height": 0.32, "fish.anal_len": 0.14,
                "fish.pectoral": 0.30, "fish.pelvic": 0.22,
                "fish.eye": 1.0,
                "fish.back_frac": 0.30, "fish.belly_frac": 0.24,
                "fish.pattern": "bars", "fish.pattern_count": 5,
                "fish.pattern_width": 0.40,
                "materials.fish_back": "skin_olive",
                "materials.fish_flank": "skin_yellow",
                "materials.fish_belly": "skin_pale",
                "materials.fish_fin": "skin_red",
                "materials.fish_pattern": "skin_dark",
                "materials.fish_eye": "skin_dark",
                "biomes.temperate_forest": 0.8, "biomes.grassland": 0.6,
                "biomes.taiga": 0.3,
                "placement.abundance": 0.6, "placement.spacing_m": 3.0,
                "detail.despawn_m": 40.0, "detail.school_min": 3,
                "detail.school_max": 12, "detail.school_radius_m": 2.5,
                "detail.water": "lake", "detail.depth_min_m": 0.4,
                "detail.depth_max_m": 4.0, "detail.min_water_depth_m": 0.6,
                "detail.per_100m2": 5.0,
            },
        ),
    ),
    "pale-minnow": (
        "the small round pale one: 8 cm, silver, no markings",
        base(
            name="pale-minnow",
            notes="The plain silver one. Authored at 20 cm -- the top of a "
                  "dace's real range rather than a minnow's 8 cm -- because "
                  "20 cm is 20 voxels and 8 cm is 8, and at 8 there is no "
                  "fish there at all. Nothing but countershading and a "
                  "forked tail; it is the test of how little will do.",
            **{
                "fish.length_m": 0.20, "fish.depth_ratio": 0.30,
                "fish.width_ratio": 0.62, "fish.depth_at": 0.40,
                "fish.fullness": 3.4, "fish.snout": 0.40, "fish.peduncle": 0.34,
                "fish.belly": 0.55, "fish.width_follow": 1.05,
                "fish.section": 2.3, "fish.head_frac": 0.30,
                "fish.caudal_shape": "forked", "fish.caudal_len": 0.20,
                "fish.caudal_span": 0.95, "fish.caudal_fork": 0.30,
                "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.40,
                "fish.dorsal_len": 0.20, "fish.dorsal_height": 0.32,
                "fish.anal_height": 0.24, "fish.anal_len": 0.12,
                "fish.pectoral": 0.24, "fish.pelvic": 0.0,
                "fish.eye": 1.0,
                "fish.back_frac": 0.30, "fish.belly_frac": 0.34,
                "fish.pattern": "none",
                "materials.fish_back": "skin_olive",
                "materials.fish_flank": "skin_silver",
                "materials.fish_belly": "skin_pale",
                "materials.fish_fin": "skin_silver",
                "materials.fish_pattern": "skin_dark",
                "materials.fish_eye": "skin_dark",
                "biomes.temperate_forest": 0.7, "biomes.grassland": 0.7,
                "biomes.savanna": 0.4, "biomes.beach": 0.3,
                "placement.abundance": 0.9, "placement.spacing_m": 1.0,
                "detail.despawn_m": 30.0, "detail.school_min": 12,
                "detail.school_max": 60, "detail.school_radius_m": 1.6,
                "detail.water": "shallow", "detail.depth_min_m": 0.1,
                "detail.depth_max_m": 1.2, "detail.min_water_depth_m": 0.25,
                "detail.per_100m2": 40.0,
            },
        ),
    ),
    "river-eel": (
        "long and anguilliform: no fork, no pelvic, a ridge down the back",
        base(
            name="river-eel",
            notes="Anguilliform. Depth ratio under 0.10, snout and wrist "
                  "almost as deep as the middle, and the dorsal and anal fins "
                  "replaced by one continuous low ridge — which is why "
                  "`dorsal_shape` is a choice and not a height slider.",
            **{
                "fish.length_m": 0.70, "fish.depth_ratio": 0.085,
                "fish.width_ratio": 0.90, "fish.depth_at": 0.30,
                "fish.fullness": 1.6, "fish.snout": 0.74, "fish.peduncle": 0.52,
                "fish.belly": 0.50, "fish.width_follow": 0.60,
                "fish.section": 2.6, "fish.head_frac": 0.14,
                "fish.caudal_shape": "pointed", "fish.caudal_len": 0.06,
                "fish.caudal_span": 0.80,
                "fish.dorsal_shape": "ridge", "fish.dorsal_start": 0.28,
                "fish.dorsal_len": 0.70, "fish.dorsal_height": 0.55,
                "fish.anal_height": 0.40, "fish.anal_len": 0.45,
                "fish.pectoral": 0.55, "fish.pelvic": 0.0,
                "fish.fin_thick": 1, "fish.eye": 1.0,
                "fish.back_frac": 0.44, "fish.belly_frac": 0.30,
                "fish.pattern": "none",
                "materials.fish_back": "skin_dark",
                "materials.fish_flank": "skin_brown",
                "materials.fish_belly": "skin_yellow",
                "materials.fish_fin": "skin_brown",
                "materials.fish_pattern": "skin_dark",
                "materials.fish_eye": "skin_dark",
                "biomes.temperate_forest": 0.5, "biomes.rainforest": 0.5,
                "biomes.grassland": 0.4, "biomes.beach": 0.4,
                "placement.abundance": 0.25, "placement.spacing_m": 12.0,
                "detail.despawn_m": 35.0, "detail.school_min": 1,
                "detail.school_max": 1, "detail.school_radius_m": 0.5,
                "detail.water": "river", "detail.depth_min_m": 0.8,
                "detail.depth_max_m": 6.0, "detail.min_water_depth_m": 0.8,
                "detail.per_100m2": 0.4,
            },
        ),
    ),
    "northern-pike": (
        "sagittiform ambush predator: mass carried back toward the tail",
        base(
            name="northern-pike",
            notes="Sagittiform — the deepest point pushed back past halfway "
                  "and the dorsal fin set right over the tail, which is the "
                  "whole shape of an ambush predator that accelerates once. "
                  "Pale saddles over the back, because what hunts a pike looks "
                  "down at it.",
            **{
                "fish.length_m": 0.75, "fish.depth_ratio": 0.16,
                "fish.width_ratio": 0.55, "fish.depth_at": 0.58,
                "fish.fullness": 2.2, "fish.snout": 0.42, "fish.peduncle": 0.44,
                "fish.belly": 0.50, "fish.width_follow": 0.95,
                "fish.section": 2.3, "fish.head_frac": 0.30,
                "fish.caudal_shape": "forked", "fish.caudal_len": 0.14,
                "fish.caudal_span": 1.25, "fish.caudal_fork": 0.30,
                "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.72,
                "fish.dorsal_len": 0.18, "fish.dorsal_height": 0.75,
                "fish.anal_height": 0.62, "fish.anal_len": 0.16,
                "fish.pectoral": 0.35, "fish.pelvic": 0.22,
                "fish.fin_thick": 2, "fish.eye": 1.0,
                "fish.back_frac": 0.34, "fish.belly_frac": 0.24,
                "fish.pattern": "saddle", "fish.pattern_scale": 0.10,
                "fish.pattern_strength": 0.35,
                "materials.fish_back": "skin_olive",
                "materials.fish_flank": "skin_green",
                "materials.fish_belly": "skin_pale",
                "materials.fish_fin": "skin_olive",
                "materials.fish_pattern": "skin_yellow",
                "materials.fish_eye": "skin_dark",
                "biomes.temperate_forest": 0.5, "biomes.taiga": 0.5,
                "biomes.grassland": 0.4,
                "placement.abundance": 0.15, "placement.spacing_m": 25.0,
                "detail.despawn_m": 60.0, "detail.school_min": 1,
                "detail.school_max": 1, "detail.school_radius_m": 0.5,
                "detail.water": "lake", "detail.depth_min_m": 0.5,
                "detail.depth_max_m": 5.0, "detail.min_water_depth_m": 1.2,
                "detail.per_100m2": 0.2,
            },
        ),
    ),
    "mud-catfish": (
        "flattened brown bottom fish with barbels and a long ridge fin",
        base(
            name="mud-catfish",
            notes="Depressiform: wider than it is deep, which is what a fish "
                  "that lives ON the bottom is. Barbels off the snout, mottled "
                  "brown, and a flat underside from a belly share under 0.5.",
            **{
                "fish.length_m": 0.38, "fish.depth_ratio": 0.20,
                "fish.width_ratio": 1.25, "fish.depth_at": 0.30,
                "fish.fullness": 2.6, "fish.snout": 0.62, "fish.peduncle": 0.34,
                "fish.belly": 0.40, "fish.width_follow": 0.85,
                "fish.section": 3.0, "fish.head_frac": 0.30,
                "fish.caudal_shape": "rounded", "fish.caudal_len": 0.16,
                "fish.caudal_span": 1.05,
                "fish.dorsal_shape": "ridge", "fish.dorsal_start": 0.42,
                "fish.dorsal_len": 0.44, "fish.dorsal_height": 0.30,
                "fish.anal_height": 0.28, "fish.anal_len": 0.30,
                "fish.pectoral": 0.55, "fish.pelvic": 0.16,
                "fish.barbels": 4, "fish.barbel_len": 0.20,
                "fish.eye": 1.0,
                "fish.back_frac": 0.42, "fish.belly_frac": 0.30,
                "fish.pattern": "mottle", "fish.pattern_scale": 0.14,
                "fish.pattern_strength": 0.40,
                "materials.fish_back": "skin_brown",
                "materials.fish_flank": "skin_brown",
                "materials.fish_belly": "skin_pale",
                "materials.fish_fin": "skin_brown",
                "materials.fish_pattern": "skin_dark",
                "materials.fish_eye": "skin_dark",
                "biomes.rainforest": 0.7, "biomes.savanna": 0.5,
                "biomes.grassland": 0.4,
                "placement.abundance": 0.3, "placement.spacing_m": 8.0,
                "detail.despawn_m": 35.0, "detail.school_min": 1,
                "detail.school_max": 3, "detail.school_radius_m": 3.0,
                "detail.water": "river", "detail.depth_min_m": 1.0,
                "detail.depth_max_m": 12.0, "detail.min_water_depth_m": 1.0,
                "detail.per_100m2": 0.8,
            },
        ),
    ),
    "golden-carp": (
        "deep-bodied orange freshwater fish with two barbels",
        base(
            name="golden-carp",
            notes="Deep, slow and unmistakably orange — the freshwater answer "
                  "to the reef fish, and the species that proves the colour "
                  "scheme carries identity on its own: its outline is not far "
                  "from the perch's and nobody confuses them.",
            **{
                "fish.length_m": 0.40, "fish.depth_ratio": 0.42,
                "fish.width_ratio": 0.40, "fish.depth_at": 0.42,
                "fish.fullness": 3.6, "fish.snout": 0.40, "fish.peduncle": 0.34,
                "fish.belly": 0.55, "fish.width_follow": 1.10,
                "fish.section": 2.0, "fish.head_frac": 0.24,
                "fish.caudal_shape": "forked", "fish.caudal_len": 0.22,
                "fish.caudal_span": 1.15, "fish.caudal_fork": 0.35,
                "fish.dorsal_shape": "sail", "fish.dorsal_start": 0.32,
                "fish.dorsal_len": 0.44, "fish.dorsal_height": 0.30,
                "fish.anal_height": 0.26, "fish.anal_len": 0.14,
                "fish.pectoral": 0.30, "fish.pelvic": 0.20,
                "fish.barbels": 2, "fish.barbel_len": 0.07,
                "fish.fin_thick": 2, "fish.eye": 1.0,
                "fish.back_frac": 0.30, "fish.belly_frac": 0.26,
                "fish.pattern": "mottle", "fish.pattern_scale": 0.22,
                "fish.pattern_strength": 0.28,
                "materials.fish_back": "skin_orange",
                "materials.fish_flank": "skin_orange",
                "materials.fish_belly": "skin_yellow",
                "materials.fish_fin": "skin_orange",
                "materials.fish_pattern": "skin_pale",
                "materials.fish_eye": "skin_dark",
                "biomes.grassland": 0.6, "biomes.temperate_forest": 0.5,
                "biomes.savanna": 0.4,
                "placement.abundance": 0.35, "placement.spacing_m": 6.0,
                "detail.despawn_m": 45.0, "detail.school_min": 2,
                "detail.school_max": 8, "detail.school_radius_m": 4.0,
                "detail.water": "lake", "detail.depth_min_m": 0.4,
                "detail.depth_max_m": 4.0, "detail.min_water_depth_m": 0.8,
                "detail.per_100m2": 1.5,
            },
        ),
    ),
    # --- salt water ---------------------------------------------------------
    "clown-anemonefish": (
        "the orange-and-white one: three pale bars, rounded tail",
        base(
            name="clown-anemonefish",
            notes="The species the reference screenshot is built around. Three "
                  "wide pale bars on orange, a rounded tail and a low even "
                  "dorsal. Nothing about its OUTLINE is remarkable and it is "
                  "still the most identifiable fish in the set, which is the "
                  "whole argument for spending the voxel budget on colour.\n\n"
                  "AUTHORED AT 18 cm, NOT LIFE SIZE. A common anemonefish is "
                  "8-11 cm, which at the 1 cm lattice is ten voxels, and ten "
                  "voxels cannot hold three bars two voxels wide. 18 cm is "
                  "the large end of the genus and it is also the smallest "
                  "this fish can be and still be this fish.\n\n"
                  "THE ONE SMALL FISH IN THE LIBRARY WITH A SEX WORTH DRAWING, "
                  "and it is the strongest sexual size difference of any bony "
                  "fish here: the breeding female is 67.5 mm against the "
                  "male's 52.6 mm, measured over 134 fish in 67 breeding pairs "
                  "(Kimbe Bay). Anemonefish live in a size hierarchy where "
                  "each rank is 1.26 times the one below it, and the female is "
                  "simply the largest fish on the anemone — every one of them "
                  "started male. There is NO colour difference: sex change in "
                  "Amphiprion does not modify the pigmentation pattern, so bar "
                  "count and melanism track species and host anemone and never "
                  "sex.",
            **{
                "fish.length_m": 0.22, "fish.sex_length": 0.78,
                "fish.depth_ratio": 0.42,
                "fish.width_ratio": 0.36, "fish.depth_at": 0.36,
                "fish.fullness": 4.5, "fish.snout": 0.42, "fish.peduncle": 0.48,
                "fish.belly": 0.52, "fish.width_follow": 1.20,
                "fish.section": 1.6, "fish.head_frac": 0.26,
                "fish.caudal_shape": "rounded", "fish.caudal_len": 0.18,
                "fish.caudal_span": 0.95,
                "fish.dorsal_shape": "sail", "fish.dorsal_start": 0.24,
                "fish.dorsal_len": 0.52, "fish.dorsal_height": 0.28,
                "fish.anal_height": 0.28, "fish.anal_len": 0.20,
                "fish.pectoral": 0.34, "fish.pelvic": 0.24,
                "fish.eye": 1.0,
                "fish.back_frac": 0.0, "fish.belly_frac": 0.0,
                "fish.pattern": "bars", "fish.pattern_count": 3,
                "fish.pattern_width": 0.36,
                "materials.fish_back": "skin_orange",
                "materials.fish_flank": "skin_orange",
                "materials.fish_belly": "skin_orange",
                "materials.fish_fin": "skin_orange",
                "materials.fish_pattern": "skin_pale",
                "materials.fish_eye": "skin_dark",
                "biomes.ocean": 0.7, "biomes.beach": 0.4,
                "placement.abundance": 0.5, "placement.spacing_m": 2.0,
                "detail.despawn_m": 30.0, "detail.school_min": 2,
                "detail.school_max": 6, "detail.school_radius_m": 1.2,
                "detail.water": "reef", "detail.depth_min_m": 0.5,
                "detail.depth_max_m": 8.0, "detail.min_water_depth_m": 0.8,
                "detail.per_100m2": 6.0,
            },
        ),
    ),
    "reef-tang": (
        "a disc on edge: blue, yellow-tailed, knife-thin",
        base(
            name="reef-tang",
            notes="The extreme of compressiform: depth over half the length, "
                  "width a third of the depth, and a section exponent near 1.3 "
                  "so the back and belly are knife edges rather than curves. "
                  "This is the species the `section` slider exists for.",
            **{
                "fish.length_m": 0.22, "fish.depth_ratio": 0.58,
                "fish.width_ratio": 0.30, "fish.depth_at": 0.42,
                "fish.fullness": 5.5, "fish.snout": 0.30, "fish.peduncle": 0.22,
                "fish.belly": 0.50, "fish.width_follow": 1.40,
                "fish.section": 1.35, "fish.head_frac": 0.24,
                "fish.caudal_shape": "forked", "fish.caudal_len": 0.16,
                "fish.caudal_span": 0.62, "fish.caudal_fork": 0.45,
                "fish.dorsal_shape": "sail", "fish.dorsal_start": 0.22,
                "fish.dorsal_len": 0.58, "fish.dorsal_height": 0.30,
                "fish.anal_height": 0.26, "fish.anal_len": 0.34,
                "fish.pectoral": 0.28, "fish.pelvic": 0.14,
                "fish.eye": 1.0,
                "fish.back_frac": 0.0, "fish.belly_frac": 0.0,
                "fish.pattern": "stripe", "fish.pattern_pos": 0.60,
                "fish.pattern_width": 0.16,
                "materials.fish_back": "skin_blue",
                "materials.fish_flank": "skin_blue",
                "materials.fish_belly": "skin_blue",
                "materials.fish_fin": "skin_yellow",
                "materials.fish_pattern": "skin_yellow",
                "materials.fish_eye": "skin_dark",
                "biomes.ocean": 0.7, "biomes.beach": 0.3,
                "placement.abundance": 0.4, "placement.spacing_m": 3.0,
                "detail.despawn_m": 35.0, "detail.school_min": 1,
                "detail.school_max": 5, "detail.school_radius_m": 3.0,
                "detail.water": "reef", "detail.depth_min_m": 0.5,
                "detail.depth_max_m": 15.0, "detail.min_water_depth_m": 1.0,
                "detail.per_100m2": 3.0,
            },
        ),
    ),
    "shoal-herring": (
        "silver open-water cruiser: dark blue back, deeply forked tail",
        base(
            name="shoal-herring",
            notes="Countershading and nothing else: a dark blue back over a "
                  "silver flank over a white belly, which is what almost every "
                  "fish in open water wears and what the `back_frac` and "
                  "`belly_frac` sliders are for. Deep fork and a very slim "
                  "wrist — a fish that never stops swimming.",
            **{
                "fish.length_m": 0.20, "fish.depth_ratio": 0.22,
                "fish.width_ratio": 0.44, "fish.depth_at": 0.40,
                "fish.fullness": 3.2, "fish.snout": 0.26, "fish.peduncle": 0.16,
                "fish.belly": 0.54, "fish.width_follow": 1.45,
                "fish.section": 1.8, "fish.head_frac": 0.24,
                "fish.caudal_shape": "forked", "fish.caudal_len": 0.24,
                "fish.caudal_span": 1.35, "fish.caudal_fork": 0.60,
                "fish.dorsal_shape": "triangular", "fish.dorsal_start": 0.40,
                "fish.dorsal_len": 0.16, "fish.dorsal_height": 0.30,
                "fish.anal_height": 0.18, "fish.anal_len": 0.16,
                "fish.pectoral": 0.24, "fish.pelvic": 0.12,
                "fish.eye": 1.0,
                "fish.back_frac": 0.34, "fish.belly_frac": 0.30,
                "fish.pattern": "none",
                "materials.fish_back": "skin_blue",
                "materials.fish_flank": "skin_silver",
                "materials.fish_belly": "skin_pale",
                "materials.fish_fin": "skin_silver",
                "materials.fish_pattern": "skin_dark",
                "materials.fish_eye": "skin_dark",
                "biomes.ocean": 1.0, "biomes.beach": 0.5,
                "placement.abundance": 1.0, "placement.spacing_m": 0.8,
                "detail.despawn_m": 50.0, "detail.school_min": 20,
                "detail.school_max": 120, "detail.school_radius_m": 6.0,
                "detail.water": "ocean", "detail.depth_min_m": 1.0,
                "detail.depth_max_m": 25.0, "detail.min_water_depth_m": 2.0,
                "detail.per_100m2": 60.0,
            },
        ),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "fish specs")
    written = 0
    for name, (blurb, changes) in SPECIES.items():
        s, rep = sm.patch(sm.default_spec(), changes)
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          label=name, width=20):
            written += 1
        print(f"  {'':<20} {blurb}")
    print(f"\n{written} of {len(SPECIES)} written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
