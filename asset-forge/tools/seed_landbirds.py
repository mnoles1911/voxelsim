"""Author the land birds: open country, forest, desert, savanna, taiga,
tundra/alpine, bare rock and rainforest. Fifty-eight species.

WHY THIS SET. Birds are the cheapest identity in the library. All fifty-eight
together cost a fraction of one `temperate-oak`, the generator is shipped and
mature, and they are the only animate thing the world can carry at all until a
quadruped generator exists. `docs/biomes/README.md` §7 puts open-country and
shore birds fifth in the whole build queue and puts the EMU sixth ON ITS OWN and
out of order, with the reason stated plainly: it is a large, unmistakable,
animal-shaped entity that needs no new generator, and it is the cheapest
possible partial answer to "the world has no animals". The greater rhea and the
common ostrich are the same trick twice more.

THREE THINGS THIS SET ADDS THAT THE FIRST TWENTY DID NOT.

  * THE RATITES. Emu, rhea and ostrich, and they are the first birds in the
    library big enough to move off the 1 cm lattice -- see below. They are also
    the first birds authored with a leg share over 0.30, which is what makes
    an animal that stands rather than perches.
  * THE TRUNK-CLINGERS. Nuthatch, treecreeper, green and black woodpecker. The
    generator already poses `great-spotted-woodpecker` at 68 degrees up a
    trunk, so the pose exists; these put four more birds on tree trunks instead
    of in the air, and the nuthatch is authored NOSE-DOWN at a negative neck
    angle, which is the one thing it does that no other bird does.
  * THE CLIFF SPECIALISTS. Peregrine, griffon vulture, chough, wallcreeper,
    crag martin, alpine swift. Bare rock hosts exactly two kinds -- rock and
    bird -- so a bird is half of everything that biome can contain, and it had
    seven species, all borrowed from somewhere else.

LATTICE. Forty-five at 1 cm, which is shipped practice. THREE AT 2 cm, and the
arithmetic is the house rule rather than a preference: a 1.5 m emu at 1 cm is
150 voxels long, and the coarsest lattice at which its smallest identifying
feature -- the bare neck, roughly a hand's width -- is still about three voxels
across is 2 cm, where the bird is 75 voxels and the neck is five. The savanna
file recommends 5 cm for the ostrich; that is rejected here, because at 5 cm a
10 cm neck is two voxels and two voxels reads as a mistake rather than a
feature. `tools/birdprobe.py --lattice` is the check.

THE SIZE FLOOR. Nothing here is under 0.20 m and eleven species are genuinely
smaller in life -- skylark, goldfinch, nuthatch, treecreeper, chaffinch,
bullfinch, blue tit, long-tailed tit, wren, trumpeter finch, snow bunting,
horned lark, wallcreeper, crag martin. Each says so in its own `notes`. A
perched songbird is authored at 36-42 degrees nose-up and 20 cm of bird at that
angle projects onto sixteen voxels of LENGTH, which is why the four small birds
already in the library sit at 20-26 cm against real lengths of 14-19.

ONE SPECIES IS DELIBERATELY NOT HERE. The goldcrest at 9 cm would have to be
authored at more than twice life size, at which point it is not a goldcrest
sitting next to a robin, it is a robin. The temperate-forest file says to
either accept that and say so or leave it out; this leaves it out, and records
the decision here so it is a decision and not an oversight.

    python tools/seed_landbirds.py
    python tools/seed_landbirds.py --force

SIZES ARE APPROXIMATE. Every length is the approximate figure from the biome
file it came from; those are unsourced general-knowledge estimates by their own
admission. Nothing here is quoted as measured.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(res="1", **over):
    changes = {
        "kind": "bird",
        "resolution_cm": res,
        "variation.amount": 1.0,
        "variation.height": 0.14,
        "variation.shape": 0.16,
        "variation.proportion": 0.18,
        "flock.entity_class": "detail",
    }
    changes.update(over)
    return changes


def b(**kw):
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


def song(**over):
    """A small perched songbird: sat up nose-high, round body, short tail."""
    d = dict(posture_deg=38, body_depth=0.88, body_width=0.66, chest_at=0.32,
             breast=0.74, rump=0.46, fullness=3.5, section=2.2, belly=0.53,
             neck_up_deg=38, neck_thick=0.52, head_size=1.05, leg_len=0.12,
             pose="perched", wing_shape="elliptical", wing_span=1.55,
             wing_aspect=5.4, wing_fold=0.34, eye=1.0)
    d.update(over)
    return b(**d)


SPECIES = {
    # --- open country: grassland is 28.06% of the world's land --------------
    "northern-lapwing": (
        "0.30 m - broad rounded wings, dark glossy back, long upswept crest",
        base(name="northern-lapwing",
             notes="THE THINNEST CREST IN THE SET at 0.85, and a genuinely "
                   "different shape from the hoopoe's fan: a lapwing's is a "
                   "single wispy quill swept back off the crown. Its wings are "
                   "the roundest of any wader -- aspect 5.2 against an "
                   "oystercatcher's 8.4 -- which is what makes it flap like a "
                   "butterfly and read as nothing else in the air.",
             **b(length_m=0.30, bill_frac=0.075, head_frac=0.125,
                 neck_frac=0.060, body_frac=0.470, tail_frac=0.270,
                 posture_deg=12, body_depth=0.70, body_width=0.76,
                 chest_at=0.32, breast=0.74, rump=0.44, fullness=3.1,
                 section=2.2, belly=0.52, head_size=1.0, neck_up_deg=26,
                 neck_thick=0.56, crest=0.85,
                 bill_depth=0.22, bill_gape=0.08,
                 tail_shape="square", tail_width=0.60, tail_droop=0.38,
                 pose="perched", wing_shape="elliptical", wing_span=1.95,
                 wing_aspect=5.2, wing_fold=0.60, wing_thick=2,
                 leg_len=0.14, eye=1.0, upperparts=0.56,
                 head_mark="cap", wing_mark="tip", body_mark="breastband",
                 mark_width=0.28,
                 mat_back="plume_iridescent", mat_belly="plume_white",
                 mat_head="skin_dark", mat_wing="plume_iridescent",
                 mat_mark="skin_orange", mat_head_mark="plume_white",
                 mat_bill="plume_grey", mat_eye="plume_white",
                 bio_grassland=1.0, bio_beach=0.4, bio_taiga=0.2,
                 place_abundance=0.4, place_spacing_m=25.0,
                 flock_despawn_m=180.0, flock_size_min=3, flock_size_max=60,
                 flock_spread_m=60.0, flock_perch="ground",
                 flock_height_min_m=2.0, flock_height_max_m=60.0,
                 flock_flight_share=0.30, flock_per_hectare=2.5)),
    ),
    "grey-partridge": (
        "0.30 m - round, short-tailed, ground-hugging, orange face",
        base(name="grey-partridge",
             notes="A GAMEBIRD BODY, which in this generator is depth 0.92 on a "
                   "tail share of 0.20 and the second-lowest aspect ratio here "
                   "at 4.8 -- round, heavy, and built to burst upward rather "
                   "than to travel. It is the counterpart to `rock-ptarmigan` "
                   "in a warm climate and it makes the same argument: a bird "
                   "with almost no colour identified by outline alone.",
             **b(length_m=0.30, bill_frac=0.050, head_frac=0.115,
                 neck_frac=0.045, body_frac=0.590, tail_frac=0.200,
                 posture_deg=12, body_depth=0.92, body_width=0.86,
                 chest_at=0.32, breast=0.80, rump=0.54, fullness=3.4,
                 section=2.5, belly=0.56, head_size=0.88, neck_up_deg=26,
                 neck_thick=0.62,
                 bill_depth=0.44, bill_curve=0.22, bill_gape=0.20,
                 tail_shape="rounded", tail_width=0.54, tail_droop=0.30,
                 pose="perched", wing_shape="elliptical", wing_span=1.40,
                 wing_aspect=4.8, wing_fold=0.50,
                 leg_len=0.085, eye=1.0, upperparts=0.50,
                 head_mark="throat", wing_mark="none", body_mark="barred",
                 mark_count=8, mark_width=0.22,
                 mat_back="skin_brown", mat_belly="plume_grey",
                 mat_head="plume_grey", mat_wing="skin_brown",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="beak_horn", mat_eye="skin_dark",
                 bio_grassland=1.0, bio_savanna=0.25,
                 place_abundance=0.35, place_spacing_m=30.0,
                 flock_despawn_m=90.0, flock_size_min=4, flock_size_max=16,
                 flock_spread_m=12.0, flock_perch="ground",
                 flock_height_min_m=0.5, flock_height_max_m=10.0,
                 flock_flight_share=0.06, flock_per_hectare=1.5)),
    ),
    "common-pheasant": (
        "0.80 m - half the bird is tail, red face, white neck ring",
        base(name="common-pheasant",
             notes="THE LONGEST TAIL SHARE IN THE LIBRARY at 0.58, beating the "
                   "scarlet macaw, and a graduated one -- a hard point rather "
                   "than a fan. On an animal this size that is nearly half a "
                   "metre of tail behind a gamebird body, and no other "
                   "silhouette in the world looks like it.\n\n"
                   "AUTHORED AS THE COCK. `sex_plumage` says male, and the hen "
                   "swaps to plain mottled buff with no head marking and a "
                   "shorter tail (`sex_tail` 1.55). That is the biggest tail "
                   "dimorphism in the library and the reason the row exists "
                   "separately from the size one.",
             **b(length_m=0.80, bill_frac=0.040, head_frac=0.075,
                 neck_frac=0.070, body_frac=0.235, tail_frac=0.580,
                 posture_deg=14, body_depth=0.86, body_width=0.78,
                 chest_at=0.32, breast=0.78, rump=0.44, fullness=3.3,
                 section=2.3, belly=0.54, head_size=0.85, neck_up_deg=34,
                 neck_thick=0.52,
                 bill_depth=0.40, bill_curve=0.18, bill_gape=0.16,
                 tail_shape="graduated", tail_width=0.18, tail_droop=0.55,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.05,
                 wing_aspect=5.0, wing_fold=0.22,
                 leg_len=0.10, eye=1.0, upperparts=0.52,
                 head_mark="collar", wing_mark="none", body_mark="speckled",
                 mark_width=0.14, mark_strength=0.30,
                 sex_tail=1.55, sex_plumage="male",
                 sex_alt_head_mark="none", sex_alt_body_mark="barred",
                 mat_back="plume_rufous", mat_belly="plume_rufous",
                 mat_head="plume_iridescent", mat_wing="skin_brown",
                 mat_mark="skin_dark", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="plume_crimson",
                 mat_alt_back="plume_buff", mat_alt_belly="plume_buff",
                 mat_alt_head="plume_buff", mat_alt_wing="plume_buff",
                 bio_grassland=1.0, bio_temperate_forest=0.7,
                 place_abundance=0.35, place_spacing_m=40.0,
                 flock_despawn_m=140.0, flock_size_min=1, flock_size_max=6,
                 flock_spread_m=20.0, flock_perch="ground",
                 flock_height_min_m=0.5, flock_height_max_m=12.0,
                 flock_flight_share=0.08, flock_per_hectare=1.0)),
    ),
    "common-crane": (
        "1.15 m - very tall, long neck and legs, drooping tertial bustle",
        base(name="common-crane",
             notes="THE LEG EXTREME at 0.36 of total length, past the heron's "
                   "0.30, and the tallest standing bird in the library short of "
                   "the ratites. Long neck held STRAIGHT rather than folded, "
                   "which is a crane's one reliable separation from a heron in "
                   "flight and here is `neck_up_deg` 50 on a straight body "
                   "rather than 64 on an S.\n\n"
                   "The drooping bustle of tertials over the tail is drawn as a "
                   "wide low-carried rounded tail; it is an approximation and "
                   "the height carries the species without it.",
             **b(length_m=1.15, bill_frac=0.085, head_frac=0.065,
                 neck_frac=0.265, body_frac=0.395, tail_frac=0.190,
                 posture_deg=6, body_depth=0.66, body_width=0.68,
                 chest_at=0.32, breast=0.70, rump=0.50, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.75, neck_up_deg=50,
                 neck_thick=0.26,
                 bill_depth=0.22, bill_gape=0.06,
                 tail_shape="rounded", tail_width=0.72, tail_droop=0.20,
                 tail_thick=2,
                 pose="perched", wing_shape="slotted", wing_span=1.95,
                 wing_aspect=7.4, wing_slots=5, wing_thick=3, wing_fold=0.85,
                 leg_len=0.36, leg_thick=1.5, eye=1.0, upperparts=0.50,
                 head_mark="cap", wing_mark="tip", body_mark="none",
                 mark_width=0.30,
                 mat_back="plume_grey", mat_belly="plume_grey",
                 mat_head="skin_dark", mat_wing="plume_slate",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="plume_crimson",
                 bio_grassland=0.9, bio_taiga=0.5, bio_temperate_forest=0.3,
                 place_abundance=0.12, place_spacing_m=120.0,
                 place_water_max_m=120.0,
                 flock_despawn_m=400.0, flock_size_min=2, flock_size_max=30,
                 flock_spread_m=100.0, flock_perch="ground",
                 flock_height_min_m=5.0, flock_height_max_m=400.0,
                 flock_flight_share=0.30, flock_per_hectare=0.2)),
    ),
    "white-stork": (
        "1.10 m - white with black flight feathers, long red bill and legs",
        base(name="white-stork",
             notes="THE ONLY BIRD IN THE LIBRARY THAT IS WHITE WITH BLACK "
                   "WINGS, which at any distance is the entire identification "
                   "and needs no shape at all -- so this spec spends its "
                   "silhouette budget on the legs and the bill instead. A heavy "
                   "straight red dagger, much deeper than a heron's, on legs "
                   "nearly as long as a crane's.",
             **b(length_m=1.10, bill_frac=0.155, head_frac=0.065,
                 neck_frac=0.190, body_frac=0.400, tail_frac=0.190,
                 posture_deg=6, body_depth=0.68, body_width=0.72,
                 chest_at=0.32, breast=0.72, rump=0.48, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.78, neck_up_deg=48,
                 neck_thick=0.34,
                 bill_depth=0.30, bill_gape=0.10,
                 tail_shape="square", tail_width=0.66, tail_droop=0.25,
                 tail_thick=2,
                 pose="perched", wing_shape="slotted", wing_span=2.10,
                 wing_aspect=6.6, wing_slots=6, wing_thick=3, wing_fold=0.85,
                 leg_len=0.32, leg_thick=1.5, eye=1.0, upperparts=0.50,
                 head_mark="none", wing_mark="tip", body_mark="none",
                 mark_width=0.45,
                 mat_back="plume_white", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_white",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="plume_crimson", mat_eye="skin_dark",
                 bio_grassland=0.9, bio_savanna=0.4, bio_temperate_forest=0.3,
                 place_abundance=0.15, place_spacing_m=150.0,
                 place_water_max_m=200.0,
                 flock_despawn_m=400.0, flock_size_min=1, flock_size_max=12,
                 flock_spread_m=80.0, flock_perch="ground",
                 flock_height_min_m=8.0, flock_height_max_m=500.0,
                 flock_flight_share=0.40, flock_per_hectare=0.2)),
    ),
    "red-kite": (
        "0.65 m - the fork is the species: long wings, deep rusty forked tail",
        base(name="red-kite",
             notes="THE ONLY FORKED-TAILED RAPTOR HERE, and the fork does all "
                   "the work: 0.55 deep on a tail share of 0.40, over a wing "
                   "span of 2.6 body lengths. A buzzard has a fanned rounded "
                   "tail at almost the same size, so the two are separated by "
                   "one tail shape and one number, which is exactly how a "
                   "birder separates them.",
             **b(length_m=0.65, bill_frac=0.050, head_frac=0.105,
                 neck_frac=0.045, body_frac=0.400, tail_frac=0.400,
                 posture_deg=4, body_depth=0.62, body_width=0.72,
                 chest_at=0.32, breast=0.74, rump=0.40, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.95, neck_up_deg=12,
                 neck_thick=0.58,
                 bill_depth=0.44, bill_hook=0.72, bill_gape=0.16,
                 tail_shape="forked", tail_width=0.50, tail_fork=0.55,
                 tail_droop=0.40, tail_thick=2,
                 pose="flying", wing_shape="slotted", wing_span=2.60,
                 wing_aspect=7.2, wing_slots=5, wing_sweep=0.20,
                 wing_dihedral=0.02, wing_thick=2, wing_fold=0.80,
                 leg_len=0.08, eye=1.0, upperparts=0.55,
                 head_mark="none", wing_mark="panel", body_mark="streaked",
                 mark_count=6, mark_width=0.26,
                 mat_back="plume_rufous", mat_belly="plume_rufous",
                 mat_head="plume_grey", mat_wing="skin_brown",
                 mat_mark="plume_white", mat_head_mark="skin_dark",
                 mat_bill="beak_horn", mat_eye="skin_yellow",
                 bio_grassland=0.9, bio_temperate_forest=0.6,
                 bio_bare_rock=0.3,
                 place_abundance=0.1, place_spacing_m=700.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=400.0, flock_size_min=1, flock_size_max=6,
                 flock_spread_m=120.0, flock_perch="canopy",
                 flock_height_min_m=20.0, flock_height_max_m=300.0,
                 flock_flight_share=0.75, flock_per_hectare=0.06)),
    ),
    "hen-harrier": (
        "0.48 m - slim long-winged raptor quartering low in a shallow V",
        base(name="hen-harrier",
             notes="THE DIHEDRAL IS THE SPECIES. A harrier hunts low with its "
                   "wings held in a hard shallow V, and `bird.wing_dihedral` at "
                   "0.42 is the highest in the library -- a buzzard is 0.18 and "
                   "a kestrel is negative. Seen from in front that V is the "
                   "whole identification and it costs one number.\n\n"
                   "AUTHORED AS THE MALE, which is pale grey with black "
                   "wingtips; the female is brown with a white rump, and the "
                   "`alt` rows swap her. Two completely different-looking birds "
                   "of one species, and `sex_plumage` declares which one "
                   "`unsexed` gives you.",
             **b(length_m=0.48, bill_frac=0.045, head_frac=0.105,
                 neck_frac=0.040, body_frac=0.375, tail_frac=0.435,
                 posture_deg=4, body_depth=0.58, body_width=0.68,
                 chest_at=0.32, breast=0.72, rump=0.38, fullness=3.0,
                 section=2.1, belly=0.50, head_size=1.0, neck_up_deg=12,
                 neck_thick=0.56,
                 bill_depth=0.42, bill_hook=0.62, bill_gape=0.14,
                 tail_shape="square", tail_width=0.42, tail_droop=0.42,
                 tail_thick=2,
                 pose="flying", wing_shape="pointed", wing_span=2.30,
                 wing_aspect=8.2, wing_sweep=0.18, wing_dihedral=0.42,
                 wing_thick=2, wing_fold=0.85,
                 leg_len=0.11, eye=1.0, upperparts=0.55,
                 head_mark="none", wing_mark="tip", body_mark="none",
                 mark_width=0.16, sex_length=0.94, sex_plumage="male",
                 sex_alt_body_mark="streaked",
                 mat_back="plume_grey", mat_belly="plume_white",
                 mat_head="plume_grey", mat_wing="plume_grey",
                 mat_mark="skin_dark", mat_head_mark="plume_white",
                 mat_bill="beak_horn", mat_eye="skin_yellow",
                 mat_alt_back="skin_brown", mat_alt_head="skin_brown",
                 mat_alt_wing="skin_brown", mat_alt_belly="plume_buff",
                 bio_grassland=0.9, bio_taiga=0.4, bio_tundra_alpine=0.3,
                 place_abundance=0.08, place_spacing_m=900.0,
                 flock_despawn_m=300.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=60.0, flock_perch="ground",
                 flock_height_min_m=2.0, flock_height_max_m=40.0,
                 flock_flight_share=0.80, flock_per_hectare=0.04)),
    ),
    "eurasian-magpie": (
        "0.46 m - black and white with an iridescent tail longer than the body",
        base(name="eurasian-magpie",
             notes="A PROPORTION SPECIES: the tail is longer than everything in "
                   "front of it, which survives any lattice and any distance. "
                   "Graduated, narrow, and carried well down. Hard black and "
                   "white blocks, with the gloss drawn as a real teal the way "
                   "`common-raven` carries its own.",
             **b(length_m=0.46, bill_frac=0.070, head_frac=0.105,
                 neck_frac=0.035, body_frac=0.290, tail_frac=0.500,
                 posture_deg=26, body_depth=0.76, body_width=0.66,
                 chest_at=0.32, breast=0.74, rump=0.42, fullness=3.2,
                 section=2.1, belly=0.52, head_size=1.0, neck_up_deg=28,
                 neck_thick=0.56,
                 bill_depth=0.36, bill_hook=0.15, bill_gape=0.18,
                 tail_shape="graduated", tail_width=0.24, tail_droop=0.60,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.30,
                 wing_aspect=4.8, wing_fold=0.28,
                 leg_len=0.11, eye=1.0, upperparts=0.60,
                 head_mark="none", wing_mark="panel", body_mark="none",
                 mark_width=0.35,
                 mat_back="skin_dark", mat_belly="plume_white",
                 mat_head="skin_dark", mat_wing="plume_iridescent",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="plume_grey", mat_eye="plume_white",
                 bio_grassland=0.9, bio_temperate_forest=0.6, bio_beach=0.2,
                 place_abundance=0.5, place_spacing_m=35.0,
                 flock_despawn_m=140.0, flock_size_min=1, flock_size_max=8,
                 flock_spread_m=30.0, flock_perch="canopy",
                 flock_height_min_m=2.0, flock_height_max_m=25.0,
                 flock_flight_share=0.30, flock_per_hectare=1.5)),
    ),
    "carrion-crow": (
        "0.48 m - all black, SQUARE tail, straight heavy bill",
        base(name="carrion-crow",
             notes="THE SHAPE `common-raven` IS DEFINED AGAINST, and the reason "
                   "it belongs in the library: a raven's identity is that it is "
                   "big, wedge-tailed and heavy-billed, and none of those three "
                   "means anything without a crow beside it. Square tail, "
                   "straighter shallower bill, three quarters of the length. "
                   "Everything else is deliberately identical.",
             **b(length_m=0.48, bill_frac=0.095, head_frac=0.135,
                 neck_frac=0.045, body_frac=0.390, tail_frac=0.335,
                 posture_deg=24, body_depth=0.72, body_width=0.68,
                 chest_at=0.34, breast=0.72, rump=0.44, fullness=3.0,
                 section=2.1, belly=0.52, head_size=1.0, neck_up_deg=26,
                 neck_thick=0.54,
                 bill_depth=0.36, bill_curve=0.04, bill_hook=0.12,
                 bill_gape=0.20,
                 tail_shape="square", tail_width=0.54, tail_droop=0.55,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.95,
                 wing_aspect=5.7, wing_fold=0.55, wing_thick=2,
                 leg_len=0.11, eye=1.0, upperparts=0.62,
                 head_mark="none", wing_mark="none", body_mark="none",
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="skin_dark", mat_wing="plume_slate",
                 mat_mark="plume_slate", mat_head_mark="plume_slate",
                 mat_bill="plume_grey", mat_eye="plume_white",
                 bio_grassland=0.9, bio_temperate_forest=0.7, bio_beach=0.4,
                 bio_taiga=0.3, bio_bare_rock=0.3,
                 place_abundance=0.5, place_spacing_m=45.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=160.0, flock_size_min=1, flock_size_max=10,
                 flock_spread_m=40.0, flock_perch="canopy",
                 flock_height_min_m=3.0, flock_height_max_m=90.0,
                 flock_flight_share=0.40, flock_per_hectare=1.2)),
    ),
    "western-jackdaw": (
        "0.34 m - small crow, pale grey nape shawl, pale eye",
        base(name="western-jackdaw",
             notes="THE THIRD CORVID IN THE SIZE LADDER -- raven 0.64, crow "
                   "0.48, jackdaw 0.34 -- and the one that is not uniform: a "
                   "pale grey shawl over the back of the head and neck, drawn "
                   "as a collar marking, plus a near-white eye that reads at "
                   "twenty voxels because it sits on black.",
             **b(length_m=0.34, bill_frac=0.075, head_frac=0.140,
                 neck_frac=0.040, body_frac=0.400, tail_frac=0.345,
                 posture_deg=26, body_depth=0.76, body_width=0.68,
                 chest_at=0.32, breast=0.74, rump=0.44, fullness=3.2,
                 section=2.1, belly=0.52, head_size=1.05, neck_up_deg=28,
                 neck_thick=0.58,
                 bill_depth=0.32, bill_gape=0.16,
                 tail_shape="square", tail_width=0.50, tail_droop=0.50,
                 pose="perched", wing_shape="elliptical", wing_span=1.85,
                 wing_aspect=5.4, wing_fold=0.50,
                 leg_len=0.11, eye=1.0, upperparts=0.62,
                 head_mark="collar", wing_mark="none", body_mark="none",
                 mark_width=0.38,
                 mat_back="skin_dark", mat_belly="plume_slate",
                 mat_head="skin_dark", mat_wing="skin_dark",
                 mat_mark="plume_grey", mat_head_mark="plume_grey",
                 mat_bill="plume_grey", mat_eye="plume_white",
                 bio_grassland=0.8, bio_bare_rock=0.6,
                 bio_temperate_forest=0.5, bio_beach=0.3,
                 place_abundance=0.6, place_spacing_m=12.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=160.0, flock_size_min=4, flock_size_max=80,
                 flock_spread_m=50.0, flock_perch="cliff",
                 flock_height_min_m=3.0, flock_height_max_m=80.0,
                 flock_flight_share=0.45, flock_per_hectare=6.0)),
    ),
    "eurasian-skylark": (
        "0.22 m - streaky brown with a short blunt crest, drawn hovering",
        base(name="eurasian-skylark",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 18 cm, the same fix four "
                   "birds in this library already carry. Do not correct it.\n\n"
                   "THE POSE IS THE SPECIES and the generator cannot hold it. A "
                   "skylark is known for hanging motionless high over a field, "
                   "which is a FLYING bird with a near-vertical body -- and "
                   "`bird.pose` flying deliberately uses a third of the "
                   "authored posture, because a bird in the air normally lies "
                   "along its line of travel. So this is authored flying with "
                   "the posture pushed to its ceiling, which gets partway "
                   "there. A true hover would be a third pose.",
             **b(length_m=0.22, bill_frac=0.070, head_frac=0.140,
                 neck_frac=0.030, body_frac=0.420, tail_frac=0.340,
                 posture_deg=60, body_depth=0.78, body_width=0.68,
                 chest_at=0.32, breast=0.74, rump=0.44, fullness=3.3,
                 section=2.1, belly=0.52, head_size=1.05, neck_up_deg=30,
                 neck_thick=0.58, crest=0.28,
                 bill_depth=0.30, bill_gape=0.12,
                 tail_shape="square", tail_width=0.46, tail_droop=0.45,
                 pose="flying", wing_shape="elliptical", wing_span=1.90,
                 wing_aspect=5.6, wing_sweep=0.10, wing_dihedral=0.20,
                 wing_fold=0.50,
                 leg_len=0.10, eye=1.0, upperparts=0.56,
                 head_mark="supercilium", wing_mark="none",
                 body_mark="streaked", mark_count=7, mark_width=0.20,
                 mat_back="plume_buff", mat_belly="plume_white",
                 mat_head="plume_buff", mat_wing="skin_brown",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="beak_horn", mat_eye="skin_dark",
                 bio_grassland=1.0, bio_beach=0.3, bio_tundra_alpine=0.2,
                 place_abundance=0.7, place_spacing_m=20.0,
                 flock_despawn_m=140.0, flock_size_min=1, flock_size_max=3,
                 flock_spread_m=40.0, flock_perch="ground",
                 flock_height_min_m=15.0, flock_height_max_m=90.0,
                 flock_flight_share=0.55, flock_per_hectare=3.0)),
    ),
    "european-goldfinch": (
        "0.22 m - red face, black-and-white head, broad gold wing bar",
        base(name="european-goldfinch",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 13 cm, which is the "
                   "largest enlargement in this file. It is worth it: a "
                   "goldfinch carries FOUR blocks of colour on a bird the size "
                   "of a matchbox, and at thirteen voxels none of them exist. "
                   "Recorded so nobody shrinks it.\n\n"
                   "The gold wing bar is the mark that survives at any range "
                   "and it is drawn wide -- a broad panel rather than a thin "
                   "bar -- because at this size a real bar is one voxel.",
             **song(length_m=0.22, bill_frac=0.075, head_frac=0.160,
                    neck_frac=0.030, body_frac=0.395, tail_frac=0.340,
                    posture_deg=34, bill_depth=0.48, bill_gape=0.14,
                    tail_shape="notched", tail_width=0.44, tail_fork=0.25,
                    tail_droop=0.48, upperparts=0.52,
                    head_mark="mask", wing_mark="bar", body_mark="none",
                    mark_width=0.30,
                    mat_back="plume_buff", mat_belly="plume_white",
                    mat_head="plume_white", mat_wing="skin_dark",
                    mat_mark="skin_yellow", mat_head_mark="plume_crimson",
                    mat_bill="beak_horn", mat_eye="skin_dark",
                    bio_grassland=0.9, bio_temperate_forest=0.5,
                    place_abundance=0.6, place_spacing_m=14.0,
                    flock_despawn_m=70.0, flock_size_min=3, flock_size_max=30,
                    flock_spread_m=15.0, flock_perch="shrub",
                    flock_height_min_m=1.0, flock_height_max_m=15.0,
                    flock_flight_share=0.30, flock_per_hectare=6.0)),
    ),
    "yellowhammer": (
        "0.22 m - lemon head and underparts over a chestnut rump",
        base(name="yellowhammer",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 17 cm.\n\n"
                   "ONE SATURATED COLOUR OVER MOST OF THE BIRD, which is the "
                   "opposite approach to the goldfinch beside it: no marking "
                   "carries this species, the base colour does. A long tail for "
                   "a bunting and a heavy seed-cracking cone of a bill.",
             **song(length_m=0.22, bill_frac=0.070, head_frac=0.145,
                    neck_frac=0.030, body_frac=0.375, tail_frac=0.380,
                    posture_deg=32, bill_depth=0.52, bill_gape=0.14,
                    tail_shape="notched", tail_width=0.40, tail_fork=0.20,
                    tail_droop=0.52, upperparts=0.46,
                    head_mark="none", wing_mark="none", body_mark="streaked",
                    mark_count=5, mark_width=0.16,
                    mat_back="plume_rufous", mat_belly="skin_yellow",
                    mat_head="skin_yellow", mat_wing="skin_brown",
                    mat_mark="skin_brown", mat_head_mark="skin_brown",
                    mat_bill="plume_slate", mat_eye="skin_dark",
                    bio_grassland=1.0, bio_temperate_forest=0.35,
                    place_abundance=0.55, place_spacing_m=18.0,
                    flock_despawn_m=70.0, flock_size_min=1, flock_size_max=12,
                    flock_spread_m=20.0, flock_perch="shrub",
                    flock_height_min_m=0.5, flock_height_max_m=12.0,
                    flock_flight_share=0.25, flock_per_hectare=4.0)),
    ),
    "little-owl": (
        "0.23 m - very small flat-headed owl with a fierce white brow",
        base(name="little-owl",
             notes="AN OWL AT A THIRD THE SIZE OF THE SHIPPED ONE, and the "
                   "difference is not only scale: a little owl's head is FLAT "
                   "on top rather than round, which here is a lower head size "
                   "and a much lower crown against `tawny-owl`'s 1.42. The "
                   "white brow over a yellow eye is the fierce expression "
                   "everybody recognises and it is two voxels.",
             **b(length_m=0.23, bill_frac=0.040, head_frac=0.185,
                 neck_frac=0.015, body_frac=0.505, tail_frac=0.255,
                 posture_deg=44, body_depth=0.82, body_width=0.80,
                 chest_at=0.30, breast=0.80, rump=0.50, fullness=3.4,
                 section=2.4, belly=0.52, head_size=1.20, neck_up_deg=42,
                 neck_thick=1.00,
                 bill_depth=0.50, bill_hook=0.65, bill_gape=0.18,
                 tail_shape="square", tail_width=0.52, tail_droop=0.35,
                 pose="perched", wing_shape="elliptical", wing_span=2.15,
                 wing_aspect=5.0, wing_fold=0.55,
                 leg_len=0.10, eye=2.0, upperparts=0.55,
                 head_mark="supercilium", wing_mark="none",
                 body_mark="speckled", mark_width=0.10, mark_strength=0.32,
                 mat_back="skin_brown", mat_belly="plume_buff",
                 mat_head="skin_brown", mat_wing="skin_brown",
                 mat_mark="skin_dark", mat_head_mark="plume_white",
                 mat_bill="plume_buff", mat_eye="skin_yellow",
                 bio_grassland=0.8, bio_desert=0.4, bio_temperate_forest=0.3,
                 bio_bare_rock=0.3,
                 place_abundance=0.15, place_spacing_m=150.0,
                 flock_despawn_m=90.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=8.0, flock_perch="shrub",
                 flock_height_min_m=1.0, flock_height_max_m=12.0,
                 flock_flight_share=0.15, flock_per_hectare=0.2)),
    ),
    # --- ratites: an animal-shaped entity with no new generator -------------
    "emu": (
        "1.50 m at 2 cm - shaggy grey-brown, bare blue neck, no flight",
        base(res="2", name="emu",
             notes="THE CHEAPEST POSSIBLE PARTIAL ANSWER TO 'THE WORLD HAS NO "
                   "ANIMALS'. `docs/biomes/README.md` §7 lists it sixth in the "
                   "whole build queue, on its own and out of order, because it "
                   "is a large unmistakable animal-shaped entity that the "
                   "SHIPPED bird generator makes.\n\n"
                   "AUTHORED AT 2 cm, NOT 1. A 1.5 m bird at 1 cm is 150 voxels "
                   "of a background animal; at 2 cm it is 75, which is inside "
                   "the range the generator is drawn to read at "
                   "(`kinds.py:129-134`), and its smallest identifying feature "
                   "-- the bare neck, roughly a hand's width -- is still five "
                   "voxels across. That is the house rule applied rather than "
                   "preferred.\n\n"
                   "NO FLIGHT. `wing_fold` is 0.02 and the wings are vestigial; "
                   "the pose is perched and `flight_share` is 0. The tail is "
                   "nearly absent and the legs are a third of the animal, which "
                   "is what makes it stand rather than perch.",
             **b(length_m=1.50, bill_frac=0.045, head_frac=0.070,
                 neck_frac=0.310, body_frac=0.485, tail_frac=0.090,
                 posture_deg=8, body_depth=0.86, body_width=0.86,
                 chest_at=0.36, breast=0.70, rump=0.68, fullness=2.6,
                 section=2.4, belly=0.52, head_size=0.62, neck_up_deg=62,
                 neck_thick=0.30,
                 bill_depth=0.42, bill_curve=0.10, bill_gape=0.28,
                 tail_shape="rounded", tail_width=0.90, tail_droop=0.10,
                 tail_thick=3,
                 pose="perched", wing_shape="elliptical", wing_span=0.85,
                 wing_aspect=3.0, wing_fold=0.02, wing_thick=2,
                 leg_len=0.33, leg_thick=3.0, eye=1.0, upperparts=0.60,
                 head_mark="none", wing_mark="none", body_mark="none",
                 mat_back="skin_brown", mat_belly="skin_brown",
                 mat_head="plume_slate", mat_wing="skin_brown",
                 mat_mark="plume_grey", mat_head_mark="skin_blue",
                 mat_bill="skin_yellow", mat_eye="skin_orange",
                 bio_grassland=0.9, bio_savanna=0.6, bio_desert=0.4,
                 place_abundance=0.08, place_spacing_m=200.0,
                 flock_despawn_m=350.0, flock_size_min=1, flock_size_max=6,
                 flock_spread_m=40.0, flock_perch="ground",
                 flock_height_min_m=0.0, flock_height_max_m=0.5,
                 flock_flight_share=0.0, flock_per_hectare=0.15)),
    ),
    "greater-rhea": (
        "1.30 m at 2 cm - grey-brown ratite, wings carried as a shawl",
        base(res="2", name="greater-rhea",
             notes="THE EMU'S TRICK AGAIN AND DELIBERATELY A DIFFERENT SHAPE: "
                   "shorter, greyer, with a longer visible neck and much bigger "
                   "wings, which a rhea carries loose over its flanks like a "
                   "shawl. `wing_fold` 0.55 against the emu's 0.02 is the whole "
                   "difference, and it is the parameter the bird research calls "
                   "the one wing cue that survives folding.",
             **b(length_m=1.30, bill_frac=0.045, head_frac=0.070,
                 neck_frac=0.340, body_frac=0.470, tail_frac=0.075,
                 posture_deg=10, body_depth=0.84, body_width=0.84,
                 chest_at=0.36, breast=0.70, rump=0.66, fullness=2.6,
                 section=2.4, belly=0.52, head_size=0.65, neck_up_deg=64,
                 neck_thick=0.28,
                 bill_depth=0.38, bill_curve=0.08, bill_gape=0.30,
                 tail_shape="rounded", tail_width=0.90, tail_droop=0.10,
                 tail_thick=3,
                 pose="perched", wing_shape="elliptical", wing_span=1.05,
                 wing_aspect=3.4, wing_fold=0.55, wing_thick=2,
                 leg_len=0.31, leg_thick=3.0, eye=1.0, upperparts=0.58,
                 head_mark="collar", wing_mark="none", body_mark="none",
                 mark_width=0.30,
                 mat_back="plume_grey", mat_belly="plume_buff",
                 mat_head="plume_grey", mat_wing="plume_grey",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="beak_horn", mat_eye="skin_dark",
                 bio_grassland=0.9, bio_savanna=0.6,
                 place_abundance=0.08, place_spacing_m=200.0,
                 flock_despawn_m=350.0, flock_size_min=2, flock_size_max=10,
                 flock_spread_m=50.0, flock_perch="ground",
                 flock_height_min_m=0.0, flock_height_max_m=0.5,
                 flock_flight_share=0.0, flock_per_hectare=0.15)),
    ),
    "common-ostrich": (
        "2.20 m at 2 cm - the largest asset in the animal library",
        base(res="2", name="common-ostrich",
             notes="THE BIGGEST BIRD THERE IS, and at 2 cm it is 110 voxels "
                   "long -- larger than any other animal in the library except "
                   "the great whales. A bare pink-grey neck and legs, a tiny "
                   "flat head, and a rounded feather mass carried very high on "
                   "two enormous legs.\n\n"
                   "THE SAVANNA FILE RECOMMENDS 5 cm AND THIS REJECTS IT. At "
                   "5 cm a 10 cm bare neck is two voxels, and the house rule in "
                   "`kinds.py:44-51` wants about three; two reads as a mistake "
                   "rather than as a feature. 2 cm gives five. That is the rule "
                   "applied to the same estimate the file used, not a "
                   "disagreement about the animal.\n\n"
                   "AUTHORED AS THE COCK -- black body with white wing and tail "
                   "plumes. The hen is uniform grey-brown and the `alt` rows "
                   "swap her, so `sex_plumage` declares that `unsexed` here is "
                   "a male.",
             **b(length_m=2.20, bill_frac=0.030, head_frac=0.055,
                 neck_frac=0.360, body_frac=0.475, tail_frac=0.080,
                 posture_deg=6, body_depth=0.92, body_width=0.90,
                 chest_at=0.36, breast=0.68, rump=0.72, fullness=2.5,
                 section=2.5, belly=0.52, head_size=0.55, neck_up_deg=68,
                 neck_thick=0.22,
                 bill_depth=0.40, bill_curve=0.08, bill_gape=0.35,
                 tail_shape="rounded", tail_width=0.95, tail_droop=0.05,
                 tail_thick=3,
                 pose="perched", wing_shape="elliptical", wing_span=0.95,
                 wing_aspect=3.2, wing_fold=0.45, wing_thick=3,
                 leg_len=0.38, leg_thick=4.0, eye=2.0, upperparts=0.72,
                 head_mark="none", wing_mark="tip", body_mark="none",
                 mark_width=0.45, sex_plumage="male",
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="plume_buff", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="beak_horn", mat_eye="skin_dark",
                 mat_alt_back="skin_brown", mat_alt_belly="skin_brown",
                 mat_alt_wing="skin_brown", mat_alt_mark="plume_buff",
                 bio_savanna=1.0, bio_desert=0.5, bio_grassland=0.3,
                 place_abundance=0.06, place_spacing_m=300.0,
                 flock_despawn_m=450.0, flock_size_min=1, flock_size_max=8,
                 flock_spread_m=60.0, flock_perch="ground",
                 flock_height_min_m=0.0, flock_height_max_m=0.5,
                 flock_flight_share=0.0, flock_per_hectare=0.1)),
    ),
    # --- forest: the trunk-clingers and the small canopy birds --------------
    "eurasian-nuthatch": (
        "0.22 m - blue-grey above, buff below, and it goes DOWN a trunk",
        base(name="eurasian-nuthatch",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 14 cm.\n\n"
                   "THE ONLY BIRD THAT DESCENDS A TRUNK HEAD-FIRST, and the "
                   "pose is the species. `posture_deg` is negative-adjacent "
                   "here in effect: the body is authored steeply nose-DOWN by "
                   "combining a low posture with a strongly negative neck angle "
                   "of -32, which points the head below the body axis. Nothing "
                   "else in the library has a negative neck angle, and it is "
                   "the one parameter that makes a nuthatch a nuthatch.\n\n"
                   "It also has NO VISIBLE NECK, so `neck_thick` is near 1 -- "
                   "head and body run together, the way an owl's do.",
             **b(length_m=0.22, bill_frac=0.115, head_frac=0.150,
                 neck_frac=0.020, body_frac=0.505, tail_frac=0.210,
                 posture_deg=-12, body_depth=0.78, body_width=0.70,
                 chest_at=0.30, breast=0.80, rump=0.48, fullness=3.4,
                 section=2.2, belly=0.52, head_size=1.05, neck_up_deg=-32,
                 neck_thick=0.95,
                 bill_depth=0.22, bill_gape=0.06,
                 tail_shape="square", tail_width=0.56, tail_droop=0.70,
                 pose="perched", wing_shape="elliptical", wing_span=1.60,
                 wing_aspect=5.0, wing_fold=0.55,
                 leg_len=0.085, eye=1.0, upperparts=0.50,
                 head_mark="mask", wing_mark="none", body_mark="none",
                 mark_width=0.22,
                 mat_back="plume_slate", mat_belly="plume_rufous",
                 mat_head="plume_slate", mat_wing="plume_slate",
                 mat_mark="skin_dark", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_temperate_forest=1.0, bio_taiga=0.3,
                 place_abundance=0.4, place_spacing_m=30.0,
                 flock_despawn_m=70.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=8.0, flock_perch="canopy",
                 flock_height_min_m=2.0, flock_height_max_m=20.0,
                 flock_flight_share=0.18, flock_per_hectare=1.5)),
    ),
    "eurasian-treecreeper": (
        "0.22 m - mottled brown, fine decurved bill, stiff tail on the bark",
        base(name="eurasian-treecreeper",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 13 cm.\n\n"
                   "THE NUTHATCH'S OPPOSITE ON ONE TRUNK: this one spirals UP, "
                   "nose-high at 66 degrees with the stiff tail pressed to the "
                   "bark as a prop -- `tail_droop` 0.85, nearly straight out of "
                   "the body line, which is the same trick "
                   "`great-spotted-woodpecker` uses. A fine strongly decurved "
                   "bill and cryptic brown, so the pose and the bill are the "
                   "entire read.",
             **b(length_m=0.22, bill_frac=0.105, head_frac=0.130,
                 neck_frac=0.025, body_frac=0.430, tail_frac=0.310,
                 posture_deg=66, body_depth=0.72, body_width=0.66,
                 chest_at=0.30, breast=0.74, rump=0.42, fullness=3.2,
                 section=2.1, belly=0.50, head_size=1.0, neck_up_deg=-4,
                 neck_thick=0.72,
                 bill_depth=0.14, bill_curve=0.55, bill_gape=0.04,
                 tail_shape="pointed", tail_width=0.42, tail_droop=0.85,
                 pose="perched", wing_shape="elliptical", wing_span=1.60,
                 wing_aspect=5.2, wing_fold=0.42,
                 leg_len=0.085, eye=1.0, upperparts=0.58,
                 head_mark="supercilium", wing_mark="bar",
                 body_mark="speckled", mark_width=0.10, mark_strength=0.30,
                 mat_back="skin_brown", mat_belly="plume_white",
                 mat_head="skin_brown", mat_wing="skin_brown",
                 mat_mark="skin_orange", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_temperate_forest=1.0, bio_taiga=0.5,
                 place_abundance=0.3, place_spacing_m=35.0,
                 flock_despawn_m=60.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=8.0, flock_perch="canopy",
                 flock_height_min_m=1.0, flock_height_max_m=18.0,
                 flock_flight_share=0.15, flock_per_hectare=1.2)),
    ),
    "common-blackbird": (
        "0.25 m - all black, and the bill has to carry it alone",
        base(name="common-blackbird",
             notes="THE HARDEST TEST OF THE BILL-CONTRAST GATE IN THE LIBRARY: "
                   "one flat black bird with a single orange-yellow bill and an "
                   "eye ring, and nothing else. Ten of the first twenty birds "
                   "here shipped with a bill invisible against the head -- a "
                   "great tit's black bill on its black cap measured a contrast "
                   "ratio of 1.00 -- so this species is the one that would fail "
                   "hardest if the gate were removed.",
             **song(length_m=0.25, bill_frac=0.085, head_frac=0.140,
                    neck_frac=0.035, body_frac=0.410, tail_frac=0.330,
                    posture_deg=30, body_depth=0.80, bill_depth=0.28,
                    bill_gape=0.13,
                    tail_shape="rounded", tail_width=0.46, tail_droop=0.55,
                    wing_span=1.65, wing_aspect=5.6, wing_fold=0.42,
                    upperparts=0.62,
                    head_mark="none", wing_mark="none", body_mark="none",
                    mat_back="skin_dark", mat_belly="skin_dark",
                    mat_head="skin_dark", mat_wing="skin_dark",
                    mat_mark="plume_slate", mat_head_mark="plume_slate",
                    mat_bill="skin_yellow", mat_eye="skin_yellow",
                    bio_temperate_forest=1.0, bio_grassland=0.6,
                    bio_taiga=0.25,
                    place_abundance=0.8, place_spacing_m=15.0,
                    flock_despawn_m=70.0, flock_size_min=1, flock_size_max=2,
                    flock_spread_m=10.0, flock_perch="shrub",
                    flock_height_min_m=0.5, flock_height_max_m=14.0,
                    flock_flight_share=0.20, flock_per_hectare=6.0)),
    ),
    "common-chaffinch": (
        "0.22 m - pink face and breast, and two hard white wing bars",
        base(name="common-chaffinch",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 15 cm.\n\n"
                   "THE DOUBLE WING BAR ON A SONGBIRD, where `rock-pigeon` has "
                   "it on a heavy grey body -- and the two make the argument "
                   "that a marking is a species only in combination with the "
                   "field it sits on. White on black wing against a pink body "
                   "and a blue-grey crown.",
             **song(length_m=0.22, bill_frac=0.075, head_frac=0.150,
                    neck_frac=0.030, body_frac=0.395, tail_frac=0.350,
                    posture_deg=34, bill_depth=0.46, bill_gape=0.14,
                    tail_shape="notched", tail_width=0.42, tail_fork=0.22,
                    tail_droop=0.50, upperparts=0.50,
                    head_mark="cap", wing_mark="doublebar", body_mark="none",
                    mark_width=0.16,
                    mat_back="skin_brown", mat_belly="plume_rufous",
                    mat_head="plume_rufous", mat_wing="skin_dark",
                    mat_mark="plume_white", mat_head_mark="plume_white",
                    mat_bill="skin_dark", mat_eye="skin_dark",
                    bio_temperate_forest=1.0, bio_taiga=0.5,
                    bio_grassland=0.4,
                    place_abundance=0.85, place_spacing_m=12.0,
                    flock_despawn_m=70.0, flock_size_min=2, flock_size_max=30,
                    flock_spread_m=18.0, flock_perch="canopy",
                    flock_height_min_m=1.0, flock_height_max_m=18.0,
                    flock_flight_share=0.28, flock_per_hectare=10.0)),
    ),
    "eurasian-bullfinch": (
        "0.22 m - fat, black-capped, brilliant rose breast, white rump",
        base(name="eurasian-bullfinch",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 16 cm.\n\n"
                   "THE DEEPEST BODY OF ANY SONGBIRD HERE at 0.98, which is "
                   "the top of the range, plus the shortest thickest bill. A "
                   "bullfinch reads as a BALL with a black cap, and that "
                   "roundness is more of the identification than the colour is.",
             **song(length_m=0.22, bill_frac=0.060, head_frac=0.155,
                    neck_frac=0.025, body_frac=0.430, tail_frac=0.330,
                    posture_deg=36, body_depth=0.98, body_width=0.72,
                    bill_depth=0.72, bill_gape=0.18,
                    tail_shape="square", tail_width=0.44, tail_droop=0.50,
                    upperparts=0.48,
                    head_mark="cap", wing_mark="bar", body_mark="none",
                    mark_width=0.18,
                    mat_back="plume_slate", mat_belly="plume_crimson",
                    mat_head="plume_crimson", mat_wing="skin_dark",
                    mat_mark="plume_white", mat_head_mark="skin_dark",
                    mat_bill="skin_dark", mat_eye="skin_dark",
                    bio_temperate_forest=1.0, bio_taiga=0.5,
                    place_abundance=0.4, place_spacing_m=25.0,
                    flock_despawn_m=70.0, flock_size_min=2, flock_size_max=6,
                    flock_spread_m=12.0, flock_perch="shrub",
                    flock_height_min_m=1.0, flock_height_max_m=12.0,
                    flock_flight_share=0.20, flock_per_hectare=2.0)),
    ),
    "blue-tit": (
        "0.22 m - blue cap, white face with a dark eye line, yellow below",
        base(name="blue-tit",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 12 cm, which is nearly "
                   "twice life size and the most arguable enlargement here. It "
                   "is done because a blue tit is FOUR colours and a mask, and "
                   "at twelve voxels it is a yellow smudge.\n\n"
                   "Deliberately built next to the shipped `great-tit`: same "
                   "size, same posture, and separated by cap colour, face "
                   "pattern and the absence of the great tit's black belly "
                   "stripe. If the two are confusable on a contact sheet, that "
                   "is the finding.",
             **song(length_m=0.22, bill_frac=0.065, head_frac=0.175,
                    neck_frac=0.025, body_frac=0.390, tail_frac=0.345,
                    posture_deg=38, body_depth=0.90, head_size=1.15,
                    bill_depth=0.28, bill_gape=0.12,
                    tail_shape="notched", tail_width=0.42, tail_fork=0.22,
                    tail_droop=0.48, upperparts=0.55,
                    head_mark="mask", wing_mark="bar", body_mark="none",
                    mark_width=0.18,
                    mat_back="plume_lime", mat_belly="skin_yellow",
                    mat_head="plume_white", mat_wing="plume_cyan",
                    mat_mark="skin_dark", mat_head_mark="skin_blue",
                    mat_bill="plume_slate", mat_eye="skin_dark",
                    bio_temperate_forest=1.0, bio_grassland=0.3,
                    bio_taiga=0.25,
                    place_abundance=0.9, place_spacing_m=10.0,
                    flock_despawn_m=60.0, flock_size_min=2, flock_size_max=12,
                    flock_spread_m=14.0, flock_perch="canopy",
                    flock_height_min_m=1.0, flock_height_max_m=16.0,
                    flock_flight_share=0.32, flock_per_hectare=12.0)),
    ),
    "long-tailed-tit": (
        "0.22 m - a ball of feathers with a tail longer than the body",
        base(name="long-tailed-tit",
             notes="A PURE RATIO SPECIES, AND RATIOS SURVIVE EVERY LATTICE -- "
                   "the temperate-forest file calls it the cheapest legible "
                   "species in the whole document and says to build it early. "
                   "Tail share 0.52 against a body share of 0.30, on the "
                   "roundest body in the set. Nothing else about it matters and "
                   "nothing else has to.\n\n"
                   "Authored at 0.22 m against a real 14 cm, of which more than "
                   "half is tail, so the body is only about seven voxels even "
                   "here.",
             **song(length_m=0.22, bill_frac=0.045, head_frac=0.135,
                    neck_frac=0.020, body_frac=0.280, tail_frac=0.520,
                    posture_deg=32, body_depth=1.00, body_width=0.76,
                    head_size=1.05, bill_depth=0.30, bill_gape=0.12,
                    tail_shape="graduated", tail_width=0.22, tail_droop=0.58,
                    wing_span=1.40, wing_aspect=4.8, wing_fold=0.22,
                    upperparts=0.48,
                    head_mark="supercilium", wing_mark="none",
                    body_mark="none", mark_width=0.30,
                    mat_back="plume_slate", mat_belly="plume_white",
                    mat_head="plume_white", mat_wing="skin_dark",
                    mat_mark="plume_white", mat_head_mark="skin_dark",
                    mat_bill="skin_dark", mat_eye="skin_dark",
                    bio_temperate_forest=1.0, bio_taiga=0.3,
                    place_abundance=0.5, place_spacing_m=20.0,
                    flock_despawn_m=70.0, flock_size_min=6, flock_size_max=20,
                    flock_spread_m=16.0, flock_perch="shrub",
                    flock_height_min_m=1.0, flock_height_max_m=12.0,
                    flock_flight_share=0.30, flock_per_hectare=3.0)),
    ),
    "eurasian-wren": (
        "0.20 m - tiny, round, rusty, tail held COCKED vertically",
        base(name="eurasian-wren",
             notes="AUTHORED AT 0.20 m AGAINST A REAL 10 cm -- twice life size, "
                   "and the library's floor. The temperate-forest file warns "
                   "about exactly this case for the goldcrest and concludes it "
                   "should not be built; the wren is kept because its identity "
                   "is a POSTURE rather than a size, and a posture reads at any "
                   "scale. Recorded so the decision is visible.\n\n"
                   "`tail_droop` is NEGATIVE at -0.35, which cocks the tail up "
                   "past vertical. Nothing else in the library uses the "
                   "negative half of that slider and the wren is the reason it "
                   "exists.",
             **song(length_m=0.20, bill_frac=0.080, head_frac=0.165,
                    neck_frac=0.020, body_frac=0.535, tail_frac=0.200,
                    posture_deg=46, body_depth=0.98, body_width=0.74,
                    head_size=1.05, bill_depth=0.20, bill_gape=0.06,
                    tail_shape="square", tail_width=0.62, tail_droop=-0.35,
                    wing_span=1.30, wing_aspect=4.6, wing_fold=0.12,
                    upperparts=0.55,
                    head_mark="supercilium", wing_mark="none",
                    body_mark="barred", mark_count=5, mark_width=0.18,
                    mat_back="plume_rufous", mat_belly="plume_buff",
                    mat_head="plume_rufous", mat_wing="skin_brown",
                    mat_mark="skin_dark", mat_head_mark="plume_white",
                    mat_bill="skin_dark", mat_eye="skin_dark",
                    bio_temperate_forest=1.0, bio_taiga=0.4,
                    bio_grassland=0.3, bio_bare_rock=0.2,
                    place_abundance=0.7, place_spacing_m=15.0,
                    flock_despawn_m=50.0, flock_size_min=1, flock_size_max=1,
                    flock_spread_m=5.0, flock_perch="shrub",
                    flock_height_min_m=0.3, flock_height_max_m=6.0,
                    flock_flight_share=0.12, flock_per_hectare=6.0)),
    ),
    "european-green-woodpecker": (
        "0.33 m - green-backed, red crown, yellow rump, feeds on the ground",
        base(name="european-green-woodpecker",
             notes="THE WOODPECKER THAT IS USUALLY ON THE GROUND, which is why "
                   "it is authored at 26 degrees rather than the shipped great "
                   "spotted's 68: it feeds on anthills. Same family, opposite "
                   "posture, and the posture is most of the difference in "
                   "silhouette.\n\n"
                   "Green back and a crimson crown, which the palette can now "
                   "do properly -- `plume_lime` is the only green light enough "
                   "to carry a dark marking on top of it.",
             **b(length_m=0.33, bill_frac=0.100, head_frac=0.145,
                 neck_frac=0.040, body_frac=0.435, tail_frac=0.280,
                 posture_deg=26, body_depth=0.78, body_width=0.70,
                 chest_at=0.31, breast=0.76, rump=0.46, fullness=3.2,
                 section=2.2, belly=0.52, head_size=1.05, neck_up_deg=30,
                 neck_thick=0.66,
                 bill_depth=0.30, bill_gape=0.14,
                 tail_shape="pointed", tail_width=0.44, tail_droop=0.70,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.75,
                 wing_aspect=5.2, wing_fold=0.42,
                 leg_len=0.10, eye=1.0, upperparts=0.60,
                 head_mark="cap", wing_mark="none", body_mark="none",
                 mark_width=0.5,
                 mat_back="plume_lime", mat_belly="plume_buff",
                 mat_head="skin_dark", mat_wing="plume_lime",
                 mat_mark="skin_yellow", mat_head_mark="plume_crimson",
                 mat_bill="skin_yellow", mat_eye="plume_white",
                 bio_temperate_forest=0.9, bio_grassland=0.4,
                 place_abundance=0.25, place_spacing_m=90.0,
                 flock_despawn_m=90.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=10.0, flock_perch="ground",
                 flock_height_min_m=1.0, flock_height_max_m=20.0,
                 flock_flight_share=0.20, flock_per_hectare=0.5)),
    ),
    "black-woodpecker": (
        "0.47 m - crow-sized, matt black, red crown, long neck",
        base(name="black-woodpecker",
             notes="TWICE THE GREAT SPOTTED WOODPECKER AND ONE COLOUR. Its "
                   "identity is size, a crimson crown and an unusually long "
                   "neck for a woodpecker -- `neck_frac` 0.07 against the "
                   "family's usual 0.03 -- which is what makes it look wrong "
                   "for a woodpecker and right for this species.",
             **b(length_m=0.47, bill_frac=0.105, head_frac=0.130,
                 neck_frac=0.070, body_frac=0.415, tail_frac=0.280,
                 posture_deg=64, body_depth=0.72, body_width=0.66,
                 chest_at=0.30, breast=0.74, rump=0.44, fullness=3.1,
                 section=2.2, belly=0.50, head_size=1.0, neck_up_deg=-2,
                 neck_thick=0.55,
                 bill_depth=0.30, bill_gape=0.16,
                 tail_shape="pointed", tail_width=0.44, tail_droop=0.82,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.70,
                 wing_aspect=5.4, wing_fold=0.45, wing_thick=2,
                 leg_len=0.09, eye=1.0, upperparts=0.65,
                 head_mark="cap", wing_mark="none", body_mark="none",
                 mark_width=0.5,
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="skin_dark", mat_wing="skin_dark",
                 mat_mark="plume_slate", mat_head_mark="plume_crimson",
                 mat_bill="plume_buff", mat_eye="plume_white",
                 bio_taiga=0.8, bio_temperate_forest=0.7,
                 place_abundance=0.12, place_spacing_m=250.0,
                 flock_despawn_m=110.0, flock_size_min=1, flock_size_max=1,
                 flock_spread_m=6.0, flock_perch="canopy",
                 flock_height_min_m=3.0, flock_height_max_m=30.0,
                 flock_flight_share=0.18, flock_per_hectare=0.2)),
    ),
    "eurasian-sparrowhawk": (
        "0.35 m - short broad wings and a long square tail",
        base(name="eurasian-sparrowhawk",
             notes="THE EXACT OPPOSITE PROPORTION TO A FALCON, and the third "
                   "point on the raptor aspect-ratio scale the shipped set "
                   "already covers at 5.6 (buzzard) and 7.5 (kestrel): an "
                   "accipiter sits at 6.2 with a much longer tail. All three "
                   "are raptors, all three read differently in the air, and the "
                   "difference is two numbers.\n\n"
                   "AUTHORED AS THE MALE. Females are appreciably larger -- "
                   "reversed size dimorphism, the rule for every raptor in this "
                   "library -- and this is the largest such ratio here at 0.87.",
             **b(length_m=0.35, bill_frac=0.045, head_frac=0.105,
                 neck_frac=0.035, body_frac=0.365, tail_frac=0.450,
                 posture_deg=6, body_depth=0.64, body_width=0.68,
                 chest_at=0.32, breast=0.74, rump=0.40, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.95, neck_up_deg=14,
                 neck_thick=0.58,
                 bill_depth=0.44, bill_hook=0.68, bill_gape=0.14,
                 tail_shape="square", tail_width=0.40, tail_droop=0.42,
                 tail_thick=2,
                 pose="flying", wing_shape="elliptical", wing_span=1.95,
                 wing_aspect=6.2, wing_sweep=0.12, wing_dihedral=0.04,
                 wing_thick=2, wing_fold=0.55,
                 leg_len=0.12, eye=1.0, upperparts=0.55,
                 head_mark="none", wing_mark="none", body_mark="barred",
                 mark_count=8, mark_width=0.30, sex_length=0.87,
                 sex_plumage="male", mat_alt_back="skin_brown",
                 mat_back="plume_slate", mat_belly="plume_white",
                 mat_head="plume_slate", mat_wing="plume_slate",
                 mat_mark="plume_rufous", mat_head_mark="skin_dark",
                 mat_bill="skin_yellow", mat_eye="skin_yellow",
                 bio_temperate_forest=0.9, bio_grassland=0.4, bio_taiga=0.3,
                 place_abundance=0.1, place_spacing_m=500.0,
                 flock_despawn_m=200.0, flock_size_min=1, flock_size_max=1,
                 flock_spread_m=20.0, flock_perch="canopy",
                 flock_height_min_m=3.0, flock_height_max_m=80.0,
                 flock_flight_share=0.55, flock_per_hectare=0.06)),
    ),
    "northern-goshawk": (
        "0.55 m - the sparrowhawk built heavy, white brow, barred breast",
        base(name="northern-goshawk",
             notes="THE SPARROWHAWK AT HALF AGAIN THE SIZE AND TWICE THE "
                   "WEIGHT, which is a real pair: same planform, same tail "
                   "shape, deeper body, heavier bill, and a bold white eyebrow "
                   "the smaller bird does not carry. Two species separated by "
                   "proportion plus one head marking is the harder half of what "
                   "this generator is for.",
             **b(length_m=0.55, bill_frac=0.050, head_frac=0.110,
                 neck_frac=0.040, body_frac=0.415, tail_frac=0.385,
                 posture_deg=6, body_depth=0.72, body_width=0.74,
                 chest_at=0.32, breast=0.78, rump=0.44, fullness=3.1,
                 section=2.2, belly=0.50, head_size=1.0, neck_up_deg=14,
                 neck_thick=0.62,
                 bill_depth=0.50, bill_hook=0.75, bill_gape=0.18,
                 tail_shape="square", tail_width=0.46, tail_droop=0.42,
                 tail_thick=2,
                 pose="flying", wing_shape="slotted", wing_span=2.05,
                 wing_aspect=6.0, wing_slots=4, wing_sweep=0.12,
                 wing_dihedral=0.06, wing_thick=2, wing_fold=0.60,
                 leg_len=0.11, eye=1.0, upperparts=0.55,
                 head_mark="supercilium", wing_mark="none", body_mark="barred",
                 mark_count=9, mark_width=0.28, sex_length=0.90,
                 mat_back="plume_slate", mat_belly="plume_white",
                 mat_head="plume_slate", mat_wing="plume_slate",
                 mat_mark="plume_grey", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="skin_orange",
                 bio_taiga=0.7, bio_temperate_forest=0.7,
                 place_abundance=0.05, place_spacing_m=1200.0,
                 flock_despawn_m=300.0, flock_size_min=1, flock_size_max=1,
                 flock_spread_m=30.0, flock_perch="canopy",
                 flock_height_min_m=5.0, flock_height_max_m=150.0,
                 flock_flight_share=0.55, flock_per_hectare=0.02)),
    ),
    "eurasian-woodcock": (
        "0.34 m - fat body, very long bill held down, eyes set far back",
        base(name="eurasian-woodcock",
             notes="A WADER'S BILL ON A GAMEBIRD'S BODY, which is a combination "
                   "nothing else has: body depth 0.94 with a bill share of 0.22. "
                   "It is cryptic dead-leaf brown with no marking that reads at "
                   "range, so the outline is the entire species and the outline "
                   "is that one contrast.",
             **b(length_m=0.34, bill_frac=0.220, head_frac=0.105,
                 neck_frac=0.030, body_frac=0.475, tail_frac=0.170,
                 posture_deg=10, body_depth=0.94, body_width=0.80,
                 chest_at=0.34, breast=0.78, rump=0.50, fullness=3.4,
                 section=2.4, belly=0.55, head_size=0.95, neck_up_deg=8,
                 neck_thick=0.70,
                 bill_depth=0.16, bill_curve=0.10, bill_gape=0.06,
                 tail_shape="rounded", tail_width=0.66, tail_droop=0.25,
                 pose="perched", wing_shape="elliptical", wing_span=1.75,
                 wing_aspect=5.0, wing_fold=0.55, wing_thick=2,
                 leg_len=0.075, eye=2.0, upperparts=0.55,
                 head_mark="none", wing_mark="none", body_mark="barred",
                 mark_count=7, mark_width=0.24,
                 mat_back="skin_brown", mat_belly="plume_buff",
                 mat_head="plume_buff", mat_wing="skin_brown",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="beak_horn", mat_eye="skin_dark",
                 bio_temperate_forest=1.0, bio_taiga=0.5,
                 place_abundance=0.15, place_spacing_m=90.0,
                 flock_despawn_m=90.0, flock_size_min=1, flock_size_max=1,
                 flock_spread_m=8.0, flock_perch="ground",
                 flock_height_min_m=1.0, flock_height_max_m=20.0,
                 flock_flight_share=0.12, flock_per_hectare=0.4)),
    ),
    # --- desert --------------------------------------------------------------
    "greater-roadrunner": (
        "0.55 m - long horizontal body, long cocked tail, blue-grey legs",
        base(name="greater-roadrunner",
             notes="A BIRD THAT BARELY FLIES, drawn as one: the body is held "
                   "level, the tail is long and cocked UP rather than trailing "
                   "(`tail_droop` 0.20 on a 0.42 tail share), and the legs are "
                   "long and heavy. `flight_share` is 0.05, which tells a "
                   "spawner to put it on the ground and leave it there.",
             **b(length_m=0.55, bill_frac=0.075, head_frac=0.110,
                 neck_frac=0.050, body_frac=0.345, tail_frac=0.420,
                 posture_deg=8, body_depth=0.62, body_width=0.66,
                 chest_at=0.32, breast=0.70, rump=0.40, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.95, neck_up_deg=24,
                 neck_thick=0.50, crest=0.35,
                 bill_depth=0.34, bill_curve=0.12, bill_gape=0.10,
                 tail_shape="graduated", tail_width=0.30, tail_droop=0.20,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.15,
                 wing_aspect=4.6, wing_fold=0.30,
                 leg_len=0.18, leg_thick=1.5, eye=1.0, upperparts=0.58,
                 head_mark="mask", wing_mark="none", body_mark="streaked",
                 mark_count=8, mark_width=0.20,
                 mat_back="skin_brown", mat_belly="plume_buff",
                 mat_head="skin_dark", mat_wing="skin_brown",
                 mat_mark="skin_dark", mat_head_mark="skin_orange",
                 mat_bill="skin_yellow", mat_eye="skin_yellow",
                 bio_desert=1.0, bio_grassland=0.35, bio_savanna=0.2,
                 place_abundance=0.12, place_spacing_m=250.0,
                 flock_despawn_m=140.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=20.0, flock_perch="ground",
                 flock_height_min_m=0.0, flock_height_max_m=4.0,
                 flock_flight_share=0.05, flock_per_hectare=0.15)),
    ),
    "crowned-sandgrouse": (
        "0.28 m - plump sand-camouflaged pigeon shape with pin tail feathers",
        base(name="crowned-sandgrouse",
             notes="THE ONE BIRD IN THE LIBRARY WHOSE PLUMAGE IS THE GROUND IT "
                   "STANDS ON. Fine sandy vermiculation over the whole animal, "
                   "drawn as a light speckle rather than a marking, so the only "
                   "hard-edged thing on it is a black-and-white face mask. "
                   "Pointed central tail feathers give it a `pointed` tail on a "
                   "gamebird body, which nothing else here has.",
             **b(length_m=0.28, bill_frac=0.045, head_frac=0.115,
                 neck_frac=0.040, body_frac=0.500, tail_frac=0.300,
                 posture_deg=12, body_depth=0.86, body_width=0.84,
                 chest_at=0.32, breast=0.80, rump=0.44, fullness=3.4,
                 section=2.4, belly=0.54, head_size=0.85, neck_up_deg=24,
                 neck_thick=0.62,
                 bill_depth=0.34, bill_gape=0.14,
                 tail_shape="pointed", tail_width=0.36, tail_droop=0.40,
                 pose="perched", wing_shape="pointed", wing_span=1.95,
                 wing_aspect=8.0, wing_sweep=0.32, wing_fold=0.80,
                 leg_len=0.06, eye=1.0, upperparts=0.50,
                 head_mark="mask", wing_mark="none", body_mark="speckled",
                 mark_width=0.06, mark_strength=0.24,
                 mat_back="plume_buff", mat_belly="plume_buff",
                 mat_head="plume_buff", mat_wing="plume_buff",
                 mat_mark="skin_brown", mat_head_mark="skin_dark",
                 mat_bill="plume_slate", mat_eye="skin_dark",
                 bio_desert=1.0, bio_savanna=0.3,
                 place_abundance=0.3, place_spacing_m=30.0,
                 flock_despawn_m=150.0, flock_size_min=4, flock_size_max=40,
                 flock_spread_m=40.0, flock_perch="ground",
                 flock_height_min_m=1.0, flock_height_max_m=60.0,
                 flock_flight_share=0.25, flock_per_hectare=2.0)),
    ),
    "cream-coloured-courser": (
        "0.23 m - pale sandy plover on long white legs, black eye stripe",
        base(name="cream-coloured-courser",
             notes="THE PALEST BIRD IN THE LIBRARY. Its whole body is one "
                   "cream-buff with no countershading worth the name -- "
                   "`upperparts` 0.35, so nearly all of it is the underparts "
                   "colour -- and the only marks are a hard black-and-white "
                   "stripe behind the eye and a grey crown. Upright, long "
                   "legged, and it runs rather than flies.",
             **b(length_m=0.23, bill_frac=0.080, head_frac=0.130,
                 neck_frac=0.060, body_frac=0.470, tail_frac=0.260,
                 posture_deg=16, body_depth=0.66, body_width=0.72,
                 chest_at=0.32, breast=0.72, rump=0.42, fullness=3.0,
                 section=2.1, belly=0.50, head_size=1.0, neck_up_deg=34,
                 neck_thick=0.52,
                 bill_depth=0.22, bill_curve=0.28, bill_gape=0.06,
                 tail_shape="square", tail_width=0.50, tail_droop=0.35,
                 pose="perched", wing_shape="pointed", wing_span=1.90,
                 wing_aspect=7.6, wing_sweep=0.28, wing_fold=0.75,
                 leg_len=0.19, eye=1.0, upperparts=0.35,
                 head_mark="supercilium", wing_mark="tip", body_mark="none",
                 mark_width=0.26,
                 mat_back="plume_buff", mat_belly="plume_buff",
                 mat_head="plume_grey", mat_wing="plume_buff",
                 mat_mark="skin_dark", mat_head_mark="plume_white",
                 mat_bill="skin_dark", mat_eye="skin_dark",
                 bio_desert=1.0, bio_savanna=0.25,
                 place_abundance=0.25, place_spacing_m=45.0,
                 flock_despawn_m=110.0, flock_size_min=2, flock_size_max=8,
                 flock_spread_m=30.0, flock_perch="ground",
                 flock_height_min_m=0.5, flock_height_max_m=15.0,
                 flock_flight_share=0.15, flock_per_hectare=0.8)),
    ),
    "trumpeter-finch": (
        "0.22 m - stubby pale finch with a swollen coral-red bill",
        base(name="trumpeter-finch",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 13 cm.\n\n"
                   "THE BILL IS MOST OF THE BIRD, and it is the deepest cone in "
                   "the library short of the puffin: `bill_depth` 0.85 in a "
                   "bright coral red on a pale grey-buff head, which is a "
                   "contrast pair rather than a colour choice. Everything else "
                   "about the animal is deliberately dull, because in life it "
                   "is.",
             **song(length_m=0.22, bill_frac=0.070, head_frac=0.165,
                    neck_frac=0.025, body_frac=0.420, tail_frac=0.320,
                    posture_deg=32, body_depth=0.92, bill_depth=0.85,
                    bill_gape=0.20,
                    tail_shape="notched", tail_width=0.44, tail_fork=0.20,
                    tail_droop=0.48, upperparts=0.46,
                    head_mark="none", wing_mark="none", body_mark="none",
                    mat_back="plume_buff", mat_belly="plume_buff",
                    mat_head="plume_buff", mat_wing="plume_buff",
                    mat_mark="plume_buff", mat_head_mark="plume_buff",
                    mat_bill="plume_crimson", mat_eye="skin_dark",
                    bio_desert=1.0, bio_bare_rock=0.3,
                    place_abundance=0.35, place_spacing_m=20.0,
                    flock_despawn_m=70.0, flock_size_min=2, flock_size_max=15,
                    flock_spread_m=20.0, flock_perch="ground",
                    flock_height_min_m=0.5, flock_height_max_m=12.0,
                    flock_flight_share=0.25, flock_per_hectare=2.0)),
    ),
    "lappet-faced-vulture": (
        "1.10 m - massive dark vulture, bare wrinkled head, heavy bill",
        base(name="lappet-faced-vulture",
             notes="THE HEAVIEST BILL ON ANY RAPTOR HERE and a bare head to go "
                   "with it. Slotted wings with six fingers on a span of 2.4 "
                   "body lengths and an aspect ratio of 6.4 -- a LAND thermal "
                   "soarer, which the bird research separates from an "
                   "albatross by a factor of two on hand-wing index. A vulture "
                   "gets its lift from area, and drawing it with an albatross "
                   "silhouette is a visible error.",
             **b(length_m=1.10, bill_frac=0.075, head_frac=0.100,
                 neck_frac=0.085, body_frac=0.435, tail_frac=0.305,
                 posture_deg=4, body_depth=0.70, body_width=0.78,
                 chest_at=0.32, breast=0.76, rump=0.46, fullness=3.0,
                 section=2.2, belly=0.50, head_size=0.90, neck_up_deg=20,
                 neck_thick=0.44,
                 bill_depth=0.66, bill_hook=0.80, bill_gape=0.24,
                 tail_shape="wedge", tail_width=0.50, tail_droop=0.40,
                 tail_thick=3,
                 pose="flying", wing_shape="slotted", wing_span=2.40,
                 wing_aspect=6.4, wing_slots=6, wing_sweep=0.14,
                 wing_dihedral=0.14, wing_thick=3, wing_fold=0.70,
                 leg_len=0.10, eye=1.0, upperparts=0.62,
                 head_mark="none", wing_mark="panel", body_mark="none",
                 mark_width=0.28,
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="skin_red", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="plume_buff", mat_eye="skin_dark",
                 bio_desert=0.9, bio_savanna=0.8, bio_bare_rock=0.4,
                 place_abundance=0.05, place_spacing_m=1500.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=600.0, flock_size_min=1, flock_size_max=6,
                 flock_spread_m=200.0, flock_perch="ground",
                 flock_height_min_m=30.0, flock_height_max_m=600.0,
                 flock_flight_share=0.80, flock_per_hectare=0.02)),
    ),
    "pharaoh-eagle-owl": (
        "0.48 m - pale sandy owl with orange eyes and two short ear tufts",
        base(name="pharaoh-eagle-owl",
             notes="THE EAR TUFTS ARE THE SILHOUETTE AND THEY ARE DRAWN "
                   "THICKENED. Real tufts taper to nothing at the tip; at 1 cm "
                   "that is two or three voxels at the root and gone above, "
                   "which reads as damage. `bird.crest` at 0.42 gives a blunt "
                   "swept spike instead -- deliberately heavier than life, and "
                   "the desert file recommends exactly this compromise.\n\n"
                   "The pale sandy version of the shipped `tawny-owl`, with "
                   "orange eyes rather than black, so the two are separated by "
                   "palette and by the tufts alone.",
             **b(length_m=0.48, bill_frac=0.040, head_frac=0.175,
                 neck_frac=0.015, body_frac=0.500, tail_frac=0.270,
                 posture_deg=42, body_depth=0.76, body_width=0.80,
                 chest_at=0.30, breast=0.80, rump=0.52, fullness=3.4,
                 section=2.4, belly=0.52, head_size=1.38, neck_up_deg=42,
                 neck_thick=1.05, crest=0.42,
                 bill_depth=0.55, bill_hook=0.70, bill_gape=0.20,
                 tail_shape="rounded", tail_width=0.58, tail_droop=0.35,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=2.30,
                 wing_aspect=5.2, wing_fold=0.60, wing_thick=2,
                 leg_len=0.09, eye=2.0, upperparts=0.52,
                 head_mark="mask", wing_mark="none", body_mark="streaked",
                 mark_count=7, mark_width=0.26,
                 mat_back="plume_buff", mat_belly="plume_buff",
                 mat_head="plume_buff", mat_wing="plume_buff",
                 mat_mark="skin_dark", mat_head_mark="skin_brown",
                 mat_bill="plume_slate", mat_eye="skin_orange",
                 bio_desert=1.0, bio_bare_rock=0.5,
                 place_abundance=0.08, place_spacing_m=400.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=140.0, flock_size_min=1, flock_size_max=1,
                 flock_spread_m=8.0, flock_perch="cliff",
                 flock_height_min_m=2.0, flock_height_max_m=30.0,
                 flock_flight_share=0.15, flock_per_hectare=0.05)),
    ),
    "houbara-bustard": (
        "0.70 m - long-legged sandy bustard with a loose black-and-white ruff",
        base(name="houbara-bustard",
             notes="A BUSTARD IS A LONG-LEGGED GROUND BIRD WITH A LONG NECK, "
                   "and the ruff of loose feathers down each side of that neck "
                   "is the display feature. There is no neck-marking slot in "
                   "the generator, so the ruff is drawn as a `collar` head "
                   "marking at maximum width -- an approximation, and the "
                   "sandy vermiculated back plus the stance carry the rest.",
             **b(length_m=0.70, bill_frac=0.055, head_frac=0.085,
                 neck_frac=0.190, body_frac=0.440, tail_frac=0.230,
                 posture_deg=8, body_depth=0.74, body_width=0.78,
                 chest_at=0.34, breast=0.74, rump=0.50, fullness=3.0,
                 section=2.3, belly=0.52, head_size=0.80, neck_up_deg=52,
                 neck_thick=0.42,
                 bill_depth=0.28, bill_curve=0.10, bill_gape=0.12,
                 tail_shape="rounded", tail_width=0.62, tail_droop=0.25,
                 tail_thick=2,
                 pose="perched", wing_shape="slotted", wing_span=1.95,
                 wing_aspect=6.2, wing_slots=4, wing_thick=2, wing_fold=0.75,
                 leg_len=0.23, leg_thick=2.0, eye=1.0, upperparts=0.52,
                 head_mark="collar", wing_mark="tip", body_mark="speckled",
                 mark_width=0.45, mark_strength=0.22,
                 mat_back="plume_buff", mat_belly="plume_white",
                 mat_head="plume_grey", mat_wing="plume_buff",
                 mat_mark="skin_dark", mat_head_mark="plume_white",
                 mat_bill="plume_slate", mat_eye="skin_dark",
                 bio_desert=1.0, bio_grassland=0.3, bio_savanna=0.25,
                 place_abundance=0.07, place_spacing_m=400.0,
                 flock_despawn_m=250.0, flock_size_min=1, flock_size_max=3,
                 flock_spread_m=50.0, flock_perch="ground",
                 flock_height_min_m=1.0, flock_height_max_m=40.0,
                 flock_flight_share=0.10, flock_per_hectare=0.1)),
    ),
    # --- savanna -------------------------------------------------------------
    "secretary-bird": (
        "1.35 m - an eagle's head on crane's legs, quills off the nape",
        base(name="secretary-bird",
             notes="THE STRANGEST PROPORTION IN THE LIBRARY: a hooked raptor "
                   "bill and head on legs that are 0.34 of the total length, "
                   "plus two very long central tail feathers. Nothing else is "
                   "built like it and it needs no colour at all to be "
                   "recognised.\n\n"
                   "THE HEAD QUILLS ARE DRAWN THICKENED. The savanna file "
                   "estimates them at about 2 cm, which at 1 cm is two voxels "
                   "and under the three-voxel rule; `bird.crest` at 0.95 -- the "
                   "highest in the library -- gives a heavy spray instead. That "
                   "is a stylisation and it is why this bird is at 1 cm and not "
                   "coarser: at 2 cm the quills would be one voxel.",
             **b(length_m=1.35, bill_frac=0.045, head_frac=0.075,
                 neck_frac=0.105, body_frac=0.335, tail_frac=0.440,
                 posture_deg=8, body_depth=0.62, body_width=0.68,
                 chest_at=0.32, breast=0.72, rump=0.38, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.95, neck_up_deg=44,
                 neck_thick=0.34, crest=0.95,
                 bill_depth=0.48, bill_hook=0.72, bill_gape=0.14,
                 tail_shape="graduated", tail_width=0.24, tail_droop=0.30,
                 tail_thick=2,
                 pose="perched", wing_shape="slotted", wing_span=1.55,
                 wing_aspect=6.6, wing_slots=5, wing_thick=2, wing_fold=0.55,
                 leg_len=0.34, leg_thick=2.0, eye=1.0, upperparts=0.48,
                 head_mark="mask", wing_mark="tip", body_mark="none",
                 mark_width=0.30,
                 mat_back="plume_grey", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_grey",
                 mat_mark="skin_dark", mat_head_mark="skin_orange",
                 mat_bill="plume_slate", mat_eye="skin_dark",
                 bio_savanna=1.0, bio_grassland=0.3,
                 place_abundance=0.06, place_spacing_m=600.0,
                 flock_despawn_m=300.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=40.0, flock_perch="ground",
                 flock_height_min_m=1.0, flock_height_max_m=60.0,
                 flock_flight_share=0.15, flock_per_hectare=0.05)),
    ),
    "lilac-breasted-roller": (
        "0.36 m - the colour hero: lilac breast, turquoise belly, blue wing",
        base(name="lilac-breasted-roller",
             notes="THE MOST COLOURS ON ONE BIRD IN THE LIBRARY, and the "
                   "species that justifies `plume_lilac` outside a jay's wing. "
                   "Four saturated fields -- lilac breast, turquoise underparts, "
                   "deep blue wing panel, brown back -- plus two thin tail "
                   "streamers. It is what stops the savanna's bird list reading "
                   "as a hot Mediterranean summer, which is the range note the "
                   "savanna file makes about the seven species already shipped "
                   "there.\n\n"
                   "TWO OF ITS FOUR COLOURS MOVED TO CLEAR THE CONTRAST "
                   "GATE, and it is the only species in this pass where "
                   "that changed the animal rather than the palette. The "
                   "real bird has a LIME-GREEN head with a lilac throat, "
                   "and `tools/birdprobe.py --read` measured that pair at "
                   "1.59 against a floor of 2.0 -- the lilac disappears "
                   "into the green. The head is drawn slate instead, "
                   "against which lilac measures 2.21; and the bill, "
                   "black in life, is drawn grey because black on slate "
                   "measures 1.94 and grey measures 2.46. Both are "
                   "departures from the animal made to keep its marks "
                   "visible, which is the trade the gate exists to force.",
             **b(length_m=0.36, bill_frac=0.070, head_frac=0.135,
                 neck_frac=0.030, body_frac=0.375, tail_frac=0.390,
                 posture_deg=22, body_depth=0.80, body_width=0.70,
                 chest_at=0.32, breast=0.78, rump=0.42, fullness=3.3,
                 section=2.1, belly=0.52, head_size=1.10, neck_up_deg=26,
                 neck_thick=0.66,
                 bill_depth=0.38, bill_hook=0.20, bill_gape=0.14,
                 tail_shape="forked", tail_width=0.30, tail_fork=0.45,
                 tail_droop=0.55,
                 pose="perched", wing_shape="elliptical", wing_span=1.90,
                 wing_aspect=5.6, wing_fold=0.50,
                 leg_len=0.075, eye=1.0, upperparts=0.44,
                 head_mark="throat", wing_mark="panel", body_mark="none",
                 mark_width=0.32,
                 mat_back="plume_buff", mat_belly="plume_cyan",
                 mat_head="plume_slate", mat_wing="skin_blue",
                 mat_mark="plume_white", mat_head_mark="plume_lilac",
                 mat_bill="plume_grey", mat_eye="skin_dark",
                 bio_savanna=1.0, bio_grassland=0.25,
                 place_abundance=0.3, place_spacing_m=60.0,
                 flock_despawn_m=140.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=25.0, flock_perch="shrub",
                 flock_height_min_m=1.0, flock_height_max_m=20.0,
                 flock_flight_share=0.25, flock_per_hectare=0.6)),
    ),
    # --- taiga ---------------------------------------------------------------
    "western-capercaillie": (
        "0.90 m - turkey-sized black grouse with a fanned upright tail",
        base(name="western-capercaillie",
             notes="THE ONE GENUINELY MISSING SHAPE IN A BIOME WHERE ELEVEN OF "
                   "TWELVE BIRDS ALREADY SHIP: a very large ground grouse with "
                   "a broad tail carried up and fanned. Body depth 0.96 and "
                   "tail width 0.85 are both near the ceiling, which together "
                   "are the display posture.\n\n"
                   "AUTHORED AS THE COCK -- matt black with an ivory bill and a "
                   "red brow. The hen is barred rufous and half the size, which "
                   "is the largest bird size dimorphism in this library at "
                   "`sex_length` 1.20.",
             **b(length_m=0.90, bill_frac=0.045, head_frac=0.100,
                 neck_frac=0.075, body_frac=0.510, tail_frac=0.270,
                 posture_deg=18, body_depth=0.96, body_width=0.86,
                 chest_at=0.34, breast=0.78, rump=0.56, fullness=3.4,
                 section=2.5, belly=0.55, head_size=0.85, neck_up_deg=40,
                 neck_thick=0.58,
                 bill_depth=0.50, bill_curve=0.22, bill_gape=0.22,
                 tail_shape="rounded", tail_width=0.85, tail_droop=-0.15,
                 tail_thick=3,
                 pose="perched", wing_shape="elliptical", wing_span=1.30,
                 wing_aspect=4.6, wing_fold=0.45, wing_thick=2,
                 leg_len=0.09, eye=1.0, upperparts=0.62,
                 head_mark="supercilium", wing_mark="none", body_mark="none",
                 mark_width=0.55, sex_length=1.20, sex_plumage="male",
                 sex_alt_body_mark="barred",
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="plume_iridescent", mat_wing="skin_brown",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="plume_white", mat_eye="skin_dark",
                 mat_alt_back="plume_rufous", mat_alt_head="plume_rufous",
                 mat_alt_belly="plume_buff", mat_alt_wing="skin_brown",
                 bio_taiga=1.0,
                 place_abundance=0.1, place_spacing_m=200.0,
                 flock_despawn_m=180.0, flock_size_min=1, flock_size_max=3,
                 flock_spread_m=20.0, flock_perch="ground",
                 flock_height_min_m=0.5, flock_height_max_m=12.0,
                 flock_flight_share=0.06, flock_per_hectare=0.15)),
    ),
    # --- tundra / alpine -----------------------------------------------------
    "bearded-vulture": (
        "1.15 m - the DIAMOND tail, narrow pointed wings, rust underparts",
        base(name="bearded-vulture",
             notes="THE ONLY DIAMOND TAIL IN THE LIBRARY, and the tundra file "
                   "is explicit that the tail and not the colour is the "
                   "identification -- so this is authored `wedge` at the "
                   "narrowest width any large bird here carries, on POINTED "
                   "rather than slotted wings. Every other big soarer in the "
                   "set is broad and fingered; this one is a long narrow "
                   "cross with a spike behind it.\n\n"
                   "Slate above, rust-orange below, with a dark facial mask.",
             **b(length_m=1.15, bill_frac=0.055, head_frac=0.095,
                 neck_frac=0.055, body_frac=0.395, tail_frac=0.400,
                 posture_deg=4, body_depth=0.62, body_width=0.72,
                 chest_at=0.32, breast=0.74, rump=0.38, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.90, neck_up_deg=14,
                 neck_thick=0.55,
                 bill_depth=0.46, bill_hook=0.72, bill_gape=0.16,
                 tail_shape="wedge", tail_width=0.22, tail_droop=0.42,
                 tail_thick=3,
                 pose="flying", wing_shape="pointed", wing_span=2.45,
                 wing_aspect=8.4, wing_sweep=0.22, wing_dihedral=0.06,
                 wing_thick=3, wing_fold=0.80,
                 leg_len=0.08, eye=1.0, upperparts=0.55,
                 head_mark="mask", wing_mark="none", body_mark="none",
                 mark_width=0.5,
                 mat_back="plume_slate", mat_belly="plume_rufous",
                 mat_head="plume_buff", mat_wing="plume_slate",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="plume_slate", mat_eye="skin_orange",
                 bio_tundra_alpine=1.0, bio_bare_rock=0.8,
                 place_abundance=0.04, place_spacing_m=2000.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=700.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=200.0, flock_perch="cliff",
                 flock_height_min_m=40.0, flock_height_max_m=800.0,
                 flock_flight_share=0.88, flock_per_hectare=0.01)),
    ),
    "snowy-owl": (
        "0.60 m - bulky white owl, round flat face, yellow eyes",
        base(name="snowy-owl",
             notes="THE CLEANEST SEX PAIR IN THE LIBRARY: an adult male is "
                   "near-pure white and a female is heavily barred, on exactly "
                   "the same geometry. That is `bird.sex_plumage` doing the "
                   "whole job -- the spec is authored MALE with no body "
                   "marking, and the female's `alt` row turns barring on. No "
                   "proportion moves at all, which is the honest form of avian "
                   "dimorphism the bird research records.",
             **b(length_m=0.60, bill_frac=0.040, head_frac=0.170,
                 neck_frac=0.015, body_frac=0.510, tail_frac=0.265,
                 posture_deg=44, body_depth=0.80, body_width=0.84,
                 chest_at=0.30, breast=0.82, rump=0.54, fullness=3.4,
                 section=2.5, belly=0.52, head_size=1.40, neck_up_deg=42,
                 neck_thick=1.10,
                 bill_depth=0.55, bill_hook=0.72, bill_gape=0.18,
                 tail_shape="rounded", tail_width=0.58, tail_droop=0.32,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=2.35,
                 wing_aspect=5.4, wing_fold=0.62, wing_thick=2,
                 leg_len=0.08, eye=2.0, upperparts=0.50,
                 head_mark="none", wing_mark="none", body_mark="none",
                 mark_count=6, mark_width=0.22, sex_length=0.94,
                 sex_plumage="male", sex_alt_body_mark="barred",
                 mat_back="plume_white", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_white",
                 mat_mark="skin_dark", mat_head_mark="plume_grey",
                 mat_bill="skin_dark", mat_eye="skin_yellow",
                 bio_tundra_alpine=1.0, bio_taiga=0.3,
                 place_abundance=0.06, place_spacing_m=800.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=250.0, flock_size_min=1, flock_size_max=1,
                 flock_spread_m=20.0, flock_perch="ground",
                 flock_height_min_m=1.0, flock_height_max_m=40.0,
                 flock_flight_share=0.25, flock_per_hectare=0.03)),
    ),
    "alpine-chough": (
        "0.38 m - glossy black corvid with a SHORT YELLOW bill",
        base(name="alpine-chough",
             notes="AUTHORED AS ONE HALF OF A DELIBERATE PAIR with "
                   "`red-billed-chough`. The two birds are the same size, the "
                   "same colour and the same shape, and the entire difference "
                   "is that this one's bill is short, straight and yellow and "
                   "the other's is long, downcurved and red. Two sliders and "
                   "one material. If the two are indistinguishable on a contact "
                   "sheet, THAT IS THE FINDING and it belongs to the owner, not "
                   "to me.",
             **b(length_m=0.38, bill_frac=0.055, head_frac=0.130,
                 neck_frac=0.040, body_frac=0.395, tail_frac=0.380,
                 posture_deg=24, body_depth=0.74, body_width=0.68,
                 chest_at=0.32, breast=0.74, rump=0.44, fullness=3.1,
                 section=2.1, belly=0.52, head_size=1.0, neck_up_deg=28,
                 neck_thick=0.58,
                 bill_depth=0.34, bill_curve=0.05, bill_gape=0.14,
                 tail_shape="square", tail_width=0.50, tail_droop=0.52,
                 pose="perched", wing_shape="elliptical", wing_span=2.05,
                 wing_aspect=5.8, wing_fold=0.65, wing_thick=2,
                 leg_len=0.10, eye=1.0, upperparts=0.62,
                 head_mark="none", wing_mark="none", body_mark="none",
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="skin_dark", mat_wing="plume_iridescent",
                 mat_mark="plume_slate", mat_head_mark="plume_slate",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_tundra_alpine=1.0, bio_bare_rock=0.8,
                 place_abundance=0.4, place_spacing_m=15.0,
                 place_slope_max_pct=70.0, place_elev_min_m=800,
                 flock_despawn_m=250.0, flock_size_min=6, flock_size_max=60,
                 flock_spread_m=70.0, flock_perch="cliff",
                 flock_height_min_m=5.0, flock_height_max_m=200.0,
                 flock_flight_share=0.55, flock_per_hectare=3.0)),
    ),
    "eurasian-dotterel": (
        "0.22 m - plump upright plover, white V eyebrow, rufous belly",
        base(name="eurasian-dotterel",
             notes="THE ONE SPECIES IN THE LIBRARY WHERE THE FEMALE IS THE "
                   "BRIGHTER BIRD, which inverts the palette rule every other "
                   "dimorphic spec here follows. So this one is authored FEMALE "
                   "-- `sex_plumage` says so -- and the male's `alt` rows dull "
                   "him down. Recorded because `unsexed` on this species is a "
                   "hen, and that is exactly the kind of thing that becomes "
                   "invisible if it is not declared.",
             **b(length_m=0.22, bill_frac=0.070, head_frac=0.140,
                 neck_frac=0.045, body_frac=0.480, tail_frac=0.265,
                 posture_deg=16, body_depth=0.78, body_width=0.76,
                 chest_at=0.32, breast=0.76, rump=0.46, fullness=3.2,
                 section=2.2, belly=0.52, head_size=1.05, neck_up_deg=28,
                 neck_thick=0.60,
                 bill_depth=0.24, bill_gape=0.08,
                 tail_shape="square", tail_width=0.50, tail_droop=0.35,
                 pose="perched", wing_shape="pointed", wing_span=1.85,
                 wing_aspect=7.4, wing_sweep=0.26, wing_fold=0.80,
                 leg_len=0.13, eye=1.0, upperparts=0.50,
                 head_mark="supercilium", wing_mark="none",
                 body_mark="breastband", mark_width=0.24,
                 sex_plumage="female",
                 mat_back="skin_brown", mat_belly="plume_rufous",
                 mat_head="plume_slate", mat_wing="skin_brown",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 mat_alt_belly="plume_buff",
                 bio_tundra_alpine=1.0,
                 place_abundance=0.2, place_spacing_m=60.0,
                 place_elev_min_m=700,
                 flock_despawn_m=100.0, flock_size_min=2, flock_size_max=10,
                 flock_spread_m=25.0, flock_perch="ground",
                 flock_height_min_m=0.5, flock_height_max_m=20.0,
                 flock_flight_share=0.18, flock_per_hectare=0.8)),
    ),
    "snow-bunting": (
        "0.22 m - white body, black wings and back, huge white wing flashes",
        base(name="snow-bunting",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 16 cm.\n\n"
                   "TWO FLAT COLOURS AND NOTHING ELSE, which at twenty-two "
                   "voxels is exactly enough: white bird, black wings and back. "
                   "It is the alpine equivalent of the scarlet tanager's "
                   "argument -- no pattern, no texture, and it works.",
             **song(length_m=0.22, bill_frac=0.065, head_frac=0.140,
                    neck_frac=0.030, body_frac=0.435, tail_frac=0.330,
                    posture_deg=28, bill_depth=0.44, bill_gape=0.14,
                    tail_shape="notched", tail_width=0.46, tail_fork=0.20,
                    tail_droop=0.45, upperparts=0.58,
                    head_mark="none", wing_mark="panel", body_mark="none",
                    mark_width=0.40,
                    mat_back="skin_dark", mat_belly="plume_white",
                    mat_head="plume_white", mat_wing="skin_dark",
                    mat_mark="plume_white", mat_head_mark="skin_dark",
                    mat_bill="skin_dark", mat_eye="skin_dark",
                    bio_tundra_alpine=1.0, bio_bare_rock=0.4, bio_taiga=0.2,
                    place_abundance=0.4, place_spacing_m=15.0,
                    place_elev_min_m=500,
                    flock_despawn_m=110.0, flock_size_min=4, flock_size_max=50,
                    flock_spread_m=30.0, flock_perch="ground",
                    flock_height_min_m=0.5, flock_height_max_m=25.0,
                    flock_flight_share=0.35, flock_per_hectare=3.0)),
    ),
    "horned-lark": (
        "0.22 m - sandy ground lark with a black mask and two feather horns",
        base(name="horned-lark",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 17 cm.\n\n"
                   "THE HORNS ARE DRAWN ABOVE LIFE SIZE AND THE TUNDRA FILE "
                   "SAYS TO. They are two feather tufts a few millimetres "
                   "across, which at 1 cm is one voxel or nothing; `bird.crest` "
                   "at 0.22 gives a short blunt spike instead. Do NOT try to "
                   "solve this with a finer lattice -- nothing else about the "
                   "bird needs one, and the black face mask over a yellow "
                   "throat is what actually carries it.",
             **song(length_m=0.22, bill_frac=0.065, head_frac=0.140,
                    neck_frac=0.030, body_frac=0.455, tail_frac=0.310,
                    posture_deg=22, body_depth=0.76, crest=0.22,
                    bill_depth=0.34, bill_gape=0.12,
                    tail_shape="square", tail_width=0.48, tail_droop=0.40,
                    upperparts=0.54,
                    head_mark="mask", wing_mark="none", body_mark="none",
                    mark_width=0.26,
                    mat_back="plume_buff", mat_belly="plume_white",
                    mat_head="skin_yellow", mat_wing="skin_brown",
                    mat_mark="skin_dark", mat_head_mark="skin_dark",
                    mat_bill="plume_slate", mat_eye="skin_dark",
                    bio_tundra_alpine=1.0, bio_grassland=0.3,
                    bio_desert=0.25,
                    place_abundance=0.45, place_spacing_m=18.0,
                    flock_despawn_m=100.0, flock_size_min=2, flock_size_max=30,
                    flock_spread_m=30.0, flock_perch="ground",
                    flock_height_min_m=0.5, flock_height_max_m=20.0,
                    flock_flight_share=0.25, flock_per_hectare=3.0)),
    ),
    # --- bare rock: half of everything the biome can contain ----------------
    "peregrine-falcon": (
        "0.45 m - anchor-shaped, slate above, black moustache",
        base(name="peregrine-falcon",
             notes="THE HIGHEST SWEEP IN THE LIBRARY at 0.80 -- the wings raked "
                   "right back into the anchor shape of a stoop, which no other "
                   "raptor here carries. A kestrel is 0.30 on the same "
                   "planform, and the difference between the two is that one "
                   "number and a much heavier body.\n\n"
                   "The black moustache under the eye is the field mark and it "
                   "is a `mask` marking placed to sit on the eye's edge, which "
                   "is what the mask mechanism does.",
             **b(length_m=0.45, bill_frac=0.045, head_frac=0.115,
                 neck_frac=0.040, body_frac=0.435, tail_frac=0.365,
                 posture_deg=4, body_depth=0.70, body_width=0.74,
                 chest_at=0.32, breast=0.78, rump=0.40, fullness=3.1,
                 section=2.2, belly=0.50, head_size=1.0, neck_up_deg=12,
                 neck_thick=0.62,
                 bill_depth=0.56, bill_hook=0.70, bill_gape=0.16,
                 tail_shape="rounded", tail_width=0.40, tail_droop=0.42,
                 tail_thick=2,
                 pose="flying", wing_shape="pointed", wing_span=2.25,
                 wing_aspect=8.0, wing_sweep=0.80, wing_dihedral=-0.15,
                 wing_thick=2, wing_fold=0.90,
                 leg_len=0.09, eye=1.0, upperparts=0.55,
                 head_mark="mask", wing_mark="none", body_mark="barred",
                 mark_count=9, mark_width=0.24, sex_length=0.90,
                 mat_back="plume_slate", mat_belly="plume_white",
                 mat_head="plume_slate", mat_wing="plume_slate",
                 mat_mark="skin_dark", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_bare_rock=1.0, bio_tundra_alpine=0.5, bio_beach=0.35,
                 bio_grassland=0.3,
                 place_abundance=0.05, place_spacing_m=1500.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=400.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=60.0, flock_perch="cliff",
                 flock_height_min_m=10.0, flock_height_max_m=400.0,
                 flock_flight_share=0.60, flock_per_hectare=0.02)),
    ),
    "griffon-vulture": (
        "1.00 m - pale sandy vulture, bare white head, plank wings",
        base(name="griffon-vulture",
             notes="THE WIDEST WING AREA IN THE LIBRARY: span 2.6 body lengths "
                   "at an aspect ratio of only 6.0, which is a plank rather "
                   "than a blade and is exactly the LAND thermal soarer the "
                   "bird research separates from an albatross. Six finger "
                   "slots, three voxels thick.\n\n"
                   "A bare whitish head on a long neck with a white ruff at its "
                   "base, which is drawn as a collar -- and the ruff is what "
                   "makes a vulture's head read as a head rather than a stub.",
             **b(length_m=1.00, bill_frac=0.060, head_frac=0.090,
                 neck_frac=0.115, body_frac=0.450, tail_frac=0.285,
                 posture_deg=4, body_depth=0.70, body_width=0.80,
                 chest_at=0.32, breast=0.76, rump=0.46, fullness=3.0,
                 section=2.2, belly=0.50, head_size=0.80, neck_up_deg=22,
                 neck_thick=0.36,
                 bill_depth=0.56, bill_hook=0.78, bill_gape=0.18,
                 tail_shape="square", tail_width=0.56, tail_droop=0.40,
                 tail_thick=3,
                 pose="flying", wing_shape="slotted", wing_span=2.60,
                 wing_aspect=6.0, wing_slots=6, wing_sweep=0.10,
                 wing_dihedral=0.16, wing_thick=3, wing_fold=0.70,
                 leg_len=0.09, eye=1.0, upperparts=0.55,
                 head_mark="collar", wing_mark="tip", body_mark="none",
                 mark_width=0.35,
                 mat_back="plume_buff", mat_belly="plume_buff",
                 mat_head="plume_white", mat_wing="skin_brown",
                 mat_mark="plume_white", mat_head_mark="skin_dark",
                 mat_bill="plume_slate", mat_eye="skin_dark",
                 bio_bare_rock=1.0, bio_tundra_alpine=0.6, bio_desert=0.4,
                 bio_grassland=0.25,
                 place_abundance=0.05, place_spacing_m=1500.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=700.0, flock_size_min=1, flock_size_max=10,
                 flock_spread_m=250.0, flock_perch="cliff",
                 flock_height_min_m=30.0, flock_height_max_m=800.0,
                 flock_flight_share=0.82, flock_per_hectare=0.03)),
    ),
    "red-billed-chough": (
        "0.40 m - glossy black corvid with a long DOWNCURVED red bill",
        base(name="red-billed-chough",
             notes="THE ALPINE CHOUGH'S PAIR, and the whole difference is the "
                   "bill: long, thin, strongly decurved and red, against short, "
                   "straight and yellow. The bare-rock file flags that at 1 cm "
                   "the bill is about a voxel thick and any error kills the "
                   "identification, so it is authored slightly deeper than life "
                   "-- 0.22 rather than the 0.14 a bill this fine would "
                   "otherwise take. Written down so it is not thinned back.",
             **b(length_m=0.40, bill_frac=0.115, head_frac=0.120,
                 neck_frac=0.040, body_frac=0.385, tail_frac=0.340,
                 posture_deg=24, body_depth=0.74, body_width=0.68,
                 chest_at=0.32, breast=0.74, rump=0.44, fullness=3.1,
                 section=2.1, belly=0.52, head_size=0.98, neck_up_deg=28,
                 neck_thick=0.56,
                 bill_depth=0.22, bill_curve=0.58, bill_gape=0.06,
                 tail_shape="square", tail_width=0.52, tail_droop=0.52,
                 pose="perched", wing_shape="elliptical", wing_span=2.10,
                 wing_aspect=5.6, wing_fold=0.62, wing_thick=2,
                 leg_len=0.10, eye=1.0, upperparts=0.62,
                 head_mark="none", wing_mark="none", body_mark="none",
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="skin_dark", mat_wing="plume_iridescent",
                 mat_mark="plume_slate", mat_head_mark="plume_slate",
                 mat_bill="plume_crimson", mat_eye="skin_dark",
                 bio_bare_rock=1.0, bio_tundra_alpine=0.6, bio_beach=0.25,
                 place_abundance=0.3, place_spacing_m=25.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=220.0, flock_size_min=3, flock_size_max=30,
                 flock_spread_m=60.0, flock_perch="cliff",
                 flock_height_min_m=3.0, flock_height_max_m=150.0,
                 flock_flight_share=0.50, flock_per_hectare=1.5)),
    ),
    "wallcreeper": (
        "0.22 m - ash grey with huge crimson-and-black rounded wings",
        base(name="wallcreeper",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 16 cm, and the bare-rock "
                   "file asks for exactly that: at 16 cm this bird is at the "
                   "bottom of the generator's range before its 3 cm needle bill "
                   "is even considered. The bill is also authored deeper than "
                   "life for the same reason as the chough's.\n\n"
                   "THE MOST CLIFF-SPECIFIC BIRD THERE IS -- it lives on "
                   "nothing else -- and it is authored FLYING because the "
                   "crimson wing panels only exist when the wings are open. "
                   "Perched it is a grey lump; that is a real property of the "
                   "animal and not a limitation of the generator.",
             **b(length_m=0.22, bill_frac=0.130, head_frac=0.130,
                 neck_frac=0.025, body_frac=0.475, tail_frac=0.240,
                 posture_deg=20, body_depth=0.76, body_width=0.68,
                 chest_at=0.30, breast=0.76, rump=0.44, fullness=3.3,
                 section=2.2, belly=0.52, head_size=1.0, neck_up_deg=10,
                 neck_thick=0.72,
                 bill_depth=0.18, bill_curve=0.30, bill_gape=0.04,
                 tail_shape="square", tail_width=0.56, tail_droop=0.50,
                 pose="flying", wing_shape="elliptical", wing_span=1.85,
                 wing_aspect=4.6, wing_sweep=0.06, wing_dihedral=0.12,
                 wing_fold=0.40,
                 leg_len=0.09, eye=1.0, upperparts=0.50,
                 head_mark="none", wing_mark="panel", body_mark="none",
                 mark_width=0.55,
                 mat_back="plume_grey", mat_belly="plume_grey",
                 mat_head="plume_grey", mat_wing="skin_dark",
                 mat_mark="plume_crimson", mat_head_mark="skin_dark",
                 mat_bill="skin_dark", mat_eye="skin_dark",
                 bio_bare_rock=1.0, bio_tundra_alpine=0.4,
                 place_abundance=0.15, place_spacing_m=80.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=90.0, flock_size_min=1, flock_size_max=1,
                 flock_spread_m=10.0, flock_perch="cliff",
                 flock_height_min_m=2.0, flock_height_max_m=60.0,
                 flock_flight_share=0.45, flock_per_hectare=0.3)),
    ),
    "alpine-swift": (
        "0.24 m - scythe wings much longer than the body, white belly",
        base(name="alpine-swift",
             notes="THE HIGHEST `wing_fold` IN THE LIBRARY at 1.25: a swift's "
                   "folded primaries reach well PAST its tail tip, which is the "
                   "one wing cue that survives folding and is more reliable "
                   "than any colour. It also has the shortest legs here at "
                   "0.025 -- a swift never stands, only clings, and "
                   "`flock.perch` is `air` to say so to a spawner.",
             **b(length_m=0.24, bill_frac=0.040, head_frac=0.120,
                 neck_frac=0.020, body_frac=0.520, tail_frac=0.300,
                 posture_deg=6, body_depth=0.46, body_width=0.70,
                 chest_at=0.28, breast=0.72, rump=0.30, fullness=3.2,
                 section=2.0, belly=0.50, head_size=1.0, neck_up_deg=10,
                 neck_thick=0.72,
                 bill_depth=0.14, bill_gape=0.70,
                 tail_shape="notched", tail_width=0.40, tail_fork=0.22,
                 tail_droop=0.42,
                 pose="flying", wing_shape="pointed", wing_span=2.45,
                 wing_aspect=10.5, wing_sweep=0.72, wing_dihedral=-0.05,
                 wing_fold=1.25,
                 leg_len=0.025, eye=1.0, upperparts=0.60,
                 head_mark="throat", wing_mark="none", body_mark="breastband",
                 mark_width=0.5,
                 mat_back="skin_brown", mat_belly="plume_white",
                 mat_head="skin_brown", mat_wing="skin_brown",
                 mat_mark="skin_brown", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_bare_rock=1.0, bio_tundra_alpine=0.5, bio_grassland=0.2,
                 place_abundance=0.3, place_spacing_m=30.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=300.0, flock_size_min=5, flock_size_max=60,
                 flock_spread_m=120.0, flock_perch="air",
                 flock_height_min_m=10.0, flock_height_max_m=300.0,
                 flock_flight_share=0.98, flock_per_hectare=2.0)),
    ),
    "eurasian-crag-martin": (
        "0.22 m - small stocky brown martin, broad square spotted tail",
        base(name="eurasian-crag-martin",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 14 cm.\n\n"
                   "THE PLAIN ONE ON THE CLIFF, and deliberately: no fork, no "
                   "streamers, no colour. It sits directly against the shipped "
                   "`barn-swallow`, which is the same family with a 0.52 fork "
                   "and a red throat, and the pair is the argument that a tail "
                   "SHAPE is a species where a tail length is only a size.",
             **b(length_m=0.22, bill_frac=0.040, head_frac=0.135,
                 neck_frac=0.025, body_frac=0.480, tail_frac=0.320,
                 posture_deg=8, body_depth=0.62, body_width=0.70,
                 chest_at=0.28, breast=0.74, rump=0.36, fullness=3.3,
                 section=2.0, belly=0.50, head_size=1.0, neck_up_deg=14,
                 neck_thick=0.68,
                 bill_depth=0.16, bill_gape=0.55,
                 tail_shape="square", tail_width=0.58, tail_droop=0.42,
                 pose="flying", wing_shape="pointed", wing_span=2.05,
                 wing_aspect=8.0, wing_sweep=0.50, wing_dihedral=0.06,
                 wing_fold=1.05,
                 leg_len=0.045, eye=1.0, upperparts=0.55,
                 head_mark="none", wing_mark="none", body_mark="speckled",
                 mark_width=0.08, mark_strength=0.14,
                 mat_back="skin_brown", mat_belly="plume_buff",
                 mat_head="skin_brown", mat_wing="skin_brown",
                 mat_mark="skin_dark", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_bare_rock=1.0, bio_tundra_alpine=0.4, bio_beach=0.25,
                 place_abundance=0.35, place_spacing_m=20.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=200.0, flock_size_min=4, flock_size_max=30,
                 flock_spread_m=60.0, flock_perch="cliff",
                 flock_height_min_m=3.0, flock_height_max_m=120.0,
                 flock_flight_share=0.75, flock_per_hectare=2.5)),
    ),
    # --- rainforest ----------------------------------------------------------
    "toco-toucan": (
        "0.60 m - a crow carrying an enormous curved orange bill",
        base(name="toco-toucan",
             notes="THE LONGEST BILL SHARE IN THE LIBRARY at 0.30, which is the "
                   "top of the slider's range -- nearly a third of the animal. "
                   "It is also deep and curved, so all four bill parameters are "
                   "doing something at once, and that is the whole species: a "
                   "black body with a white bib behind an orange shape.",
             **b(length_m=0.60, bill_frac=0.300, head_frac=0.090,
                 neck_frac=0.030, body_frac=0.325, tail_frac=0.255,
                 posture_deg=24, body_depth=0.80, body_width=0.70,
                 chest_at=0.32, breast=0.76, rump=0.44, fullness=3.3,
                 section=2.1, belly=0.52, head_size=1.05, neck_up_deg=26,
                 neck_thick=0.68,
                 bill_depth=0.60, bill_curve=0.35, bill_gape=0.22,
                 tail_shape="square", tail_width=0.44, tail_droop=0.55,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.35,
                 wing_aspect=4.8, wing_fold=0.35,
                 leg_len=0.075, eye=1.0, upperparts=0.60,
                 head_mark="throat", wing_mark="none", body_mark="none",
                 mark_width=0.40,
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="skin_dark", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="skin_orange", mat_eye="skin_blue",
                 bio_rainforest=1.0,
                 place_abundance=0.25, place_spacing_m=70.0,
                 flock_despawn_m=160.0, flock_size_min=2, flock_size_max=8,
                 flock_spread_m=30.0, flock_perch="canopy",
                 flock_height_min_m=8.0, flock_height_max_m=45.0,
                 flock_flight_share=0.25, flock_per_hectare=0.5)),
    ),
    "great-hornbill": (
        "1.10 m - heavy bird, long banded tail, casque on the bill",
        base(name="great-hornbill",
             notes="THE TOUCAN'S HEAVYWEIGHT COUNTERPART, and it needs a "
                   "different trick: a hornbill's casque is a hollow block "
                   "sitting ON TOP of the bill, and there is no casque "
                   "parameter. It is drawn as an unusually DEEP bill instead -- "
                   "`bill_depth` 0.80 at a bill share of 0.22 -- which gives "
                   "the right heavy blunt profile at twenty voxels and is an "
                   "honest approximation rather than a rendering of the "
                   "structure.\n\n"
                   "Broad white wing bars and a long banded tail, both of which "
                   "read from below, which is where it is seen.",
             **b(length_m=1.10, bill_frac=0.220, head_frac=0.085,
                 neck_frac=0.045, body_frac=0.350, tail_frac=0.300,
                 posture_deg=18, body_depth=0.78, body_width=0.72,
                 chest_at=0.32, breast=0.76, rump=0.46, fullness=3.2,
                 section=2.2, belly=0.52, head_size=1.0, neck_up_deg=22,
                 neck_thick=0.62,
                 bill_depth=0.80, bill_curve=0.28, bill_gape=0.24,
                 tail_shape="square", tail_width=0.42, tail_droop=0.50,
                 tail_thick=3,
                 pose="perched", wing_shape="slotted", wing_span=1.55,
                 wing_aspect=5.4, wing_slots=4, wing_thick=3, wing_fold=0.50,
                 leg_len=0.075, eye=1.0, upperparts=0.60,
                 head_mark="none", wing_mark="doublebar", body_mark="none",
                 mark_width=0.24,
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="skin_dark", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="skin_yellow",
                 mat_bill="skin_yellow", mat_eye="plume_crimson",
                 bio_rainforest=1.0,
                 place_abundance=0.12, place_spacing_m=200.0,
                 flock_despawn_m=250.0, flock_size_min=1, flock_size_max=4,
                 flock_spread_m=50.0, flock_perch="canopy",
                 flock_height_min_m=10.0, flock_height_max_m=50.0,
                 flock_flight_share=0.30, flock_per_hectare=0.2)),
    ),
    "harpy-eagle": (
        "1.00 m - massive chest, SHORT broad wings, double crest",
        base(name="harpy-eagle",
             notes="THE SHORTEST WING OF ANY LARGE RAPTOR HERE -- span 1.6 body "
                   "lengths against a golden eagle's 2.45 -- because it hunts "
                   "inside a canopy and cannot afford span. That inversion of "
                   "what a big eagle looks like is the point of having it: the "
                   "aspect ratio and the span are what separate a forest raptor "
                   "from an open-country one, and nothing else does.\n\n"
                   "The double crest that splits into two horns is drawn as a "
                   "single heavy crest; two separate spikes at 1 cm would be "
                   "one voxel each.",
             **b(length_m=1.00, bill_frac=0.060, head_frac=0.115,
                 neck_frac=0.050, body_frac=0.455, tail_frac=0.320,
                 posture_deg=16, body_depth=0.82, body_width=0.78,
                 chest_at=0.30, breast=0.82, rump=0.46, fullness=3.3,
                 section=2.3, belly=0.52, head_size=1.05, neck_up_deg=24,
                 neck_thick=0.66, crest=0.55,
                 bill_depth=0.62, bill_hook=0.88, bill_gape=0.20,
                 tail_shape="square", tail_width=0.46, tail_droop=0.45,
                 tail_thick=3,
                 pose="perched", wing_shape="slotted", wing_span=1.60,
                 wing_aspect=5.0, wing_slots=5, wing_thick=3, wing_fold=0.45,
                 leg_len=0.14, leg_thick=2.5, eye=1.0, upperparts=0.58,
                 head_mark="none", wing_mark="none", body_mark="breastband",
                 mark_width=0.30, sex_length=0.88,
                 mat_back="plume_slate", mat_belly="plume_white",
                 mat_head="plume_grey", mat_wing="plume_slate",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="skin_dark", mat_eye="plume_grey",
                 bio_rainforest=1.0,
                 place_abundance=0.04, place_spacing_m=2000.0,
                 flock_despawn_m=300.0, flock_size_min=1, flock_size_max=1,
                 flock_spread_m=30.0, flock_perch="canopy",
                 flock_height_min_m=10.0, flock_height_max_m=60.0,
                 flock_flight_share=0.30, flock_per_hectare=0.01)),
    ),
    "great-blue-turaco": (
        "0.75 m - thickset blue-green bird with a blunt black crest",
        base(name="great-blue-turaco",
             notes="A LARGE BIRD THAT IS ACTUALLY BLUE-GREEN, which the palette "
                   "could not do before `plume_cyan` and `plume_lime` existed. "
                   "Long broad tail with a hard black band, a yellow-and-red "
                   "bill, and a standing crest of blunt black feathers over the "
                   "forehead rather than swept back.",
             **b(length_m=0.75, bill_frac=0.065, head_frac=0.110,
                 neck_frac=0.035, body_frac=0.345, tail_frac=0.445,
                 posture_deg=22, body_depth=0.82, body_width=0.72,
                 chest_at=0.32, breast=0.78, rump=0.44, fullness=3.3,
                 section=2.2, belly=0.52, head_size=1.05, neck_up_deg=26,
                 neck_thick=0.64, crest=0.48,
                 bill_depth=0.52, bill_curve=0.20, bill_gape=0.20,
                 tail_shape="square", tail_width=0.40, tail_droop=0.55,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.25,
                 wing_aspect=4.6, wing_fold=0.30,
                 leg_len=0.09, eye=1.0, upperparts=0.55,
                 head_mark="cap", wing_mark="none", body_mark="none",
                 mark_width=0.5,
                 mat_back="plume_cyan", mat_belly="plume_lime",
                 mat_head="plume_cyan", mat_wing="plume_cyan",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="plume_crimson", mat_eye="skin_dark",
                 bio_rainforest=1.0,
                 place_abundance=0.2, place_spacing_m=80.0,
                 flock_despawn_m=160.0, flock_size_min=2, flock_size_max=8,
                 flock_spread_m=30.0, flock_perch="canopy",
                 flock_height_min_m=8.0, flock_height_max_m=40.0,
                 flock_flight_share=0.20, flock_per_hectare=0.4)),
    ),
    "hoatzin": (
        "0.65 m - huge ragged spiky crest, bare blue face, red eye",
        base(name="hoatzin",
             notes="THE RAGGED CREST IS THE SPECIES and it is authored at 0.88, "
                   "second only to the secretary bird. Small body, broad tail, "
                   "heavy chestnut-and-cream wings, and a bare blue face with a "
                   "red eye -- which is the only place in the library the eye "
                   "material is doing identification work on its own.",
             **b(length_m=0.65, bill_frac=0.050, head_frac=0.110,
                 neck_frac=0.075, body_frac=0.375, tail_frac=0.390,
                 posture_deg=26, body_depth=0.80, body_width=0.70,
                 chest_at=0.32, breast=0.76, rump=0.46, fullness=3.2,
                 section=2.2, belly=0.52, head_size=0.95, neck_up_deg=34,
                 neck_thick=0.48, crest=0.88,
                 bill_depth=0.40, bill_curve=0.15, bill_gape=0.16,
                 tail_shape="rounded", tail_width=0.56, tail_droop=0.50,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.25,
                 wing_aspect=4.6, wing_fold=0.35,
                 leg_len=0.10, eye=1.0, upperparts=0.55,
                 head_mark="mask", wing_mark="panel", body_mark="streaked",
                 mark_count=5, mark_width=0.24,
                 mat_back="skin_brown", mat_belly="plume_rufous",
                 mat_head="skin_blue", mat_wing="skin_brown",
                 mat_mark="plume_buff", mat_head_mark="plume_white",
                 mat_bill="skin_yellow", mat_eye="plume_crimson",
                 bio_rainforest=1.0,
                 place_abundance=0.15, place_spacing_m=60.0,
                 place_water_max_m=30.0,
                 flock_despawn_m=140.0, flock_size_min=3, flock_size_max=12,
                 flock_spread_m=20.0, flock_perch="shrub",
                 flock_height_min_m=2.0, flock_height_max_m=15.0,
                 flock_flight_share=0.10, flock_per_hectare=0.6)),
    ),
    "great-curassow": (
        "0.90 m - glossy black ground bird with a tight curling crest",
        base(name="great-curassow",
             notes="A TURKEY-SIZED GROUND BIRD FOR THE RAINFOREST FLOOR, which "
                   "the biome had nothing of. Glossy black with a white belly, "
                   "a yellow knob on the bill, and a crest of forward-CURLING "
                   "feathers -- the curl cannot be drawn, so it is a short "
                   "dense crest and the knob plus the size carry it.",
             **b(length_m=0.90, bill_frac=0.045, head_frac=0.100,
                 neck_frac=0.080, body_frac=0.475, tail_frac=0.300,
                 posture_deg=20, body_depth=0.90, body_width=0.80,
                 chest_at=0.34, breast=0.78, rump=0.52, fullness=3.4,
                 section=2.4, belly=0.55, head_size=0.88, neck_up_deg=42,
                 neck_thick=0.56, crest=0.40,
                 bill_depth=0.50, bill_curve=0.20, bill_gape=0.20,
                 tail_shape="rounded", tail_width=0.62, tail_droop=0.35,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.20,
                 wing_aspect=4.6, wing_fold=0.40, wing_thick=2,
                 leg_len=0.13, leg_thick=1.5, eye=1.0, upperparts=0.65,
                 head_mark="none", wing_mark="none", body_mark="none",
                 mat_back="skin_dark", mat_belly="plume_white",
                 mat_head="skin_dark", mat_wing="plume_iridescent",
                 mat_mark="plume_white", mat_head_mark="skin_yellow",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_rainforest=1.0,
                 place_abundance=0.12, place_spacing_m=150.0,
                 flock_despawn_m=180.0, flock_size_min=1, flock_size_max=4,
                 flock_spread_m=25.0, flock_perch="ground",
                 flock_height_min_m=0.5, flock_height_max_m=10.0,
                 flock_flight_share=0.06, flock_per_hectare=0.2)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "land bird specs")
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
