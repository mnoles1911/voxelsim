"""Author the shore and open-ocean birds: twelve waders and fifteen seabirds.

WHY THIS SET. `docs/biomes/README.md` §7 puts open-country and shore birds
fifth in the whole build queue, and gives the reason in one sentence: wader
identity lives almost entirely in BILL SHAPE, which `bird.bill_frac`,
`bill_curve`, `bill_depth` and `bill_gape` already parameterise. Many species
for very little new geometry. The ocean file's own priority list puts seabirds
second for the same reason -- the generator is shipped and proven, and a sea
with gulls, gannets and terns over it reads as inhabited where an empty one
reads as a bug.

Beach is 5.54% of land and it WRAPS EVERY COASTLINE AND EVERY LAKE SHORE in the
world, so a player at any water's edge anywhere sees these. Ocean is not land at
all and hosts exactly three kinds -- fish, cetacean and bird -- so a bird is a
third of everything that can be in it.

THE FOUR BILLS THIS SET EXISTS TO PROVE, and they are four different sliders:

    oystercatcher   straight, long, deep, blunt          curve  0.00
    curlew          long and evenly DEcurved             curve +0.62
    avocet          fine and strongly REcurved           curve -0.50
    puffin          short, enormously deep, triangular   depth  1.15

`bird.bill_curve` bends the CENTRELINE rather than aiming the bill, which is
why an avocet is a negative number here and not a rotation: a straight bill
pointing uphill is a different animal and looks close enough in a render to
survive several passes.

ALL AT 1 cm, which is shipped practice for every bird in the library
(`forge/kinds.py:129-134`, twenty species at 334-28,355 voxels each). Nothing
here is authored under 0.22 m: three of the small waders are 16-20 cm in life
and say so in their own `notes`, which is the same fix `european-robin`,
`great-tit`, `common-kingfisher` and `barn-swallow` already carry. A note is
what stops the next person "correcting" it back.

POSE. Waders are authored PERCHED -- which for a wader means standing, at a
near-level posture on long legs -- because that is where they are seen and
because `render.camera_for` sends a perched bird to the broadside camera, which
is the one that shows a bill. Six seabirds are authored FLYING, because a
gannet, an albatross, a skua, a fulmar, a shearwater and a kittiwake are seen in
the air and their wing planform is invisible folded. Both poses are authorable
on every species; the pose in the spec is where the species is usually seen, not
a restriction, and `tools/birdprobe.py --pose` builds both and checks each is
one piece.

COLOUR IS PUSHED PAST LIFE, the same way `tools/seed_birds.py` records: a
palette weighted by AREA is browns and greys, so the bright bills and legs that
identify waders are drawn at their flare rather than their average. Every
species here carries a bill that CONTRASTS with its head -- ten of the first
twenty birds in this library shipped with an invisible bill and it took a
fourth contrast check to find them, so `tools/birdprobe.py --read` gates on it
now and every spec below was checked against that gate.

    python tools/seed_shorebirds.py
    python tools/seed_shorebirds.py --force

SIZES ARE APPROXIMATE. Every length is the approximate figure from the biome
file it came from; `docs/biomes/README.md` §8 says plainly that those are
unsourced general-knowledge estimates. Nothing here is quoted as measured.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(**over):
    changes = {
        "kind": "bird",
        "resolution_cm": "1",
        "variation.amount": 1.0,
        "variation.height": 0.14,
        "variation.shape": 0.16,
        "variation.proportion": 0.18,
        "flock.entity_class": "detail",
    }
    changes.update(over)
    return changes


def b(**kw):
    """`bird.*`, `materials.bird_*`, `flock.*`, `biomes.*`, `placement.*`."""
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials.bird_" + k[len("mat_"):]] = v
        elif k.startswith("flock_"):
            out["flock." + k[len("flock_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        else:
            out["bird." + k] = v
    return out


def wader(**over):
    """What every long-legged shore bird shares: level body, long legs, a long
    neck it can pull in, short tail, and a bill that carries the species."""
    d = dict(posture_deg=8, body_depth=0.60, body_width=0.74, chest_at=0.32,
             breast=0.70, rump=0.42, fullness=3.0, section=2.1, belly=0.50,
             tail_shape="square", tail_droop=0.30, tail_width=0.60,
             pose="perched", eye=1.0)
    d.update(over)
    return b(**d)


SPECIES = {
    # --- waders: the bill is the species ------------------------------------
    "eurasian-oystercatcher": (
        "0.42 m - hard black and white, long straight orange dagger",
        base(name="eurasian-oystercatcher",
             notes="THE LOUDEST SILHOUETTE ON ANY SHORE, and the straight-bill "
                   "reference the curlew and the avocet are measured against: "
                   "0.185 of total length, dead straight, deep enough to see in "
                   "outline, and orange. Two flat colours on the body and one "
                   "on the bill is the whole animal.",
             **wader(length_m=0.42, bill_frac=0.185, head_frac=0.105,
                     neck_frac=0.085, body_frac=0.415, tail_frac=0.21,
                     head_size=0.95, neck_up_deg=26, neck_thick=0.52,
                     bill_depth=0.30, bill_curve=0.0, bill_gape=0.10,
                     wing_shape="pointed", wing_span=1.95, wing_aspect=8.4,
                     wing_fold=0.85, wing_thick=2, tail_thick=2,
                     leg_len=0.17, leg_thick=1.5, upperparts=0.55,
                     head_mark="none", wing_mark="bar", body_mark="none",
                     mark_width=0.20,
                     mat_back="skin_dark", mat_belly="plume_white",
                     mat_head="skin_dark", mat_wing="skin_dark",
                     mat_mark="plume_white", mat_head_mark="plume_white",
                     mat_bill="skin_orange", mat_eye="skin_red",
                     bio_beach=1.0, bio_grassland=0.3, bio_ocean=0.2,
                     place_abundance=0.45, place_spacing_m=25.0,
                     place_water_max_m=40.0,
                     flock_despawn_m=180.0, flock_size_min=2, flock_size_max=25,
                     flock_spread_m=40.0, flock_perch="waterside",
                     flock_height_min_m=2.0, flock_height_max_m=40.0,
                     flock_flight_share=0.30, flock_per_hectare=2.0)),
    ),
    "eurasian-curlew": (
        "0.55 m - the bill IS the species: long, thin, evenly downcurved",
        base(name="eurasian-curlew",
             notes="THE DECURVED EXTREME. A quarter of the total length is bill "
                   "and it curves along its whole run, which is the highest "
                   "`bill_curve` in the library -- a hoopoe is 0.55 on a bill "
                   "less than half as long. Nothing else about the bird is "
                   "distinctive at all: cryptic streaky brown, no marking that "
                   "reads, and it does not need one.",
             **wader(length_m=0.55, bill_frac=0.245, head_frac=0.085,
                     neck_frac=0.095, body_frac=0.395, tail_frac=0.18,
                     head_size=0.88, neck_up_deg=28, neck_thick=0.48,
                     bill_depth=0.14, bill_curve=0.62, bill_gape=0.04,
                     wing_shape="pointed", wing_span=1.85, wing_aspect=7.8,
                     wing_fold=0.80, wing_thick=2, tail_thick=2,
                     leg_len=0.19, leg_thick=1.5, upperparts=0.58,
                     head_mark="none", wing_mark="none", body_mark="streaked",
                     mark_count=7, mark_width=0.22,
                     mat_back="plume_buff", mat_belly="plume_white",
                     mat_head="plume_buff", mat_wing="skin_brown",
                     mat_mark="skin_dark", mat_head_mark="skin_dark",
                     mat_bill="skin_dark", mat_eye="skin_dark",
                     bio_beach=1.0, bio_grassland=0.5, bio_tundra_alpine=0.25,
                     place_abundance=0.3, place_spacing_m=45.0,
                     place_water_max_m=60.0,
                     flock_despawn_m=180.0, flock_size_min=1, flock_size_max=12,
                     flock_spread_m=60.0, flock_perch="waterside",
                     flock_height_min_m=3.0, flock_height_max_m=60.0,
                     flock_flight_share=0.30, flock_per_hectare=0.8)),
    ),
    "pied-avocet": (
        "0.44 m - fine strongly UPcurved bill, black cap and wing panels",
        base(name="pied-avocet",
             notes="THE ONLY RECURVED BILL IN THE LIBRARY, and the reason "
                   "`bird.bill_curve` runs negative at all. It sits directly "
                   "opposite the curlew on one slider, on birds of the same "
                   "length and nearly the same build, which is the cleanest "
                   "demonstration in the set that one number can be a species. "
                   "White bird, black cap and wing panels, blue-grey legs.",
             **wader(length_m=0.44, bill_frac=0.195, head_frac=0.095,
                     neck_frac=0.115, body_frac=0.395, tail_frac=0.20,
                     head_size=0.90, neck_up_deg=34, neck_thick=0.44,
                     bill_depth=0.12, bill_curve=-0.50, bill_gape=0.04,
                     wing_shape="pointed", wing_span=2.05, wing_aspect=8.8,
                     wing_fold=0.80, wing_thick=2, tail_thick=2,
                     leg_len=0.22, leg_thick=1.5, upperparts=0.50,
                     head_mark="cap", wing_mark="panel", body_mark="none",
                     mark_width=0.26,
                     mat_back="plume_white", mat_belly="plume_white",
                     mat_head="plume_white", mat_wing="plume_white",
                     mat_mark="skin_dark", mat_head_mark="skin_dark",
                     mat_bill="skin_dark", mat_eye="skin_dark",
                     bio_beach=1.0, bio_grassland=0.2,
                     place_abundance=0.25, place_spacing_m=30.0,
                     place_water_max_m=25.0,
                     flock_despawn_m=170.0, flock_size_min=3, flock_size_max=20,
                     flock_spread_m=35.0, flock_perch="waterside",
                     flock_height_min_m=2.0, flock_height_max_m=40.0,
                     flock_flight_share=0.25, flock_per_hectare=1.2)),
    ),
    "bar-tailed-godwit": (
        "0.38 m - long straight-to-slightly-upturned bill, rusty in summer",
        base(name="bar-tailed-godwit",
             notes="The MIDDLE of the bill-curve axis and the reason it is a "
                   "continuous slider rather than three entries: a godwit's "
                   "bill is very slightly recurved -- 0.12 here against an "
                   "avocet's 0.50 -- and that small difference plus a rusty "
                   "body is the whole separation from the curlew standing next "
                   "to it on the same mudflat.",
             **wader(length_m=0.38, bill_frac=0.195, head_frac=0.095,
                     neck_frac=0.085, body_frac=0.435, tail_frac=0.19,
                     head_size=0.92, neck_up_deg=26, neck_thick=0.50,
                     bill_depth=0.15, bill_curve=-0.12, bill_gape=0.05,
                     wing_shape="pointed", wing_span=1.90, wing_aspect=8.6,
                     wing_fold=0.85, wing_thick=2,
                     leg_len=0.16, leg_thick=1.5, upperparts=0.55,
                     head_mark="supercilium", wing_mark="none",
                     body_mark="barred", mark_count=6, mark_width=0.24,
                     mat_back="skin_brown", mat_belly="plume_rufous",
                     mat_head="plume_rufous", mat_wing="skin_brown",
                     mat_mark="skin_dark", mat_head_mark="plume_buff",
                     mat_bill="skin_dark", mat_eye="skin_dark",
                     bio_beach=1.0, bio_tundra_alpine=0.3,
                     place_abundance=0.3, place_spacing_m=20.0,
                     place_water_max_m=30.0,
                     flock_despawn_m=160.0, flock_size_min=5, flock_size_max=60,
                     flock_spread_m=40.0, flock_perch="waterside",
                     flock_height_min_m=2.0, flock_height_max_m=50.0,
                     flock_flight_share=0.30, flock_per_hectare=2.5)),
    ),
    "common-redshank": (
        "0.28 m - mid brown wader, and the legs are the mark",
        base(name="common-redshank",
             notes="THE LEGS CARRY IT, which nothing else in this library does. "
                   "The bird is an unremarkable streaky brown wader and what "
                   "identifies it is bright orange-red legs and a bill base of "
                   "the same colour -- so `materials.bird_bill` is doing double "
                   "duty here, because bill and legs share one material slot. "
                   "That shared slot is exactly right for this species and it "
                   "is worth noting that it would be wrong for a species whose "
                   "bill and legs differ.",
             **wader(length_m=0.28, bill_frac=0.135, head_frac=0.105,
                     neck_frac=0.075, body_frac=0.455, tail_frac=0.23,
                     head_size=0.95, neck_up_deg=26, neck_thick=0.52,
                     bill_depth=0.16, bill_curve=0.06, bill_gape=0.05,
                     wing_shape="pointed", wing_span=1.85, wing_aspect=8.0,
                     wing_fold=0.80,
                     leg_len=0.19, leg_thick=1.5, upperparts=0.58,
                     head_mark="supercilium", wing_mark="bar",
                     body_mark="speckled", mark_width=0.10,
                     mark_strength=0.28,
                     mat_back="skin_brown", mat_belly="plume_white",
                     mat_head="skin_brown", mat_wing="skin_brown",
                     mat_mark="skin_orange", mat_head_mark="plume_white",
                     mat_bill="skin_orange", mat_eye="skin_dark",
                     bio_beach=1.0, bio_grassland=0.4, bio_taiga=0.2,
                     place_abundance=0.4, place_spacing_m=18.0,
                     place_water_max_m=25.0,
                     flock_despawn_m=140.0, flock_size_min=2, flock_size_max=15,
                     flock_spread_m=25.0, flock_perch="waterside",
                     flock_height_min_m=1.0, flock_height_max_m=30.0,
                     flock_flight_share=0.28, flock_per_hectare=2.0)),
    ),
    "ruddy-turnstone": (
        "0.23 m - squat, short-legged, harlequin tortoiseshell back",
        base(name="ruddy-turnstone",
             notes="THE ANTI-WADER, and worth having for that: short legs, "
                   "squat body, short blunt slightly upturned bill -- every "
                   "proportion the opposite of the curlew beside it. Its back "
                   "is a broken tortoiseshell of rust, black and white, which "
                   "is the busiest plumage in the set and drawn here as a "
                   "speckle over rufous.",
             **wader(length_m=0.23, bill_frac=0.095, head_frac=0.125,
                     neck_frac=0.045, body_frac=0.505, tail_frac=0.23,
                     posture_deg=14, body_depth=0.72, head_size=1.0,
                     neck_up_deg=22, neck_thick=0.62,
                     bill_depth=0.24, bill_curve=-0.08, bill_gape=0.10,
                     wing_shape="pointed", wing_span=1.80, wing_aspect=7.4,
                     wing_fold=0.80,
                     leg_len=0.085, upperparts=0.56,
                     head_mark="mask", wing_mark="bar", body_mark="speckled",
                     mark_width=0.18, mark_strength=0.34,
                     mat_back="plume_rufous", mat_belly="plume_white",
                     mat_head="plume_white", mat_wing="plume_rufous",
                     mat_mark="skin_dark", mat_head_mark="skin_dark",
                     mat_bill="skin_dark", mat_eye="skin_dark",
                     bio_beach=1.0, bio_ocean=0.2, bio_bare_rock=0.2,
                     place_abundance=0.4, place_spacing_m=12.0,
                     place_water_max_m=15.0,
                     flock_despawn_m=110.0, flock_size_min=3, flock_size_max=20,
                     flock_spread_m=18.0, flock_perch="ground",
                     flock_height_min_m=1.0, flock_height_max_m=20.0,
                     flock_flight_share=0.20, flock_per_hectare=3.0)),
    ),
    "ringed-plover": (
        "0.22 m - round small wader, one black breast band, face mask",
        base(name="ringed-plover",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 19 cm. At 1 cm nineteen "
                   "voxels cannot hold a breast band two voxels wide AND a face "
                   "mask AND a bill; 22 can. Same fix four birds in this "
                   "library already carry, and this note is what stops it being "
                   "'corrected' back.\n\n"
                   "THE BREASTBAND IS THE SPECIES and it is the one body "
                   "marking in the set that is a single band rather than a "
                   "texture. Plovers run and stop rather than probe, so the "
                   "bill is short and the legs are mid.",
             **wader(length_m=0.22, bill_frac=0.075, head_frac=0.145,
                     neck_frac=0.040, body_frac=0.485, tail_frac=0.255,
                     posture_deg=12, body_depth=0.74, head_size=1.15,
                     neck_up_deg=24, neck_thick=0.66,
                     bill_depth=0.24, bill_gape=0.10,
                     wing_shape="pointed", wing_span=1.85, wing_aspect=7.6,
                     wing_fold=0.85,
                     leg_len=0.12, upperparts=0.52,
                     head_mark="mask", wing_mark="bar", body_mark="breastband",
                     mark_width=0.30,
                     mat_back="skin_brown", mat_belly="plume_white",
                     mat_head="plume_white", mat_wing="skin_brown",
                     mat_mark="skin_orange", mat_head_mark="skin_dark",
                     mat_bill="skin_orange", mat_eye="skin_dark",
                     bio_beach=1.0, bio_grassland=0.2,
                     place_abundance=0.45, place_spacing_m=14.0,
                     place_water_max_m=20.0,
                     flock_despawn_m=100.0, flock_size_min=2, flock_size_max=12,
                     flock_spread_m=20.0, flock_perch="ground",
                     flock_height_min_m=1.0, flock_height_max_m=20.0,
                     flock_flight_share=0.22, flock_per_hectare=2.5)),
    ),
    "sanderling": (
        "0.22 m - almost white, running at the very edge of the water",
        base(name="sanderling",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 20 cm, which is the "
                   "library's floor rather than a large enlargement. Recorded "
                   "anyway so the pattern is visible across the set.\n\n"
                   "THE PALEST BIRD HERE AFTER THE WINTER PTARMIGAN, and "
                   "authored as such: nearly white all over with a black bill "
                   "and legs and a dark shoulder, which at twenty-two voxels is "
                   "three dark marks on white and nothing else. It runs at the "
                   "waterline, so `water_max_m` is the tightest of any wader.",
             **wader(length_m=0.22, bill_frac=0.105, head_frac=0.125,
                     neck_frac=0.045, body_frac=0.475, tail_frac=0.25,
                     posture_deg=10, body_depth=0.66, head_size=1.0,
                     neck_up_deg=22, neck_thick=0.58,
                     bill_depth=0.16, bill_gape=0.05,
                     wing_shape="pointed", wing_span=1.90, wing_aspect=8.2,
                     wing_fold=0.90,
                     leg_len=0.115, upperparts=0.40,
                     head_mark="none", wing_mark="bar", body_mark="none",
                     mark_width=0.16,
                     mat_back="plume_grey", mat_belly="plume_white",
                     mat_head="plume_white", mat_wing="plume_grey",
                     mat_mark="skin_dark", mat_head_mark="plume_grey",
                     mat_bill="skin_dark", mat_eye="skin_dark",
                     bio_beach=1.0, bio_tundra_alpine=0.2,
                     place_abundance=0.5, place_spacing_m=8.0,
                     place_water_max_m=8.0,
                     flock_despawn_m=100.0, flock_size_min=5, flock_size_max=40,
                     flock_spread_m=15.0, flock_perch="ground",
                     flock_height_min_m=0.5, flock_height_max_m=15.0,
                     flock_flight_share=0.25, flock_per_hectare=4.0)),
    ),
    "little-egret": (
        "0.60 m - pure white heron shape, black legs, yellow feet",
        base(name="little-egret",
             notes="A HERON'S PROPORTIONS IN ONE COLOUR, which is what makes it "
                   "the useful counterpart to the shipped `grey-heron`: same "
                   "neck share, same dagger, same stance, and the entire bird "
                   "is white with a black bill. Everything that separates the "
                   "two is palette rather than shape, and having both proves "
                   "the plumage half of the generator carries a species on its "
                   "own.\n\n"
                   "The yellow feet are the field mark and they are two voxels; "
                   "bill and legs share one material slot so they cannot differ "
                   "here, and the bill wins because it is bigger.",
             **wader(length_m=0.60, bill_frac=0.145, head_frac=0.075,
                     neck_frac=0.245, body_frac=0.375, tail_frac=0.16,
                     posture_deg=6, body_depth=0.66, body_width=0.62,
                     head_size=0.85, neck_up_deg=60, neck_thick=0.24,
                     bill_depth=0.18, bill_gape=0.03,
                     wing_shape="slotted", wing_span=1.80, wing_aspect=6.8,
                     wing_slots=3, wing_fold=0.85, wing_thick=2, tail_thick=2,
                     leg_len=0.28, leg_thick=1.5, upperparts=0.50, crest=0.30,
                     head_mark="none", wing_mark="none", body_mark="none",
                     mat_back="plume_white", mat_belly="plume_white",
                     mat_head="plume_white", mat_wing="plume_white",
                     mat_mark="plume_grey", mat_head_mark="plume_grey",
                     mat_bill="skin_dark", mat_eye="skin_yellow",
                     bio_beach=0.9, bio_grassland=0.4, bio_savanna=0.35,
                     bio_rainforest=0.25,
                     place_abundance=0.2, place_spacing_m=60.0,
                     place_water_max_m=30.0,
                     flock_despawn_m=200.0, flock_size_min=1, flock_size_max=6,
                     flock_spread_m=30.0, flock_perch="waterside",
                     flock_height_min_m=3.0, flock_height_max_m=60.0,
                     flock_flight_share=0.22, flock_per_hectare=0.5)),
    ),
    "great-cormorant": (
        "0.90 m - long low body, snake neck, hooked bill",
        base(name="great-cormorant",
             notes="THE SNAKE NECK ON A LOW BODY, which is a combination "
                   "nothing else here has: a heron's neck share carried "
                   "HORIZONTALLY at a low angle rather than upright, on a long "
                   "flat body. That one angle -- `neck_up_deg` 18 against a "
                   "heron's 64 -- is most of the difference between the two "
                   "birds in silhouette.\n\n"
                   "Oily blue-black with a pale face patch and a yellow gape. "
                   "The wings-open drying pose it is famous for is a third "
                   "pose the generator does not have; `flying` is the nearest "
                   "and this one is authored perched.",
             **wader(length_m=0.90, bill_frac=0.105, head_frac=0.085,
                     neck_frac=0.185, body_frac=0.385, tail_frac=0.24,
                     posture_deg=10, body_depth=0.58, body_width=0.70,
                     head_size=0.88, neck_up_deg=18, neck_thick=0.40,
                     bill_depth=0.26, bill_hook=0.55, bill_gape=0.10,
                     tail_shape="wedge", tail_width=0.34, tail_droop=0.45,
                     tail_thick=2,
                     wing_shape="soaring", wing_span=1.70, wing_aspect=7.6,
                     wing_fold=0.60, wing_thick=2,
                     leg_len=0.08, upperparts=0.65,
                     head_mark="throat", wing_mark="none", body_mark="none",
                     mark_width=0.24,
                     mat_back="plume_iridescent", mat_belly="skin_dark",
                     mat_head="skin_dark", mat_wing="plume_iridescent",
                     mat_mark="plume_white", mat_head_mark="plume_white",
                     mat_bill="skin_yellow", mat_eye="skin_green",
                     bio_beach=0.8, bio_ocean=0.7, bio_temperate_forest=0.3,
                     bio_bare_rock=0.25,
                     place_abundance=0.2, place_spacing_m=50.0,
                     place_water_max_m=20.0,
                     flock_despawn_m=250.0, flock_size_min=1, flock_size_max=15,
                     flock_spread_m=50.0, flock_perch="waterside",
                     flock_height_min_m=2.0, flock_height_max_m=60.0,
                     flock_flight_share=0.35, flock_per_hectare=0.6)),
    ),
    "brown-pelican": (
        "1.20 m - the pouched bill, nearly as long as the neck",
        base(name="brown-pelican",
             notes="THE BIGGEST BILL IN THE LIBRARY BY EVERY MEASURE: a fifth "
                   "of the total length, at the top of `bill_gape`, which is "
                   "the slider that separates a spatula from a dagger. A "
                   "mallard is 0.95 there and a heron 0.03 on bills of similar "
                   "length, and nobody confuses them; a pelican is that axis at "
                   "its limit on a bird four times the size.\n\n"
                   "Heavy grey-brown body, short neck folded back, and a pale "
                   "head -- so the bill is the only long thing on the animal "
                   "and carries it entirely.",
             **wader(length_m=1.20, bill_frac=0.200, head_frac=0.085,
                     neck_frac=0.115, body_frac=0.415, tail_frac=0.185,
                     posture_deg=8, body_depth=0.68, body_width=0.80,
                     head_size=0.85, neck_up_deg=30, neck_thick=0.55,
                     bill_depth=0.34, bill_hook=0.30, bill_gape=0.90,
                     wing_shape="soaring", wing_span=2.05, wing_aspect=8.6,
                     wing_fold=0.75, wing_thick=3, tail_thick=2,
                     leg_len=0.07, upperparts=0.60,
                     head_mark="cap", wing_mark="none", body_mark="none",
                     mark_width=0.7,
                     mat_back="skin_brown", mat_belly="skin_brown",
                     mat_head="plume_white", mat_wing="plume_slate",
                     mat_mark="plume_buff", mat_head_mark="skin_dark",
                     mat_bill="beak_horn", mat_eye="plume_white",
                     bio_beach=0.9, bio_ocean=0.6,
                     place_abundance=0.12, place_spacing_m=90.0,
                     place_water_max_m=40.0,
                     flock_despawn_m=300.0, flock_size_min=2, flock_size_max=12,
                     flock_spread_m=60.0, flock_perch="waterside",
                     flock_height_min_m=3.0, flock_height_max_m=80.0,
                     flock_flight_share=0.45, flock_per_hectare=0.3)),
    ),
    # --- seabirds -----------------------------------------------------------
    "northern-gannet": (
        "0.90 m - a white cross in the air: cigar body, dagger bill",
        base(name="northern-gannet",
             notes="THE SILHOUETTE IS A CROSS and nothing else in the library "
                   "is: a cigar-shaped body with a dagger at one end and a "
                   "point at the other, and very long narrow wings straight out "
                   "of the middle. Authored FLYING, because a gannet over the "
                   "sea is the shape people know and a folded wing hides the "
                   "whole planform.\n\n"
                   "Brilliant white with black wingtips and a buff head -- the "
                   "same three-part scheme the herring gull uses, at nearly "
                   "twice the aspect ratio.",
             **b(length_m=0.90, bill_frac=0.115, head_frac=0.085,
                 neck_frac=0.095, body_frac=0.475, tail_frac=0.23,
                 posture_deg=2, body_depth=0.52, body_width=0.82,
                 chest_at=0.34, breast=0.70, rump=0.32, fullness=2.6,
                 section=2.1, belly=0.50, head_size=0.85, neck_up_deg=8,
                 neck_thick=0.62,
                 bill_depth=0.28, bill_gape=0.06,
                 tail_shape="pointed", tail_width=0.30, tail_droop=0.45,
                 tail_thick=2,
                 pose="flying", wing_shape="soaring", wing_span=2.05,
                 wing_aspect=12.5, wing_sweep=0.18, wing_dihedral=0.04,
                 wing_thick=2, wing_fold=0.95,
                 leg_len=0.05, eye=1.0, upperparts=0.45,
                 head_mark="cap", wing_mark="tip", body_mark="none",
                 mark_width=0.14,
                 mat_back="plume_white", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_white",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="plume_grey", mat_eye="plume_white",
                 bio_ocean=1.0, bio_beach=0.5, bio_bare_rock=0.3,
                 place_abundance=0.25, place_spacing_m=60.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=400.0, flock_size_min=2, flock_size_max=40,
                 flock_spread_m=150.0, flock_perch="cliff",
                 flock_height_min_m=5.0, flock_height_max_m=120.0,
                 flock_flight_share=0.85, flock_per_hectare=1.0)),
    ),
    "wandering-albatross": (
        "1.20 m - the longest wing in the library, and it is nearly all wing",
        base(name="wandering-albatross",
             notes="THE ASPECT-RATIO CEILING, and the species that proves "
                   "`soaring` is not one wing. Hand-wing index separates ocean "
                   "dynamic soarers from land thermal soarers by a factor of "
                   "two: an albatross gets its lift from LENGTH and a vulture "
                   "from AREA, so this bird carries the highest span and the "
                   "highest aspect ratio in the set and a golden eagle of "
                   "similar mass carries neither.\n\n"
                   "Authored FLYING. Perched it is a white lump; in the air it "
                   "is a 3 m plank with a body in the middle, and that is the "
                   "animal.",
             **b(length_m=1.20, bill_frac=0.130, head_frac=0.090,
                 neck_frac=0.090, body_frac=0.480, tail_frac=0.21,
                 posture_deg=2, body_depth=0.60, body_width=0.82,
                 chest_at=0.32, breast=0.76, rump=0.36, fullness=2.8,
                 section=2.2, belly=0.50, head_size=0.90, neck_up_deg=10,
                 neck_thick=0.62,
                 bill_depth=0.34, bill_hook=0.42, bill_gape=0.14,
                 tail_shape="wedge", tail_width=0.40, tail_droop=0.40,
                 tail_thick=2,
                 pose="flying", wing_shape="soaring", wing_span=2.60,
                 wing_aspect=15.5, wing_sweep=0.12, wing_dihedral=0.02,
                 wing_thick=3, wing_fold=1.05,
                 leg_len=0.05, eye=1.0, upperparts=0.42,
                 head_mark="none", wing_mark="tip", body_mark="none",
                 mark_width=0.30,
                 mat_back="plume_white", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_white",
                 mat_mark="skin_dark", mat_head_mark="plume_grey",
                 mat_bill="beak_horn", mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.05, place_spacing_m=800.0,
                 flock_despawn_m=700.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=200.0, flock_perch="water",
                 flock_height_min_m=2.0, flock_height_max_m=60.0,
                 flock_flight_share=0.95, flock_per_hectare=0.02)),
    ),
    "black-browed-albatross": (
        "0.90 m - half the wanderer, dark brow smudge, orange bill",
        base(name="black-browed-albatross",
             notes="Three quarters of the wandering albatross's length and "
                   "considerably less wing, which is the point of having both: "
                   "aspect ratio 13.0 against 15.5 reads as a genuinely "
                   "different bird in the air rather than as a smaller copy. "
                   "Dark upperwing against a white body, a dark brow over the "
                   "eye, and a bright orange bill that carries at distance.",
             **b(length_m=0.90, bill_frac=0.125, head_frac=0.090,
                 neck_frac=0.085, body_frac=0.480, tail_frac=0.22,
                 posture_deg=2, body_depth=0.60, body_width=0.80,
                 chest_at=0.32, breast=0.76, rump=0.36, fullness=2.8,
                 section=2.2, belly=0.50, head_size=0.90, neck_up_deg=10,
                 neck_thick=0.60,
                 bill_depth=0.32, bill_hook=0.40, bill_gape=0.13,
                 tail_shape="wedge", tail_width=0.40, tail_droop=0.40,
                 tail_thick=2,
                 pose="flying", wing_shape="soaring", wing_span=2.55,
                 wing_aspect=13.0, wing_sweep=0.14, wing_dihedral=0.03,
                 wing_thick=2, wing_fold=1.05,
                 leg_len=0.05, eye=1.0, upperparts=0.44,
                 head_mark="mask", wing_mark="none", body_mark="none",
                 mark_width=0.45,
                 mat_back="skin_dark", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="skin_dark",
                 mat_bill="skin_orange", mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.08, place_spacing_m=500.0,
                 flock_despawn_m=600.0, flock_size_min=1, flock_size_max=4,
                 flock_spread_m=150.0, flock_perch="water",
                 flock_height_min_m=2.0, flock_height_max_m=50.0,
                 flock_flight_share=0.92, flock_per_hectare=0.05)),
    ),
    "northern-fulmar": (
        "0.47 m - gull-shaped but bull-necked and stiff-winged",
        base(name="northern-fulmar",
             notes="A GULL THAT IS NOT A GULL, and the difference is entirely "
                   "PROPORTION AND POSTURE: a thick neck, a heavy blunt head "
                   "and wings held dead straight rather than bent, on a bird "
                   "the same colour as a kittiwake. Both `neck_thick` at 0.80 "
                   "and `wing_dihedral` at 0.0 are doing that, and neither is a "
                   "colour.\n\n"
                   "THE TUBE NOSTRILS ARE THE FAMILY MARK AND ARE NOT DRAWN. "
                   "They are about a centimetre on the bill, which is one voxel "
                   "at 1 cm; the bare-rock file recommends not attempting them "
                   "and this spec agrees. The stiff-winged flight posture "
                   "identifies the bird at any distance the player will see it, "
                   "and posture is free.",
             **b(length_m=0.47, bill_frac=0.090, head_frac=0.115,
                 neck_frac=0.055, body_frac=0.470, tail_frac=0.27,
                 posture_deg=4, body_depth=0.66, body_width=0.84,
                 chest_at=0.32, breast=0.78, rump=0.44, fullness=3.2,
                 section=2.3, belly=0.52, head_size=1.05, neck_up_deg=12,
                 neck_thick=0.80,
                 bill_depth=0.36, bill_hook=0.30, bill_gape=0.18,
                 tail_shape="rounded", tail_width=0.56, tail_droop=0.40,
                 tail_thick=2,
                 pose="flying", wing_shape="soaring", wing_span=2.25,
                 wing_aspect=10.5, wing_sweep=0.16, wing_dihedral=0.0,
                 wing_thick=2, wing_fold=0.95,
                 leg_len=0.05, eye=1.0, upperparts=0.44,
                 head_mark="none", wing_mark="tip", body_mark="none",
                 mark_width=0.12,
                 mat_back="plume_grey", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_grey",
                 mat_mark="plume_slate", mat_head_mark="plume_grey",
                 mat_bill="beak_horn", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_bare_rock=0.6, bio_beach=0.35,
                 place_abundance=0.3, place_spacing_m=30.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=300.0, flock_size_min=2, flock_size_max=30,
                 flock_spread_m=90.0, flock_perch="cliff",
                 flock_height_min_m=3.0, flock_height_max_m=120.0,
                 flock_flight_share=0.80, flock_per_hectare=2.0)),
    ),
    "manx-shearwater": (
        "0.35 m - black above, white below, split down the exact midline",
        base(name="manx-shearwater",
             notes="THE HARDEST COUNTERSHADE BOUNDARY IN THE LIBRARY. Its "
                   "whole identity is that the black and the white meet at the "
                   "midline with nothing in between, which `bird.upperparts` at "
                   "exactly 0.50 says and no marking could -- a shearwater "
                   "banking shows one colour then the other and nothing "
                   "gradual. Cuthill's 2016 finding that a SHARP countershading "
                   "transition is optimal under direct sun is this bird "
                   "literally.\n\n"
                   "Narrow stiff wings held straight out, authored flying.",
             **b(length_m=0.35, bill_frac=0.095, head_frac=0.105,
                 neck_frac=0.050, body_frac=0.490, tail_frac=0.26,
                 posture_deg=2, body_depth=0.58, body_width=0.76,
                 chest_at=0.32, breast=0.74, rump=0.38, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.95, neck_up_deg=10,
                 neck_thick=0.66,
                 bill_depth=0.22, bill_hook=0.35, bill_gape=0.07,
                 tail_shape="wedge", tail_width=0.34, tail_droop=0.42,
                 pose="flying", wing_shape="soaring", wing_span=2.45,
                 wing_aspect=12.0, wing_sweep=0.14, wing_dihedral=0.0,
                 wing_fold=1.00,
                 leg_len=0.04, eye=1.0, upperparts=0.50,
                 head_mark="none", wing_mark="none", body_mark="none",
                 mat_back="skin_dark", mat_belly="plume_white",
                 mat_head="skin_dark", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_bare_rock=0.3,
                 place_abundance=0.3, place_spacing_m=40.0,
                 flock_despawn_m=280.0, flock_size_min=5, flock_size_max=80,
                 flock_spread_m=120.0, flock_perch="water",
                 flock_height_min_m=0.5, flock_height_max_m=30.0,
                 flock_flight_share=0.90, flock_per_hectare=3.0)),
    ),
    "atlantic-puffin": (
        "0.30 m - the bill is the entire species: deep, triangular, banded",
        base(name="atlantic-puffin",
             notes="THE BILL-DEPTH EXTREME at 1.15, which is deeper than it is "
                   "long and the only bird in the library over 1.0 there. A "
                   "macaw is 0.85 and every other seabird is under 0.4. That "
                   "one number IS the puffin; the squat black-and-white body "
                   "under it is a guillemot.\n\n"
                   "The bill's orange, blue-grey and yellow bands are three "
                   "colours on six voxels and cannot all exist -- it is drawn "
                   "orange, which is the one that reads, with the face white "
                   "behind it for contrast.",
             **b(length_m=0.30, bill_frac=0.110, head_frac=0.160,
                 neck_frac=0.030, body_frac=0.545, tail_frac=0.155,
                 posture_deg=30, body_depth=0.80, body_width=0.78,
                 chest_at=0.32, breast=0.80, rump=0.46, fullness=3.4,
                 section=2.3, belly=0.52, head_size=1.20, neck_up_deg=26,
                 neck_thick=0.85,
                 bill_depth=1.15, bill_gape=0.08,
                 tail_shape="square", tail_width=0.60, tail_droop=0.35,
                 pose="perched", wing_shape="pointed", wing_span=1.60,
                 wing_aspect=8.8, wing_sweep=0.30, wing_fold=0.55,
                 leg_len=0.07, eye=1.0, upperparts=0.58,
                 head_mark="cap", wing_mark="none", body_mark="none",
                 mark_width=0.6,
                 mat_back="skin_dark", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="skin_dark",
                 mat_bill="skin_orange", mat_eye="skin_dark",
                 bio_ocean=0.9, bio_bare_rock=0.7, bio_beach=0.3,
                 place_abundance=0.4, place_spacing_m=10.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=160.0, flock_size_min=3, flock_size_max=40,
                 flock_spread_m=30.0, flock_perch="cliff",
                 flock_height_min_m=2.0, flock_height_max_m=50.0,
                 flock_flight_share=0.35, flock_per_hectare=6.0)),
    ),
    "common-guillemot": (
        "0.42 m - upright cigar on a bare ledge, thin pointed bill",
        base(name="common-guillemot",
             notes="THE MOST UPRIGHT PERCHING BIRD IN THE LIBRARY at 62 "
                   "degrees, beaten only by the woodpecker clinging to a trunk "
                   "-- and where a woodpecker is hanging on, a guillemot is "
                   "STANDING. A cigar body on a bare ledge with the tail almost "
                   "gone, which is a tail share of 0.11, the lowest here.\n\n"
                   "Dark chocolate above, white below, with a thin dagger. "
                   "Almost no colour at all, so the shape has to do it, which "
                   "is what makes it a useful test.",
             **b(length_m=0.42, bill_frac=0.115, head_frac=0.120,
                 neck_frac=0.055, body_frac=0.600, tail_frac=0.110,
                 posture_deg=62, body_depth=0.66, body_width=0.72,
                 chest_at=0.30, breast=0.76, rump=0.44, fullness=3.0,
                 section=2.2, belly=0.50, head_size=1.0, neck_up_deg=30,
                 neck_thick=0.72,
                 bill_depth=0.20, bill_gape=0.05,
                 tail_shape="pointed", tail_width=0.50, tail_droop=0.55,
                 pose="perched", wing_shape="pointed", wing_span=1.45,
                 wing_aspect=9.4, wing_sweep=0.28, wing_fold=0.65,
                 leg_len=0.07, eye=1.0, upperparts=0.55,
                 head_mark="none", wing_mark="none", body_mark="none",
                 mat_back="skin_brown", mat_belly="plume_white",
                 mat_head="skin_brown", mat_wing="skin_brown",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="plume_white",
                 bio_ocean=0.9, bio_bare_rock=0.8,
                 place_abundance=0.5, place_spacing_m=6.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=180.0, flock_size_min=6, flock_size_max=120,
                 flock_spread_m=25.0, flock_perch="cliff",
                 flock_height_min_m=2.0, flock_height_max_m=60.0,
                 flock_flight_share=0.30, flock_per_hectare=10.0)),
    ),
    "razorbill": (
        "0.40 m - the guillemot in black, with a deep blunt white-lined bill",
        base(name="razorbill",
             notes="AUTHORED AS THE GUILLEMOT'S PAIR, deliberately: same "
                   "stance, same ledge, same body, and the only differences are "
                   "a jet-black back instead of chocolate and a bill twice as "
                   "deep and blunt-ended. Two species that must be told apart "
                   "by ONE proportion and one colour is a much harder test than "
                   "two that look nothing alike, and it is the test a real "
                   "seabird colony sets.",
             **b(length_m=0.40, bill_frac=0.100, head_frac=0.125,
                 neck_frac=0.055, body_frac=0.595, tail_frac=0.125,
                 posture_deg=58, body_depth=0.70, body_width=0.72,
                 chest_at=0.30, breast=0.78, rump=0.44, fullness=3.1,
                 section=2.2, belly=0.50, head_size=1.05, neck_up_deg=28,
                 neck_thick=0.74,
                 bill_depth=0.60, bill_gape=0.08,
                 tail_shape="pointed", tail_width=0.48, tail_droop=0.55,
                 pose="perched", wing_shape="pointed", wing_span=1.45,
                 wing_aspect=9.0, wing_sweep=0.28, wing_fold=0.65,
                 leg_len=0.07, eye=1.0, upperparts=0.58,
                 head_mark="none", wing_mark="none", body_mark="none",
                 mat_back="skin_dark", mat_belly="plume_white",
                 mat_head="skin_dark", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="plume_white", mat_eye="plume_white",
                 bio_ocean=0.9, bio_bare_rock=0.8,
                 place_abundance=0.35, place_spacing_m=8.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=180.0, flock_size_min=4, flock_size_max=60,
                 flock_spread_m=25.0, flock_perch="cliff",
                 flock_height_min_m=2.0, flock_height_max_m=60.0,
                 flock_flight_share=0.30, flock_per_hectare=6.0)),
    ),
    "black-legged-kittiwake": (
        "0.40 m - the clean gull: wingtips dipped in solid black",
        base(name="black-legged-kittiwake",
             notes="THE TIP MARK WITH NO WHITE IN IT. A herring gull's black "
                   "wingtips carry white spots and a kittiwake's do not -- they "
                   "look cut off -- and at 1 cm that difference is expressible: "
                   "`mark_width` narrow with the marking colour flat, against "
                   "the herring gull's wider softer tip. It is the smallest "
                   "deliberate difference between any two species in this "
                   "library and it is the real field mark.",
             **b(length_m=0.40, bill_frac=0.070, head_frac=0.110,
                 neck_frac=0.060, body_frac=0.435, tail_frac=0.325,
                 posture_deg=4, body_depth=0.60, body_width=0.78,
                 chest_at=0.32, breast=0.76, rump=0.40, fullness=3.1,
                 section=2.2, belly=0.50, head_size=0.95, neck_up_deg=12,
                 neck_thick=0.60,
                 bill_depth=0.26, bill_hook=0.15, bill_gape=0.12,
                 tail_shape="square", tail_width=0.58, tail_droop=0.40,
                 tail_thick=2,
                 pose="flying", wing_shape="soaring", wing_span=2.45,
                 wing_aspect=10.5, wing_sweep=0.20, wing_dihedral=0.05,
                 wing_thick=2, wing_fold=0.95,
                 leg_len=0.09, eye=1.0, upperparts=0.42,
                 head_mark="none", wing_mark="tip", body_mark="none",
                 mark_width=0.10,
                 mat_back="plume_grey", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_grey",
                 mat_mark="skin_dark", mat_head_mark="plume_grey",
                 mat_bill="beak_horn", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_bare_rock=0.6, bio_beach=0.4,
                 place_abundance=0.4, place_spacing_m=12.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=280.0, flock_size_min=4, flock_size_max=80,
                 flock_spread_m=80.0, flock_perch="cliff",
                 flock_height_min_m=3.0, flock_height_max_m=100.0,
                 flock_flight_share=0.70, flock_per_hectare=5.0)),
    ),
    "black-headed-gull": (
        "0.38 m - small gull with a chocolate hood and a white leading edge",
        base(name="black-headed-gull",
             notes="THE HOOD IS NOT BLACK, which is the joke the name makes and "
                   "the thing to draw: it is a dark chocolate brown, and drawn "
                   "in the library's black it would be a different bird. "
                   "`skin_brown` on the head against a white body, and the red "
                   "bill and legs are the second mark.",
             **b(length_m=0.38, bill_frac=0.075, head_frac=0.115,
                 neck_frac=0.055, body_frac=0.435, tail_frac=0.320,
                 posture_deg=8, body_depth=0.62, body_width=0.78,
                 chest_at=0.32, breast=0.76, rump=0.42, fullness=3.1,
                 section=2.2, belly=0.50, head_size=0.98, neck_up_deg=18,
                 neck_thick=0.60,
                 bill_depth=0.24, bill_hook=0.12, bill_gape=0.10,
                 tail_shape="square", tail_width=0.58, tail_droop=0.40,
                 pose="perched", wing_shape="soaring", wing_span=2.35,
                 wing_aspect=9.4, wing_sweep=0.22, wing_fold=0.95,
                 leg_len=0.13, eye=1.0, upperparts=0.42,
                 head_mark="cap", wing_mark="tip", body_mark="none",
                 mark_width=0.40,
                 mat_back="plume_grey", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_grey",
                 mat_mark="skin_dark", mat_head_mark="skin_brown",
                 mat_bill="skin_red", mat_eye="skin_dark",
                 bio_beach=0.9, bio_grassland=0.6, bio_ocean=0.5,
                 bio_temperate_forest=0.25,
                 place_abundance=0.6, place_spacing_m=8.0,
                 flock_despawn_m=200.0, flock_size_min=4, flock_size_max=100,
                 flock_spread_m=60.0, flock_perch="ground",
                 flock_height_min_m=2.0, flock_height_max_m=80.0,
                 flock_flight_share=0.55, flock_per_hectare=8.0)),
    ),
    "common-tern": (
        "0.34 m - slight body, long forked tail, black cap, red-black bill",
        base(name="common-tern",
             notes="A GULL BUILT FOR SPEED: half the body depth, twice the "
                   "tail, a pointed planform instead of a soaring one, and a "
                   "deeply forked tail. It is what the fork slider does on a "
                   "seabird rather than on a swallow, and it is the reason a "
                   "tern and a gull are never confused at any distance.",
             **b(length_m=0.34, bill_frac=0.100, head_frac=0.105,
                 neck_frac=0.040, body_frac=0.365, tail_frac=0.390,
                 posture_deg=6, body_depth=0.52, body_width=0.72,
                 chest_at=0.30, breast=0.72, rump=0.32, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.95, neck_up_deg=14,
                 neck_thick=0.58,
                 bill_depth=0.16, bill_gape=0.05,
                 tail_shape="forked", tail_width=0.44, tail_fork=0.55,
                 tail_droop=0.40,
                 pose="flying", wing_shape="pointed", wing_span=2.35,
                 wing_aspect=11.0, wing_sweep=0.34, wing_dihedral=0.04,
                 wing_fold=1.05,
                 leg_len=0.06, eye=1.0, upperparts=0.40,
                 head_mark="cap", wing_mark="tip", body_mark="none",
                 mark_width=0.34,
                 mat_back="plume_grey", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_grey",
                 mat_mark="plume_slate", mat_head_mark="skin_dark",
                 mat_bill="skin_orange", mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.7, bio_grassland=0.2,
                 place_abundance=0.4, place_spacing_m=12.0,
                 place_water_max_m=60.0,
                 flock_despawn_m=220.0, flock_size_min=3, flock_size_max=40,
                 flock_spread_m=50.0, flock_perch="ground",
                 flock_height_min_m=2.0, flock_height_max_m=40.0,
                 flock_flight_share=0.75, flock_per_hectare=4.0)),
    ),
    "arctic-tern": (
        "0.35 m - slighter still, longer streamers, blood-red bill",
        base(name="arctic-tern",
             notes="THE COMMON TERN TAKEN FURTHER IN EVERY DIRECTION: a "
                   "shallower body, a deeper fork, longer streamers, and a bill "
                   "that is red to the tip where the common tern's is black-"
                   "tipped. That last one is a single voxel and it is the real "
                   "field mark; here it is expressed as bill colour alone, "
                   "which is honest -- a black tip on a six-voxel bill would be "
                   "one voxel of noise.",
             **b(length_m=0.35, bill_frac=0.095, head_frac=0.100,
                 neck_frac=0.035, body_frac=0.335, tail_frac=0.435,
                 posture_deg=6, body_depth=0.48, body_width=0.70,
                 chest_at=0.30, breast=0.70, rump=0.30, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.92, neck_up_deg=14,
                 neck_thick=0.56,
                 bill_depth=0.14, bill_gape=0.04,
                 tail_shape="forked", tail_width=0.36, tail_fork=0.68,
                 tail_droop=0.38,
                 pose="flying", wing_shape="pointed", wing_span=2.45,
                 wing_aspect=12.0, wing_sweep=0.36, wing_dihedral=0.04,
                 wing_fold=1.10,
                 leg_len=0.045, eye=1.0, upperparts=0.38,
                 head_mark="cap", wing_mark="tip", body_mark="none",
                 mark_width=0.30,
                 mat_back="plume_grey", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_grey",
                 mat_mark="plume_slate", mat_head_mark="skin_dark",
                 mat_bill="plume_crimson", mat_eye="skin_dark",
                 bio_ocean=0.9, bio_beach=0.7, bio_tundra_alpine=0.4,
                 place_abundance=0.35, place_spacing_m=14.0,
                 flock_despawn_m=220.0, flock_size_min=3, flock_size_max=50,
                 flock_spread_m=60.0, flock_perch="ground",
                 flock_height_min_m=2.0, flock_height_max_m=50.0,
                 flock_flight_share=0.80, flock_per_hectare=3.0)),
    ),
    "sooty-tern": (
        "0.44 m - all dark above, white below, white forehead triangle",
        base(name="sooty-tern",
             notes="The tropical tern, and the one that is DARK: black-brown "
                   "upperparts against a white underside with a hard white "
                   "wedge on the forehead. Between it and the two grey terns "
                   "the set covers both ends of what a tern looks like, and the "
                   "forehead wedge is the only supercilium in the library used "
                   "as a block rather than a stripe.",
             **b(length_m=0.44, bill_frac=0.100, head_frac=0.105,
                 neck_frac=0.040, body_frac=0.375, tail_frac=0.380,
                 posture_deg=6, body_depth=0.54, body_width=0.74,
                 chest_at=0.30, breast=0.72, rump=0.34, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.95, neck_up_deg=14,
                 neck_thick=0.60,
                 bill_depth=0.18, bill_gape=0.05,
                 tail_shape="forked", tail_width=0.44, tail_fork=0.60,
                 tail_droop=0.40,
                 pose="flying", wing_shape="pointed", wing_span=2.30,
                 wing_aspect=11.0, wing_sweep=0.34, wing_dihedral=0.04,
                 wing_fold=1.05,
                 leg_len=0.05, eye=1.0, upperparts=0.52,
                 head_mark="supercilium", wing_mark="none", body_mark="none",
                 mark_width=0.62,
                 mat_back="skin_dark", mat_belly="plume_white",
                 mat_head="skin_dark", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.5,
                 place_abundance=0.3, place_spacing_m=15.0,
                 flock_despawn_m=250.0, flock_size_min=6, flock_size_max=150,
                 flock_spread_m=90.0, flock_perch="ground",
                 flock_height_min_m=3.0, flock_height_max_m=80.0,
                 flock_flight_share=0.85, flock_per_hectare=5.0)),
    ),
    "great-skua": (
        "0.55 m - bulky brown pirate gull with a white primary flash",
        base(name="great-skua",
             notes="THE HEAVIEST-BUILT SEABIRD HERE: a gull's plan on a "
                   "buzzard's body, with a heavy hooked bill and a broad blunt "
                   "wing at aspect 7.5, against the herring gull's 9.7 and the "
                   "gannet's 12.5. That progression across four ocean species "
                   "on one slider is what stops a sea full of birds reading as "
                   "one bird repeated.\n\n"
                   "Uniform dark brown with a hard white flash at the base of "
                   "the primaries -- one panel on an otherwise plain animal.",
             **b(length_m=0.55, bill_frac=0.080, head_frac=0.115,
                 neck_frac=0.050, body_frac=0.480, tail_frac=0.275,
                 posture_deg=6, body_depth=0.72, body_width=0.82,
                 chest_at=0.32, breast=0.78, rump=0.46, fullness=3.2,
                 section=2.3, belly=0.52, head_size=1.0, neck_up_deg=14,
                 neck_thick=0.70,
                 bill_depth=0.42, bill_hook=0.55, bill_gape=0.16,
                 tail_shape="rounded", tail_width=0.56, tail_droop=0.42,
                 tail_thick=2,
                 pose="flying", wing_shape="pointed", wing_span=2.35,
                 wing_aspect=7.5, wing_sweep=0.24, wing_dihedral=0.06,
                 wing_thick=2, wing_fold=0.85,
                 leg_len=0.09, eye=1.0, upperparts=0.60,
                 head_mark="none", wing_mark="panel", body_mark="streaked",
                 mark_count=6, mark_width=0.20,
                 mat_back="skin_brown", mat_belly="skin_brown",
                 mat_head="skin_brown", mat_wing="skin_brown",
                 mat_mark="plume_white", mat_head_mark="plume_buff",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_bare_rock=0.5, bio_tundra_alpine=0.3,
                 place_abundance=0.15, place_spacing_m=120.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=350.0, flock_size_min=1, flock_size_max=4,
                 flock_spread_m=80.0, flock_perch="ground",
                 flock_height_min_m=3.0, flock_height_max_m=120.0,
                 flock_flight_share=0.65, flock_per_hectare=0.3)),
    ),
    "magnificent-frigatebird": (
        "1.00 m - all black, scissor tail, wings longer than anything but an albatross",
        base(name="magnificent-frigatebird",
             notes="THE DEEPEST FORK IN THE LIBRARY at 0.82, on a tail share "
                   "over 0.40 -- a pair of scissors trailing behind a black "
                   "cross. Nothing else here is shaped like it, and the whole "
                   "read is silhouette: one colour, no markings that matter, "
                   "and extreme proportions.\n\n"
                   "The male's red throat pouch is real dimorphism and it is "
                   "authored: `sex_plumage` says the spec is the MALE, and the "
                   "female swaps the throat marking off and the head to white. "
                   "That declaration is the point -- `unsexed` on this species "
                   "is a male, and saying so in a field the probe can read is "
                   "the difference between a known limitation and a silent one.",
             **b(length_m=1.00, bill_frac=0.105, head_frac=0.085,
                 neck_frac=0.055, body_frac=0.345, tail_frac=0.410,
                 posture_deg=4, body_depth=0.54, body_width=0.70,
                 chest_at=0.32, breast=0.74, rump=0.30, fullness=2.8,
                 section=2.1, belly=0.50, head_size=0.90, neck_up_deg=12,
                 neck_thick=0.58,
                 bill_depth=0.22, bill_hook=0.70, bill_gape=0.06,
                 tail_shape="forked", tail_width=0.30, tail_fork=0.82,
                 tail_droop=0.45, tail_thick=2,
                 pose="flying", wing_shape="pointed", wing_span=2.40,
                 wing_aspect=12.5, wing_sweep=0.40, wing_dihedral=-0.10,
                 wing_thick=2, wing_fold=1.10,
                 leg_len=0.03, eye=1.0, upperparts=0.72,
                 head_mark="throat", wing_mark="none", body_mark="none",
                 mark_width=0.35,
                 sex_plumage="male", sex_alt_head_mark="none",
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="skin_dark", mat_wing="skin_dark",
                 mat_mark="plume_crimson", mat_head_mark="plume_crimson",
                 mat_bill="plume_grey", mat_eye="skin_dark",
                 mat_alt_head="plume_white", mat_alt_belly="plume_white",
                 bio_ocean=1.0, bio_beach=0.4,
                 place_abundance=0.1, place_spacing_m=200.0,
                 flock_despawn_m=500.0, flock_size_min=1, flock_size_max=8,
                 flock_spread_m=150.0, flock_perch="canopy",
                 flock_height_min_m=10.0, flock_height_max_m=300.0,
                 flock_flight_share=0.92, flock_per_hectare=0.1)),
    ),
    "brown-booby": (
        "0.75 m - chocolate above with a hard-edged white belly",
        base(name="brown-booby",
             notes="THE HARD EDGE. Like the shearwater this is a bird whose "
                   "identity is a countershading BOUNDARY rather than a "
                   "marking, but the boundary sits high on the flank rather "
                   "than at the midline -- `upperparts` 0.62 against the "
                   "shearwater's 0.50 -- so the white is a belly patch with a "
                   "straight top edge. Same mechanism, different number, "
                   "different bird.",
             **b(length_m=0.75, bill_frac=0.120, head_frac=0.090,
                 neck_frac=0.075, body_frac=0.455, tail_frac=0.260,
                 posture_deg=6, body_depth=0.56, body_width=0.78,
                 chest_at=0.32, breast=0.72, rump=0.34, fullness=2.8,
                 section=2.1, belly=0.50, head_size=0.88, neck_up_deg=12,
                 neck_thick=0.62,
                 bill_depth=0.30, bill_gape=0.08,
                 tail_shape="wedge", tail_width=0.32, tail_droop=0.45,
                 tail_thick=2,
                 pose="flying", wing_shape="pointed", wing_span=2.05,
                 wing_aspect=10.5, wing_sweep=0.24, wing_dihedral=0.04,
                 wing_thick=2, wing_fold=0.95,
                 leg_len=0.05, eye=1.0, upperparts=0.62,
                 head_mark="none", wing_mark="none", body_mark="none",
                 mat_back="skin_brown", mat_belly="plume_white",
                 mat_head="skin_brown", mat_wing="skin_brown",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="plume_white",
                 bio_ocean=1.0, bio_beach=0.5,
                 place_abundance=0.15, place_spacing_m=90.0,
                 flock_despawn_m=350.0, flock_size_min=1, flock_size_max=10,
                 flock_spread_m=80.0, flock_perch="cliff",
                 flock_height_min_m=3.0, flock_height_max_m=100.0,
                 flock_flight_share=0.70, flock_per_hectare=0.5)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "shore and seabird specs")
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
