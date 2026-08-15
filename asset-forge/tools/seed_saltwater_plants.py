"""Author the sea's plant layer: algae, seagrass, soft corals, anemones and the
splash zone.

Forty-one species. The enumeration, the three decisions behind them and the list
of what could not be built are in `docs/aquatic-species.md`; this file is the
build.

WHY THIS IS POSSIBLE NOW. Until 2026-08-15 the Ocean biome's `hosts` tuple was
`("fish", "cetacean", "bird")` and nothing that grows could stand on the sea
floor. The owner opened it to `rock`, `grass`, `reed` and `bush`. `plantable`
stays False on purpose and the distinction is the point: `plantable` means "a
land plant can root here", which is still no; `hosts` means "an asset of this
kind belongs here", which for kelp and seagrass is plainly yes.

`flower` IS NOT IN THAT TUPLE, and it bites exactly once. A plumose anemone is a
frilly white crown on a bare column, which is a `flower` shape and nothing else,
so it is authored BEACH-ONLY -- harbour walls and pilings, which is a real half
of its range. The seven splash-zone flowers at the foot of this file are Beach
species anyway and are unaffected. Nothing in `spec.validate` enforces this;
`biomes.ocean` accepted 0.8 on a `flower` spec with no warning when it was
tested, so it is a rule kept by hand.

FIVE CENTIMETRES FOR EVERYTHING EXCEPT THE BRANCHING CORALS, which are 2. The
house rule is the coarsest voxel at which the smallest identifying feature is
still about three voxels across. Measured on `branching-stony-coral` at four
lattices: 32,055 voxels at 1 cm, 5,302 at 2, 1,010 at 5 and 397 at 10, with the
branch tip going 5.00 / 2.50 / 1.00 / 0.50 voxels across. Two voxels is the
floor for a thicket that still has gaps in it, so the branching corals are 2 cm
and the rest of the file is 5.

KELP STAYS AT 5 cm AND THE COST WAS MEASURED RATHER THAN FEARED. `giant-kelp` at
its authored 28 m is 18,341 voxels and 1.3 s -- 1.7% of one `temperate-oak`. The
whole of this file together is a fraction of one tree. What 10 cm would cost is
the stipe: 0.10 m through is 2 voxels at 5 cm and 1 at 10, the one-voxel
minimum, at which a 40 m plant is a 40 m thread.

SEAWEED IS NOT A TUFT, AND SAYING SO ONCE HERE IS BETTER THAN SEVENTEEN
SURPRISES. The tuft generator makes a spray of stems from a root crown with an
optional head on TOP of each. It has no along-the-stem foliage, no forking, no
flat blade and no bladder. So:

  * A wrack's paired bladders at each fork are not drawn on any of the four
    wracks here. The fork itself is approximated by many stems from one crown.
  * A kelp's blade is drawn as a WIDE, LOW-TAPER stem -- `width_m` up to 0.20
    against a grass's 0.05 -- which gives the right silhouette and the right
    volume and is a strap rather than a blade with an edge.
  * An anemone's tentacles ARE a tuft, exactly, which is why the anemones came
    out better than the seaweeds and why they are here rather than waiting for a
    generator.

THE COLOUR IS THE WEAKEST THING IN THIS FILE. `materials.stem` offers seven
choices and all seven are land vegetation: grass, savanna grass, dry leaf,
needle, broadleaf, jungle leaf and podzol. There is no red anywhere in it, so
every red alga here is `podzol` (86,72,60), a dark brown, which is the closest
honest thing and is not red. Dulse, coralline turf, laver and the comb-weed all
carry that compromise and each says so. Same shape as the saguaro's missing
green and the coral heads' missing ochre.

THE 20 cm FLOOR (owner). Three species here are genuinely smaller and are
authored up with the arithmetic in their own `notes`: channelled wrack,
coralline turf and dwarf eelgrass. The library's shipped precedent is
`clown-anemonefish` at 2.2x life size; coralline turf is the worst here at up to
4.4x and it says so.

`tuft.base_m` IS NEVER SMALLER THAN `tuft.spread_m` anywhere in this file. The
root crown is what makes a clump ONE PIECE at 26-connectivity and that is what
`tools/buildcheck.py` enforces. A kelp bed, a seagrass meadow and an anemone
carpet are all MANY organisms; what is authored is one plant, and placement
makes the bed.

DEPTH IS IN THE NOTES, IN WORDS, AND THAT IS DELIBERATE. Nothing in a plant spec
records how deep the water is. `detail.depth_min_m` exists, means exactly the
right thing, and is scoped to `('fish','cetacean')` -- but it is writable on a
grass spec with no warning, and all 705 specs on disk already carry the 0.3 m
default, so an authored value would be indistinguishable from a cactus's. See
`docs/aquatic-species.md` §3.3. `placement.elev_min_m` bottoms out at -10.0 m
(measured: -30.0 clamps and warns), so every ocean spec here is pinned there
whether it lives at 2 m or at 40.

    python tools/seed_saltwater_plants.py
    python tools/seed_saltwater_plants.py --force

SIZES ARE APPROXIMATE. Every figure is a typical adult from general knowledge,
the convention `docs/biomes/README.md` §8 sets. Nothing here is measured or
sourced, and nothing is quoted as either.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(kind, res="5", **over):
    changes = {
        "kind": kind,
        "resolution_cm": res,
        "variation.amount": 1.0,
        "variation.height": 0.28,
        "variation.shape": 0.18,
        "variation.proportion": 0.20,
        "variation.rotate": True,
    }
    changes.update(over)
    return changes


def t(**kw):
    """`tuft.*`, `materials.*`, `biomes.*`, `placement.*` from keywords."""
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials." + k[len("mat_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        else:
            out["tuft." + k] = v
    return out


_BUSH_GROUP = {
    "radius_base_m": "trunk", "clear_frac": "trunk", "lean_deg": "trunk",
    "wander": "trunk",
    "shape": "crown", "radius_m": "crown", "height_frac": "crown",
    "center_frac": "crown", "squash": "crown", "asymmetry": "crown",
    "offset": "crown", "points": "crown",
    "model": "growth", "step_m": "growth", "influence_m": "growth",
    "kill_m": "growth", "gravity": "growth", "phototropism": "growth",
    "inertia": "growth", "jitter": "growth", "max_iter": "growth",
    "tip_radius_m": "growth", "radius_exp": "growth",
    "enabled": "foliage", "min_order": "foliage", "clump_radius_m": "foliage",
    "density": "foliage", "rough": "foliage", "coverage": "foliage",
    "separation": "foliage", "clump_jitter": "foliage", "droop_m": "foliage",
    "habit": "foliage", "clustering": "foliage", "top_bias": "foliage",
    "stretch": "foliage",
}


def b(**kw):
    """`trunk.*` / `crown.*` / `growth.*` / `foliage.*` for the bush kinds."""
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials." + k[len("mat_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        elif k == "squash_f":
            out["foliage.squash"] = v
        else:
            out[f"{_BUSH_GROUP[k]}.{k}"] = v
    return out


def bush_base(**over):
    """A soft-bodied colony: branches from the ground, no bole, no leaves."""
    d = dict(clear_frac=0.05, shape="sphere", center_frac=0.55,
             height_frac=0.92, model="colonize", enabled=False,
             kill_m=0.1, jitter=0.1, inertia=0.55, radius_exp=2.0)
    d.update(over)
    return d


# --- the sea floor is pinned at the elevation floor; see the docstring -------
SEA = dict(place_elev_min_m=-10.0, place_elev_max_m=0.0)
SHORE = dict(place_elev_min_m=-10.0, place_elev_max_m=4.0)


SPECIES = {

    # ================================================================
    # KELPS AND LARGE BROWN ALGAE  --  reed, 5 cm
    # ================================================================

    "oarweed": (
        "reed 1.60 m - a stout stipe under a blade split into a hand of straps",
        base("reed", name="oarweed", height_m=1.60,
             notes="THE SPLIT IS THE SPECIES. An oarweed is one blade torn into "
                   "five to nine finger straps by the surf, and it is the split "
                   "that separates it from the sugar kelp beside it -- same "
                   "colour, same holdfast, same habitat, one entire blade "
                   "against a hand of fingers.\n\n"
                   "So it is authored as SEVEN wide stems from one crown rather "
                   "than as one, which is the tuft generator saying 'divided' "
                   "the only way it can. The stipe those fingers share is not "
                   "drawn: every stem here starts at the ground, and a strap "
                   "that begins 40 cm up is not something a tuft can express. "
                   "What is lost is the bare stalk under the blade; what is kept "
                   "is the split, which is worth more.\n\n"
                   "Lower shore and shallow subtidal, roughly 0 to 8 m of water. "
                   "That figure is in this note and not in the spec because no "
                   "field records it -- see the file docstring.",
             **t(stems=7, spread_m=0.06, splay_deg=22, arc=0.50, width_m=0.14,
                 taper=0.70, wander=0.26, length_var=0.30, base_m=0.09,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=1.0, bio_beach=0.5,
                 place_abundance=0.8, place_spacing_m=0.7, place_cluster=0.9,
                 place_slope_max_pct=55, **SHORE)),
    ),
    "dabberlocks": (
        "reed 2.40 m - a long strap with a thick pale midrib down the middle",
        base("reed", name="dabberlocks", height_m=2.40,
             notes="A MIDRIB IS A LINE AND THE GENERATOR DRAWS SOLIDS, so what "
                   "is authored is the midrib itself: three very wide, very "
                   "slightly tapered stems standing near-vertical, which is the "
                   "rib with its wings collapsed onto it. The frilly wings "
                   "either side are sub-voxel at 5 cm on a strap this thin and "
                   "are not attempted.\n\n"
                   "`taper` 0.80 is high on purpose. A kelp blade is very nearly "
                   "the same width for its whole length and then stops; a stem "
                   "that narrows to a point reads as a grass, which is the one "
                   "thing this must not look like.\n\n"
                   "Cold exposed coasts, low shore to about 8 m.",
             **t(stems=3, spread_m=0.06, splay_deg=8, arc=0.45, width_m=0.16,
                 taper=0.80, wander=0.24, length_var=0.28, base_m=0.08,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.6, place_spacing_m=0.9, place_cluster=0.85,
                 place_slope_max_pct=55, **SHORE)),
    ),
    "furbelows": (
        "reed 2.80 m - a huge blade on a twisted stipe from a warty bulb",
        base("reed", name="furbelows", height_m=2.80,
             notes="THE HOLDFAST IS THE FIELD MARK AND `tuft.base_m` IS THE ONLY "
                   "THING HERE THAT COULD DRAW IT. Furbelows grows from a warty "
                   "bulb the size of a football, unmistakable and unlike every "
                   "other kelp's tangle of finger roots. `base_m` is a flat disc "
                   "joining every stem at the ground, and at 0.30 m it is by a "
                   "long way the largest in the library -- which gives a broad "
                   "solid pad under the blades rather than a bulb, because a "
                   "disc has no height. It is the right footprint and the wrong "
                   "profile, and it is the closest the parameter reaches.\n\n"
                   "Five broad blades, strongly arched, on the biggest brown alga "
                   "in cool European water. Lower shore to about 30 m; the spec "
                   "cannot say 30 m, see the docstring.",
             **t(stems=5, spread_m=0.10, splay_deg=14, arc=0.52, width_m=0.20,
                 taper=0.72, wander=0.30, length_var=0.32, base_m=0.30,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=1.0, bio_beach=0.2,
                 place_abundance=0.45, place_spacing_m=1.2, place_cluster=0.8,
                 place_slope_max_pct=50, **SEA)),
    ),
    "sea-palm": (
        "reed 0.55 m - a stout upright trunk with a drooping mop of straps",
        base("reed", name="sea-palm", height_m=0.55,
             notes="A PALM TREE IN THE SURF, and it is authored as one: `arc` "
                   "0.85 is near the ceiling, which lays every strap right over "
                   "at the tip while the base stays upright, because arc is "
                   "weighted toward the tip. That is exactly a palm crown and it "
                   "is what this alga looks like.\n\n"
                   "It lives on the most wave-battered rock there is and stands "
                   "up to it by being short and springy, so it is authored "
                   "SHORT and THICK -- 9 cm stems on a 55 cm plant, proportions "
                   "no land plant here uses.",
             **t(stems=9, spread_m=0.05, splay_deg=6, arc=0.85, width_m=0.09,
                 taper=0.50, wander=0.22, length_var=0.22, base_m=0.08,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=0.7, bio_beach=1.0,
                 place_abundance=0.5, place_spacing_m=0.5, place_cluster=1.0,
                 place_slope_max_pct=65, **SHORE)),
    ),
    "thongweed": (
        "reed 1.80 m - a button on the rock throwing one very long bootlace",
        base("reed", name="thongweed", height_m=1.80,
             notes="A BUTTON AND A BOOTLACE, and the button is `tuft.base_m` at "
                   "0.12 against a 0.08 spread. Thongweed spends its first year "
                   "as a small stalked cup on the rock and then throws a strap "
                   "two metres long out of the middle of it; both stages are on "
                   "the shore at once and the pair is what identifies it.\n\n"
                   "Four thin near-parallel straps with almost no taper, so what "
                   "reads is length rather than shape. The forking of each thong "
                   "into two is not drawn -- a tuft stem does not branch -- and "
                   "the stem count stands in for it.",
             **t(stems=4, spread_m=0.08, splay_deg=14, arc=0.55, width_m=0.05,
                 taper=0.85, wander=0.34, length_var=0.35, base_m=0.12,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=0.8, bio_beach=1.0,
                 place_abundance=0.6, place_spacing_m=0.6, place_cluster=0.95,
                 place_slope_max_pct=60, **SHORE)),
    ),
    "feather-boa-kelp": (
        "reed 3.60 m - one very long midrib strap trailing blades down both sides",
        base("reed", name="feather-boa-kelp", height_m=3.60,
             notes="THE THIRD KELP IN THE LIBRARY THAT LOSES ITS ALONG-THE-STEM "
                   "STRUCTURE, and by now that is a pattern rather than an "
                   "accident: `tuft.head` only ever sits on the TOP of a stem, "
                   "so a plant whose identity is what hangs off the SIDES of a "
                   "midrib -- giant kelp, this, and the wracks below -- comes out "
                   "as a bare strap. That is one missing drawing pass on the "
                   "tuft generator and it would fix six species here at once.\n\n"
                   "What is authored is the boa without its feathers: four long "
                   "low-taper straps, strongly wandering, at nearly four metres. "
                   "Length and drift carry it.",
             **t(stems=4, spread_m=0.07, splay_deg=10, arc=0.58, width_m=0.10,
                 taper=0.80, wander=0.42, length_var=0.34, base_m=0.09,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=1.0,
                 place_abundance=0.5, place_spacing_m=1.1, place_cluster=0.85,
                 place_slope_max_pct=55, **SEA)),
    ),
    "knotted-wrack": (
        "reed 1.00 m - long olive straps with big egg bladders set in them",
        base("reed", name="knotted-wrack", height_m=1.00,
             notes="THE LONGEST-LIVED SEAWEED ON A SHELTERED SHORE and the one "
                   "that makes the deep olive mat you walk on. Its bladders are "
                   "single large eggs set INTO the strap at intervals, not paired "
                   "at a fork like a bladderwrack's, and that difference is the "
                   "whole way the two are told apart in the field.\n\n"
                   "NEITHER IS DRAWN. There is no bladder in the tuft generator "
                   "at any size and no along-the-stem anything, so the two "
                   "species are separated here by what IS expressible: this one "
                   "is longer, laxer, has no midrib and arcs harder; the "
                   "bladderwrack is half the height with twice the splay. That "
                   "is an honest partial answer and the note is here so nobody "
                   "later assumes the bladders were forgotten.",
             **t(stems=14, spread_m=0.10, splay_deg=20, arc=0.62, width_m=0.07,
                 taper=0.75, wander=0.36, length_var=0.38, base_m=0.11,
                 head="none", mat_stem="podzol",
                 bio_beach=1.0, bio_ocean=0.6,
                 place_abundance=0.9, place_spacing_m=0.4, place_cluster=1.0,
                 place_slope_max_pct=60, **SHORE)),
    ),
    "serrated-wrack": (
        "reed 0.45 m - flat forking straps with a saw edge and no bladders",
        base("reed", name="serrated-wrack", height_m=0.45,
             notes="THE ONLY WRACK WITH NO BLADDERS AT ALL, which for once is a "
                   "feature the generator can express perfectly, by omission. It "
                   "is also the limpest: with nothing holding it up it lies flat "
                   "on the rock when the tide is out, so `arc` is 0.72 and the "
                   "splay is the widest of the four wracks here.\n\n"
                   "The saw-toothed edge that names it is a 2-3 mm serration and "
                   "is two orders of magnitude under a 5 cm voxel. Not "
                   "attempted; the limpness carries it.\n\n"
                   "Lowest of the wrack zones, just above the kelp.",
             **t(stems=18, spread_m=0.11, splay_deg=32, arc=0.72, width_m=0.07,
                 taper=0.60, wander=0.40, length_var=0.36, base_m=0.12,
                 head="none", mat_stem="podzol",
                 bio_beach=1.0, bio_ocean=0.5,
                 place_abundance=0.9, place_spacing_m=0.35, place_cluster=1.0,
                 place_slope_max_pct=60, **SHORE)),
    ),
    "spiral-wrack": (
        "reed 0.30 m - short forking straps twisted along their own length",
        base("reed", name="spiral-wrack", height_m=0.30,
             notes="UPPER SHORE, AND SHORT BECAUSE OF IT. This is the wrack that "
                   "spends most of the day out of the water, and everything "
                   "about it is a response to that: short, stiff, thick-walled, "
                   "drying to olive-black rather than staying olive-green.\n\n"
                   "The twist that names it is a rotation about the strap's own "
                   "axis. A tuft stem is a swept capsule with a circular section "
                   "and has no axial rotation to give, so the twist is not drawn "
                   "and `wander` stands in for it -- sideways drift along the "
                   "stem, which reads as irregular rather than as spiralled.\n\n"
                   "Placed high: it is the wrack a player standing on dry rock "
                   "is nearest to.",
             **t(stems=16, spread_m=0.09, splay_deg=28, arc=0.60, width_m=0.06,
                 taper=0.65, wander=0.44, length_var=0.32, base_m=0.10,
                 head="none", mat_stem="podzol",
                 bio_beach=1.0,
                 place_abundance=0.85, place_spacing_m=0.35, place_cluster=1.0,
                 place_slope_max_pct=65,
                 place_elev_min_m=-2.0, place_elev_max_m=4.0)),
    ),
    "channelled-wrack": (
        "reed 0.22 m - very short stiff straps curled into a gutter",
        base("reed", name="channelled-wrack", height_m=0.22,
             notes="AUTHORED UP, AND HERE IS THE ARITHMETIC. Channelled wrack is "
                   "about 0.10-0.15 m in life (an estimate, like every size in "
                   "this file). The owner's floor is 0.20 m, so it is authored "
                   "at 0.22 -- roughly 1.7x life size. That is BELOW the "
                   "library's shipped precedent: `clown-anemonefish` is 22 cm "
                   "against a real 10, which is 2.2x. Written down so the next "
                   "person does not 'correct' it back and delete the species.\n\n"
                   "THE HIGHEST SEAWEED ON ANY SHORE -- above the spiral wrack, "
                   "in the splash zone, out of water for days at a time. The "
                   "channel it rolls itself into holds water while it waits, and "
                   "at 5 cm a 4 mm gutter is a twelfth of a voxel and is not "
                   "drawn. What is authored is very short, very stiff, barely "
                   "arced stems, which is what the plant looks like from a metre "
                   "away.",
             **t(stems=20, spread_m=0.08, splay_deg=24, arc=0.42, width_m=0.05,
                 taper=0.70, wander=0.30, length_var=0.26, base_m=0.09,
                 head="none", mat_stem="podzol",
                 bio_beach=1.0,
                 place_abundance=0.8, place_spacing_m=0.3, place_cluster=1.0,
                 place_slope_max_pct=65,
                 place_elev_min_m=0.0, place_elev_max_m=4.0)),
    ),
    "sargassum-weed": (
        "bush 0.80 m - bushy branching brown alga with berry-sized bladders",
        base("bush", name="sargassum-weed", height_m=0.80,
             notes="THE ONE BROWN ALGA IN THIS FILE THAT IS A BUSH AND NOT A "
                   "TUFT, and the reason is structural rather than aesthetic: "
                   "sargassum genuinely branches, repeatedly, in three "
                   "dimensions, which is what `growth.colonize` does and what a "
                   "tuft cannot do at all. Every other seaweed here is straps "
                   "from a crown and comes out better as a tuft.\n\n"
                   "Foliage OFF. There are no leaves on a brown alga -- the "
                   "branches ARE the plant -- so what ships is bare architecture, "
                   "which is also what `branching-stony-coral` does and for the "
                   "same reason. The berry bladders are 5-8 mm and are not drawn "
                   "at 5 cm.\n\n"
                   "`materials.bark` offers four choices and all four are wood. "
                   "`deadwood` at (134,122,104) is a pale grey-brown and is the "
                   "nearest thing to olive in the menu. Third file in the "
                   "library to ask for a wider bark palette.",
             **b(**bush_base(radius_base_m=0.05, lean_deg=3.0, wander=0.25,
                             radius_m=0.34, tip_radius_m=0.028, step_m=0.08,
                             influence_m=0.40, points=900, gravity=0.10,
                             phototropism=0.35, squash=0.95, asymmetry=0.35,
                             offset=0.14, max_iter=200),
                 mat_bark="deadwood", mat_core="deadwood",
                 bio_ocean=1.0, bio_beach=0.4,
                 place_abundance=0.6, place_spacing_m=0.8, place_cluster=0.9,
                 place_slope_max_pct=55, **SHORE)),
    ),
    "sea-oak-weed": (
        "bush 0.60 m - stiff zig-zag branching with pod-shaped bladders at the tips",
        base("bush", name="sea-oak-weed", height_m=0.60,
             notes="NAMED `sea-oak-weed` AND NOT `sea-oak` ON PURPOSE, because "
                   "the library has seventy-eight trees in it and a species "
                   "called `sea-oak` in a list of them is a trap.\n\n"
                   "Stiffer and more open than the sargassum beside it: a higher "
                   "kill radius spaces the branches out, and a low gravity with "
                   "low phototropism gives the flat zig-zag rather than an "
                   "upward reach. The siliqua pods at the tips -- flattened "
                   "bladders like a pea pod, which is the field mark -- are 1-2 "
                   "cm and are not drawn.\n\n"
                   "AT 0.60 m IT IS THE SMALLEST BUSH IN THIS FILE THAT STILL "
                   "WORKS, and that is not a coincidence: `irish-moss` records "
                   "the four parameter floors that make a bush under about half "
                   "a metre come out as a scribble, and this one sits just "
                   "above them. It needs three of them AT the floor to do it -- "
                   "`step_m` 0.08, `influence_m` 0.40, `kill_m` 0.10 -- with the "
                   "growth point count raised to 1400 to compensate. A first "
                   "draft with all three above their floors built 168 voxels.",
             **b(**bush_base(radius_base_m=0.05, lean_deg=4.0, wander=0.30,
                             radius_m=0.32, tip_radius_m=0.028, step_m=0.08,
                             influence_m=0.40, kill_m=0.10, points=1400,
                             gravity=0.05, phototropism=0.20, inertia=0.30,
                             squash=0.85, asymmetry=0.40, offset=0.16,
                             max_iter=220),
                 mat_bark="deadwood", mat_core="deadwood",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.45, place_spacing_m=0.9, place_cluster=0.8,
                 place_slope_max_pct=55, **SEA)),
    ),

    # ================================================================
    # RED ALGAE
    # ================================================================

    "dulse": (
        "grass 0.40 m - flat deep-red hand-shaped fronds from one short stalk",
        base("grass", name="dulse", height_m=0.40,
             notes="THERE IS NO RED IN `materials.stem`. The seven choices are "
                   "grass, savanna grass, dry leaf, needle, broadleaf, jungle "
                   "leaf and podzol -- all land vegetation, all green, olive or "
                   "brown. `podzol` at (86,72,60) is the darkest and the closest "
                   "to a wet red alga's near-black-crimson, and it is not red. "
                   "Every red alga in this file carries that compromise and this "
                   "is the first of four notes saying so.\n\n"
                   "Six very wide low-taper stems from a tiny crown, splayed "
                   "hard: that is a hand of broad lobes, which is the shape. The "
                   "lobes divide again at their tips in life and do not here.\n\n"
                   "Lower shore and shallow water, very often growing ON kelp "
                   "stipes rather than on rock -- which nothing in `placement` "
                   "can express, so it is placed on the bottom.",
             **t(stems=6, spread_m=0.07, splay_deg=26, arc=0.55, width_m=0.14,
                 taper=0.60, wander=0.28, length_var=0.32, base_m=0.09,
                 head="none", mat_stem="podzol",
                 bio_ocean=0.9, bio_beach=1.0,
                 place_abundance=0.7, place_spacing_m=0.4, place_cluster=0.95,
                 place_slope_max_pct=60, **SHORE)),
    ),
    "irish-moss": (
        "grass 0.25 m - a dense low fan of flat forking blades",
        base("grass", name="irish-moss", height_m=0.25,
             notes="A FAN THAT WANTED TO BE A BUSH AND MEASURED AS A SCRIBBLE. "
                   "Carrageen forks dichotomously -- every branch splits into "
                   "two, over and over -- which is what `growth.colonize` does "
                   "and what a tuft cannot do at all, so this was authored as a "
                   "`bush` first. IT DOES NOT WORK BELOW ABOUT HALF A METRE and "
                   "the numbers are worth recording, because they bind on four "
                   "species in this file:\n\n"
                   "  crown.radius_m      floors at 0.30 m\n"
                   "  growth.influence_m  floors at 0.40 m\n"
                   "  growth.step_m       floors at 0.08 m\n"
                   "  trunk.radius_base_m floors at 0.05 m\n\n"
                   "On a 25 cm plant the crown radius floor is 1.2x the whole "
                   "plant's height, the influence radius is 1.6x it, and one "
                   "growth step is a third of it. Measured: the bush version "
                   "built 102 voxels at seed 1 and a sibling species built 45. "
                   "That is not an asset, it is a scribble.\n\n"
                   "SO IT IS A `grass`, and the tuft's floors are an order of "
                   "magnitude lower. What is lost is the forking, which is the "
                   "plant's actual architecture; what is gained is a dense low "
                   "fan of wide flat splayed blades at the right size, which is "
                   "what it looks like. Twenty-four stems at 0.05 m, splayed 40 "
                   "degrees.\n\n"
                   "The blue iridescence at the tips in sunlight is a "
                   "structural-colour effect, is the single most recognisable "
                   "thing about the species underwater, and is not available in "
                   "any palette here at any lattice.",
             **t(stems=24, spread_m=0.09, splay_deg=40, arc=0.40, width_m=0.05,
                 taper=0.60, wander=0.32, length_var=0.30, base_m=0.10,
                 head="none", mat_stem="podzol",
                 bio_beach=1.0, bio_ocean=0.7,
                 place_abundance=0.8, place_spacing_m=0.3, place_cluster=1.0,
                 place_slope_max_pct=65, **SHORE)),
    ),
    "coralline-turf": (
        "grass 0.22 m - a stiff pink mat of jointed calcified branches",
        base("grass", name="coralline-turf", height_m=0.22,
             notes="THE LARGEST AUTHORED-UP IN THIS FILE AND THE ARITHMETIC IS "
                   "UNCOMFORTABLE. Coralline turf is roughly 0.05-0.10 m in life "
                   "(an estimate). At the owner's 0.20 m floor this is authored "
                   "at 0.22, which is 2.2x at the top of that range and 4.4x at "
                   "the bottom. The library's shipped precedent is 2.2x. This is "
                   "at or past it.\n\n"
                   "IT IS HERE ANYWAY BECAUSE THE ALTERNATIVE IS AN EMPTY LAYER. "
                   "A rock pool with no turf in it is a bare stone bowl, and "
                   "this is the ground cover of every lower-shore pool there is. "
                   "The honest alternatives were a 1 cm lattice -- at which a 5 "
                   "cm plant is five voxels and still not an object -- or a "
                   "material. It should be a material eventually; see "
                   "`docs/aquatic-species.md` §6.\n\n"
                   "Drawn as it feels rather than as it bends: `arc` 0.25 is "
                   "nearly rigid and `taper` 0.80 keeps the branches the same "
                   "thickness throughout, because coralline algae is calcified "
                   "and SNAPS. Everything else in this file bends. Pink is not "
                   "available; `podzol` again.",
             **t(stems=34, spread_m=0.09, splay_deg=34, arc=0.25, width_m=0.03,
                 taper=0.80, wander=0.30, length_var=0.30, base_m=0.10,
                 head="none", mat_stem="podzol",
                 bio_ocean=1.0, bio_beach=0.9,
                 place_abundance=0.9, place_spacing_m=0.25, place_cluster=1.0,
                 place_slope_max_pct=65, **SHORE)),
    ),
    "laver-weed": (
        "grass 0.30 m - thin translucent purple-brown sheets clinging flat",
        base("grass", name="laver-weed", height_m=0.30,
             notes="A SHEET ONE OR TWO CELLS THICK, which is the thinnest thing "
                   "anyone has asked this library to draw and is roughly a "
                   "thousandth of a voxel. What is authored is the sheet's "
                   "OUTLINE given thickness: eight very wide, very limp, heavily "
                   "arced stems that lie over each other, so the mass reads as "
                   "crumpled sheet rather than as blades.\n\n"
                   "`arc` 0.70 with `splay_deg` 30 is deliberately close to the "
                   "sea lettuce beside it, because the two genuinely do look "
                   "alike in shape and differ almost entirely in colour -- "
                   "translucent purple-brown against brilliant green. That is "
                   "the one axis the palette CAN carry, so it does the work: "
                   "`podzol` here against `grass` there.",
             **t(stems=8, spread_m=0.08, splay_deg=30, arc=0.70, width_m=0.15,
                 taper=0.55, wander=0.34, length_var=0.34, base_m=0.10,
                 head="none", mat_stem="podzol",
                 bio_beach=1.0, bio_ocean=0.4,
                 place_abundance=0.7, place_spacing_m=0.35, place_cluster=1.0,
                 place_slope_max_pct=65,
                 place_elev_min_m=-2.0, place_elev_max_m=4.0)),
    ),
    "red-comb-weed": (
        "grass 0.25 m - finely branched red alga combed all to one side",
        base("grass", name="red-comb-weed", height_m=0.25,
             notes="THE SPECIES THAT MEASURED THE BUSH FLOOR. Authored first as "
                   "a `bush`, it built 45, 133, 53, 162 and 123 voxels over "
                   "seeds 1-5 -- a scribble, and the number that sent four "
                   "species in this file to the tuft generator. `irish-moss` "
                   "carries the full arithmetic; the short version is that "
                   "`crown.radius_m` floors at 0.30 m and "
                   "`growth.influence_m` at 0.40 m, both larger than this whole "
                   "plant.\n\n"
                   "COMBED TO ONE SIDE, and that asymmetry is the species -- "
                   "every branchlet comes off the same side of its parent, so "
                   "the plant looks brushed. There is no handedness in the tuft "
                   "generator either. `wander` 0.60 is the highest in this file "
                   "and is what stands in: heavy sideways drift along every "
                   "stem, which reads as combed-and-tangled rather than as "
                   "combed-one-way. An honest half.\n\n"
                   "Very fine (0.022 m, a one-voxel thread at 5 cm) and very "
                   "numerous, which is the other half of what a Plocamium looks "
                   "like. The fourth and last of the red algae that cannot be "
                   "red -- `podzol` again.",
             **t(stems=34, spread_m=0.07, splay_deg=30, arc=0.45, width_m=0.022,
                 taper=0.80, wander=0.60, length_var=0.34, base_m=0.08,
                 head="none", mat_stem="podzol",
                 bio_ocean=1.0, bio_beach=0.4,
                 place_abundance=0.5, place_spacing_m=0.35, place_cluster=0.9,
                 place_slope_max_pct=60, **SEA)),
    ),

    # ================================================================
    # GREEN ALGAE
    # ================================================================

    "sea-lettuce": (
        "grass 0.30 m - broad crumpled sheets of brilliant translucent green",
        base("grass", name="sea-lettuce", height_m=0.30,
             notes="THE BRIGHTEST GREEN IN THE SEA and one of the few marine "
                   "species whose colour the existing palette gets RIGHT: "
                   "`grass` at (96,140,62) is a clean mid green and sea lettuce "
                   "is a clean mid green. After four red algae that had to be "
                   "brown, worth saying.\n\n"
                   "Seven very wide stems -- 0.18 m, the widest in this file "
                   "apart from furbelows -- splayed hard and arced hard, so they "
                   "fold over one another into a crumpled mass. That is the "
                   "shape: it is not blades, it is torn sheet.\n\n"
                   "It grows anywhere with nutrients, which is why it is "
                   "authored at high abundance and low spacing on both shore and "
                   "shallow bottom.",
             **t(stems=7, spread_m=0.07, splay_deg=32, arc=0.68, width_m=0.18,
                 taper=0.50, wander=0.36, length_var=0.34, base_m=0.09,
                 head="none", mat_stem="grass",
                 bio_beach=1.0, bio_ocean=0.8,
                 place_abundance=1.0, place_spacing_m=0.3, place_cluster=1.0,
                 place_slope_max_pct=65, **SHORE)),
    ),
    "gutweed": (
        "grass 0.25 m - bright green gas-filled tubes standing up in a pool",
        base("grass", name="gutweed", height_m=0.25,
             notes="INFLATED, WHICH IS WHY IT STANDS UP. Every other green alga "
                   "on a shore lies flat when the tide goes out; gutweed's "
                   "tubes are full of gas and hold their shape, which is how it "
                   "is told from a sea lettuce at a glance. So it is authored "
                   "near-vertical (`splay_deg` 16, `arc` 0.30) where the sea "
                   "lettuce beside it is at 32 and 0.68.\n\n"
                   "`taper` 0.90 -- the highest in the file -- because a tube is "
                   "the same width at the top as at the bottom, exactly as "
                   "`glasswort` argues for its beads.\n\n"
                   "0.25 m is within the real 0.10-0.30 m range (an estimate) "
                   "and is the top of it. This is NOT an authored-up; it is "
                   "choosing the large end so the species clears the 20 cm "
                   "floor without lying.",
             **t(stems=26, spread_m=0.08, splay_deg=16, arc=0.30, width_m=0.035,
                 taper=0.90, wander=0.24, length_var=0.30, base_m=0.09,
                 head="none", mat_stem="grass",
                 bio_beach=1.0, bio_ocean=0.3,
                 place_abundance=0.9, place_spacing_m=0.25, place_cluster=1.0,
                 place_slope_max_pct=65,
                 place_elev_min_m=-2.0, place_elev_max_m=4.0)),
    ),
    "green-sea-fingers": (
        "grass 0.35 m - spongy dark-green forking cylindrical fingers",
        base("grass", name="green-sea-fingers", height_m=0.35,
             notes="THE ONE SPECIES SENT TO THE TUFT GENERATOR THAT LOST "
                   "NOTHING BY IT. Codium's fingers are ROUND -- a centimetre "
                   "through, velvety, cylindrical -- and a tuft stem is a swept "
                   "capsule with a circular section, so the primitive is "
                   "exactly right. What is lost is the forking, which is real "
                   "but is the same dichotomy at every branch and reads as "
                   "'many fingers from a base' either way.\n\n"
                   "It was authored as a `bush` first and built 119 voxels; see "
                   "`irish-moss` for the floors that cause that and why four "
                   "species in this file moved. Here it is 16 fat stems at "
                   "0.055 m -- one of the widest stem widths in the file -- with "
                   "`taper` 0.85, because a codium finger is the same thickness "
                   "all the way to its blunt end.\n\n"
                   "`leaf_jungle` (58,108,48) is the darkest true green in "
                   "`materials.stem` and codium is a very dark green. For once "
                   "the palette is close.",
             **t(stems=16, spread_m=0.08, splay_deg=30, arc=0.40, width_m=0.055,
                 taper=0.85, wander=0.30, length_var=0.32, base_m=0.10,
                 head="none", mat_stem="leaf_jungle",
                 bio_ocean=1.0, bio_beach=0.8,
                 place_abundance=0.6, place_spacing_m=0.4, place_cluster=0.9,
                 place_slope_max_pct=60, **SHORE)),
    ),
    "halimeda": (
        "grass 0.25 m - chains of small flat calcified discs jointed end to end",
        base("grass", name="halimeda", height_m=0.25,
             notes="THE PLANT THAT MAKES THE SAND. Halimeda lays calcium "
                   "carbonate down in its own segments and drops them; a "
                   "startling share of a tropical beach is dead halimeda, which "
                   "makes it worth having even though it is small.\n\n"
                   "THE SEGMENTS ARE THE SPECIES AND THEY ARE NOT THERE. Each "
                   "disc is 5-10 mm across and 1 mm thick (estimates), which at "
                   "5 cm is a fifth of a voxel; and a chain of them is a string "
                   "of BEADS, which no primitive here makes -- `forge/skeleton.py` "
                   "and the tuft both draw swept capsules, and a capsule has no "
                   "beading. What is authored is the chain as a smooth stem, so "
                   "what ships is a small pale plant of the right size in the "
                   "right place with the wrong surface.\n\n"
                   "It was a `bush` first and built 153 voxels; see `irish-moss` "
                   "for the floors. `taper` 0.85 keeps each chain the same "
                   "width to its tip, which is what a segmented alga does and "
                   "what a blade of grass does not.\n\n"
                   "`leaf_dry` (146,138,74) is the palest, most washed-out green "
                   "in `materials.stem` and is the nearest thing to a calcified "
                   "alga's chalky green. The one place a stone-white would have "
                   "been right and `materials.stem` has no white.",
             **t(stems=20, spread_m=0.08, splay_deg=32, arc=0.35, width_m=0.035,
                 taper=0.85, wander=0.34, length_var=0.30, base_m=0.09,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=1.0,
                 place_abundance=0.7, place_spacing_m=0.35, place_cluster=0.95,
                 place_slope_max_pct=45, **SEA)),
    ),

    # ================================================================
    # SEAGRASSES  --  the only flowering plants fully submerged in the sea
    # ================================================================

    "neptune-grass": (
        "grass 0.90 m - long stiff ribbons over a thick fibrous mat",
        base("grass", name="neptune-grass", height_m=0.90,
             notes="THE LARGEST SEAGRASS AND THE THIRD IN THE LIBRARY, and it is "
                   "authored to differ from the two shipped ones on the axes "
                   "that read at distance rather than on leaf anatomy: taller "
                   "than `eelgrass-meadow` by half, stiffer than either (`arc` "
                   "0.34 against eelgrass's 0.74), and rooted in a much wider "
                   "crown.\n\n"
                   "THE MATTE IS WHY THE CROWN IS WIDE. Neptune grass builds a "
                   "metres-thick mat of its own dead leaf bases underneath "
                   "itself over centuries, and the living blades stand on top of "
                   "it. `tuft.base_m` at 0.16 is a flat disc and not a mat, but "
                   "it is the only thing that puts visible material under the "
                   "blades, and it also does the job the base always does -- "
                   "keeping the clump one connected piece.\n\n"
                   "Meadows down to 30-40 m in clear water, which the spec cannot "
                   "say; `placement.elev_min_m` floors at -10.",
             **t(stems=15, spread_m=0.14, splay_deg=12, arc=0.34, width_m=0.075,
                 taper=0.55, wander=0.22, length_var=0.32, base_m=0.16,
                 head="none", mat_stem="leaf_needle",
                 bio_ocean=1.0,
                 place_abundance=0.9, place_spacing_m=0.4, place_cluster=1.0,
                 place_slope_max_pct=25, **SEA)),
    ),
    "manatee-grass": (
        "grass 0.40 m - the only seagrass with round leaves: a tuft of wires",
        base("grass", name="manatee-grass", height_m=0.40,
             notes="ROUND LEAVES, AND THAT IS THE ONE SEAGRASS FIELD MARK THIS "
                   "GENERATOR GETS FOR FREE. Every other seagrass has a flat "
                   "ribbon blade, which a tuft draws as a wide swept capsule and "
                   "which is therefore always a compromise -- a capsule has a "
                   "circular section. Manatee grass's leaves genuinely ARE "
                   "cylindrical, so here the primitive is exactly right and the "
                   "plant is a tuft of green wires because that is what it is.\n\n"
                   "Authored narrow (0.02 m, a one-voxel thread at 5 cm) and "
                   "numerous, against the turtlegrass's 0.09 m straps. Those two "
                   "grow interleaved on the same Caribbean bottom and this is "
                   "how a player would tell them apart.",
             **t(stems=30, spread_m=0.09, splay_deg=14, arc=0.40, width_m=0.02,
                 taper=0.80, wander=0.28, length_var=0.30, base_m=0.10,
                 head="none", mat_stem="leaf_jungle",
                 bio_ocean=1.0,
                 place_abundance=0.85, place_spacing_m=0.3, place_cluster=0.95,
                 place_slope_max_pct=25, **SEA)),
    ),
    "shoal-grass": (
        "grass 0.25 m - narrow blades with a notched tip; the first coloniser",
        base("grass", name="shoal-grass", height_m=0.25,
             notes="THE PIONEER. Shoal grass takes bare sand that nothing else "
                   "will hold and makes it a meadow, which the other three "
                   "seagrasses then take over; so it is authored at the highest "
                   "abundance and the tightest spacing of the five, and with the "
                   "widest slope tolerance, because it is the one found on "
                   "disturbed unstable bottom.\n\n"
                   "The notched leaf tip that identifies it is 2-3 mm across "
                   "(estimate) and is not drawn at 5 cm. `taper` 0.85 keeps the "
                   "blade parallel-sided to its end, which is the readable half "
                   "of the same fact.",
             **t(stems=28, spread_m=0.08, splay_deg=18, arc=0.44, width_m=0.03,
                 taper=0.85, wander=0.26, length_var=0.28, base_m=0.09,
                 head="none", mat_stem="leaf_needle",
                 bio_ocean=1.0, bio_beach=0.6,
                 place_abundance=1.0, place_spacing_m=0.22, place_cluster=1.0,
                 place_slope_max_pct=35, **SHORE)),
    ),
    "surfgrass": (
        "grass 0.80 m - bright green ribbons growing on ROCK in the surf",
        base("grass", name="surfgrass", height_m=0.80,
             notes="THE ONLY SEAGRASS THAT GROWS ON ROCK, and nothing in "
                   "`placement` can say so. Every other seagrass roots in sand "
                   "or mud; surfgrass grips bare wave-swept stone with a mat of "
                   "rhizomes, and the substrate is most of what identifies it. "
                   "`placement.slope_max_pct` is set to 65 -- the highest of the "
                   "five seagrasses -- which is the nearest available proxy for "
                   "'steep rock' and is not the same statement.\n\n"
                   "Long, limp and combed: `arc` 0.78 is the highest in the "
                   "seagrass set, because surfgrass streams flat with every wave "
                   "and that streaming is how it reads in motion.\n\n"
                   "Brightest green of the seagrasses, so `grass` rather than "
                   "the darker `leaf_needle` the other four use.",
             **t(stems=22, spread_m=0.10, splay_deg=20, arc=0.78, width_m=0.045,
                 taper=0.65, wander=0.32, length_var=0.36, base_m=0.12,
                 head="none", mat_stem="grass",
                 bio_ocean=0.9, bio_beach=1.0,
                 place_abundance=0.8, place_spacing_m=0.35, place_cluster=1.0,
                 place_slope_max_pct=65, **SHORE)),
    ),
    "dwarf-eelgrass": (
        "grass 0.22 m - a short fine intertidal turf on mudflats",
        base("grass", name="dwarf-eelgrass", height_m=0.22,
             notes="AUTHORED UP, WITH THE ARITHMETIC. Real dwarf eelgrass is "
                   "roughly 0.10-0.20 m (an estimate); the owner's floor is "
                   "0.20, so this is 0.22 -- between 1.1x and 2.2x, and at the "
                   "top of its real range it is not an authored-up at all. The "
                   "cheapest of the three enlargements in this file.\n\n"
                   "IT IS THE ONE SEAGRASS A PLAYER WALKS ON. The other four are "
                   "under water permanently; dwarf eelgrass carpets estuarine "
                   "mudflats and is exposed at every low tide, which makes it "
                   "the seagrass most likely to be seen close up and the reason "
                   "it is worth authoring at all at this size.\n\n"
                   "Very fine and very dense: 36 stems at 0.02 m, the highest "
                   "count in the file.",
             **t(stems=36, spread_m=0.09, splay_deg=22, arc=0.52, width_m=0.02,
                 taper=0.75, wander=0.30, length_var=0.30, base_m=0.10,
                 head="none", mat_stem="leaf_needle",
                 bio_beach=1.0, bio_ocean=0.5,
                 place_abundance=1.0, place_spacing_m=0.2, place_cluster=1.0,
                 place_slope_max_pct=20,
                 place_elev_min_m=-3.0, place_elev_max_m=3.0)),
    ),

    # ================================================================
    # BRANCHING AND SOFT CORALS  --  bush at 2 cm; see the docstring
    # ================================================================

    "staghorn-coral": (
        "bush 1.20 m - open antler branching, few thick tines, pale tips",
        base("bush", "2", name="staghorn-coral", height_m=1.20,
             notes="TWO CENTIMETRES, AND THE MEASUREMENT IS THE ARGUMENT. Built "
                   "at four lattices, `branching-stony-coral` -- the same shape "
                   "-- gives 32,055 voxels at 1 cm, 5,302 at 2, 1,010 at 5 and "
                   "397 at 10, with the branch tip 5.00 / 2.50 / 1.00 / 0.50 "
                   "voxels across. At the 10 cm a `rock` is locked to, 92.5% of "
                   "the coral is gone and a 2.5 cm branch is drawn at the "
                   "one-voxel minimum -- four times life size, with the gaps "
                   "between branches closing at the same rate. The gaps ARE a "
                   "staghorn. So this is a `bush`, and the cost of that is real: "
                   "a detail-lattice asset is not destructible as terrain, which "
                   "for reef rock is a genuine loss. The massive corals are "
                   "`rock` for exactly that reason.\n\n"
                   "OPEN, WHICH IS WHAT SEPARATES IT FROM THE SHIPPED THICKET. "
                   "`branching-stony-coral` is a dense mass; a staghorn is a few "
                   "long thick antler tines with daylight between them. That is "
                   "a bigger `kill_m`, a bigger step and a much lower point "
                   "count. `growth.kill_m` has a 0.10 m FLOOR, so no two "
                   "branches in this library can come closer than 10 cm and a "
                   "real staghorn's are half that -- this one is authored at "
                   "0.22, well clear of the floor, so for once the floor does "
                   "not bind.\n\n"
                   "`bark_pale` (186,184,174) for a cream stony skeleton; there "
                   "is no coral colour in the four wood choices.",
             **b(**bush_base(radius_base_m=0.05, lean_deg=4.0, wander=0.22,
                             radius_m=0.62, tip_radius_m=0.030, step_m=0.10,
                             influence_m=0.55, kill_m=0.22, points=260,
                             gravity=0.20, phototropism=0.45, inertia=0.65,
                             squash=0.90, asymmetry=0.35, offset=0.14,
                             max_iter=200, height_frac=0.94),
                 mat_bark="bark_pale", mat_core="bark_pale",
                 bio_ocean=1.0,
                 place_abundance=0.6, place_spacing_m=1.6, place_cluster=0.9,
                 place_slope_max_pct=45, **SEA)),
    ),
    "elkhorn-coral": (
        "bush 1.40 m - broad flattened blade-branches facing into the current",
        base("bush", "2", name="elkhorn-coral", height_m=1.40,
             notes="FLATTENED, AND THE GENERATOR CANNOT FLATTEN A BRANCH. An "
                   "elkhorn's branches are palmate blades -- wide one way and "
                   "thin the other, like a moose's antler -- and every branch "
                   "primitive in `forge/skeleton.py` is a swept capsule with a "
                   "circular section. `crown.squash` flattens the ENVELOPE "
                   "vertically, not the branches, so what is authored is a "
                   "squat wide colony of very THICK round branches, which reads "
                   "as elkhorn in silhouette and is round in the hand.\n\n"
                   "The same shape gap `docs/aquatic-species.md` §8.1 records "
                   "for the gorgonian, one dimension less severe: a sea fan is "
                   "a plane and cannot be faked, an elkhorn branch is a fat oval "
                   "and can be approximated by a fat circle.\n\n"
                   "`tip_radius_m` 0.055 is the thickest branch tip in the file, "
                   "which at 2 cm is 5.5 voxels across.",
             **b(**bush_base(radius_base_m=0.07, lean_deg=5.0, wander=0.20,
                             radius_m=0.75, tip_radius_m=0.055, step_m=0.12,
                             influence_m=0.60, kill_m=0.28, points=180,
                             gravity=0.15, phototropism=0.40, inertia=0.70,
                             squash=0.62, asymmetry=0.45, offset=0.20,
                             max_iter=180, center_frac=0.50),
                 mat_bark="bark_pale", mat_core="bark_pale",
                 bio_ocean=1.0,
                 place_abundance=0.45, place_spacing_m=2.2, place_cluster=0.85,
                 place_slope_max_pct=40, **SEA)),
    ),
    "cold-water-coral": (
        "bush 1.00 m - brilliant white open branching, deep and cold",
        base("bush", "2", name="cold-water-coral", height_m=1.00,
             notes="A REEF WITH NO ALGAE IN IT AT ALL, which is the whole "
                   "interest of Lophelia: it builds mounds hundreds of metres "
                   "across in cold dark water where no photosynthesis is "
                   "possible, so it is white rather than the brown of a tropical "
                   "coral's symbionts. `bark_pale` is the palest material "
                   "available and for once the species genuinely is that "
                   "colour.\n\n"
                   "Finer and more delicate than the staghorn beside it -- "
                   "thinner tips, shorter steps, more of them -- which is the "
                   "structural difference between the two and is expressible.\n\n"
                   "DEPTH IS THE PROBLEM AND IT IS NOT SOLVED. Lophelia reefs "
                   "are at 200-1,000 m. `placement.elev_min_m` floors at -10.0 "
                   "(measured: -30.0 clamps and warns), so this authors -10 like "
                   "every other ocean spec and is indistinguishable from a "
                   "shallow reef coral. `docs/aquatic-species.md` §8.6.",
             **b(**bush_base(radius_base_m=0.04, lean_deg=3.0, wander=0.24,
                             radius_m=0.50, tip_radius_m=0.022, step_m=0.07,
                             influence_m=0.40, kill_m=0.16, points=420,
                             gravity=0.18, phototropism=0.42, inertia=0.60,
                             squash=0.92, asymmetry=0.32, offset=0.12,
                             max_iter=200, height_frac=0.94),
                 mat_bark="bark_pale", mat_core="bark_pale",
                 bio_ocean=1.0,
                 place_abundance=0.5, place_spacing_m=1.4, place_cluster=1.0,
                 place_slope_max_pct=55, **SEA)),
    ),
    "black-coral-tree": (
        "bush 2.00 m - a tall dark feathery colony, finely branched",
        base("bush", "2", name="black-coral-tree", height_m=2.00,
             notes="THE TALLEST CORAL IN THE LIBRARY and the one that is most "
                   "obviously a tree, which is the point `docs/biomes/README.md` "
                   "§3 makes about coral in general -- 'the shortest route to a "
                   "reef is the tree generator with a stone palette and no "
                   "leaves' -- taken to its limit. A black coral colony is a "
                   "2 m feathery bush and `growth.colonize` builds it directly.\n\n"
                   "FINE, WHICH IS WHY IT IS THE ONE THAT MOST WANTS 1 cm. "
                   "`tip_radius_m` 0.015 is 1.5 voxels at the authored 2 cm, "
                   "just clear of the one-voxel minimum; at 1 cm it would be 3 "
                   "and the feathering would read properly. It is left at 2 for "
                   "consistency with the other three branching corals, and "
                   "because a 2 m plant at 1 cm is eight times the voxels for a "
                   "difference visible only up close.\n\n"
                   "`bark` (88,66,48) is the darkest wood and a black coral's "
                   "skeleton is genuinely near-black under its living tissue.",
             **b(**bush_base(radius_base_m=0.04, lean_deg=4.0, wander=0.26,
                             radius_m=0.68, tip_radius_m=0.015, step_m=0.06,
                             influence_m=0.34, kill_m=0.13, points=900,
                             gravity=0.12, phototropism=0.40, inertia=0.55,
                             squash=1.00, asymmetry=0.34, offset=0.14,
                             max_iter=240, height_frac=0.90),
                 mat_bark="bark", mat_core="bark",
                 bio_ocean=1.0,
                 place_abundance=0.3, place_spacing_m=2.5, place_cluster=0.7,
                 place_slope_max_pct=60, **SEA)),
    ),
    "dead-mans-fingers": (
        "grass 0.25 m - fat blunt lobed soft fingers on an encrusting base",
        base("grass", name="dead-mans-fingers", height_m=0.25,
             notes="THE FIFTH SPECIES IN THIS FILE SENT FROM `bush` TO THE TUFT "
                   "GENERATOR, AND THE ONLY ONE THAT FAILED A HEALTH CHECK "
                   "RATHER THAN JUST LOOKING THIN. Authored as a bush at 2 cm "
                   "it built 1,079 voxels at seed 1 -- respectable -- and at "
                   "seed 2 `pipeline.health` rejected it outright: 'bare: the "
                   "trunk never branched'. `irish-moss` records the four "
                   "parameter floors behind that; the one that bites here is "
                   "`crown.radius_m`, which cannot go below 0.30 m, so on a "
                   "20 cm colony every growth target is outside the plant and "
                   "on some seeds the skeleton never reaches one.\n\n"
                   "IT IS A BETTER TUFT THAN IT WAS A BUSH ANYWAY. A soft coral "
                   "is a few FAT BLUNT lobes from an encrusting base -- no "
                   "branching, no forking, no architecture -- which is a spray "
                   "of stems from a crown exactly. `width_m` 0.06 with `taper` "
                   "0.92 gives fingers that are thick and stay thick to their "
                   "rounded ends, which is what separates Alcyonium from every "
                   "seaweed here.\n\n"
                   "Nine lobes rather than dozens, splayed 26 degrees and barely "
                   "arced, so the colony reads as a hand rather than as a "
                   "mop.\n\n"
                   "White to orange in life; `leaf_dry` (146,138,74) is the "
                   "palest entry in `materials.stem` and is a khaki.",
             **t(stems=9, spread_m=0.07, splay_deg=26, arc=0.22, width_m=0.06,
                 taper=0.92, wander=0.18, length_var=0.30, base_m=0.09,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=1.0, bio_beach=0.3,
                 place_abundance=0.7, place_spacing_m=0.5, place_cluster=0.95,
                 place_slope_max_pct=65, **SHORE)),
    ),
    "carnation-soft-coral": (
        "bush 0.40 m - a drooping bunch of translucent pink polyp stalks",
        base("bush", "2", name="carnation-soft-coral", height_m=0.40,
             notes="IT HANGS, AND `growth.gravity` IS WHY IT LOOKS LIKE ITSELF. "
                   "Dendronephthya inflates with water, fans out into the "
                   "current and droops under its own weight -- so gravity is "
                   "authored NEGATIVE (-0.20), which is the only spec in this "
                   "file that does that and which pulls the branches down "
                   "instead of up. Every other coral here reaches.\n\n"
                   "The polyp clusters at the branch ends -- the 'flowers' that "
                   "give it the name -- are 3-5 mm bunches (estimate) and at "
                   "2 cm are a quarter of a voxel. Not drawn; the drooping "
                   "architecture is what is left and it is the recognisable "
                   "half.\n\n"
                   "Pink is not available in any of the four bark materials. "
                   "`bark_pale` is the least wrong.",
             **b(**bush_base(radius_base_m=0.035, lean_deg=8.0, wander=0.30,
                             radius_m=0.26, tip_radius_m=0.020, step_m=0.05,
                             influence_m=0.24, kill_m=0.11, points=340,
                             gravity=-0.20, phototropism=0.10, inertia=0.45,
                             squash=0.95, asymmetry=0.40, offset=0.18,
                             max_iter=160, center_frac=0.60),
                 mat_bark="bark_pale", mat_core="bark_pale",
                 bio_ocean=1.0,
                 place_abundance=0.5, place_spacing_m=0.8, place_cluster=0.85,
                 place_slope_max_pct=70, **SEA)),
    ),
    "leather-coral": (
        "bush 0.50 m - a thick fleshy stalk under a folded rubbery cap",
        base("bush", "2", name="leather-coral", height_m=0.50,
             notes="A TOADSTOOL, WHICH IS THE ONE SHAPE THE BUSH GENERATOR MAKES "
                   "BY ACCIDENT AND NOT ON PURPOSE. A Sarcophyton is one thick "
                   "stalk carrying a broad folded cap, and what produces that "
                   "here is `trunk.clear_frac` 0.35 -- a bare bole for the "
                   "bottom third, which every other bush in this file sets to "
                   "0.05 -- under a heavily squashed crown. That is a tree's "
                   "parameterisation on a 50 cm animal.\n\n"
                   "`crown.squash` 0.45 is the flattest crown in the file and it "
                   "is doing the cap. The folds and lobes on the cap's rim are "
                   "1-2 cm (estimate), which is one voxel at 2 cm, and are not "
                   "drawn.\n\n"
                   "The stalk is genuinely thick -- `radius_base_m` 0.06 on a "
                   "50 cm plant, six voxels through -- because a leather coral's "
                   "stalk is a third of its own cap width.",
             **b(**bush_base(clear_frac=0.35, radius_base_m=0.06, lean_deg=3.0,
                             wander=0.14, radius_m=0.30, tip_radius_m=0.035,
                             step_m=0.06, influence_m=0.26, kill_m=0.13,
                             points=220, gravity=0.05, phototropism=0.45,
                             inertia=0.60, squash=0.45, asymmetry=0.26,
                             offset=0.10, max_iter=140, center_frac=0.72,
                             height_frac=0.62),
                 mat_bark="deadwood", mat_core="deadwood",
                 bio_ocean=1.0,
                 place_abundance=0.55, place_spacing_m=0.9, place_cluster=0.9,
                 place_slope_max_pct=45, **SEA)),
    ),
    "sea-whip": (
        "reed 1.00 m - one or a few unbranched whips standing off the bottom",
        base("reed", name="sea-whip", height_m=1.00,
             notes="THE ONE GORGONIAN SHAPE THIS LIBRARY CAN BUILD. A sea fan is "
                   "a flat rigid NET held across the current -- a plane, which "
                   "nothing here makes, and `docs/aquatic-species.md` §8.1 "
                   "records why. A sea WHIP is the same animal without the net: "
                   "one unbranched rod from a holdfast, which is exactly what a "
                   "reed tuft is. So the family gets its representative at no "
                   "cost.\n\n"
                   "Three stems rather than one, because a single stem from a "
                   "single crown is the most fragile thing this generator "
                   "produces and three of them is what a real colony cluster "
                   "looks like anyway. `taper` 0.88 keeps them parallel-sided; "
                   "`arc` 0.18 keeps them nearly straight, because a sea whip is "
                   "stiffened with a horny axial rod and does NOT stream like a "
                   "kelp.\n\n"
                   "Red and yellow in life; no red in `materials.stem`. "
                   "`savanna_grass` (170,152,78) is the closest to the yellow "
                   "form.",
             **t(stems=3, spread_m=0.06, splay_deg=8, arc=0.18, width_m=0.04,
                 taper=0.88, wander=0.20, length_var=0.30, base_m=0.08,
                 head="none", mat_stem="savanna_grass",
                 bio_ocean=1.0,
                 place_abundance=0.5, place_spacing_m=0.9, place_cluster=0.85,
                 place_slope_max_pct=65, **SEA)),
    ),

    # ================================================================
    # ANEMONES AND OTHER SOFT-BODIED SESSILE ANIMALS
    #
    # Filed with the plants because they occupy a plant's visual role and are
    # built by the plant generators -- NOT because they are plants. Each one
    # says so in its own notes.
    # ================================================================

    "snakelocks-anemone": (
        "grass 0.30 m - a mop of long wavy green tentacles with magenta tips",
        base("grass", name="snakelocks-anemone", height_m=0.30,
             notes="AN ANIMAL, AUTHORED AS A GRASS, AND THE GENERATOR IS EXACTLY "
                   "RIGHT FOR ONCE. Every seaweed in this file is a compromise "
                   "because a tuft has no forking and no along-the-stem "
                   "structure. An anemone is a spray of long soft filaments from "
                   "a common base and NOTHING ELSE -- which is the literal "
                   "definition of `tuft`. This is the best-fitting species in "
                   "the file.\n\n"
                   "Snakelocks is the anemone that never retracts, so the "
                   "tentacles are always out and always waving: `arc` 0.66 and "
                   "`wander` 0.55 -- the highest wander here -- give the "
                   "characteristic loose writhe.\n\n"
                   "The magenta tips are the field mark and there is no per-stem "
                   "tip colour in the tuft generator; `tuft.head` would put a "
                   "blob on every tentacle, which is a different and wrong "
                   "thing. Green body only. `grass` (96,140,62) is close to the "
                   "real symbiont green.",
             **t(stems=44, spread_m=0.07, splay_deg=40, arc=0.66, width_m=0.025,
                 taper=0.85, wander=0.55, length_var=0.30, base_m=0.09,
                 head="none", mat_stem="grass",
                 bio_ocean=0.9, bio_beach=1.0,
                 place_abundance=0.7, place_spacing_m=0.4, place_cluster=0.95,
                 place_slope_max_pct=70, **SHORE)),
    ),
    "magnificent-sea-anemone": (
        "grass 0.70 m - a wide carpet of thick blunt tentacles; clownfish host",
        base("grass", name="magnificent-sea-anemone", height_m=0.70,
             notes="THE HOST, AND THAT IS WHY IT IS WORTH AUTHORING AHEAD OF "
                   "SMALLER ANEMONES. `clown-anemonefish` has shipped since the "
                   "first fish pass with nothing to live in; this is the "
                   "anemone it lives in. Placing the two together is a "
                   "placement matter and nothing here can state the "
                   "association.\n\n"
                   "THE WIDEST THING IN THIS FILE, AND WIDTH IS NOT A "
                   "PARAMETER. A magnificent anemone is a metre across and "
                   "barely 20 cm tall -- a carpet, not a bush -- and "
                   "`height_m` is the only size the tuft generator takes. "
                   "What produces the spread instead is `splay_deg` 62, near "
                   "the 80 ceiling, laying every tentacle almost flat, plus a "
                   "0.20 m root crown. The authored 0.70 m is therefore the "
                   "stem LENGTH and not the animal's height; measured from a "
                   "render it will look about half that tall and twice that "
                   "wide, which is right.\n\n"
                   "Thick blunt tentacles: `width_m` 0.05 and `taper` 0.90, so "
                   "they are the same fat all the way out.",
             **t(stems=52, spread_m=0.18, splay_deg=62, arc=0.55, width_m=0.05,
                 taper=0.90, wander=0.35, length_var=0.26, base_m=0.20,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_ocean=1.0,
                 place_abundance=0.4, place_spacing_m=2.0, place_cluster=0.6,
                 place_slope_max_pct=40, **SEA)),
    ),
    "giant-green-anemone": (
        "grass 0.30 m - a thick column with a flat ring of stubby tentacles",
        base("grass", name="giant-green-anemone", height_m=0.30,
             notes="A COLUMN WITH A CROWN, AND THE COLUMN IS `tuft.base_m`. This "
                   "anemone is a fat cylinder of flesh with the tentacles only "
                   "on top, unlike the snakelocks beside it which is tentacles "
                   "all the way down. `base_m` 0.20 against a 0.10 spread gives "
                   "a broad solid pad under the tentacle ring -- a disc rather "
                   "than a column, because the base has no height, so the "
                   "column reads as squat rather than as tall. That is the "
                   "second time in this file the base has been asked to be a "
                   "3D shape and could only be 2D; the furbelows holdfast was "
                   "the first.\n\n"
                   "Short stubby tentacles in a dense ring: 48 of them, arced "
                   "only 0.34, splayed 50 degrees so they lie out flat around "
                   "the mouth.\n\n"
                   "Wave-swept mid to low shore, so it is placed on steep rock "
                   "and at high abundance.",
             **t(stems=48, spread_m=0.10, splay_deg=50, arc=0.34, width_m=0.035,
                 taper=0.80, wander=0.30, length_var=0.22, base_m=0.20,
                 head="none", mat_stem="leaf_jungle",
                 bio_ocean=0.7, bio_beach=1.0,
                 place_abundance=0.75, place_spacing_m=0.5, place_cluster=0.9,
                 place_slope_max_pct=70, **SHORE)),
    ),
    "plumose-anemone": (
        "flower 0.40 m - a tall smooth column under a dense white frilly crown",
        base("flower", name="plumose-anemone", height_m=0.40,
             notes="BEACH-ONLY, AND NOT BECAUSE OF THE ANIMAL. `flower` is not "
                   "in the ocean biome's `hosts` tuple -- it is ('fish', "
                   "'cetacean', 'bird', 'rock', 'grass', 'reed', 'bush') -- and "
                   "this is the one species in the file that is a `flower` "
                   "shape and nothing else: a bare column with a dense white "
                   "cauliflower crown on top, which is `tuft.head` = 'bloom' "
                   "with a low head share, and which no `grass` parameterisation "
                   "reaches. So it carries beach 1.0 and ocean 0.0.\n\n"
                   "THAT IS NOT A LIE ABOUT THE SPECIES. Metridium is the "
                   "anemone of harbour walls, pilings, pier legs and the "
                   "shallow subtidal -- Beach is a real and probably the "
                   "commonest half of its range. Nothing enforces the `hosts` "
                   "tuple in `spec.validate` (a `flower` spec accepted "
                   "`biomes.ocean` 0.8 with no warning when it was tested), so "
                   "this is a rule kept by hand and it is kept.\n\n"
                   "Nine thick columns, almost no arc, each topped by a broad "
                   "white bloom at 0.18 m -- the crown is nearly half the "
                   "animal's width, which is what makes it read.",
             **t(stems=9, spread_m=0.08, splay_deg=10, arc=0.14, width_m=0.05,
                 taper=0.80, wander=0.18, length_var=0.30, base_m=0.10,
                 head="bloom", head_m=0.18, head_frac=0.30, head_share=1.0,
                 mat_stem="leaf_dry", mat_head="plume_white",
                 bio_beach=1.0,
                 place_abundance=0.5, place_spacing_m=0.6, place_cluster=0.95,
                 place_slope_max_pct=70,
                 place_elev_min_m=-10.0, place_elev_max_m=2.0)),
    ),
    "tube-anemone": (
        "grass 0.45 m - very long fine tentacles from a leathery buried tube",
        base("grass", name="tube-anemone", height_m=0.45,
             notes="THE ONLY ANEMONE HERE THAT LIVES IN MUD RATHER THAN ON "
                   "ROCK, and it is placed accordingly: `slope_max_pct` 20, "
                   "the lowest in the file, because a cerianthid needs soft flat "
                   "bottom to sink its tube into.\n\n"
                   "TWO RINGS OF TENTACLES, ONE LONG AND ONE SHORT, is the "
                   "family's defining anatomy and the tuft generator has one "
                   "ring. `length_var` 0.55 is the highest in the file and it is "
                   "standing in: a wide spread of stem lengths from one crown "
                   "reads as a long outer ring over a short inner one, without "
                   "being it.\n\n"
                   "Very fine and very long -- 0.02 m stems, a one-voxel thread "
                   "at 5 cm, arced hard so they trail. This is the most "
                   "delicate silhouette in the file and the one most likely to "
                   "read as noise if the lattice ever coarsens.",
             **t(stems=40, spread_m=0.07, splay_deg=44, arc=0.72, width_m=0.02,
                 taper=0.88, wander=0.45, length_var=0.55, base_m=0.09,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=1.0,
                 place_abundance=0.4, place_spacing_m=1.2, place_cluster=0.6,
                 place_slope_max_pct=20, **SEA)),
    ),
    "sea-pen": (
        "reed 0.50 m - a single quill stuck upright in mud, feathered both sides",
        base("reed", name="sea-pen", height_m=0.50,
             notes="A FEATHER STUCK IN THE MUD, and the feathering is on the "
                   "SIDES of the quill -- which is the same missing mechanism "
                   "that costs the giant kelp its blades, the feather-boa its "
                   "boa and the wracks their bladders. Four species in this "
                   "file are waiting on one drawing pass: along-the-stem "
                   "foliage on a tuft.\n\n"
                   "What ships is the rachis: three stiff near-vertical quills "
                   "with a slight taper, in soft flat mud. A sea pen without its "
                   "barbs is a stick, and it is here because it is the only "
                   "member of a whole order (the pennatulaceans) the library "
                   "could reach at all, and because the placement -- alone, "
                   "sparse, on featureless deep mud where nothing else in this "
                   "file will go -- is itself worth having.\n\n"
                   "`placement.spacing_m` 3.0 and `abundance` 0.3: sea pen "
                   "fields are sparse and even, not clumped, so `cluster` is "
                   "0.25, the lowest in the file.",
             **t(stems=3, spread_m=0.05, splay_deg=6, arc=0.22, width_m=0.045,
                 taper=0.55, wander=0.18, length_var=0.28, base_m=0.07,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=1.0,
                 place_abundance=0.3, place_spacing_m=3.0, place_cluster=0.25,
                 place_slope_max_pct=15, **SEA)),
    ),
    "feather-duster-worm": (
        "grass 0.25 m - a round crown of fine banded filaments on a tube",
        base("grass", name="feather-duster-worm", height_m=0.25,
             notes="A WORM, AUTHORED AS A GRASS, and the same argument as the "
                   "anemones: the visible part of a sabellid is a radial fan of "
                   "fine filaments from one point, which is a tuft exactly. The "
                   "leathery tube it lives in is under the sand and is not "
                   "drawn.\n\n"
                   "SPLAYED WIDE AND BARELY ARCED, because the crown is held out "
                   "as a stiff flat funnel to filter water rather than trailing "
                   "in it: `splay_deg` 56 with `arc` 0.20, which is a "
                   "combination nothing else in this file uses -- the seaweeds "
                   "are the opposite way round.\n\n"
                   "The banding on each filament is 2-3 mm (estimate) and is not "
                   "drawn at any lattice here. `length_var` is low (0.16) "
                   "because a sabellid's crown is remarkably even, which is "
                   "itself a field mark against the ragged anemones nearby.",
             **t(stems=38, spread_m=0.04, splay_deg=56, arc=0.20, width_m=0.02,
                 taper=0.85, wander=0.25, length_var=0.16, base_m=0.06,
                 head="none", mat_stem="leaf_dry",
                 bio_ocean=1.0, bio_beach=0.7,
                 place_abundance=0.6, place_spacing_m=0.5, place_cluster=0.9,
                 place_slope_max_pct=40, **SHORE)),
    ),

    # ================================================================
    # INTERTIDAL AND SPLASH ZONE  --  Beach biome
    #
    # Nineteen dune and saltmarsh species already ship. What was missing is the
    # ROCK end of the shore rather than the sand end.
    # ================================================================

    "rock-samphire": (
        "flower 0.35 m - fleshy blue-green forked leaves and flat yellow heads",
        base("flower", name="rock-samphire", height_m=0.35,
             notes="THE PLANT OF THE SPLASH ZONE ITSELF -- above the highest "
                   "seaweed and below the first grass, on bare cliff rock that "
                   "is wetted by spray and never by tide. Nothing else in the "
                   "library occupies that band.\n\n"
                   "SUCCULENT, WHICH IS `width_m` AND `taper` TOGETHER. Its "
                   "leaves are fat blue-green fingers, so the stems are wide "
                   "(0.055) and barely taper (0.75), which is the same argument "
                   "`glasswort` makes for its beads and the opposite of every "
                   "grass in the library.\n\n"
                   "`leaf_needle` (46,84,62) is the darkest, bluest green in the "
                   "menu and is the nearest to a samphire's glaucous foliage. "
                   "The flat umbel heads are `bloom` at 0.10 m on half the "
                   "stems, which is what gives a flowering plant its leaves for "
                   "free.\n\n"
                   "STEEP ROCK, AND THE BIOME IT ACTUALLY WANTS IS NOT OPEN "
                   "TO IT. Rock samphire grows on sea-cliff faces, which the "
                   "engine classifies as BARE ROCK once they pass a 70% grade -- "
                   "and bare rock's `hosts` tuple is ('rock', 'bird', "
                   "'quadruped'), with `plantable` False. No plant kind is "
                   "admitted there at all. So `biomes.bare_rock` is 0 and "
                   "`placement.slope_max_pct` is set to 70, the ceiling, which "
                   "puts the species on the steepest ground Beach itself "
                   "reaches. That is the honest expressible half.\n\n"
                   "Nothing in `spec.validate` enforces this -- a weight on a "
                   "biome that does not host the kind is accepted silently -- so "
                   "it is a rule kept by hand and `tools/aquaticprobe.py "
                   "--hosts` is what keeps it. The same rule costs "
                   "`plumose-anemone` its ocean weight.",
             **t(stems=14, spread_m=0.09, splay_deg=34, arc=0.36, width_m=0.055,
                 taper=0.75, wander=0.30, length_var=0.32, base_m=0.10,
                 head="bloom", head_m=0.10, head_frac=0.20, head_share=0.5,
                 mat_stem="leaf_needle", mat_head="skin_yellow",
                 bio_beach=1.0,
                 place_abundance=0.5, place_spacing_m=1.0, place_cluster=0.7,
                 place_slope_max_pct=70,
                 place_elev_min_m=1.0, place_elev_max_m=25.0)),
    ),
    "golden-samphire": (
        "flower 0.40 m - narrow fleshy leaves under bright yellow daisy heads",
        base("flower", name="golden-samphire", height_m=0.40,
             notes="THE OTHER SAMPHIRE, and the pair is worth having because "
                   "they share a habitat and look nothing alike close up: rock "
                   "samphire has forked succulent fingers and a flat umbel, this "
                   "has simple narrow leaves and a proper yellow daisy. Same "
                   "head material, different head SHAPE -- 0.13 m and a higher "
                   "share, so the flowers dominate rather than sit among "
                   "leaves.\n\n"
                   "Lower and wetter than rock samphire: the upper saltmarsh and "
                   "the foot of a sea cliff rather than its face, so "
                   "`slope_max_pct` is 55 and the elevation band starts at "
                   "zero.\n\n"
                   "Stiffer and more upright than its neighbour (`arc` 0.24), "
                   "which is the readable difference at distance.",
             **t(stems=12, spread_m=0.08, splay_deg=24, arc=0.24, width_m=0.04,
                 taper=0.65, wander=0.26, length_var=0.30, base_m=0.09,
                 head="bloom", head_m=0.13, head_frac=0.20, head_share=0.65,
                 mat_stem="leaf_needle", mat_head="skin_yellow",
                 bio_beach=1.0,
                 place_abundance=0.45, place_spacing_m=0.9, place_cluster=0.75,
                 place_slope_max_pct=55,
                 place_elev_min_m=0.0, place_elev_max_m=12.0)),
    ),
    "sea-campion": (
        "flower 0.25 m - a low grey-green cushion studded with white flowers",
        base("flower", name="sea-campion", height_m=0.25,
             notes="A CUSHION, WHICH IS A SHAPE THIS LIBRARY ALREADY KNOWS HOW "
                   "TO MAKE -- `alpine-cushion-flower` is the same build in a "
                   "different place, and a sea campion on a cliff top and a "
                   "cushion saxifrage on a summit are convergent for the same "
                   "reason: constant wind. Very short stems, very high splay "
                   "(46 degrees), very wide crown relative to height.\n\n"
                   "The inflated papery calyx behind each flower is the field "
                   "mark and is 1 cm (estimate) -- a fifth of a voxel at 5 cm, "
                   "not drawn. What is drawn is the density of white against "
                   "grey-green, which is what the plant reads as from more than "
                   "two metres.\n\n"
                   "IT GENUINELY GROWS ON CLIFF LEDGES FAR TOO STEEP FOR SOIL "
                   "AND CANNOT SAY SO. Ground past a 70% grade classifies as "
                   "BARE ROCK, whose `hosts` tuple is ('rock', 'bird', "
                   "'quadruped') and whose `plantable` is False -- no plant kind "
                   "is admitted. A first draft of this spec carried "
                   "`biomes.bare_rock` 0.5 and it was accepted with no warning, "
                   "because nothing validates a weight against the hosts tuple. "
                   "Removed, and `slope_max_pct` left at the 70 ceiling.\n\n"
                   "This is the second species in the file to lose a real half "
                   "of its range to a `hosts` tuple, after `plumose-anemone`. "
                   "Both are one word in a file this pass does not own; both are "
                   "recorded rather than requested.",
             **t(stems=24, spread_m=0.10, splay_deg=46, arc=0.42, width_m=0.03,
                 taper=0.60, wander=0.34, length_var=0.30, base_m=0.11,
                 head="bloom", head_m=0.09, head_frac=0.18, head_share=0.55,
                 mat_stem="leaf_needle", mat_head="plume_white",
                 bio_beach=1.0,
                 place_abundance=0.6, place_spacing_m=0.6, place_cluster=0.85,
                 place_slope_max_pct=70,
                 place_elev_min_m=1.0, place_elev_max_m=60.0)),
    ),
    "sea-beet": (
        "grass 0.50 m - sprawling glossy dark leathery leaves and a dull spike",
        base("grass", name="sea-beet", height_m=0.50,
             notes="THE WILD ANCESTOR OF EVERY BEETROOT, CHARD AND SUGAR BEET "
                   "THERE IS, which is not a shape fact and is the reason it is "
                   "worth having on a shore that already carries nineteen "
                   "species.\n\n"
                   "It is authored as a `grass` and not a `flower` because its "
                   "flowers are green, tiny and negligible -- what you see is "
                   "leaves. Very wide (0.09 m), very glossy, sprawling: `arc` "
                   "0.70 and `splay_deg` 42 lay them out flat over the shingle, "
                   "which is what separates it from the upright saltmarsh "
                   "grasses beside it.\n\n"
                   "`leaf_broadleaf` (78,122,54) rather than a grass green, "
                   "because the leaf is a broad glossy plate and not a blade.",
             **t(stems=11, spread_m=0.10, splay_deg=42, arc=0.70, width_m=0.09,
                 taper=0.55, wander=0.32, length_var=0.34, base_m=0.11,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_beach=1.0,
                 place_abundance=0.5, place_spacing_m=0.9, place_cluster=0.7,
                 place_slope_max_pct=50,
                 place_elev_min_m=0.0, place_elev_max_m=8.0)),
    ),
    "sea-blite": (
        "grass 0.35 m - a stiff shrublet of narrow blue-green succulent leaves",
        base("grass", name="sea-blite", height_m=0.35,
             notes="THE THIRD SUCCULENT ON THIS SHORE, after `glasswort` and "
                   "`sea-purslane`, and the three of them together are the "
                   "middle saltmarsh. It differs from both on one axis that "
                   "reads: it is BUSHY and stiff where the glasswort is a stack "
                   "of upright beads and the purslane is a mat. So the stem "
                   "count is high (30), the splay is wide (38), and the arc is "
                   "moderate -- a dense low dome rather than a spike or a "
                   "carpet.\n\n"
                   "It reddens hard in autumn and there is no seasonal colour in "
                   "any spec in this library, so the summer blue-green is what "
                   "ships. `leaf_needle` for the glaucous cast.\n\n"
                   "Placed on bare saltmarsh mud and drift lines, above the "
                   "cordgrass and below the sea rush.",
             **t(stems=30, spread_m=0.09, splay_deg=38, arc=0.44, width_m=0.03,
                 taper=0.70, wander=0.36, length_var=0.32, base_m=0.10,
                 head="none", mat_stem="leaf_needle",
                 bio_beach=1.0,
                 place_abundance=0.7, place_spacing_m=0.5, place_cluster=0.9,
                 place_slope_max_pct=25,
                 place_elev_min_m=0.0, place_elev_max_m=5.0)),
    ),
    "sea-rush": (
        "reed 0.80 m - stiff sharp-pointed dark cylindrical saltmarsh stems",
        base("reed", name="sea-rush", height_m=0.80,
             notes="THE UPPER SALTMARSH, and the tussock that defines it. "
                   "`smooth-cordgrass` already ships for the LOWER marsh, which "
                   "floods twice a day; sea rush is the band above it that "
                   "floods only on spring tides, and the two mark the tidal "
                   "range between them more clearly than any rock does.\n\n"
                   "SHARP, WHICH IS `taper` 0.30 -- the LOWEST in this file. "
                   "Every other stem here stays nearly the same thickness to its "
                   "tip; a sea rush ends in a genuine needle point that will go "
                   "through a boot, and the taper is the only parameter that "
                   "says so. `arc` 0.12 keeps them dead straight and vertical.\n\n"
                   "Dense: 26 stems in a 0.12 m crown, which is a tussock rather "
                   "than a stand.",
             **t(stems=26, spread_m=0.12, splay_deg=9, arc=0.12, width_m=0.035,
                 taper=0.30, wander=0.16, length_var=0.30, base_m=0.13,
                 head="none", mat_stem="leaf_needle",
                 bio_beach=1.0,
                 place_abundance=0.75, place_spacing_m=0.5, place_cluster=0.9,
                 place_slope_max_pct=20,
                 place_elev_min_m=0.5, place_elev_max_m=6.0)),
    ),
    "saltmarsh-grass": (
        "grass 0.30 m - a short dense grey-green turf over the middle marsh",
        base("grass", name="saltmarsh-grass", height_m=0.30,
             notes="THE LAWN OF THE SALTMARSH, and it is authored as one: the "
                   "tightest spacing in the file (0.18 m) at full abundance and "
                   "full clustering, so placement makes a continuous sward "
                   "rather than clumps. Puccinellia is what grazes down to a "
                   "carpet and covers whole square kilometres of estuary.\n\n"
                   "Short, fine and dense -- 40 stems at 0.02 m, arced 0.5 -- "
                   "which at 5 cm is a chunky vegetation mat rather than "
                   "individual blades, exactly as `tools/lattice_ab.py` measured "
                   "for grass at this lattice. That is the intended look here "
                   "rather than a loss: a turf SHOULD read as a mat.\n\n"
                   "It fills the gap between the cordgrass below it and the rush "
                   "above, and it is the species a player actually walks across "
                   "to reach either.",
             **t(stems=40, spread_m=0.08, splay_deg=26, arc=0.50, width_m=0.02,
                 taper=0.55, wander=0.30, length_var=0.28, base_m=0.09,
                 head="none", mat_stem="leaf_dry",
                 bio_beach=1.0,
                 place_abundance=1.0, place_spacing_m=0.18, place_cluster=1.0,
                 place_slope_max_pct=20,
                 place_elev_min_m=0.0, place_elev_max_m=5.0)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "saltwater plant specs")
    written = 0
    for name, (blurb, changes) in SPECIES.items():
        s, rep = sm.patch(sm.default_spec(), changes)
        if seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=force,
                          label=name, width=26):
            written += 1
        print(f"  {'':<26} {blurb}")
    print(f"\n{written} of {len(SPECIES)} written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
