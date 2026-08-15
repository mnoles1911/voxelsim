"""Author the last of the fish: the rows the biome files still had queued.

FIFTEEN SPECIES ACROSS SIX BIOMES, and two of the seventeen the biome files list
are NOT here. Those two are the point of this docstring, so they come first.

TWO SPECIES ARE DELIBERATELY UNAUTHORED, AND THE REASON IS THE SAME ONE TWICE.

  * THE LONGSNOUT SEAHORSE. Its identity is a posture: a vertical S-shaped body
    with a prehensile tail curled round a holdfast. `fish` has no orientation and
    no posture parameter -- `forge/spec.py`'s fish table has `curve_amount` and
    `curve_at`, and both of them are a COLOUR-FIELD control, not a body bend:
    `forge/fish.py:1086-1099` uses them to move `back_frac` and `belly_frac`
    along the animal, which is where the dark cape sits, not where the animal
    points. `caudal_plane` turns the tail FIN through ninety degrees and is
    there for cetaceans. So a seahorse built here is a horizontal seahorse,
    which is a lozenge with a snout. It is left out.
  * THE GARDEN EEL. Its identity is also a posture -- "a thin vertical stalk
    emerging from sand, only the upper third of the animal visible" -- and it
    fails on the same missing parameter. Worse, with the posture gone what is
    left is a plain slender eel, and the library already ships `river-eel`,
    `conger-eel`, `giant-moray` and `lesser-sand-eel`. It would be a fifth
    horizontal eel with nothing of its own. It is left out.

Neither is a size problem and neither is a lattice problem; both are a kind
problem, and the fix is a `fish.posture` or a serpentine kind, which is a
generator change this file has no business making.

THE OARFISH IS HERE AND THE OCEAN FILE WARNED THAT IT MIGHT NOT BE. Its warning
was that a 15-20:1 ribbon leaves a body five or six voxels deep and a hundred
long, and that an eel-class peduncle rounds to a fraction of a voxel and comes
apart. Measured at 5 cm over three seeds, it builds as ONE piece at 26-
connectivity every time: a BODY seven voxels deep on a hundred of length,
seventeen deep counting the dorsal crest. The thing that makes it
work is the same thing the fish research recorded: the body axis is stamped as a
solid one-voxel run before anything else is added.

THE 20 cm FLOOR, AND SEVEN SPECIES UNDER IT. Shanny, sand goby, gudgeon,
stickleback, stone loach, brook lamprey and the mosquitofish are all genuinely
smaller in life, and each is authored up with the arithmetic in its own `notes`.
Two of them are worth naming here because they are the extremes: the
STICKLEBACK is authored at 3.3x life size and the MOSQUITOFISH at 4.0x, both
past `clown-anemonefish`'s 2.2x, which was the library's previous largest. Both
say so, and both say what the enlargement costs -- the size relationship with
the animals beside them.

ONE MARKING AND NEVER TWO, still. And a second rule this file kept running into:
`forge/fish.py:1205-1210` draws exactly ONE stripe and ignores `pattern_count`,
so a species with several thin flank stripes gets one of them. The tigerfish
says so in its own notes rather than here.

    python tools/seed_fish2.py
    python tools/seed_fish2.py --force

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
    # --- open ocean ---------------------------------------------------------
    "giant-trevally": (
        "1.00 m at 2 cm - a silver slab with a steep forehead and no wrist",
        base(res="2", name="giant-trevally",
             notes="THE NARROWEST TAIL WRIST IN THE LIBRARY at `peduncle` 0.14, "
                   "and it is the species: a trevally's body runs deep and heavy "
                   "and then pinches to almost nothing before a very wide fork. "
                   "That silhouette -- slab, pinch, fork -- is what separates it "
                   "from every other big silver fish in open water, none of "
                   "which pinch.\n\n"
                   "AUTHORED AT 2 cm, AND THE WRIST IS WHY. The smallest "
                   "identifying feature here is that pinch, roughly 7 cm deep on "
                   "a 1 m fish: at 2 cm it is 3.5 voxels, which is the house "
                   "rule's 'about three'; at 5 cm it is 1.4 and the fish has no "
                   "wrist at all, which makes it a tuna. That is the rule "
                   "applied, and it agrees with the ocean file's 2 cm.\n\n"
                   "The steep forehead is `depth_at` 0.30 -- the deepest point "
                   "moved forward onto the shoulder -- rather than a snout "
                   "number, because a blunt snout on a shallow body reads as a "
                   "chub and not as this.",
             **f(length_m=1.00, depth_ratio=0.38, width_ratio=0.34,
                 depth_at=0.30, fullness=4.4, snout=0.20, peduncle=0.14,
                 belly=0.50, width_follow=1.35, section=1.8, head_frac=0.30,
                 caudal_shape="forked", caudal_len=0.20, caudal_span=1.50,
                 caudal_fork=0.55,
                 dorsal_shape="spiny", dorsal_start=0.34, dorsal_len=0.08,
                 dorsal_height=0.26,
                 dorsal2_height=0.34, dorsal2_start=0.48, dorsal2_len=0.30,
                 anal_height=0.30, anal_len=0.28,
                 pectoral=0.40, pectoral_aspect=1.8, pelvic=0.18, eye=1.0,
                 back_frac=0.30, belly_frac=0.26, pattern="none",
                 mat_back="plume_slate", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="plume_slate",
                 mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.4,
                 place_abundance=0.2, place_spacing_m=12.0,
                 det_despawn_m=90.0, det_school_min=1, det_school_max=8,
                 det_school_radius_m=12.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=40.0,
                 det_min_water_depth_m=3.0, det_per_100m2=0.6)),
    ),
    "oarfish": (
        "5.00 m at 5 cm - a hundred voxels of silver ribbon under a red crest",
        base(res="5", name="oarfish",
             notes="THE OCEAN FILE FLAGGED THIS ONE AS POSSIBLY UNBUILDABLE AND "
                   "IT BUILDS. Its warning was exact: a 15-20:1 ribbon at 5 cm "
                   "is a body five or six voxels deep and a hundred long, and "
                   "the fish research had already recorded an eel-class peduncle "
                   "rounding to 0.2 of a voxel and producing the animal in three "
                   "pieces. Measured here over three seeds at 5 cm it comes out "
                   "as ONE piece at 26-connectivity every time, seven voxels "
                   "deep and a hundred long, because the body axis is stamped as "
                   "a solid one-voxel run before anything else is added. The "
                   "warning was right about the numbers and wrong about the "
                   "outcome, and both halves belong on the record.\n\n"
                   "5 cm AND NOT 2. The smallest identifying feature is the "
                   "crest of long first dorsal rays over the head, roughly "
                   "30-40 cm: at 5 cm that is 6-8 voxels, well past the house "
                   "rule's three, and 2 cm buys nothing except 250 voxels of "
                   "length on a background animal.\n\n"
                   "EVERY FIN IS RED, which is the animal and is also convenient "
                   "-- `materials.fish_fin` is one slot, so a species whose "
                   "crest is the only coloured thing on it would otherwise have "
                   "to paint the crest by painting all the fins. Here that is "
                   "simply true.\n\n"
                   "The dark blotching a real oarfish carries is NOT drawn. On a "
                   "body seven voxels deep a blotch is a voxel, and the outline "
                   "carries this species on its own.",
             **f(length_m=5.00, depth_ratio=0.075, width_ratio=0.28,
                 depth_at=0.30, fullness=2.6, snout=0.30, peduncle=0.55,
                 belly=0.50, width_follow=1.00, section=2.0, head_frac=0.10,
                 caudal_shape="pointed", caudal_len=0.05, caudal_span=0.50,
                 dorsal_shape="ridge", dorsal_start=0.05, dorsal_len=0.85,
                 dorsal_height=0.55,
                 anal_height=0.0, anal_len=0.05,
                 pectoral=0.20, pelvic=0.30, eye=1.0, fin_thick=2,
                 back_frac=0.30, belly_frac=0.24, pattern="none",
                 mat_back="skin_silver", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_red",
                 mat_eye="skin_dark",
                 bio_ocean=1.0,
                 place_abundance=0.03, place_spacing_m=200.0,
                 det_despawn_m=250.0, det_school_min=1, det_school_max=1,
                 det_school_radius_m=20.0, det_water="ocean",
                 det_depth_min_m=20.0, det_depth_max_m=200.0,
                 det_min_water_depth_m=25.0, det_per_100m2=0.02)),
    ),
    "turbot": (
        "0.60 m at 2 cm - a near-circular flatfish, the roundest fish here",
        base(res="2", name="turbot",
             notes="THE ROUNDEST OUTLINE IN THE LIBRARY. `width_ratio` is 1.80, "
                   "the top of the parameter's range, against `european-plaice`'s "
                   "1.60 and `common-sole`'s 1.15 in this same file -- and the "
                   "three of them together are the point: a plaice is an oval, a "
                   "sole is a narrow strap and a turbot is a disc. That is one "
                   "number doing all the separating, and a flatfish has no other "
                   "silhouette to spend.\n\n"
                   "THE BONY TUBERCLES ARE NOT DRAWN AND CANNOT BE. They are "
                   "about a centimetre across, which is half a voxel at 2 cm and "
                   "one voxel at 1 cm -- the house rule wants three. What is "
                   "drawn instead is the marbling a turbot also has, as a "
                   "coherent mottle. The lattice is decided by the OUTLINE "
                   "rather than by the tubercles: 60 cm at 2 cm is 30 voxels "
                   "across the disc, and at 5 cm it would be 12, under the "
                   "probe's floor of eighteen.\n\n"
                   "`width_follow` is 0.62, well under 1, which stops the width "
                   "tapering with the depth and is what keeps the disc a disc "
                   "rather than a leaf.",
             **f(length_m=0.60, depth_ratio=0.22, width_ratio=1.80,
                 depth_at=0.42, fullness=4.6, snout=0.30, peduncle=0.34,
                 belly=0.46, width_follow=0.62, section=2.4, head_frac=0.20,
                 caudal_shape="rounded", caudal_len=0.13, caudal_span=0.85,
                 dorsal_shape="ridge", dorsal_start=0.06, dorsal_len=0.82,
                 dorsal_height=0.44,
                 anal_height=0.40, anal_len=0.50,
                 pectoral=0.20, pelvic=0.0, eye=1.0,
                 back_frac=0.60, belly_frac=0.28,
                 pattern="mottle", pattern_scale=0.10, pattern_strength=0.40,
                 mat_back="skin_brown", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.5,
                 place_abundance=0.25, place_spacing_m=6.0,
                 det_despawn_m=60.0, det_school_min=1, det_school_max=3,
                 det_school_radius_m=12.0, det_water="shallow",
                 det_depth_min_m=1.0, det_depth_max_m=50.0,
                 det_min_water_depth_m=2.0, det_per_100m2=1.5)),
    ),
    "atlantic-wolffish": (
        "1.20 m at 2 cm - a blunt thick-lipped head on an eel of a body",
        base(res="2", name="atlantic-wolffish",
             notes="AN EEL SHAPE WITH A HEAD ON IT, which no other anguilliform "
                   "in the library has: `conger-eel` and `giant-moray` both run "
                   "`snout` at 0.72 on a `head_frac` under 0.20, so their heads "
                   "are part of the tube. This one carries `head_frac` 0.24 with "
                   "the deepest point of the body at 0.24 -- the mass is at the "
                   "FRONT -- which is the whole impression of a wolffish and is "
                   "two numbers.\n\n"
                   "THE CANINES ARE NOT DRAWN. There is no tooth or jaw "
                   "parameter in `fish`, and the row's own description leads "
                   "with them, so this is a real loss rather than a rounding. "
                   "What carries the species instead is the dark vertical "
                   "barring -- eleven bars over sixty voxels, so five voxels of "
                   "period, which is comfortably past the fish research's floor "
                   "of two on and two off.\n\n"
                   "THE FLANK IS PAINTED GREY RATHER THAN SLATE so the bars "
                   "measure. On slate a black bar comes out at a contrast ratio "
                   "of 1.93 against the fish floor of 1.5 -- it would have "
                   "passed, faintly -- and on grey it is 4.75. A wolffish is a "
                   "grey-brown to purplish fish and both are defensible, so the "
                   "measurement chose.",
             **f(length_m=1.20, depth_ratio=0.17, width_ratio=0.80,
                 depth_at=0.24, fullness=2.8, snout=0.70, peduncle=0.30,
                 belly=0.50, width_follow=1.00, section=2.6, head_frac=0.24,
                 caudal_shape="rounded", caudal_len=0.10, caudal_span=0.75,
                 dorsal_shape="ridge", dorsal_start=0.16, dorsal_len=0.78,
                 dorsal_height=0.34,
                 anal_height=0.26, anal_len=0.42,
                 pectoral=0.40, pelvic=0.0, eye=1.0, fin_thick=2,
                 back_frac=0.38, belly_frac=0.22,
                 pattern="bars", pattern_count=11, pattern_width=0.30,
                 mat_back="plume_slate", mat_flank="plume_grey",
                 mat_belly="skin_pale", mat_fin="plume_slate",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.12, place_spacing_m=25.0,
                 det_despawn_m=90.0, det_school_min=1, det_school_max=2,
                 det_school_radius_m=15.0, det_water="ocean",
                 det_depth_min_m=3.0, det_depth_max_m=100.0,
                 det_min_water_depth_m=4.0, det_per_100m2=0.3)),
    ),
    "clown-triggerfish": (
        "0.40 m - a black disc carrying big white blotches",
        base(name="clown-triggerfish",
             notes="THE HIGHEST-CONTRAST FISH IN THE LIBRARY: white on black "
                   "measures a contrast ratio of 12.14 against a floor of 1.5, "
                   "and there is nothing else on the animal. At forty voxels "
                   "that is a species identified from further away than anything "
                   "else in the reef set.\n\n"
                   "THE BLOTCHES SCATTER OVER THE WHOLE FLANK AND ON THE ANIMAL "
                   "THEY ARE ON THE BELLY. `forge/fish.py:1225-1235` places "
                   "`spots` at random heights within about half the body depth "
                   "and takes no position parameter -- `pattern_pos` is read "
                   "only by `stripe` -- so 'large white belly blotches' comes "
                   "out as large white blotches. The blotch is right and the "
                   "placement is not, and that is a generator limit rather than "
                   "a choice made here.\n\n"
                   "NO PELVIC FINS, which is real -- a triggerfish has none -- "
                   "and a tiny separate first dorsal spine at `dorsal_len` 0.05, "
                   "which is the shortest fin in the library and is the "
                   "structure the whole family is named for.",
             **f(length_m=0.40, depth_ratio=0.48, width_ratio=0.30,
                 depth_at=0.40, fullness=5.0, snout=0.34, peduncle=0.26,
                 belly=0.52, width_follow=1.30, section=1.6, head_frac=0.30,
                 caudal_shape="truncate", caudal_len=0.16, caudal_span=1.00,
                 dorsal_shape="spiny", dorsal_start=0.36, dorsal_len=0.05,
                 dorsal_height=0.30,
                 dorsal2_height=0.32, dorsal2_start=0.60, dorsal2_len=0.26,
                 anal_height=0.30, anal_len=0.24,
                 pectoral=0.26, pelvic=0.0, eye=1.0,
                 back_frac=0.26, belly_frac=0.20,
                 pattern="spots", pattern_count=8, pattern_scale=0.16,
                 mat_back="skin_dark", mat_flank="skin_dark",
                 mat_belly="skin_dark", mat_fin="skin_yellow",
                 mat_pattern="plume_white", mat_eye="skin_dark",
                 bio_ocean=0.8, bio_beach=0.3,
                 place_abundance=0.35, place_spacing_m=4.0,
                 det_despawn_m=40.0, det_school_min=1, det_school_max=3,
                 det_school_radius_m=4.0, det_water="reef",
                 det_depth_min_m=1.0, det_depth_max_m=25.0,
                 det_min_water_depth_m=1.5, det_per_100m2=2.0)),
    ),
    "remora": (
        "0.50 m - a slim hitch-hiker with a ridged plate over its head",
        base(name="remora",
             notes="THE SUCKER PLATE IS THE FIRST DORSAL FIN, which is what "
                   "makes this species authorable at all. It looks like a "
                   "structure the generator does not have, and anatomically it "
                   "is a dorsal fin that migrated onto the head and flattened -- "
                   "so it is drawn as exactly that: `dorsal_shape` ridge at "
                   "`dorsal_start` 0.05, running 0.14 of the body at a height "
                   "that `fin_min_vox` floors to two voxels. On a fifty-voxel "
                   "fish the head is twelve voxels and the plate is seven long "
                   "and two tall, which is the house rule met by the part rather "
                   "than by the animal.\n\n"
                   "THE REAL SOFT DORSAL IS STILL THERE, set far back as "
                   "`dorsal2`, so the fish has both fins the animal has and they "
                   "are in the right two places.\n\n"
                   "THE ROW SAYS IT 'ONLY READS WHEN ATTACHED TO SOMETHING "
                   "ELSE', AND THAT IS NOT A BUILD PROBLEM -- it is a placement "
                   "one. Nothing in `detail.*` can pin one asset to another, so "
                   "this ships as a free-swimming remora near reef and shark "
                   "water and the attachment is left to whoever owns placement.",
             **f(length_m=0.50, depth_ratio=0.16, width_ratio=0.62,
                 depth_at=0.40, fullness=2.8, snout=0.30, peduncle=0.36,
                 belly=0.50, width_follow=1.10, section=2.2, head_frac=0.24,
                 caudal_shape="truncate", caudal_len=0.14, caudal_span=1.00,
                 dorsal_shape="ridge", dorsal_start=0.05, dorsal_len=0.14,
                 dorsal_height=0.10,
                 dorsal2_height=0.22, dorsal2_start=0.52, dorsal2_len=0.34,
                 anal_height=0.22, anal_len=0.34,
                 pectoral=0.24, pelvic=0.14, eye=1.0,
                 back_frac=0.34, belly_frac=0.26,
                 pattern="stripe", pattern_width=0.26, pattern_pos=0.46,
                 mat_back="plume_slate", mat_flank="plume_grey",
                 mat_belly="skin_pale", mat_fin="plume_slate",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.2, place_spacing_m=15.0,
                 det_despawn_m=60.0, det_school_min=1, det_school_max=4,
                 det_school_radius_m=8.0, det_water="ocean",
                 det_depth_min_m=1.0, det_depth_max_m=60.0,
                 det_min_water_depth_m=2.0, det_per_100m2=0.5)),
    ),
    # --- beach and shallow sand ---------------------------------------------
    "common-sole": (
        "0.35 m - a narrow sandy strap with its mouth at one end",
        base(name="common-sole",
             notes="THE NARROWEST FLATFISH IN THE LIBRARY at `width_ratio` "
                   "1.15, against `european-plaice`'s 1.60 and the turbot's 1.80 "
                   "in this same file. That trio is authored as a deliberate "
                   "ladder, because a flatfish has almost nothing else: no "
                   "posture, no fin shape worth the name, and here not even a "
                   "marking.\n\n"
                   "UNIFORM SANDY BROWN AND NO PATTERN AT ALL, which is the "
                   "row's description and is also the strongest version of the "
                   "ladder argument -- if a sole with no marking still reads as "
                   "a different animal from a plaice with orange spots, the "
                   "outline did it.\n\n"
                   "THE SMALL MOUTH SET AT ONE END is `snout` 0.24 on "
                   "`head_frac` 0.20, which is the smallest head-and-snout "
                   "combination among the three flatfish and is the one feature "
                   "of a sole's face anybody would name.",
             **f(length_m=0.35, depth_ratio=0.18, width_ratio=1.15,
                 depth_at=0.44, fullness=3.8, snout=0.24, peduncle=0.40,
                 belly=0.46, width_follow=0.72, section=2.2, head_frac=0.20,
                 caudal_shape="rounded", caudal_len=0.12, caudal_span=0.80,
                 dorsal_shape="ridge", dorsal_start=0.06, dorsal_len=0.82,
                 dorsal_height=0.38,
                 anal_height=0.34, anal_len=0.50,
                 pectoral=0.16, pelvic=0.0, eye=1.0,
                 back_frac=0.60, belly_frac=0.30, pattern="none",
                 mat_back="skin_brown", mat_flank="skin_brown",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.5,
                 place_abundance=0.4, place_spacing_m=4.0,
                 det_despawn_m=45.0, det_school_min=1, det_school_max=4,
                 det_school_radius_m=8.0, det_water="shallow",
                 det_depth_min_m=0.5, det_depth_max_m=30.0,
                 det_min_water_depth_m=0.8, det_per_100m2=3.0)),
    ),
    "shanny": (
        "0.22 m - a blunt big-eyed rockpool fish under one long fin ridge",
        base(name="shanny",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 15 cm, and the beach file "
                   "asks for exactly this by name: 'build the shanny, author at "
                   "20 cm'. Fifteen voxels is under the probe's floor of "
                   "eighteen and under the 20 cm floor the library set. Do not "
                   "correct it back.\n\n"
                   "ONE DORSAL RUNNING THE WHOLE BACK, which is `dorsal_len` "
                   "0.68 on a `ridge` -- the longest single dorsal on any "
                   "non-flatfish, non-eel species in the library. Together with "
                   "`eye` at 2.0 and `snout` at 0.62 that is the entire animal: "
                   "blunt, big-eyed, and finned from head to tail.\n\n"
                   "THE PECTORALS ARE BIG ON PURPOSE at 0.42. A blenny props "
                   "itself on them in a rockpool the way `bullhead` does on the "
                   "bottom of a stream, and the pair make the same argument in "
                   "two biomes.",
             **f(length_m=0.22, depth_ratio=0.22, width_ratio=0.68,
                 depth_at=0.30, fullness=3.2, snout=0.62, peduncle=0.42,
                 belly=0.46, width_follow=0.95, section=2.5, head_frac=0.30,
                 caudal_shape="rounded", caudal_len=0.15, caudal_span=0.90,
                 dorsal_shape="ridge", dorsal_start=0.22, dorsal_len=0.68,
                 dorsal_height=0.30,
                 anal_height=0.28, anal_len=0.34,
                 pectoral=0.42, pectoral_aspect=1.3, pelvic=0.12, eye=2.0,
                 back_frac=0.36, belly_frac=0.22,
                 pattern="mottle", pattern_scale=0.16, pattern_strength=0.45,
                 mat_back="skin_brown", mat_flank="plume_buff",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.3,
                 place_abundance=0.6, place_spacing_m=1.5,
                 det_despawn_m=30.0, det_school_min=1, det_school_max=3,
                 det_school_radius_m=2.0, det_water="shallow",
                 det_depth_min_m=0.1, det_depth_max_m=4.0,
                 det_min_water_depth_m=0.2, det_per_100m2=6.0)),
    ),
    "sand-goby": (
        "0.20 m - a pale speckled sand fish the player is meant not to see",
        base(name="sand-goby",
             notes="AUTHORED AT 0.20 m AGAINST A REAL 9 cm, which is 2.2x and is "
                   "exactly `clown-anemonefish`'s enlargement against its real "
                   "10 cm. Nine voxels is under the probe's floor of eighteen "
                   "and under the library's 20 cm floor. Do not correct it "
                   "back.\n\n"
                   "THE BEACH FILE RECOMMENDS SKIPPING THIS SPECIES, and the "
                   "recommendation is recorded here rather than quietly "
                   "overruled: 'a deliberately cryptic 9-voxel fish is a lot of "
                   "work to produce something the player is designed not to "
                   "see'. That is an argument about VALUE, not about "
                   "feasibility, and it is a fair one. What it buys is the only "
                   "small sand-bottom fish in the shallow set -- everything else "
                   "authored on sand here is a flatfish -- so a sandy shallow "
                   "with nothing moving in it now has something.\n\n"
                   "TWO SEPARATE DORSALS AND A ROUNDED TAIL, which is a goby's "
                   "whole outline and is the only thing distinguishing it from "
                   "the shanny beside it, which has one continuous dorsal and "
                   "the same colour scheme. That pair is the test.",
             **f(length_m=0.20, depth_ratio=0.20, width_ratio=0.60,
                 depth_at=0.34, fullness=3.2, snout=0.44, peduncle=0.38,
                 belly=0.48, width_follow=1.00, section=2.3, head_frac=0.28,
                 caudal_shape="rounded", caudal_len=0.16, caudal_span=0.95,
                 dorsal_shape="spiny", dorsal_start=0.34, dorsal_len=0.10,
                 dorsal_height=0.26,
                 dorsal2_height=0.24, dorsal2_start=0.54, dorsal2_len=0.26,
                 anal_height=0.22, anal_len=0.26,
                 pectoral=0.34, pelvic=0.16, eye=1.0,
                 back_frac=0.32, belly_frac=0.26,
                 pattern="spots", pattern_count=8, pattern_scale=0.14,
                 mat_back="plume_buff", mat_flank="plume_buff",
                 mat_belly="skin_pale", mat_fin="plume_buff",
                 mat_pattern="skin_brown", mat_eye="skin_dark",
                 bio_beach=1.0, bio_ocean=0.2,
                 place_abundance=0.7, place_spacing_m=1.0,
                 det_despawn_m=28.0, det_school_min=3, det_school_max=20,
                 det_school_radius_m=3.0, det_water="shallow",
                 det_depth_min_m=0.1, det_depth_max_m=5.0,
                 det_min_water_depth_m=0.2, det_per_100m2=14.0)),
    ),
    # --- grassland rivers and ponds -----------------------------------------
    "gudgeon": (
        "0.22 m - a small spotted bottom fish with a downturned mouth",
        base(name="gudgeon",
             notes="AUTHORED AT 0.22 m AGAINST A REAL 13 cm, which is 1.7x. The "
                   "grassland file names this species and the stickleback "
                   "together and gives the rule: author up with the reason "
                   "written down, or leave them out. Do not correct it back.\n\n"
                   "TWO BARBELS AND NOT FOUR, WHICH IS THE ANIMAL. A gudgeon has "
                   "exactly two, one at each mouth corner, so this species does "
                   "not have to work around the barbel defect `barbel`'s notes "
                   "record -- it simply is a two-barbel fish. It is the only "
                   "small species in the library that carries barbels at all, "
                   "which at twenty-two voxels is a real feature: "
                   "`barbel_len` 0.05 is about a voxel of thread off each corner "
                   "of the snout.\n\n"
                   "FLAT-BELLIED at `belly` 0.40, which is the bottom-liver's "
                   "signature the shipped `barbel` and `bullhead` both carry, "
                   "under an arched back.",
             **f(length_m=0.22, depth_ratio=0.24, width_ratio=0.58,
                 depth_at=0.38, fullness=3.6, snout=0.52, peduncle=0.30,
                 belly=0.40, width_follow=1.05, section=2.4, head_frac=0.28,
                 caudal_shape="forked", caudal_len=0.18, caudal_span=1.05,
                 caudal_fork=0.28,
                 dorsal_shape="triangular", dorsal_start=0.42, dorsal_len=0.14,
                 dorsal_height=0.30,
                 anal_height=0.24, anal_len=0.12,
                 pectoral=0.34, pelvic=0.20, barbels=2, barbel_len=0.05,
                 eye=1.0, back_frac=0.34, belly_frac=0.24,
                 pattern="spots", pattern_count=8, pattern_scale=0.12,
                 mat_back="skin_brown", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_grassland=1.0, bio_temperate_forest=0.6,
                 place_abundance=0.7, place_spacing_m=1.2,
                 det_despawn_m=32.0, det_school_min=4, det_school_max=25,
                 det_school_radius_m=3.0, det_water="river",
                 det_depth_min_m=0.2, det_depth_max_m=2.0,
                 det_min_water_depth_m=0.3, det_per_100m2=14.0)),
    ),
    "three-spined-stickleback": (
        "0.20 m - three erect spines, a scarlet throat, and a blue eye",
        base(name="three-spined-stickleback",
             notes="AUTHORED AT 0.20 m AGAINST A REAL 6 cm. THAT IS 3.3x AND IT "
                   "IS THE LARGEST ENLARGEMENT IN THE LIBRARY, past "
                   "`clown-anemonefish`'s 2.2x, and it should not be repeated "
                   "casually. The grassland file put the choice plainly: a "
                   "six-voxel stickleback is a chip of colour and not a fish, so "
                   "either author it up or leave it out. This authors it up. "
                   "WHAT IT COSTS is the size relationship -- in life this is "
                   "the smallest fish in fresh water and here it is the same "
                   "length as a gudgeon, which is three times its size. Do not "
                   "correct the size back, and do not use this species as a "
                   "precedent for a fourth doubling.\n\n"
                   "AUTHORED AS THE BREEDING MALE, and unlike the birds that "
                   "cannot be a dimorphism: `fish` has `sex_length`, "
                   "`sex_dorsal` and `sex_pectoral` but no `sex_plumage` and no "
                   "`materials.fish_alt_*`, so a fish's two sexes can differ in "
                   "SIZE and not in colour. The scarlet throat and belly is "
                   "therefore painted into `materials.fish_belly` outright, and "
                   "the drab female is not expressible.\n\n"
                   "THE THREE SPINES are `dorsal_shape` spiny at `dorsal_len` "
                   "0.12 -- a short spiny block ahead of a separate soft "
                   "`dorsal2` -- and the very narrow `peduncle` 0.14 behind them "
                   "is the other half of the outline. The blue eye is real on a "
                   "breeding male and costs one material slot.",
             **f(length_m=0.20, depth_ratio=0.24, width_ratio=0.42,
                 depth_at=0.38, fullness=3.4, snout=0.28, peduncle=0.14,
                 belly=0.50, width_follow=1.20, section=2.0, head_frac=0.28,
                 caudal_shape="truncate", caudal_len=0.16, caudal_span=1.00,
                 dorsal_shape="spiny", dorsal_start=0.36, dorsal_len=0.12,
                 dorsal_height=0.34,
                 dorsal2_height=0.24, dorsal2_start=0.58, dorsal2_len=0.20,
                 anal_height=0.24, anal_len=0.18,
                 pectoral=0.32, pelvic=0.14, eye=1.0,
                 back_frac=0.34, belly_frac=0.30, pattern="none",
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_red", mat_fin="skin_silver",
                 mat_eye="skin_blue",
                 bio_grassland=1.0, bio_temperate_forest=0.6, bio_taiga=0.4,
                 place_abundance=0.9, place_spacing_m=0.8,
                 det_despawn_m=28.0, det_school_min=6, det_school_max=50,
                 det_school_radius_m=2.5, det_water="any",
                 det_depth_min_m=0.1, det_depth_max_m=1.5,
                 det_min_water_depth_m=0.2, det_per_100m2=25.0)),
    ),
    # --- temperate-forest streams -------------------------------------------
    "stone-loach": (
        "0.22 m - a mottled brown thread of a bottom fish with barbels",
        base(name="stone-loach",
             notes="AUTHORED AT 0.26 m AGAINST A REAL 12 cm, which is 2.2x. "
                   "Twelve voxels is under the probe's floor of eighteen, and "
                   "the extra above 0.22 was bought by MEASUREMENT rather than "
                   "by taste: at 0.22 m and `depth_ratio` 0.16 -- which is the "
                   "animal's real slimness -- `fishprobe.py --read` came back "
                   "THIN, with 15% of the silhouette surviving a one-voxel "
                   "erosion against a floor of 25%. A loach that slim is three "
                   "voxels of solid body and it dissolves under its own fins. "
                   "At 0.26 m and `depth_ratio` 0.20 it measures 46-61% over "
                   "five seeds. The animal is slimmer than this and the lattice "
                   "will not carry it. Do not correct either number back.\n\n"
                   "FOUR BARBELS AND THE ANIMAL HAS SIX, AND THAT IS A HARD CAP "
                   "RATHER THAN A CHOICE. `forge/fish.py:1011` clamps the count "
                   "to four -- `n = max(0, min(int(p['barbels']), 4))` -- and "
                   "`fish.barbels` is declared 0..4 in the parameter table, so "
                   "six is not authorable at any size or lattice. Four is what "
                   "the generator can draw and four is what is here. It is also "
                   "the count `barbel`'s own notes warn about, because a barbel "
                   "is a face-connected thread starting on a snout voxel and on "
                   "a narrow head the outer pair starts beside the head rather "
                   "than on it: this species carries `width_ratio` 0.90, which "
                   "is a broad flat head by the standard of a 22 cm fish, and it "
                   "was checked at three seeds for one piece before shipping.\n\n"
                   "A THREAD WITH A FLAT UNDERSIDE: `depth_ratio` 0.16 with "
                   "`belly` 0.36. It is the slimmest freshwater fish in the "
                   "library that is not an eel, and beside `bullhead` -- same "
                   "stream, same stones, `width_ratio` 1.35 and a huge head -- "
                   "the two cover the bottom of a stream between them.",
             **f(length_m=0.26, depth_ratio=0.20, width_ratio=0.90,
                 depth_at=0.36, fullness=3.0, snout=0.58, peduncle=0.44,
                 belly=0.36, width_follow=0.90, section=2.6, head_frac=0.26,
                 caudal_shape="truncate", caudal_len=0.15, caudal_span=0.95,
                 dorsal_shape="triangular", dorsal_start=0.46, dorsal_len=0.14,
                 dorsal_height=0.26,
                 anal_height=0.22, anal_len=0.12,
                 pectoral=0.42, pelvic=0.22, barbels=4, barbel_len=0.07,
                 eye=1.0, back_frac=0.36, belly_frac=0.20,
                 pattern="mottle", pattern_scale=0.14, pattern_strength=0.45,
                 mat_back="skin_brown", mat_flank="plume_buff",
                 mat_belly="skin_pale", mat_fin="skin_brown",
                 mat_pattern="skin_brown", mat_eye="skin_dark",
                 bio_temperate_forest=1.0, bio_grassland=0.5, bio_taiga=0.4,
                 place_abundance=0.6, place_spacing_m=1.5,
                 det_despawn_m=30.0, det_school_min=1, det_school_max=4,
                 det_school_radius_m=2.5, det_water="river",
                 det_depth_min_m=0.15, det_depth_max_m=1.5,
                 det_min_water_depth_m=0.25, det_per_100m2=8.0)),
    ),
    "brook-lamprey": (
        "0.28 m - an eel with no jaws and no paired fins whatever",
        base(name="brook-lamprey",
             notes="AUTHORED AT 0.28 m AGAINST A REAL 15 cm, which is what the "
                   "temperate-forest file asks for by name: 'author it at 25-30 "
                   "cm or leave it out'. Do not correct it back.\n\n"
                   "THE ONLY ANIMAL IN THE LIBRARY WITH NO PAIRED FINS AT ALL. "
                   "`pectoral` 0.0 and `pelvic` 0.0 together, plus `anal_height` "
                   "0.0 -- three parameters set to nothing, which is the entire "
                   "species and costs no shape budget whatever. Every other eel "
                   "here keeps its pectorals; this one is a tube with a fin "
                   "ridge on the back half and nothing else, and that absence is "
                   "more distinctive at twenty-eight voxels than any marking "
                   "could be.\n\n"
                   "THE SEVEN GILL PORES ARE DRAWN AS SEVEN MARKS SPREAD ALONG "
                   "THE WHOLE BODY AND ON THE ANIMAL THEY ARE CLUSTERED BEHIND "
                   "THE HEAD. `bars` distributes by a modulo over the length "
                   "(`forge/fish.py:1212-1223`) and takes no origin; `spots` "
                   "scatters at random and does not even keep them in a row. "
                   "Seven marks in a row was judged the closer of the two "
                   "wrongs, because a row is what a person reads, and the "
                   "displacement is written down here rather than left for "
                   "somebody to find.\n\n"
                   "THE ROUND SUCKER MOUTH is `snout` 0.55 on a very small "
                   "`head_frac` 0.16 -- blunt, and barely a head at all, which "
                   "is what a jawless animal looks like.",
             **f(length_m=0.28, depth_ratio=0.13, width_ratio=0.85,
                 depth_at=0.40, fullness=2.4, snout=0.55, peduncle=0.55,
                 belly=0.50, width_follow=1.00, section=2.4, head_frac=0.16,
                 caudal_shape="pointed", caudal_len=0.10, caudal_span=0.60,
                 dorsal_shape="ridge", dorsal_start=0.52, dorsal_len=0.42,
                 dorsal_height=0.26,
                 anal_height=0.0, anal_len=0.03,
                 pectoral=0.0, pelvic=0.0, eye=1.0,
                 back_frac=0.38, belly_frac=0.24,
                 pattern="bars", pattern_count=7, pattern_width=0.20,
                 mat_back="skin_olive", mat_flank="plume_buff",
                 mat_belly="skin_pale", mat_fin="skin_olive",
                 mat_pattern="skin_dark", mat_eye="skin_dark",
                 bio_temperate_forest=1.0, bio_grassland=0.4, bio_taiga=0.3,
                 place_abundance=0.3, place_spacing_m=4.0,
                 det_despawn_m=32.0, det_school_min=1, det_school_max=6,
                 det_school_radius_m=3.0, det_water="river",
                 det_depth_min_m=0.15, det_depth_max_m=1.5,
                 det_min_water_depth_m=0.25, det_per_100m2=3.0)),
    ),
    # --- rainforest rivers ---------------------------------------------------
    "african-tigerfish": (
        "0.70 m - deep-shouldered silver predator with red-orange fins",
        base(name="african-tigerfish",
             notes="ONE STRIPE, AND THE ANIMAL HAS SEVERAL. "
                   "`forge/fish.py:1205-1210` draws the `stripe` pattern as a "
                   "single horizontal band and IGNORES `pattern_count` "
                   "outright -- the parameter is read by `bars` and `spots` and "
                   "not by this branch. So a species whose flank carries a set "
                   "of thin dark horizontal lines gets one of them, drawn at "
                   "`pattern_width` 0.14. This is a generator limit and not a "
                   "stylisation, and it is the second time this file has hit it "
                   "in a different form; `bluestripe-snapper` carries "
                   "`pattern_count` 6 on disk today and draws one stripe, which "
                   "is a silent no-op that has already shipped.\n\n"
                   "THE INTERLOCKING TEETH ARE NOT DRAWN. There is no tooth or "
                   "jaw parameter, and the row leads with them. What carries the "
                   "species instead is the deep SHOULDER -- `depth_at` 0.30, the "
                   "deepest point pushed forward onto the front third -- over a "
                   "hard forked tail, plus red-orange fins, which no other "
                   "silver predator in the library has.\n\n"
                   "IT HAS AN ADIPOSE FIN, which is real for the family and is a "
                   "free identifying mark: the only other adipose fish here are "
                   "the salmonids, and none of them is silver, deep-shouldered "
                   "and orange-finned.",
             **f(length_m=0.70, depth_ratio=0.26, width_ratio=0.44,
                 depth_at=0.30, fullness=3.6, snout=0.26, peduncle=0.20,
                 belly=0.50, width_follow=1.30, section=2.0, head_frac=0.28,
                 caudal_shape="forked", caudal_len=0.20, caudal_span=1.20,
                 caudal_fork=0.45,
                 dorsal_shape="triangular", dorsal_start=0.40, dorsal_len=0.14,
                 dorsal_height=0.32, adipose=True,
                 anal_height=0.28, anal_len=0.18,
                 pectoral=0.28, pelvic=0.20, eye=1.0,
                 back_frac=0.32, belly_frac=0.26,
                 pattern="stripe", pattern_width=0.14, pattern_pos=0.52,
                 mat_back="plume_slate", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_orange",
                 mat_pattern="skin_dark", mat_eye="skin_yellow",
                 bio_rainforest=1.0, bio_savanna=0.4,
                 place_abundance=0.25, place_spacing_m=10.0,
                 det_despawn_m=60.0, det_school_min=1, det_school_max=8,
                 det_school_radius_m=10.0, det_water="river",
                 det_depth_min_m=0.5, det_depth_max_m=8.0,
                 det_min_water_depth_m=1.0, det_per_100m2=1.0)),
    ),
    # --- desert springs ------------------------------------------------------
    "western-mosquitofish": (
        "0.20 m - a plain grey scrap with an upturned mouth, and introduced",
        base(name="western-mosquitofish",
             notes="AUTHORED AT 0.20 m AGAINST A REAL 5 cm. THAT IS 4.0x AND IT "
                   "IS THE LARGEST ENLARGEMENT IN THE LIBRARY, past this file's "
                   "own stickleback at 3.3x and `clown-anemonefish` at 2.2x. It "
                   "should be read as a decision that could reasonably go the "
                   "other way, and the argument against it is recorded first "
                   "because the desert file makes it: at 1 cm this animal is "
                   "five voxels, below the point where a fish has a shape at "
                   "all, and 'the honest recommendation is to ship the pupfish "
                   "oversized as the single visible spring species and skip the "
                   "mosquitofish'. WHAT THE ENLARGEMENT COSTS is the size "
                   "relationship: a mosquitofish beside a pupfish is half its "
                   "length in life and the same length here, so the two now "
                   "differ only in shape.\n\n"
                   "IT IS INTRODUCED AND NOT NATIVE, which the desert file flags "
                   "and which belongs in the spec rather than only in the doc: "
                   "this species reached desert springs by human transport, so a "
                   "world that wants a fauna of a place should weight it to zero "
                   "rather than delete it.\n\n"
                   "WHAT SEPARATES IT FROM `desert-pupfish` IS TWO NUMBERS. The "
                   "pupfish is a stubby disc at `depth_ratio` 0.38 with the "
                   "dorsal over the middle at 0.46; this is a slimmer fish at "
                   "0.30 "
                   "with the dorsal set well back at 0.58, over an upturned "
                   "mouth (`snout` 0.16, the shallowest here). Plain grey-olive "
                   "and no marking at all, which is also the animal.",
             **f(length_m=0.20, depth_ratio=0.30, width_ratio=0.46,
                 depth_at=0.40, fullness=4.0, snout=0.16, peduncle=0.34,
                 belly=0.54, width_follow=1.15, section=2.0, head_frac=0.26,
                 caudal_shape="rounded", caudal_len=0.20, caudal_span=1.05,
                 dorsal_shape="triangular", dorsal_start=0.58, dorsal_len=0.14,
                 dorsal_height=0.28,
                 anal_height=0.26, anal_len=0.14,
                 pectoral=0.26, pelvic=0.16, eye=1.0,
                 back_frac=0.34, belly_frac=0.28, pattern="none",
                 mat_back="skin_olive", mat_flank="skin_silver",
                 mat_belly="skin_pale", mat_fin="skin_pale",
                 mat_eye="skin_dark",
                 bio_desert=1.0,
                 place_abundance=0.8, place_spacing_m=0.8,
                 det_despawn_m=25.0, det_school_min=5, det_school_max=40,
                 det_school_radius_m=2.0, det_water="shallow",
                 det_depth_min_m=0.05, det_depth_max_m=1.0,
                 det_min_water_depth_m=0.1, det_per_100m2=30.0)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "fish specs")
    written = 0
    for name, (blurb, changes) in SPECIES.items():
        s, rep = sm.patch(sm.default_spec(), changes)
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          label=name, width=28):
            written += 1
        print(f"  {'':<28} {blurb}")
    print(f"\n{written} of {len(SPECIES)} written")
    print("NOT authored, and deliberately: longsnout-seahorse and garden-eel.")
    print("Both are a POSTURE, and `fish` has no orientation parameter.")
    print("Now run:  python tools/fishprobe.py --read")
    print("          python tools/buildcheck.py --kind fish")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
