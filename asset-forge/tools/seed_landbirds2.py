"""Author the last of the birds: the rows the biome files still had queued.

WHAT IS LEFT AND WHY IT IS LEFT. `tools/seed_birds.py`, `seed_landbirds.py`,
`seed_shorebirds.py` and `seed_ocean.py` between them cleared a hundred and five
species. What they did not clear is a tail of twenty-two rows that are hard for
one of three reasons, and every one of those reasons is a note in a spec below
rather than a silence:

  * TWELVE ARE UNDER THE 20 cm FLOOR. Storm petrel, corn bunting, meadow pipit,
    quail, wheatear, stonechat, goldcrest, wood thrush, scarlet tanager,
    redstart, snowy plover, hummingbird. Each is authored up and each says so in
    its own `notes` with the arithmetic, which is the only thing that stops the
    next person "correcting" the size and quietly destroying the bird.
  * SEVEN HAVE A FIELD MARK THE GENERATOR CANNOT PAINT. The storm petrel's white
    rump, the wheatear's black tail-T, the pipit's white outer tail feathers,
    the sandwich tern's yellow bill tip, the pileated woodpecker's white neck
    stripes, the redstart's grey wing against a rusty tail, the wild turkey's
    red wattles. `bird` has eight material slots and the tail is not one of them
    -- it
    takes `materials.bird_wing` (`forge/bird.py:1302`) and the legs take
    `materials.bird_bill` (`:1303`). Where that costs the species something, the
    spec says which half was chosen and why.
  * THE GOLDCREST, which `tools/seed_landbirds.py` deliberately LEFT OUT. It is
    here now, at 2.4x life size, and its `notes` carry the argument on both
    sides rather than pretending the earlier decision did not happen.

THE BILL CONTRAST GATE MOVED FIVE COLOURS. `tools/birdprobe.py --read` measures
every marking against the part it sits on and the bird floor is a contrast ratio
of 2.0 (`birdprobe.py:1032`) -- higher than the fish floor of 1.5, because the
brief is "colourful and stylised". Five species below have a black bill in life
and a black bill measures 1.0 against a black head, 1.88 against horn and 1.93
against slate: the storm petrel, the stonechat, the goldcrest, the pileated
woodpecker and the hummingbird's head all had to move. Four of them take
`plume_grey`, which is what `carrion-crow` and `common-raven` already do; the
hummingbird instead moved the ANIMAL, taking its head from the dark
`plume_iridescent` to the bright `plume_lime` so it could keep both a black
needle bill and a crimson gorget. Each says so.

LATTICE. All twenty-two at 1 cm, and that is the measured answer rather than the
default. The largest here is the wild turkey at 1.10 m: at 2 cm it would be 55
voxels, but its smallest identifying feature is the bare head, roughly 8 cm, and
at 2 cm that is four voxels of head carrying a marking that then has nowhere to
go -- so 1 cm, where the head is eight voxels and the white crown patch is three.
The ratites went to 2 cm on the same arithmetic run the other way; see
`seed_landbirds.py`.

    python tools/seed_landbirds2.py
    python tools/seed_landbirds2.py --force

SIZES ARE APPROXIMATE. Every length is the approximate figure from the biome
file it came from, and those files say plainly that their numbers are unsourced
general-knowledge estimates. Nothing here is quoted as measured.
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
    # --- ocean -------------------------------------------------------------
    "wilsons-storm-petrel": (
        "0.24 m - tiny sooty seabird pattering with its feet past its tail",
        base(name="wilsons-storm-petrel",
             notes="AUTHORED AT 0.24 m AGAINST A REAL 18 cm, which is the fix "
                   "`european-robin`, `great-tit`, `common-kingfisher` and "
                   "`barn-swallow` already carry and the ocean file asks for by "
                   "name. Eighteen voxels is under the eighteen-voxel floor "
                   "`birdprobe.py --read` gates on, so at life size this bird "
                   "would ship already flagged. Do not correct it back.\n\n"
                   "THE WHITE RUMP CANNOT BE DRAWN AND IT IS THE FIELD MARK. "
                   "`bird.body_mark` offers barred, streaked, speckled and "
                   "breastband, and none of the four sits on the rump; there is "
                   "no rump slot and no tail slot. So the species is carried "
                   "here by two things that ARE expressible: the feet trailing "
                   "PAST the tail, which is `leg_len` 0.26 against a tail share "
                   "of 0.22, and the pale carpal bar across a sooty upperwing. "
                   "That is a real bird and it is not the whole bird, and the "
                   "gap is recorded rather than papered over.\n\n"
                   "THE BILL IS GREY AND IN LIFE IT IS BLACK. Black on a black "
                   "head measures a contrast ratio of 1.00 against a floor of "
                   "2.0, which is no bill at all; `carrion-crow`, `common-raven` "
                   "and `western-jackdaw` all solved it the same way.",
             **b(length_m=0.24, bill_frac=0.055, head_frac=0.110,
                 neck_frac=0.030, body_frac=0.585, tail_frac=0.220,
                 posture_deg=4, body_depth=0.62, body_width=0.70,
                 chest_at=0.32, breast=0.74, rump=0.40, fullness=3.0,
                 section=2.1, belly=0.50, head_size=1.0, neck_up_deg=10,
                 neck_thick=0.62,
                 bill_depth=0.20, bill_hook=0.30, bill_gape=0.06,
                 tail_shape="square", tail_width=0.46, tail_droop=0.35,
                 pose="flying", wing_shape="pointed", wing_span=2.60,
                 wing_aspect=8.4, wing_sweep=0.30, wing_dihedral=0.10,
                 wing_fold=1.00,
                 leg_len=0.26, leg_thick=1.0, eye=1.0, upperparts=0.60,
                 head_mark="none", wing_mark="bar", body_mark="none",
                 mark_width=0.26,
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="skin_dark", mat_wing="skin_dark",
                 mat_mark="plume_grey", mat_head_mark="plume_grey",
                 mat_bill="plume_grey", mat_eye="plume_white",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.35, place_spacing_m=30.0,
                 flock_despawn_m=200.0, flock_size_min=4, flock_size_max=60,
                 flock_spread_m=90.0, flock_perch="water",
                 flock_height_min_m=0.3, flock_height_max_m=20.0,
                 flock_flight_share=0.90, flock_per_hectare=3.0)),
    ),
    # --- beach -------------------------------------------------------------
    "sandwich-tern": (
        "0.40 m - bigger, shaggier-crested tern with a heavy black bill",
        base(name="sandwich-tern",
             notes="THE YELLOW BILL TIP IS THE ONLY SEPARATOR FROM "
                   "`common-tern` AND IT CANNOT BE DRAWN. `materials.bird_bill` "
                   "is one slot for the whole bill; a two-tone bill needs a "
                   "second slot the kind does not have. So the pair is separated "
                   "here by the two things that ARE expressible and that a "
                   "birder uses at the same time: the shaggy rear crest, "
                   "`crest` 0.55 against the common tern's 0.00, and six "
                   "centimetres of length. If those two do not separate them on "
                   "a contact sheet, that is a real measurement of what the "
                   "generator resolves and it belongs to the owner.\n\n"
                   "The bill is also drawn LONGER and shallower than a common "
                   "tern's -- 0.12 of total length against 0.10 -- which is the "
                   "other honest difference between the two birds.",
             **b(length_m=0.40, bill_frac=0.120, head_frac=0.100,
                 neck_frac=0.040, body_frac=0.400, tail_frac=0.340,
                 posture_deg=6, body_depth=0.54, body_width=0.72,
                 chest_at=0.30, breast=0.72, rump=0.34, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.98, neck_up_deg=14,
                 neck_thick=0.58, crest=0.55,
                 bill_depth=0.15, bill_gape=0.05,
                 tail_shape="forked", tail_width=0.44, tail_fork=0.48,
                 tail_droop=0.40,
                 pose="flying", wing_shape="pointed", wing_span=2.45,
                 wing_aspect=11.0, wing_sweep=0.32, wing_dihedral=0.04,
                 wing_fold=1.05,
                 leg_len=0.055, eye=1.0, upperparts=0.40,
                 head_mark="cap", wing_mark="tip", body_mark="none",
                 mark_width=0.34,
                 mat_back="plume_grey", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_grey",
                 mat_mark="plume_slate", mat_head_mark="skin_dark",
                 mat_bill="skin_dark", mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.6, bio_grassland=0.15,
                 place_abundance=0.35, place_spacing_m=14.0,
                 place_water_max_m=60.0,
                 flock_despawn_m=220.0, flock_size_min=3, flock_size_max=40,
                 flock_spread_m=50.0, flock_perch="ground",
                 flock_height_min_m=2.0, flock_height_max_m=45.0,
                 flock_flight_share=0.72, flock_per_hectare=3.5)),
    ),
    "osprey": (
        "0.60 m - white below, dark above, dark eye stripe, kinked wings",
        base(name="osprey",
             notes="THE ONLY WHITE-BELLIED RAPTOR IN THE LIBRARY, and from "
                   "below that white underside with dark carpal patches is the "
                   "entire identification -- so this spec spends its budget "
                   "there rather than on the back.\n\n"
                   "THE KINK IN THE WING CANNOT BE DRAWN AND IT IS HALF THE "
                   "SPECIES. An osprey holds its inner wing raised and its outer "
                   "wing drooped, which reads as a shallow M, and "
                   "`bird.wing_dihedral` is ONE angle for the whole wing -- it "
                   "can make a harrier's V (0.42) or a droop, but not both on "
                   "one wing. This is authored at 0.20, the raised inner half, "
                   "with the sweep pushed to 0.42 so the wrist is visibly "
                   "forward of the hand. It gets partway there. A per-panel "
                   "dihedral would be a generator change and is not one this "
                   "spec should make.",
             **b(length_m=0.60, bill_frac=0.045, head_frac=0.105,
                 neck_frac=0.045, body_frac=0.415, tail_frac=0.390,
                 posture_deg=4, body_depth=0.62, body_width=0.74,
                 chest_at=0.32, breast=0.76, rump=0.44, fullness=3.1,
                 section=2.2, belly=0.50, head_size=1.0, neck_up_deg=12,
                 neck_thick=0.58,
                 bill_depth=0.50, bill_hook=0.85, bill_gape=0.16,
                 tail_shape="square", tail_width=0.56, tail_droop=0.40,
                 tail_thick=2,
                 pose="flying", wing_shape="slotted", wing_span=2.55,
                 wing_aspect=7.8, wing_slots=4, wing_sweep=0.42,
                 wing_dihedral=0.20, wing_thick=2, wing_fold=0.85,
                 leg_len=0.12, leg_thick=1.5, eye=1.0, upperparts=0.62,
                 head_mark="mask", wing_mark="panel", body_mark="none",
                 mark_width=0.26,
                 mat_back="skin_dark", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="skin_dark",
                 mat_bill="skin_dark", mat_eye="skin_yellow",
                 bio_beach=1.0, bio_ocean=0.3, bio_temperate_forest=0.35,
                 bio_grassland=0.3, bio_rainforest=0.2,
                 place_abundance=0.08, place_spacing_m=800.0,
                 place_water_max_m=250.0,
                 flock_despawn_m=400.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=100.0, flock_perch="waterside",
                 flock_height_min_m=10.0, flock_height_max_m=200.0,
                 flock_flight_share=0.70, flock_per_hectare=0.05)),
    ),
    "snowy-plover": (
        "0.22 m - very pale small plover on dry sand above the tideline",
        base(name="snowy-plover",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 16 cm, which is what the "
                   "beach file asks for by name for all three of its small "
                   "waders and what `sanderling` and `ringed-plover` already "
                   "do. Sixteen voxels is under the probe's floor of eighteen. "
                   "Do not correct it back.\n\n"
                   "THE PARTIAL NECK PATCHES ARE DRAWN AS A FACE MASK, NOT AS A "
                   "BREASTBAND, and that is the whole point of the row: this "
                   "species is the plover WITHOUT the full dark collar that "
                   "`ringed-plover` has, and `bird.body_mark` breastband draws "
                   "the complete band. Drawing one here would erase the only "
                   "difference between the two birds. The dark forecrown bar "
                   "and ear patch that the species does have go in `head_mark` "
                   "mask instead, and the neck patches themselves are simply "
                   "not drawn.\n\n"
                   "IT IS ALSO THE PALEST WADER HERE -- buff back against the "
                   "ringed plover's brown -- which on dry sand is the field "
                   "impression the row is actually describing.",
             **b(length_m=0.22, bill_frac=0.065, head_frac=0.145,
                 neck_frac=0.040, body_frac=0.495, tail_frac=0.255,
                 posture_deg=12, body_depth=0.74, body_width=0.74,
                 chest_at=0.32, breast=0.70, rump=0.42, fullness=3.0,
                 section=2.1, belly=0.50, head_size=1.15, neck_up_deg=24,
                 neck_thick=0.66,
                 bill_depth=0.22, bill_gape=0.08,
                 tail_shape="square", tail_width=0.58, tail_droop=0.30,
                 pose="perched", wing_shape="pointed", wing_span=1.85,
                 wing_aspect=7.6, wing_fold=0.85,
                 leg_len=0.13, eye=1.0, upperparts=0.50,
                 head_mark="mask", wing_mark="none", body_mark="none",
                 mark_width=0.34,
                 mat_back="plume_buff", mat_belly="plume_white",
                 mat_head="plume_white", mat_wing="plume_buff",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="skin_dark", mat_eye="skin_dark",
                 bio_beach=1.0, bio_desert=0.2,
                 place_abundance=0.4, place_spacing_m=18.0,
                 place_water_max_m=40.0,
                 flock_despawn_m=90.0, flock_size_min=2, flock_size_max=10,
                 flock_spread_m=25.0, flock_perch="ground",
                 flock_height_min_m=0.5, flock_height_max_m=12.0,
                 flock_flight_share=0.18, flock_per_hectare=1.8)),
    ),
    # --- grassland ---------------------------------------------------------
    "corn-bunting": (
        "0.24 m - fat, dull, streaky brown, and that IS the species",
        base(name="corn-bunting",
             notes="AUTHORED AT 0.24 m AGAINST A REAL 18 cm. A perched songbird "
                   "sits at 34-38 degrees nose-up, so it loses length to the "
                   "projection before the probe ever measures it; the four birds "
                   "already in the library at 20-26 cm against real lengths of "
                   "14-19 are the same arithmetic. Do not correct it back.\n\n"
                   "THE BIRD WITH NO FIELD MARK, deliberately. Its neighbour "
                   "`yellowhammer` is the same family at the same size and is a "
                   "block of lemon; this one has streaks on buff and nothing "
                   "else, and the pair is the library's cheapest test of whether "
                   "a plain species is worth authoring at all. What separates it "
                   "is BULK: `body_depth` 0.95 and a tail share of 0.30 against "
                   "the yellowhammer's 0.38, which is a fat bunting against a "
                   "long-tailed one.",
             **song(length_m=0.24, bill_frac=0.070, head_frac=0.140,
                    neck_frac=0.030, body_frac=0.460, tail_frac=0.300,
                    posture_deg=34, body_depth=0.95, body_width=0.72,
                    fullness=3.6, bill_depth=0.54, bill_gape=0.15,
                    tail_shape="square", tail_width=0.46, tail_droop=0.48,
                    wing_span=1.60, wing_aspect=5.2, wing_fold=0.40,
                    upperparts=0.54,
                    head_mark="none", wing_mark="none", body_mark="streaked",
                    mark_count=7, mark_width=0.16,
                    mat_back="plume_buff", mat_belly="plume_buff",
                    mat_head="plume_buff", mat_wing="skin_brown",
                    mat_mark="skin_dark", mat_head_mark="skin_dark",
                    mat_bill="beak_horn", mat_eye="skin_dark",
                    bio_grassland=1.0, bio_savanna=0.2,
                    place_abundance=0.5, place_spacing_m=20.0,
                    flock_despawn_m=70.0, flock_size_min=1, flock_size_max=15,
                    flock_spread_m=25.0, flock_perch="shrub",
                    flock_height_min_m=0.5, flock_height_max_m=10.0,
                    flock_flight_share=0.22, flock_per_hectare=3.5)),
    ),
    "meadow-pipit": (
        "0.24 m - slim streaky brown, thin bill, walks rather than hops",
        base(name="meadow-pipit",
             notes="AUTHORED AT 0.24 m AGAINST A REAL 15 cm, which is a 1.6x "
                   "enlargement -- smaller than the goldfinch's already in the "
                   "library at 0.22 against 13 cm. Do not correct it back.\n\n"
                   "THE WHITE OUTER TAIL FEATHERS CANNOT BE DRAWN, and they are "
                   "the one thing that separates this from every other small "
                   "streaky brown bird in a field. The tail takes "
                   "`materials.bird_wing` (`forge/bird.py:1302`) -- it has no "
                   "slot of its own and no marking region -- so an outer-feather "
                   "flash is not expressible at all. What is left is the SHAPE: "
                   "a pipit is a thin-billed slim bird where a bunting is a "
                   "thick-billed fat one, so `bill_depth` is 0.26 against the "
                   "corn bunting's 0.54 beside it, and that pair is the whole "
                   "separation this spec is trying to buy.",
             **song(length_m=0.24, bill_frac=0.075, head_frac=0.135,
                    neck_frac=0.030, body_frac=0.400, tail_frac=0.360,
                    posture_deg=32, body_depth=0.78, body_width=0.62,
                    fullness=3.2, bill_depth=0.26, bill_gape=0.10,
                    tail_shape="notched", tail_width=0.40, tail_fork=0.18,
                    tail_droop=0.50, leg_len=0.14,
                    wing_span=1.70, wing_aspect=6.0, wing_fold=0.45,
                    upperparts=0.52,
                    head_mark="none", wing_mark="none", body_mark="streaked",
                    mark_count=8, mark_width=0.14,
                    mat_back="plume_buff", mat_belly="plume_white",
                    mat_head="plume_buff", mat_wing="skin_brown",
                    mat_mark="skin_dark", mat_head_mark="skin_dark",
                    mat_bill="skin_dark", mat_eye="skin_dark",
                    bio_grassland=1.0, bio_tundra_alpine=0.4, bio_taiga=0.3,
                    bio_beach=0.2,
                    place_abundance=0.6, place_spacing_m=16.0,
                    flock_despawn_m=70.0, flock_size_min=1, flock_size_max=20,
                    flock_spread_m=30.0, flock_perch="ground",
                    flock_height_min_m=0.5, flock_height_max_m=25.0,
                    flock_flight_share=0.30, flock_per_hectare=5.0)),
    ),
    "common-quail": (
        "0.24 m - a round buff ball that is almost never off the ground",
        base(name="common-quail",
             notes="AUTHORED AT 0.24 m AGAINST A REAL 18 cm.\n\n"
                   "THE ROUNDEST BIRD IN THE LIBRARY: `body_depth` 0.98 on a "
                   "tail share of 0.14, which beats `grey-partridge` at 0.92 on "
                   "0.20 and is the point of having both. A quail is a partridge "
                   "with the tail taken away and the body left, and at "
                   "twenty-four voxels that ratio is the entire species.\n\n"
                   "THE DARK THROAT ANCHOR RATHER THAN AN EYEBROW. A pale "
                   "supercilium on a buff head measures a contrast ratio of "
                   "1.91 against the bird floor of 2.0 -- faint by measurement, "
                   "not by opinion -- so the head marking is the dark throat "
                   "the cock actually carries, which measures 6.36 on the same "
                   "head.",
             **b(length_m=0.24, bill_frac=0.045, head_frac=0.125,
                 neck_frac=0.035, body_frac=0.655, tail_frac=0.140,
                 posture_deg=12, body_depth=0.98, body_width=0.88,
                 chest_at=0.32, breast=0.82, rump=0.56, fullness=3.6,
                 section=2.5, belly=0.56, head_size=0.88, neck_up_deg=26,
                 neck_thick=0.66,
                 bill_depth=0.42, bill_curve=0.20, bill_gape=0.18,
                 tail_shape="rounded", tail_width=0.50, tail_droop=0.28,
                 pose="perched", wing_shape="elliptical", wing_span=1.35,
                 wing_aspect=4.8, wing_fold=0.42,
                 leg_len=0.085, eye=1.0, upperparts=0.52,
                 head_mark="throat", wing_mark="none", body_mark="streaked",
                 mark_count=7, mark_width=0.16,
                 mat_back="plume_buff", mat_belly="plume_buff",
                 mat_head="plume_buff", mat_wing="skin_brown",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="plume_slate", mat_eye="skin_dark",
                 bio_grassland=1.0, bio_savanna=0.3,
                 place_abundance=0.3, place_spacing_m=35.0,
                 flock_despawn_m=70.0, flock_size_min=1, flock_size_max=6,
                 flock_spread_m=12.0, flock_perch="ground",
                 flock_height_min_m=0.3, flock_height_max_m=6.0,
                 flock_flight_share=0.03, flock_per_hectare=1.2)),
    ),
    "great-bustard": (
        "1.05 m - a barrel on long legs, and the heaviest bird that flies",
        base(name="great-bustard",
             notes="THE THICKEST NECK ON A LONG-NECKED BIRD IN THE LIBRARY. A "
                   "crane and a stork carry a neck share of 0.19-0.27 at "
                   "`neck_thick` 0.26-0.34; this one carries 0.20 at 0.52, which "
                   "is a neck twice as thick for the same length, and that is "
                   "exactly what makes a bustard look heavy where a crane looks "
                   "drawn out. It stands on the same 0.26 legs.\n\n"
                   "THE BIGGEST SIZE DIMORPHISM OF ANY BIRD HERE. `sex_length` "
                   "is 1.30, at the top of the 0.70-1.45 range the parameter "
                   "allows and past `western-capercaillie`'s 1.20. The ratio is "
                   "an approximation of a general-knowledge figure, not a "
                   "measurement, and it is here because a bustard hen beside a "
                   "cock is the clearest dimorphism the library can show without "
                   "a repaint.\n\n"
                   "THE DISPLAY IS NOT DRAWN. A displaying male turns himself "
                   "inside out into a white ball, which is a POSE, and "
                   "`bird.pose` offers perched and flying. `sex_alt_head_mark` "
                   "takes the white whiskers off the hen and that is as far as "
                   "this goes.",
             **b(length_m=1.05, bill_frac=0.045, head_frac=0.070,
                 neck_frac=0.200, body_frac=0.455, tail_frac=0.230,
                 posture_deg=8, body_depth=0.90, body_width=0.86,
                 chest_at=0.36, breast=0.78, rump=0.58, fullness=3.0,
                 section=2.4, belly=0.54, head_size=0.80, neck_up_deg=54,
                 neck_thick=0.52,
                 bill_depth=0.34, bill_curve=0.06, bill_gape=0.14,
                 tail_shape="rounded", tail_width=0.70, tail_droop=0.22,
                 tail_thick=2,
                 pose="perched", wing_shape="slotted", wing_span=2.05,
                 wing_aspect=6.0, wing_slots=5, wing_thick=3, wing_fold=0.70,
                 leg_len=0.26, leg_thick=2.5, eye=1.0, upperparts=0.56,
                 head_mark="throat", wing_mark="tip", body_mark="barred",
                 mark_count=7, mark_width=0.24,
                 sex_length=1.30, sex_plumage="male",
                 sex_alt_head_mark="none",
                 mat_back="plume_rufous", mat_belly="plume_white",
                 mat_head="plume_grey", mat_wing="plume_buff",
                 mat_mark="skin_dark", mat_head_mark="plume_white",
                 mat_bill="skin_dark", mat_eye="skin_dark",
                 bio_grassland=1.0, bio_savanna=0.3, bio_desert=0.2,
                 place_abundance=0.06, place_spacing_m=400.0,
                 flock_despawn_m=400.0, flock_size_min=1, flock_size_max=8,
                 flock_spread_m=70.0, flock_perch="ground",
                 flock_height_min_m=0.0, flock_height_max_m=60.0,
                 flock_flight_share=0.10, flock_per_hectare=0.06)),
    ),
    "northern-wheatear": (
        "0.24 m - grey back, black mask and wings, bobbing on a stone",
        base(name="northern-wheatear",
             notes="AUTHORED AT 0.24 m AGAINST A REAL 16 cm.\n\n"
                   "THE WHITE RUMP AND THE BLACK TAIL-T ARE NOT DRAWN, and "
                   "between them they are how anybody actually identifies this "
                   "bird -- it flies away and the rump is the whole record of "
                   "what it was. The tail takes `materials.bird_wing` "
                   "(`forge/bird.py:1302`) and there is no rump region in "
                   "`bird.body_mark`, so neither half of that pattern has "
                   "anywhere to live. What is authored instead is the bird AT "
                   "REST: a clean pale grey back against solid black wings and a "
                   "hard black mask, which is three flat blocks and reads at "
                   "twenty voxels.",
             **b(length_m=0.24, bill_frac=0.070, head_frac=0.140,
                 neck_frac=0.030, body_frac=0.470, tail_frac=0.290,
                 posture_deg=30, body_depth=0.82, body_width=0.66,
                 chest_at=0.32, breast=0.76, rump=0.44, fullness=3.4,
                 section=2.1, belly=0.52, head_size=1.05, neck_up_deg=34,
                 neck_thick=0.54,
                 bill_depth=0.28, bill_gape=0.12,
                 tail_shape="square", tail_width=0.50, tail_droop=0.42,
                 pose="perched", wing_shape="pointed", wing_span=1.85,
                 wing_aspect=6.6, wing_fold=0.55,
                 leg_len=0.15, eye=1.0, upperparts=0.56,
                 head_mark="mask", wing_mark="none", body_mark="none",
                 mark_width=0.36,
                 mat_back="plume_grey", mat_belly="plume_buff",
                 mat_head="plume_grey", mat_wing="skin_dark",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="skin_dark", mat_eye="skin_dark",
                 bio_grassland=0.9, bio_tundra_alpine=0.7, bio_bare_rock=0.5,
                 bio_beach=0.2,
                 place_abundance=0.4, place_spacing_m=25.0,
                 place_slope_max_pct=70.0,
                 flock_despawn_m=80.0, flock_size_min=1, flock_size_max=4,
                 flock_spread_m=20.0, flock_perch="ground",
                 flock_height_min_m=0.3, flock_height_max_m=12.0,
                 flock_flight_share=0.25, flock_per_hectare=2.0)),
    ),
    "european-stonechat": (
        "0.24 m - upright little ball: black head, white collar, orange breast",
        base(name="european-stonechat",
             notes="AUTHORED AT 0.24 m AGAINST A REAL 13 cm, which ties "
                   "`european-goldfinch` for the largest enlargement in the land "
                   "birds at 1.85x. It is worth it for the same reason: this "
                   "species is THREE blocks of colour stacked vertically on a "
                   "bird the size of a matchbox, and at thirteen voxels there is "
                   "one block. Do not correct it back.\n\n"
                   "THE POSTURE IS HALF THE SPECIES. A stonechat sits bolt "
                   "upright on the top of a bush, so `posture_deg` is 46 -- the "
                   "steepest perched songbird here -- on a very short tail. That "
                   "outline is what makes it read against the wheatear beside "
                   "it, which is the same size and sits horizontally.\n\n"
                   "THE BILL IS GREY AND IN LIFE IT IS BLACK. On a black head a "
                   "black bill measures 1.00 against the floor of 2.0, horn "
                   "measures 1.88 and slate 1.93; grey measures 4.75 and is what "
                   "`carrion-crow` already does.",
             **song(length_m=0.24, bill_frac=0.065, head_frac=0.150,
                    neck_frac=0.025, body_frac=0.480, tail_frac=0.280,
                    posture_deg=46, body_depth=0.92, body_width=0.70,
                    fullness=3.6, bill_depth=0.34, bill_gape=0.12,
                    tail_shape="square", tail_width=0.44, tail_droop=0.46,
                    wing_span=1.55, wing_aspect=5.0, wing_fold=0.38,
                    upperparts=0.58,
                    head_mark="collar", wing_mark="none", body_mark="none",
                    mark_width=0.30,
                    mat_back="skin_brown", mat_belly="skin_orange",
                    mat_head="skin_dark", mat_wing="skin_dark",
                    mat_mark="plume_white", mat_head_mark="plume_white",
                    mat_bill="plume_grey", mat_eye="skin_dark",
                    bio_grassland=1.0, bio_beach=0.3, bio_tundra_alpine=0.25,
                    place_abundance=0.5, place_spacing_m=22.0,
                    flock_despawn_m=70.0, flock_size_min=1, flock_size_max=4,
                    flock_spread_m=18.0, flock_perch="shrub",
                    flock_height_min_m=0.4, flock_height_max_m=8.0,
                    flock_flight_share=0.20, flock_per_hectare=2.5)),
    ),
    "burrowing-owl": (
        "0.23 m - a small spotted owl standing tall on absurd legs",
        base(name="burrowing-owl",
             notes="THE LONGEST LEGS ON AN OWL, AND THAT IS THE WHOLE ROW. "
                   "`leg_len` is 0.28 of total length where `tawny-owl` is 0.08 "
                   "and `little-owl` is 0.10 -- three times either -- and an owl "
                   "standing at its own height on open ground is a silhouette "
                   "the library has nothing else like. It also has NO EAR TUFTS, "
                   "so `crest` stays at 0.\n\n"
                   "THE BILL IS PALE HORN-BUFF BECAUSE HORN DOES NOT WORK. "
                   "`beak_horn` on a brown head measures a contrast ratio of "
                   "1.03 -- effectively no bill -- against a floor of 2.0; "
                   "`plume_buff` measures 3.48 and is the pale yellowish bill "
                   "the species has anyway. The legs take the same slot "
                   "(`forge/bird.py:1303`), so they come out pale too, which is "
                   "right for this bird and is luck rather than design.",
             **b(length_m=0.23, bill_frac=0.040, head_frac=0.170,
                 neck_frac=0.020, body_frac=0.505, tail_frac=0.265,
                 posture_deg=38, body_depth=0.78, body_width=0.72,
                 chest_at=0.30, breast=0.78, rump=0.48, fullness=3.2,
                 section=2.3, belly=0.52, head_size=1.18, neck_up_deg=40,
                 neck_thick=0.95,
                 bill_depth=0.46, bill_hook=0.60, bill_gape=0.16,
                 tail_shape="square", tail_width=0.50, tail_droop=0.30,
                 pose="perched", wing_shape="elliptical", wing_span=2.05,
                 wing_aspect=5.2, wing_fold=0.50,
                 leg_len=0.28, leg_thick=1.5, eye=2.0, upperparts=0.55,
                 head_mark="supercilium", wing_mark="none",
                 body_mark="speckled", mark_width=0.12, mark_strength=0.34,
                 mat_back="skin_brown", mat_belly="plume_buff",
                 mat_head="skin_brown", mat_wing="skin_brown",
                 mat_mark="skin_dark", mat_head_mark="plume_white",
                 mat_bill="plume_buff", mat_eye="skin_yellow",
                 bio_grassland=1.0, bio_desert=0.5, bio_savanna=0.3,
                 place_abundance=0.18, place_spacing_m=120.0,
                 flock_despawn_m=90.0, flock_size_min=1, flock_size_max=4,
                 flock_spread_m=15.0, flock_perch="ground",
                 flock_height_min_m=0.0, flock_height_max_m=6.0,
                 flock_flight_share=0.10, flock_per_hectare=0.3)),
    ),
    "western-meadowlark": (
        "0.23 m - brown above, brilliant yellow below, black V on the chest",
        base(name="western-meadowlark",
             notes="THE ONLY BLACK-ON-YELLOW BREASTBAND IN THE LIBRARY, and it "
                   "measures a contrast ratio of 7.64 -- one of the highest "
                   "here. That V is the species from any angle in front and it "
                   "costs one marking.\n\n"
                   "IT IS THE COUNTER-EXAMPLE TO `corn-bunting` IN THE SAME "
                   "FILE. Same family of habitat, near enough the same size, and "
                   "the two are authored on opposite arguments: the bunting has "
                   "no mark at all and has to live on bulk, this one has one "
                   "hard mark on a saturated ground and needs no shape argument "
                   "whatever. If the pair does not read as two species, the "
                   "bunting is the one that failed.\n\n"
                   "The long straight sharp bill is a third of the head and is "
                   "drawn in slate, which is the colour it is and also the only "
                   "thing that measures over the floor on a buff head.",
             **b(length_m=0.23, bill_frac=0.105, head_frac=0.125,
                 neck_frac=0.030, body_frac=0.480, tail_frac=0.260,
                 posture_deg=22, body_depth=0.88, body_width=0.74,
                 chest_at=0.32, breast=0.78, rump=0.48, fullness=3.4,
                 section=2.2, belly=0.53, head_size=0.98, neck_up_deg=30,
                 neck_thick=0.56,
                 bill_depth=0.24, bill_gape=0.10,
                 tail_shape="square", tail_width=0.50, tail_droop=0.35,
                 pose="perched", wing_shape="elliptical", wing_span=1.60,
                 wing_aspect=5.2, wing_fold=0.45,
                 leg_len=0.13, eye=1.0, upperparts=0.54,
                 head_mark="supercilium", wing_mark="none",
                 body_mark="breastband", mark_width=0.22,
                 mat_back="plume_buff", mat_belly="skin_yellow",
                 mat_head="plume_buff", mat_wing="skin_brown",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="plume_slate", mat_eye="skin_dark",
                 bio_grassland=1.0, bio_savanna=0.25, bio_desert=0.15,
                 place_abundance=0.5, place_spacing_m=25.0,
                 flock_despawn_m=90.0, flock_size_min=1, flock_size_max=10,
                 flock_spread_m=30.0, flock_perch="ground",
                 flock_height_min_m=0.3, flock_height_max_m=15.0,
                 flock_flight_share=0.25, flock_per_hectare=2.5)),
    ),
    # --- temperate forest --------------------------------------------------
    "goldcrest": (
        "0.22 m - olive-green scrap with a bright yellow crown stripe",
        base(name="goldcrest",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 9 cm, AND "
                   "`tools/seed_landbirds.py` DELIBERATELY LEFT THIS SPECIES "
                   "OUT. Both halves of that belong here.\n\n"
                   "THE ARGUMENT AGAINST, which is the temperate-forest file's "
                   "and is not wrong: 2.4x life size is the largest enlargement "
                   "in the library, and a goldcrest the size of a robin standing "
                   "next to a robin is not a goldcrest, it is a robin.\n\n"
                   "THE ARGUMENT FOR, which is why it is here: nine voxels is "
                   "half the eighteen the probe gates on and a third of the "
                   "20-90 the generator is drawn to read at "
                   "(`kinds.py:129-134`), so the choice is not between a small "
                   "goldcrest and a big one -- it is between a big one and none. "
                   "And what separates it from the robin beside it is not size, "
                   "it is a saturated yellow stripe down the middle of an "
                   "olive-green crown, which measures a contrast ratio of 3.91 "
                   "and is a marking no other bird in the library carries. On "
                   "that reading the enlargement costs the scale relationship "
                   "and keeps the species.\n\n"
                   "THE BILL IS GREY AND IN LIFE IT IS A FINE BLACK NEEDLE, for "
                   "the usual measured reason: black on an olive head is 1.96 "
                   "against a floor of 2.0, and grey is 2.43.",
             **song(length_m=0.22, bill_frac=0.060, head_frac=0.165,
                    neck_frac=0.020, body_frac=0.475, tail_frac=0.280,
                    posture_deg=36, body_depth=0.92, body_width=0.70,
                    fullness=3.6, head_size=1.12,
                    bill_depth=0.16, bill_gape=0.06,
                    tail_shape="notched", tail_width=0.42, tail_fork=0.20,
                    tail_droop=0.45, leg_len=0.10,
                    wing_span=1.55, wing_aspect=5.0, wing_fold=0.36,
                    upperparts=0.52,
                    head_mark="cap", wing_mark="doublebar", body_mark="none",
                    mark_width=0.20,
                    mat_back="skin_olive", mat_belly="plume_buff",
                    mat_head="skin_olive", mat_wing="skin_olive",
                    mat_mark="plume_white", mat_head_mark="skin_yellow",
                    mat_bill="plume_grey", mat_eye="skin_dark",
                    bio_temperate_forest=1.0, bio_taiga=0.7,
                    place_abundance=0.45, place_spacing_m=25.0,
                    flock_despawn_m=60.0, flock_size_min=2, flock_size_max=8,
                    flock_spread_m=12.0, flock_perch="canopy",
                    flock_height_min_m=2.0, flock_height_max_m=22.0,
                    flock_flight_share=0.20, flock_per_hectare=3.0)),
    ),
    "wild-turkey": (
        "1.10 m - iridescent bronze-black bulk on a bare blue head",
        base(name="wild-turkey",
             notes="THE HEAVIEST GROUND BIRD THAT IS NOT A RATITE, and the one "
                   "the tail does the work for: `tail_frac` 0.30 on "
                   "`tail_width` 1.10 at `tail_droop` -0.10, which carries the "
                   "fan UP behind the body the way `western-capercaillie` does "
                   "at -0.15. Nothing else in the library has a tail wider than "
                   "the bird.\n\n"
                   "THE HEAD IS BLUE AND WHITE AND IN LIFE IT ALSO HAS RED "
                   "WATTLES, WHICH ARE NOT DRAWN. Crimson on blue measures a "
                   "contrast ratio of 1.21 against the bird floor of 2.0, so a "
                   "red wattle on a blue head is a shape with no edge. What is "
                   "drawn is the OTHER half of the same real animal: a "
                   "displaying gobbler's head goes white and blue, and white on "
                   "blue measures 5.78. This is a colour moved rather than an "
                   "animal moved, and the reason is a number.\n\n"
                   "AUTHORED AT 1 cm, NOT 2. At 2 cm the bird is 55 voxels and "
                   "the bare head -- roughly 8 cm, and the only part of this "
                   "species that is not brown-black -- is four voxels with a "
                   "marking inside it. At 1 cm the head is eight voxels and the "
                   "white crown is three, which is the house rule met rather "
                   "than approximated.",
             **b(length_m=1.10, bill_frac=0.035, head_frac=0.075,
                 neck_frac=0.160, body_frac=0.430, tail_frac=0.300,
                 posture_deg=12, body_depth=0.96, body_width=0.88,
                 chest_at=0.34, breast=0.80, rump=0.60, fullness=3.4,
                 section=2.5, belly=0.55, head_size=0.68, neck_up_deg=48,
                 neck_thick=0.40,
                 bill_depth=0.44, bill_curve=0.22, bill_gape=0.20,
                 tail_shape="rounded", tail_width=1.10, tail_droop=-0.10,
                 tail_thick=3,
                 pose="perched", wing_shape="elliptical", wing_span=1.35,
                 wing_aspect=4.6, wing_fold=0.45, wing_thick=2,
                 leg_len=0.20, leg_thick=2.5, eye=1.0, upperparts=0.62,
                 head_mark="throat", wing_mark="bar", body_mark="none",
                 mark_width=0.34,
                 mat_back="plume_iridescent", mat_belly="plume_iridescent",
                 mat_head="skin_blue", mat_wing="skin_brown",
                 mat_mark="plume_white", mat_head_mark="plume_white",
                 mat_bill="plume_buff", mat_eye="skin_dark",
                 bio_temperate_forest=1.0, bio_grassland=0.35,
                 place_abundance=0.10, place_spacing_m=200.0,
                 flock_despawn_m=300.0, flock_size_min=2, flock_size_max=14,
                 flock_spread_m=40.0, flock_perch="ground",
                 flock_height_min_m=0.0, flock_height_max_m=12.0,
                 flock_flight_share=0.05, flock_per_hectare=0.2)),
    ),
    "pileated-woodpecker": (
        "0.45 m - crow-sized black woodpecker under a flaming red crest",
        base(name="pileated-woodpecker",
             notes="THE BIGGEST CREST IN THE LIBRARY AT 0.90, level with "
                   "`eurasian-hoopoe` and on a bird twice the size, so in "
                   "voxels it is the largest crest here by a distance. It is "
                   "also the only crest that is a DIFFERENT COLOUR from the head "
                   "it sits on: `forge/bird.py:1301` paints the crest with "
                   "`materials.bird_head`, and `_head_mark` then runs over head "
                   "AND crest together (`:1335`), so a `cap` marking is the only "
                   "way to get a red crest on a black bird, and it works.\n\n"
                   "CRIMSON ON BLACK MEASURES 2.53 against a floor of 2.0. That "
                   "is over the line and not by much, and it is recorded here "
                   "because it is the tightest passing contrast in this file: if "
                   "the floor ever moves, this is the species that fails first.\n\n"
                   "THE WHITE NECK STRIPES ARE NOT DRAWN. `bird.head_mark` is "
                   "one marking, the crest took it, and the crest is the "
                   "species. One marking and never two.\n\n"
                   "Posed on a trunk at 68 degrees with the tail pressed to the "
                   "bark as a prop, which is what `great-spotted-woodpecker` "
                   "already does and is the only right pose for the family.",
             **b(length_m=0.45, bill_frac=0.100, head_frac=0.130,
                 neck_frac=0.055, body_frac=0.395, tail_frac=0.320,
                 posture_deg=68, body_depth=0.74, body_width=0.66,
                 chest_at=0.30, breast=0.76, rump=0.44, fullness=3.2,
                 section=2.2, belly=0.50, head_size=1.02, neck_up_deg=-4,
                 neck_thick=0.62, crest=0.90,
                 bill_depth=0.30, bill_gape=0.16,
                 tail_shape="pointed", tail_width=0.46, tail_droop=0.82,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.75,
                 wing_aspect=5.2, wing_fold=0.42,
                 leg_len=0.09, eye=1.0, upperparts=0.62,
                 head_mark="cap", wing_mark="panel", body_mark="none",
                 mark_width=0.30,
                 mat_back="skin_dark", mat_belly="skin_dark",
                 mat_head="skin_dark", mat_wing="skin_dark",
                 mat_mark="plume_white", mat_head_mark="plume_crimson",
                 mat_bill="plume_grey", mat_eye="skin_yellow",
                 bio_temperate_forest=1.0, bio_taiga=0.25,
                 place_abundance=0.25, place_spacing_m=90.0,
                 flock_despawn_m=110.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=8.0, flock_perch="canopy",
                 flock_height_min_m=2.0, flock_height_max_m=25.0,
                 flock_flight_share=0.18, flock_per_hectare=0.4)),
    ),
    "wood-thrush": (
        "0.24 m - rusty head over a white breast of big round black spots",
        base(name="wood-thrush",
             notes="AUTHORED AT 0.24 m AGAINST A REAL 20 cm, which is the "
                   "smallest enlargement in this file and is there only to clear "
                   "the projection: a songbird authored at 34 degrees nose-up "
                   "puts 20 cm onto about sixteen voxels of length, under the "
                   "probe's floor of eighteen.\n\n"
                   "IT IS AUTHORED AGAINST `song-thrush`, WHICH IS THE SAME BIRD "
                   "AT THE SAME SIZE IN THE SAME LIBRARY. Two numbers separate "
                   "them and both are deliberate: `mark_width` 0.15 against the "
                   "song thrush's 0.09, which is a big round blot against a fine "
                   "arrowhead, and a WHITE ground rather than a buff one, which "
                   "takes the spot contrast from 6.36 to 12.14. A wood thrush is "
                   "the boldly spotted thrush; if the pair does not read as two "
                   "species on a contact sheet, that is a measurement of what "
                   "`mark_width` buys at twenty voxels and it belongs to the "
                   "owner.",
             **song(length_m=0.24, bill_frac=0.085, head_frac=0.145,
                    neck_frac=0.035, body_frac=0.410, tail_frac=0.325,
                    posture_deg=34, body_depth=0.86, body_width=0.70,
                    fullness=3.4, bill_depth=0.28, bill_gape=0.13,
                    tail_shape="square", tail_width=0.44, tail_droop=0.48,
                    leg_len=0.14,
                    wing_span=1.60, wing_aspect=5.4, wing_fold=0.40,
                    upperparts=0.56,
                    head_mark="none", wing_mark="none", body_mark="speckled",
                    mark_width=0.15, mark_strength=0.38,
                    mat_back="plume_rufous", mat_belly="plume_white",
                    mat_head="plume_rufous", mat_wing="plume_rufous",
                    mat_mark="skin_dark", mat_head_mark="skin_dark",
                    mat_bill="skin_dark", mat_eye="skin_dark",
                    bio_temperate_forest=1.0, bio_rainforest=0.2,
                    place_abundance=0.45, place_spacing_m=30.0,
                    flock_despawn_m=70.0, flock_size_min=1, flock_size_max=2,
                    flock_spread_m=12.0, flock_perch="canopy",
                    flock_height_min_m=1.0, flock_height_max_m=16.0,
                    flock_flight_share=0.20, flock_per_hectare=2.0)),
    ),
    "scarlet-tanager": (
        "0.24 m - unbroken scarlet against solid black wings and tail",
        base(name="scarlet-tanager",
             notes="AUTHORED AT 0.24 m AGAINST A REAL 17 cm.\n\n"
                   "TWO FLAT COLOURS AND NO MARKING AT ALL, which makes it the "
                   "only bird in the library with `head_mark`, `wing_mark` and "
                   "`body_mark` all set to none on a species that is unmistakable "
                   "anyway. It is the strongest single argument in the set that "
                   "colour beats pattern at twenty-four voxels.\n\n"
                   "THE BLACK TAIL IS FREE, AND THAT IS LUCK. The tail takes "
                   "`materials.bird_wing` (`forge/bird.py:1302`) rather than "
                   "having a slot of its own, which costs the wheatear and the "
                   "redstart in this same file dearly -- and here it is exactly "
                   "right, because this bird's wings and tail are the same solid "
                   "black.\n\n"
                   "THE BILL IS PALE BUFF BECAUSE NOTHING DARKER MEASURES. On a "
                   "crimson head, horn is 1.35 and grey is 1.88 against a floor "
                   "of 2.0; buff is 2.51. A scarlet tanager's bill is a pale "
                   "greyish ivory in life, so the measurement and the animal "
                   "agree, which is not always how this goes.",
             **song(length_m=0.24, bill_frac=0.070, head_frac=0.145,
                    neck_frac=0.030, body_frac=0.435, tail_frac=0.320,
                    posture_deg=32, body_depth=0.86, body_width=0.68,
                    fullness=3.4, bill_depth=0.50, bill_gape=0.14,
                    tail_shape="notched", tail_width=0.44, tail_fork=0.18,
                    tail_droop=0.48,
                    wing_span=1.70, wing_aspect=5.8, wing_fold=0.42,
                    upperparts=0.50,
                    head_mark="none", wing_mark="none", body_mark="none",
                    mat_back="plume_crimson", mat_belly="plume_crimson",
                    mat_head="plume_crimson", mat_wing="skin_dark",
                    mat_mark="skin_dark", mat_head_mark="skin_dark",
                    mat_bill="plume_buff", mat_eye="skin_dark",
                    bio_temperate_forest=1.0, bio_rainforest=0.25,
                    place_abundance=0.3, place_spacing_m=40.0,
                    flock_despawn_m=80.0, flock_size_min=1, flock_size_max=2,
                    flock_spread_m=15.0, flock_perch="canopy",
                    flock_height_min_m=4.0, flock_height_max_m=24.0,
                    flock_flight_share=0.22, flock_per_hectare=1.2)),
    ),
    "common-cuckoo": (
        "0.33 m - grey, slim, long-tailed, and shaped like a sparrowhawk",
        base(name="common-cuckoo",
             notes="AUTHORED TO BE MISTAKEN FOR `eurasian-sparrowhawk`, WHICH IS "
                   "THE POINT OF THE ROW. Grey above, hard-barred white below, "
                   "long tail, pointed wings -- every small bird in Europe reacts "
                   "to this outline as if it were a hawk, and the library "
                   "already has the hawk to be mistaken FOR. The two are "
                   "separated by one number that a hawk cannot have: "
                   "`wing_aspect` 8.6 against the sparrowhawk's short broad "
                   "wing, because a cuckoo travels and a sparrowhawk ambushes.\n\n"
                   "THE BARRING IS DRAWN COARSER THAN THE ANIMAL'S, AND THAT "
                   "WAS MEASURED RATHER THAN CHOSEN. A cuckoo's underside bars "
                   "are fine and close, so this was first authored at nine bars "
                   "of `mark_width` 0.10 -- and `birdprobe.py --read` came back "
                   "with the marking covering 0.5% of the bird against a floor "
                   "of 2.5%, which is this project's signature defect: a "
                   "feature perfectly present in the spec and invisible in the "
                   "world. Seven bars at 0.26 measured 2.4% and STILL failed; "
                   "six bars at 0.42 measures 4.9% and clears it. There are "
                   "therefore "
                   "fewer bars than the bird has and each is wider, which is a "
                   "stylisation and is written down as one.",
             **b(length_m=0.33, bill_frac=0.060, head_frac=0.110,
                 neck_frac=0.035, body_frac=0.375, tail_frac=0.420,
                 posture_deg=6, body_depth=0.62, body_width=0.64,
                 chest_at=0.32, breast=0.72, rump=0.40, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.95, neck_up_deg=14,
                 neck_thick=0.56,
                 bill_depth=0.28, bill_curve=0.16, bill_gape=0.14,
                 tail_shape="graduated", tail_width=0.26, tail_droop=0.55,
                 tail_thick=2,
                 pose="flying", wing_shape="pointed", wing_span=2.30,
                 wing_aspect=8.6, wing_sweep=0.34, wing_dihedral=0.06,
                 wing_fold=0.90,
                 leg_len=0.07, eye=1.0, upperparts=0.58,
                 head_mark="none", wing_mark="none", body_mark="barred",
                 mark_count=6, mark_width=0.42,
                 mat_back="plume_grey", mat_belly="plume_white",
                 mat_head="plume_grey", mat_wing="plume_slate",
                 mat_mark="skin_dark", mat_head_mark="skin_dark",
                 mat_bill="skin_dark", mat_eye="skin_yellow",
                 bio_temperate_forest=1.0, bio_grassland=0.5, bio_taiga=0.4,
                 place_abundance=0.15, place_spacing_m=150.0,
                 flock_despawn_m=160.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=40.0, flock_perch="canopy",
                 flock_height_min_m=3.0, flock_height_max_m=60.0,
                 flock_flight_share=0.45, flock_per_hectare=0.3)),
    ),
    "common-redstart": (
        "0.24 m - grey above, orange below, and a rusty tail that never stops",
        base(name="common-redstart",
             notes="AUTHORED AT 0.24 m AGAINST A REAL 14 cm.\n\n"
                   "THE TAIL IS THE SPECIES AND THE TAIL HAS NO SLOT, so this "
                   "spec makes a trade and states it. The tail takes "
                   "`materials.bird_wing` (`forge/bird.py:1302`); a redstart has "
                   "a grey-brown wing and a burning rufous tail, and only one of "
                   "the two can exist. The tail wins -- `bird_wing` is set to "
                   "`plume_rufous` -- so the wing comes out rusty as well, which "
                   "the animal is not. The alternative was a correct wing and a "
                   "grey tail, which is a bird nobody would name. The half that "
                   "was given up is written here so it is a decision.\n\n"
                   "THE HEAD IS GREY RATHER THAN SLATE, and that is the ANIMAL "
                   "moved rather than a colour: on slate, the black face mask "
                   "measures a contrast ratio of 1.93 and the black bill "
                   "measures 1.93, both under the floor of 2.0, and BOTH clear "
                   "at 4.75 on the paler grey. Two failures fixed by one shade, "
                   "and a redstart's crown really is a pale blue-grey.",
             **song(length_m=0.24, bill_frac=0.065, head_frac=0.145,
                    neck_frac=0.030, body_frac=0.425, tail_frac=0.335,
                    posture_deg=36, body_depth=0.84, body_width=0.66,
                    fullness=3.4, bill_depth=0.24, bill_gape=0.12,
                    tail_shape="square", tail_width=0.42, tail_droop=0.50,
                    wing_span=1.65, wing_aspect=5.6, wing_fold=0.40,
                    upperparts=0.56,
                    head_mark="mask", wing_mark="none", body_mark="none",
                    mark_width=0.34,
                    mat_back="plume_slate", mat_belly="skin_orange",
                    mat_head="plume_grey", mat_wing="plume_rufous",
                    mat_mark="skin_dark", mat_head_mark="skin_dark",
                    mat_bill="skin_dark", mat_eye="skin_dark",
                    bio_temperate_forest=1.0, bio_taiga=0.4,
                    bio_grassland=0.25,
                    place_abundance=0.4, place_spacing_m=28.0,
                    flock_despawn_m=70.0, flock_size_min=1, flock_size_max=2,
                    flock_spread_m=14.0, flock_perch="canopy",
                    flock_height_min_m=1.0, flock_height_max_m=16.0,
                    flock_flight_share=0.22, flock_per_hectare=2.0)),
    ),
    # --- rainforest --------------------------------------------------------
    "sunbittern": (
        "0.50 m - cryptic barred heron of the forest floor and stream edge",
        base(name="sunbittern",
             notes="THE MARK IS ON THE SPREAD WING AND THE BIRD IS DRAWN AT "
                   "REST, which is the honest version of this species. The "
                   "eyespots only exist when the wings are open, `bird.pose` "
                   "offers perched and flying, and a flying sunbittern is not "
                   "what the biome file is describing. So `wing_mark` panel "
                   "draws the pale ring of the ocellus on a chestnut wing and "
                   "the black centre is not drawn.\n\n"
                   "THE ANIMAL WAS MOVED TO GET THE MARK. A chestnut-and-black "
                   "eyespot on a brown wing measures a contrast ratio of 1.45 "
                   "-- under the floor of 2.0 and effectively invisible -- so "
                   "the wing is painted the chestnut and the MARKING is painted "
                   "buff, which measures 2.40. Same two colours, opposite way "
                   "round, and the bird is still a cryptic grey-brown barred "
                   "heron-shape. The same buff then does the body barring "
                   "against a brown belly at 3.48, so one mark colour serves "
                   "both regions, which is all `materials.bird_mark` allows.\n\n"
                   "SHORT LEGS, WHICH IS WHAT MAKES IT NOT A HERON. `leg_len` "
                   "0.15 against `grey-heron`'s 0.30 on a neck share nearly as "
                   "long: a heron on stilts and this one standing on the ground.",
             **b(length_m=0.50, bill_frac=0.130, head_frac=0.075,
                 neck_frac=0.180, body_frac=0.400, tail_frac=0.215,
                 posture_deg=8, body_depth=0.68, body_width=0.66,
                 chest_at=0.34, breast=0.72, rump=0.46, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.85, neck_up_deg=48,
                 neck_thick=0.30,
                 bill_depth=0.20, bill_gape=0.05,
                 tail_shape="rounded", tail_width=0.62, tail_droop=0.30,
                 tail_thick=2,
                 pose="perched", wing_shape="elliptical", wing_span=1.70,
                 wing_aspect=5.0, wing_fold=0.55, wing_thick=2,
                 leg_len=0.15, leg_thick=1.5, eye=1.0, upperparts=0.52,
                 head_mark="supercilium", wing_mark="panel",
                 body_mark="barred", mark_count=7, mark_width=0.22,
                 mat_back="skin_brown", mat_belly="skin_brown",
                 mat_head="skin_dark", mat_wing="plume_rufous",
                 mat_mark="plume_buff", mat_head_mark="plume_white",
                 mat_bill="skin_orange", mat_eye="plume_crimson",
                 bio_rainforest=1.0,
                 place_abundance=0.15, place_spacing_m=100.0,
                 place_water_max_m=60.0,
                 flock_despawn_m=110.0, flock_size_min=1, flock_size_max=2,
                 flock_spread_m=20.0, flock_perch="waterside",
                 flock_height_min_m=0.0, flock_height_max_m=10.0,
                 flock_flight_share=0.10, flock_per_hectare=0.25)),
    ),
    "wattled-jacana": (
        "0.25 m - chestnut rail on absurd toes, walking on floating leaves",
        base(name="wattled-jacana",
             notes="THE TOES ARE THE SPECIES AND THEY ARE DRAWN ABOVE LIFE "
                   "SIZE, exactly as the rainforest file asks. A jacana's toes "
                   "are about 1 cm thick, which at the 1 cm bird lattice is one "
                   "voxel, and one voxel reads as a mistake rather than as a "
                   "feature -- the house rule wants about three. `leg_thick` is "
                   "3.0, which draws them at roughly 3 cm, and `leg_len` is 0.30 "
                   "of total length. That is the same fix "
                   "`common-kingfisher` already carries for its size, applied to "
                   "a part rather than to the whole animal. Do not thin them.\n\n"
                   "THE LEGS COME OUT YELLOW AND IN LIFE THEY ARE DULL GREY. "
                   "Legs take `materials.bird_bill` (`forge/bird.py:1303`), and "
                   "the bill has to be yellow because the yellow frontal shield "
                   "is the second identifying feature. Given one slot for both, "
                   "the enormous yellow feet are the more useful half at "
                   "twenty-five voxels -- and they are also the half the row is "
                   "actually about.\n\n"
                   "The green-yellow flight feathers are drawn as a wing panel, "
                   "which measures 2.57 against the chestnut wing -- over the "
                   "floor of 2.0 and the only bright thing on the bird.",
             **b(length_m=0.25, bill_frac=0.070, head_frac=0.115,
                 neck_frac=0.070, body_frac=0.545, tail_frac=0.200,
                 posture_deg=10, body_depth=0.70, body_width=0.62,
                 chest_at=0.32, breast=0.72, rump=0.46, fullness=3.0,
                 section=2.1, belly=0.50, head_size=0.90, neck_up_deg=38,
                 neck_thick=0.44,
                 bill_depth=0.26, bill_gape=0.08,
                 tail_shape="rounded", tail_width=0.52, tail_droop=0.25,
                 pose="perched", wing_shape="elliptical", wing_span=1.75,
                 wing_aspect=5.4, wing_fold=0.55,
                 leg_len=0.30, leg_thick=3.0, eye=1.0, upperparts=0.55,
                 head_mark="cap", wing_mark="panel", body_mark="none",
                 mark_width=0.24,
                 mat_back="plume_rufous", mat_belly="plume_rufous",
                 mat_head="skin_dark", mat_wing="plume_rufous",
                 mat_mark="plume_lime", mat_head_mark="skin_yellow",
                 mat_bill="skin_yellow", mat_eye="skin_dark",
                 bio_rainforest=1.0, bio_savanna=0.2,
                 place_abundance=0.3, place_spacing_m=40.0,
                 place_water_max_m=25.0,
                 flock_despawn_m=90.0, flock_size_min=1, flock_size_max=6,
                 flock_spread_m=20.0, flock_perch="waterside",
                 flock_height_min_m=0.0, flock_height_max_m=6.0,
                 flock_flight_share=0.12, flock_per_hectare=1.0)),
    ),
    "hummingbird": (
        "0.22 m - a needle bill and a gorget, drawn hanging in the air",
        base(name="hummingbird",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 11 cm, which is 2x and is "
                   "the choice the rainforest file sets out: eleven voxels is "
                   "under half the 20-90 the bird generator is drawn to read at "
                   "(`kinds.py:129-134`), so a hummingbird at true scale is a "
                   "smudge at any lattice. Author it oversized or drop it; this "
                   "authors it oversized. Do not correct it back.\n\n"
                   "THE ANIMAL WAS MOVED TO KEEP TWO FEATURES AT ONCE, and this "
                   "is the clearest case in the file. On the dark "
                   "`plume_iridescent` green a real hummingbird's head is, a "
                   "black bill measures a contrast ratio of 1.40 and a crimson "
                   "gorget measures 1.81 -- both under the floor of 2.0, so the "
                   "bird would have shipped with an invisible needle AND an "
                   "invisible throat, which is the entire species twice over. "
                   "Painting the head the brighter `plume_lime` instead takes "
                   "the bill to 6.81 and the gorget to 2.69 and keeps both, at "
                   "the cost of a yellower green than the animal's. The back "
                   "stays `plume_iridescent`, so the dark green is still on the "
                   "bird.\n\n"
                   "THE BILL IS 0.28 OF TOTAL LENGTH, longer than any other bird "
                   "here including the curlew, because the row's own description "
                   "is a needle as long as the body.\n\n"
                   "A HOVER IS NOT A POSE THE GENERATOR HAS. `bird.pose` flying "
                   "uses a third of the authored posture, because a bird in the "
                   "air normally lies along its line of travel -- so this is "
                   "authored flying with `posture_deg` at 72, which gets partway "
                   "to vertical. `eurasian-skylark` records the same limit for "
                   "the same reason.",
             **b(length_m=0.22, bill_frac=0.280, head_frac=0.120,
                 neck_frac=0.020, body_frac=0.440, tail_frac=0.140,
                 posture_deg=72, body_depth=0.86, body_width=0.66,
                 chest_at=0.32, breast=0.78, rump=0.44, fullness=3.4,
                 section=2.1, belly=0.52, head_size=0.95, neck_up_deg=20,
                 neck_thick=0.70,
                 bill_depth=0.10, bill_gape=0.03,
                 tail_shape="square", tail_width=0.46, tail_droop=0.40,
                 pose="flying", wing_shape="pointed", wing_span=2.20,
                 wing_aspect=7.0, wing_sweep=0.35, wing_dihedral=0.06,
                 wing_fold=1.05,
                 leg_len=0.04, eye=1.0, upperparts=0.56,
                 head_mark="throat", wing_mark="none", body_mark="none",
                 mark_width=0.45,
                 mat_back="plume_iridescent", mat_belly="plume_grey",
                 mat_head="plume_lime", mat_wing="plume_iridescent",
                 mat_mark="plume_crimson", mat_head_mark="plume_crimson",
                 mat_bill="skin_dark", mat_eye="skin_dark",
                 bio_rainforest=1.0, bio_savanna=0.15,
                 place_abundance=0.5, place_spacing_m=15.0,
                 flock_despawn_m=50.0, flock_size_min=1, flock_size_max=3,
                 flock_spread_m=12.0, flock_perch="shrub",
                 flock_height_min_m=1.0, flock_height_max_m=20.0,
                 flock_flight_share=0.85, flock_per_hectare=2.5)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "bird specs")
    written = 0
    for name, (blurb, changes) in SPECIES.items():
        s, rep = sm.patch(sm.default_spec(), changes)
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          label=name, width=28):
            written += 1
        print(f"  {'':<28} {blurb}")
    print(f"\n{written} of {len(SPECIES)} written")
    print("Now run:  python tools/birdprobe.py --read")
    print("          python tools/buildcheck.py --kind bird")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
