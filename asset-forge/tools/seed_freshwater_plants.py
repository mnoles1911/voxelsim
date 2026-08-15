"""Author fresh water's plant layer: submerged, floating-leaved, emergent, the
river mosses and the algal mats.

Forty-two species. The enumeration and the decisions are in
`docs/aquatic-species.md`; this file is the build.

THERE IS NO FRESHWATER BIOME AND THERE IS NOT GOING TO BE ONE. The world has
exactly ten biomes and fresh water is not one of them: a river, a pond or a lake
sits INSIDE grassland, temperate forest, taiga or tundra, and the engine
classifies the column by the LAND AROUND THE WATER (`forge/biomes.py:37-42`). So
every species here carries weights on the land biomes whose water it lives in
and NOT on ocean, exactly as the freshwater fish already do -- `brown-trout`
ships at temperate forest 0.9, taiga 0.6, tundra 0.35, grassland 0.3, read out of
the file. This is the same table serving upland tarns, lowland ponds, chalk
streams and rainforest backwaters, separated by weights and nothing else.

`placement.water_max_m` IS THE ONE THING THAT MAKES THESE WATER PLANTS. It is
distance to a watercourse, it is whitelisted for every kind in this file, and 0
means "does not care" -- which is what a meadow buttercup wants and is exactly
wrong here. Every species below sets it, from 1 m for the fully submerged plants
out to 8 m for the marsh flowers. `marsh-marigold` already ships doing this and
is the precedent.

IT IS ALSO THE ONLY WATER PARAMETER THERE IS, AND IT IS NOT DEPTH. A submerged
pondweed, a floating water lily and an emergent cattail occupy three different
DEPTHS of water and nothing in a plant spec records which. `detail.depth_min_m`
exists, means exactly the right thing, and is scoped to `('fish','cetacean')` --
but it is writable on a `grass` spec with no warning, and all 705 specs on disk
already carry its 0.3 m default, so an authored value would be indistinguishable
from a cactus's. See `docs/aquatic-species.md` §3.3. Each species says its depth
band in its own `notes`, in words, which is a comment and not a contract.

FIVE CENTIMETRES THROUGHOUT. Everything here is a tuft kind on the detail
lattice, and the shipped ground-cover practice is 5 cm. `tools/lattice_ab.py`
measured what that costs: a reed loses almost nothing because a 2 m stem has
voxels to spare, and grass stops being individual blades and becomes a chunky
vegetation clump. Underwater that is if anything MORE correct -- a pondweed bed
seen through water is a mass, not a set of leaves.

THE TUFT GENERATOR HAS NO WHORLS, NO PAD AND NO PINNA, and three whole groups
here are defined by one of those:

  * WHORLED submerged plants -- hornwort, milfoil, waterweed, stonewort, fanwort
    -- carry rings of fine leaves at intervals up a stem. `tuft.head` sits on
    the TOP of a stem only. So they are authored as many fine stems from one
    crown, which gives the right MASS and the right fineness and has no whorls
    in it. Five species, one missing drawing pass.
  * A LILY PAD is a flat horizontal disc lying ON a surface the asset does not
    contain. `tuft.head` = 'bloom' at pad diameter is the nearest thing and it
    reads as a lily from above and as a bouquet from the side. Four species.
  * A PINNATE LEAF -- watercress, marsh cinquefoil, bogbean -- is leaflets in
    ranks along a midrib. Same missing pass as the whorls, and
    `tools/seed_groundcover.py` already records it for the ferns.

Saying this once here is better than fourteen surprises.

`materials.head` AND `materials.stem` ARE TWO DIFFERENT MENUS, which caught
five species here on the first run. `stem` offers seven land-vegetation
greens and browns; `head` offers fourteen and they do NOT overlap -- there is
no `leaf_jungle` and no `podzol` in `head`. A head material outside its menu
is not refused, it is SILENTLY REPLACED WITH `leaf_blossom`, which is a pink
at (226,168,190). So the first draft of this file shipped a brown-cigar
cattail, a black-spiked pond sedge and two sets of lotus pads all wearing
blossom pink, and every one of them validated clean. That is this project's
signature failure exactly -- it ran, it reported success, and it changed the
thing it was told not to. The warning IS printed by the seed script; it is
worth reading rather than scrolling past.

THERE IS NO DARK BROWN IN `materials.head`. A cattail's cigar and a pond
sedge's flower spike are both near-black brown; `leaf_autumn` (192,122,46) is
the darkest warm entry and is an orange. Both species carry that and say so.

THE 20 cm FLOOR (owner). Four species are genuinely smaller and are authored up
with the arithmetic in their own `notes`: water-starwort, quillwort, needle
spike-rush and the two river mosses. The mosses are the worst in the library at
2-7x, against a shipped precedent of 2.2x (`clown-anemonefish`), and they say so
and say why.

`tuft.base_m` IS NEVER SMALLER THAN `tuft.spread_m`. The root crown is what makes
a clump one connected piece at 26-connectivity, which `tools/buildcheck.py`
enforces. A pondweed bed and a lily pond are placement results; what is authored
is one plant.

    python tools/seed_freshwater_plants.py
    python tools/seed_freshwater_plants.py --force

SIZES ARE APPROXIMATE. `docs/biomes/README.md` §8 sets the convention: typical
adult figures from general knowledge, unsourced, good enough to choose a lattice
and not good enough to quote.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(kind, **over):
    changes = {
        "kind": kind,
        "resolution_cm": "5",
        "variation.amount": 1.0,
        "variation.height": 0.28,
        "variation.shape": 0.18,
        "variation.proportion": 0.20,
        "variation.rotate": True,
    }
    changes.update(over)
    return changes


def t(**kw):
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


SPECIES = {

    # ================================================================
    # SUBMERGED -- rooted or free-floating below the surface
    # ================================================================

    "curled-pondweed": (
        "grass 0.60 m - translucent strap leaves with strongly crisped edges",
        base("grass", name="curled-pondweed", height_m=0.60,
             notes="THE COMMONEST POND PLANT IN THE TEMPERATE WORLD and the "
                   "first fully submerged freshwater species in the library. "
                   "Its wavy crisped leaf edge is the field mark and is a 2-3 mm "
                   "undulation (estimate), a twentieth of a voxel at 5 cm -- so "
                   "what is authored is the leaf's OUTLINE: wide, limp, heavily "
                   "arced straps that lie over each other.\n\n"
                   "`arc` 0.80 is the highest in this file and it is the whole "
                   "difference between a submerged plant and an emergent one. "
                   "Water holds a pondweed up; air does not, and nothing here "
                   "knows that -- an underwater plant drawn with a reed's "
                   "stiffness reads as a reed. Every submerged species below is "
                   "arced past 0.60 for this reason and it is the single most "
                   "important shared decision in the file.\n\n"
                   "0.5-3 m of water, rooted in silt. Placed hard against water "
                   "(`water_max_m` 1) on nearly flat ground.",
             **t(stems=18, spread_m=0.09, splay_deg=28, arc=0.80, width_m=0.05,
                 taper=0.60, wander=0.40, length_var=0.36, base_m=0.10,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_temperate_forest=0.9, bio_grassland=0.9, bio_taiga=0.5,
                 place_abundance=0.9, place_spacing_m=0.4, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=15)),
    ),
    "canadian-waterweed": (
        "grass 0.40 m - dense whorls of three blunt leaves up a brittle stem",
        base("grass", name="canadian-waterweed", height_m=0.40,
             notes="WHORLED, AND THERE ARE NO WHORLS IN THE TUFT GENERATOR. "
                   "Elodea carries rings of three small leaves every centimetre "
                   "up a brittle stem, and `tuft.head` only ever sits on the TOP "
                   "of a stem -- there is no along-the-stem foliage anywhere in "
                   "this generator. Five submerged species in this file are "
                   "whorled and none of them gets its whorls; one drawing pass "
                   "would fix all five, and the same pass fixes the giant kelp's "
                   "blades in `tools/seed_saltwater_plants.py`.\n\n"
                   "So it is authored as MASS instead: 40 very fine stems from a "
                   "small crown, which at 5 cm is a dense green thicket. That is "
                   "how an Elodea bed reads through half a metre of water "
                   "anyway, so the loss is smaller here than it would be on a "
                   "specimen render.\n\n"
                   "Distinguished from the milfoil beside it by being SHORTER "
                   "and DENSER -- 40 stems on 0.40 m against 22 on 1.00 -- which "
                   "is the pair's real difference and is expressible.\n\n"
                   "0.3-3 m of water; it will grow in almost anything.",
             **t(stems=40, spread_m=0.07, splay_deg=24, arc=0.68, width_m=0.02,
                 taper=0.85, wander=0.42, length_var=0.30, base_m=0.08,
                 head="none", mat_stem="leaf_jungle",
                 bio_temperate_forest=1.0, bio_grassland=0.9, bio_taiga=0.5,
                 place_abundance=1.0, place_spacing_m=0.3, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=15)),
    ),
    "rigid-hornwort": (
        "grass 0.60 m - stiff brittle dark whorls of forked bristle leaves",
        base("grass", name="rigid-hornwort", height_m=0.60,
             notes="ROOTLESS, WHICH NOTHING HERE CAN SAY. Ceratophyllum has no "
                   "roots at all -- it hangs in mid-water, drifting, anchored by "
                   "nothing -- and every asset in this library stands on the "
                   "ground. `tuft.base_m` is a root crown and this plant does "
                   "not have one. It is authored rooted because the alternative "
                   "is not authoring it, and because a hornwort mass usually "
                   "does lie against the bed.\n\n"
                   "STIFF AND BRITTLE, which is the one thing that separates it "
                   "from the milfoil at a glance: hornwort snaps and milfoil is "
                   "limp. `arc` 0.42 is the LOWEST of the submerged plants here "
                   "-- everything else in this section is past 0.60 -- and "
                   "`taper` 0.88 keeps the bristles the same thickness "
                   "throughout, which is what brittle looks like.\n\n"
                   "Whorled, and see `canadian-waterweed` for why the whorls are "
                   "not drawn. Dark, almost black-green in life: `leaf_jungle` "
                   "(58,108,48) is the darkest green in `materials.stem`.\n\n"
                   "0.5-5 m of water, and it tolerates shade better than "
                   "anything else here, which is why it is the deep-water one.",
             **t(stems=34, spread_m=0.08, splay_deg=32, arc=0.42, width_m=0.022,
                 taper=0.88, wander=0.50, length_var=0.34, base_m=0.09,
                 head="none", mat_stem="leaf_jungle",
                 bio_temperate_forest=0.9, bio_grassland=1.0,
                 bio_rainforest=0.5,
                 place_abundance=0.8, place_spacing_m=0.4, place_cluster=0.95,
                 place_water_max_m=1, place_slope_max_pct=15)),
    ),
    "spiked-water-milfoil": (
        "grass 1.00 m - feathery whorls up a long limp stem, spike at the top",
        base("grass", name="spiked-water-milfoil", height_m=1.00,
             notes="THE ONE SUBMERGED PLANT HERE THAT BREAKS THE SURFACE, and it "
                   "is the only one that gets a `tuft.head`. A milfoil's "
                   "flowering spike is held CLEAR of the water on a bare stalk "
                   "while the whole rest of the plant is under it -- so a "
                   "'spike' head on a third of the stems is not decoration, it "
                   "is the single visible sign of the plant from a bank, and it "
                   "is what a player will actually see.\n\n"
                   "`head_share` 0.35 leaves two thirds of the stems plain, "
                   "which is how a flowering plant gets its leaves for free and "
                   "is the shipped trick from `tools/seed_wildflowers.py`.\n\n"
                   "The tallest submerged plant in the file at a metre, limp "
                   "(`arc` 0.72) and very fine. Whorled, and the whorls are not "
                   "drawn; see `canadian-waterweed`.\n\n"
                   "1-3 m of water, in still or slow lowland water.",
             **t(stems=22, spread_m=0.09, splay_deg=20, arc=0.72, width_m=0.025,
                 taper=0.80, wander=0.44, length_var=0.36, base_m=0.10,
                 head="spike", head_m=0.07, head_frac=0.10, head_share=0.35,
                 mat_stem="leaf_needle", mat_head="plume_crimson",
                 bio_grassland=1.0, bio_temperate_forest=0.9, bio_taiga=0.4,
                 place_abundance=0.85, place_spacing_m=0.45, place_cluster=0.95,
                 place_water_max_m=1, place_slope_max_pct=15)),
    ),
    "river-water-crowfoot": (
        "flower 1.40 m - trailing thread leaves combed downstream, white flowers",
        base("flower", name="river-water-crowfoot", height_m=1.40,
             notes="THE PLANT THAT MAKES A CHALK STREAM LOOK LIKE A CHALK "
                   "STREAM. Long streamers of thread-fine leaves lie combed flat "
                   "downstream all summer, and in May the surface is covered in "
                   "white five-petalled flowers sitting ON the water. Nothing "
                   "else in the library is both submerged and conspicuously "
                   "flowering.\n\n"
                   "`arc` 0.88 IS THE HIGHEST VALUE IN EITHER AQUATIC FILE, and "
                   "it is doing the current: arc is weighted toward the tip, so "
                   "the base stays upright and the whole length lies over. That "
                   "is exactly what a crowfoot streamer does and it is the one "
                   "place the parameter's tip-weighting is the physically right "
                   "shape rather than a convenient one.\n\n"
                   "IT HAS NO DIRECTION. Every streamer in a real stand points "
                   "the same way, downstream, and `variation.rotate` points each "
                   "individual randomly. There is no flow direction in "
                   "`placement` and a bed of these will look combed "
                   "individually and scattered collectively.\n\n"
                   "Fast shallow water, 0.2-1 m, over gravel. `slope_max_pct` 25 "
                   "rather than 15, because unlike every other submerged plant "
                   "here it wants moving water and moving water wants a "
                   "gradient.",
             **t(stems=20, spread_m=0.10, splay_deg=30, arc=0.88, width_m=0.02,
                 taper=0.85, wander=0.35, length_var=0.40, base_m=0.11,
                 head="bloom", head_m=0.08, head_frac=0.10, head_share=0.30,
                 mat_stem="leaf_jungle", mat_head="plume_white",
                 bio_temperate_forest=1.0, bio_grassland=0.8, bio_taiga=0.3,
                 place_abundance=0.8, place_spacing_m=0.5, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=25)),
    ),
    "common-stonewort": (
        "grass 0.35 m - brittle grey-green calcified whorls, no true roots",
        base("grass", name="common-stonewort", height_m=0.35,
             notes="NOT A PLANT. Chara is a green ALGA that looks like a "
                   "horsetail, has no true roots, no flowers and no vascular "
                   "tissue, and encrusts itself in lime until it is rough and "
                   "grey and snaps in the hand. It is here because a clear "
                   "hard-water lake bottom is a stonewort meadow and nothing "
                   "else looks like one.\n\n"
                   "GREY-GREEN IS THE FIELD MARK AND THE PALETTE NEARLY HAS IT. "
                   "`leaf_dry` (146,138,74) is the palest and most washed-out "
                   "entry in `materials.stem`, which against the "
                   "`leaf_jungle` of the hornwort beside it is a visible "
                   "difference in exactly the right direction. Most of the "
                   "colour compromises in these two files go the other way, so "
                   "this one is worth recording.\n\n"
                   "Stiff (`arc` 0.30) and untapered (`taper` 0.88), because "
                   "calcified means brittle -- the same argument the coralline "
                   "turf makes in the saltwater file.\n\n"
                   "0.5-4 m of clear calcareous water. It is a pioneer on bare "
                   "marl and is often the first thing on a new lake bed.",
             **t(stems=32, spread_m=0.08, splay_deg=26, arc=0.30, width_m=0.025,
                 taper=0.88, wander=0.34, length_var=0.30, base_m=0.09,
                 head="none", mat_stem="leaf_dry",
                 bio_grassland=1.0, bio_temperate_forest=0.8,
                 bio_tundra_alpine=0.4,
                 place_abundance=0.8, place_spacing_m=0.35, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=12)),
    ),
    "ribbon-weed": (
        "grass 0.80 m - very long flat ribbons rising straight from the bed",
        base("grass", name="ribbon-weed", height_m=0.80,
             notes="THE FRESHWATER ANSWER TO EELGRASS, and it is authored to "
                   "read as one: long flat parallel-sided ribbons rising "
                   "straight off the bed and spiralling where they reach the "
                   "surface. `eelgrass-meadow` in the sea and this in a river "
                   "are convergent shapes in unrelated families, which is why "
                   "the parameters are close (`arc` 0.76 against 0.74) and why "
                   "the difference that matters is the BIOME WEIGHTS: that one "
                   "is ocean, this one is rainforest, savanna and grassland.\n\n"
                   "The spiral at the surface is a rotation about the ribbon's "
                   "own axis. A tuft stem is a swept capsule with a circular "
                   "section and has no axial rotation to give, so it is not "
                   "drawn -- the same limit `spiral-wrack` records in the "
                   "saltwater file.\n\n"
                   "0.5-4 m, warm slow water. Tropical and subtropical, which is "
                   "why the weights avoid taiga and tundra entirely.",
             **t(stems=20, spread_m=0.09, splay_deg=14, arc=0.76, width_m=0.04,
                 taper=0.75, wander=0.30, length_var=0.38, base_m=0.10,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_rainforest=1.0, bio_savanna=0.8, bio_grassland=0.5,
                 place_abundance=0.85, place_spacing_m=0.4, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=15)),
    ),
    "water-starwort": (
        "grass 0.25 m - a pale rosette of spoon leaves floating flat",
        base("grass", name="water-starwort", height_m=0.25,
             notes="AUTHORED UP: real Callitriche is 0.10-0.20 m (estimate) "
                   "against the owner's 0.20 m floor, so 0.25 is 1.25x to 2.5x. "
                   "The shipped precedent is 2.2x (`clown-anemonefish`).\n\n"
                   "IT IS TWO PLANTS AT ONCE AND ONLY ONE IS DRAWN. A starwort "
                   "has narrow submerged leaves down the stem and a flat rosette "
                   "of broad spoon leaves floating on the surface, and the "
                   "rosette is what identifies it from a bank. `tuft` gives one "
                   "stem shape and one head position, so what is authored is "
                   "the rosette -- very short, very wide-splayed (52 degrees), "
                   "heavily arced stems that lie flat -- and the submerged tail "
                   "is not there.\n\n"
                   "The smallest plant in this file and the one most likely to "
                   "read as a smear at 5 cm. It is here because a shallow muddy "
                   "ditch or a stream margin with nothing in it is the wrong "
                   "place, and this is what is in one.",
             **t(stems=22, spread_m=0.08, splay_deg=52, arc=0.72, width_m=0.035,
                 taper=0.60, wander=0.34, length_var=0.28, base_m=0.09,
                 head="none", mat_stem="grass",
                 bio_temperate_forest=1.0, bio_grassland=0.9, bio_taiga=0.5,
                 place_abundance=0.8, place_spacing_m=0.3, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=12)),
    ),
    "fanwort": (
        "grass 0.50 m - opposite pairs of fan-shaped leaves, flat as a hand",
        base("grass", name="fanwort", height_m=0.50,
             notes="THE FLATTEST SUBMERGED LEAF THERE IS. Cabomba's leaves are "
                   "finely divided into a semicircular FAN held in one plane, "
                   "opposite pairs up the stem -- so a stand of it looks like "
                   "rows of green hands. That planarity is the species and there "
                   "is no plane in the tuft generator; every stem is a round "
                   "swept capsule and the fans are not drawn.\n\n"
                   "What IS authored is the fineness and the regularity: 30 very "
                   "fine stems with a LOW `length_var` (0.18, the lowest in this "
                   "file), because a fanwort stand is unusually even where "
                   "everything else here is ragged. Evenness is the one half of "
                   "the field mark that a tuft can carry.\n\n"
                   "Warm still water 0.5-3 m; a tropical and subtropical plant "
                   "and famously invasive well outside that.",
             **t(stems=30, spread_m=0.08, splay_deg=22, arc=0.66, width_m=0.02,
                 taper=0.85, wander=0.30, length_var=0.18, base_m=0.09,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_rainforest=1.0, bio_savanna=0.7, bio_grassland=0.3,
                 place_abundance=0.7, place_spacing_m=0.35, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=12)),
    ),
    "quillwort": (
        "grass 0.22 m - a rosette of stiff dark quills in a cold stony lake",
        base("grass", name="quillwort", height_m=0.22,
             notes="AUTHORED UP: real Isoetes is 0.05-0.20 m (estimate) against "
                   "a 0.20 m floor, so 0.22 is 1.1x to 4.4x depending where in "
                   "that range the individual sits. At the top it is honest; at "
                   "the bottom it is past the library's shipped 2.2x ceiling and "
                   "this note is where that is admitted.\n\n"
                   "IT IS HERE FOR A PLACE RATHER THAN FOR ITSELF. An "
                   "oligotrophic upland tarn -- cold, clear, acid, stony, almost "
                   "nutrient-free -- has essentially nothing growing in it, and "
                   "a quillwort lawn is what it does have. Without this the "
                   "tundra and taiga lakes in the world are bare stone bowls, "
                   "which is the same argument `coralline-turf` makes in the "
                   "saltwater file.\n\n"
                   "Stiff dark cylindrical quills in a tight rosette: `arc` "
                   "0.24, `splay_deg` 34, `taper` 0.75, and the widest stems of "
                   "any submerged plant here relative to height. It is the only "
                   "species in this file weighted to tundra above taiga.",
             **t(stems=18, spread_m=0.06, splay_deg=34, arc=0.24, width_m=0.03,
                 taper=0.75, wander=0.20, length_var=0.26, base_m=0.07,
                 head="none", mat_stem="leaf_needle",
                 bio_tundra_alpine=1.0, bio_taiga=0.8,
                 bio_temperate_forest=0.3,
                 place_abundance=0.7, place_spacing_m=0.3, place_cluster=0.95,
                 place_water_max_m=1, place_slope_max_pct=12)),
    ),
    "water-soldier": (
        "grass 0.40 m - an aloe-like rosette of stiff saw-edged spiny leaves",
        base("grass", name="water-soldier", height_m=0.40,
             notes="IT RISES AND SINKS WITH THE SEASON, which is the most "
                   "un-asset-like behaviour of anything in this library: "
                   "Stratiotes floats at the surface to flower in summer and "
                   "sinks to the bottom for the winter. A spec is one static "
                   "shape and there is no seasonality anywhere here, so what "
                   "ships is the summer plant, rooted.\n\n"
                   "A ROSETTE OF SWORDS, and it is the stiffest submerged plant "
                   "in the file: `arc` 0.20 and `taper` 0.35, so the leaves are "
                   "rigid and taper to a genuine point. That combination "
                   "appears nowhere else here -- every other submerged plant is "
                   "limp and parallel-sided -- and it is what makes a water "
                   "soldier look like an aloe that fell in a pond.\n\n"
                   "The saw teeth down each leaf edge are 2-3 mm (estimate) and "
                   "are not drawn at 5 cm.\n\n"
                   "0.5-2 m, still calcareous water in ditches and dykes.",
             **t(stems=16, spread_m=0.08, splay_deg=36, arc=0.20, width_m=0.05,
                 taper=0.35, wander=0.16, length_var=0.24, base_m=0.09,
                 head="none", mat_stem="leaf_needle",
                 bio_temperate_forest=1.0, bio_grassland=0.7,
                 place_abundance=0.5, place_spacing_m=0.7, place_cluster=0.9,
                 place_water_max_m=2, place_slope_max_pct=12)),
    ),
    "needle-spike-rush": (
        "grass 0.22 m - a fine bright-green lawn of hair-thin quills",
        base("grass", name="needle-spike-rush", height_m=0.22,
             notes="AUTHORED UP: real Eleocharis acicularis is 0.03-0.10 m "
                   "(estimate) against a 0.20 m floor, so this is 2.2x to 7x. "
                   "That is the second-worst enlargement in either aquatic file "
                   "-- only the river mosses are worse -- and it is stated "
                   "plainly rather than buried.\n\n"
                   "IT IS THE LAWN AT THE EDGE OF EVERY SHALLOW POND, which is "
                   "the reason it survives that arithmetic: the alternative is "
                   "bare mud at the exact spot a player walks into the water. "
                   "Authored at the tightest spacing in the file (0.15 m) at "
                   "full abundance and full clustering, so placement makes a "
                   "continuous sward rather than clumps.\n\n"
                   "Very fine and very numerous -- 44 stems at 0.02 m, which is "
                   "a one-voxel thread at 5 cm. At this lattice it will read as "
                   "a chunky green mat rather than as quills, which "
                   "`tools/lattice_ab.py` measured for grass generally and which "
                   "is the intended look for a turf.",
             **t(stems=44, spread_m=0.07, splay_deg=18, arc=0.34, width_m=0.02,
                 taper=0.80, wander=0.26, length_var=0.28, base_m=0.08,
                 head="none", mat_stem="grass",
                 bio_grassland=1.0, bio_temperate_forest=0.9, bio_taiga=0.4,
                 place_abundance=1.0, place_spacing_m=0.15, place_cluster=1.0,
                 place_water_max_m=2, place_slope_max_pct=12)),
    ),

    # ================================================================
    # FLOATING-LEAVED
    # ================================================================

    "white-water-lily": (
        "flower 0.35 m - round notched pads flat on the water, white cup flower",
        base("flower", name="white-water-lily", height_m=0.35,
             notes="THE PAD IS THE SPECIES AND THERE IS NO PAD. `tuft.head` "
                   "draws a bloom, a spike or a plume on TOP of a stem; a lily "
                   "pad is a flat horizontal disc lying ON a water surface that "
                   "this asset does not contain. So what is authored is a "
                   "'bloom' head at PAD diameter (0.26 m) on every stem, which "
                   "reads as a lily from above -- a scatter of round discs at "
                   "one height -- and as a bouquet from the side. Four "
                   "floating-leaved species in this file share that "
                   "compromise.\n\n"
                   "THE FLOWER IS NOT DRAWN SEPARATELY. A real stand is mostly "
                   "pads with a few white cups among them, and one head type per "
                   "spec means choosing; the pads are chosen because there are "
                   "twenty of them for every flower and because green discs on "
                   "water is the read. `materials.head` is `leaf_broadleaf` "
                   "(78,122,54) rather than a white, which is the honest "
                   "consequence.\n\n"
                   "`arc` 0.86 and `splay_deg` 56 lay every stem right over, so "
                   "the discs sit out at the ends of long lax stalks rather than "
                   "on top of a bunch. That is what puts them at surface level "
                   "in a render.\n\n"
                   "0.5-2.5 m of still water, rooted in mud.",
             **t(stems=9, spread_m=0.10, splay_deg=56, arc=0.86, width_m=0.035,
                 taper=0.85, wander=0.36, length_var=0.34, base_m=0.11,
                 head="bloom", head_m=0.26, head_frac=0.14, head_share=1.0,
                 mat_stem="leaf_broadleaf", mat_head="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_grassland=0.9,
                 place_abundance=0.6, place_spacing_m=1.2, place_cluster=0.9,
                 place_water_max_m=1, place_slope_max_pct=10)),
    ),
    "yellow-water-lily": (
        "flower 0.40 m - leathery oval pads and a small yellow globe held clear",
        base("flower", name="yellow-water-lily", height_m=0.40,
             notes="THE ONE FLOATING-LEAVED PLANT WHOSE FLOWER THIS GENERATOR "
                   "CAN ACTUALLY DRAW, and that is why it is authored the "
                   "opposite way round from the white lily beside it. A "
                   "Nuphar's flower is a hard yellow globe held ten centimetres "
                   "CLEAR of the water on a thick stalk -- it is not lying on "
                   "the surface -- which is exactly what a `bloom` head on a "
                   "stiff stem is. So this one gets a real flower at "
                   "`skin_yellow`, on 40% of the stems, and the pads are the "
                   "other 60% drawn as plain wide stems.\n\n"
                   "The pair is deliberate: a white lily authored as pads and a "
                   "yellow lily authored as flowers, which between them cover "
                   "the two things a `flower` spec can be and neither of which "
                   "is both.\n\n"
                   "Larger and more leathery than the white lily, and it "
                   "tolerates moving water and shade, so it takes the taiga "
                   "weight the white one does not. 0.5-3 m.",
             **t(stems=11, spread_m=0.10, splay_deg=48, arc=0.72, width_m=0.045,
                 taper=0.80, wander=0.32, length_var=0.34, base_m=0.11,
                 head="bloom", head_m=0.11, head_frac=0.14, head_share=0.40,
                 mat_stem="leaf_broadleaf", mat_head="skin_yellow",
                 bio_temperate_forest=1.0, bio_grassland=0.8, bio_taiga=0.6,
                 place_abundance=0.6, place_spacing_m=1.1, place_cluster=0.9,
                 place_water_max_m=1, place_slope_max_pct=12)),
    ),
    "sacred-lotus": (
        "flower 1.20 m - round pads held ABOVE the water on stiff stalks",
        base("flower", name="sacred-lotus", height_m=1.20,
             notes="THE ONE FLOATING-LEAVED PLANT THAT IS NOT FLOATING, AND IT "
                   "IS THE BEST FIT IN THE SECTION. A lotus holds its round "
                   "blue-green pads a metre ABOVE the water on stiff vertical "
                   "stalks, with the flower higher still -- so it is a stem with "
                   "a disc on top, which is precisely what `tuft.head` is. Every "
                   "other lily here is a compromise; this is not.\n\n"
                   "`arc` 0.16 is the stiffest in the whole floating section "
                   "(the white lily is 0.86) and it is the entire difference "
                   "between the two plants. A lotus that arcs is a water lily.\n\n"
                   "The pads are 0.40 m across and the flower is 0.25; one head "
                   "type per spec, so the PADS are drawn, at 0.34 m as a "
                   "compromise between the two, on every stem. `leaf_jungle` "
                   "for the blue-green.\n\n"
                   "0.3-2 m of warm still water. Tropical and subtropical, "
                   "hence rainforest and savanna and nothing colder.",
             **t(stems=8, spread_m=0.12, splay_deg=12, arc=0.16, width_m=0.05,
                 taper=0.70, wander=0.22, length_var=0.36, base_m=0.13,
                 head="bloom", head_m=0.34, head_frac=0.12, head_share=1.0,
                 mat_stem="leaf_jungle", mat_head="grass",
                 bio_rainforest=1.0, bio_savanna=0.8, bio_grassland=0.3,
                 place_abundance=0.6, place_spacing_m=1.0, place_cluster=0.95,
                 place_water_max_m=1, place_slope_max_pct=10)),
    ),
    "giant-water-lily": (
        "flower 0.60 m - pads two metres across with a vertical upturned rim",
        base("flower", name="giant-water-lily", height_m=0.60,
             notes="THE BIGGEST LEAF IN THE LIBRARY AND THE RIM IS MISSING. A "
                   "Victoria pad is two to three metres across (estimate) with a "
                   "vertical UPTURNED LIP five to ten centimetres high all the "
                   "way round, and that lip is the entire species -- it is what "
                   "makes the pad a tray rather than a disc. `tuft.head` draws a "
                   "flat bloom and there is no rim primitive at any lattice. "
                   "Authored as a large plain pad.\n\n"
                   "`tuft.head_m` CEILS AT 8.0 m, so the diameter itself is "
                   "expressible with room to spare; it is set to 1.6 rather than "
                   "the full 2-3 because a single-stem plant whose head is four "
                   "times its own height reads as an umbrella, and because at "
                   "0.05 m voxels a 1.6 m disc is already 32 voxels across.\n\n"
                   "FIVE STEMS, WHICH IS BOTANICALLY WRONG AND STRUCTURALLY "
                   "NECESSARY. One pad per rootstock is the truth; one stem from "
                   "one crown is the most fragile thing this generator makes and "
                   "`tools/buildcheck.py` requires one connected piece. Five "
                   "short thick stalks under five pads is a clump of the same "
                   "plant and is what a stand looks like anyway.\n\n"
                   "0.5-2 m, warm still backwater. Rainforest only.",
             **t(stems=5, spread_m=0.14, splay_deg=40, arc=0.62, width_m=0.06,
                 taper=0.80, wander=0.24, length_var=0.30, base_m=0.15,
                 head="bloom", head_m=1.60, head_frac=0.10, head_share=1.0,
                 mat_stem="leaf_jungle", mat_head="grass",
                 bio_rainforest=1.0,
                 place_abundance=0.3, place_spacing_m=2.6, place_cluster=0.8,
                 place_water_max_m=1, place_slope_max_pct=8)),
    ),
    "fringed-water-lily": (
        "flower 0.25 m - small heart-shaped pads and a fringed yellow flower",
        base("flower", name="fringed-water-lily", height_m=0.25,
             notes="NOT A LILY AT ALL -- Nymphoides is a bogbean relative that "
                   "has converged on the lily's shape -- and it is here as the "
                   "SMALL member of the floating group, because a pond covered "
                   "entirely in half-metre lily pads reads as one plant.\n\n"
                   "It is authored the yellow lily's way rather than the white "
                   "lily's: a real `bloom` head in `skin_yellow` on 45% of the "
                   "stems, because a fringed water-lily's flower stands proud of "
                   "its pad and is bright enough to be the read at any distance. "
                   "The deeply fringed petal edges that name it are 3-5 mm "
                   "(estimate) and are not drawn.\n\n"
                   "Small pads (0.10 m) on many short lax stems -- 14 of them, "
                   "against the white lily's 9 -- so a clump is a mat rather "
                   "than a scatter.\n\n"
                   "0.3-1.5 m of still or slow water.",
             **t(stems=14, spread_m=0.09, splay_deg=52, arc=0.80, width_m=0.03,
                 taper=0.80, wander=0.34, length_var=0.32, base_m=0.10,
                 head="bloom", head_m=0.10, head_frac=0.14, head_share=0.45,
                 mat_stem="leaf_broadleaf", mat_head="skin_yellow",
                 bio_temperate_forest=0.9, bio_grassland=1.0,
                 place_abundance=0.7, place_spacing_m=0.6, place_cluster=0.95,
                 place_water_max_m=1, place_slope_max_pct=10)),
    ),
    "water-hyacinth": (
        "flower 0.50 m - glossy round leaves on swollen bulbs, violet spike",
        base("flower", name="water-hyacinth", height_m=0.50,
             notes="THE MOST DESTRUCTIVE FRESHWATER WEED ON EARTH, and it floats "
                   "free -- no roots in the bed at all, just a dangling black "
                   "beard hanging in open water. Like the hornwort it is "
                   "authored rooted, because every asset in this library stands "
                   "on the ground.\n\n"
                   "THE INFLATED PETIOLES ARE THE SPECIES. Each leaf stalk "
                   "swells into a spongy float halfway up -- fat in the middle "
                   "and narrow at both ends -- and a tuft stem tapers "
                   "monotonically from root to tip. There is no bulge parameter. "
                   "`taper` 0.90, nearly the highest here, at least keeps the "
                   "stalk fat all the way up rather than pointed, which is the "
                   "reachable half.\n\n"
                   "The violet flower spike is what everyone recognises and it "
                   "gets a real `spike` head in `plume_lilac` (166,132,208) on a "
                   "third of the stems -- one of the very few times in these two "
                   "files that the palette carries a species' actual colour.\n\n"
                   "Floating on still or slow warm water of any depth.",
             **t(stems=13, spread_m=0.09, splay_deg=30, arc=0.34, width_m=0.05,
                 taper=0.90, wander=0.26, length_var=0.30, base_m=0.10,
                 head="spike", head_m=0.12, head_frac=0.24, head_share=0.32,
                 mat_stem="leaf_broadleaf", mat_head="plume_lilac",
                 bio_rainforest=1.0, bio_savanna=0.9, bio_grassland=0.3,
                 place_abundance=0.9, place_spacing_m=0.4, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=10)),
    ),
    "water-lettuce": (
        "grass 0.25 m - a floating rosette of thick fluted velvety leaves",
        base("grass", name="water-lettuce", height_m=0.25,
             notes="AN OPEN CABBAGE ON THE WATER, and it is authored as a "
                   "rosette rather than as a tuft: `splay_deg` 50 with a low "
                   "`length_var` (0.20), so the leaves come out at one angle in "
                   "a ring and all reach about the same distance. That evenness "
                   "is what a Pistia rosette is; most things in this file are "
                   "deliberately ragged and this one must not be.\n\n"
                   "THICK AND FLUTED: `width_m` 0.07 is the widest stem of any "
                   "grass in this file, and the deep parallel ribs that make the "
                   "leaf look pleated are 3-5 mm (estimate) and are not drawn at "
                   "5 cm.\n\n"
                   "Free-floating, like the hyacinth beside it, and authored "
                   "rooted for the same reason. Pale grey-green and velvety: "
                   "`leaf_dry` (146,138,74) is the palest green available and is "
                   "closer than a grass green would be.\n\n"
                   "Still warm water of any depth.",
             **t(stems=13, spread_m=0.08, splay_deg=50, arc=0.44, width_m=0.07,
                 taper=0.55, wander=0.22, length_var=0.20, base_m=0.09,
                 head="none", mat_stem="leaf_dry",
                 bio_rainforest=1.0, bio_savanna=0.8,
                 place_abundance=0.9, place_spacing_m=0.3, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=10)),
    ),
    "water-chestnut": (
        "grass 0.25 m - a flat rhombic rosette of toothed leaves on floats",
        base("grass", name="water-chestnut", height_m=0.25,
             notes="A SNOWFLAKE ON THE WATER. Trapa's floating rosette is "
                   "geometrically the most regular thing in this file -- "
                   "diamond-shaped toothed leaves on inflated stalks, arranged "
                   "in a flat rotationally symmetric star -- and the tuft "
                   "generator's `length_var` and `wander` both exist to destroy "
                   "exactly that kind of regularity, so both are pushed to their "
                   "lowest useful values here (0.14 and 0.14).\n\n"
                   "IT IS THE MOST NEARLY FLAT ASSET IN THE LIBRARY: "
                   "`splay_deg` 66 with `arc` 0.30, so the stems go out almost "
                   "horizontally and barely rise. At 5 cm a 0.25 m plant laid "
                   "that flat is two or three voxels tall, which is a mat, and "
                   "that is what it should be.\n\n"
                   "The inflated float on each stalk is the same missing bulge "
                   "the water hyacinth records.\n\n"
                   "0.5-3 m of still water; the nuts sink and root in mud.",
             **t(stems=12, spread_m=0.09, splay_deg=66, arc=0.30, width_m=0.05,
                 taper=0.60, wander=0.14, length_var=0.14, base_m=0.10,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_temperate_forest=0.9, bio_grassland=1.0,
                 place_abundance=0.7, place_spacing_m=0.4, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=10)),
    ),

    # ================================================================
    # EMERGENT -- the ones not already authored.
    #
    # Already shipped and NOT duplicated here: bulrush, water-reed,
    # reed-sweet-grass, giant-reed, papyrus, floodplain-sedge, smooth-cordgrass,
    # marsh-marigold, common-cottongrass, tussock-cottongrass, wood-horsetail.
    # ================================================================

    "broadleaf-cattail": (
        "reed 2.20 m - flat straps and a fat brown cigar on a bare stalk",
        base("reed", name="broadleaf-cattail", height_m=2.20,
             notes="THE MOST RECOGNISABLE WATERSIDE PLANT THERE IS AND THE "
                   "LIBRARY DID NOT HAVE IT. `bulrush` ships with 'shorter, "
                   "thicker stems with a fat dark head' and `water-reed` with "
                   "'tall near-vertical waterside stems with seed heads'; "
                   "neither is a Typha, whose head is a dense velvet-brown "
                   "CYLINDER 15-20 cm long and 2-3 cm thick on an otherwise bare "
                   "stalk. That silhouette is unmistakable and unmatched by "
                   "anything shipped.\n\n"
                   "`head_m` IS 0.09 AND THE HONEST FIGURE IS 0.02, AND THE "
                   "RENDER IS WHY. A real cigar is 2-3 cm thick (estimate). At "
                   "0.05 m -- already twice life size, because one voxel is the "
                   "floor -- the head is ONE voxel across, and on a contact "
                   "sheet it came out as a scatter of loose dots up the stem "
                   "rather than as a cylinder. At 0.09 it is two voxels and "
                   "reads as a cigar. That is roughly four times life "
                   "thickness.\n\n"
                   "TWO VOXELS IS THE FLOOR FOR ANYTHING THAT HAS TO READ AS A "
                   "SHAPE RATHER THAN AS INK, which is the same conclusion "
                   "`docs/fish-shape-research.md` reached for a shaped colour "
                   "boundary and `docs/bird-shape-research.md` for a bill. "
                   "Written down because it is exactly the kind of number "
                   "someone later halves back to life size and quietly "
                   "destroys.\n\n"
                   "`head_frac` 0.11 puts it in the top 24 cm of a 2.2 m "
                   "stem.\n\n"
                   "`head_share` 0.35: two thirds of the shoots in a real stand "
                   "are sterile leaf fans with no cigar at all, and that mix is "
                   "what a cattail bed looks like.\n\n"
                   "0-0.5 m of water at the margin; it will not grow in deep "
                   "water and it dominates everything shallower.",
             **t(stems=14, spread_m=0.12, splay_deg=8, arc=0.16, width_m=0.045,
                 taper=0.60, wander=0.18, length_var=0.32, base_m=0.13,
                 head="spike", head_m=0.09, head_frac=0.11, head_share=0.35,
                 mat_stem="leaf_broadleaf", mat_head="leaf_autumn",
                 bio_grassland=1.0, bio_temperate_forest=0.9, bio_taiga=0.6,
                 place_abundance=0.9, place_spacing_m=0.5, place_cluster=1.0,
                 place_water_max_m=2, place_slope_max_pct=15)),
    ),
    "branched-bur-reed": (
        "reed 1.00 m - zig-zag stems with spiky spherical burr heads",
        base("reed", name="branched-bur-reed", height_m=1.00,
             notes="THE BURRS ARE SPHERES AND `tuft.head` DRAWS ONE PER STEM. A "
                   "Sparganium carries several spiky green balls SCATTERED ALONG "
                   "a zig-zag stem, and a tuft head sits on the top of a stem "
                   "and nowhere else -- the same along-the-stem gap that costs "
                   "the giant kelp its blades and five submerged species their "
                   "whorls. So each stem gets one terminal burr instead of four "
                   "spaced ones, and the stem count (17) stands in for the "
                   "scatter.\n\n"
                   "The zig-zag itself is `wander` 0.42, which is sideways drift "
                   "along a stem rather than a real kink, and is the closest "
                   "thing available.\n\n"
                   "`head` is 'bloom' rather than 'spike' because a burr is "
                   "round and a spike is a cylinder, and roundness is the "
                   "readable half of the field mark.\n\n"
                   "0-0.5 m at the margin, often in slightly moving water where "
                   "the cattail will not go.",
             **t(stems=17, spread_m=0.10, splay_deg=16, arc=0.30, width_m=0.03,
                 taper=0.55, wander=0.42, length_var=0.34, base_m=0.11,
                 head="bloom", head_m=0.07, head_frac=0.10, head_share=0.5,
                 mat_stem="leaf_broadleaf", mat_head="leaf_dry",
                 bio_temperate_forest=1.0, bio_grassland=0.9, bio_taiga=0.5,
                 place_abundance=0.7, place_spacing_m=0.5, place_cluster=0.95,
                 place_water_max_m=3, place_slope_max_pct=18)),
    ),
    "sweet-flag": (
        "reed 1.00 m - sword leaves with a crimped edge and a side-borne finger",
        base("reed", name="sweet-flag", height_m=1.00,
             notes="THE ODDEST FLOWER SHAPE IN THE FILE AND IT IS NOT DRAWN. "
                   "Acorus carries its flower spike NOT at the top of a stem but "
                   "sticking out sideways at 45 degrees halfway up a leaf, like "
                   "a finger pointing off the blade. Nothing in the tuft "
                   "generator puts a head anywhere except the tip of a stem, so "
                   "this species ships with `head` 'none' and no flower at "
                   "all.\n\n"
                   "WHAT IS LEFT IS STILL WORTH HAVING: a dense fan of stiff "
                   "sword leaves with one crimped wavy edge, in exactly the "
                   "places a yellow flag iris grows and looking enough like one "
                   "to be mistaken for it, which is true in the field too. The "
                   "crimp is a 3-5 mm undulation (estimate) and is not drawn "
                   "either.\n\n"
                   "`splay_deg` 14 with `arc` 0.30 gives the flat fan; the "
                   "leaves come off in one plane in life and in a full circle "
                   "here, which is the tuft generator's usual azimuth "
                   "limitation.\n\n"
                   "0-0.3 m at the very margin, in mud.",
             **t(stems=15, spread_m=0.11, splay_deg=14, arc=0.30, width_m=0.045,
                 taper=0.40, wander=0.20, length_var=0.30, base_m=0.12,
                 head="none", mat_stem="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_grassland=0.8,
                 place_abundance=0.6, place_spacing_m=0.6, place_cluster=0.95,
                 place_water_max_m=2, place_slope_max_pct=15)),
    ),
    "water-horsetail": (
        "reed 0.90 m - bare jointed hollow tubes standing dead straight",
        base("reed", name="water-horsetail", height_m=0.90,
             notes="THE SIMPLEST SHAPE IN THIS FILE AND THE ONE THE GENERATOR "
                   "GETS EXACTLY RIGHT. Equisetum fluviatile is a bare "
                   "unbranched hollow tube standing vertically out of the water "
                   "with essentially no leaves -- which is a tuft stem, "
                   "unmodified. `arc` 0.06 and `splay_deg` 4 are the stiffest "
                   "and most vertical settings in either aquatic file.\n\n"
                   "`wood-horsetail` ALREADY SHIPS AND IS A DIFFERENT PLANT: "
                   "that one is the branched forest-floor species with whorls of "
                   "side shoots. This one is the unbranched aquatic species that "
                   "stands in half a metre of water, and the two look nothing "
                   "alike -- one is a bottle brush and one is a bundle of "
                   "straws.\n\n"
                   "The dark joints every few centimetres are the field mark and "
                   "there is no banding on a tuft stem at any lattice. "
                   "`taper` 0.92 is the highest in the file, because a horsetail "
                   "is the same diameter for its whole length and then simply "
                   "ends.\n\n"
                   "THIRTEEN STEMS IN A 0.17 m CROWN AND THE FIRST DRAFT HAD "
                   "TWENTY-TWO IN 0.11, WHICH RENDERED AS A SOLID BLOCK. At "
                   "5 cm a 0.03 m stem is one voxel; twenty-two of them rooted "
                   "in a 0.11 m disc are 2.2 voxels apart on average and the "
                   "gaps close. `tools/lattice_ab.py` measured that effect for "
                   "grass generally -- at 5 cm a tuft stops being blades and "
                   "becomes a clump -- and for most species here that is the "
                   "intended look. It is NOT the intended look for a horsetail, "
                   "whose whole identity is separate bare tubes with water "
                   "visible between them. Fewer stems in a wider crown is the "
                   "fix, and it is the general answer whenever a species is "
                   "about its gaps.\n\n"
                   "0.2-1 m of water, and it is the emergent that goes DEEPEST "
                   "of the ones here.",
             **t(stems=13, spread_m=0.17, splay_deg=5, arc=0.06, width_m=0.03,
                 taper=0.92, wander=0.10, length_var=0.30, base_m=0.18,
                 head="none", mat_stem="leaf_needle",
                 bio_taiga=1.0, bio_temperate_forest=0.9,
                 bio_tundra_alpine=0.5, bio_grassland=0.5,
                 place_abundance=0.8, place_spacing_m=0.4, place_cluster=1.0,
                 place_water_max_m=2, place_slope_max_pct=15)),
    ),
    "wild-rice": (
        "reed 2.00 m - a very tall coarse grass with a wide open panicle",
        base("reed", name="wild-rice", height_m=2.00,
             notes="A CEREAL THAT GROWS IN A METRE OF WATER, which no other "
                   "grass in the library does: `big-bluestem` at 2 m is the "
                   "tallest shipped grass and it is a prairie plant on dry "
                   "ground. This is the same height standing in a lake, and a "
                   "wild rice bed is a wall of green out of open water.\n\n"
                   "THE PANICLE IS THE FIELD MARK AND IT IS AN OPEN HAZE. Male "
                   "flowers droop below and female stand erect above on the same "
                   "head; a `plume` at a wide diameter and a low share is the "
                   "closest thing, and it is the same trick `switchgrass` uses "
                   "for its seed haze. Widening the plume and dropping the share "
                   "is how this generator says 'airy'.\n\n"
                   "0.3-1.2 m of water, slow and mud-bottomed. It is the "
                   "emergent a canoe goes through rather than around.",
             **t(stems=10, spread_m=0.12, splay_deg=10, arc=0.24, width_m=0.04,
                 taper=0.50, wander=0.22, length_var=0.36, base_m=0.13,
                 head="plume", head_m=0.26, head_frac=0.20, head_share=0.4,
                 mat_stem="grass", mat_head="leaf_dry",
                 bio_grassland=1.0, bio_temperate_forest=0.8,
                 place_abundance=0.6, place_spacing_m=0.7, place_cluster=0.95,
                 place_water_max_m=2, place_slope_max_pct=12)),
    ),
    "soft-rush": (
        "reed 0.90 m - a tussock of smooth cylindrical stems, flowers side-borne",
        base("reed", name="soft-rush", height_m=0.90,
             notes="THE PLANT THAT TELLS YOU THE GROUND IS WET. A field full of "
                   "soft rush tussocks is a field with a drainage problem, and "
                   "that association is the reason it is worth authoring: it is "
                   "the cheapest way to make a piece of grassland read as marshy "
                   "without any water being visible.\n\n"
                   "SO IT IS THE ONE SPECIES IN THIS FILE PLACED AWAY FROM "
                   "WATER: `water_max_m` 8, the widest here, because a rush "
                   "grows across whole wet meadows and not just at a margin.\n\n"
                   "Its flower cluster bursts out of the SIDE of the stem near "
                   "the top, which is the sweet flag's problem again and gets "
                   "the same answer -- `head` 'none'. What is authored is the "
                   "tussock: 34 smooth cylindrical stems, tightly rooted, "
                   "barely tapered, in a dense hummock.",
             **t(stems=34, spread_m=0.12, splay_deg=13, arc=0.26, width_m=0.025,
                 taper=0.75, wander=0.20, length_var=0.32, base_m=0.13,
                 head="none", mat_stem="leaf_needle",
                 bio_grassland=1.0, bio_temperate_forest=0.9, bio_taiga=0.6,
                 place_abundance=0.8, place_spacing_m=0.6, place_cluster=0.9,
                 place_water_max_m=8, place_slope_max_pct=25)),
    ),
    "lesser-pond-sedge": (
        "grass 1.00 m - broad drooping keeled leaves and nodding black spikes",
        base("grass", name="lesser-pond-sedge", height_m=1.00,
             notes="THE THIRD SEDGE IN THE LIBRARY AND THE ONLY WATERSIDE ONE. "
                   "`floodplain-sedge` and `wood-sedge` ship; neither is a Carex "
                   "acutiformis, which forms metre-tall stands in standing water "
                   "at the edge of every lowland pond and river in the temperate "
                   "world.\n\n"
                   "DROOPING, WHICH IS WHAT SEPARATES A POND SEDGE FROM A REED. "
                   "`arc` 0.58 on a metre-tall plant lays the leaf tips right "
                   "over; a `water-reed` at the same height is authored near "
                   "vertical. That difference reads at fifty metres and is most "
                   "of why the two are worth having side by side.\n\n"
                   "The nodding brown-black flower spikes are drawn as `spike` "
                   "heads at a low share on the longest stems. Keeled "
                   "M-sectioned leaves are a cross-section detail and there is "
                   "one cross-section in the tuft generator.\n\n"
                   "0-0.4 m of water; it forms the belt between the open water "
                   "and the wet meadow.",
             **t(stems=20, spread_m=0.11, splay_deg=18, arc=0.58, width_m=0.045,
                 taper=0.45, wander=0.28, length_var=0.34, base_m=0.12,
                 head="spike", head_m=0.06, head_frac=0.16, head_share=0.30,
                 mat_stem="leaf_broadleaf", mat_head="leaf_autumn",
                 bio_temperate_forest=1.0, bio_grassland=0.9,
                 place_abundance=0.85, place_spacing_m=0.45, place_cluster=1.0,
                 place_water_max_m=3, place_slope_max_pct=18)),
    ),
    "yellow-flag-iris": (
        "flower 1.00 m - stiff grey-green sword leaves and a big yellow flag",
        base("flower", name="yellow-flag-iris", height_m=1.00,
             notes="THE BRIGHTEST THING ON A RIVERBANK IN JUNE, and the reason "
                   "it matters is scale: an iris flower is 8-10 cm across "
                   "(estimate), which at 5 cm is two voxels, and two voxels of "
                   "saturated yellow at the top of a metre-tall fan is visible "
                   "from a long way off. Most flowers in this library are one "
                   "voxel and read as texture.\n\n"
                   "`head_m` 0.10 with `head_share` 0.30: a real stand is mostly "
                   "leaf fans with a scatter of flowering shoots, and putting a "
                   "flag on every stem would make it read as a solid yellow "
                   "block.\n\n"
                   "THE LEAVES ARE STIFFER AND FLATTER THAN THE SWEET FLAG "
                   "BESIDE IT, which is the pair's real difference when neither "
                   "is flowering -- `arc` 0.22 against 0.30 and a slower taper. "
                   "In the field the two are told apart by exactly this and by "
                   "smell.\n\n"
                   "0-0.3 m at the margin, and it tolerates brackish water, "
                   "which nothing else in this file does.",
             **t(stems=13, spread_m=0.10, splay_deg=12, arc=0.22, width_m=0.05,
                 taper=0.35, wander=0.16, length_var=0.30, base_m=0.11,
                 head="bloom", head_m=0.10, head_frac=0.14, head_share=0.30,
                 mat_stem="leaf_needle", mat_head="skin_yellow",
                 bio_temperate_forest=1.0, bio_grassland=0.9, bio_taiga=0.5,
                 place_abundance=0.7, place_spacing_m=0.6, place_cluster=0.9,
                 place_water_max_m=3, place_slope_max_pct=20)),
    ),
    "flowering-rush": (
        "flower 1.20 m - three-cornered rush leaves under an umbel of pink",
        base("flower", name="flowering-rush", height_m=1.20,
             notes="ONE TALL BARE STALK CARRYING A ROUND UMBEL OF PINK FLOWERS "
                   "well above the leaves, which is a silhouette nothing else in "
                   "the waterside set has -- every other flowering emergent here "
                   "holds its flowers among its foliage. So the head is on a "
                   "SMALL share of the stems (0.25) at a large diameter (0.16 m, "
                   "three voxels) and the rest are plain leaves, which puts the "
                   "pink where it belongs: a few discs floating above a green "
                   "fan.\n\n"
                   "`leaf_blossom` (226,168,190) is the palette's only pink and "
                   "is very close to this species' actual colour. Third time in "
                   "these two files that the palette has been right.\n\n"
                   "The leaves are triangular in section -- three-cornered, "
                   "which is where the name comes from -- and the tuft generator "
                   "has one cross-section. Not drawn.\n\n"
                   "0-0.5 m at the margin of slow rivers and canals.",
             **t(stems=12, spread_m=0.10, splay_deg=14, arc=0.34, width_m=0.035,
                 taper=0.50, wander=0.22, length_var=0.34, base_m=0.11,
                 head="bloom", head_m=0.16, head_frac=0.12, head_share=0.25,
                 mat_stem="leaf_broadleaf", mat_head="leaf_blossom",
                 bio_grassland=1.0, bio_temperate_forest=0.8,
                 place_abundance=0.5, place_spacing_m=0.8, place_cluster=0.85,
                 place_water_max_m=3, place_slope_max_pct=18)),
    ),
    "arrowhead": (
        "flower 0.80 m - arrow-shaped leaves held vertical, white three-petals",
        base("flower", name="arrowhead", height_m=0.80,
             notes="THE LEAF SHAPE IS THE NAME AND IT IS NOT DRAWN. Sagittaria's "
                   "aerial leaves are perfect arrowheads -- a triangular blade "
                   "with two long backward barbs -- held VERTICALLY on a long "
                   "stalk, and a tuft stem is a swept capsule. There is no blade "
                   "primitive at all in this generator, which is the same wall "
                   "the ferns hit in `tools/seed_groundcover.py`.\n\n"
                   "WHAT IS AUTHORED IS THE STANCE, which is genuinely "
                   "distinctive: `splay_deg` 10 and `arc` 0.14, so every leaf "
                   "stands bolt upright out of the water on a long stalk. "
                   "Almost every other emergent here arcs. A vertical fan of "
                   "stalks with white flowers among them is the read at "
                   "distance and it is not wrong, it is incomplete.\n\n"
                   "White three-petalled flowers in whorls up a separate stalk; "
                   "drawn as `bloom` at `plume_white` on a third of the stems.\n\n"
                   "0.1-0.5 m of still or slow water over mud.",
             **t(stems=13, spread_m=0.09, splay_deg=10, arc=0.14, width_m=0.04,
                 taper=0.55, wander=0.18, length_var=0.32, base_m=0.10,
                 head="bloom", head_m=0.09, head_frac=0.12, head_share=0.33,
                 mat_stem="leaf_broadleaf", mat_head="plume_white",
                 bio_temperate_forest=1.0, bio_grassland=0.9,
                 place_abundance=0.7, place_spacing_m=0.5, place_cluster=0.95,
                 place_water_max_m=2, place_slope_max_pct=15)),
    ),
    "pickerelweed": (
        "flower 0.80 m - glossy heart-shaped leaves and a blue-violet spike",
        base("flower", name="pickerelweed", height_m=0.80,
             notes="THE ONLY BLUE FLOWER IN EITHER AQUATIC FILE, and blue is the "
                   "rarest colour in the palette -- `skin_blue` (46,96,168) is "
                   "the one entry and `sea-holly` is the only shipped species "
                   "using it. Pontederia's dense violet-blue spike over glossy "
                   "green heart leaves is close enough to be worth the row on "
                   "colour alone.\n\n"
                   "`spike` rather than `bloom`, because the inflorescence is a "
                   "tall dense cylinder rather than a disc, and `head_frac` 0.26 "
                   "makes it a quarter of the stem's length -- one of the "
                   "longest heads in the library. That proportion is the "
                   "species.\n\n"
                   "The heart-shaped leaf blade is not drawn, for the same "
                   "reason the arrowhead's arrow is not: there is no blade "
                   "primitive.\n\n"
                   "0.1-0.5 m of still water; a New World plant, so the weights "
                   "are grassland and savanna rather than the temperate-forest "
                   "default most of this file uses.",
             **t(stems=14, spread_m=0.09, splay_deg=16, arc=0.28, width_m=0.04,
                 taper=0.55, wander=0.22, length_var=0.30, base_m=0.10,
                 head="spike", head_m=0.09, head_frac=0.26, head_share=0.35,
                 mat_stem="leaf_broadleaf", mat_head="skin_blue",
                 bio_grassland=1.0, bio_savanna=0.7,
                 bio_temperate_forest=0.6,
                 place_abundance=0.7, place_spacing_m=0.5, place_cluster=0.95,
                 place_water_max_m=2, place_slope_max_pct=15)),
    ),
    "water-plantain": (
        "flower 0.80 m - a rosette of ribbed oval leaves under an airy pyramid",
        base("flower", name="water-plantain", height_m=0.80,
             notes="A PYRAMID OF NOTHING. Alisma's flower head is a huge open "
                   "candelabra of whorled branches carrying flowers 5 mm across "
                   "-- it is mostly air, and at 5 cm each flower is a tenth of a "
                   "voxel. There is no branched inflorescence in the tuft "
                   "generator either.\n\n"
                   "SO THE HEAD IS DRAWN AS A WIDE LOW-DENSITY `plume` (0.22 m "
                   "across, on 40% of the stems), which is the same answer "
                   "`switchgrass` and `wild-rice` give for an airy panicle: "
                   "widen it and drop the share. It is the third species in "
                   "these two files to use that trick and it is worth naming as "
                   "a pattern rather than rediscovering.\n\n"
                   "The basal rosette of long-stalked ribbed oval leaves is the "
                   "other half of the plant and is authored as the remaining "
                   "60% of the stems, splayed wide and arced over.\n\n"
                   "0-0.2 m at the very margin, on bare mud.",
             **t(stems=13, spread_m=0.10, splay_deg=34, arc=0.52, width_m=0.04,
                 taper=0.55, wander=0.28, length_var=0.36, base_m=0.11,
                 head="plume", head_m=0.22, head_frac=0.24, head_share=0.40,
                 mat_stem="leaf_broadleaf", mat_head="plume_white",
                 bio_grassland=1.0, bio_temperate_forest=0.9,
                 place_abundance=0.6, place_spacing_m=0.6, place_cluster=0.9,
                 place_water_max_m=3, place_slope_max_pct=18)),
    ),
    "purple-loosestrife": (
        "flower 1.20 m - a stiff spire of magenta over a whorled leafy stem",
        base("flower", name="purple-loosestrife", height_m=1.20,
             notes="THE ONE WATERSIDE FLOWER THAT COLOURS A WHOLE BANK, and the "
                   "reason it is worth authoring over a dozen prettier ones is "
                   "that it grows in dense single-species stands hundreds of "
                   "metres long. One spec changes the colour of a river.\n\n"
                   "A LONG DENSE SPIKE, WHICH IS THE EASIEST SHAPE IN THIS FILE. "
                   "`head_frac` 0.38 makes the flower head over a third of the "
                   "stem's length -- the longest in either aquatic file -- and "
                   "`head_share` 0.75, because in a real stand nearly every "
                   "shoot flowers. That is the opposite of every other species "
                   "here and it is what makes a stand read as a block of "
                   "colour.\n\n"
                   "`plume_lilac` (166,132,208) against a real magenta: too "
                   "blue, too pale, and the closest of the fourteen head "
                   "materials. `plume_crimson` (208,40,56) is the alternative "
                   "and is too red.\n\n"
                   "0-0.2 m, and it also grows on wet ground well back from the "
                   "water, hence `water_max_m` 6.",
             **t(stems=10, spread_m=0.09, splay_deg=10, arc=0.14, width_m=0.035,
                 taper=0.45, wander=0.18, length_var=0.32, base_m=0.10,
                 head="spike", head_m=0.10, head_frac=0.38, head_share=0.75,
                 mat_stem="leaf_broadleaf", mat_head="plume_lilac",
                 bio_grassland=1.0, bio_temperate_forest=0.9,
                 place_abundance=0.8, place_spacing_m=0.5, place_cluster=1.0,
                 place_water_max_m=6, place_slope_max_pct=25)),
    ),
    "water-mint": (
        "flower 0.50 m - a low sprawling clump with rounded lilac flower heads",
        base("flower", name="water-mint", height_m=0.50,
             notes="LOW AND SPRAWLING, WHICH IS WHAT THE MARGIN NEEDS. Every "
                   "other flowering emergent in this file is upright and a "
                   "metre tall; a river's edge also has a low untidy layer "
                   "underneath them, and this is it. `splay_deg` 40 and `arc` "
                   "0.58 push it sideways rather than up.\n\n"
                   "The flower head is a tight ROUND ball at the top of each "
                   "stem, which is `bloom` at a small diameter and a HIGH share "
                   "(0.6) -- water mint flowers freely and a patch in August is "
                   "more lilac than green.\n\n"
                   "It is the most heavily insect-visited plant on a British "
                   "riverbank, which is not a shape fact and is the sort of "
                   "thing a placement rule might one day want.\n\n"
                   "0-0.1 m; the splash zone of fresh water, on mud and gravel "
                   "alike.",
             **t(stems=18, spread_m=0.10, splay_deg=40, arc=0.58, width_m=0.03,
                 taper=0.55, wander=0.34, length_var=0.32, base_m=0.11,
                 head="bloom", head_m=0.08, head_frac=0.14, head_share=0.60,
                 mat_stem="leaf_broadleaf", mat_head="plume_lilac",
                 bio_temperate_forest=1.0, bio_grassland=0.9, bio_taiga=0.4,
                 place_abundance=0.8, place_spacing_m=0.4, place_cluster=0.95,
                 place_water_max_m=4, place_slope_max_pct=25)),
    ),
    "water-forget-me-not": (
        "flower 0.30 m - trailing stems with sprays of sky-blue yellow-eyed flowers",
        base("flower", name="water-forget-me-not", height_m=0.30,
             notes="THE SECOND BLUE FLOWER, AND THE EYE IS THE PROBLEM. A "
                   "forget-me-not is a 5 mm sky-blue disc with a bright yellow "
                   "centre, and the yellow eye against the blue is most of what "
                   "makes the flower recognisable. `tuft.head` is one solid "
                   "colour and there is no centre. At 5 cm the whole flower is "
                   "a tenth of a voxel anyway, so `head_m` 0.06 is drawn at "
                   "roughly ten times life size to exist at all -- one voxel "
                   "is the floor.\n\n"
                   "That enlargement is the same decision `broadleaf-cattail` "
                   "records for its seed head and it is much larger here. It is "
                   "acceptable because what the head is standing in for is not "
                   "ONE flower but a SPRAY of them, which really is a few "
                   "centimetres across.\n\n"
                   "Low and trailing (`arc` 0.66), the lowest flowering "
                   "emergent in the file, growing half in and half out of the "
                   "water at the very edge.",
             **t(stems=20, spread_m=0.09, splay_deg=38, arc=0.66, width_m=0.022,
                 taper=0.60, wander=0.36, length_var=0.30, base_m=0.10,
                 head="bloom", head_m=0.06, head_frac=0.12, head_share=0.55,
                 mat_stem="grass", mat_head="skin_blue",
                 bio_temperate_forest=1.0, bio_taiga=0.7, bio_grassland=0.8,
                 place_abundance=0.8, place_spacing_m=0.35, place_cluster=0.95,
                 place_water_max_m=3, place_slope_max_pct=25)),
    ),
    "bogbean": (
        "flower 0.30 m - three thick leaflets and a spike of white fringed stars",
        base("flower", name="bogbean", height_m=0.30,
             notes="THE PLANT OF THE BOG POOL, and the library's peatland had "
                   "`sphagnum-hummock` and `common-cottongrass` and no flower at "
                   "all. Menyanthes is the one: it grows out of the open water "
                   "in a quaking bog, in acid peaty pools too poor for anything "
                   "else, and its flower is the most striking in the file -- "
                   "white stars covered in a thick white fringe of hairs.\n\n"
                   "THE FRINGE IS 2-3 mm (estimate) AND IS NOT DRAWN, so what "
                   "the head carries is white and roundness. `plume_white` "
                   "(246,246,242) is the brightest material in the palette and "
                   "against the dark peat water it will be the read.\n\n"
                   "THREE THICK LEAFLETS ON ONE STALK -- like a broad bean's, "
                   "which is the name -- is a pinnate arrangement and there is "
                   "no leaflet in the tuft generator. Authored as thick wide "
                   "stems (0.05 m) at a low count (10), which gives the right "
                   "coarse fleshy mass without the division.\n\n"
                   "0-0.3 m in bog pools and lake margins. Taiga and tundra, "
                   "which almost nothing else in this file reaches.",
             **t(stems=10, spread_m=0.09, splay_deg=26, arc=0.44, width_m=0.05,
                 taper=0.65, wander=0.26, length_var=0.28, base_m=0.10,
                 head="bloom", head_m=0.09, head_frac=0.16, head_share=0.4,
                 mat_stem="leaf_broadleaf", mat_head="plume_white",
                 bio_taiga=1.0, bio_tundra_alpine=0.8,
                 bio_temperate_forest=0.4,
                 place_abundance=0.6, place_spacing_m=0.5, place_cluster=0.95,
                 place_water_max_m=2, place_slope_max_pct=12)),
    ),
    "marsh-cinquefoil": (
        "flower 0.40 m - sprawling, with dark purple-red five-petalled flowers",
        base("flower", name="marsh-cinquefoil", height_m=0.40,
             notes="THE DARKEST FLOWER IN THE LIBRARY. Comarum's petals are a "
                   "deep blackish purple-red that almost no other temperate "
                   "wildflower has, and against the pale plumes and yellows of "
                   "the rest of the waterside set it is a genuinely different "
                   "note. `plume_crimson` (208,40,56) is far too bright and "
                   "orange for it; there is no dark red in `materials.head` at "
                   "all, so the species ships wearing a colour it does not "
                   "have. The most conspicuous palette failure in either aquatic "
                   "file, and it is the flower's own identity.\n\n"
                   "Sprawling and scrambling rather than upright: `arc` 0.56 and "
                   "`splay_deg` 34. It grows THROUGH the sphagnum and the sedge "
                   "rather than beside them, which nothing in `placement` can "
                   "say.\n\n"
                   "Pinnate leaves, not drawn -- see `bogbean`.\n\n"
                   "0-0.2 m in fens, bog margins and wet peaty ground.",
             **t(stems=15, spread_m=0.09, splay_deg=34, arc=0.56, width_m=0.03,
                 taper=0.55, wander=0.32, length_var=0.32, base_m=0.10,
                 head="bloom", head_m=0.08, head_frac=0.14, head_share=0.45,
                 mat_stem="leaf_broadleaf", mat_head="plume_crimson",
                 bio_taiga=1.0, bio_temperate_forest=0.7,
                 bio_tundra_alpine=0.6,
                 place_abundance=0.6, place_spacing_m=0.5, place_cluster=0.9,
                 place_water_max_m=4, place_slope_max_pct=20)),
    ),
    "watercress": (
        "grass 0.30 m - low dense dark pinnate mats over spring-fed gravel",
        base("grass", name="watercress", height_m=0.30,
             notes="IT MARKS A SPRING, and that is the whole reason to have it. "
                   "Watercress needs cold clean water at a constant temperature "
                   "coming out of chalk or limestone, so a bed of it means a "
                   "spring head or a chalk stream and nothing else. Placed "
                   "tight to water (`water_max_m` 1) on nearly flat ground.\n\n"
                   "PINNATE LEAVES, NOT DRAWN. A watercress leaf is a row of "
                   "rounded leaflets on a midrib and there is no leaflet in the "
                   "tuft generator -- the same wall the bogbean and the marsh "
                   "cinquefoil hit and the same one "
                   "`tools/seed_groundcover.py` records for the ferns. Authored "
                   "as a low dense mat of wide dark stems, which is the mass "
                   "without the division.\n\n"
                   "DENSE IS THE POINT: 30 stems in a 0.09 m crown at a 0.25 m "
                   "spacing, so placement makes a continuous choking mat. "
                   "Watercress does not grow in tufts, it fills a channel bank "
                   "to bank.\n\n"
                   "`leaf_jungle` for the very dark glossy green.",
             **t(stems=30, spread_m=0.09, splay_deg=40, arc=0.62, width_m=0.04,
                 taper=0.55, wander=0.34, length_var=0.30, base_m=0.10,
                 head="none", mat_stem="leaf_jungle",
                 bio_temperate_forest=1.0, bio_grassland=0.8,
                 place_abundance=0.8, place_spacing_m=0.25, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=15)),
    ),

    # ================================================================
    # RIVER MOSSES, LIVERWORTS AND ALGAL MATS
    # ================================================================

    "willow-moss": (
        "grass 0.35 m - dark olive streamers on a rock, combed downstream",
        base("grass", name="willow-moss", height_m=0.35,
             notes="AUTHORED UP, AND IT IS THE WORST IN EITHER AQUATIC FILE "
                   "ALONGSIDE ITS TWO NEIGHBOURS. Real Fontinalis shoots are "
                   "0.05-0.30 m (estimate); this is authored at 0.35, which at "
                   "the bottom of that range is 7x and at the top is 1.2x. The "
                   "library's shipped ceiling is 2.2x "
                   "(`clown-anemonefish`).\n\n"
                   "IT IS AUTHORED ANYWAY BECAUSE THE ALTERNATIVE IS A BARE "
                   "BOULDER. Every submerged stone in a clean upland river is "
                   "covered in this, it is the single commonest living surface "
                   "in fresh water, and at 5 cm a 5 cm moss is one voxel -- "
                   "which is not an object, it is the top of the rock. That is "
                   "the same argument `tools/seed_groundcover.py` records for "
                   "eleven land mosses and cushions, and the same open question: "
                   "a continuous mat may belong in the material palette rather "
                   "than in the asset library. Until that is decided, this is "
                   "the compromise the library already uses everywhere else -- "
                   "author it to read, and write down that you did.\n\n"
                   "`arc` 0.84 combs it flat downstream, which is the second "
                   "highest in the file after the crowfoot. Very fine, very "
                   "numerous, very dark: 46 stems at 0.02 m in `leaf_jungle`.\n\n"
                   "Permanently submerged, 0.1-2 m, attached to stone in moving "
                   "water. `slope_max_pct` 45 because it grows on the sides of "
                   "boulders, not just their tops.",
             **t(stems=46, spread_m=0.08, splay_deg=44, arc=0.84, width_m=0.02,
                 taper=0.80, wander=0.44, length_var=0.34, base_m=0.09,
                 head="none", mat_stem="leaf_jungle",
                 bio_temperate_forest=1.0, bio_taiga=0.9,
                 bio_tundra_alpine=0.6,
                 place_abundance=0.9, place_spacing_m=0.3, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=45)),
    ),
    "brook-moss-cushion": (
        "grass 0.22 m - a dense bright-green cushion on a splashed boulder",
        base("grass", name="brook-moss-cushion",
             height_m=0.22,
             notes="AUTHORED UP: real brook mosses form cushions 0.03-0.08 m "
                   "deep (estimate), so 0.22 is roughly 3x to 7x. See "
                   "`willow-moss` for the full argument; the short version is "
                   "that one voxel is not an object and a bare splash-zone "
                   "boulder is the wrong picture.\n\n"
                   "THE PAIR IS THE POINT. `willow-moss` is submerged, dark and "
                   "combed flat by current; this is a SPLASH-ZONE cushion -- out "
                   "of the water, bright yellow-green, and mounded rather than "
                   "streaming. `arc` 0.30 against the willow moss's 0.84 and "
                   "`splay_deg` 48 against 44 is the whole difference, and "
                   "between them the two describe a wet boulder from waterline "
                   "to top.\n\n"
                   "Extremely dense -- 60 stems, the highest count in either "
                   "aquatic file, in a 0.10 m crown -- because a cushion moss is "
                   "solid. At 5 cm that is a chunky green mound, which is what a "
                   "cushion looks like.\n\n"
                   "Above the waterline but permanently wet, in the spray of a "
                   "fall or a rapid.",
             **t(stems=60, spread_m=0.10, splay_deg=48, arc=0.30, width_m=0.02,
                 taper=0.70, wander=0.30, length_var=0.24, base_m=0.11,
                 head="none", mat_stem="grass",
                 bio_taiga=1.0, bio_temperate_forest=0.9,
                 bio_tundra_alpine=0.7,
                 place_abundance=0.9, place_spacing_m=0.25, place_cluster=1.0,
                 place_water_max_m=2, place_slope_max_pct=55)),
    ),
    "water-earwort": (
        "grass 0.20 m - dark reddish overlapping scales flat on acid-stream stone",
        base("grass", name="water-earwort", height_m=0.20,
             notes="A LIVERWORT, WHICH IS NOT A MOSS, and the difference is "
                   "visible: Scapania's leaves are folded in two and overlap "
                   "flat in one plane like roof tiles, where a moss's spiral "
                   "round the stem. Neither the fold nor the plane exists in "
                   "the tuft generator, so what separates it from the two mosses "
                   "here is what CAN be said -- it is flatter (`arc` 0.60, "
                   "`splay_deg` 56, so it lies over rather than mounding), "
                   "coarser (0.03 m stems against their 0.02) and a different "
                   "colour.\n\n"
                   "THE COLOUR IS THE FIELD MARK AND IT IS THE USUAL PROBLEM. "
                   "This liverwort is a dark reddish purple-brown -- the sign of "
                   "an acid, metal-rich, nutrient-poor stream -- and there is no "
                   "red in `materials.stem`'s seven land-vegetation choices. "
                   "`podzol` (86,72,60) is the darkest brown and is the same "
                   "compromise all four red algae in the saltwater file make.\n\n"
                   "AUTHORED AT EXACTLY THE 0.20 m FLOOR against a real 0.02-"
                   "0.05 m (estimate), which is 4x to 10x -- the largest "
                   "enlargement anywhere in this pass. It is here because acid "
                   "upland streams otherwise have nothing on their stones at "
                   "all, and it is the last thing that should be authored "
                   "rather than the first.",
             **t(stems=38, spread_m=0.09, splay_deg=56, arc=0.60, width_m=0.03,
                 taper=0.70, wander=0.36, length_var=0.26, base_m=0.10,
                 head="none", mat_stem="podzol",
                 bio_tundra_alpine=1.0, bio_taiga=0.9,
                 bio_temperate_forest=0.4,
                 place_abundance=0.7, place_spacing_m=0.3, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=50)),
    ),
    "blanket-weed": (
        "grass 0.40 m - coarse bright-green cotton-wool filaments off a stone",
        base("grass", name="blanket-weed", height_m=0.40,
             notes="THE SIGN THAT SOMETHING IS WRONG. Cladophora blooms when a "
                   "watercourse is over-enriched, and a river choked with "
                   "blanket weed is a river with fertiliser in it -- which makes "
                   "this the one plant in either aquatic file whose presence is "
                   "information rather than scenery.\n\n"
                   "FILAMENTS, WHICH IS THE ONE THING A TUFT DRAWS PERFECTLY. It "
                   "is unbranched threads from an attachment point, and a tuft "
                   "is a spray of stems from a crown; no forking, no whorls, no "
                   "blades, nothing missing. It and the anemones in the "
                   "saltwater file are the only species in this pass that lose "
                   "nothing to the generator.\n\n"
                   "The finest and most numerous thing here: 52 stems at 0.02 m "
                   "-- a one-voxel thread at 5 cm -- arced 0.82 so they stream, "
                   "with `wander` 0.55 so they tangle. Real filaments are 0.1 mm "
                   "and there are millions; this is fifty threads at two "
                   "hundred times life thickness, and it is the mass that "
                   "reads.\n\n"
                   "Attached to stone in 0.1-1 m of slow enriched water.",
             **t(stems=52, spread_m=0.07, splay_deg=40, arc=0.82, width_m=0.02,
                 taper=0.85, wander=0.55, length_var=0.40, base_m=0.08,
                 head="none", mat_stem="grass",
                 bio_grassland=1.0, bio_temperate_forest=0.8,
                 place_abundance=0.7, place_spacing_m=0.3, place_cluster=1.0,
                 place_water_max_m=1, place_slope_max_pct=30)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "freshwater plant specs")
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
