"""Author the fresh-water fish: rivers and lakes across seven biomes.

The engine tags a river by the LAND AROUND IT (`forge/biomes.py:37-42`), so a
freshwater species' biome weights mean "which landscape's rivers and lakes hold
this". That is why one table covers grassland ponds, temperate-forest streams,
taiga rivers, alpine tarns, rainforest blackwater and savanna floodplains: they
are the same generator and the same kind, separated by weights.

LATTICE, BY THE HOUSE RULE. 1 cm for anything under about half a metre, 2 cm
from there to about a metre and a half, 5 cm above that. The rule is the
coarsest voxel at which the smallest identifying feature is still about three
voxels across, and `tools/fishprobe.py --lattice` measures it: at 5 cm the tail
fork and the eye are gone on every small species, and between them those two are
most of what makes a small object read as an animal rather than a lozenge.

THE 20 cm FLOOR (owner, 2026-08-13). No fish is authored under 0.20 m. Four
species here are genuinely smaller -- the bullhead, the stone loach, the common
minnow and the desert pupfish -- and each is authored up with the reason in its
own `notes`, exactly as `clown-anemonefish` is at 22 cm against a real 10.
Enlarging the animal was chosen over adding a lattice tier finer than 1 cm.

ONE MARKING AND NEVER TWO. A flank twelve voxels deep cannot hold a stripe and
bars without them reading as noise, and in nature they are mutually exclusive
anyway. Several species below have a second real-life mark that is simply not
drawn, and where that matters the `notes` say which one was dropped.

    python tools/seed_freshwater.py
    python tools/seed_freshwater.py --force

SIZES ARE APPROXIMATE. Every length is the approximate figure from the biome
file it came from; those are unsourced general-knowledge estimates by their own
admission, and nothing here is quoted as measured.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(res="1", **over):
    changes = {
        "kind": "fish",
        "resolution_cm": res,
        "variation.amount": 1.0,
        "variation.height": 0.18,
        "variation.shape": 0.14,
        "variation.proportion": 0.20,
        "detail.entity_class": "detail",
    }
    changes.update(over)
    return changes


def f(**kw):
    """`fish.*`, `materials.fish_*`, `detail.*`, `biomes.*`, `placement.*`."""
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials.fish_" + k[len("mat_"):]] = v
        elif k.startswith("det_"):
            out["detail." + k[len("det_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        else:
            out["fish." + k] = v
    return out


SPECIES = {
    # --- deep-bodied still-water fish ---------------------------------------
    "common-bream": (
        "0.50 m - the flattest fish here: a bronze slab with a long anal fin",
        base(name="common-bream",
             notes="THE DEPTH AND WIDTH EXTREMES AT ONCE: depth ratio 0.44 with "
                   "a width ratio of 0.30, which is a body more than three "
                   "times deeper than it is wide. It is the compressiform end "
                   "of the axis `river-eel` holds the other end of, and the "
                   "long low anal fin running half the underside is what "
                   "separates it from every other deep silver fish.",
             **f(length_m=0.50, depth_ratio=0.44, width_ratio=0.30,
                 depth_at=0.34, fullness=5.0, snout=0.26, peduncle=0.20,
                 belly=0.55, width_follow=1.40, section=1.5, head_frac=0.22,
                 caudal_shape="forked", caudal_len=0.20, caudal_span=1.10,
                 caudal_fork=0.42,
                 dorsal_shape="triangular", dorsal_start=0.48, dorsal_len=0.14,
                 dorsal_height=0.34,
                 anal_height=0.30, anal_len=0.34,
                 pectoral=0.30, pelvic=0.20, eye=1.0,
                 back_frac=0.30, belly_frac=0.24, pattern="none",
                 mat_back="skin_dark", mat_flank="skin_yellow",
                 mat_belly="skin_pale", mat_fin="skin_dark",
                 mat_eye="skin_dark",
                 bio_grassland=0.9, bio_temperate_forest=0.6,
                 place_abundance=0.5, place_spacing_m=3.0,
                 det_despawn_m=45.0, det_school_min=4, det_school_max=25,
                 det_school_radius_m=5.0, det_water="lake",
                 det_depth_min_m=1.0, det_depth_max_m=6.0,
                 det_min_water_depth_m=1.5, det_per_100m2=4.0)),
    ),
    "tench": (
        "0.45 m - thick olive body, tiny red eye, all fins rounded",
        base(name="tench",
             notes="EVERY FIN ROUNDED, which nothing else in the library is: a "
                   "tench has no fork anywhere, and at twenty voxels that "
                   "absence is more distinctive than any marking. Thick-set "
                   "rather than deep, in a dark olive-green, with one small "
                   "barbel at each mouth corner.",
             **f(length_m=0.45, depth_ratio=0.28, width_ratio=0.62,
                 depth_at=0.42, fullness=3.4, snout=0.40, peduncle=0.42,
                 belly=0.52, width_follow=1.05, section=2.5, head_frac=0.24,
                 caudal_shape="truncate", caudal_len=0.17, caudal_span=1.00,
                 dorsal_shape="triangular", dorsal_start=0.46, dorsal_len=0.16,
                 dorsal_height=0.30,
                 anal_height=0.26, anal_len=0.14,
                 pectoral=0.30, pelvic=0.22, barbels=2, barbel_len=0.05,
                 eye=1.0, back_frac=0.36, belly_frac=0.22, pattern="none",
                 mat_back="skin_olive", mat_flank="skin_olive",
                 mat_belly="skin_yellow", mat_fin="skin_olive",
                 mat_eye="skin_red",
                 bio_grassland=0.9, bio_temperate_forest=0.5,
                 place_abundance=0.4, place_spacing_m=4.0,
                 det_despawn_m=40.0, det_school_min=1, det_school_max=4,
                 det_school_radius_m=6.0, det_water="lake",
                 det_depth_min_m=0.8, det_depth_max_m=4.0,
                 det_min_water_depth_m=1.0, det_per_100m2=1.5)),
    ),
    "crucian-carp": (
        "0.30 m - deep humped golden-bronze disc with no barbels at all",
        base(name="crucian-carp",
             notes="A CARP WITHOUT BARBELS, which is the whole separation from "
                   "the shipped `golden-carp` and `mirror-carp` -- so this one "
                   "leans on outline instead: deeper, higher-backed, and with a "
                   "convex dorsal edge rather than a straight one. Rounded fins "
                   "throughout.",
             **f(length_m=0.30, depth_ratio=0.42, width_ratio=0.40,
                 depth_at=0.40, fullness=4.6, snout=0.34, peduncle=0.30,
                 belly=0.52, width_follow=1.25, section=1.9, head_frac=0.24,
                 caudal_shape="forked", caudal_len=0.17, caudal_span=1.00,
                 caudal_fork=0.16,
                 dorsal_shape="triangular", dorsal_start=0.40, dorsal_len=0.32,
                 dorsal_height=0.32,
                 anal_height=0.26, anal_len=0.14,
                 pectoral=0.28, pelvic=0.20, eye=1.0,
                 back_frac=0.34, belly_frac=0.24, pattern="none",
                 mat_back="skin_brown", mat_flank="skin_yellow",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_eye="skin_dark",
                 bio_grassland=0.9, bio_temperate_forest=0.5, bio_taiga=0.25,
                 place_abundance=0.5, place_spacing_m=2.5,
                 det_despawn_m=40.0, det_school_min=3, det_school_max=14,
                 det_school_radius_m=4.0, det_water="lake",
                 det_depth_min_m=0.5, det_depth_max_m=3.0,
                 det_min_water_depth_m=0.8, det_per_100m2=5.0)),
    ),
    "bluegill": (
        "0.22 m - very deep disc with a solid black opercular flap",
        base(name="bluegill",
             notes="A DISC ON EDGE with one hard black mark on the gill cover, "
                   "which is the only place in the freshwater set a marking "
                   "sits on the HEAD rather than the flank -- drawn here as a "
                   "single vertical bar placed forward. Faint body bars are the "
                   "species' second mark and are deliberately not drawn: one "
                   "marking and never two.",
             **f(length_m=0.22, depth_ratio=0.46, width_ratio=0.30,
                 depth_at=0.38, fullness=5.2, snout=0.30, peduncle=0.26,
                 belly=0.52, width_follow=1.35, section=1.6, head_frac=0.26,
                 caudal_shape="truncate", caudal_len=0.16, caudal_span=1.00,
                 dorsal_shape="spiny", dorsal_start=0.30, dorsal_len=0.38,
                 dorsal_height=0.40,
                 anal_height=0.34, anal_len=0.18,
                 pectoral=0.34, pelvic=0.22, eye=1.0,
                 back_frac=0.32, belly_frac=0.24,
                 pattern="bars", pattern_count=1, pattern_width=0.30,
                 mat_back="skin_olive", mat_flank="skin_green",
                 mat_belly="skin_yellow", mat_fin="skin_olive",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_grassland=0.8, bio_temperate_forest=0.7,
                 place_abundance=0.7, place_spacing_m=1.5,
                 det_despawn_m=35.0, det_school_min=5, det_school_max=30,
                 det_school_radius_m=3.0, det_water="lake",
                 det_depth_min_m=0.4, det_depth_max_m=3.0,
                 det_min_water_depth_m=0.6, det_per_100m2=12.0)),
    ),
    # --- silver mid-water shoalers ------------------------------------------
    "roach": (
        "0.25 m - silver, red eye, orange-red fins",
        base(name="roach",
             notes="AUTHORED AS ONE HALF OF A PAIR with `rudd`, and the pair is "
                   "the hardest separation in the freshwater set: the two are "
                   "the same silver fish, and what tells them apart is that a "
                   "rudd's mouth turns UP and its dorsal fin sits FURTHER BACK. "
                   "Both of those are expressible -- `snout` and `dorsal_start` "
                   "-- and nothing else about either is different, which makes "
                   "this the freshwater equivalent of the guillemot and the "
                   "razorbill.",
             **f(length_m=0.25, depth_ratio=0.30, width_ratio=0.42,
                 depth_at=0.40, fullness=4.0, snout=0.34, peduncle=0.26,
                 belly=0.52, width_follow=1.30, section=1.9, head_frac=0.24,
                 caudal_shape="forked", caudal_len=0.19, caudal_span=1.05,
                 caudal_fork=0.34,
                 dorsal_shape="triangular", dorsal_start=0.44, dorsal_len=0.16,
                 dorsal_height=0.36,
                 anal_height=0.28, anal_len=0.14,
                 pectoral=0.28, pelvic=0.20, eye=1.0,
                 back_frac=0.32, belly_frac=0.28, pattern="none",
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_red",
                 mat_eye="skin_red",
                 bio_grassland=1.0, bio_temperate_forest=0.6,
                 place_abundance=0.9, place_spacing_m=1.0,
                 det_despawn_m=45.0, det_school_min=8, det_school_max=60,
                 det_school_radius_m=4.0, det_water="any",
                 det_depth_min_m=0.4, det_depth_max_m=4.0,
                 det_min_water_depth_m=0.6, det_per_100m2=20.0)),
    ),
    "rudd": (
        "0.28 m - as roach, but the mouth turns up and the dorsal sits back",
        base(name="rudd",
             notes="THE ROACH'S PAIR. Two changes and nothing else: the snout "
                   "is shallower (an upturned mouth) and the dorsal fin starts "
                   "0.08 further back, which puts it behind the pelvic rather "
                   "than over it. If those two cannot be told apart on a "
                   "contact sheet, that is a real measurement of what this "
                   "generator resolves at twenty-eight voxels, and it belongs "
                   "to the owner.",
             **f(length_m=0.28, depth_ratio=0.33, width_ratio=0.40,
                 depth_at=0.40, fullness=4.2, snout=0.22, peduncle=0.26,
                 belly=0.56, width_follow=1.30, section=1.9, head_frac=0.24,
                 caudal_shape="forked", caudal_len=0.19, caudal_span=1.05,
                 caudal_fork=0.34,
                 dorsal_shape="triangular", dorsal_start=0.52, dorsal_len=0.16,
                 dorsal_height=0.36,
                 anal_height=0.30, anal_len=0.16,
                 pectoral=0.28, pelvic=0.20, eye=1.0,
                 back_frac=0.34, belly_frac=0.28, pattern="none",
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_yellow", mat_fin="skin_red",
                 mat_eye="skin_yellow",
                 bio_grassland=1.0, bio_temperate_forest=0.5,
                 place_abundance=0.7, place_spacing_m=1.2,
                 det_despawn_m=45.0, det_school_min=6, det_school_max=40,
                 det_school_radius_m=4.0, det_water="lake",
                 det_depth_min_m=0.3, det_depth_max_m=3.0,
                 det_min_water_depth_m=0.6, det_per_100m2=14.0)),
    ),
    "common-dace": (
        "0.22 m - slim silver with a concave dorsal edge",
        base(name="common-dace",
             notes="THE SMALL PLAIN ONE, and it earns its place by being the "
                   "only fish here with a CONCAVE dorsal edge -- the fin dips "
                   "in the middle rather than bulging. That is one shape entry "
                   "and it separates a dace from a roach at a glance in a way "
                   "colour cannot, because both are silver.",
             **f(length_m=0.22, depth_ratio=0.22, width_ratio=0.48,
                 depth_at=0.38, fullness=3.4, snout=0.30, peduncle=0.24,
                 belly=0.50, width_follow=1.25, section=2.0, head_frac=0.24,
                 caudal_shape="forked", caudal_len=0.20, caudal_span=1.10,
                 caudal_fork=0.40,
                 dorsal_shape="sail", dorsal_start=0.44, dorsal_len=0.14,
                 dorsal_height=0.30,
                 anal_height=0.26, anal_len=0.12,
                 pectoral=0.26, pelvic=0.18, eye=1.0,
                 back_frac=0.30, belly_frac=0.30, pattern="none",
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_eye="skin_dark",
                 bio_temperate_forest=0.9, bio_grassland=0.7,
                 place_abundance=1.0, place_spacing_m=0.8,
                 det_despawn_m=40.0, det_school_min=10, det_school_max=80,
                 det_school_radius_m=3.5, det_water="river",
                 det_depth_min_m=0.3, det_depth_max_m=2.0,
                 det_min_water_depth_m=0.4, det_per_100m2=30.0)),
    ),
    "chub": (
        "0.45 m - blunt heavy silver body with a very large white mouth",
        base(name="chub",
             notes="A BLUNT HEAD ON A THICK BODY, which is `snout` at 0.62 -- "
                   "the highest in the freshwater set after the eel. Everything "
                   "else about a chub is generic silver fish, so that blunt "
                   "front end plus a big pale mouth is the entire read.",
             **f(length_m=0.45, depth_ratio=0.26, width_ratio=0.56,
                 depth_at=0.38, fullness=3.2, snout=0.62, peduncle=0.28,
                 belly=0.52, width_follow=1.15, section=2.3, head_frac=0.26,
                 caudal_shape="forked", caudal_len=0.19, caudal_span=1.05,
                 caudal_fork=0.30,
                 dorsal_shape="triangular", dorsal_start=0.44, dorsal_len=0.14,
                 dorsal_height=0.32,
                 anal_height=0.28, anal_len=0.14,
                 pectoral=0.30, pelvic=0.20, eye=1.0,
                 back_frac=0.34, belly_frac=0.26, pattern="none",
                 mat_back="skin_dark", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_red",
                 mat_eye="skin_yellow",
                 bio_temperate_forest=0.9, bio_grassland=0.7,
                 place_abundance=0.4, place_spacing_m=5.0,
                 det_despawn_m=45.0, det_school_min=1, det_school_max=6,
                 det_school_radius_m=6.0, det_water="river",
                 det_depth_min_m=0.4, det_depth_max_m=3.0,
                 det_min_water_depth_m=0.8, det_per_100m2=2.0)),
    ),
    "barbel": (
        "0.70 m - long low flat-bellied bottom fish with four barbels",
        base(name="barbel",
             notes="A FLAT UNDERSIDE UNDER AN ARCHED BACK, which is `belly` at "
                   "0.34 -- well below the 0.5 that puts the axis in the middle "
                   "-- and it is what every bottom-living fish has and no "
                   "mid-water fish does, plus a very high first dorsal.\n\n"
                   "TWO BARBELS, NOT THE FOUR THE ANIMAL HAS, AND THE REASON IS "
                   "A REAL DEFECT. A barbel is drawn as a face-connected thread "
                   "starting on a SNOUT voxel; on a narrow head the outer pair "
                   "starts beside the head rather than on it and comes away as "
                   "a loose piece. Measured over eight seeds: four barbels on "
                   "this body fail on one to two of them at any length tried, "
                   "and two barbels pass on all eight. Every shipped "
                   "four-barbel fish is a WIDE flat-headed species -- "
                   "`mud-catfish` is at width ratio 1.25 against this fish's "
                   "0.68 -- which is why the defect had never appeared. This "
                   "spec works around it; the fix belongs in `forge/fish.py`.",
             **f(length_m=0.70, depth_ratio=0.21, width_ratio=0.68,
                 depth_at=0.40, fullness=3.0, snout=0.44, peduncle=0.28,
                 belly=0.34, width_follow=1.10, section=2.6, head_frac=0.26,
                 caudal_shape="forked", caudal_len=0.20, caudal_span=1.15,
                 caudal_fork=0.42,
                 dorsal_shape="sail", dorsal_start=0.38, dorsal_len=0.14,
                 dorsal_height=0.52,
                 anal_height=0.26, anal_len=0.12,
                 pectoral=0.34, pelvic=0.22, barbels=2, barbel_len=0.06,
                 eye=1.0, back_frac=0.38, belly_frac=0.22, pattern="none",
                 mat_back="skin_brown", mat_flank="skin_yellow",
                 mat_belly="skin_pale", mat_fin="skin_orange",
                 mat_eye="skin_dark",
                 bio_temperate_forest=0.8, bio_grassland=0.7,
                 place_abundance=0.3, place_spacing_m=8.0,
                 det_despawn_m=50.0, det_school_min=1, det_school_max=8,
                 det_school_radius_m=8.0, det_water="river",
                 det_depth_min_m=0.8, det_depth_max_m=4.0,
                 det_min_water_depth_m=1.0, det_per_100m2=1.5)),
    ),
    "common-minnow": (
        "0.20 m - tiny silver-olive shoaler with a broken dark side stripe",
        base(name="common-minnow",
             notes="AUTHORED AT 0.20 m AGAINST A REAL 9 cm, which is the "
                   "library's floor: at 1 cm nine voxels is a dash, not a fish. "
                   "Recorded so it is not shrunk back.\n\n"
                   "It sits directly beside the shipped `pale-minnow`, which is "
                   "plain silver, and the difference is one broken lateral "
                   "stripe -- so the pair tests whether a single marking at "
                   "twenty voxels is worth having. It should be: a stripe is "
                   "the open-water schooling mark and it is what turns a shoal "
                   "into a pattern.",
             **f(length_m=0.20, depth_ratio=0.22, width_ratio=0.50,
                 depth_at=0.40, fullness=3.4, snout=0.36, peduncle=0.28,
                 belly=0.52, width_follow=1.20, section=2.1, head_frac=0.26,
                 caudal_shape="forked", caudal_len=0.20, caudal_span=1.05,
                 caudal_fork=0.32,
                 dorsal_shape="triangular", dorsal_start=0.46, dorsal_len=0.14,
                 dorsal_height=0.30,
                 anal_height=0.26, anal_len=0.12,
                 pectoral=0.26, pelvic=0.18, eye=1.0,
                 back_frac=0.32, belly_frac=0.28,
                 pattern="stripe", pattern_width=0.20, pattern_pos=0.46,
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_temperate_forest=1.0, bio_taiga=0.6, bio_grassland=0.5,
                 bio_tundra_alpine=0.3,
                 place_abundance=1.0, place_spacing_m=0.6,
                 det_despawn_m=35.0, det_school_min=15, det_school_max=120,
                 det_school_radius_m=3.0, det_water="river",
                 det_depth_min_m=0.2, det_depth_max_m=1.5,
                 det_min_water_depth_m=0.3, det_per_100m2=50.0)),
    ),
    # --- predators -----------------------------------------------------------
    "zander": (
        "0.70 m - a pike's head on a perch's body, glassy eye",
        base(name="zander",
             notes="EXACTLY WHAT ITS NAME SAYS, and it is a shape the library "
                   "did not have: the long flat head and pointed snout of "
                   "`northern-pike` on the deep barred body and spiny first "
                   "dorsal of `river-perch`. Faint bars rather than a perch's "
                   "hard ones, and a pale reflective eye.",
             **f(length_m=0.70, depth_ratio=0.22, width_ratio=0.46,
                 depth_at=0.42, fullness=3.4, snout=0.18, peduncle=0.22,
                 belly=0.50, width_follow=1.30, section=1.9, head_frac=0.30,
                 caudal_shape="forked", caudal_len=0.18, caudal_span=1.05,
                 caudal_fork=0.30,
                 dorsal_shape="spiny", dorsal_start=0.32, dorsal_len=0.22,
                 dorsal_height=0.44,
                 dorsal2_height=0.30, dorsal2_start=0.60, dorsal2_len=0.16,
                 anal_height=0.26, anal_len=0.14,
                 pectoral=0.28, pelvic=0.20, eye=1.0,
                 back_frac=0.32, belly_frac=0.24,
                 pattern="bars", pattern_count=8, pattern_width=0.28,
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_olive",
                 mat_pattern="skin_dark", mat_eye="skin_pale",
                 bio_grassland=0.9, bio_temperate_forest=0.5,
                 place_abundance=0.2, place_spacing_m=15.0,
                 det_despawn_m=60.0, det_school_min=1, det_school_max=3,
                 det_school_radius_m=10.0, det_water="lake",
                 det_depth_min_m=1.0, det_depth_max_m=8.0,
                 det_min_water_depth_m=2.0, det_per_100m2=0.6)),
    ),
    "wels-catfish": (
        "2.00 m at 5 cm - enormous flat head, six barbels, half-body anal fin",
        base(res="5", name="wels-catfish",
             notes="THE BIGGEST FRESHWATER FISH IN THE LIBRARY and the one that "
                   "sets the freshwater lattice ceiling: 2 m at 5 cm is forty "
                   "voxels, which is where an anal fin running half the body "
                   "still has a shape.\n\n"
                   "A TINY DORSAL AND AN ENORMOUS ANAL FIN is the whole "
                   "outline, and it is a combination nothing else here has. Six "
                   "barbels; the generator caps them at four, so four are drawn "
                   "and the count is a stylisation.",
             **f(length_m=2.00, depth_ratio=0.16, width_ratio=1.10,
                 depth_at=0.30, fullness=3.0, snout=0.60, peduncle=0.38,
                 belly=0.40, width_follow=1.05, section=2.8, head_frac=0.28,
                 caudal_shape="rounded", caudal_len=0.14, caudal_span=0.90,
                 dorsal_shape="ridge", dorsal_start=0.30, dorsal_len=0.06,
                 dorsal_height=0.18,
                 anal_height=0.24, anal_len=0.48,
                 pectoral=0.34, pelvic=0.16, barbels=4, barbel_len=0.22,
                 eye=1.0, fin_thick=2,
                 back_frac=0.38, belly_frac=0.24,
                 pattern="mottle", pattern_scale=0.10, pattern_strength=0.45,
                 mat_back="skin_dark", mat_flank="skin_olive",
                 mat_belly="skin_pale", mat_fin="skin_dark",
                 mat_pattern="skin_pale", mat_eye="skin_dark",
                 bio_grassland=0.8, bio_temperate_forest=0.4,
                 place_abundance=0.05, place_spacing_m=60.0,
                 det_despawn_m=120.0, det_school_min=1, det_school_max=1,
                 det_school_radius_m=1.0, det_water="river",
                 det_depth_min_m=2.0, det_depth_max_m=15.0,
                 det_min_water_depth_m=3.0, det_per_100m2=0.05)),
    ),
    "burbot": (
        "0.55 m - the only freshwater cod: one chin barbel, two long dorsals",
        base(name="burbot",
             notes="A COD SHAPE IN FRESH WATER, which is a long tapering body "
                   "with TWO dorsal fins, the second running most of the back, "
                   "and a single barbel on the chin. Marbled dark olive. The "
                   "second dorsal is what the `dorsal2` rows exist for outside "
                   "the sharks, and this is the only bony fish in the library "
                   "that needs them long rather than as a nub.",
             **f(length_m=0.55, depth_ratio=0.15, width_ratio=0.78,
                 depth_at=0.32, fullness=3.0, snout=0.52, peduncle=0.44,
                 belly=0.46, width_follow=1.05, section=2.5, head_frac=0.24,
                 caudal_shape="rounded", caudal_len=0.14, caudal_span=0.85,
                 dorsal_shape="ridge", dorsal_start=0.26, dorsal_len=0.10,
                 dorsal_height=0.26,
                 dorsal2_height=0.24, dorsal2_start=0.42, dorsal2_len=0.40,
                 anal_height=0.22, anal_len=0.38,
                 pectoral=0.30, pelvic=0.16, barbels=1, barbel_len=0.06,
                 eye=1.0, back_frac=0.36, belly_frac=0.22,
                 pattern="mottle", pattern_scale=0.09, pattern_strength=0.50,
                 mat_back="skin_brown", mat_flank="skin_olive",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_taiga=1.0, bio_temperate_forest=0.5,
                 bio_tundra_alpine=0.3,
                 place_abundance=0.2, place_spacing_m=12.0,
                 det_despawn_m=50.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=8.0, det_water="lake",
                 det_depth_min_m=2.0, det_depth_max_m=20.0,
                 det_min_water_depth_m=2.5, det_per_100m2=0.5)),
    ),
    "smallmouth-bass": (
        "0.40 m - bronze-olive with faint bars and a jaw ending under the eye",
        base(name="smallmouth-bass",
             notes="A NORTH AMERICAN COUNTERPART TO THE PERCH, and deliberately "
                   "softer: the bars are faint rather than hard, the first "
                   "dorsal is lower, and the body is less deep. Where "
                   "`river-perch` is a hard-edged fish, this one is a bronze "
                   "haze with structure in it.",
             **f(length_m=0.40, depth_ratio=0.28, width_ratio=0.44,
                 depth_at=0.38, fullness=3.8, snout=0.28, peduncle=0.26,
                 belly=0.52, width_follow=1.25, section=1.9, head_frac=0.30,
                 caudal_shape="forked", caudal_len=0.18, caudal_span=1.00,
                 caudal_fork=0.18,
                 dorsal_shape="spiny", dorsal_start=0.32, dorsal_len=0.34,
                 dorsal_height=0.34,
                 anal_height=0.30, anal_len=0.14,
                 pectoral=0.30, pelvic=0.20, eye=1.0,
                 back_frac=0.32, belly_frac=0.24,
                 pattern="bars", pattern_count=9, pattern_width=0.22,
                 mat_back="skin_olive", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_olive",
                 mat_pattern="skin_dark", mat_eye="skin_red",
                 bio_temperate_forest=0.9, bio_grassland=0.6,
                 place_abundance=0.4, place_spacing_m=5.0,
                 det_despawn_m=45.0, det_school_min=1, det_school_max=5,
                 det_school_radius_m=6.0, det_water="river",
                 det_depth_min_m=0.6, det_depth_max_m=5.0,
                 det_min_water_depth_m=1.0, det_per_100m2=2.5)),
    ),
    # --- salmonids and cold water -------------------------------------------
    "atlantic-salmon": (
        "0.90 m at 2 cm - powerful, deeply forked, narrow wrist, X-shaped spots",
        base(res="2", name="atlantic-salmon",
             notes="A TROUT BUILT FOR THE SEA: the same fusiform outline as the "
                   "shipped `brown-trout` with a much narrower tail wrist "
                   "(`peduncle` 0.18 against 0.30) and a deeper fork. That "
                   "wrist is what a fish that never stops swimming has, and it "
                   "is the cheapest single cue separating the two.\n\n"
                   "Spots ABOVE THE LATERAL LINE ONLY is the real field mark "
                   "and the generator has no half-flank marking; the spot count "
                   "is kept low instead so the flank stays mostly clean.",
             **f(length_m=0.90, depth_ratio=0.22, width_ratio=0.52,
                 depth_at=0.40, fullness=3.0, snout=0.30, peduncle=0.18,
                 belly=0.52, width_follow=1.35, section=2.1, head_frac=0.24,
                 caudal_shape="forked", caudal_len=0.20, caudal_span=1.25,
                 caudal_fork=0.34,
                 dorsal_shape="triangular", dorsal_start=0.38, dorsal_len=0.16,
                 dorsal_height=0.34, adipose=True,
                 anal_height=0.28, anal_len=0.14,
                 pectoral=0.28, pelvic=0.18, eye=1.0,
                 back_frac=0.34, belly_frac=0.28,
                 pattern="spots", pattern_count=8, pattern_scale=0.04,
                 mat_back="skin_dark", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_taiga=0.9, bio_temperate_forest=0.7,
                 bio_tundra_alpine=0.3,
                 place_abundance=0.2, place_spacing_m=10.0,
                 det_despawn_m=60.0, det_school_min=1, det_school_max=6,
                 det_school_radius_m=10.0, det_water="river",
                 det_depth_min_m=0.5, det_depth_max_m=6.0,
                 det_min_water_depth_m=1.0, det_per_100m2=0.8)),
    ),
    "brook-trout": (
        "0.28 m - dark olive with white-edged lower fins",
        base(name="brook-trout",
             notes="THE FIN EDGES ARE THE RELIABLE MARK and the generator "
                   "cannot draw a fin edge, so this one leans on the "
                   "alternative: PALE spots on a DARK ground, which is the "
                   "inverse of the brown trout's dark-on-pale and is a real "
                   "difference between the two animals. `materials.fish_fin` is "
                   "set pale so the fins carry some of the white.",
             **f(length_m=0.28, depth_ratio=0.26, width_ratio=0.52,
                 depth_at=0.42, fullness=3.0, snout=0.34, peduncle=0.30,
                 belly=0.54, width_follow=1.20, section=2.1, head_frac=0.26,
                 caudal_shape="truncate", caudal_len=0.17, caudal_span=1.00,
                 dorsal_shape="triangular", dorsal_start=0.36, dorsal_len=0.18,
                 dorsal_height=0.34, adipose=True,
                 anal_height=0.30, anal_len=0.14,
                 pectoral=0.28, pelvic=0.18, eye=1.0,
                 back_frac=0.40, belly_frac=0.22,
                 pattern="spots", pattern_count=16, pattern_scale=0.05,
                 mat_back="skin_dark", mat_flank="skin_olive",
                 mat_belly="skin_orange", mat_fin="skin_pale",
                 mat_pattern="skin_pale", mat_eye="skin_dark",
                 bio_taiga=1.0, bio_temperate_forest=0.6,
                 bio_tundra_alpine=0.4,
                 place_abundance=0.4, place_spacing_m=4.0,
                 det_despawn_m=45.0, det_school_min=1, det_school_max=6,
                 det_school_radius_m=4.0, det_water="river",
                 det_depth_min_m=0.2, det_depth_max_m=2.5,
                 det_min_water_depth_m=0.4, det_per_100m2=3.0)),
    ),
    "rainbow-trout": (
        "0.40 m - silver with a broad pink lateral band and dense fine spots",
        base(name="rainbow-trout",
             notes="THE THIRD TROUT, and the one whose marking is a STRIPE "
                   "rather than spots -- a broad pink band along the flank. "
                   "`brown-trout` is spotted, `brook-trout` is pale-spotted on "
                   "dark, and this one is striped, so the three cover the three "
                   "marking mechanisms a salmonid can wear without any two "
                   "reading as the same animal.\n\n"
                   "In life it carries both the band AND dense spots; only the "
                   "band is drawn, because one marking and never two.",
             **f(length_m=0.40, depth_ratio=0.24, width_ratio=0.50,
                 depth_at=0.42, fullness=3.0, snout=0.32, peduncle=0.26,
                 belly=0.52, width_follow=1.25, section=2.1, head_frac=0.24,
                 caudal_shape="truncate", caudal_len=0.18, caudal_span=1.05,
                 dorsal_shape="triangular", dorsal_start=0.38, dorsal_len=0.16,
                 dorsal_height=0.34, adipose=True,
                 anal_height=0.28, anal_len=0.14,
                 pectoral=0.28, pelvic=0.18, eye=1.0,
                 back_frac=0.34, belly_frac=0.26,
                 pattern="stripe", pattern_width=0.26, pattern_pos=0.48,
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_pattern="plume_rufous", mat_eye="skin_dark",
                 bio_temperate_forest=0.9, bio_taiga=0.6,
                 bio_tundra_alpine=0.35, bio_grassland=0.3,
                 place_abundance=0.45, place_spacing_m=4.0,
                 det_despawn_m=50.0, det_school_min=2, det_school_max=10,
                 det_school_radius_m=5.0, det_water="river",
                 det_depth_min_m=0.3, det_depth_max_m=4.0,
                 det_min_water_depth_m=0.6, det_per_100m2=3.0)),
    ),
    "arctic-char": (
        "0.50 m at 2 cm - dark blue-green back, PALE spots, orange belly",
        base(res="2", name="arctic-char",
             notes="THE INVERSE OF A TROUT, stated as a colour claim rather "
                   "than a shape one: pale spots on a dark ground where a trout "
                   "is dark on pale. The tundra file makes exactly that point "
                   "and it is the whole separation at twenty-five voxels. Deep "
                   "orange-red belly in spawning condition, which is what is "
                   "drawn here because the dull phase is a grey fish.",
             **f(length_m=0.50, depth_ratio=0.22, width_ratio=0.52,
                 depth_at=0.42, fullness=3.0, snout=0.32, peduncle=0.24,
                 belly=0.54, width_follow=1.25, section=2.1, head_frac=0.24,
                 caudal_shape="forked", caudal_len=0.19, caudal_span=1.10,
                 caudal_fork=0.28,
                 dorsal_shape="triangular", dorsal_start=0.38, dorsal_len=0.16,
                 dorsal_height=0.32, adipose=True,
                 anal_height=0.28, anal_len=0.14,
                 pectoral=0.28, pelvic=0.18, eye=1.0,
                 back_frac=0.42, belly_frac=0.30,
                 pattern="spots", pattern_count=12, pattern_scale=0.05,
                 mat_back="skin_blue", mat_flank="skin_olive",
                 mat_belly="skin_orange", mat_fin="skin_pale",
                 mat_pattern="skin_pale", mat_eye="skin_dark",
                 bio_tundra_alpine=1.0, bio_taiga=0.8,
                 place_abundance=0.3, place_spacing_m=6.0,
                 det_despawn_m=55.0, det_school_min=2, det_school_max=12,
                 det_school_radius_m=8.0, det_water="lake",
                 det_depth_min_m=1.0, det_depth_max_m=25.0,
                 det_min_water_depth_m=2.0, det_per_100m2=1.5)),
    ),
    "arctic-grayling": (
        "0.40 m - the sail dorsal IS the species",
        base(name="arctic-grayling",
             notes="THE TALLEST AND LONGEST DORSAL FIN IN THE LIBRARY: height "
                   "1.30 of body depth over 0.34 of the body's length. Nothing "
                   "else comes close, and the taiga file says the fin's AREA is "
                   "what identifies the species rather than its thickness -- so "
                   "a one-voxel sail is the correct trade here even though it "
                   "will look like paper from the side.",
             **f(length_m=0.40, depth_ratio=0.24, width_ratio=0.46,
                 depth_at=0.40, fullness=3.2, snout=0.30, peduncle=0.24,
                 belly=0.52, width_follow=1.30, section=2.0, head_frac=0.22,
                 caudal_shape="forked", caudal_len=0.19, caudal_span=1.10,
                 caudal_fork=0.32,
                 dorsal_shape="sail", dorsal_start=0.26, dorsal_len=0.34,
                 dorsal_height=1.30, adipose=True,
                 anal_height=0.28, anal_len=0.14,
                 pectoral=0.28, pelvic=0.18, eye=1.0,
                 back_frac=0.34, belly_frac=0.26,
                 pattern="spots", pattern_count=7, pattern_scale=0.04,
                 mat_back="plume_lilac", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="plume_lilac",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_taiga=1.0, bio_tundra_alpine=0.6,
                 bio_temperate_forest=0.3,
                 place_abundance=0.35, place_spacing_m=5.0,
                 det_despawn_m=50.0, det_school_min=2, det_school_max=15,
                 det_school_radius_m=6.0, det_water="river",
                 det_depth_min_m=0.4, det_depth_max_m=3.0,
                 det_min_water_depth_m=0.6, det_per_100m2=3.0)),
    ),
    "taimen": (
        "1.50 m at 2 cm - the giant salmonid: long, blunt-headed, red tail",
        base(res="2", name="taimen",
             notes="A SALMON AT FIVE TIMES THE MASS, and the biggest thing in a "
                   "taiga river. Long and heavy rather than deep, with a big "
                   "blunt head, a fine peppering of small dark spots and a "
                   "distinctly red tail and anal fin -- which is a fin colour "
                   "doing identification work, something only the perch and the "
                   "roach otherwise use here.",
             **f(length_m=1.50, depth_ratio=0.17, width_ratio=0.58,
                 depth_at=0.42, fullness=2.8, snout=0.42, peduncle=0.24,
                 belly=0.50, width_follow=1.25, section=2.2, head_frac=0.26,
                 caudal_shape="forked", caudal_len=0.18, caudal_span=1.15,
                 caudal_fork=0.30,
                 dorsal_shape="triangular", dorsal_start=0.40, dorsal_len=0.14,
                 dorsal_height=0.32, adipose=True,
                 anal_height=0.26, anal_len=0.12,
                 pectoral=0.28, pelvic=0.18, eye=1.0, fin_thick=2,
                 back_frac=0.36, belly_frac=0.24,
                 pattern="spots", pattern_count=18, pattern_scale=0.025,
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_red",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_taiga=1.0,
                 place_abundance=0.08, place_spacing_m=40.0,
                 det_despawn_m=90.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=15.0, det_water="river",
                 det_depth_min_m=1.0, det_depth_max_m=8.0,
                 det_min_water_depth_m=1.5, det_per_100m2=0.2)),
    ),
    "bullhead": (
        "0.20 m - flat wide head, huge fanned pectorals, sits on stones",
        base(name="bullhead",
             notes="AUTHORED AT 0.20 m AGAINST A REAL 12 cm, the library's "
                   "floor. Recorded so it is not shrunk.\n\n"
                   "THE WIDEST BODY-TO-DEPTH RATIO OF ANY SMALL FISH HERE at "
                   "1.35 -- flattened TOP TO BOTTOM rather than side to side, "
                   "which is the third end of the `width_ratio` axis and one "
                   "the freshwater set otherwise never uses. It has no swim "
                   "bladder and sits on the bottom, so `depth_min_m` puts it "
                   "there and the huge pectorals fan out sideways.",
             **f(length_m=0.20, depth_ratio=0.20, width_ratio=1.35,
                 depth_at=0.26, fullness=3.6, snout=0.72, peduncle=0.34,
                 belly=0.40, width_follow=0.85, section=2.8, head_frac=0.34,
                 caudal_shape="rounded", caudal_len=0.16, caudal_span=0.90,
                 dorsal_shape="ridge", dorsal_start=0.36, dorsal_len=0.34,
                 dorsal_height=0.28,
                 anal_height=0.22, anal_len=0.20,
                 pectoral=0.75, pectoral_aspect=1.30, pelvic=0.20, eye=1.0,
                 back_frac=0.40, belly_frac=0.20,
                 pattern="mottle", pattern_scale=0.14, pattern_strength=0.45,
                 mat_back="skin_brown", mat_flank="skin_olive",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_temperate_forest=1.0, bio_taiga=0.5,
                 place_abundance=0.5, place_spacing_m=1.5,
                 det_despawn_m=30.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=2.0, det_water="river",
                 det_depth_min_m=0.2, det_depth_max_m=1.5,
                 det_min_water_depth_m=0.25, det_per_100m2=6.0)),
    ),
    # --- rainforest ----------------------------------------------------------
    "arapaima": (
        "2.50 m at 5 cm - a cylinder with the fins pushed right aft, red rear",
        base(res="5", name="arapaima",
             notes="A FIFTY-VOXEL CYLINDER WITH ITS FINS AT THE BACK, which is "
                   "an extreme version of what makes a pike a pike -- but on a "
                   "fish four times the length and with a rounded tail rather "
                   "than a forked one. The broad red wash over the back third "
                   "is drawn as the FLANK colour aft, using a saddle marking to "
                   "put it there.",
             **f(length_m=2.50, depth_ratio=0.19, width_ratio=0.62,
                 depth_at=0.46, fullness=2.6, snout=0.50, peduncle=0.56,
                 belly=0.50, width_follow=1.00, section=2.5, head_frac=0.22,
                 caudal_shape="rounded", caudal_len=0.13, caudal_span=1.05,
                 dorsal_shape="ridge", dorsal_start=0.74, dorsal_len=0.20,
                 dorsal_height=0.34,
                 anal_height=0.32, anal_len=0.20,
                 pectoral=0.26, pelvic=0.16, eye=1.0, fin_thick=2,
                 back_frac=0.36, belly_frac=0.22,
                 pattern="saddle", pattern_scale=0.16, pattern_strength=0.35,
                 mat_back="skin_olive", mat_flank="skin_dark",
                 mat_belly="skin_pale", mat_fin="skin_red",
                 mat_pattern="skin_red", mat_eye="skin_dark",
                 bio_rainforest=1.0,
                 place_abundance=0.06, place_spacing_m=50.0,
                 det_despawn_m=140.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=15.0, det_water="river",
                 det_depth_min_m=0.5, det_depth_max_m=10.0,
                 det_min_water_depth_m=2.0, det_per_100m2=0.1)),
    ),
    "red-bellied-piranha": (
        "0.28 m - a steep-foreheaded silver disc with a solid red throat",
        base(name="red-bellied-piranha",
             notes="A BLUNT VERTICAL FOREHEAD ON A DEEP DISC, which is `snout` "
                   "at 0.12 -- the shallowest in the library -- combined with a "
                   "depth ratio of 0.46. That near-vertical face is the "
                   "silhouette, and the solid orange-red throat and belly is "
                   "the colour. Fine speckling on the flank is dropped: one "
                   "marking and never two, and the belly is the one that reads.",
             **f(length_m=0.28, depth_ratio=0.46, width_ratio=0.32,
                 depth_at=0.34, fullness=5.4, snout=0.12, peduncle=0.28,
                 belly=0.56, width_follow=1.35, section=1.6, head_frac=0.26,
                 caudal_shape="truncate", caudal_len=0.17, caudal_span=1.05,
                 dorsal_shape="triangular", dorsal_start=0.42, dorsal_len=0.20,
                 dorsal_height=0.30, adipose=True,
                 anal_height=0.34, anal_len=0.24,
                 pectoral=0.28, pelvic=0.20, eye=1.0,
                 back_frac=0.30, belly_frac=0.40, pattern="none",
                 mat_back="skin_dark", mat_flank="skin_silver",
                 mat_belly="skin_red", mat_fin="skin_dark",
                 mat_eye="skin_red",
                 bio_rainforest=1.0,
                 place_abundance=0.6, place_spacing_m=1.5,
                 det_despawn_m=40.0, det_school_min=8, det_school_max=40,
                 det_school_radius_m=4.0, det_water="river",
                 det_depth_min_m=0.4, det_depth_max_m=4.0,
                 det_min_water_depth_m=0.8, det_per_100m2=10.0)),
    ),
    "electric-eel": (
        "2.00 m at 5 cm - a cylinder with one enormous anal fin and no dorsal",
        base(res="5", name="electric-eel",
             notes="NO DORSAL FIN AT ALL, and one anal fin running two thirds "
                   "of the underside -- which is a fin arrangement no other "
                   "species in the library has and is expressible exactly. Flat "
                   "head, plain dark grey-brown, and a yellow-orange throat as "
                   "the only colour.",
             **f(length_m=2.00, depth_ratio=0.09, width_ratio=0.95,
                 depth_at=0.28, fullness=2.4, snout=0.78, peduncle=0.60,
                 belly=0.46, width_follow=1.00, section=2.7, head_frac=0.16,
                 caudal_shape="pointed", caudal_len=0.10, caudal_span=0.80,
                 dorsal_shape="none", dorsal_height=0.0,
                 anal_height=0.34, anal_len=0.50,
                 pectoral=0.22, pelvic=0.0, eye=1.0, fin_thick=2,
                 back_frac=0.44, belly_frac=0.20, pattern="none",
                 mat_back="skin_dark", mat_flank="skin_brown",
                 mat_belly="skin_yellow", mat_fin="skin_dark",
                 mat_eye="skin_dark",
                 bio_rainforest=1.0,
                 place_abundance=0.1, place_spacing_m=25.0,
                 det_despawn_m=90.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=8.0, det_water="river",
                 det_depth_min_m=0.5, det_depth_max_m=5.0,
                 det_min_water_depth_m=1.0, det_per_100m2=0.3)),
    ),
    "peacock-cichlid": (
        "0.50 m at 2 cm - gold-green oval with three dark bars and a tail eyespot",
        base(res="2", name="peacock-cichlid",
             notes="THE EYESPOT AT THE TAIL BASE is the mark everyone knows and "
                   "the generator has no single-blotch marking, so the fish "
                   "carries BARS instead -- three of them, which is its other "
                   "real mark and one the bar mechanism draws honestly. The "
                   "eyespot is not drawn and that is recorded rather than "
                   "faked.",
             **f(length_m=0.50, depth_ratio=0.34, width_ratio=0.40,
                 depth_at=0.36, fullness=4.0, snout=0.34, peduncle=0.32,
                 belly=0.52, width_follow=1.20, section=1.9, head_frac=0.28,
                 caudal_shape="rounded", caudal_len=0.16, caudal_span=1.05,
                 dorsal_shape="spiny", dorsal_start=0.28, dorsal_len=0.46,
                 dorsal_height=0.36,
                 anal_height=0.30, anal_len=0.18,
                 pectoral=0.30, pelvic=0.22, eye=1.0,
                 back_frac=0.32, belly_frac=0.24,
                 pattern="bars", pattern_count=3, pattern_width=0.30,
                 mat_back="skin_olive", mat_flank="skin_yellow",
                 mat_belly="skin_pale", mat_fin="skin_green",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_rainforest=1.0, bio_savanna=0.3,
                 place_abundance=0.4, place_spacing_m=4.0,
                 det_despawn_m=50.0, det_school_min=1, det_school_max=6,
                 det_school_radius_m=6.0, det_water="river",
                 det_depth_min_m=0.5, det_depth_max_m=5.0,
                 det_min_water_depth_m=1.0, det_per_100m2=2.0)),
    ),
    "armoured-catfish": (
        "0.30 m - flat-bottomed plated body under a tall fan of a dorsal",
        base(name="armoured-catfish",
             notes="A FLAT UNDERSIDE, A SUCKER MOUTH AND A SAIL held UP, which "
                   "together are unlike anything else in the library: the "
                   "dorsal is 0.85 of body depth on a fish whose belly is dead "
                   "flat (`belly` 0.28, the lowest here). Wider than deep, "
                   "mottled grey-brown, and it sits on the bottom.",
             **f(length_m=0.30, depth_ratio=0.22, width_ratio=1.20,
                 depth_at=0.28, fullness=3.4, snout=0.62, peduncle=0.34,
                 belly=0.28, width_follow=0.95, section=2.9, head_frac=0.30,
                 caudal_shape="forked", caudal_len=0.16, caudal_span=1.00,
                 caudal_fork=0.24,
                 dorsal_shape="sail", dorsal_start=0.30, dorsal_len=0.24,
                 dorsal_height=0.85,
                 anal_height=0.20, anal_len=0.12,
                 pectoral=0.55, pectoral_aspect=1.10, pelvic=0.22,
                 eye=1.0, back_frac=0.44, belly_frac=0.18,
                 pattern="mottle", pattern_scale=0.10, pattern_strength=0.50,
                 mat_back="skin_dark", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_rainforest=1.0,
                 place_abundance=0.5, place_spacing_m=2.5,
                 det_despawn_m=35.0, det_school_min=1, det_school_max=5,
                 det_school_radius_m=4.0, det_water="river",
                 det_depth_min_m=0.4, det_depth_max_m=4.0,
                 det_min_water_depth_m=0.6, det_per_100m2=5.0)),
    ),
    "freshwater-stingray": (
        "0.90 m at 2 cm - a flat disc with a whip tail as long again",
        base(res="2", name="freshwater-stingray",
             notes="THE DEPRESSIFORM PROBE, AND THE ONE SPEC MOST LIKELY TO "
                   "COME OUT WRONG. The rainforest file says outright that a "
                   "disc much wider than it is long with a whip behind it is at "
                   "the edge of what a single loft along one axis can do, and "
                   "recommends trying it before deciding anything new is "
                   "needed. This is that try: `width_ratio` at its ceiling of "
                   "1.80 with a depth ratio of 0.10, a pointed tail carrying "
                   "most of the length, and no dorsal.\n\n"
                   "IF IT READS AS A FLAT FISH RATHER THAN A RAY, THAT IS THE "
                   "FINDING and it is worth more than the asset -- five other "
                   "rays and four flatfish across the ocean and beach lists "
                   "wait on the same answer. The owner judges the render.",
             **f(length_m=0.90, depth_ratio=0.10, width_ratio=1.80,
                 depth_at=0.30, fullness=6.0, snout=0.40, peduncle=0.14,
                 belly=0.44, width_follow=0.55, section=2.6, head_frac=0.24,
                 caudal_shape="pointed", caudal_len=0.55, caudal_span=0.35,
                 dorsal_shape="none", dorsal_height=0.0,
                 anal_height=0.0, anal_len=0.05,
                 pectoral=0.0, pelvic=0.0, eye=1.0, fin_thick=2,
                 back_frac=0.55, belly_frac=0.30,
                 pattern="spots", pattern_count=10, pattern_scale=0.07,
                 mat_back="skin_brown", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_pale", mat_eye="skin_dark",
                 bio_rainforest=1.0,
                 place_abundance=0.15, place_spacing_m=20.0,
                 det_despawn_m=60.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=10.0, det_water="river",
                 det_depth_min_m=0.5, det_depth_max_m=6.0,
                 det_min_water_depth_m=1.0, det_per_100m2=0.4)),
    ),
    # --- savanna and desert --------------------------------------------------
    "nile-tilapia": (
        "0.35 m - deep compressed olive-silver with bars onto the tail",
        base(name="nile-tilapia",
             notes="A LONG SPINY DORSAL RUNNING MOST OF THE BACK is what a "
                   "cichlid is at this size, and this one carries the bars onto "
                   "the tail as well -- which the generator cannot do, so the "
                   "flank bars are drawn and the tail is left plain. Recorded "
                   "as an approximation.",
             **f(length_m=0.35, depth_ratio=0.38, width_ratio=0.36,
                 depth_at=0.36, fullness=4.4, snout=0.30, peduncle=0.30,
                 belly=0.52, width_follow=1.30, section=1.8, head_frac=0.26,
                 caudal_shape="truncate", caudal_len=0.17, caudal_span=1.05,
                 dorsal_shape="spiny", dorsal_start=0.26, dorsal_len=0.50,
                 dorsal_height=0.36,
                 anal_height=0.32, anal_len=0.18,
                 pectoral=0.30, pelvic=0.22, eye=1.0,
                 back_frac=0.32, belly_frac=0.24,
                 pattern="bars", pattern_count=6, pattern_width=0.28,
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_olive",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_savanna=1.0, bio_desert=0.5, bio_rainforest=0.4,
                 place_abundance=0.8, place_spacing_m=1.5,
                 det_despawn_m=40.0, det_school_min=5, det_school_max=30,
                 det_school_radius_m=4.0, det_water="any",
                 det_depth_min_m=0.3, det_depth_max_m=4.0,
                 det_min_water_depth_m=0.5, det_per_100m2=12.0)),
    ),
    "nile-perch": (
        "1.50 m at 2 cm - big blunt silver body with a notched dorsal",
        base(res="2", name="nile-perch",
             notes="THE BIGGEST FISH IN THE SAVANNA, and its one shape mark is "
                   "that the dorsal fin is SPLIT IN TWO by a deep notch -- "
                   "which is exactly what the `dorsal2` rows say: a first spiny "
                   "fin and a second soft one with a gap between. A large "
                   "rounded tail and a plain silver flank do the rest.",
             **f(length_m=1.50, depth_ratio=0.27, width_ratio=0.48,
                 depth_at=0.40, fullness=3.4, snout=0.34, peduncle=0.26,
                 belly=0.52, width_follow=1.25, section=2.1, head_frac=0.30,
                 caudal_shape="rounded", caudal_len=0.16, caudal_span=1.10,
                 dorsal_shape="spiny", dorsal_start=0.32, dorsal_len=0.18,
                 dorsal_height=0.42,
                 dorsal2_height=0.34, dorsal2_start=0.56, dorsal2_len=0.18,
                 anal_height=0.28, anal_len=0.14,
                 pectoral=0.30, pelvic=0.20, eye=1.0, fin_thick=2,
                 back_frac=0.32, belly_frac=0.26, pattern="none",
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_eye="skin_yellow",
                 bio_savanna=1.0, bio_desert=0.3,
                 place_abundance=0.12, place_spacing_m=30.0,
                 det_despawn_m=90.0, det_school_min=1, det_school_max=3,
                 det_school_radius_m=12.0, det_water="river",
                 det_depth_min_m=1.0, det_depth_max_m=15.0,
                 det_min_water_depth_m=2.0, det_per_100m2=0.4)),
    ),
    "african-sharptooth-catfish": (
        "1.20 m at 2 cm - eel-like body under one unbroken dorsal",
        base(res="2", name="african-sharptooth-catfish",
             notes="ONE DORSAL FIN RUNNING NEARLY THE WHOLE BACK on a long "
                   "eel-shaped body, with a very wide flat bony head and four "
                   "barbel pairs. It is the shape `mud-catfish` would be if it "
                   "were three times longer, and having both makes the family "
                   "read as a family rather than as one fish at two sizes.",
             **f(length_m=1.20, depth_ratio=0.14, width_ratio=0.95,
                 depth_at=0.30, fullness=3.0, snout=0.66, peduncle=0.46,
                 belly=0.42, width_follow=1.00, section=2.7, head_frac=0.26,
                 caudal_shape="rounded", caudal_len=0.12, caudal_span=0.90,
                 dorsal_shape="ridge", dorsal_start=0.24, dorsal_len=0.60,
                 dorsal_height=0.28,
                 anal_height=0.24, anal_len=0.46,
                 pectoral=0.34, pelvic=0.16, barbels=4, barbel_len=0.16,
                 eye=1.0, fin_thick=2,
                 back_frac=0.40, belly_frac=0.24,
                 pattern="mottle", pattern_scale=0.10, pattern_strength=0.40,
                 mat_back="skin_dark", mat_flank="skin_olive",
                 mat_belly="skin_pale", mat_fin="skin_dark",
                 mat_pattern="skin_pale", mat_eye="skin_dark",
                 bio_savanna=1.0, bio_desert=0.6, bio_rainforest=0.4,
                 place_abundance=0.3, place_spacing_m=10.0,
                 det_despawn_m=70.0, det_school_min=1, det_school_max=4,
                 det_school_radius_m=8.0, det_water="river",
                 det_depth_min_m=0.4, det_depth_max_m=6.0,
                 det_min_water_depth_m=0.8, det_per_100m2=1.5)),
    ),
    "african-lungfish": (
        "1.40 m at 2 cm - a cylinder with rope-like paired fins and no forks",
        base(res="2", name="african-lungfish",
             notes="ROPE FINS, WHICH IS THE ONE THING THAT MAKES IT ODD, and "
                   "the generator draws paired fins as plates -- so the "
                   "`pectoral_aspect` is pushed to its narrow end (0.18) to get "
                   "a long thin filament rather than a paddle. That is the same "
                   "slider a humpback's flipper uses, at the same extreme, for "
                   "a completely different animal.\n\n"
                   "One continuous fin ridge round the tail and no fork "
                   "anywhere.",
             **f(length_m=1.40, depth_ratio=0.12, width_ratio=0.90,
                 depth_at=0.36, fullness=2.6, snout=0.66, peduncle=0.42,
                 belly=0.50, width_follow=1.00, section=2.6, head_frac=0.18,
                 caudal_shape="pointed", caudal_len=0.14, caudal_span=0.85,
                 dorsal_shape="ridge", dorsal_start=0.30, dorsal_len=0.60,
                 dorsal_height=0.30,
                 anal_height=0.26, anal_len=0.40,
                 pectoral=0.60, pectoral_aspect=0.18, pelvic=0.30,
                 eye=1.0, fin_thick=2,
                 back_frac=0.38, belly_frac=0.22, pattern="none",
                 mat_back="skin_brown", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_eye="skin_dark",
                 bio_savanna=1.0, bio_rainforest=0.3,
                 place_abundance=0.12, place_spacing_m=25.0,
                 det_despawn_m=70.0, det_school_min=1, det_school_max=1,
                 det_school_radius_m=5.0, det_water="shallow",
                 det_depth_min_m=0.2, det_depth_max_m=3.0,
                 det_min_water_depth_m=0.4, det_per_100m2=0.3)),
    ),
    "desert-pupfish": (
        "0.20 m - stubby deep body, big fan tail, brilliant blue male",
        base(name="desert-pupfish",
             notes="AUTHORED AT 0.20 m AGAINST A REAL 6 cm -- more than three "
                   "times life size, the largest enlargement in the fish "
                   "library. The desert file recommends exactly this and "
                   "recommends skipping the mosquitofish rather than doing it "
                   "twice: a desert has essentially no permanent water, so ONE "
                   "visible spring species is the honest answer and a second "
                   "adds nothing.\n\n"
                   "The breeding male's electric blue is what is drawn, because "
                   "the plain phase is a small grey fish and nobody will ever "
                   "be close enough to see it.",
             **f(length_m=0.20, depth_ratio=0.38, width_ratio=0.46,
                 depth_at=0.34, fullness=4.6, snout=0.36, peduncle=0.36,
                 belly=0.54, width_follow=1.15, section=2.0, head_frac=0.28,
                 caudal_shape="truncate", caudal_len=0.22, caudal_span=1.15,
                 dorsal_shape="triangular", dorsal_start=0.46, dorsal_len=0.22,
                 dorsal_height=0.36,
                 anal_height=0.30, anal_len=0.18,
                 pectoral=0.30, pelvic=0.20, eye=1.0,
                 back_frac=0.34, belly_frac=0.26, pattern="none",
                 mat_back="skin_blue", mat_flank="skin_blue",
                 mat_belly="skin_pale", mat_fin="skin_yellow",
                 mat_eye="skin_dark",
                 bio_desert=1.0,
                 place_abundance=0.9, place_spacing_m=0.5,
                 det_despawn_m=25.0, det_school_min=6, det_school_max=40,
                 det_school_radius_m=2.0, det_water="shallow",
                 det_depth_min_m=0.05, det_depth_max_m=1.0,
                 det_min_water_depth_m=0.1, det_per_100m2=40.0)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "freshwater fish specs")
    written = 0
    for name, (blurb, changes) in SPECIES.items():
        s, rep = sm.patch(sm.default_spec(), changes)
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          label=name, width=28):
            written += 1
        print(f"  {'':<28} {blurb}")
    print(f"\n{written} of {len(SPECIES)} written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
