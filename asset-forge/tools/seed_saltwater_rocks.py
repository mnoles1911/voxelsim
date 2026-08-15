"""Author the sea floor: reef structure, submarine stone, the massive corals
and the sponges.

Twenty-three specs. The enumeration and the decisions behind them are in
`docs/aquatic-species.md`; this file is the build. It follows the six sea-floor
rocks the `hosts` change unblocked (`submerged-granite-boulder`, `boulder-reef`,
`bedrock-ledge`, `seamount-flank`, `sea-cave-mouth`, `rubble-apron`) and adds
what a reef, a vent field and a lava coast are made of.

TEN CENTIMETRES, AND NOTHING ELSE. A rock joins the world's own voxel grid and
is destructible exactly as terrain is (`forge/kinds.py:29-59`); the grid has one
cell size, `vxc::kVoxelSizeMm` = 100 mm, and `forge.cli.selftest` refuses any
other value on a terrain-lattice kind. That is not a preference and it is the
whole reason the coral set is SPLIT across two files.

WHY SEVEN CORALS AND FOUR SPONGES ARE IN THE ROCK FILE. Measured on
`branching-stony-coral` built at four lattices: 32,055 voxels at 1 cm, 5,302 at
2, 1,010 at 5 and 397 at 10, with the branch tip 5.00 / 2.50 / 1.00 / 0.50
voxels across. At 10 cm a branching coral loses 92.5% of itself and a 2.5 cm
branch is drawn at the one-voxel minimum -- four times life size, with the gaps
between branches closing at the same rate. So the BRANCHING corals are `bush` at
2 cm and live in `tools/seed_saltwater_plants.py`.

The MASSIVE corals are the opposite case and the measurement runs the other way.
A brain coral, a boulder star, a lettuce plate and a barrel sponge have no
feature under about 20 cm; a 10 cm voxel holds every one of them with relief to
spare. And being on the terrain lattice is CORRECT for them rather than merely
tolerable -- a reef is stone, a player should be able to break it, and a reef
built out of detail-lattice bushes would be a reef you cannot break. That is the
argument, and it is why the split is by form rather than by taxonomy.

WHAT THE GENERATOR CANNOT DO, STATED ONCE. It only CUTS. Three species here are
built up rather than eroded down -- the black smoker, the hydrothermal mound and
every coral in the file -- and are carved approximations, exactly as
`tufa-curtain` already records about itself. And there is no through-bore:
`rock.pans` dishes a TOP surface where water has nowhere lower to go, which is
not the same as a hole. The barrel sponge's central well and the tube sponge's
tubes are dished tops, not tubes.

`rock.size_m` IS MEASURED, NOT ESTIMATED. Every step of the build takes mass
away, so the raw lumps start larger and the builder measures the result and
corrects on the same seed. The number in each spec is what comes out.

ONE CONNECTED PIECE. `tools/buildcheck.py` enforces it at 26-connectivity, and
this file is full of things that are plural in life: a reef, a rubble bank, a
maerl bed, an oyster reef, a tube sponge cluster. Each is authored as ONE lump
of the whole and placement makes the field. `rock.spread` and `rock.rubble` are
what make a single lump look like a heap without becoming several.

    python tools/seed_saltwater_rocks.py
    python tools/seed_saltwater_rocks.py --force

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


def base(**over):
    changes = {
        "kind": "rock",
        "resolution_cm": "10",
        "variation.amount": 1.0,
        "variation.height": 0.22,
        "variation.shape": 0.18,
        "variation.rotate": True,
    }
    changes.update(over)
    return changes


def r(**kw):
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials." + k[len("mat_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        else:
            out["rock." + k] = v
    return out


SEA = dict(place_elev_min_m=-10.0, place_elev_max_m=0.0)
SHORE = dict(place_elev_min_m=-10.0, place_elev_max_m=4.0)


SPECIES = {

    # ================================================================
    # REEF STRUCTURE
    # ================================================================

    "reef-spur": (
        "3.20 m - a long finger of reef running seaward, groove either side",
        base(name="reef-spur",
             notes="SPUR-AND-GROOVE IS THE LARGEST PATTERN ON ANY REEF AND IT IS "
                   "MADE OF THIS ONE PIECE. Every surf-facing tropical coast "
                   "builds parallel reef fingers running straight out to sea "
                   "with sand-floored channels between them, at a spacing of "
                   "ten to thirty metres; that pattern is a PLACEMENT result "
                   "and the spur is the unit it repeats. Authoring the pattern "
                   "here would break the one-piece rule for nothing.\n\n"
                   "So it is authored as a long thing: `elongate` 3.4, the "
                   "highest in this file, on a low flattened mass. Aligning it "
                   "perpendicular to the shore is placement's job and nothing "
                   "here can ask for it, which is worth knowing before someone "
                   "scatters these at random rotations and gets a rubble field "
                   "instead of a reef.\n\n"
                   "`sand` for the palette -- consolidated reef limestone is a "
                   "warm cream and `sand` at (214,192,140) is the palest and "
                   "warmest of the five rock materials. Its sandstone patch "
                   "mottle also breaks the long flank up, which a flat grey "
                   "would not.",
             **r(size_m=3.2, lumps=6, spread=0.45, flatten=0.52, elongate=2.60,
                 angular=0.30, facets=3, rough=0.52, erode=0.28,
                 cavernous=0.28, pans=0.30, pan_depth_m=0.25,
                 bury=0.30, rubble=0.35, mat_rock="sand",
                 bio_ocean=1.0, bio_beach=0.4,
                 place_abundance=0.5, place_spacing_m=14.0, place_cluster=0.85,
                 place_slope_max_pct=45, **SEA)),
    ),
    "patch-reef-head": (
        "2.60 m - an isolated reef mound alone on sand, undercut at the foot",
        base(name="patch-reef-head",
             notes="THE ONE THAT STANDS ALONE, and the undercut is why it reads "
                   "as reef rather than as a boulder. A patch reef grows up "
                   "faster than it grows out and grazing fish keep the base "
                   "clear, so the whole mound overhangs its own footing -- "
                   "which is `rock.notch` at 1.4 with a narrow spread, the same "
                   "parameter `tidal-notch` uses at 3.2 for a shoreline and "
                   "which nothing else in the sea-floor set touches.\n\n"
                   "Tall for its width (`flatten` 1.35) and heavily pitted "
                   "(`cavernous` 0.55), because a patch reef is riddled with "
                   "holes that every reef fish in the library lives in. The "
                   "holes are decimetre-scale here and real ones run from a "
                   "centimetre up, so what ships is the large end of a real "
                   "distribution.",
             **r(size_m=2.6, lumps=5, spread=0.38, flatten=1.35, elongate=1.20,
                 angular=0.24, facets=2, rough=0.55, erode=0.42,
                 cavernous=0.55, notch=1.4, notch_z_m=0.35,
                 notch_spread_m=0.20, bury=0.24, rubble=0.30,
                 mat_rock="sand",
                 bio_ocean=1.0,
                 place_abundance=0.4, place_spacing_m=9.0, place_cluster=0.6,
                 place_slope_max_pct=35, **SEA)),
    ),
    "reef-flat-pavement": (
        "3.00 m - a near-flat pitted limestone floor barely proud of the sand",
        base(name="reef-flat-pavement",
             notes="THE FLOOR BEHIND EVERY REEF CREST, and the flattest thing in "
                   "the library after `rockpool-platform`: `flatten` 0.20 with "
                   "`bury` 0.50, so half of it is in the ground and what shows "
                   "is a pavement rather than an object.\n\n"
                   "IT IS WORTH A SPEC BECAUSE A REEF LAGOON IS MOSTLY THIS. "
                   "The corals and the boulders are what a player looks at; the "
                   "pavement is what they are standing on, and a lagoon floor "
                   "made of bare mud is the wrong biome in the same way a "
                   "forest floor made of meadow grass is.\n\n"
                   "`pans` 0.70 hollows the surface wherever water has nowhere "
                   "lower to go, which on a reef flat is exactly right -- the "
                   "pools left at low tide are the feature -- and it is the same "
                   "mechanism `rockpool-platform` uses on a temperate shore.",
             **r(size_m=3.0, lumps=7, spread=0.60, flatten=0.20, elongate=1.55,
                 angular=0.35, facets=4, rough=0.45, erode=0.45,
                 cavernous=0.45, pans=0.70, pan_depth_m=0.30,
                 bedding=0.30, bed_thickness_m=0.45, bed_dip_deg=2.0,
                 bury=0.50, rubble=0.25, mat_rock="sand",
                 bio_ocean=1.0, bio_beach=0.6,
                 place_abundance=0.6, place_spacing_m=7.0, place_cluster=0.8,
                 place_slope_max_pct=15, **SHORE)),
    ),
    "coral-rubble-bank": (
        "1.40 m - a heap of broken coral sticks and plates, sharp and unsorted",
        base(name="coral-rubble-bank",
             notes="WHAT A STORM LEAVES. A reef crest breaks in a cyclone and "
                   "the pieces pile up behind it, and that bank is often the "
                   "only thing above water for miles -- every low coral island "
                   "there is starts as one.\n\n"
                   "UNSORTED AND ANGULAR IS THE WHOLE DESCRIPTION, and it is the "
                   "same build `moraine-boulder-ridge` uses for the same reason: "
                   "`spread` 0.80 with `rubble` 0.85 and a heavy clast field, so "
                   "the stone is a heap rather than a mass. What separates the "
                   "two is that a moraine's clasts are rounded and a reef's are "
                   "SHARP -- broken coral sticks are knife-edged for years -- so "
                   "`angular` is 0.62 here against the moraine's 0.52 and the "
                   "erosion is much lower.\n\n"
                   "`clast_size_m` 0.22 against a real broken staghorn stick of "
                   "10-20 cm (estimate). The parameter floors at 0.08 m and its "
                   "own help says under about 15 cm a clast reads as noise, so "
                   "0.22 is the smallest that reads at the terrain lattice.",
             **r(size_m=1.8, lumps=8, spread=0.62, flatten=0.68, elongate=1.80,
                 angular=0.62, facets=5, rough=0.58, erode=0.14,
                 cavernous=0.0, clasts=180, clast_size_m=0.22,
                 clast_hardness=2.0, bury=0.28, rubble=0.55,
                 mat_rock="sand",
                 bio_ocean=1.0, bio_beach=0.8,
                 place_abundance=0.7, place_spacing_m=3.0, place_cluster=1.0,
                 place_slope_max_pct=40, **SHORE)),
    ),
    "maerl-bed": (
        "1.20 m - a low lens of loose pink coralline knuckles",
        base(name="maerl-bed",
             notes="A BED, NOT A NODULE, AND THAT IS A DELIBERATE CHOICE. One "
                   "maerl knuckle is 2-5 cm across (estimate), which is under "
                   "the owner's 20 cm species floor and half a voxel at the "
                   "terrain lattice; it cannot be an asset. The BED is the "
                   "habitat -- a metres-thick living gravel of loose coralline "
                   "algae, one of the slowest-growing and most protected "
                   "bottoms there is -- and a lens of it is one connected "
                   "piece.\n\n"
                   "SO THE KNUCKLES ARE `rock.clasts` AND THEY ARE TWICE LIFE "
                   "SIZE. `clast_size_m` floors at 0.08 m, so each one is 8 cm "
                   "against a real 2-5. That is the identical trap "
                   "`conglomerate-boulder` records for pebbles, at half the "
                   "scale, and it is the reason this reads as coarse gravel "
                   "rather than as maerl.\n\n"
                   "Built as a LENS like `gravel-bar`: very flat, elongated, "
                   "deeply buried, so what shows is a bank of loose material "
                   "rather than a stone. Pink is not in the rock palette; "
                   "`gravel` (140,132,118) is the only material whose whole "
                   "identity is 'made of loose pieces'.",
             **r(size_m=1.6, lumps=7, spread=0.60, flatten=0.38, elongate=2.20,
                 angular=0.30, facets=2, rough=0.55, erode=0.22,
                 cavernous=0.0, clasts=260, clast_size_m=0.08,
                 clast_hardness=2.2, bury=0.34, rubble=0.30,
                 mat_rock="gravel",
                 bio_ocean=1.0,
                 place_abundance=0.5, place_spacing_m=5.0, place_cluster=0.95,
                 place_slope_max_pct=15, **SEA)),
    ),
    "oyster-reef-bank": (
        "1.60 m - a ridge of cemented shell, every face an overlapping plate",
        base(name="oyster-reef-bank",
             notes="A BIOGENIC REEF IN COLD WATER, which the tropics get all the "
                   "credit for and the temperate estuaries actually run on: an "
                   "oyster bank is built by animals, is metres thick, armours a "
                   "shoreline and is a completely different silhouette from any "
                   "coral -- a low ridge across the current, not a mound.\n\n"
                   "`coquina-shell-rock` ALREADY SHIPS AND IS NOT THIS. That one "
                   "is cemented shell FRAGMENT on a beach -- a rock made of "
                   "broken pieces. This is a living reef of WHOLE overlapping "
                   "valves, so the clasts are three times the size (0.24 m "
                   "against 0.10) and far fewer, and the mass is a ridge rather "
                   "than a block: `elongate` 2.6 and `flatten` 0.48.\n\n"
                   "Sharp-edged (`angular` 0.58, `erode` 0.16), because a live "
                   "oyster reef's shells have not been worn by anything.",
             **r(size_m=2.0, lumps=7, spread=0.52, flatten=0.62, elongate=2.20,
                 angular=0.58, facets=4, rough=0.60, erode=0.14,
                 cavernous=0.20, clasts=150, clast_size_m=0.24,
                 clast_hardness=3.2, bury=0.28, rubble=0.30,
                 mat_rock="gravel",
                 bio_ocean=1.0, bio_beach=0.9,
                 place_abundance=0.5, place_spacing_m=6.0, place_cluster=1.0,
                 place_slope_max_pct=20, **SHORE)),
    ),
    "honeycomb-worm-reef": (
        "1.20 m - a biogenic sandstone crust, its surface a mass of small holes",
        base(name="honeycomb-worm-reef",
             notes="THE HOLES ARE THE SPECIES AND THEY ARE FIFTY TIMES TOO "
                   "SMALL. A Sabellaria reef is a crust built out of millions of "
                   "sand tubes each about 5 mm across (estimate); at the 10 cm a "
                   "rock is locked to that is a twentieth of a voxel, and the "
                   "lattice is not free for this kind. Not close, and not "
                   "rescuable.\n\n"
                   "WHAT SHIPS IS `rock.cavernous` AT 0.80 -- the highest in "
                   "this file -- which pits the surface at decimetre scale. That "
                   "is the right MECHANISM (once a pit exists it traps water and "
                   "deepens faster, which is genuinely how the runaway works) at "
                   "the wrong SCALE, so it reads as a pocked crust rather than a "
                   "honeycomb. The honeycomb is a texture ask, exactly as "
                   "`brain-coral`'s grooves are.\n\n"
                   "A crust, so it is flat and mostly buried: it coats bedrock "
                   "and boulders on surf-exposed sand coasts rather than sitting "
                   "on them. `sand` because it is literally made of cemented "
                   "sand.",
             **r(size_m=1.6, lumps=6, spread=0.50, flatten=0.55, elongate=1.70,
                 angular=0.28, facets=2, rough=0.58, erode=0.22,
                 cavernous=0.45, rind=0.35, rind_m=0.16,
                 bury=0.30, rubble=0.20, mat_rock="sand",
                 bio_beach=1.0, bio_ocean=0.7,
                 place_abundance=0.6, place_spacing_m=4.0, place_cluster=1.0,
                 place_slope_max_pct=45,
                 place_elev_min_m=-6.0, place_elev_max_m=3.0)),
    ),

    # ================================================================
    # THE MASSIVE CORALS  --  rock, 10 cm; see the docstring
    # ================================================================

    "boulder-star-coral": (
        "1.80 m - a lumpy hemispherical mound covered in low knobs",
        base(name="boulder-star-coral",
             notes="THE REEF'S BULK BUILDER. Staghorn and elkhorn are what a "
                   "reef LOOKS like; Orbicella is what most of it is MADE of -- "
                   "slow massive mounds that survive the storms the branching "
                   "corals do not. Two of them and a brain coral are a reef "
                   "framework.\n\n"
                   "IT IS THE PURE CASE FOR CORAL-AS-ROCK. Its smallest "
                   "identifying feature is the knobbing on its own surface at "
                   "10-30 cm (estimate), which is one to three voxels at the "
                   "terrain lattice -- the three-voxel rule, satisfied without "
                   "argument. Contrast the staghorn in "
                   "`tools/seed_saltwater_plants.py`, whose branch is half a "
                   "voxel here.\n\n"
                   "The knobs are `lumps` 7 at `spread` 0.30 with `rough` 0.55: "
                   "seven fused sub-masses under a rough surface, which is a "
                   "lumpy dome rather than a smooth one. `angular` 0.04 and no "
                   "facets, because nothing has ever fractured it.",
             **r(size_m=1.8, lumps=7, spread=0.30, flatten=0.88, elongate=1.10,
                 angular=0.04, facets=0, rough=0.55, erode=0.20,
                 cavernous=0.0, bury=0.24, rubble=0.10, mat_rock="sand",
                 bio_ocean=1.0,
                 place_abundance=0.6, place_spacing_m=3.5, place_cluster=0.8,
                 place_slope_max_pct=40, **SEA)),
    ),
    "pillar-coral": (
        "1.50 m - blunt vertical fingers rising off a shared encrusting base",
        base(name="pillar-coral",
             notes="THE ONE MASSIVE CORAL THAT IS TALL, and the only reason it "
                   "survives the 10 cm lattice while a staghorn does not: a "
                   "Dendrogyra pillar is 10-20 cm THROUGH (estimate) against a "
                   "staghorn branch's 2-5, so a pillar is one to two voxels and "
                   "a staghorn tine is a half. The line between the two coral "
                   "files runs exactly here.\n\n"
                   "`flatten` 2.4 makes it far taller than wide, and `lumps` 5 "
                   "at a 0.42 spread gives the several separate fingers -- fused "
                   "at the base, which is both what the animal does and what "
                   "`tools/buildcheck.py` requires. `columns` was the obvious "
                   "alternative and is wrong: it makes a polygonal crack network "
                   "for cooling basalt, which produces flat-sided prisms, and a "
                   "coral pillar is a rounded blunt club.\n\n"
                   "Fine surface (`rough` 0.40) with no facets and almost no "
                   "erosion.",
             **r(size_m=1.5, lumps=5, spread=0.42, flatten=2.40, elongate=1.15,
                 angular=0.05, facets=0, rough=0.40, erode=0.18,
                 cavernous=0.0, bury=0.20, rubble=0.12, mat_rock="sand",
                 bio_ocean=1.0,
                 place_abundance=0.35, place_spacing_m=6.0, place_cluster=0.7,
                 place_slope_max_pct=35, **SEA)),
    ),
    "lettuce-coral": (
        "0.90 m - whorled crinkled plates stacked in a rosette",
        base(name="lettuce-coral",
             notes="STACKED PLATES, AND `rock.bedding` IS WHAT MAKES THEM. "
                   "Bedding lays alternating hard and soft layers through the "
                   "stone and weathers the soft ones back, which on a "
                   "sedimentary cliff gives banding and on a squat dome gives "
                   "exactly this -- a stack of out-turned plates with recesses "
                   "between them. It is the biggest variety lever in the rock "
                   "generator and this is the only coral in the library that "
                   "uses it.\n\n"
                   "`bed_thickness_m` 0.14 is near the 0.05 floor and gives "
                   "plates about one and a half voxels apart, which is the "
                   "finest stacking the terrain lattice will carry. Real "
                   "Agaricia plates are 1-3 cm apart (estimate) and would need "
                   "1 cm, which a `rock` cannot have.\n\n"
                   "`bed_dip_deg` 12 tilts the whole stack, because a lettuce "
                   "coral grows toward the light rather than level, and a "
                   "perfectly horizontal stack reads as a machined part.",
             **r(size_m=1.3, lumps=4, spread=0.36, flatten=0.72, elongate=1.25,
                 angular=0.12, facets=1, rough=0.42, erode=0.20,
                 cavernous=0.10, bedding=0.55, bed_thickness_m=0.20,
                 bed_dip_deg=12.0, bury=0.22, rubble=0.15,
                 mat_rock="sand",
                 bio_ocean=1.0,
                 place_abundance=0.5, place_spacing_m=2.5, place_cluster=0.85,
                 place_slope_max_pct=55, **SEA)),
    ),
    "fire-coral": (
        "0.70 m - flat mustard-yellow upright blades with smooth white edges",
        base(name="fire-coral",
             notes="NOT A CORAL AT ALL -- Millepora is a hydrozoan, closer to a "
                   "jellyfish than to anything else in these two files -- and it "
                   "is here because it looks like one, occupies a coral's place "
                   "on a reef and is the single most memorable thing on it for "
                   "anyone who has touched one.\n\n"
                   "UPRIGHT BLADES, WHICH IS `flatten` 2.0 AND `elongate` 2.4 "
                   "TOGETHER: tall in one axis and long in another, which is a "
                   "standing plate. That combination appears nowhere else in the "
                   "library's rocks -- everything else is either a lump or a "
                   "pavement -- and it is the closest the generator gets to a "
                   "vertical sheet without a plane primitive.\n\n"
                   "Very smooth (`rough` 0.30, `angular` 0.02): fire coral's "
                   "surface is famously featureless, which is why people put a "
                   "hand on it. `sand` for the mustard-yellow, which is a cream; "
                   "no yellow in the five rock materials.",
             **r(size_m=1.1, lumps=5, spread=0.34, flatten=1.70, elongate=2.00,
                 angular=0.02, facets=0, rough=0.30, erode=0.14,
                 cavernous=0.0, bury=0.24, rubble=0.10, mat_rock="sand",
                 bio_ocean=1.0,
                 place_abundance=0.5, place_spacing_m=3.0, place_cluster=0.8,
                 place_slope_max_pct=55, **SEA)),
    ),
    # MUSHROOM CORAL IS NOT HERE, AND THE NUMBER IS WHY.
    #
    # A free-living Fungia is a single disc 0.10-0.25 m across (estimate) and a
    # few centimetres thick. At the 10 cm a `rock` is LOCKED to, that disc is
    # 2.5 voxels across and under one voxel thick: the WHOLE ORGANISM fails the
    # three-voxel rule, before anyone asks about the radial septa that name it.
    #
    # Measured, sweeping size against flatten at three seeds each:
    #
    #     0.75 m  flatten 0.42   21-25 voxels
    #     0.75 m  flatten 0.55   40-52
    #     0.90 m  flatten 0.55   49-60
    #     0.90 m  flatten 0.70   57-82
    #     1.10 m  flatten 0.55   82-113   <- comparable to `river-cobble` at 142
    #
    # So it takes 1.10 m to build an asset with as many voxels as a 0.9 m river
    # cobble, and 1.10 m is FOUR TIMES life size. The library's shipped ceiling
    # for authoring up is 2.2x (`clown-anemonefish`). Broadening the row to the
    # large free-living fungiids -- Herpolitha, Ctenactis, which reach about
    # half a metre -- still leaves it at 2x with 60 voxels.
    #
    # It is not authored. The lattice is not free for this kind, so there is no
    # fix on this side: a mushroom coral needs a detail-lattice kind, and every
    # detail-lattice kind here is a branching skeleton or a spray of stems, and
    # a fungiid is a solid disc. `docs/aquatic-species.md` records it as blocked.

    "organ-pipe-coral": (
        "0.40 m - tight parallel red vertical tubes with flat tops",
        base(name="organ-pipe-coral",
             notes="THE ONE CORAL `rock.columns` IS RIGHT FOR, and it is worth "
                   "saying why, because `pillar-coral` next door explicitly "
                   "rejects the same parameter. Columns build a 2D Voronoi "
                   "tessellation and extrude it along an axis, giving "
                   "flat-sided prisms packed edge to edge -- which is wrong for "
                   "a coral pillar (a rounded club) and RIGHT for Tubipora, "
                   "whose skeleton genuinely is a bundle of parallel tubes fused "
                   "side by side with flat tops.\n\n"
                   "`column_gap_m` 0.10 is the smallest gap that is one visible "
                   "voxel at the terrain lattice. Real organ-pipe tubes are "
                   "1-2 mm across (estimate) and there are hundreds; what ships "
                   "is about twenty tubes at ten centimetres, which is fifty "
                   "times life size. That is a stylisation and it is the only "
                   "way the mechanism reads at all.\n\n"
                   "`column_stagger` 0.30 gives the tubes their own top heights, "
                   "because a colonnade sawn flat reads as an extruded shape "
                   "rather than as a living thing.\n\n"
                   "Blood red in life. No red in the rock palette; `clay` "
                   "(150,118,96) is the warmest of the five.",
             **r(size_m=1.20, lumps=3, spread=0.28, flatten=1.30, elongate=1.15,
                 angular=0.10, facets=0, rough=0.30, erode=0.10,
                 cavernous=0.0, columns=7, column_gap_m=0.14,
                 column_stagger=0.30, bury=0.18, rubble=0.08,
                 mat_rock="clay",
                 bio_ocean=1.0,
                 place_abundance=0.45, place_spacing_m=2.0, place_cluster=0.85,
                 place_slope_max_pct=50, **SEA)),
    ),
    "bubble-coral": (
        "0.40 m - a dome covered in fat rounded grape-sized vesicles",
        base(name="bubble-coral",
             notes="A DOME MADE OF SPHERES, which is the accretion step of "
                   "`forge/rock.py` used for what it literally is: the generator "
                   "unions ellipsoids as a field and thresholds them, so a rock "
                   "authored with MANY lumps at a WIDE spread and almost no "
                   "surface noise comes out as a cluster of fused balls. That is "
                   "a Plerogyra exactly.\n\n"
                   "`lumps` 12 at `spread` 0.62 with `rough` 0.16 -- the lowest "
                   "roughness in this file, and it is the one place the "
                   "generator's founding rule is relaxed on purpose. "
                   "`rock.rough` exists because a smooth curve at twenty voxels "
                   "shows its stair-steps as concentric contour rings; a bubble "
                   "coral IS a set of smooth curves and the rings are the honest "
                   "look. `wave-polished-boulder` makes the same trade and says "
                   "the same thing: if it reads as generated spheres rather than "
                   "as vesicles, raise the roughness and accept a less bubbly "
                   "coral. That is the owner's call from a render.\n\n"
                   "Real vesicles are 2-3 cm (estimate) and these are 10-15. "
                   "Twelve big ones rather than three hundred small.",
             **r(size_m=0.7, lumps=7, spread=0.40, flatten=0.85, elongate=1.10,
                 angular=0.0, facets=0, rough=0.16, erode=0.08,
                 cavernous=0.0, bury=0.20, rubble=0.05, mat_rock="sand",
                 bio_ocean=1.0,
                 place_abundance=0.4, place_spacing_m=2.2, place_cluster=0.8,
                 place_slope_max_pct=45, **SEA)),
    ),

    # ================================================================
    # SPONGES
    # ================================================================

    "barrel-sponge": (
        "1.40 m - a rough-walled barrel with a deep central well",
        base(name="barrel-sponge",
             notes="THE WELL IS THE SPECIES AND THE GENERATOR CANNOT BORE. A "
                   "giant barrel sponge is a metre-deep TUBE with walls a "
                   "handspan thick; `forge/rock.py` only cuts from the outside "
                   "and the nearest thing it has to a hole is `rock.pans`, which "
                   "runs water down the stone and hollows out wherever there is "
                   "nowhere lower to go. That dishes a TOP. So what ships is a "
                   "barrel with a deep bowl in it rather than a tube, which "
                   "reads from three metres and is wrong from one.\n\n"
                   "`pans` 0.95 and `pan_depth_m` 0.55 are both the hardest "
                   "settings in the library, which is the honest way to say "
                   "'this parameter is being asked for more than it has'. "
                   "`docs/aquatic-species.md` §8.4 books it against a "
                   "subtraction feature that would also fix the bedrock pothole "
                   "and the black smoker's conduit -- three species, one "
                   "mechanism.\n\n"
                   "THE ROUGHNESS IS THE COMPENSATION. A barrel sponge's outer "
                   "wall is deeply grooved and pitted, so `rough` 0.68 and "
                   "`cavernous` 0.60 give it a surface worth looking at even "
                   "when the silhouette is a bucket. `clay` for the rust-brown "
                   "that most large barrel sponges are.",
             **r(size_m=1.4, lumps=4, spread=0.26, flatten=1.45, elongate=1.10,
                 angular=0.06, facets=0, rough=0.68, erode=0.30,
                 cavernous=0.60, pans=0.95, pan_depth_m=0.55,
                 bury=0.20, rubble=0.10, mat_rock="clay",
                 bio_ocean=1.0,
                 place_abundance=0.4, place_spacing_m=5.0, place_cluster=0.6,
                 place_slope_max_pct=45, **SEA)),
    ),
    "tube-sponge-cluster": (
        "1.00 m - a bundle of tall thin vertical tubes, splayed slightly",
        base(name="tube-sponge-cluster",
             notes="THE SAME HOLE PROBLEM AS THE BARREL AND A DIFFERENT ANSWER. "
                   "An Aplysina cluster is several separate tubes rising "
                   "together from one base; the tubes cannot be hollow here, so "
                   "what is authored is the CLUSTER instead -- `columns` 9 at a "
                   "0.16 m gap with heavy stagger, which gives nine separate "
                   "prisms of different heights fused at the foot. That is the "
                   "silhouette, and the silhouette is what a tube sponge is "
                   "recognised by from any distance at which it matters.\n\n"
                   "Reusing `rock.columns` for a sponge is the third time in "
                   "these two files that a basalt mechanism has turned out to be "
                   "the right shape for an animal, and the reason is always the "
                   "same: a Voronoi tessellation extruded up an axis is a "
                   "generic 'bundle of vertical things' and cooling lava is only "
                   "one of them.\n\n"
                   "Tall (`flatten` 2.6) and slightly splayed by `column_stagger` "
                   "and the lump spread together. Purple and blue in life; `clay` "
                   "is the least grey of the five and it is a brown.",
             **r(size_m=1.0, lumps=4, spread=0.34, flatten=2.60, elongate=1.20,
                 angular=0.08, facets=0, rough=0.44, erode=0.22,
                 cavernous=0.20, columns=9, column_gap_m=0.16,
                 column_stagger=0.55, bury=0.22, rubble=0.10,
                 mat_rock="clay",
                 bio_ocean=1.0,
                 place_abundance=0.45, place_spacing_m=3.5, place_cluster=0.75,
                 place_slope_max_pct=60, **SEA)),
    ),
    "elephant-ear-sponge": (
        "1.00 m - a broad ruffled fan standing on edge off the reef wall",
        base(name="elephant-ear-sponge",
             notes="THE NEAREST THING TO A PLANE THIS LIBRARY BUILDS, and it is "
                   "worth recording that it is not close. A gorgonian sea fan "
                   "was left unbuilt entirely because it is a flat rigid NET and "
                   "nothing here makes a plane "
                   "(`docs/aquatic-species.md` §8.1); an elephant-ear sponge is "
                   "a flat rigid SHEET, which is the same problem without the "
                   "mesh, and it is buildable only because a sheet with "
                   "thickness is still a solid.\n\n"
                   "`flatten` 2.2 with `elongate` 2.8 and only three lumps at a "
                   "tight spread: tall, wide and thin, which is as close to a "
                   "standing plate as an accretion of ellipsoids gets. It will "
                   "be a slab rather than a sheet -- expect two to four voxels "
                   "through at the terrain lattice against a real 3-5 cm "
                   "(estimate), so it is two to four times too thick and that is "
                   "the one dimension the lattice cannot give back.\n\n"
                   "The ruffled rim is `rock.erode` at 0.50 with high roughness, "
                   "which frays the outline. Orange in life; `clay` again.",
             **r(size_m=1.6, lumps=4, spread=0.32, flatten=1.55, elongate=1.90,
                 angular=0.10, facets=1, rough=0.50, erode=0.22,
                 cavernous=0.10, bury=0.22, rubble=0.12, mat_rock="clay",
                 bio_ocean=1.0,
                 place_abundance=0.4, place_spacing_m=4.0, place_cluster=0.7,
                 place_slope_max_pct=70, **SEA)),
    ),
    "vase-sponge": (
        "0.45 m - one deep flaring cup with a rough knobbly outer wall",
        base(name="vase-sponge",
             notes="THE SMALL VERSION OF THE BARREL AND THE ONE WHERE `pans` "
                   "NEARLY WORKS. A vase sponge's cavity is shallow relative to "
                   "its width -- a cup rather than a tube -- so a dished top is "
                   "much closer to the truth here than it is on the barrel "
                   "beside it. Same parameter, same setting, a species that "
                   "suits it.\n\n"
                   "`flatten` 1.60 makes it taller than wide and `pan_depth_m` "
                   "0.22 is half the object's height, which is a genuine cup at "
                   "the terrain lattice: 2 voxels of wall around 2 voxels of "
                   "hollow on a 4.5-voxel object. That is thin, and it is the "
                   "smallest hollow the library contains.\n\n"
                   "The knobbly outer wall is `rough` 0.66 with `cavernous` "
                   "0.40, which pits it; the smooth inner wall real vase sponges "
                   "have cannot be differentiated from the outer one, because "
                   "`rock.aspect` weathers one SIDE harder and not one "
                   "SURFACE.",
             **r(size_m=0.95, lumps=3, spread=0.28, flatten=1.35, elongate=1.15,
                 angular=0.05, facets=0, rough=0.52, erode=0.18,
                 cavernous=0.22, pans=0.50, pan_depth_m=0.22,
                 bury=0.20, rubble=0.08, mat_rock="clay",
                 bio_ocean=1.0,
                 place_abundance=0.5, place_spacing_m=2.0, place_cluster=0.8,
                 place_slope_max_pct=55, **SEA)),
    ),

    # ================================================================
    # SUBMARINE STONE: VOLCANIC, HYDROTHERMAL, KARST AND STORM
    # ================================================================

    "pillow-lava-mound": (
        "2.20 m - a pile of rounded bulbous lobes with cracked glassy crusts",
        base(name="pillow-lava-mound",
             notes="THE MOST COMMON ROCK SURFACE ON EARTH AND THE LIBRARY HAD "
                   "NONE OF IT. Most of the planet's crust is ocean floor and "
                   "most of that is pillow basalt -- lava extruded under water "
                   "chills instantly into a skin, balloons, splits, and extrudes "
                   "the next lobe out of the split.\n\n"
                   "`rind` IS THE MECHANISM AND IT IS EXACTLY RIGHT FOR ONCE. A "
                   "pillow's identity is a HARD GLASSY SKIN over a softer "
                   "interior, which is what `rock.rind` models, and the crust's "
                   "radial cracks are what `rock.erode` frays. Set at 0.75 with "
                   "a 0.12 m rind, which is one voxel of skin at the terrain "
                   "lattice -- the thinnest that reads.\n\n"
                   "The bulbous lobes are `lumps` 9 at `spread` 0.66 with a very "
                   "low `angular`, because a pillow has no fracture faces at all "
                   "-- it is a bag of lava and every surface is a curve. "
                   "`bedrock` (72,70,74) is the darkest rock material and fresh "
                   "submarine basalt is near-black.",
             **r(size_m=2.2, lumps=9, spread=0.66, flatten=0.72, elongate=1.35,
                 angular=0.04, facets=0, rough=0.44, erode=0.34,
                 cavernous=0.0, rind=0.75, rind_m=0.12,
                 bury=0.30, rubble=0.25, mat_rock="bedrock",
                 bio_ocean=1.0, bio_beach=0.4,
                 place_abundance=0.6, place_spacing_m=5.0, place_cluster=0.9,
                 place_slope_max_pct=55, **SHORE)),
    ),
    "black-smoker-chimney": (
        "3.00 m - a narrow ragged spire, flaring at the foot, black to rust",
        base(name="black-smoker-chimney",
             notes="THE MOST EXTREME SHAPE IN THE ROCK LIBRARY AND THE ONE THE "
                   "GENERATOR IS LEAST SUITED TO, and both halves of that are "
                   "worth writing down.\n\n"
                   "IT IS ACCRETED, AND THE GENERATOR ONLY CUTS. A chimney is "
                   "built UP from mineral precipitating out of 350-degree vent "
                   "fluid the instant it hits 2-degree seawater -- it grows "
                   "several metres a year, from nothing, upward. There is no "
                   "accretion step anywhere in `forge/rock.py`; every mechanism "
                   "in it takes mass away. `tufa-curtain` records the identical "
                   "problem and is the precedent for carving one anyway.\n\n"
                   "IT IS ALSO A TUBE. The conduit up the middle is what the "
                   "fluid comes out of, and there is no through-bore -- the same "
                   "gap the barrel sponge and the bedrock pothole hit. Three "
                   "species, one missing subtraction.\n\n"
                   "What IS authored: `flatten` 3.60, the tallest-for-its-width "
                   "setting in this file, on three lumps at a tight spread, with "
                   "the flare at the foot coming from `bury` and lump spread "
                   "rather than from growth. `flutes` 0.60 runs vertical runnels "
                   "down the outside, which is what the fluid does. `erode` 0.55 "
                   "and `rough` 0.70 make the spire ragged rather than clean, "
                   "which is most of what separates it from a termite mound.\n\n"
                   "`bedrock` for the sulphide black. It should be rust-streaked "
                   "and there is no second colour on a rock.\n\n"
                   "DEPTH: 2,000 m or more. `placement.elev_min_m` floors at "
                   "-10.0, so this authors the same depth as a rock pool.",
             **r(size_m=3.0, lumps=3, spread=0.24, flatten=3.60, elongate=1.15,
                 angular=0.20, facets=2, rough=0.70, erode=0.55,
                 cavernous=0.30, flutes=0.60, flute_width_m=0.22,
                 bury=0.24, rubble=0.40, mat_rock="bedrock",
                 bio_ocean=1.0,
                 place_abundance=0.15, place_spacing_m=12.0, place_cluster=1.0,
                 place_slope_max_pct=60, **SEA)),
    ),
    "hydrothermal-mound": (
        "2.40 m - a low crumbling sulphide mound around a vent field",
        base(name="hydrothermal-mound",
             notes="WHAT THE CHIMNEYS STAND ON. A vent field is not a spire on "
                   "bare rock -- it is a mound of collapsed and re-cemented "
                   "sulphide tens of metres across, built out of every chimney "
                   "that ever fell over there, with new ones growing out of it. "
                   "The mound and the chimney are two assets that belong "
                   "together and neither reads alone.\n\n"
                   "CRUMBLING IS THE DESCRIPTION AND `rubble` 0.90 IS THE "
                   "HIGHEST IN THE LIBRARY. Sulphide mineral is brittle and "
                   "porous and falls apart under its own weight, so the surface "
                   "is loose blocks rather than stone. `erode` 0.60 and "
                   "`cavernous` 0.55 together give the pitted, undercut, "
                   "collapsing look; `clasts` at a coarse size are the fallen "
                   "chimney pieces embedded in it.\n\n"
                   "Low and broad against the chimney's spire -- `flatten` 0.55 "
                   "against 3.60 -- so the pair contrast rather than repeat. "
                   "`clay` for the rust: sulphide mounds oxidise to a strong "
                   "orange-brown, which is the one colour the rock palette can "
                   "nearly reach.",
             **r(size_m=2.4, lumps=8, spread=0.62, flatten=0.55, elongate=1.40,
                 angular=0.40, facets=3, rough=0.66, erode=0.60,
                 cavernous=0.55, clasts=200, clast_size_m=0.30,
                 clast_hardness=1.8, bury=0.34, rubble=0.90,
                 mat_rock="clay",
                 bio_ocean=1.0,
                 place_abundance=0.2, place_spacing_m=20.0, place_cluster=1.0,
                 place_slope_max_pct=50, **SEA)),
    ),
    "submerged-limestone-pavement": (
        "3.00 m - flat blocks split by deep straight water-widened joints",
        base(name="submerged-limestone-pavement",
             notes="A DROWNED KARREN FLOOR. `karren-pavement` already ships for "
                   "the land version; this is the same rock after sea level rose "
                   "over it, and the difference is not cosmetic -- limestone "
                   "dissolves far faster in seawater at the joints than on the "
                   "flats, so the grikes are DEEPER and straighter and the "
                   "clints between them are flatter than any land pavement's.\n\n"
                   "So `block_relief_m` is 0.30 against the land version's "
                   "smaller opening: three voxels of visible GAP between blocks, "
                   "and the gap is the whole effect -- a continuous mass with "
                   "faces drawn on it reads as one rock. `joint_sets` 2 with a "
                   "low scatter keeps every face parallel, which is what makes "
                   "stone read as jointed rather than as lumpy.\n\n"
                   "Very flat and deeply buried, like every pavement here.",
             **r(size_m=3.0, lumps=6, spread=0.52, flatten=0.44, elongate=1.45,
                 angular=0.62, facets=5, rough=0.36, erode=0.24,
                 cavernous=0.20, joint_sets=2, joint_scatter=0.06,
                 block_size_m=1.1, block_relief_m=0.16,
                 bedding=0.30, bed_thickness_m=0.55, bed_dip_deg=3.0,
                 bury=0.36, rubble=0.20, mat_rock="rock",
                 bio_ocean=1.0, bio_beach=0.7,
                 place_abundance=0.4, place_spacing_m=10.0, place_cluster=0.8,
                 place_slope_max_pct=25, **SHORE)),
    ),
    "lava-tube-bench": (
        "3.40 m - a flat basalt shelf with a collapsed edge and one straight lip",
        base(name="lava-tube-bench",
             notes="THE SHELF A VOLCANIC COAST IS MADE OF. Lava reaching the sea "
                   "runs in roofed tubes; the roof collapses and leaves a flat "
                   "bench with one long straight overhung edge where the tube "
                   "wall was. That straight edge on an otherwise irregular rock "
                   "is what identifies it.\n\n"
                   "`notch` 2.2 AT A SINGLE HEIGHT IS THE OVERHANG. It is the "
                   "same parameter `tidal-notch` uses and it is used here for a "
                   "structural reason rather than a tidal one, which is worth "
                   "noting because the two will sit on the same coast and read "
                   "as the same feature -- the tidal notch is at ONE height on "
                   "every rock along a shore, this one is at whatever height the "
                   "tube was.\n\n"
                   "`joint_sets` 2 with `block_relief_m` 0.18 gives the "
                   "columnar-ish blocky break basalt has; full columns "
                   "(`rock.columns`) were rejected here because a lava tube "
                   "cools too fast and too irregularly to build a proper "
                   "colonnade -- `basalt-colonnade` is the spec for that.\n\n"
                   "`bedrock` for near-black fresh basalt.",
             **r(size_m=3.4, lumps=6, spread=0.44, flatten=0.52, elongate=1.85,
                 angular=0.55, facets=5, rough=0.48, erode=0.22,
                 cavernous=0.18, notch=1.4, notch_z_m=0.55,
                 notch_spread_m=0.18, joint_sets=2, joint_scatter=0.12,
                 block_size_m=1.4, block_relief_m=0.14,
                 bury=0.30, rubble=0.30, mat_rock="bedrock",
                 bio_beach=1.0, bio_ocean=0.8, bio_bare_rock=0.4,
                 place_abundance=0.4, place_spacing_m=12.0, place_cluster=0.8,
                 place_slope_max_pct=60, **SHORE)),
    ),
    "storm-cast-boulder": (
        "2.40 m - a huge angular block thrown clear above the tideline",
        base(name="storm-cast-boulder",
             notes="THE ONE ROCK IN THE LIBRARY THAT IS IN THE WRONG PLACE ON "
                   "PURPOSE. A storm-cast block sits ABOVE the highest tide, "
                   "square on flat bare rock, with nothing around it that could "
                   "have produced it -- and that wrongness is the entire asset. "
                   "It is how a coast tells a player what the sea does here.\n\n"
                   "`bury` 0.06 IS THE LOWEST IN THE LIBRARY and it is the whole "
                   "point: everything else here is part-buried and settled, "
                   "which is what makes stone look like it belongs. This one "
                   "must look DROPPED. The rock generator's burial step exists "
                   "specifically so rocks do not look dropped, so this is that "
                   "step deliberately switched almost off.\n\n"
                   "Angular and fresh -- `angular` 0.72, `erode` 0.16 -- because "
                   "it was quarried out of a jointed cliff by a wave within "
                   "living memory and nothing has rounded it since. "
                   "`joint_sets` 3 so every face shares the frame it broke "
                   "along.\n\n"
                   "Placed at 4-12 m above sea level on shallow ground, which is "
                   "supratidal and is the only elevation band in these two files "
                   "that is entirely out of the water.",
             **r(size_m=2.4, lumps=3, spread=0.26, flatten=0.80, elongate=1.30,
                 angular=0.72, facets=6, rough=0.46, erode=0.16,
                 cavernous=0.0, joint_sets=3, joint_scatter=0.10,
                 block_size_m=2.0, bury=0.06, rubble=0.15,
                 mat_rock="rock",
                 bio_beach=1.0, bio_bare_rock=0.5,
                 place_abundance=0.2, place_spacing_m=25.0, place_cluster=0.7,
                 place_slope_max_pct=30,
                 place_elev_min_m=3.0, place_elev_max_m=14.0)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "saltwater rock specs")
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
