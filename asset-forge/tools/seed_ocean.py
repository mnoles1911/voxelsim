"""Author the sea: pelagic fish, sharks, rays, flatfish, reef fish and ten
more cetaceans.

WHY THE SEA PAYS BACK FASTEST. `docs/biomes/00-ocean.md` puts pelagic shoal
fish first and seabirds second in its own build order, for one reason: the
water column needs NO engine change. Ocean hosts exactly three kinds -- fish,
cetacean and bird -- and all three generators are shipped and proven. Every
row here is authoring work.

LATTICE, BY THE HOUSE RULE, AND IT IS A LADDER. Small fish 1 cm, medium 2 cm,
sharks and dolphins 5 cm, great whales 10 cm. That is shipped practice
(`docs/marine-megafauna-research.md`) and the reason it is a ladder rather than
one number is that a big animal needs MORE voxels of length than a small one:
the features that identify it are a smaller fraction of its body. A blue
whale's dorsal fin is about one percent of its length.

FOUR THINGS THIS SET ADDS THAT THE SHIPPED SIXTEEN DID NOT.

  * THE DEPRESSIFORM FAMILY -- rays and flatfish, a body WIDER THAN IT IS DEEP.
    The ocean file calls this the one shape family the fish generator has never
    been asked for and recommends doing ONE as a probe before committing to
    five. There are four here (`giant-manta-ray`, `spotted-eagle-ray`,
    `thornback-ray`, and the two flatfish) and every one of them says in its
    own `notes` that it is a probe. If they read as flat fish rather than as
    rays, that is the finding and it is worth more than the assets.
  * THE BILLFISH. A spear that is a fifth to a third of the animal, which
    `fish.snout` cannot say -- it sets the DEPTH at the front, not a projection.
    The bill is drawn as an extremely low snout on a very long head instead,
    which gives a long tapering point. Stated as an approximation in each spec.
  * THE FIVE SHARK OUTLINES the shipped four do not cover: a filter feeder with
    its mouth open, a slim ocean cruiser, a lunate speedster, a bottom-hugging
    nurse shark and a tail as long as the body.
  * TEN MORE CETACEANS, which the ocean file lists third in its own order
    because they are parameter changes on a shipped generator rather than new
    work. Two of them have NO DORSAL FIN AT ALL, which is the strongest single
    silhouette statement available in the group.

ONE MARKING AND NEVER TWO. Several species below wear two in life; the `notes`
say which one was dropped and why.

    python tools/seed_ocean.py
    python tools/seed_ocean.py --force

SIZES ARE APPROXIMATE. Every length is the approximate figure from the ocean
and beach files, which say plainly that they are unsourced general-knowledge
estimates. Nothing here is quoted as measured, and in particular no
proportion below is offered as a published ratio the way
`tools/seed_marine.py`'s are -- that file cites its sources and this one does
not, because it does not have any.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(kind="fish", res="1", **over):
    changes = {
        "kind": kind,
        "resolution_cm": res,
        "variation.amount": 1.0,
        "variation.height": 0.15,
        "variation.shape": 0.12,
        "variation.proportion": 0.18,
        "detail.entity_class": "detail",
    }
    changes.update(over)
    return changes


def f(**kw):
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


def cet(**over):
    """A cetacean's shared shape, matching `tools/seed_marine.py`: horizontal
    fluke, barrel trunk becoming a blade at the wrist, flippers, blowhole, and
    no pelvic or anal fin."""
    d = dict(caudal_plane="horizontal", caudal_shape="forked", section=2.5,
             section_tail=1.3, belly=0.50, pelvic=0.0, anal_height=0.0,
             barbels=0, pattern="none", eye=1.0, blowhole=1.0,
             mat_eye="skin_dark", bio_ocean=1.0)
    d.update(over)
    return f(**d)


def shark(**over):
    """A shark's shared shape: a heterocercal tail with a long upper lobe, a
    second dorsal nub, no adipose fin, and a hard countershade line."""
    d = dict(caudal_shape="forked", section=2.2, section_tail=1.6,
             pelvic=0.14, anal_height=0.14, anal_len=0.08, eye=1.0,
             fin_thick=2, pattern="none", mat_eye="skin_dark",
             bio_ocean=1.0)
    d.update(over)
    return f(**d)


SPECIES = {
    # --- pelagic: the shoal fish that pay back most -------------------------
    "atlantic-mackerel": (
        "0.40 m - streamlined, iridescent green back with black wavy bars",
        base(name="atlantic-mackerel",
             notes="THE BARS RUN OVER THE TOP OF THE ANIMAL RATHER THAN DOWN "
                   "ITS FLANK, which is unusual and is what makes a shoal of "
                   "mackerel look like moving water from above. The generator "
                   "draws bars around the body, so the effect is got by keeping "
                   "the dark back deep (0.46) and the bars narrow: the ink "
                   "lands mostly on the back where the boundary already is.\n\n"
                   "The core pelagic shape -- a lunate tail on a very slim tail "
                   "wrist -- and the reason a shoal is many entities for one "
                   "spec.",
             **f(length_m=0.40, depth_ratio=0.20, width_ratio=0.60,
                 depth_at=0.40, fullness=3.2, snout=0.24, peduncle=0.12,
                 belly=0.50, width_follow=1.60, section=2.2, head_frac=0.26,
                 caudal_shape="forked", caudal_len=0.18, caudal_span=1.70,
                 caudal_fork=0.70,
                 dorsal_shape="triangular", dorsal_start=0.32, dorsal_len=0.14,
                 dorsal_height=0.34,
                 dorsal2_height=0.20, dorsal2_start=0.62, dorsal2_len=0.12,
                 anal_height=0.24, anal_len=0.12,
                 pectoral=0.26, pelvic=0.14, eye=1.0,
                 back_frac=0.46, belly_frac=0.30,
                 pattern="bars", pattern_count=14, pattern_width=0.30,
                 mat_back="skin_green", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.5,
                 place_abundance=1.0, place_spacing_m=0.5,
                 det_despawn_m=60.0, det_school_min=30, det_school_max=300,
                 det_school_radius_m=10.0, det_water="ocean",
                 det_depth_min_m=1.0, det_depth_max_m=40.0,
                 det_min_water_depth_m=3.0, det_per_100m2=80.0)),
    ),
    "sardine": (
        "0.22 m - small round-bellied silver shoaler with one soft dorsal",
        base(name="sardine",
             notes="THE PLAINEST FISH IN THE SEA AND THE MOST NUMEROUS. One "
                   "soft dorsal, a deep fork, countershading and a single row "
                   "of faint dark spots along the upper flank -- which is the "
                   "one thing separating it from `shoal-herring` beside it, "
                   "and the reason both are worth having: a bait ball of two "
                   "species is a bait ball, and a bait ball of one is a "
                   "texture.",
             **f(length_m=0.22, depth_ratio=0.23, width_ratio=0.50,
                 depth_at=0.42, fullness=3.4, snout=0.26, peduncle=0.20,
                 belly=0.58, width_follow=1.35, section=2.0, head_frac=0.26,
                 caudal_shape="forked", caudal_len=0.20, caudal_span=1.25,
                 caudal_fork=0.55,
                 dorsal_shape="triangular", dorsal_start=0.40, dorsal_len=0.16,
                 dorsal_height=0.28,
                 anal_height=0.22, anal_len=0.14,
                 pectoral=0.24, pelvic=0.14, eye=1.0,
                 back_frac=0.34, belly_frac=0.34,
                 pattern="spots", pattern_count=6, pattern_scale=0.04,
                 mat_back="skin_blue", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.6,
                 place_abundance=1.0, place_spacing_m=0.5,
                 det_despawn_m=55.0, det_school_min=40, det_school_max=400,
                 det_school_radius_m=8.0, det_water="ocean",
                 det_depth_min_m=1.0, det_depth_max_m=30.0,
                 det_min_water_depth_m=2.5, det_per_100m2=120.0)),
    ),
    "european-anchovy": (
        "0.20 m - very slim, huge underslung jaw, one broad silver stripe",
        base(name="european-anchovy",
             notes="AUTHORED AT 0.20 m AGAINST A REAL 15 cm, the library's "
                   "floor. Recorded so it is not shrunk back.\n\n"
                   "ONE BROAD BRIGHT STRIPE ON A NEARLY TRANSPARENT FISH is the "
                   "whole species, and it is drawn wide -- 0.34 of the body "
                   "depth -- because a narrow one on a twenty-voxel flank is "
                   "two voxels and reads as a scratch. The enormous jaw is "
                   "`head_frac` at 0.34, the largest head share in the "
                   "library.",
             **f(length_m=0.20, depth_ratio=0.17, width_ratio=0.52,
                 depth_at=0.36, fullness=3.0, snout=0.22, peduncle=0.20,
                 belly=0.52, width_follow=1.30, section=2.0, head_frac=0.34,
                 caudal_shape="forked", caudal_len=0.20, caudal_span=1.20,
                 caudal_fork=0.55,
                 dorsal_shape="triangular", dorsal_start=0.44, dorsal_len=0.14,
                 dorsal_height=0.26,
                 anal_height=0.22, anal_len=0.14,
                 pectoral=0.22, pelvic=0.12, eye=1.0,
                 back_frac=0.28, belly_frac=0.30,
                 pattern="stripe", pattern_width=0.34, pattern_pos=0.55,
                 mat_back="skin_green", mat_flank="skin_blue",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_pattern="skin_silver", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.5,
                 place_abundance=1.0, place_spacing_m=0.5,
                 det_despawn_m=50.0, det_school_min=50, det_school_max=400,
                 det_school_radius_m=8.0, det_water="ocean",
                 det_depth_min_m=1.0, det_depth_max_m=25.0,
                 det_min_water_depth_m=2.0, det_per_100m2=150.0)),
    ),
    "atlantic-cod": (
        "1.00 m at 2 cm - thick body, THREE dorsals, a chin barbel",
        base(res="2", name="atlantic-cod",
             notes="THREE SEPARATE DORSAL FINS AND THE GENERATOR HAS TWO. The "
                   "first and second are drawn and the third is folded into the "
                   "second's length, which keeps the read -- a back that is fin "
                   "nearly all the way along, broken once -- and is an "
                   "approximation stated rather than hidden. One chin barbel "
                   "and a pale curved lateral line complete it; the line is not "
                   "drawn, because a one-voxel curve on a fifty-voxel fish is a "
                   "scratch.",
             **f(length_m=1.00, depth_ratio=0.22, width_ratio=0.62,
                 depth_at=0.34, fullness=3.4, snout=0.42, peduncle=0.24,
                 belly=0.54, width_follow=1.25, section=2.3, head_frac=0.28,
                 caudal_shape="truncate", caudal_len=0.16, caudal_span=1.05,
                 dorsal_shape="triangular", dorsal_start=0.28, dorsal_len=0.16,
                 dorsal_height=0.34,
                 dorsal2_height=0.30, dorsal2_start=0.50, dorsal2_len=0.34,
                 anal_height=0.26, anal_len=0.30,
                 pectoral=0.30, pelvic=0.18, barbels=1, barbel_len=0.05,
                 eye=1.0, fin_thick=2,
                 back_frac=0.36, belly_frac=0.28,
                 pattern="spots", pattern_count=14, pattern_scale=0.035,
                 mat_back="skin_olive", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_olive",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.4,
                 place_abundance=0.4, place_spacing_m=4.0,
                 det_despawn_m=80.0, det_school_min=3, det_school_max=25,
                 det_school_radius_m=12.0, det_water="ocean",
                 det_depth_min_m=5.0, det_depth_max_m=200.0,
                 det_min_water_depth_m=8.0, det_per_100m2=2.0)),
    ),
    "european-sea-bass": (
        "0.70 m at 2 cm - clean silver fusiform with a spiny first dorsal",
        base(res="2", name="european-sea-bass",
             notes="THE REFERENCE SEA FISH: no marking at all, a hard spiny "
                   "first dorsal, a soft second, and a sharp gill-cover edge. "
                   "It is here to be the plain one that everything else is "
                   "measured against, the way `pale-minnow` is in fresh water.",
             **f(length_m=0.70, depth_ratio=0.24, width_ratio=0.46,
                 depth_at=0.38, fullness=3.4, snout=0.26, peduncle=0.20,
                 belly=0.52, width_follow=1.35, section=2.0, head_frac=0.28,
                 caudal_shape="forked", caudal_len=0.18, caudal_span=1.15,
                 caudal_fork=0.35,
                 dorsal_shape="spiny", dorsal_start=0.32, dorsal_len=0.18,
                 dorsal_height=0.40,
                 dorsal2_height=0.30, dorsal2_start=0.56, dorsal2_len=0.16,
                 anal_height=0.28, anal_len=0.14,
                 pectoral=0.28, pelvic=0.18, eye=1.0, fin_thick=2,
                 back_frac=0.32, belly_frac=0.28, pattern="none",
                 mat_back="plume_slate", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_eye="skin_dark",
                 bio_ocean=0.9, bio_beach=0.9,
                 place_abundance=0.5, place_spacing_m=3.0,
                 det_despawn_m=70.0, det_school_min=2, det_school_max=20,
                 det_school_radius_m=10.0, det_water="ocean",
                 det_depth_min_m=1.0, det_depth_max_m=30.0,
                 det_min_water_depth_m=2.0, det_per_100m2=3.0)),
    ),
    "yellowfin-tuna": (
        "1.80 m at 2 cm - long sickle second dorsal and anal trailing back",
        base(res="2", name="yellowfin-tuna",
             notes="THE TWO SICKLE FINS ARE THE SPECIES, and they are the "
                   "longest second dorsal in the library at 0.34 of the body "
                   "with a height matching the first. `bluefin-tuna` is the "
                   "same animal without them, so the pair is a clean test of "
                   "whether one fin length reads at ninety voxels.",
             **f(length_m=1.80, depth_ratio=0.24, width_ratio=0.62,
                 depth_at=0.36, fullness=4.0, snout=0.28, peduncle=0.10,
                 belly=0.52, width_follow=1.70, section=2.2, head_frac=0.26,
                 caudal_shape="forked", caudal_len=0.16, caudal_span=1.85,
                 caudal_fork=0.80,
                 dorsal_shape="triangular", dorsal_start=0.30, dorsal_len=0.14,
                 dorsal_height=0.48,
                 dorsal2_height=0.46, dorsal2_start=0.48, dorsal2_len=0.34,
                 anal_height=0.44, anal_len=0.30,
                 pectoral=0.32, pectoral_aspect=0.60, pelvic=0.16,
                 eye=1.0, fin_thick=2,
                 back_frac=0.34, belly_frac=0.30, pattern="none",
                 mat_back="skin_blue", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_yellow",
                 mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.2, place_spacing_m=12.0,
                 det_despawn_m=140.0, det_school_min=3, det_school_max=30,
                 det_school_radius_m=30.0, det_water="ocean",
                 det_depth_min_m=2.0, det_depth_max_m=100.0,
                 det_min_water_depth_m=10.0, det_per_100m2=1.0)),
    ),
    "great-barracuda": (
        "1.40 m at 2 cm - long silver tube with two widely separated dorsals",
        base(res="2", name="great-barracuda",
             notes="SAGITTIFORM, LIKE A PIKE, BUT IN THE SEA -- and the two "
                   "dorsals are placed as far apart as the sliders allow, which "
                   "is the barracuda's one shape mark. Dark blotches low on the "
                   "flank rather than bars over it; a saddle marking would put "
                   "them on the back, so they are drawn as sparse mottle "
                   "instead.",
             **f(length_m=1.40, depth_ratio=0.13, width_ratio=0.70,
                 depth_at=0.42, fullness=2.8, snout=0.16, peduncle=0.20,
                 belly=0.50, width_follow=1.30, section=2.3, head_frac=0.30,
                 caudal_shape="forked", caudal_len=0.16, caudal_span=1.40,
                 caudal_fork=0.50,
                 dorsal_shape="spiny", dorsal_start=0.30, dorsal_len=0.10,
                 dorsal_height=0.44,
                 dorsal2_height=0.40, dorsal2_start=0.70, dorsal2_len=0.12,
                 anal_height=0.34, anal_len=0.10,
                 pectoral=0.26, pelvic=0.14, eye=1.0, fin_thick=2,
                 back_frac=0.34, belly_frac=0.30,
                 pattern="mottle", pattern_scale=0.06, pattern_strength=0.18,
                 mat_back="plume_slate", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_dark",
                 mat_pattern="skin_dark", mat_eye="skin_yellow",
                 bio_ocean=1.0, bio_beach=0.4,
                 place_abundance=0.15, place_spacing_m=20.0,
                 det_despawn_m=120.0, det_school_min=1, det_school_max=6,
                 det_school_radius_m=20.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=40.0,
                 det_min_water_depth_m=4.0, det_per_100m2=0.5)),
    ),
    "mahi-mahi": (
        "1.20 m at 2 cm - a blunt vertical forehead and one fin nose to tail",
        base(res="2", name="mahi-mahi",
             notes="ONE DORSAL FIN RUNNING FROM THE HEAD TO THE TAIL, which is "
                   "`dorsal_len` at 0.62 -- the longest single dorsal in the "
                   "library -- over a body with a near-vertical forehead "
                   "(`snout` 0.14). Those two together are unmistakable and "
                   "neither is a colour.",
             **f(length_m=1.20, depth_ratio=0.24, width_ratio=0.38,
                 depth_at=0.20, fullness=5.5, snout=0.14, peduncle=0.14,
                 belly=0.48, width_follow=1.45, section=1.8, head_frac=0.20,
                 caudal_shape="forked", caudal_len=0.18, caudal_span=1.75,
                 caudal_fork=0.72,
                 dorsal_shape="ridge", dorsal_start=0.10, dorsal_len=0.62,
                 dorsal_height=0.42,
                 anal_height=0.30, anal_len=0.34,
                 pectoral=0.26, pelvic=0.14, eye=1.0, fin_thick=2,
                 back_frac=0.36, belly_frac=0.30, pattern="none",
                 mat_back="skin_blue", mat_flank="skin_green",
                 mat_belly="skin_yellow", mat_fin="skin_blue",
                 mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.2, place_spacing_m=15.0,
                 det_despawn_m=110.0, det_school_min=2, det_school_max=12,
                 det_school_radius_m=25.0, det_water="ocean",
                 det_depth_min_m=0.5, det_depth_max_m=40.0,
                 det_min_water_depth_m=8.0, det_per_100m2=0.8)),
    ),
    "atlantic-sailfish": (
        "2.50 m at 5 cm - a full-length dorsal fan taller than the body is deep",
        base(res="5", name="atlantic-sailfish",
             notes="THE DORSAL IS THE SPECIES: a fan running two thirds of the "
                   "back at 1.9 times the body's own depth, which is the "
                   "tallest fin in the library by a wide margin.\n\n"
                   "THE SPEAR IS AN APPROXIMATION AND IT HAS TO BE. "
                   "`fish.snout` sets how DEEP the front of the body is, not "
                   "how far it projects, so a bill is drawn as an extremely low "
                   "snout on a long head -- 0.06 over a head share of 0.34 -- "
                   "which gives a long tapering point rather than a separate "
                   "rod. It reads at fifty voxels and it is not the anatomy.",
             **f(length_m=2.50, depth_ratio=0.16, width_ratio=0.40,
                 depth_at=0.36, fullness=3.2, snout=0.06, peduncle=0.12,
                 belly=0.50, width_follow=1.50, section=2.0, head_frac=0.34,
                 caudal_shape="forked", caudal_len=0.16, caudal_span=1.85,
                 caudal_fork=0.80,
                 dorsal_shape="sail", dorsal_start=0.24, dorsal_len=0.62,
                 dorsal_height=1.90,
                 anal_height=0.34, anal_len=0.14,
                 pectoral=0.28, pectoral_aspect=0.45, pelvic=0.20,
                 eye=1.0, fin_thick=2,
                 back_frac=0.36, belly_frac=0.28, pattern="none",
                 mat_back="skin_blue", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_blue",
                 mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.08, place_spacing_m=60.0,
                 det_despawn_m=200.0, det_school_min=1, det_school_max=4,
                 det_school_radius_m=40.0, det_water="ocean",
                 det_depth_min_m=1.0, det_depth_max_m=60.0,
                 det_min_water_depth_m=15.0, det_per_100m2=0.2)),
    ),
    "swordfish": (
        "3.00 m at 5 cm - flat broad bill, one tall crescent dorsal, no pelvics",
        base(res="5", name="swordfish",
             notes="NO PELVIC FINS AT ALL, which is a real and unusual absence "
                   "and is free to express here. One tall crescent dorsal set "
                   "well forward on an otherwise clean body, and the same bill "
                   "approximation the sailfish uses -- a very low snout on a "
                   "long head.",
             **f(length_m=3.00, depth_ratio=0.19, width_ratio=0.60,
                 depth_at=0.34, fullness=3.2, snout=0.07, peduncle=0.10,
                 belly=0.50, width_follow=1.55, section=2.2, head_frac=0.34,
                 caudal_shape="forked", caudal_len=0.16, caudal_span=1.90,
                 caudal_fork=0.82,
                 dorsal_shape="triangular", dorsal_start=0.34, dorsal_len=0.10,
                 dorsal_height=0.72,
                 anal_height=0.30, anal_len=0.10,
                 pectoral=0.28, pectoral_aspect=0.50, pelvic=0.0,
                 eye=1.0, fin_thick=2,
                 back_frac=0.40, belly_frac=0.30, pattern="none",
                 mat_back="skin_dark", mat_flank="plume_slate",
                 mat_belly="skin_pale", mat_fin="skin_dark",
                 mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.06, place_spacing_m=80.0,
                 det_despawn_m=220.0, det_school_min=1, det_school_max=1,
                 det_school_radius_m=20.0, det_water="ocean",
                 det_depth_min_m=2.0, det_depth_max_m=300.0,
                 det_min_water_depth_m=20.0, det_per_100m2=0.15)),
    ),
    "blue-marlin": (
        "3.50 m at 5 cm - round-sectioned spear, short pointed dorsal",
        base(res="5", name="blue-marlin",
             notes="THE THIRD BILLFISH AND DELIBERATELY THE PLAIN ONE: where "
                   "the sailfish has a fan and the swordfish a crescent, a "
                   "marlin's dorsal is short and pointed and its body is round "
                   "in section rather than flattened. Faint pale vertical bars "
                   "on cobalt, which is the one billfish here with a marking at "
                   "all.",
             **f(length_m=3.50, depth_ratio=0.18, width_ratio=0.78,
                 depth_at=0.34, fullness=3.2, snout=0.08, peduncle=0.11,
                 belly=0.50, width_follow=1.55, section=2.5, head_frac=0.32,
                 caudal_shape="forked", caudal_len=0.15, caudal_span=1.80,
                 caudal_fork=0.78,
                 dorsal_shape="triangular", dorsal_start=0.28, dorsal_len=0.16,
                 dorsal_height=0.60,
                 dorsal2_height=0.16, dorsal2_start=0.72, dorsal2_len=0.08,
                 anal_height=0.28, anal_len=0.10,
                 pectoral=0.30, pectoral_aspect=0.40, pelvic=0.10,
                 eye=1.0, fin_thick=2,
                 back_frac=0.40, belly_frac=0.28,
                 pattern="bars", pattern_count=12, pattern_width=0.16,
                 mat_back="skin_blue", mat_flank="plume_slate",
                 mat_belly="skin_pale", mat_fin="skin_blue",
                 mat_pattern="skin_silver", mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.05, place_spacing_m=100.0,
                 det_despawn_m=250.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=30.0, det_water="ocean",
                 det_depth_min_m=1.0, det_depth_max_m=150.0,
                 det_min_water_depth_m=25.0, det_per_100m2=0.1)),
    ),
    "ocean-sunfish": (
        "2.00 m at 5 cm - a disc with no tail, one tall dorsal and one anal",
        base(res="5", name="ocean-sunfish",
             notes="THE ONLY FISH IN THE LIBRARY WITH NO TAIL. The body ends in "
                   "a rudder fringe, so `caudal_len` is 0.02 and the outline is "
                   "a disc with two long blades sticking off it opposite each "
                   "other. Depth ratio 0.78 is the top of the slider and the "
                   "deepest body here by half again; nothing else is shaped "
                   "remotely like it and it needs no colour at all.",
             **f(length_m=2.00, depth_ratio=0.78, width_ratio=0.24,
                 depth_at=0.44, fullness=6.5, snout=0.34, peduncle=0.68,
                 belly=0.50, width_follow=1.05, section=1.9, head_frac=0.26,
                 caudal_shape="truncate", caudal_len=0.02, caudal_span=1.00,
                 dorsal_shape="triangular", dorsal_start=0.60, dorsal_len=0.18,
                 dorsal_height=0.95,
                 anal_height=0.90, anal_len=0.18,
                 pectoral=0.16, pelvic=0.0, eye=1.0, fin_thick=2,
                 back_frac=0.42, belly_frac=0.34, pattern="none",
                 mat_back="plume_slate", mat_flank="plume_grey",
                 mat_belly="skin_pale", mat_fin="plume_slate",
                 mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.05, place_spacing_m=120.0,
                 det_despawn_m=200.0, det_school_min=1, det_school_max=1,
                 det_school_radius_m=10.0, det_water="ocean",
                 det_depth_min_m=0.0, det_depth_max_m=60.0,
                 det_min_water_depth_m=15.0, det_per_100m2=0.05)),
    ),
    "flying-fish": (
        "0.30 m - slim body with pectoral fins as long as itself, held out",
        base(name="flying-fish",
             notes="THE LONGEST PECTORALS IN THE LIBRARY at 1.2 of body depth "
                   "with a chord ratio of 0.55, which is a pair of wings rather "
                   "than a pair of paddles. That one slider pair IS the species "
                   "and everything else about it is a small silver fish.",
             **f(length_m=0.30, depth_ratio=0.17, width_ratio=0.62,
                 depth_at=0.40, fullness=3.0, snout=0.30, peduncle=0.24,
                 belly=0.50, width_follow=1.25, section=2.2, head_frac=0.26,
                 caudal_shape="forked", caudal_len=0.22, caudal_span=1.30,
                 caudal_fork=0.60,
                 dorsal_shape="triangular", dorsal_start=0.62, dorsal_len=0.14,
                 dorsal_height=0.30,
                 anal_height=0.24, anal_len=0.12,
                 pectoral=1.20, pectoral_aspect=0.55, pelvic=0.30,
                 eye=1.0, back_frac=0.36, belly_frac=0.30, pattern="none",
                 mat_back="skin_blue", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="plume_slate",
                 mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.5, place_spacing_m=1.5,
                 det_despawn_m=60.0, det_school_min=5, det_school_max=40,
                 det_school_radius_m=8.0, det_water="ocean",
                 det_depth_min_m=0.0, det_depth_max_m=10.0,
                 det_min_water_depth_m=5.0, det_per_100m2=6.0)),
    ),
    "conger-eel": (
        "2.00 m at 2 cm - very heavy anguilliform, dorsal starting far forward",
        base(res="2", name="conger-eel",
             notes="`river-eel` AT THREE TIMES THE SIZE AND TWICE THE GIRTH, "
                   "with the dorsal starting just behind the pectoral instead "
                   "of halfway back -- so the fin ridge runs almost the whole "
                   "animal. Plain grey-brown, and the mass is the point.",
             **f(length_m=2.00, depth_ratio=0.11, width_ratio=0.90,
                 depth_at=0.38, fullness=2.4, snout=0.72, peduncle=0.34,
                 belly=0.50, width_follow=1.00, section=2.5, head_frac=0.18,
                 caudal_shape="pointed", caudal_len=0.10, caudal_span=0.80,
                 dorsal_shape="ridge", dorsal_start=0.16, dorsal_len=0.76,
                 dorsal_height=0.34,
                 anal_height=0.28, anal_len=0.50,
                 pectoral=0.28, pelvic=0.0, eye=1.0, fin_thick=2,
                 back_frac=0.40, belly_frac=0.22, pattern="none",
                 mat_back="plume_slate", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_dark",
                 mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.6, bio_bare_rock=0.0,
                 place_abundance=0.15, place_spacing_m=20.0,
                 det_despawn_m=80.0, det_school_min=1, det_school_max=1,
                 det_school_radius_m=5.0, det_water="ocean",
                 det_depth_min_m=2.0, det_depth_max_m=60.0,
                 det_min_water_depth_m=3.0, det_per_100m2=0.4)),
    ),
    "anglerfish": (
        "0.80 m - globiform, enormous upturned mouth, a rod over the snout",
        base(name="anglerfish",
             notes="THE LURE IS THE SPECIES AND THE ROD HAS TO BE DRAWN THICKER "
                   "THAN LIFE. The ocean file works it out: a 2-3 cm bulb at "
                   "1 cm is two or three voxels and just clears the floor, but "
                   "ONLY if the rod holding it is at least one voxel along its "
                   "whole length -- which means drawing the rod above life "
                   "size. It is drawn here as a forward-set spiny dorsal ray "
                   "(`dorsal_start` 0.06 with a very short length and a big "
                   "height), which is the nearest the generator gets and is a "
                   "stylisation.\n\n"
                   "Globiform: depth ratio 0.60 with a head share of 0.42, the "
                   "largest in the library.",
             **f(length_m=0.80, depth_ratio=0.60, width_ratio=0.95,
                 depth_at=0.24, fullness=6.0, snout=0.55, peduncle=0.20,
                 belly=0.60, width_follow=1.60, section=2.6, head_frac=0.42,
                 caudal_shape="rounded", caudal_len=0.14, caudal_span=0.85,
                 dorsal_shape="spiny", dorsal_start=0.06, dorsal_len=0.06,
                 dorsal_height=0.55,
                 dorsal2_height=0.20, dorsal2_start=0.66, dorsal2_len=0.12,
                 anal_height=0.20, anal_len=0.12,
                 pectoral=0.34, pelvic=0.18, eye=1.0,
                 back_frac=0.44, belly_frac=0.20,
                 pattern="mottle", pattern_scale=0.12, pattern_strength=0.45,
                 mat_back="skin_dark", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_dark",
                 mat_pattern="skin_dark", mat_eye="skin_pale",
                 bio_ocean=1.0,
                 place_abundance=0.1, place_spacing_m=25.0,
                 det_despawn_m=45.0, det_school_min=1, det_school_max=1,
                 det_school_radius_m=3.0, det_water="ocean",
                 det_depth_min_m=40.0, det_depth_max_m=600.0,
                 det_min_water_depth_m=50.0, det_per_100m2=0.2)),
    ),
    # --- estuary and surf ----------------------------------------------------
    "thick-lipped-mullet": (
        "0.55 m at 2 cm - blunt heavy-lipped head, two separated dorsals",
        base(res="2", name="thick-lipped-mullet",
             notes="THE HARBOUR FISH: a blunt head with heavy lips, two well "
                   "separated dorsals, and faint horizontal stripes on grey. "
                   "The stripes are drawn narrow and low-contrast on purpose -- "
                   "they are barely there in life and a bold version would read "
                   "as a different animal.",
             **f(length_m=0.55, depth_ratio=0.23, width_ratio=0.62,
                 depth_at=0.36, fullness=3.2, snout=0.56, peduncle=0.24,
                 belly=0.50, width_follow=1.20, section=2.3, head_frac=0.26,
                 caudal_shape="forked", caudal_len=0.18, caudal_span=1.15,
                 caudal_fork=0.38,
                 dorsal_shape="spiny", dorsal_start=0.34, dorsal_len=0.08,
                 dorsal_height=0.36,
                 dorsal2_height=0.30, dorsal2_start=0.62, dorsal2_len=0.10,
                 anal_height=0.26, anal_len=0.12,
                 pectoral=0.28, pelvic=0.18, eye=1.0, fin_thick=2,
                 back_frac=0.34, belly_frac=0.28,
                 pattern="stripe", pattern_width=0.10, pattern_pos=0.50,
                 mat_back="plume_slate", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_pattern="plume_slate", mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.5,
                 place_abundance=0.5, place_spacing_m=2.5,
                 det_despawn_m=55.0, det_school_min=4, det_school_max=30,
                 det_school_radius_m=8.0, det_water="shallow",
                 det_depth_min_m=0.3, det_depth_max_m=6.0,
                 det_min_water_depth_m=0.8, det_per_100m2=8.0)),
    ),
    "garfish": (
        "0.75 m - extremely elongated with both jaws drawn into a long beak",
        base(name="garfish",
             notes="THE BEAK IS DRAWN TWO VOXELS THICK AND THAT IS A DECISION. "
                   "The beach file works it out: at 1 cm a real garfish beak is "
                   "one voxel wide along its whole run, which is the absolute "
                   "floor -- so it is authored deliberately blunter, as a very "
                   "low snout on a head share of 0.38, the largest here. "
                   "Stylised, and said so.\n\n"
                   "Both fins pushed right aft, near-15:1 body, bright silver "
                   "flank.",
             **f(length_m=0.75, depth_ratio=0.075, width_ratio=0.72,
                 depth_at=0.46, fullness=2.6, snout=0.10, peduncle=0.30,
                 belly=0.50, width_follow=1.10, section=2.2, head_frac=0.38,
                 caudal_shape="forked", caudal_len=0.14, caudal_span=1.45,
                 caudal_fork=0.42,
                 dorsal_shape="triangular", dorsal_start=0.74, dorsal_len=0.14,
                 dorsal_height=0.60,
                 anal_height=0.55, anal_len=0.14,
                 pectoral=0.34, pelvic=0.20, eye=1.0,
                 back_frac=0.36, belly_frac=0.32, pattern="none",
                 mat_back="skin_green", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.6,
                 place_abundance=0.4, place_spacing_m=3.0,
                 det_despawn_m=55.0, det_school_min=3, det_school_max=20,
                 det_school_radius_m=8.0, det_water="shallow",
                 det_depth_min_m=0.2, det_depth_max_m=8.0,
                 det_min_water_depth_m=1.0, det_per_100m2=5.0)),
    ),
    "lesser-sand-eel": (
        "0.22 m - a silver thread, essentially depth-free",
        base(name="lesser-sand-eel",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 20 cm, at the floor.\n\n"
                   "THE MOST EXTREME CASE OF A KNOWN DEFECT, AND IT ONLY WORKS "
                   "BECAUSE THAT DEFECT WAS FIXED. The fish research recorded "
                   "that an eel-class body at this size has a caudal peduncle "
                   "of about 0.2 voxels, and that any code assuming a "
                   "cross-section always contains a cell centre produces the "
                   "fish in three pieces -- so the body axis is stamped as a "
                   "solid one-voxel run FIRST and the cross-sections are added "
                   "to it. A sand eel is that case at its limit: depth ratio "
                   "0.06, the lowest in the library. It is here partly as a "
                   "shoal species and partly as a standing test of that fix.",
             **f(length_m=0.22, depth_ratio=0.06, width_ratio=0.85,
                 depth_at=0.42, fullness=2.4, snout=0.30, peduncle=0.40,
                 belly=0.50, width_follow=1.00, section=2.2, head_frac=0.22,
                 caudal_shape="forked", caudal_len=0.14, caudal_span=1.10,
                 caudal_fork=0.35,
                 dorsal_shape="ridge", dorsal_start=0.22, dorsal_len=0.62,
                 dorsal_height=0.30,
                 anal_height=0.26, anal_len=0.34,
                 pectoral=0.22, pelvic=0.0, eye=1.0,
                 back_frac=0.34, belly_frac=0.32, pattern="none",
                 mat_back="skin_green", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_silver",
                 mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.8,
                 place_abundance=1.0, place_spacing_m=0.5,
                 det_despawn_m=45.0, det_school_min=40, det_school_max=400,
                 det_school_radius_m=6.0, det_water="shallow",
                 det_depth_min_m=0.5, det_depth_max_m=15.0,
                 det_min_water_depth_m=1.0, det_per_100m2=150.0)),
    ),
    "atlantic-mudskipper": (
        "0.22 m - props itself on muscular pectorals, eyes on top of the head",
        base(name="atlantic-mudskipper",
             notes="THE ONE FISH THAT WALKS, and what makes it read is the "
                   "pectorals: short, very broad, and set low -- chord ratio "
                   "2.4, the widest in the library, so they are stumps rather "
                   "than blades. The eyes sit on TOP of the head and the "
                   "generator places an eye on the head's side; that is not "
                   "expressible and is recorded rather than faked.",
             **f(length_m=0.22, depth_ratio=0.18, width_ratio=0.90,
                 depth_at=0.30, fullness=3.0, snout=0.70, peduncle=0.44,
                 belly=0.44, width_follow=1.00, section=2.6, head_frac=0.30,
                 caudal_shape="rounded", caudal_len=0.16, caudal_span=0.90,
                 dorsal_shape="sail", dorsal_start=0.34, dorsal_len=0.18,
                 dorsal_height=0.55,
                 dorsal2_height=0.30, dorsal2_start=0.62, dorsal2_len=0.16,
                 anal_height=0.20, anal_len=0.16,
                 pectoral=0.50, pectoral_aspect=2.40, pelvic=0.22,
                 eye=1.0, back_frac=0.40, belly_frac=0.20,
                 pattern="spots", pattern_count=10, pattern_scale=0.05,
                 mat_back="skin_brown", mat_flank="skin_olive",
                 mat_belly="skin_pale", mat_fin="skin_blue",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_beach=1.0, bio_rainforest=0.3,
                 place_abundance=0.5, place_spacing_m=1.5,
                 det_despawn_m=35.0, det_school_min=2, det_school_max=12,
                 det_school_radius_m=4.0, det_water="shallow",
                 det_depth_min_m=0.0, det_depth_max_m=0.5,
                 det_min_water_depth_m=0.1, det_per_100m2=8.0)),
    ),
    # --- sharks --------------------------------------------------------------
    "blue-shark": (
        "2.50 m at 5 cm - the slimmest shark: very long pectorals, deep indigo",
        base(res="5", name="blue-shark",
             **shark(length_m=2.50, depth_ratio=0.13, width_ratio=0.66,
                     depth_at=0.36, fullness=2.8, snout=0.14, peduncle=0.16,
                     belly=0.50, width_follow=1.40, head_frac=0.26,
                     caudal_len=0.24, caudal_span=1.40, caudal_fork=0.55,
                     caudal_upper=0.55,
                     dorsal_shape="triangular", dorsal_start=0.44,
                     dorsal_len=0.12, dorsal_height=0.55,
                     dorsal2_height=0.16, dorsal2_start=0.76, dorsal2_len=0.08,
                     pectoral=0.90, pectoral_aspect=0.35,
                     back_frac=0.46, belly_frac=0.34,
                     mat_back="skin_blue", mat_flank="skin_blue",
                     mat_belly="skin_pale", mat_fin="skin_blue",
                     bio_ocean=1.0,
                     place_abundance=0.1, place_spacing_m=60.0,
                     det_despawn_m=200.0, det_school_min=1, det_school_max=3,
                     det_school_radius_m=40.0, det_water="ocean",
                     det_depth_min_m=1.0, det_depth_max_m=200.0,
                     det_min_water_depth_m=20.0, det_per_100m2=0.2),
             notes="THE LONGEST PECTORALS ON ANY SHARK HERE at 0.90 of body "
                   "depth and a chord ratio of 0.35 -- long thin scythes rather "
                   "than the broad triangles a requiem shark carries. That plus "
                   "the slimmest body in the group (depth ratio 0.13) is the "
                   "whole silhouette, and the deep indigo back is the only "
                   "shark colour in the library that is not grey."),
    ),
    "shortfin-mako": (
        "2.80 m at 5 cm - compact, near-lunate tail, sharply pointed snout",
        base(res="5", name="shortfin-mako",
             **shark(length_m=2.80, depth_ratio=0.20, width_ratio=0.72,
                     depth_at=0.36, fullness=3.4, snout=0.10, peduncle=0.10,
                     belly=0.50, width_follow=1.70, head_frac=0.24,
                     caudal_len=0.18, caudal_span=1.70, caudal_fork=0.72,
                     caudal_upper=0.12,
                     dorsal_shape="triangular", dorsal_start=0.38,
                     dorsal_len=0.12, dorsal_height=0.50,
                     dorsal2_height=0.14, dorsal2_start=0.76, dorsal2_len=0.06,
                     pectoral=0.42, pectoral_aspect=0.55,
                     back_frac=0.44, belly_frac=0.36,
                     mat_back="skin_blue", mat_flank="skin_silver",
                     mat_belly="skin_pale", mat_fin="skin_blue",
                     bio_ocean=1.0,
                     place_abundance=0.08, place_spacing_m=80.0,
                     det_despawn_m=200.0, det_school_min=1, det_school_max=2,
                     det_school_radius_m=30.0, det_water="ocean",
                     det_depth_min_m=1.0, det_depth_max_m=150.0,
                     det_min_water_depth_m=20.0, det_per_100m2=0.15),
             notes="A NEARLY SYMMETRIC TAIL ON A SHARK, which almost nothing "
                   "else in the group has: `caudal_upper` 0.12 against a "
                   "requiem shark's 0.55 and a thresher's 1.0. A mako is built "
                   "like a tuna and this is where the shark and tuna outlines "
                   "meet -- a very slim wrist under a huge lunate fin."),
    ),
    "thresher-shark": (
        "4.50 m at 5 cm - the upper tail lobe is as long as the whole body",
        base(res="5", name="thresher-shark",
             **shark(length_m=4.50, depth_ratio=0.17, width_ratio=0.70,
                     depth_at=0.32, fullness=3.2, snout=0.16, peduncle=0.14,
                     belly=0.50, width_follow=1.50, head_frac=0.22,
                     caudal_len=0.58, caudal_span=1.30, caudal_fork=0.55,
                     caudal_upper=1.00,
                     dorsal_shape="triangular", dorsal_start=0.38,
                     dorsal_len=0.12, dorsal_height=0.50,
                     dorsal2_height=0.12, dorsal2_start=0.80, dorsal2_len=0.06,
                     pectoral=0.70, pectoral_aspect=0.40,
                     back_frac=0.44, belly_frac=0.32,
                     mat_back="plume_slate", mat_flank="skin_silver",
                     mat_belly="skin_pale", mat_fin="plume_slate",
                     bio_ocean=1.0,
                     place_abundance=0.05, place_spacing_m=120.0,
                     det_despawn_m=250.0, det_school_min=1, det_school_max=2,
                     det_school_radius_m=40.0, det_water="ocean",
                     det_depth_min_m=2.0, det_depth_max_m=200.0,
                     det_min_water_depth_m=25.0, det_per_100m2=0.08),
             notes="`caudal_upper` AT ITS CEILING OF 1.0 AND `caudal_len` AT "
                   "0.58, which together make HALF THE ASSET A TAIL. Nothing "
                   "else in the library spends its length that way, and it is "
                   "the clearest demonstration that the heterocercal slider "
                   "does real work rather than adding a bump."),
    ),
    "nurse-shark": (
        "2.50 m at 5 cm - blunt rounded head, two barbels, dorsals set far back",
        base(res="5", name="nurse-shark",
             **shark(length_m=2.50, depth_ratio=0.18, width_ratio=1.00,
                     depth_at=0.30, fullness=2.6, snout=0.62, peduncle=0.36,
                     belly=0.42, width_follow=1.05, head_frac=0.20,
                     caudal_len=0.26, caudal_span=0.95, caudal_fork=0.20,
                     caudal_upper=0.90,
                     dorsal_shape="triangular", dorsal_start=0.58,
                     dorsal_len=0.14, dorsal_height=0.40,
                     dorsal2_height=0.32, dorsal2_start=0.76, dorsal2_len=0.10,
                     pectoral=0.50, pectoral_aspect=0.90,
                     barbels=2, barbel_len=0.04,
                     back_frac=0.42, belly_frac=0.22,
                     mat_back="skin_brown", mat_flank="skin_brown",
                     mat_belly="skin_pale", mat_fin="skin_brown",
                     bio_ocean=0.9, bio_beach=0.5,
                     place_abundance=0.12, place_spacing_m=30.0,
                     det_despawn_m=140.0, det_school_min=1, det_school_max=4,
                     det_school_radius_m=15.0, det_water="reef",
                     det_depth_min_m=1.0, det_depth_max_m=30.0,
                     det_min_water_depth_m=3.0, det_per_100m2=0.3),
             notes="A SHARK THAT LIES ON THE BOTTOM, and every proportion says "
                   "so: a flat underside (`belly` 0.42), a wide round section, "
                   "a blunt snout, both dorsals pushed right back over the "
                   "tail, and a caudal-lobe ratio near 5:1 which is the highest "
                   "of any real shark. Two barbels -- the head is wide enough "
                   "to carry them, unlike the freshwater `barbel`."),
    ),
    "basking-shark": (
        "8.00 m at 5 cm - slow grey shark with the mouth held wide open",
        base(res="5", name="basking-shark",
             **shark(length_m=8.00, depth_ratio=0.19, width_ratio=0.80,
                     depth_at=0.32, fullness=2.4, snout=0.90, peduncle=0.14,
                     belly=0.50, width_follow=1.40, head_frac=0.24,
                     caudal_len=0.20, caudal_span=1.50, caudal_fork=0.60,
                     caudal_upper=0.25,
                     dorsal_shape="triangular", dorsal_start=0.42,
                     dorsal_len=0.12, dorsal_height=0.60,
                     dorsal2_height=0.14, dorsal2_start=0.78, dorsal2_len=0.06,
                     pectoral=0.55, pectoral_aspect=0.50, fin_thick=3,
                     back_frac=0.48, belly_frac=0.28,
                     mat_back="plume_slate", mat_flank="plume_grey",
                     mat_belly="plume_grey", mat_fin="plume_slate",
                     bio_ocean=1.0,
                     place_abundance=0.04, place_spacing_m=200.0,
                     det_despawn_m=350.0, det_school_min=1, det_school_max=3,
                     det_school_radius_m=60.0, det_water="ocean",
                     det_depth_min_m=0.0, det_depth_max_m=60.0,
                     det_min_water_depth_m=20.0, det_per_100m2=0.05),
             notes="THE OPEN MOUTH IS THE SPECIES AND `fish.snout` IS WHAT "
                   "SAYS IT: 0.90 means the front of the animal is nearly as "
                   "deep as its deepest point, which is a hoop rather than a "
                   "point. Every other shark here is between 0.10 and 0.62. "
                   "That one number turns a grey shark into a filter feeder, "
                   "and it is the cheapest silhouette change in the group."),
    ),
    "small-spotted-catshark": (
        "0.75 m at 2 cm - slim sandy shark, dense small dark spots",
        base(res="2", name="small-spotted-catshark",
             **shark(length_m=0.75, depth_ratio=0.13, width_ratio=0.86,
                     depth_at=0.34, fullness=2.6, snout=0.50, peduncle=0.34,
                     belly=0.44, width_follow=1.05, head_frac=0.20,
                     caudal_len=0.22, caudal_span=0.80, caudal_fork=0.10,
                     caudal_upper=0.80,
                     dorsal_shape="triangular", dorsal_start=0.62,
                     dorsal_len=0.10, dorsal_height=0.34,
                     dorsal2_height=0.26, dorsal2_start=0.80, dorsal2_len=0.08,
                     pectoral=0.44, pectoral_aspect=0.85, fin_thick=1,
                     back_frac=0.40, belly_frac=0.22,
                     pattern="spots", pattern_count=22, pattern_scale=0.025,
                     mat_back="plume_buff", mat_flank="plume_buff",
                     mat_belly="skin_pale", mat_fin="plume_buff",
                     mat_pattern="skin_dark",
                     bio_beach=1.0, bio_ocean=0.7,
                     place_abundance=0.3, place_spacing_m=8.0,
                     det_despawn_m=70.0, det_school_min=1, det_school_max=4,
                     det_school_radius_m=10.0, det_water="shallow",
                     det_depth_min_m=1.0, det_depth_max_m=40.0,
                     det_min_water_depth_m=2.0, det_per_100m2=1.0),
             notes="THE SMALLEST SHARK IN THE LIBRARY, and the only one that is "
                   "SPOTTED rather than plain or barred. It is also the only "
                   "one on the 2 cm lattice: at 0.75 m that is 38 voxels, and "
                   "at 5 cm it would be fifteen, which cannot hold a spot "
                   "pattern of any density."),
    ),
    # --- rays and flatfish: the depressiform probe --------------------------
    "spotted-eagle-ray": (
        "2.00 m at 2 cm - narrow diamond, duck-like snout, white spots",
        base(res="2", name="spotted-eagle-ray",
             notes="THE DEPRESSIFORM PROBE THE OCEAN FILE ASKS FOR BY NAME. "
                   "'Do one (spotted eagle ray) as a probe before committing to "
                   "five' -- this is that one. `width_ratio` is at its ceiling "
                   "of 1.80 with a depth ratio of 0.09, so the body is twenty "
                   "times wider than it is deep, and the pectorals are turned "
                   "off entirely because on a ray the DISC is the pectorals.\n\n"
                   "IF IT COMES OUT AS A FLATFISH RATHER THAN A RAY, THAT IS "
                   "THE FINDING, and it decides four more specs across the "
                   "ocean, beach and rainforest lists. The owner judges the "
                   "render; I am not offering a verdict on it.",
             **f(length_m=2.00, depth_ratio=0.09, width_ratio=1.80,
                 depth_at=0.28, fullness=5.5, snout=0.28, peduncle=0.10,
                 belly=0.46, width_follow=0.45, section=2.4, head_frac=0.22,
                 caudal_shape="pointed", caudal_len=0.60, caudal_span=0.30,
                 dorsal_shape="none", dorsal_height=0.0,
                 anal_height=0.0, anal_len=0.05,
                 pectoral=0.0, pelvic=0.0, eye=1.0, fin_thick=2,
                 back_frac=0.60, belly_frac=0.28,
                 pattern="spots", pattern_count=18, pattern_scale=0.03,
                 mat_back="skin_dark", mat_flank="skin_dark",
                 mat_belly="skin_pale", mat_fin="skin_dark",
                 mat_pattern="plume_white", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.5,
                 place_abundance=0.1, place_spacing_m=40.0,
                 det_despawn_m=120.0, det_school_min=1, det_school_max=6,
                 det_school_radius_m=25.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=40.0,
                 det_min_water_depth_m=4.0, det_per_100m2=0.3)),
    ),
    "giant-manta-ray": (
        "5.00 m at 5 cm - a flat diamond wing with a whip tail",
        base(res="5", name="giant-manta-ray",
             notes="THE SAME PROBE AT FIVE METRES. Nothing else in the library "
                   "is this shape, and the two forward cephalic lobes that "
                   "identify it are not drawn -- there is no primitive for a "
                   "pair of horns off the front of a disc, and faking them with "
                   "barbels would put threads where blades belong. Recorded as "
                   "a known omission.\n\n"
                   "Black above, white below, with the hard boundary carrying "
                   "the whole animal at a hundred voxels.",
             **f(length_m=5.00, depth_ratio=0.10, width_ratio=1.80,
                 depth_at=0.30, fullness=5.0, snout=0.34, peduncle=0.08,
                 belly=0.46, width_follow=0.40, section=2.4, head_frac=0.20,
                 caudal_shape="pointed", caudal_len=0.40, caudal_span=0.30,
                 dorsal_shape="none", dorsal_height=0.0,
                 anal_height=0.0, anal_len=0.05,
                 pectoral=0.0, pelvic=0.0, eye=1.0, fin_thick=3,
                 back_frac=0.62, belly_frac=0.34, pattern="none",
                 mat_back="skin_dark", mat_flank="skin_dark",
                 mat_belly="skin_pale", mat_fin="skin_dark",
                 mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.04, place_spacing_m=200.0,
                 det_despawn_m=300.0, det_school_min=1, det_school_max=3,
                 det_school_radius_m=60.0, det_water="ocean",
                 det_depth_min_m=0.0, det_depth_max_m=60.0,
                 det_min_water_depth_m=15.0, det_per_100m2=0.05)),
    ),
    "thornback-ray": (
        "0.70 m at 2 cm - rounded diamond, mottled brown, a row of thorns",
        base(res="2", name="thornback-ray",
             notes="THE SHALLOW-WATER RAY, and the third of the depressiform "
                   "probes -- shorter-winged and more rounded than the eagle "
                   "ray, lying on sand rather than flying. The row of heavy "
                   "thorns down the back and tail is what names it and is not "
                   "drawn: at 2 cm a thorn is one voxel, and a line of "
                   "one-voxel bumps on a mottled back is noise. The mottle is "
                   "what carries it instead.",
             **f(length_m=0.70, depth_ratio=0.11, width_ratio=1.70,
                 depth_at=0.34, fullness=4.5, snout=0.26, peduncle=0.14,
                 belly=0.46, width_follow=0.55, section=2.3, head_frac=0.24,
                 caudal_shape="pointed", caudal_len=0.45, caudal_span=0.30,
                 dorsal_shape="none", dorsal_height=0.0,
                 anal_height=0.0, anal_len=0.05,
                 pectoral=0.0, pelvic=0.0, eye=1.0, fin_thick=2,
                 back_frac=0.60, belly_frac=0.30,
                 pattern="mottle", pattern_scale=0.10, pattern_strength=0.45,
                 mat_back="skin_brown", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.6,
                 place_abundance=0.2, place_spacing_m=12.0,
                 det_despawn_m=70.0, det_school_min=1, det_school_max=3,
                 det_school_radius_m=15.0, det_water="shallow",
                 det_depth_min_m=1.0, det_depth_max_m=30.0,
                 det_min_water_depth_m=2.0, det_per_100m2=0.8)),
    ),
    "european-plaice": (
        "0.40 m - small brown flatfish with a scatter of orange spots",
        base(name="european-plaice",
             notes="THE FLATFISH PROBE, AND IT IS A DIFFERENT PROBLEM FROM THE "
                   "RAY. A ray is flattened top-to-bottom and swims that way; a "
                   "flatfish is flattened SIDE TO SIDE and then lies over, with "
                   "both eyes on the upper side. The generator has no way to "
                   "roll a fish onto its flank, so this is drawn as a "
                   "depressiform oval instead -- wide, shallow, with a fin "
                   "ridge all the way round. That is the right OUTLINE seen "
                   "from above, which is how a plaice is seen, and it is the "
                   "wrong anatomy.\n\n"
                   "THE ORANGE SPOTS ARE THE SPECIES and they survive the "
                   "compromise unchanged.",
             **f(length_m=0.40, depth_ratio=0.16, width_ratio=1.60,
                 depth_at=0.42, fullness=4.0, snout=0.34, peduncle=0.36,
                 belly=0.46, width_follow=0.70, section=2.2, head_frac=0.22,
                 caudal_shape="rounded", caudal_len=0.14, caudal_span=0.85,
                 dorsal_shape="ridge", dorsal_start=0.10, dorsal_len=0.78,
                 dorsal_height=0.40,
                 anal_height=0.36, anal_len=0.50,
                 pectoral=0.22, pelvic=0.0, eye=1.0,
                 back_frac=0.62, belly_frac=0.30,
                 pattern="spots", pattern_count=14, pattern_scale=0.05,
                 mat_back="skin_brown", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_orange", mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.6,
                 place_abundance=0.4, place_spacing_m=3.0,
                 det_despawn_m=50.0, det_school_min=1, det_school_max=6,
                 det_school_radius_m=10.0, det_water="shallow",
                 det_depth_min_m=0.5, det_depth_max_m=30.0,
                 det_min_water_depth_m=1.0, det_per_100m2=3.0)),
    ),
    "european-flounder": (
        "0.35 m - flat oval, uniform brown with paler blotches",
        base(name="european-flounder",
             notes="THE PLAICE'S PLAIN COUNTERPART, carrying the same "
                   "depressiform compromise -- see that spec's notes for what "
                   "is approximated and why. The beach file recommends building "
                   "ONE flatfish and looking at it before committing to four; "
                   "there are two here rather than four, and the second exists "
                   "only to test whether a MOTTLE and a SPOT pattern separate "
                   "two otherwise identical brown ovals.",
             **f(length_m=0.35, depth_ratio=0.15, width_ratio=1.55,
                 depth_at=0.42, fullness=4.0, snout=0.34, peduncle=0.36,
                 belly=0.46, width_follow=0.70, section=2.2, head_frac=0.22,
                 caudal_shape="truncate", caudal_len=0.14, caudal_span=0.85,
                 dorsal_shape="ridge", dorsal_start=0.10, dorsal_len=0.78,
                 dorsal_height=0.38,
                 anal_height=0.34, anal_len=0.50,
                 pectoral=0.22, pelvic=0.0, eye=1.0,
                 back_frac=0.60, belly_frac=0.30,
                 pattern="mottle", pattern_scale=0.13, pattern_strength=0.40,
                 mat_back="skin_brown", mat_flank="skin_olive",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.4,
                 place_abundance=0.4, place_spacing_m=3.0,
                 det_despawn_m=50.0, det_school_min=1, det_school_max=5,
                 det_school_radius_m=10.0, det_water="shallow",
                 det_depth_min_m=0.2, det_depth_max_m=20.0,
                 det_min_water_depth_m=0.5, det_per_100m2=3.0)),
    ),
    "atlantic-halibut": (
        "2.00 m at 5 cm - a huge flat diamond, dark above and pure white below",
        base(res="5", name="atlantic-halibut",
             notes="THE FLATFISH AT FORTY VOXELS, which is where the "
                   "depressiform compromise has the most room to work. Same "
                   "approximation as `european-plaice`. Its whole colour scheme "
                   "is one hard boundary -- near-black above, pure white below "
                   "-- so it is the cleanest test of whether countershading "
                   "alone identifies an animal with no marking at all.",
             **f(length_m=2.00, depth_ratio=0.17, width_ratio=1.60,
                 depth_at=0.44, fullness=3.6, snout=0.32, peduncle=0.34,
                 belly=0.46, width_follow=0.72, section=2.2, head_frac=0.22,
                 caudal_shape="truncate", caudal_len=0.14, caudal_span=0.95,
                 dorsal_shape="ridge", dorsal_start=0.08, dorsal_len=0.80,
                 dorsal_height=0.36,
                 anal_height=0.32, anal_len=0.50,
                 pectoral=0.22, pelvic=0.0, eye=1.0, fin_thick=2,
                 back_frac=0.62, belly_frac=0.34, pattern="none",
                 mat_back="skin_dark", mat_flank="skin_dark",
                 mat_belly="plume_white", mat_fin="skin_dark",
                 mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.06, place_spacing_m=60.0,
                 det_despawn_m=140.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=20.0, det_water="ocean",
                 det_depth_min_m=10.0, det_depth_max_m=400.0,
                 det_min_water_depth_m=15.0, det_per_100m2=0.1)),
    ),
    # --- reef ----------------------------------------------------------------
    "parrotfish": (
        "0.50 m - deep body, fused beak jaws, blue-green with a pink cheek",
        base(name="parrotfish",
             notes="A BEAK RATHER THAN A MOUTH, which is drawn as a very blunt "
                   "high snout (0.66) on a short head -- the front of the "
                   "animal is a cropped-off wedge rather than a taper. Blue-"
                   "green with a bright cheek streak, which is a `stripe` "
                   "marking placed high and short.",
             **f(length_m=0.50, depth_ratio=0.34, width_ratio=0.44,
                 depth_at=0.36, fullness=4.2, snout=0.66, peduncle=0.32,
                 belly=0.52, width_follow=1.20, section=2.0, head_frac=0.24,
                 caudal_shape="truncate", caudal_len=0.20, caudal_span=1.20,
                 dorsal_shape="ridge", dorsal_start=0.26, dorsal_len=0.48,
                 dorsal_height=0.30,
                 anal_height=0.28, anal_len=0.20,
                 pectoral=0.36, pelvic=0.18, eye=1.0,
                 back_frac=0.32, belly_frac=0.26,
                 pattern="stripe", pattern_width=0.14, pattern_pos=0.72,
                 mat_back="skin_green", mat_flank="skin_blue",
                 mat_belly="skin_green", mat_fin="skin_blue",
                 mat_pattern="skin_orange", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.4,
                 place_abundance=0.5, place_spacing_m=3.0,
                 det_despawn_m=50.0, det_school_min=2, det_school_max=15,
                 det_school_radius_m=8.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=25.0,
                 det_min_water_depth_m=2.0, det_per_100m2=6.0)),
    ),
    "emperor-angelfish": (
        "0.35 m - deep disc, tight diagonal blue and yellow stripes, dark mask",
        base(name="emperor-angelfish",
             notes="THE MOST STRIPES ANY FISH HERE CARRIES at fourteen, and it "
                   "is the test of the 2-on-2-off floor: at 35 voxels on a "
                   "depth of 0.62, fourteen bands is about two voxels each, "
                   "which is exactly the documented minimum. If they blur "
                   "together that is the count being wrong rather than the "
                   "mechanism, and lowering it is one number.\n\n"
                   "Drawn as BARS, because the real stripes run diagonally and "
                   "the generator has vertical and horizontal only; vertical is "
                   "the closer of the two.",
             **f(length_m=0.35, depth_ratio=0.62, width_ratio=0.24,
                 depth_at=0.40, fullness=5.5, snout=0.34, peduncle=0.22,
                 belly=0.50, width_follow=1.30, section=1.4, head_frac=0.24,
                 caudal_shape="rounded", caudal_len=0.16, caudal_span=1.05,
                 dorsal_shape="ridge", dorsal_start=0.22, dorsal_len=0.58,
                 dorsal_height=0.34,
                 anal_height=0.34, anal_len=0.40,
                 pectoral=0.30, pelvic=0.24, eye=1.0,
                 back_frac=0.28, belly_frac=0.24,
                 pattern="bars", pattern_count=14, pattern_width=0.45,
                 mat_back="skin_blue", mat_flank="skin_blue",
                 mat_belly="skin_blue", mat_fin="skin_yellow",
                 mat_pattern="skin_yellow", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.4, place_spacing_m=4.0,
                 det_despawn_m=40.0, det_school_min=1, det_school_max=3,
                 det_school_radius_m=5.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=25.0,
                 det_min_water_depth_m=2.0, det_per_100m2=4.0)),
    ),
    "racoon-butterflyfish": (
        "0.22 m - disc body, black eye band and a second dark bar behind it",
        base(name="racoon-butterflyfish",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 20 cm, at the floor and "
                   "recorded.\n\n"
                   "TWO BARS AND NOTHING ELSE, which is `pattern_count` 2 -- "
                   "the lowest bar count in the library. The first sits ON the "
                   "eye, which is the mask trick a fish cannot do directly, so "
                   "it is placed by `pattern_pos` instead and the eye is drawn "
                   "on top of it.",
             **f(length_m=0.22, depth_ratio=0.58, width_ratio=0.26,
                 depth_at=0.40, fullness=5.5, snout=0.26, peduncle=0.24,
                 belly=0.50, width_follow=1.30, section=1.5, head_frac=0.26,
                 caudal_shape="truncate", caudal_len=0.16, caudal_span=1.00,
                 dorsal_shape="ridge", dorsal_start=0.24, dorsal_len=0.56,
                 dorsal_height=0.32,
                 anal_height=0.32, anal_len=0.34,
                 pectoral=0.28, pelvic=0.22, eye=1.0,
                 back_frac=0.30, belly_frac=0.22,
                 pattern="bars", pattern_count=2, pattern_width=0.34,
                 mat_back="skin_yellow", mat_flank="skin_yellow",
                 mat_belly="skin_pale", mat_fin="skin_yellow",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.4,
                 place_abundance=0.5, place_spacing_m=2.5,
                 det_despawn_m=35.0, det_school_min=2, det_school_max=8,
                 det_school_radius_m=4.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=20.0,
                 det_min_water_depth_m=1.5, det_per_100m2=8.0)),
    ),
    "moorish-idol": (
        "0.24 m - a near-triangle with three broad bands and a trailing filament",
        base(name="moorish-idol",
             notes="AUTHORED AT 0.24 m AGAINST A REAL 20 cm, because three "
                   "bands need at least twelve voxels of length between them "
                   "and twenty at this body depth does not leave it. Same fix "
                   "`clown-anemonefish` carries.\n\n"
                   "THE TRAILING DORSAL FILAMENT is drawn as an extremely tall "
                   "narrow sail (height 1.5, length 0.16) rather than as a "
                   "thread, because a one-voxel thread would come off the fish. "
                   "That is a stylisation of a real feature and it is why the "
                   "outline still reads as a triangle.",
             **f(length_m=0.24, depth_ratio=0.66, width_ratio=0.22,
                 depth_at=0.36, fullness=6.0, snout=0.18, peduncle=0.20,
                 belly=0.50, width_follow=1.35, section=1.4, head_frac=0.26,
                 caudal_shape="truncate", caudal_len=0.14, caudal_span=0.90,
                 dorsal_shape="sail", dorsal_start=0.22, dorsal_len=0.16,
                 dorsal_height=1.50,
                 anal_height=0.34, anal_len=0.34,
                 pectoral=0.26, pelvic=0.26, eye=1.0,
                 back_frac=0.28, belly_frac=0.24,
                 pattern="bars", pattern_count=3, pattern_width=0.42,
                 mat_back="plume_white", mat_flank="plume_white",
                 mat_belly="plume_white", mat_fin="skin_yellow",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.35, place_spacing_m=3.0,
                 det_despawn_m=35.0, det_school_min=1, det_school_max=6,
                 det_school_radius_m=4.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=25.0,
                 det_min_water_depth_m=2.0, det_per_100m2=5.0)),
    ),
    "lionfish": (
        "0.35 m - red-and-white banded body inside a fan of long spines",
        base(name="lionfish",
             notes="FEWER AND THICKER SPINES THAN THE ANIMAL HAS, AND THE OCEAN "
                   "FILE SAYS TO SAY SO. Eighteen separated spines on a 35 cm "
                   "body is a spine every 2 cm with a 1 cm gap; at 1 cm the "
                   "gaps close and the fan becomes a paddle. So the dorsal is "
                   "drawn as a very tall short SPINY fin and the pectorals as "
                   "unusually long broad plates, which gives the fanned "
                   "silhouette without pretending the count is achievable.\n\n"
                   "Hard red-and-white banding does the rest, and it is the one "
                   "reef fish here whose bars run onto its fins in life -- "
                   "which the generator cannot do and which is not faked.",
             **f(length_m=0.35, depth_ratio=0.30, width_ratio=0.55,
                 depth_at=0.34, fullness=4.4, snout=0.34, peduncle=0.28,
                 belly=0.50, width_follow=1.20, section=2.0, head_frac=0.30,
                 caudal_shape="rounded", caudal_len=0.20, caudal_span=1.10,
                 dorsal_shape="spiny", dorsal_start=0.24, dorsal_len=0.34,
                 dorsal_height=1.60,
                 anal_height=0.50, anal_len=0.18,
                 pectoral=1.10, pectoral_aspect=0.80, pelvic=0.55,
                 eye=1.0, back_frac=0.34, belly_frac=0.26,
                 pattern="bars", pattern_count=9, pattern_width=0.45,
                 mat_back="skin_red", mat_flank="plume_white",
                 mat_belly="plume_white", mat_fin="skin_red",
                 mat_pattern="skin_red", mat_eye="skin_yellow",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.25, place_spacing_m=6.0,
                 det_despawn_m=40.0, det_school_min=1, det_school_max=3,
                 det_school_radius_m=5.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=30.0,
                 det_min_water_depth_m=2.0, det_per_100m2=2.0)),
    ),
    "porcupinefish": (
        "0.40 m - inflated globe covered in short erect spines, huge eyes",
        base(name="porcupinefish",
             notes="GLOBIFORM, which in this generator is a depth ratio of 0.52 "
                   "on a width ratio near 1.0 -- a ball rather than a disc, "
                   "which is what separates it from the reef's several "
                   "flattened species. The spines are 1 cm and are not drawn; "
                   "the round outline and the enormous eye carry it.",
             **f(length_m=0.40, depth_ratio=0.52, width_ratio=0.92,
                 depth_at=0.34, fullness=5.5, snout=0.56, peduncle=0.26,
                 belly=0.54, width_follow=1.40, section=2.6, head_frac=0.30,
                 caudal_shape="rounded", caudal_len=0.16, caudal_span=0.90,
                 dorsal_shape="triangular", dorsal_start=0.66, dorsal_len=0.14,
                 dorsal_height=0.30,
                 anal_height=0.28, anal_len=0.12,
                 pectoral=0.34, pelvic=0.0, eye=2.0,
                 back_frac=0.40, belly_frac=0.30,
                 pattern="spots", pattern_count=16, pattern_scale=0.05,
                 mat_back="plume_buff", mat_flank="plume_buff",
                 mat_belly="skin_pale", mat_fin="plume_buff",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.2, place_spacing_m=8.0,
                 det_despawn_m=40.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=6.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=30.0,
                 det_min_water_depth_m=2.0, det_per_100m2=1.5)),
    ),
    "yellow-boxfish": (
        "0.25 m - a near-cube with tiny fins at the corners",
        base(name="yellow-boxfish",
             notes="THE ONLY FISH WHOSE SILHOUETTE IS A RECTANGLE, and it is "
                   "one number: `fish.section` at 3.6, near the top of the "
                   "superellipse range, which turns the cross-section into a "
                   "rounded BOX. Everything else here is between 1.4 and 2.8. A "
                   "flat yellow body with small dark spots and almost no fins.",
             **f(length_m=0.25, depth_ratio=0.60, width_ratio=0.95,
                 depth_at=0.42, fullness=8.0, snout=0.80, peduncle=0.34,
                 belly=0.50, width_follow=1.10, section=3.6, head_frac=0.26,
                 caudal_shape="truncate", caudal_len=0.16, caudal_span=0.80,
                 dorsal_shape="triangular", dorsal_start=0.66, dorsal_len=0.10,
                 dorsal_height=0.18,
                 anal_height=0.16, anal_len=0.10,
                 pectoral=0.20, pelvic=0.0, eye=1.0,
                 back_frac=0.36, belly_frac=0.28,
                 pattern="spots", pattern_count=12, pattern_scale=0.05,
                 mat_back="skin_yellow", mat_flank="skin_yellow",
                 mat_belly="skin_yellow", mat_fin="skin_yellow",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.25, place_spacing_m=4.0,
                 det_despawn_m=35.0, det_school_min=1, det_school_max=3,
                 det_school_radius_m=4.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=20.0,
                 det_min_water_depth_m=1.5, det_per_100m2=2.5)),
    ),
    "bluestripe-snapper": (
        "0.30 m - yellow with four hard-edged horizontal blue stripes",
        base(name="bluestripe-snapper",
             notes="THE TEXTBOOK 2-ON-2-OFF STRIPE TEST, and the ocean file "
                   "calls it exactly that. Four stripes on a 30-voxel fish "
                   "whose flank is about nine voxels deep is two voxels of "
                   "stripe and two of gap, which is the documented floor -- so "
                   "if this species reads, the floor is right, and if it blurs, "
                   "the floor is optimistic. The generator draws one stripe, so "
                   "the four are got from a wide single stripe on a strongly "
                   "contrasting ground; that is an approximation and it is "
                   "recorded.",
             **f(length_m=0.30, depth_ratio=0.34, width_ratio=0.40,
                 depth_at=0.38, fullness=4.0, snout=0.30, peduncle=0.24,
                 belly=0.52, width_follow=1.30, section=1.9, head_frac=0.28,
                 caudal_shape="forked", caudal_len=0.18, caudal_span=1.10,
                 caudal_fork=0.30,
                 dorsal_shape="spiny", dorsal_start=0.30, dorsal_len=0.42,
                 dorsal_height=0.34,
                 anal_height=0.30, anal_len=0.16,
                 pectoral=0.30, pelvic=0.20, eye=1.0,
                 back_frac=0.30, belly_frac=0.24,
                 pattern="stripe", pattern_width=0.22, pattern_pos=0.60,
                 mat_back="skin_yellow", mat_flank="skin_yellow",
                 mat_belly="plume_white", mat_fin="skin_yellow",
                 mat_pattern="skin_blue", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.7, place_spacing_m=1.5,
                 det_despawn_m=40.0, det_school_min=10, det_school_max=80,
                 det_school_radius_m=5.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=30.0,
                 det_min_water_depth_m=2.0, det_per_100m2=25.0)),
    ),
    "coral-grouper": (
        "0.60 m at 2 cm - heavy blunt-headed body, red with small blue spots",
        base(res="2", name="coral-grouper",
             notes="THE HEAVY AMBUSH SHAPE ON A REEF: a big head, a big mouth "
                   "and a body that carries its mass forward, which is "
                   "`depth_at` 0.30 -- well ahead of the 0.36-0.42 most fish "
                   "here use. Small pale spots on red, which is the one "
                   "spot-on-saturated-colour scheme in the library.",
             **f(length_m=0.60, depth_ratio=0.30, width_ratio=0.62,
                 depth_at=0.30, fullness=3.8, snout=0.46, peduncle=0.30,
                 belly=0.52, width_follow=1.20, section=2.2, head_frac=0.34,
                 caudal_shape="rounded", caudal_len=0.16, caudal_span=1.05,
                 dorsal_shape="spiny", dorsal_start=0.30, dorsal_len=0.42,
                 dorsal_height=0.32,
                 anal_height=0.28, anal_len=0.16,
                 pectoral=0.34, pelvic=0.22, eye=1.0, fin_thick=2,
                 back_frac=0.34, belly_frac=0.24,
                 pattern="spots", pattern_count=20, pattern_scale=0.03,
                 mat_back="skin_red", mat_flank="skin_red",
                 mat_belly="skin_orange", mat_fin="skin_red",
                 mat_pattern="plume_cyan", mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.25, place_spacing_m=8.0,
                 det_despawn_m=55.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=8.0, det_water="reef",
                 det_depth_min_m=2.0, det_depth_max_m=50.0,
                 det_min_water_depth_m=3.0, det_per_100m2=1.5)),
    ),
    "napoleon-wrasse": (
        "1.50 m at 2 cm - huge deep wrasse with a bulging forehead hump",
        base(res="2", name="napoleon-wrasse",
             notes="THE FOREHEAD HUMP IS THE SPECIES and there is no hump "
                   "parameter, so it is drawn as a very high snout (0.78) with "
                   "the body's deepest point pulled right forward to 0.22 -- "
                   "which puts the mass over the head and gives the profile a "
                   "bulge. An approximation, stated.",
             **f(length_m=1.50, depth_ratio=0.36, width_ratio=0.46,
                 depth_at=0.22, fullness=4.6, snout=0.78, peduncle=0.30,
                 belly=0.50, width_follow=1.25, section=2.0, head_frac=0.30,
                 caudal_shape="truncate", caudal_len=0.16, caudal_span=1.05,
                 dorsal_shape="ridge", dorsal_start=0.24, dorsal_len=0.52,
                 dorsal_height=0.30,
                 anal_height=0.28, anal_len=0.22,
                 pectoral=0.36, pelvic=0.20, eye=1.0, fin_thick=2,
                 back_frac=0.34, belly_frac=0.26, pattern="none",
                 mat_back="skin_green", mat_flank="skin_blue",
                 mat_belly="skin_green", mat_fin="skin_blue",
                 mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.1, place_spacing_m=30.0,
                 det_despawn_m=90.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=15.0, det_water="reef",
                 det_depth_min_m=2.0, det_depth_max_m=50.0,
                 det_min_water_depth_m=4.0, det_per_100m2=0.3)),
    ),
    "giant-moray": (
        "2.00 m at 2 cm - heavy anguilliform, no pectorals, mouth held open",
        base(res="2", name="giant-moray",
             notes="NO PECTORAL FINS AT ALL, which no other fish in the library "
                   "does -- so from any angle it is a pure tube with a fin "
                   "ridge, and that absence is most of the identification. A "
                   "dark mottle over the whole animal and a blunt head with the "
                   "jaw open, drawn as a very high snout.",
             **f(length_m=2.00, depth_ratio=0.13, width_ratio=0.72,
                 depth_at=0.34, fullness=2.6, snout=0.82, peduncle=0.28,
                 belly=0.50, width_follow=1.10, section=2.2, head_frac=0.16,
                 caudal_shape="pointed", caudal_len=0.10, caudal_span=0.85,
                 dorsal_shape="ridge", dorsal_start=0.10, dorsal_len=0.80,
                 dorsal_height=0.42,
                 anal_height=0.32, anal_len=0.50,
                 pectoral=0.0, pelvic=0.0, eye=1.0, fin_thick=2,
                 back_frac=0.40, belly_frac=0.24,
                 pattern="mottle", pattern_scale=0.07, pattern_strength=0.50,
                 mat_back="skin_dark", mat_flank="skin_brown",
                 mat_belly="skin_yellow", mat_fin="skin_dark",
                 mat_pattern="skin_pale", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.15, place_spacing_m=20.0,
                 det_despawn_m=70.0, det_school_min=1, det_school_max=1,
                 det_school_radius_m=4.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=40.0,
                 det_min_water_depth_m=2.0, det_per_100m2=0.5)),
    ),
    # --- cetaceans -----------------------------------------------------------
    "fin-whale": (
        "20 m at 10 cm - long and sleek, and asymmetric on purpose",
        base(kind="cetacean", res="10", name="fin-whale",
             notes="THE ASYMMETRIC JAW -- white on the RIGHT side only, dark on "
                   "the left -- is the species and the ocean file points out "
                   "that it is FREE at any lattice, because it is a palette "
                   "split rather than geometry. The generator paints a "
                   "countershading field symmetrically, so what is authored "
                   "here is a very high pale belly reaching up the flank, which "
                   "gets the pale-jaw read from one side and not the other only "
                   "by accident. THAT IS AN APPROXIMATION and a true "
                   "left/right split would be a new mechanism worth about one "
                   "field in the fish colour code.\n\n"
                   "The second-largest animal there is, and its outline against "
                   "`blue-whale` is one number: a taller, more falcate, further "
                   "forward dorsal fin.",
             **cet(length_m=20.0, depth_ratio=0.155, width_ratio=0.92,
                   depth_at=0.36, fullness=2.6, snout=0.20, peduncle=0.16,
                   width_follow=1.40, head_frac=0.24,
                   caudal_len=0.11, caudal_span=1.30, caudal_fork=0.34,
                   dorsal_shape="triangular", dorsal_start=0.70,
                   dorsal_len=0.08, dorsal_height=0.28,
                   pectoral=0.30, pectoral_aspect=0.30, fin_thick=2,
                   back_frac=0.52, belly_frac=0.40,
                   mat_back="plume_slate", mat_flank="plume_grey",
                   mat_belly="plume_white", mat_fin="plume_slate",
                   bio_ocean=1.0,
                   place_abundance=0.04, place_spacing_m=400.0,
                   det_despawn_m=800.0, det_school_min=1, det_school_max=3,
                   det_school_radius_m=200.0, det_water="ocean",
                   det_depth_min_m=0.0, det_depth_max_m=200.0,
                   det_min_water_depth_m=50.0, det_per_100m2=0.01)),
    ),
    "sei-whale": (
        "15 m at 10 cm - between minke and fin, tall dorsal set well forward",
        base(kind="cetacean", res="10", name="sei-whale",
             notes="THE MIDDLE ROrQUAL, and it earns a spec because the group's "
                   "identification IS the dorsal fin's height and position: a "
                   "sei's is tall and falcate and sits further forward than a "
                   "fin whale's. Two numbers separate three animals, which is "
                   "the argument for having all three.",
             **cet(length_m=15.0, depth_ratio=0.165, width_ratio=0.92,
                   depth_at=0.36, fullness=2.6, snout=0.20, peduncle=0.16,
                   width_follow=1.40, head_frac=0.23,
                   caudal_len=0.11, caudal_span=1.28, caudal_fork=0.34,
                   dorsal_shape="triangular", dorsal_start=0.62,
                   dorsal_len=0.08, dorsal_height=0.42,
                   pectoral=0.28, pectoral_aspect=0.30, fin_thick=2,
                   back_frac=0.54, belly_frac=0.32,
                   mat_back="plume_slate", mat_flank="plume_slate",
                   mat_belly="plume_grey", mat_fin="plume_slate",
                   bio_ocean=1.0,
                   place_abundance=0.03, place_spacing_m=400.0,
                   det_despawn_m=700.0, det_school_min=1, det_school_max=3,
                   det_school_radius_m=200.0, det_water="ocean",
                   det_depth_min_m=0.0, det_depth_max_m=150.0,
                   det_min_water_depth_m=40.0, det_per_100m2=0.01)),
    ),
    "minke-whale": (
        "8 m at 5 cm - smallest rorqual, sharp rostrum, white flipper band",
        base(kind="cetacean", res="5", name="minke-whale",
             notes="THE SMALLEST ROrQUAL AND THE ONLY ONE ON THE 5 cm LATTICE, "
                   "which is what lets it carry a marking the great whales "
                   "cannot: a clean white band across each flipper. That is "
                   "drawn as a pale flipper against a dark body -- the "
                   "generator colours a whole fin -- and the band itself is an "
                   "approximation.",
             **cet(length_m=8.0, depth_ratio=0.17, width_ratio=0.92,
                   depth_at=0.36, fullness=2.6, snout=0.16, peduncle=0.16,
                   width_follow=1.40, head_frac=0.22,
                   caudal_len=0.12, caudal_span=1.25, caudal_fork=0.32,
                   dorsal_shape="triangular", dorsal_start=0.62,
                   dorsal_len=0.10, dorsal_height=0.40,
                   pectoral=0.26, pectoral_aspect=0.35, fin_thick=2,
                   back_frac=0.52, belly_frac=0.34,
                   mat_back="skin_dark", mat_flank="plume_slate",
                   mat_belly="plume_white", mat_fin="plume_white",
                   bio_ocean=1.0,
                   place_abundance=0.06, place_spacing_m=250.0,
                   det_despawn_m=500.0, det_school_min=1, det_school_max=2,
                   det_school_radius_m=120.0, det_water="ocean",
                   det_depth_min_m=0.0, det_depth_max_m=120.0,
                   det_min_water_depth_m=25.0, det_per_100m2=0.02)),
    ),
    "grey-whale": (
        "13 m at 10 cm - mottled grey, NO dorsal fin, a knuckled ridge instead",
        base(kind="cetacean", res="10", name="grey-whale",
             notes="NO DORSAL FIN AT ALL, which is the strongest single "
                   "statement available in this group -- a whale's fin is the "
                   "thing everyone looks for, and its absence identifies two "
                   "species instantly. `dorsal_height` 0 and a heavy mottle "
                   "over the whole animal for the barnacle patches.\n\n"
                   "The low knuckled ridge that replaces the fin is not drawn: "
                   "on a 130-voxel animal each knuckle would be one voxel.",
             **cet(length_m=13.0, depth_ratio=0.19, width_ratio=0.95,
                   depth_at=0.34, fullness=2.6, snout=0.30, peduncle=0.20,
                   width_follow=1.30, head_frac=0.22,
                   caudal_len=0.12, caudal_span=1.30, caudal_fork=0.32,
                   dorsal_shape="none", dorsal_height=0.0,
                   pectoral=0.26, pectoral_aspect=0.45, fin_thick=2,
                   back_frac=0.50, belly_frac=0.24,
                   pattern="mottle", pattern_scale=0.05,
                   pattern_strength=0.35,
                   mat_back="plume_grey", mat_flank="plume_grey",
                   mat_belly="plume_grey", mat_fin="plume_grey",
                   mat_pattern="plume_white",
                   bio_ocean=1.0, bio_beach=0.3,
                   place_abundance=0.04, place_spacing_m=300.0,
                   det_despawn_m=600.0, det_school_min=1, det_school_max=3,
                   det_school_radius_m=150.0, det_water="ocean",
                   det_depth_min_m=0.0, det_depth_max_m=60.0,
                   det_min_water_depth_m=15.0, det_per_100m2=0.01)),
    ),
    "north-atlantic-right-whale": (
        "15 m at 10 cm - no dorsal, strongly arched jawline, pale callosities",
        base(kind="cetacean", res="10", name="north-atlantic-right-whale",
             notes="THE SECOND FINLESS WHALE, and deliberately built to differ "
                   "from the grey whale in every other way: a much deeper body "
                   "(0.24 against 0.19), a strongly arched jawline drawn as a "
                   "very high snout, an almost black skin, and the pale "
                   "callosity patches on the head drawn as a saddle marking "
                   "placed forward. Two finless whales that could be confused "
                   "would be a waste of one spec, so the pair is the test.",
             **cet(length_m=15.0, depth_ratio=0.24, width_ratio=0.98,
                   depth_at=0.32, fullness=2.8, snout=0.66, peduncle=0.18,
                   width_follow=1.35, head_frac=0.28,
                   caudal_len=0.13, caudal_span=1.45, caudal_fork=0.38,
                   dorsal_shape="none", dorsal_height=0.0,
                   pectoral=0.32, pectoral_aspect=0.75, fin_thick=2,
                   back_frac=0.60, belly_frac=0.18,
                   pattern="saddle", pattern_scale=0.10,
                   pattern_strength=0.20,
                   mat_back="skin_dark", mat_flank="skin_dark",
                   mat_belly="plume_grey", mat_fin="skin_dark",
                   mat_pattern="plume_white",
                   bio_ocean=1.0,
                   place_abundance=0.02, place_spacing_m=500.0,
                   det_despawn_m=600.0, det_school_min=1, det_school_max=2,
                   det_school_radius_m=150.0, det_water="ocean",
                   det_depth_min_m=0.0, det_depth_max_m=80.0,
                   det_min_water_depth_m=20.0, det_per_100m2=0.008)),
    ),
    "narwhal": (
        "4.50 m at 5 cm - mottled, no dorsal, and half the asset is a tusk",
        base(kind="cetacean", res="5", name="narwhal",
             notes="THE TUSK IS HALF THE ASSET AND IT IS DRAWN AS A SNOUT, "
                   "WHICH IS AN APPROXIMATION. There is no projecting-spike "
                   "primitive; what the generator can do is taper the front of "
                   "the body to a very fine point over a long head, so `snout` "
                   "is 0.06 with `head_frac` 0.34. The result is a long "
                   "tapering spear continuous with the animal rather than a "
                   "separate spiral rod, and the spiral is not drawn at all.\n\n"
                   "AUTHORED AS THE MALE. Only males carry the tusk, so "
                   "`unsexed` here is a bull; that is the same declaration "
                   "problem `bird.sex_plumage` exists for, and the fish group "
                   "has no equivalent field -- so it is recorded here in the "
                   "notes and nowhere a probe can read it. That gap is worth "
                   "knowing about.",
             **cet(length_m=4.50, depth_ratio=0.20, width_ratio=0.95,
                   depth_at=0.38, fullness=2.6, snout=0.06, peduncle=0.20,
                   width_follow=1.30, head_frac=0.34,
                   caudal_len=0.14, caudal_span=1.25, caudal_fork=0.30,
                   dorsal_shape="none", dorsal_height=0.0,
                   pectoral=0.24, pectoral_aspect=0.60, fin_thick=2,
                   back_frac=0.48, belly_frac=0.30,
                   pattern="mottle", pattern_scale=0.06,
                   pattern_strength=0.40,
                   mat_back="plume_grey", mat_flank="plume_grey",
                   mat_belly="plume_white", mat_fin="plume_grey",
                   mat_pattern="skin_dark",
                   bio_ocean=1.0,
                   place_abundance=0.05, place_spacing_m=120.0,
                   det_despawn_m=250.0, det_school_min=2, det_school_max=12,
                   det_school_radius_m=60.0, det_water="ocean",
                   det_depth_min_m=0.0, det_depth_max_m=200.0,
                   det_min_water_depth_m=20.0, det_per_100m2=0.03)),
    ),
    "long-finned-pilot-whale": (
        "5.50 m at 5 cm - bulbous melon, very broad low-set dorsal, all black",
        base(kind="cetacean", res="5", name="long-finned-pilot-whale",
             notes="THE WIDEST DORSAL FIN IN THE GROUP -- `dorsal_len` 0.24 at "
                   "a height of only 0.30, which is a long low blade rather "
                   "than the tall triangle every dolphin here carries. That "
                   "shape plus a bulbous forehead and a completely black body "
                   "is the whole animal.",
             **cet(length_m=5.50, depth_ratio=0.22, width_ratio=0.95,
                   depth_at=0.32, fullness=3.0, snout=0.72, peduncle=0.22,
                   width_follow=1.35, head_frac=0.20,
                   caudal_len=0.13, caudal_span=1.25, caudal_fork=0.32,
                   dorsal_shape="triangular", dorsal_start=0.30,
                   dorsal_len=0.24, dorsal_height=0.30,
                   pectoral=0.50, pectoral_aspect=0.28, fin_thick=2,
                   back_frac=0.66, belly_frac=0.16,
                   mat_back="skin_dark", mat_flank="skin_dark",
                   mat_belly="plume_grey", mat_fin="skin_dark",
                   bio_ocean=1.0,
                   place_abundance=0.06, place_spacing_m=150.0,
                   det_despawn_m=300.0, det_school_min=6, det_school_max=40,
                   det_school_radius_m=80.0, det_water="ocean",
                   det_depth_min_m=0.0, det_depth_max_m=300.0,
                   det_min_water_depth_m=30.0, det_per_100m2=0.05)),
    ),
    "rissos-dolphin": (
        "3.50 m at 5 cm - blunt creased forehead, tall dorsal, scarred pale",
        base(kind="cetacean", res="5", name="rissos-dolphin",
             notes="NO BEAK AT ALL AND THE TALLEST DORSAL OF ANY DOLPHIN HERE, "
                   "which together are unmistakable beside `bottlenose-dolphin` "
                   "at the same size. Old animals scar nearly white, and that "
                   "is drawn as a heavy pale mottle over a grey body -- one of "
                   "the very few cases where a marking represents AGE rather "
                   "than species.",
             **cet(length_m=3.50, depth_ratio=0.21, width_ratio=0.95,
                   depth_at=0.34, fullness=3.0, snout=0.76, peduncle=0.24,
                   width_follow=1.35, head_frac=0.18,
                   caudal_len=0.13, caudal_span=1.28, caudal_fork=0.32,
                   dorsal_shape="triangular", dorsal_start=0.40,
                   dorsal_len=0.14, dorsal_height=0.75,
                   pectoral=0.46, pectoral_aspect=0.45, fin_thick=2,
                   back_frac=0.46, belly_frac=0.26,
                   pattern="mottle", pattern_scale=0.05,
                   pattern_strength=0.45,
                   mat_back="plume_slate", mat_flank="plume_grey",
                   mat_belly="plume_white", mat_fin="plume_slate",
                   mat_pattern="plume_white",
                   bio_ocean=1.0,
                   place_abundance=0.08, place_spacing_m=120.0,
                   det_despawn_m=250.0, det_school_min=3, det_school_max=20,
                   det_school_radius_m=60.0, det_water="ocean",
                   det_depth_min_m=0.0, det_depth_max_m=200.0,
                   det_min_water_depth_m=25.0, det_per_100m2=0.05)),
    ),
    "spinner-dolphin": (
        "2.00 m at 2 cm - slim three-tone flank and a long thin beak",
        base(kind="cetacean", res="2", name="spinner-dolphin",
             notes="A THREE-TONE FLANK -- dark cape, grey side, white belly -- "
                   "which is exactly what the countershading fields draw and is "
                   "why this one is authored at 2 cm rather than 5: at "
                   "100 voxels the three bands are each a dozen voxels deep and "
                   "the cape has room to curve. `common-dolphin` is at 2 cm for "
                   "the same reason and it is the only other one.\n\n"
                   "The longest, thinnest beak in the group.",
             **cet(length_m=2.00, depth_ratio=0.165, width_ratio=0.92,
                   depth_at=0.40, fullness=2.8, snout=0.14, peduncle=0.22,
                   width_follow=1.40, head_frac=0.26,
                   caudal_len=0.13, caudal_span=1.25, caudal_fork=0.32,
                   dorsal_shape="triangular", dorsal_start=0.44,
                   dorsal_len=0.14, dorsal_height=0.52,
                   pectoral=0.44, pectoral_aspect=0.50,
                   field_curve="cape", curve_at=0.48, curve_amount=0.22,
                   back_frac=0.40, belly_frac=0.32,
                   mat_back="skin_dark", mat_flank="plume_grey",
                   mat_belly="plume_white", mat_fin="skin_dark",
                   bio_ocean=1.0,
                   place_abundance=0.15, place_spacing_m=60.0,
                   det_despawn_m=180.0, det_school_min=8, det_school_max=80,
                   det_school_radius_m=60.0, det_water="ocean",
                   det_depth_min_m=0.0, det_depth_max_m=80.0,
                   det_min_water_depth_m=15.0, det_per_100m2=0.2)),
    ),
    "harbour-porpoise": (
        "1.60 m at 2 cm - small, blunt-faced, NO beak, small triangular dorsal",
        base(kind="cetacean", res="2", name="harbour-porpoise",
             notes="THE SMALLEST CETACEAN IN THE LIBRARY, and the porpoise/"
                   "dolphin separation is one number: no beak at all. `snout` "
                   "0.84 against a bottlenose's 0.30. Its dorsal is a small "
                   "low triangle rather than a falcate blade, which is the "
                   "second number. Nothing else about it is distinctive and it "
                   "does not need to be.",
             **cet(length_m=1.60, depth_ratio=0.22, width_ratio=0.95,
                   depth_at=0.36, fullness=3.0, snout=0.84, peduncle=0.24,
                   width_follow=1.35, head_frac=0.18,
                   caudal_len=0.14, caudal_span=1.20, caudal_fork=0.30,
                   dorsal_shape="triangular", dorsal_start=0.44,
                   dorsal_len=0.12, dorsal_height=0.34,
                   pectoral=0.34, pectoral_aspect=0.60,
                   back_frac=0.48, belly_frac=0.34,
                   mat_back="skin_dark", mat_flank="plume_grey",
                   mat_belly="plume_white", mat_fin="skin_dark",
                   bio_ocean=1.0, bio_beach=0.5,
                   place_abundance=0.15, place_spacing_m=60.0,
                   det_despawn_m=140.0, det_school_min=1, det_school_max=6,
                   det_school_radius_m=30.0, det_water="ocean",
                   det_depth_min_m=0.0, det_depth_max_m=60.0,
                   det_min_water_depth_m=5.0, det_per_100m2=0.1)),
    ),
    "amazon-river-dolphin": (
        "2.30 m at 5 cm - long bristled beak, no real dorsal, pink over grey",
        base(kind="cetacean", res="5", name="amazon-river-dolphin",
             notes="THE ONLY LARGE ANIMAL IN THE RAINFOREST THAT CAN BE BUILT "
                   "TODAY WITH NO NEW CODE, which the rainforest file says is "
                   "why it is worth more than its share of attention: every "
                   "other big animal in that biome waits on a quadruped "
                   "generator and this one is a cetacean.\n\n"
                   "A LOW RIDGE INSTEAD OF A DORSAL FIN, drawn as a long very "
                   "low `ridge`, plus very broad paddle flippers "
                   "(`pectoral_aspect` 1.1, the widest of any cetacean here) "
                   "and a pink body over grey. Freshwater: its biome weight is "
                   "rainforest, not ocean, which is the first cetacean in the "
                   "library for which that is true.\n\n"
                   "THE PINK IS APPROXIMATE AND THE PALETTE IS WHY. There is "
                   "no pink among the twenty-one creature materials the fish "
                   "rows may choose from; the only pale pink in the engine is "
                   "a TREE blossom material and it is not on that menu. "
                   "`skin_orange` over `plume_grey` is the nearest "
                   "pink-over-grey available and it is warmer and more "
                   "saturated than the animal. A creature pink is a real "
                   "palette gap and this species is the one that shows it.",
             **cet(length_m=2.30, depth_ratio=0.19, width_ratio=0.92,
                   depth_at=0.38, fullness=2.8, snout=0.10, peduncle=0.26,
                   width_follow=1.30, head_frac=0.30,
                   caudal_len=0.14, caudal_span=1.20, caudal_fork=0.26,
                   dorsal_shape="ridge", dorsal_start=0.40, dorsal_len=0.30,
                   dorsal_height=0.16,
                   pectoral=0.52, pectoral_aspect=1.10, fin_thick=2,
                   back_frac=0.42, belly_frac=0.30,
                   mat_back="plume_grey", mat_flank="skin_orange",
                   mat_belly="skin_orange", mat_fin="plume_grey",
                   bio_ocean=0.0, bio_rainforest=1.0,
                   place_abundance=0.1, place_spacing_m=60.0,
                   det_despawn_m=180.0, det_school_min=1, det_school_max=4,
                   det_school_radius_m=30.0, det_water="river",
                   det_depth_min_m=0.5, det_depth_max_m=20.0,
                   det_min_water_depth_m=3.0, det_per_100m2=0.15)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "ocean specs")
    written = 0
    for name, (blurb, changes) in SPECIES.items():
        s, rep = sm.patch(sm.default_spec(), changes)
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          label=name, width=30):
            written += 1
        print(f"  {'':<30} {blurb}")
    print(f"\n{written} of {len(SPECIES)} written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
