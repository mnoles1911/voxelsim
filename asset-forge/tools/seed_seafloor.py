"""Author the sea floor: seagrass, kelp, seaweed, reef rock and coral.

WHY THIS IS ITS OWN FILE. Ocean carried 67 fish specs and nothing whatever for
them to swim around. `forge/biomes.py`'s ocean row hosted only `("fish",
"cetacean", "bird")`, so `docs/biomes/00-ocean.md` marked about fifteen species
`host: rock` or `host: grass` -- right for the place, undeliverable. The owner
widened that tuple to `("fish", "cetacean", "bird", "rock", "grass", "reed",
"bush")` on 2026-08-15 and left `plantable` False, which is the distinction the
row exists to make: `plantable` means "a land plant can root here" and is still
no; `hosts` means "an asset of this kind belongs here", and kelp, seagrass and a
reef plainly do. This file is what that change unblocked.

FIFTEEN SPECIES IN FOUR KINDS, and the kind is a LATTICE decision here more
often than a botanical one -- the same call `dwarf-birch` records:

    grass  5 cm   eelgrass, turtlegrass                    the seagrass meadows
    reed   5 cm   giant kelp, bull kelp, sugar kelp,       tall strap algae
                  bladderwrack
    bush   2 cm   branching stony coral                    the tree machinery
    rock  10 cm   nine reef and sea-floor forms            incl. two corals

A ROCK IS LOCKED TO 10 cm AND THAT DECIDED TWO SPECIES. `forge/kinds.py:29-58`
puts rocks on the terrain lattice because they join the world's voxel grid and
are destructible as terrain is, and `forge.cli.selftest` refuses a `rock` at any
other size. Every rock below is at 10 cm and nothing here argues with that. What
it cost is written into two `notes`: the branching coral is NOT a rock, because
its finger is 3-5 cm and 10 cm cannot hold one, and it is authored as a `bush`
at 2 cm instead.

THE DOC PROPOSED THREE ROUTES AND TWO OF THEM WORK.

  * ROCK WITH A DIFFERENT PALETTE, for the brain coral and the table coral.
    WORKS. A massive brain coral is geometrically a dome and a table coral is a
    hard disc on a wasted neck, which is `rock.caprock` doing precisely what its
    own help text describes -- "weathering eats the neck and leaves the head:
    hoodoos, pedestals and mushroom rocks". Measured, both build as one piece.
  * THE TREE GENERATOR WITH A STONE PALETTE AND NO LEAVES, for the branching
    thicket. THE GEOMETRY WORKS AND THE PALETTE DOES NOT. `foliage.enabled`
    False on a `bush` gives a bare self-similar branch skeleton, which is the
    right shape; but `materials.bark` offers exactly four choices
    (`forge/materials.py:286` -- bark, bark_pale, heartwood, deadwood) and not
    one of them is stone. `deadwood` at (138,126,106) is a pale grey-tan and is
    the nearest thing available. A stone or coral entry on the WOOD menu is a
    one-row ask and this species is the argument for it.
  * THE FIN PLATE, for the sea fan. THE GEOMETRY WORKS AND THE KIND IS WRONG,
    so the sea fan IS NOT AUTHORED HERE. See the block comment at the end of
    this docstring.

TWO PARAMETER LIMITS THAT BIT, BOTH WORTH FIXING BY SOMEBODY ELSE.

`placement.elev_min_m` BOTTOMS OUT AT -10 m AND THE OCEAN IS DEEPER THAN THAT.
Its range is (-10, 4000) (`forge/spec.py`), so the only submerged elevation
window any spec can express is -10 m to 0. A giant kelp forest lives at 20-40 m
down and a seamount flank deeper still, and neither can say so. Everything fully
submerged here is authored at `elev_min_m` -10 / `elev_max_m` 0, which means
"below sea level" and nothing finer; the intertidal pair (bladderwrack, rubble
apron) gets -10 to 4, which is the beach gate's own band. Nothing reads
placement yet, so this costs nothing today.

`placement.water_max_m` IS MEANINGLESS FOR SOMETHING THAT IS IN THE WATER and is
left at 0 ("does not care") on every spec here. The parameter's own help calls
this out for fish, which is why `fish` is excluded from its `kinds` whitelist;
`rock`, `grass`, `reed` and `bush` are not excluded, because until 2026-08-15
none of them could be underwater.

THE SEA FAN IS NOT AUTHORED AND THAT IS DELIBERATE. `00-ocean.md`'s own note
says a gorgonian is "a flat rigid net held perpendicular to the current -- a
plane, not a volume", and that the shape to reach for is the fish generator's
fin plate. Three routes were built and measured at seed 1:

    bush, wedge crown, squash 0.30, 2 cm    52 x 60 x 52 vox  (1.04 x 1.20 x 1.04 m)
    fish, sail dorsal, 1 cm                 98 x  6 x 263 vox (0.98 x 0.06 x 2.63 m)

The fin plate is the shape. The bush is 60 voxels -- 1.2 m -- thick where a real
sea fan is under 2 cm, and it cannot be made thinner: `crown.squash` squashes
VERTICALLY and there is no parameter anywhere that flattens a crown in one
horizontal direction. The tuft kinds cannot do it either, and for a different
reason: `forge/ground.py:_stem` stratifies stem azimuths over a full circle and
draws each stem as a round `grid.capsule`, so a tuft is a radial spray by
construction and never a plane.

So the only kind that makes the shape is `fish` -- and a `fish` spec is placed
as a SWIMMING detail entity (`detail.depth_min_m`, `school_min`/`school_max`,
`despawn_m`), which would put a gorgonian in mid-water instead of anchored to
the bottom. That is a placement fault rather than a shape one, and working
around it silently would ship a coral that swims. Left unauthored, with the
numbers, for whoever decides whether a benthic flat-plate kind is worth having.

    python tools/seed_seafloor.py
    python tools/seed_seafloor.py --force

SIZES ARE APPROXIMATE AND SAY SO. Every height and block size below is the
approximate figure from `docs/biomes/00-ocean.md`, whose own closing section
states that none of it is measured and none of it is sourced. Nothing here is
quoted as a fact. Where a spec departs from the listed figure the reason is in
its `notes`. The one number in this file that IS measured is `rock.size_m`,
which the rock builder measures and corrects on the same seed.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def _prefixed(kw: dict, default_group: str) -> dict:
    """`mat_`/`bio_`/`place_` prefixes route to their own groups; the rest to
    `default_group`."""
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials." + k[len("mat_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        else:
            out[f"{default_group}.{k}"] = v
    return out


def tuft(kind, name, height_m, notes, **kw):
    """A `grass` or `reed` spec at the 5 cm ground-cover lattice."""
    return {
        "kind": kind, "name": name, "height_m": height_m, "notes": notes,
        "resolution_cm": "5",
        "variation.amount": 1.0,
        "variation.height": 0.28,
        "variation.shape": 0.18,
        "variation.proportion": 0.20,
        **_prefixed(kw, "tuft"),
    }


def rock(name, notes, **kw):
    """A `rock` spec. TEN CENTIMETRES, and nothing else is legal."""
    return {
        "kind": "rock", "name": name, "notes": notes,
        "resolution_cm": "10",
        "variation.amount": 1.0,
        "variation.height": 0.22,
        "variation.shape": 0.18,
        "variation.rotate": True,
        **_prefixed(kw, "rock"),
    }


_BUSH_GROUPS = {
    "radius_base_m": "trunk", "clear_frac": "trunk", "lean_deg": "trunk",
    "wander": "trunk",
    "shape": "crown", "radius_m": "crown", "height_frac": "crown",
    "center_frac": "crown", "squash": "crown", "asymmetry": "crown",
    "offset": "crown", "points": "crown",
    "model": "growth", "step_m": "growth", "influence_m": "growth",
    "kill_m": "growth", "gravity": "growth", "phototropism": "growth",
    "inertia": "growth", "jitter": "growth", "max_iter": "growth",
    "shade": "growth", "tip_radius_m": "growth", "radius_exp": "growth",
    "enabled": "foliage",
}


def bush(name, height_m, resolution_cm, notes, **kw):
    """A `bush` spec: the tree machinery, authored short."""
    out = {
        "kind": "bush", "name": name, "height_m": height_m, "notes": notes,
        "resolution_cm": resolution_cm,
        "variation.amount": 1.0,
        "variation.height": 0.24,
        "variation.crown_radius": 0.24,
        "variation.trunk_radius": 0.20,
        "variation.shape": 0.16,
        "variation.proportion": 0.26,
        "variation.lean_deg": 8.0,
        "variation.rotate": True,
    }
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials." + k[len("mat_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        else:
            out[f"{_BUSH_GROUPS[k]}.{k}"] = v
    return out


# name -> (blurb, changes)
SPECIES = {
    # --- ground cover: the seagrass meadows ---------------------------------
    #
    # SEAGRASSES ARE TRUE FLOWERING PLANTS -- the only ones that live fully
    # submerged -- and the ocean file lists them under Ground cover rather than
    # Flowers because the tuft generator is what builds them and because neither
    # flowers in a way worth drawing at 5 cm.
    "eelgrass-meadow": (
        "grass 0.60 m - dense ribbon blades all leaning one way, in current",
        tuft("grass", "eelgrass-meadow", 0.60,
             notes="THE FIRST ASSET THE SEA FLOOR HAS EVER HAD, and the one "
                   "that changes the most ground: eelgrass grows as a "
                   "CONTINUOUS MEADOW rather than as scattered tufts, so what "
                   "identifies it at any distance is not one plant but an "
                   "unbroken green sheet over mud.\n\n"
                   "SPACING 0.20 m, WHICH IS THE HONEST NUMBER AND WAS NOT "
                   "SAYABLE UNTIL TODAY. `placement.spacing_m` had a floor of "
                   "0.5 m until the owner lowered it to 0.1 on 2026-08-15, and "
                   "0.5 m between tufts of a meadow grass is a scatter with mud "
                   "showing through it. 0.20 m is one tuft's own diameter -- "
                   "`spread_m` 0.10 is a radius -- so tufts stand shoulder to "
                   "shoulder without interpenetrating, which is what a meadow "
                   "is. Anything tighter would be dishonest for a different "
                   "reason: it would overlap the asset with itself.\n\n"
                   "THE ONE-WAY LEAN IS THE FIELD MARK AND IT IS NOT DRAWN. "
                   "Every blade in a seagrass bed lies over in the same "
                   "direction because the current is the same for all of them, "
                   "and `forge/ground.py:_stem` stratifies stem azimuths evenly "
                   "over a full circle on purpose -- the comment there says "
                   "independent draws clump and a fan reads as manufactured. "
                   "There is no directional-lean parameter on a tuft. What is "
                   "authored is a high arc with a low splay, which gives blades "
                   "laid right over in every direction rather than in one. A "
                   "current direction is a placement or a generator feature, "
                   "not a retune of this spec.",
             stems=26, spread_m=0.10, splay_deg=16, arc=0.74, width_m=0.055,
             taper=0.5, wander=0.30, length_var=0.35, base_m=0.10,
             head="none", mat_stem="leaf_needle",
             bio_ocean=1.0, bio_beach=0.4,
             place_abundance=1.0, place_spacing_m=0.2, place_cluster=1.0,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=25),
    ),
    "turtlegrass": (
        "grass 0.50 m - wider, blunter, stiffer straps, more sparsely set",
        tuft("grass", "turtlegrass", 0.50,
             notes="THE OTHER SEAGRASS, AND DELIBERATELY THE OPPOSITE BUILD TO "
                   "THE EELGRASS BESIDE IT: half as many stems, nearly twice as "
                   "wide, and a third of the arc, so it stands where eelgrass "
                   "lies over. Two meadow grasses in one biome must not read as "
                   "one plant, and at ten voxels blade width and stiffness are "
                   "the only two axes there are.\n\n"
                   "THE DOC'S PROSE AND ITS SIZE COLUMN DISAGREE AND THE COLUMN "
                   "WON. `00-ocean.md` describes turtlegrass as 'sparser and "
                   "taller' than eelgrass and then gives it 0.3-0.6 m against "
                   "eelgrass's 0.3-0.9. Authored to the columns -- 0.50 against "
                   "0.60 -- so this is the SHORTER of the two, and the "
                   "separation it carries is width and stiffness rather than "
                   "height. Written down so it is not 'corrected' either way "
                   "without someone deciding which half of the doc is right.\n\n"
                   "Sparser than the eelgrass in placement too: 0.35 m spacing "
                   "rather than 0.20, so mud shows between the clumps.",
             stems=13, spread_m=0.09, splay_deg=12, arc=0.26, width_m=0.09,
             taper=0.45, wander=0.20, length_var=0.30, base_m=0.09,
             head="none", mat_stem="leaf_jungle",
             bio_ocean=1.0, bio_beach=0.3,
             place_abundance=0.9, place_spacing_m=0.35, place_cluster=0.9,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=25),
    ),
    # --- ground cover: the kelps and the wrack -------------------------------
    "giant-kelp": (
        "reed 28 m - the tallest asset in the library; a forest of one plant",
        tuft("reed", "giant-kelp", 28.0,
             notes="THE TALLEST NON-HERO ASSET IN THE LIBRARY BY EIGHT TIMES. "
                   "The next tallest ground-layer spec is `giant-reed` at 4 m; "
                   "this is 28. `00-ocean.md` puts a giant kelp at 20-40 m and "
                   "notes it is taller than every tree in the library except "
                   "`hero-sequoia`, and it is filed as a reed rather than a "
                   "tree because a stipe is a strap and not a trunk.\n\n"
                   "MEASURED COST, so nobody has to discover it. This is the "
                   "most expensive non-hero spec in the library and by a wide "
                   "margin: seed 1 builds 18,341 voxels in a 53 MB grid in "
                   "1.3 s, against 1 MB and 60 ms for the bull kelp beside it. "
                   "It is still four hundred times cheaper than "
                   "`hero-arch-colossal` and it does not go near "
                   "`forge.cli.HEAVY_SPECS`, so it needs no special handling -- "
                   "but it is the one asset here that would show up if the "
                   "whole library were built in a loop.\n\n"
                   "THE AUTHORED HEIGHT IS A MEAN, NOT A CAP, AND SEED 1 LANDS "
                   "AT THE TOP OF THE RANGE. `variation.height` 0.28 on 28 m "
                   "puts individuals across roughly 20 to 36 m, which is the "
                   "doc's own 20-40 m without anybody authoring the top of it "
                   "-- and the measured seed-1 individual came out 39.6 m tall "
                   "(792 voxels), which is ABOVE that arithmetic because the "
                   "variation compounds with `variation.proportion`. Anyone "
                   "reading 28 in this spec and expecting a 28 m plant will be "
                   "wrong by a third, so it is written down. It also spreads "
                   "11 m across: a 14% arc over a 40 m stipe lays the tips a "
                   "long way out, which is what a kelp canopy does in current "
                   "and is not a defect.\n\n"
                   "ONE KELP IS ONE PLANT AND NOT A STAND. `tools/buildcheck.py` "
                   "enforces one connected piece at 26-connectivity, so a kelp "
                   "FOREST is a placement result -- hence 1.5 m spacing at "
                   "maximum clustering -- and not something this spec can "
                   "contain.\n\n"
                   "TWO FIELD MARKS ARE LOST AND BOTH ARE STRUCTURAL. First, "
                   "the gas bladder at the base of each blade is 2-5 cm and IS "
                   "the identifying feature; at 5 cm it is one voxel, and the "
                   "lattice that would hold it is 1 cm, at which this plant is "
                   "2,800 voxels tall. It is not drawn at any size. Second, the "
                   "paired blades run the WHOLE LENGTH of the stipe and "
                   "`tuft.head` only ever sits on the top of a stem -- there is "
                   "no along-the-stem foliage on a tuft. So what is authored is "
                   "bare straps with a single plume on top standing in for the "
                   "surface canopy, which is the read from below and is not the "
                   "structure of the plant.",
             stems=7, spread_m=0.16, splay_deg=6, arc=0.14, width_m=0.10,
             taper=0.8, wander=0.30, length_var=0.30, base_m=0.16,
             head="plume", head_m=0.50, head_frac=0.10, head_share=1.0,
             mat_stem="leaf_dry", mat_head="leaf_dry",
             bio_ocean=1.0,
             place_abundance=0.7, place_spacing_m=1.5, place_cluster=1.0,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=40),
    ),
    "bull-kelp": (
        "reed 14 m - one bare whip to one float: the simplest silhouette here",
        tuft("reed", "bull-kelp", 14.0,
             notes="A VERY STRONG AND VERY SIMPLE SILHOUETTE, which is the "
                   "doc's own phrase and the whole reason it is worth having "
                   "beside the giant kelp: one bare dark whip running clean to "
                   "a single ball at the top, against the giant kelp's bundle "
                   "of seven. Three stems rather than one, tight together at a "
                   "3 degree splay with almost no arc and almost no length "
                   "spread, so the clump reads as ONE cable -- the same trick "
                   "`esparto-grass` uses to make a bundle read as a column.\n\n"
                   "THE CROWN OF BLADES TRAILING FROM THE FLOAT CANNOT BE "
                   "DRAWN. Those blades hang from the TOP of the plant, and "
                   "every stem in `forge/ground.py` is rooted at z=0 -- there "
                   "is nothing in the tuft generator that hangs anything from "
                   "anything. The float and its blade crown are drawn together "
                   "as one wide plume, which gives the right silhouette and not "
                   "the right structure.\n\n"
                   "`podzol` for the stipe because it is the darkest brown the "
                   "stem menu has and bull kelp is a very dark olive-brown; "
                   "`leaf_dry` for the float, which is paler, so the ball reads "
                   "against the whip.",
             stems=3, spread_m=0.07, splay_deg=3, arc=0.08, width_m=0.09,
             taper=0.9, wander=0.16, length_var=0.10, base_m=0.10,
             head="plume", head_m=0.60, head_frac=0.08, head_share=1.0,
             mat_stem="podzol", mat_head="leaf_dry",
             bio_ocean=1.0,
             place_abundance=0.5, place_spacing_m=1.2, place_cluster=0.9,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=40),
    ),
    "sugar-kelp": (
        "reed 2.20 m - one broad crinkle-edged strap from a small holdfast",
        tuft("reed", "sugar-kelp", 2.20,
             notes="THE ONE KELP THE GENERATOR CAN DRAW ALMOST HONESTLY, for "
                   "the same reason `harts-tongue-fern` is the one honest fern: "
                   "it does not branch, it has no bladders and it has no "
                   "canopy. It is a single broad undivided strap from a small "
                   "holdfast, and a wide low-taper stem is exactly that.\n\n"
                   "Authored as four straps rather than one so the holdfast has "
                   "something to hold and the plant has a silhouette from more "
                   "than one angle. `width_m` 0.18 is the widest stem on any "
                   "tuft in the library -- three to four voxels at 5 cm -- "
                   "which is what makes it a strap rather than a blade.\n\n"
                   "The crinkled ruffled edge that names it is 2-3 cm of "
                   "relief and is sub-voxel at 5 cm. Not attempted; the width "
                   "carries the species without it.",
             stems=4, spread_m=0.07, splay_deg=10, arc=0.42, width_m=0.18,
             taper=0.75, wander=0.28, length_var=0.30, base_m=0.07,
             head="none", mat_stem="leaf_dry",
             bio_ocean=1.0, bio_beach=0.3,
             place_abundance=0.6, place_spacing_m=0.6, place_cluster=0.85,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=45),
    ),
    "bladderwrack": (
        "reed 0.60 m - forking olive-brown straps, dense on intertidal rock",
        tuft("reed", "bladderwrack", 0.60,
             notes="THE INTERTIDAL ONE, AND IT BELONGS TO THE BEACH AS MUCH AS "
                   "TO THE OCEAN -- so it carries a full beach weight as well "
                   "as an ocean one and an elevation window of -10 to +4 m, "
                   "which is the beach gate's own band (`biome.h`: -3 m to "
                   "+4 m). It is the only asset in this file that is out of the "
                   "water half the time.\n\n"
                   "BOTH FIELD MARKS ARE BELOW THE LATTICE AND ONE IS ALSO "
                   "BELOW THE GENERATOR. The paired round bladders are 1-2 cm, "
                   "which is under half a voxel at 5 cm. The repeated FORKING "
                   "is worse than small: a tuft stem does not branch at all "
                   "(`forge/ground.py` draws each stem as one unbranched "
                   "polyline), so a dichotomous frond cannot be expressed at "
                   "any lattice by this generator. What is authored is many "
                   "narrow straps from a wide crown, which gives a dense "
                   "olive-brown mass at the right height and reads correctly "
                   "on a rock at low tide; it is a stylisation and not the "
                   "plant.\n\n"
                   "`podzol` rather than `leaf_dry`, which separates it from "
                   "the sugar kelp above: wrack is a darker, browner olive.",
             stems=22, spread_m=0.12, splay_deg=26, arc=0.58, width_m=0.06,
             taper=0.6, wander=0.40, length_var=0.40, base_m=0.12,
             head="none", mat_stem="podzol",
             bio_ocean=0.8, bio_beach=1.0,
             place_abundance=0.9, place_spacing_m=0.3, place_cluster=1.0,
             place_elev_min_m=-10.0, place_elev_max_m=4.0,
             place_slope_max_pct=55),
    ),
    # --- the branching coral: the tree generator, tested ---------------------
    "branching-stony-coral": (
        "bush 1.00 m - a dense thicket of stone fingers on a low base",
        bush("branching-stony-coral", 1.00, "2",
             notes="THE DOC'S SECOND ROUTE, BUILT AND MEASURED. "
                   "`00-ocean.md` argues that two of the three coral forms are "
                   "a rock with a different palette and one is not -- the "
                   "branching thicket is a SELF-SIMILAR BRANCHING STRUCTURE, "
                   "which is what `forge/skeleton.py`'s `grow` already builds "
                   "for trees, so the shortest route to a reef is the tree "
                   "generator with a stone palette and no leaves rather than a "
                   "new `coral` kind. It calls that a claim worth testing and "
                   "not a conclusion. Tested: the SHAPE route works, the "
                   "PALETTE route does not, and the branch spacing is coarser "
                   "than life by a floor nobody can tune around.\n\n"
                   "NOT A ROCK, AND THE LATTICE IS WHY. A `rock` is a "
                   "terrain-lattice asset locked to 10 cm "
                   "(`forge/kinds.py:29-58`; `forge.cli.selftest` refuses any "
                   "other size), and the doc says outright that the branch, at "
                   "3-5 cm, is the smallest identifying feature and it decides "
                   "the lattice. At 10 cm a finger is under half a voxel and "
                   "the thicket is a lump. A `bush` is a DETAIL asset, its "
                   "lattice is free, and 2 cm puts a 4 cm finger at two voxels. "
                   "The kind here is a lattice decision, not a botanical one -- "
                   "the same call `dwarf-birch` records for the opposite "
                   "reason.\n\n"
                   "THE FIRST BUSH IN THE LIBRARY NOT AT 5 cm -- the other "
                   "forty-nine are all at 5 -- and the cost was measured on "
                   "THIS spec before committing: 1,010 voxels at 5 cm "
                   "(`buildcheck --res 5`) against 5,302 at its authored 2 cm, "
                   "both at seed 1, both one piece. Five times the voxels for a "
                   "metre-tall asset is cheap: `witch-hazel` is 14,819 and "
                   "`understory-fan-palm` 24,938. At 5 cm every finger is "
                   "exactly one voxel, which is the lump the 10 cm lattice "
                   "would have given, only smaller.\n\n"
                   "THE PALETTE IS THE REAL LOSS. `materials.bark` offers four "
                   "choices and every one of them is wood: bark, bark_pale, "
                   "heartwood, deadwood (`forge/materials.py:286`). There is no "
                   "stone on the WOOD menu, even though `materials.rock` has "
                   "five stones on its own. `deadwood` at (138,126,106) is a "
                   "pale grey-tan and is the closest thing to bleached "
                   "limestone available; a live stony coral is browner and a "
                   "dead one is whiter. A stone entry on the wood choice list "
                   "is a one-row ask and this is the argument for it.\n\n"
                   "THE THICKET IS COARSER THAN LIFE AND THE NUMBER IS 0.10 m. "
                   "`growth.kill_m` has a floor of 0.10 m, so a growth target "
                   "is consumed within 10 cm of a branch tip and no two "
                   "branches can be closer than that however fine the lattice "
                   "is. Real coral fingers stand 3-5 cm apart. So this is a "
                   "thicket at half the real density, and lowering the lattice "
                   "further would not change it -- the floor is in metres, not "
                   "in voxels. `growth.step_m` (floor 0.08) and "
                   "`growth.influence_m` (floor 0.4) bind the same way.\n\n"
                   "`foliage.enabled` False, which is what 'no leaves' means "
                   "here and is safe: `pipeline.health`'s 'bald' check only "
                   "fires when foliage is ON and no clump was placed.",
             radius_base_m=0.10, clear_frac=0.10, lean_deg=2.0, wander=0.20,
             shape="sphere", radius_m=0.55, height_frac=0.92, center_frac=0.55,
             squash=0.85, asymmetry=0.30, offset=0.12, points=1400,
             model="colonize", step_m=0.08, influence_m=0.40, kill_m=0.10,
             gravity=0.30, phototropism=0.40, inertia=0.55, jitter=0.10,
             max_iter=260, tip_radius_m=0.025, radius_exp=2.0,
             enabled=False,
             mat_bark="deadwood", mat_core="deadwood",
             bio_ocean=1.0,
             place_abundance=0.6, place_spacing_m=1.5, place_cluster=0.9,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=45),
    ),
    # --- rock: the sea floor itself ------------------------------------------
    "submerged-granite-boulder": (
        "rock 1.60 m - the joint block again, darker and settled deep in mud",
        rock("submerged-granite-boulder",
             notes="A SEPARATE SPEC, AND THE ARGUMENT FOR IT IS THREE "
                   "DIFFERENCES RATHER THAN ONE. `00-ocean.md` says this is the "
                   "shipped `granite-boulder` and that 'the difference is the "
                   "palette and the burial fraction, not the geometry', which "
                   "reads as an argument for reusing the shipped spec. It is "
                   "not, for three reasons, and they are worth stating because "
                   "the opposite call would have been defensible.\n\n"
                   "FIRST, THE SHIPPED SPEC CANNOT BE REUSED WITHOUT BEING "
                   "EDITED. `specs/granite-boulder.json` carries "
                   "`biomes.ocean` 0.0, and giving it an ocean weight means "
                   "writing to a spec that has been tuned on disk -- which is "
                   "exactly what `tools/seedspec.py` exists to refuse, after "
                   "`seed_heroes.py` reverted a finished `hero-natural-arch` to "
                   "draft values. SECOND, the burial genuinely differs and it "
                   "is not cosmetic: `bury` 0.48 against the shipped 0.25, "
                   "because mud is soft and a boulder settles into it, and "
                   "burial is the parameter that decides how settled a stone "
                   "looks. THIRD, the size differs -- the doc's own block-size "
                   "column says 1.6 m and the shipped spec measures 2.4 m -- so "
                   "'not the geometry' is not quite true either.\n\n"
                   "THE PALETTE IS THE WEAKEST OF THE THREE. `materials.rock` "
                   "offers rock, bedrock, gravel, sand and clay, and none of "
                   "them is a wet, dark, algae-filmed stone. `bedrock` is the "
                   "darker grey and reads as an unweathered face, which is the "
                   "nearest available; it is not the green-black a boulder "
                   "under water actually is. Same one-row materials ask the "
                   "chalk outcrop records from the other end of the range.\n\n"
                   "The joint sets are kept because they are the point of a "
                   "granite boulder: it rounds from the corners in, and the "
                   "faces under the rounding have to CORRELATE or it reads as "
                   "merely lumpy.",
             size_m=1.6, lumps=7, spread=0.55, flatten=0.78, elongate=1.25,
             angular=0.50, facets=4, rough=0.40, erode=0.40, cavernous=0.0,
             joint_sets=3, joint_scatter=0.10, joint_dip_deg=8,
             bury=0.48, rubble=0.25, mat_rock="bedrock",
             bio_ocean=1.0, bio_beach=0.5,
             place_abundance=0.6, place_spacing_m=6.0, place_cluster=0.5,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=60),
    ),
    "boulder-reef": (
        "rock 2.20 m - angular blocks with wide gaps, standing proud of mud",
        rock("boulder-reef",
             notes="THE GAPS ARE THE REEF. A loose cluster of angular blocks "
                   "with open space between them is what makes a rocky reef "
                   "worth swimming through, and a continuous mass with faces "
                   "drawn on it reads as one stone -- which is the finding "
                   "`fractured-outcrop` already records. So this is "
                   "`rock.block_relief_m` on three joint sets, with `settle_m` "
                   "letting the separated blocks drop out of perfect alignment. "
                   "Without the settle they stay dead parallel and the stone "
                   "reads as one lump with grooves cut into it.\n\n"
                   "IT WAS RETUNED AFTER THE FIRST BUILD AND THE NUMBER THAT "
                   "FORCED IT IS FIVE. The draft asked for `flatten` 0.70, "
                   "`spread` 0.70 and `block_relief_m` 0.18 -- reasonable "
                   "settings read one at a time -- and the measured seed-1 "
                   "result was 2.2 x 1.2 x 0.5 m and 232 voxels: a PANCAKE, "
                   "five voxels tall, on a spec whose entire job is to sit "
                   "PROUD of the bottom. Three settings that each remove mass "
                   "vertically had compounded. Pulled back to `flatten` 1.15, "
                   "`spread` 0.45 and `block_relief_m` 0.10 it measures "
                   "2.3 x 1.8 x 1.8 m and 1,505 voxels, and is a mass with gaps "
                   "in it rather than a slab. Recorded because nothing in the "
                   "spec would have told anyone: it built clean and passed "
                   "every check both times.\n\n"
                   "SITTING PROUD IS ALSO A BURIAL NUMBER: `bury` 0.12, the "
                   "lowest of the nine rocks here except the seamount flank. A "
                   "reef that is half in the mud is a boulder field.\n\n"
                   "`rock` rather than `bedrock`, because a reef block is an "
                   "old weathered surface and not a fresh face -- it is the one "
                   "sea-floor rock here that should read the same grey as the "
                   "land ones.",
             size_m=2.2, lumps=5, spread=0.45, flatten=1.15, elongate=1.30,
             angular=0.75, facets=6, rough=0.48, erode=0.22, cavernous=0.10,
             joint_sets=3, joint_scatter=0.12, block_size_m=0.8,
             block_relief_m=0.10, settle_m=0.12,
             bury=0.12, rubble=0.55, mat_rock="rock",
             bio_ocean=1.0, bio_beach=0.4,
             place_abundance=0.5, place_spacing_m=8.0, place_cluster=0.8,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=65),
    ),
    "bedrock-ledge": (
        "rock 3.50 m - a flat-topped shelf in two or three risers, undercut",
        rock("bedrock-ledge",
             notes="TWO PARAMETERS DOING THE WHOLE JOB, and they are the two "
                   "the doc's description names. The RISERS are "
                   "`rock.bedding` at 0.80 with a 0.55 m bed thickness, which "
                   "on a 3.5 m stone is five or six hard-soft cycles -- so the "
                   "soft beds retreat, the hard ones stand proud, and the face "
                   "steps down rather than sloping. The UNDERCUT is "
                   "`rock.notch` at 1.6 in a narrow band low down, which is the "
                   "missing half of every undercut shape and is what a wave or "
                   "a current cuts at the foot of a ledge.\n\n"
                   "FLAT-TOPPED IS `flatten` 0.45 ON A 1.7:1 ELONGATION -- a "
                   "shelf is long, low and one-directional, not a dome. Buried "
                   "to 0.36 so it reads as the edge of bedrock rather than as a "
                   "free-standing slab, which is the same thing "
                   "`leaf-littered-slab` does on a forest floor for the same "
                   "reason.\n\n"
                   "The pinnacle half of the doc's row is NOT authored here. A "
                   "flat-topped shelf and a standing pinnacle are opposite "
                   "shapes -- `flatten` 0.45 against something above 2 -- and "
                   "one spec cannot be both. This is the ledge; a pinnacle "
                   "would be a second spec and is left for whoever wants it.",
             size_m=3.5, lumps=4, spread=0.34, flatten=0.45, elongate=1.70,
             angular=0.70, facets=6, rough=0.36, erode=0.30, cavernous=0.10,
             bedding=0.80, bed_thickness_m=0.55, bed_dip_deg=4.0,
             notch=1.6, notch_z_m=0.35, notch_spread_m=0.18,
             bury=0.36, rubble=0.35, mat_rock="bedrock",
             bio_ocean=1.0, bio_beach=0.4,
             place_abundance=0.35, place_spacing_m=14.0, place_cluster=0.6,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=70),
    ),
    "seamount-flank": (
        "rock 6.00 m - one steep sharp-faceted cone section, no burial at all",
        rock("seamount-flank",
             notes="THE BIGGEST ASSET IN THIS FILE AND THE ONLY ONE WITH NO "
                   "BURIAL. `rock.bury` 0.0 is what the doc's row asks for in "
                   "so many words, and it is unique here: everything else on "
                   "the sea floor settles into mud and this is the mountain the "
                   "mud is lying against. `flatten` 1.60 stands it up, two "
                   "lumps at a very low spread keep it a single mass rather "
                   "than a pile, and seven facets at `angular` 0.75 give the "
                   "sharp unweathered faces of rock that has never been above "
                   "water.\n\n"
                   "`joint_dip_deg` 70 IS DELIBERATE AND IS THE ONE SUBTLE "
                   "SETTING HERE. The parameter's own help explains that joint "
                   "sets are taken IN ORDER and that at dip 0 the first set is "
                   "a horizontal bedding plane -- which would cut horizontal "
                   "shelves into a cone and turn it into a ziggurat. Dipping "
                   "the frame steeply puts the vertical sets first, so the "
                   "faces run down the flank the way they do on a real "
                   "volcanic cone.\n\n"
                   "MEASURED: 20,362 voxels at seed 1, standing "
                   "3.8 x 3.1 x 5.6 m. That is the second largest rock in this "
                   "file -- the 6.5 m sea cave mouth is 32,749 -- and still a "
                   "ninth of the shipped `summit-tor`'s 178,629. Ocean only: a "
                   "seamount by definition never reaches the beach band.",
             size_m=6.0, lumps=2, spread=0.18, flatten=1.60, elongate=1.15,
             angular=0.75, facets=7, rough=0.38, erode=0.18, cavernous=0.0,
             joint_sets=2, joint_scatter=0.10, joint_dip_deg=70,
             bury=0.0, rubble=0.25, mat_rock="bedrock",
             bio_ocean=1.0,
             place_abundance=0.1, place_spacing_m=60.0, place_cluster=0.3,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=70),
    ),
    "sea-cave-mouth": (
        "rock 6.50 m - a dark arch cut into a ledge face; the arch is the asset",
        rock("sea-cave-mouth",
             notes="AUTHORED AT 6.50 m AGAINST THE DOC'S 3-5 m, AND THE REASON "
                   "IS A MEASUREMENT. THIS IS A DEPARTURE FROM THE SOURCE AND "
                   "IT NEEDS THE OWNER'S EYE, NOT MINE.\n\n"
                   "There is a hard gate first: `forge/rock.py:368` runs the "
                   "arch cut only if `rock.size_m` >= 4.0, and the parameter's "
                   "own help says an arch 'is refused below about 4 m because a "
                   "small one is not credible'. So the bottom half of the doc's "
                   "range would have come out as a plain block with no hole in "
                   "it and nothing would have said so.\n\n"
                   "CLEARING THE GATE IS NOT THE SAME AS OPENING A HOLE, and "
                   "that is what the numbers showed. `tools/archprobe.py` "
                   "exists precisely because `_arch` bails silently in four "
                   "places and restores the stone as if nothing happened. Run "
                   "over seeds on this spec's own settings:\n\n"
                   "    4.5 m   3 of 5 seeds opened   (two at 0 and 1 px daylight)\n"
                   "    5.0 m   4 of 6 seeds opened\n"
                   "    6.5 m   6 of 6 seeds opened   894-1,642 px daylight\n"
                   "    8.0 m   5 of 6 seeds opened\n\n"
                   "At the doc's 4.5 m, two individuals in five are a sea cave "
                   "with no cave -- a rock that passes every check in the "
                   "repository and is simply the wrong asset. 6.5 m is the "
                   "smallest size tested at which every seed opens. Note that "
                   "8 m is WORSE than 6.5, so this is not 'bigger is better': "
                   "the fit loop is stochastic and 6.5 m is a measured sweet "
                   "spot rather than a trend.\n\n"
                   "THE TRADE, STATED SO IT CAN BE OVERRULED: a 6.5 m sea cave "
                   "mouth is 30% larger than the doc's top figure, and the "
                   "doc's figures are its own admitted unsourced "
                   "approximations. The alternative is a 4.5 m one that is "
                   "hollow only 60% of the time. If the size matters more than "
                   "the hole, this should go back to 4.5 m and be allow-listed "
                   "rather than quietly shipping both faults at once.\n\n"
                   "THE HOLE IS THE ONLY THING HERE THAT MATTERS, and the doc "
                   "says so: 'the identifying feature is the arch, not the "
                   "rock'. `rock.arch` is the only weathering in the generator "
                   "that makes a THROUGH-GOING hole -- hollows eaten from both "
                   "sides only meet by luck, and a slab thin enough for them to "
                   "meet usually breaks first.\n\n"
                   "Tall and moderately thin so the arch has something to span, "
                   "with light bedding for the horizontal grain a sea cliff has "
                   "and a burial of 0.30 so it reads as the foot of a face "
                   "rather than as a free-standing gateway. The DARKNESS of the "
                   "void is a lighting result and not something the asset can "
                   "carry.",
             size_m=6.5, lumps=3, spread=0.24, flatten=1.35, elongate=1.10,
             angular=0.55, facets=5, rough=0.40, erode=0.35, cavernous=0.20,
             arch=0.75, bedding=0.35, bed_thickness_m=0.60, bed_dip_deg=3.0,
             bury=0.30, rubble=0.30, mat_rock="bedrock",
             bio_ocean=1.0, bio_beach=0.6,
             place_abundance=0.12, place_spacing_m=50.0, place_cluster=0.35,
             place_elev_min_m=-10.0, place_elev_max_m=4.0,
             place_slope_max_pct=70),
    ),
    "rubble-apron": (
        "rock 0.80 m - fine angular scree at a ledge foot; 50 voxels of it",
        rock("rubble-apron",
             notes="THE SMALLEST ROCK IN THE LIBRARY, AND THE 10 cm LATTICE IS "
                   "WHY IT IS BARELY AN OBJECT. Authored at 0.80 m, which is "
                   "the TOP of the doc's 0.3-0.8 m range and was chosen for "
                   "that reason: a rock is locked to the terrain lattice "
                   "(`forge/kinds.py:29-58`), 0.80 m is eight voxels across, "
                   "and the measured result at seed 1 is 7 x 4 x 4 voxels and "
                   "50 voxels total -- a third of `river-cobble`'s 142, which "
                   "was the previous smallest. At the bottom of the doc's range "
                   "it would be three voxels, which is not a shape. Nothing "
                   "here goes "
                   "finer, because a finer rock is not legal at any size and "
                   "authoring this as a detail kind instead would make a scree "
                   "block that is not destructible as terrain -- which is what "
                   "a scree block most obviously should be.\n\n"
                   "AN APRON IS A DISTRIBUTION AND THIS IS ONE CLUMP OF IT. "
                   "`tools/seed_landforms.py` already records the same verdict "
                   "for the blockfield, the talus cone and the rock glacier: "
                   "they are DISTRIBUTIONS of blocks over ground rather than "
                   "one block, which is a placement feature, and faking one as "
                   "a hand-authored mega-boulder is worse than leaving the gap "
                   "visible. So this spec is the individual, and the fan shape "
                   "the doc describes is bought with 1.0 m spacing at maximum "
                   "clustering plus a high abundance.\n\n"
                   "`gravel` for the material, whose high per-voxel jitter is "
                   "what makes a heap read as loose stones rather than as one "
                   "carved lump -- at 50 voxels that jitter is doing more work "
                   "than the geometry is. Beach as well as ocean: a rubble "
                   "apron at the foot of a sea cliff is above the tideline as "
                   "often as below it.",
             size_m=0.8, lumps=7, spread=0.80, flatten=0.40, elongate=1.80,
             angular=0.85, facets=7, rough=0.55, erode=0.20, cavernous=0.0,
             bury=0.30, rubble=0.90, mat_rock="gravel",
             bio_ocean=1.0, bio_beach=0.8, bio_bare_rock=0.3,
             place_abundance=0.9, place_spacing_m=1.0, place_cluster=1.0,
             place_elev_min_m=-10.0, place_elev_max_m=4.0,
             place_slope_max_pct=70),
    ),
    # --- rock: the two corals that really are rocks --------------------------
    "brain-coral": (
        "rock 1.60 m - a pale dome; the maze of grooves is below the lattice",
        rock("brain-coral",
             notes="THE DOC'S FIRST ROUTE, AND IT WORKS: 'geometrically a rock "
                   "with a texture, which is why this one probably IS the rock "
                   "generator plus a palette'. It is. A massive brain coral is "
                   "a boulder-shaped dome, and a dome is what three lumps at a "
                   "0.22 spread with `angular` 0.06 and no facets produce. "
                   "Measured at seed 1: 867 voxels, one piece, no health "
                   "problems. It is the cheapest of the three corals -- against "
                   "1,719 for the plate and 5,302 for the branching thicket -- "
                   "and its DOME needed no retuning at all, which is the "
                   "strongest form the doc's claim could have taken. Only its "
                   "surface did; see below.\n\n"
                   "THE GROOVES ARE THE SPECIES AND THEY ARE NOT THERE. The "
                   "meandering surface maze that names the animal is 5-10 mm "
                   "wide, which is a TENTH of a voxel at the 10 cm a rock is "
                   "locked to -- not close, and not rescuable by a lattice "
                   "change, because the lattice is not free for this kind. "
                   "`rock.flutes` is the only directional weathering the "
                   "generator has and it is the wrong shape twice over: it runs "
                   "grooves DOWNHILL, because it works by running water down "
                   "the outside, and its own help says the finest it reaches is "
                   "decimetre runnels. A brain coral's grooves meander and "
                   "close on themselves and have no downhill.\n\n"
                   "IT IS SET TO 0.70 AND THE FIRST DRAFT'S 0.35 WAS A KNOB "
                   "THAT BARELY MOVED. Measured against the same spec with "
                   "`flutes` 0 at the same seed: 0.35 at a 0.20 m width changed "
                   "the voxel count by -1.4% and the silhouette by 5.8%, which "
                   "on a 1.4 m dome is decoration you would not find in a "
                   "render. 0.70 at 0.16 m takes 37% of the voxels and moves "
                   "the silhouette 15.2%, which is a visibly grooved stone. "
                   "This is the failure shape `tools/rockmech.py` was written "
                   "for -- 'a slider that changes nothing is worse than a "
                   "missing feature, because it looks like a knob and reads as "
                   "tuning' -- and it is the reason every mechanism in this "
                   "file was diffed against itself turned off before it "
                   "shipped. What it gives is a grooved surface, not a maze; "
                   "the maze is a texture ask, not a geometry one.\n\n"
                   "`sand` at (216,200,154) is the palest and warmest of the "
                   "five rock materials and the closest to a live coral head; "
                   "it also carries sandstone's patch mottle, which breaks the "
                   "dome up. It is a cream and a brain coral is more of a "
                   "yellow-ochre. Third spec in this file to ask for a wider "
                   "materials menu.",
             size_m=1.6, lumps=3, spread=0.22, flatten=0.95, elongate=1.12,
             angular=0.06, facets=0, rough=0.30, erode=0.22, cavernous=0.0,
             flutes=0.70, flute_width_m=0.16,
             bury=0.22, rubble=0.10, mat_rock="sand",
             bio_ocean=1.0,
             place_abundance=0.5, place_spacing_m=4.0, place_cluster=0.8,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=45),
    ),
    "plate-coral": (
        "rock 2.00 m - a flat disc on a wasted neck: a mushroom rock in coral",
        rock("plate-coral",
             notes="A TABLE CORAL IS A HOODOO AND `rock.caprock` ALREADY MAKES "
                   "ONE. The doc describes it as 'a single flat horizontal disc "
                   "on a short central stem, like a mushroom cap in stone', and "
                   "the caprock parameter's own help says it 'makes the top of "
                   "the stone much harder than the rest, so weathering eats the "
                   "neck and leaves the head: hoodoos, pedestals and mushroom "
                   "rocks'. That is the same object described twice. Caprock "
                   "0.95 at `cap_frac` 0.55 with `erode` 0.55 and a wide notch "
                   "at the waist gives a hard plate on a thin stalk in one "
                   "pass, and it is the strongest confirmation in this file "
                   "that the doc's rock-with-a-palette route is right for two "
                   "of the three corals.\n\n"
                   "THE PLATE IS THICKER THAN LIFE AND THAT IS THE 10 cm "
                   "LATTICE. A real table coral's plate is 5-10 cm thick, which "
                   "is one voxel at the lattice a rock is locked to, and one "
                   "voxel of horizontal plate would come and go along its own "
                   "length. What is authored is a plate two to three voxels "
                   "thick -- visibly heavier than the animal -- because a plate "
                   "that is intermittent is worse than a plate that is stout. "
                   "Recorded so it is not thinned back.\n\n"
                   "IT WAS RETUNED ONCE AND A TABLE CORAL IS WIDER THAN IT IS "
                   "TALL. The draft stood the blank up hard -- `flatten` 1.30 "
                   "with the cap starting at 0.55 of the height -- on the "
                   "reasoning that the flattening applies to the WHOLE mass "
                   "before the cap and the notch carve it, so a squat blank "
                   "gives a squat disc with no stalk under it. That reasoning "
                   "is right and the setting was too strong: the measured "
                   "seed-1 result was 1.6 x 1.5 x 1.9 m, TALLER than the disc "
                   "was wide, which is a mushroom rock and not a table. At "
                   "`flatten` 0.85 with the cap at 0.40 and a harder notch it "
                   "measures 2.0 x 1.3 x 1.3 m over three seeds -- the plate "
                   "wider than the whole thing is tall, which is the right way "
                   "round.\n\n"
                   "`sand` for the same reason the brain coral uses it, and "
                   "with the same caveat.",
             size_m=2.0, lumps=3, spread=0.22, flatten=0.85, elongate=1.35,
             angular=0.20, facets=2, rough=0.30, erode=0.60, cavernous=0.0,
             caprock=0.95, cap_frac=0.40,
             notch=1.6, notch_z_m=0.32, notch_spread_m=0.22,
             bury=0.10, rubble=0.10, mat_rock="sand",
             bio_ocean=1.0,
             place_abundance=0.4, place_spacing_m=5.0, place_cluster=0.75,
             place_elev_min_m=-10.0, place_elev_max_m=0.0,
             place_slope_max_pct=45),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "sea-floor specs")
    written = 0
    for name, (blurb, changes) in SPECIES.items():
        s, rep = sm.patch(sm.default_spec(), changes)
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          label=name, width=26):
            written += 1
        print(f"  {'':<26} {blurb}")
    print(f"\n{written} of {len(SPECIES)} written")
    print("  sea-fan                    NOT AUTHORED -- see this file's "
          "docstring; the fin plate is the right shape and `fish` is the "
          "wrong kind")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
