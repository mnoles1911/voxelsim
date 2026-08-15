"""Author fourteen rocks the shipped thirty-four do not cover.

THE ROCK SET IS THE BEST-SERVED KIND IN THE LIBRARY and this is deliberately a
short list. Thirty-four specs already ship and thirty-two of them carry a
bare-rock weight; the bare-rock file's own conclusion is that what is missing is
not more boulders but a SUBTRACTION capability -- overhangs, chimneys and cave
mouths -- which is generator work and not authoring. So this file adds only what
the existing generator can already say and the existing set does not:

  * THREE LITHOLOGIES with a distinct colour or fabric. Chalk is the only asset
    in the library that would be white; laterite is the tropical rust-red
    crust; conglomerate is a matrix visibly full of rounded pebbles, which is
    `rock.clasts` doing exactly what it was written for and which no shipped
    spec uses.
  * FOUR WATER-SHAPED FORMS -- a wave-polished boulder, a rockpool bench, a
    tidal notch and a rapids whaleback. Beach is 5.54% of land and wraps every
    shore in the world; three shipped rocks serve it.
  * TWO GLACIAL FORMS the tundra file asks for by name: a roche moutonnee,
    whose whole identity is that ONE end is smooth and the other is plucked
    ragged, and a moraine boulder ridge.
  * THE TWO SAVANNA LANDFORMS the savanna file names as the most recognisable
    objects on a plain that are not trees -- a granite kopje and a termite
    mound -- neither of which is a species and both of which are rock-generator
    work.
  * THREE FOREST-FLOOR ROCKS, which are ordinary blocks distinguished by how
    they sit rather than by what they are made of.

TEN CENTIMETRES, AND NOTHING ELSE, for the same reason the trees are: a rock
joins the world's own voxel grid and is destructible as terrain is
(`forge/kinds.py:29-58`), and `forge.cli.selftest` refuses any other size.

`rock.size_m` IS MEASURED, NOT ESTIMATED. Every step of the build takes mass
away, so the raw lumps start larger and the builder measures the result and
corrects on the same seed. The number in each spec below is what comes out.

WHAT IS NOT HERE, AND WHY. The blockfield, the talus cone, the rock glacier and
the patterned ground are all in the biome files and none is a spec: they are
DISTRIBUTIONS of blocks over ground rather than one block, which is a placement
feature serving five entries across two files. Faking any of them as one
hand-authored mega-boulder would be worse than leaving the gap visible.

    python tools/seed_landforms.py
    python tools/seed_landforms.py --force
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


SPECIES = {
    "chalk-outcrop": (
        "1.60 m - blinding white blocky low outcrop with dark flint bands",
        base(name="chalk-outcrop",
             notes="THE ONLY WHITE ROCK IN THE LIBRARY, and the bare-rock file "
                   "calls it the cheapest way to make one region's cliffs "
                   "unmistakable: the whole palette runs grey to sandstone.\n\n"
                   "THE MATERIAL IS THE PROBLEM AND `sand` IS THE ANSWER. There "
                   "is no white in `materials.rock`'s five choices -- rock, "
                   "bedrock, gravel, sand, clay -- and `sand` is the palest "
                   "and the one with sandstone's patch mottle, which reads as "
                   "chalk's own texture better than a flat grey would. It is "
                   "still a cream rather than a white. A chalk material is a "
                   "one-row ask.\n\n"
                   "Soft and BLOCKY rather than fractured: high angularity on "
                   "few facets, heavy bedding at a thin bed thickness for the "
                   "flint bands, and enough erosion to crumble the edges.",
             **r(size_m=1.6, lumps=5, spread=0.40, flatten=0.85, elongate=1.35,
                 angular=0.62, facets=5, rough=0.42, erode=0.34,
                 cavernous=0.0, bedding=0.55, bed_thickness_m=0.22,
                 bed_dip_deg=3.0, bury=0.30, rubble=0.35, mat_rock="sand",
                 bio_grassland=0.8, bio_beach=0.7, bio_bare_rock=0.8,
                 bio_temperate_forest=0.4,
                 place_abundance=0.3, place_spacing_m=18.0,
                 place_slope_max_pct=70)),
    ),
    "sarsen-stone": (
        "2.00 m - a hard rounded silcrete block lying loose on downland",
        base(name="sarsen-stone",
             notes="A BLOCK WITH NO BEDDING AT ALL, which is what silcrete is "
                   "and what separates it from every other big pale stone in "
                   "the set: no layers, no joints, no facets worth the name -- "
                   "just a very hard rounded lump lying where it was left. "
                   "Spheroidal weathering at a low rate, `joint_sets` 0, "
                   "`bedding` 0, and it sits proud rather than part-buried.",
             **r(size_m=2.0, lumps=4, spread=0.30, flatten=0.62, elongate=1.45,
                 angular=0.18, facets=2, rough=0.46, erode=0.30,
                 cavernous=0.0, bedding=0.0, joint_sets=0, bury=0.16,
                 rubble=0.10, mat_rock="rock",
                 bio_grassland=0.9, bio_temperate_forest=0.3,
                 place_abundance=0.15, place_spacing_m=40.0,
                 place_slope_max_pct=35)),
    ),
    "conglomerate-boulder": (
        "1.40 m - a block visibly made of rounded pebbles in a finer matrix",
        base(name="conglomerate-boulder",
             notes="THE ONE SPEC IN THE LIBRARY THAT USES `rock.clasts`, and it "
                   "is exactly what that parameter was written for: separate "
                   "lumps embedded in a softer matrix, so as the matrix "
                   "weathers back some clasts stand proud and others drop out "
                   "and leave sockets. That mix of bumps and holes IS the "
                   "texture and no amount of surface noise imitates it.\n\n"
                   "CLAST SIZE IS AT THE FLOOR ON PURPOSE. The parameter's own "
                   "note says that below about 15 cm a clast is one to three "
                   "voxels and reads as noise rather than as a pebble; 0.20 m "
                   "is two voxels of relief at the terrain lattice, which is "
                   "the smallest that works. Real pebble conglomerate is finer "
                   "than that and is a texture job, not geometry.",
             **r(size_m=1.4, lumps=4, spread=0.34, flatten=0.72, elongate=1.20,
                 angular=0.34, facets=3, rough=0.40, erode=0.45,
                 cavernous=0.25, bedding=0.15, bed_thickness_m=0.5,
                 clasts=260, clast_size_m=0.20, clast_hardness=3.0,
                 bury=0.24, rubble=0.30, mat_rock="gravel",
                 bio_grassland=0.7, bio_bare_rock=0.7, bio_beach=0.5,
                 bio_desert=0.4,
                 place_abundance=0.25, place_spacing_m=20.0,
                 place_slope_max_pct=70)),
    ),
    "laterite-crust-block": (
        "1.00 m - a rust-red slab with a hard pitted crust full of holes",
        base(name="laterite-crust-block",
             notes="THE TROPICAL ROCK, and rainforest had two shipped rocks. "
                   "Its identity is a HARD SKIN over softer material, riddled "
                   "with rounded holes a few centimetres across -- which is "
                   "`rock.rind` plus `rock.cavernous` together, and that pair "
                   "is what makes tafoni read as tafoni rather than as a smooth "
                   "bowl: cavernous weathering on its own retreats the rim "
                   "along with everything else, and the rind leaves the "
                   "overhanging lip.\n\n"
                   "The rust-red is `clay`, which the palette offers for badland "
                   "and mudstone and is the warmest rock colour there is. Real "
                   "laterite is more orange.",
             **r(size_m=1.0, lumps=4, spread=0.34, flatten=0.55, elongate=1.30,
                 angular=0.48, facets=4, rough=0.50, erode=0.50,
                 cavernous=0.72, rind=0.70, rind_m=0.14, bedding=0.20,
                 bed_thickness_m=0.35, bury=0.26, rubble=0.28,
                 mat_rock="clay",
                 bio_rainforest=1.0, bio_savanna=0.5, bio_bare_rock=0.3,
                 place_abundance=0.4, place_spacing_m=10.0,
                 place_slope_max_pct=60)),
    ),
    "wave-polished-boulder": (
        "1.30 m - rounder and smoother than a river cobble, no facets left",
        base(name="wave-polished-boulder",
             notes="THE SMOOTHEST STONE IN THE LIBRARY, and it is the one place "
                   "where the rock generator's founding rule is deliberately "
                   "relaxed. `rock.rough` is what decides whether a stone reads "
                   "as stone rather than as a Minecraft sphere, and it is "
                   "authored LOW here (0.26) because a wave-polished boulder "
                   "genuinely is smooth -- the concentric stair-steps that "
                   "roughness exists to prevent are the honest look of a "
                   "polished curve at this lattice.\n\n"
                   "IF IT READS AS A GENERATED SPHERE RATHER THAN AS A POLISHED "
                   "STONE, THE ANSWER IS TO RAISE THE ROUGHNESS AND ACCEPT A "
                   "LESS POLISHED ROCK. That is the trade and it is the owner's "
                   "call from a render.\n\n"
                   "A wave notch at the base gives the dark wet band a boulder "
                   "on a shore always has.",
             **r(size_m=1.3, lumps=3, spread=0.26, flatten=0.72, elongate=1.30,
                 angular=0.06, facets=0, rough=0.26, erode=0.42,
                 cavernous=0.0, notch=0.55, notch_z_m=0.14,
                 notch_spread_m=0.10, bury=0.28, rubble=0.20,
                 mat_rock="rock",
                 bio_beach=1.0, bio_ocean=0.0, bio_bare_rock=0.3,
                 place_abundance=0.7, place_spacing_m=2.5,
                 place_elev_max_m=10, place_slope_max_pct=45)),
    ),
    "rockpool-platform": (
        "2.60 m - a flat wave-cut bench with shallow pools and a seaward step",
        base(name="rockpool-platform",
             notes="A FLOOR RATHER THAN A LUMP, which is `rock.flatten` at 0.22 "
                   "-- the flattest thing in the library -- with the burial "
                   "raised so most of it sits in the ground and only a bench "
                   "shows.\n\n"
                   "THE POOLS ARE `rock.pans` AND THEY ARE FOUND RATHER THAN "
                   "PLACED: the generator runs water down the stone and hollows "
                   "out wherever there is nowhere lower to go, which is exactly "
                   "what a rock pool is. Flat floors and slightly overhung rims "
                   "are what separates a solution pan from a dent, and they are "
                   "what makes this read as a platform with pools in it rather "
                   "than as a dimpled slab.",
             **r(size_m=2.6, lumps=6, spread=0.55, flatten=0.22, elongate=1.60,
                 angular=0.50, facets=5, rough=0.36, erode=0.36,
                 cavernous=0.30, pans=0.85, pan_depth_m=0.28,
                 bedding=0.35, bed_thickness_m=0.40, bed_dip_deg=4.0,
                 notch=0.6, notch_z_m=0.10, notch_spread_m=0.12,
                 bury=0.45, rubble=0.20, mat_rock="rock",
                 bio_beach=1.0, bio_bare_rock=0.4,
                 place_abundance=0.4, place_spacing_m=8.0,
                 place_elev_max_m=6, place_slope_max_pct=25)),
    ),
    "tidal-notch": (
        "2.40 m - a cliff foot with a horizontal groove cut at ONE height",
        base(name="tidal-notch",
             notes="THE WHOLE ASSET IS ONE PARAMETER USED HARD. "
                   "`rock.notch` at 3.2 with a narrow spread saws a waist into "
                   "the stone at exactly one height, and that horizontal line "
                   "-- the same height on every rock along a shore -- is what "
                   "tells a player where the sea reaches. It is the missing "
                   "half of every undercut shape and the shipped set uses it "
                   "nowhere.\n\n"
                   "Tall and narrow so the notch has something to undercut, and "
                   "deeply buried so it reads as the foot of a face rather than "
                   "as a free-standing stone.",
             **r(size_m=2.4, lumps=4, spread=0.30, flatten=1.60, elongate=1.20,
                 angular=0.55, facets=4, rough=0.44, erode=0.30,
                 cavernous=0.20, notch=3.2, notch_z_m=0.55,
                 notch_spread_m=0.16, bedding=0.40, bed_thickness_m=0.45,
                 bury=0.38, rubble=0.30, mat_rock="rock",
                 bio_beach=1.0, bio_bare_rock=0.6,
                 place_abundance=0.25, place_spacing_m=12.0,
                 place_elev_max_m=8, place_slope_max_pct=70)),
    ),
    "rapids-whaleback": (
        "2.40 m - a smooth grey dome in the channel, undercut at low water",
        base(name="rapids-whaleback",
             notes="A RIVER'S VERSION OF THE WAVE-POLISHED BOULDER, and built "
                   "the same way for the same reason: very low roughness, no "
                   "facets, a strong horizontal attack band at the low-water "
                   "line. What differs is scale and proportion -- a whaleback "
                   "is long, low and aligned with the flow, so it is elongated "
                   "2.4 to 1 and flattened.\n\n"
                   "Placed hard against water on flat ground.",
             **r(size_m=2.4, lumps=4, spread=0.34, flatten=0.50, elongate=2.40,
                 angular=0.08, facets=0, rough=0.28, erode=0.40,
                 cavernous=0.0, notch=1.3, notch_z_m=0.25,
                 notch_spread_m=0.14, bury=0.26, rubble=0.15,
                 mat_rock="rock",
                 bio_rainforest=0.8, bio_temperate_forest=0.7,
                 bio_taiga=0.5, bio_grassland=0.4,
                 place_abundance=0.4, place_spacing_m=6.0,
                 place_water_max_m=4, place_slope_max_pct=20)),
    ),
    "karst-breakdown-block": (
        "2.00 m - a pale limestone slab with ONE clean face and three broken",
        base(name="karst-breakdown-block",
             notes="ONE FRESH FACE AND THREE OLD ONES is the story -- a slab "
                   "fallen from a collapsed cave roof -- and the generator "
                   "cannot age one face differently from another. What it CAN "
                   "do is `rock.aspect`, which weathers one side harder than "
                   "the rest, and at 0.85 that gives one clean flat side "
                   "against three rounded ones. It is the same asymmetry from "
                   "the opposite direction and it reads.\n\n"
                   "`rock.aspect` also exists because a rock weathered evenly "
                   "on all sides is the most reliable tell that it came out of "
                   "a generator, and this is the only spec in the library that "
                   "pushes it past 0.5.",
             **r(size_m=2.0, lumps=3, spread=0.28, flatten=0.55, elongate=1.50,
                 angular=0.78, facets=6, rough=0.42, erode=0.34,
                 cavernous=0.15, aspect=0.85, joint_sets=2,
                 joint_scatter=0.10, block_size_m=1.6,
                 bedding=0.30, bed_thickness_m=0.55,
                 bury=0.22, rubble=0.40, mat_rock="rock",
                 bio_rainforest=0.7, bio_bare_rock=0.8,
                 bio_temperate_forest=0.4, bio_grassland=0.3,
                 place_abundance=0.3, place_spacing_m=10.0,
                 place_slope_max_pct=70)),
    ),
    "roche-moutonnee": (
        "4.00 m - ice-scoured: one smooth striated slope, one plucked face",
        base(name="roche-moutonnee",
             notes="THE ASYMMETRY IS THE ASSET, and the tundra file says so in "
                   "those words: one gently sloping face that ice smoothed and "
                   "one abrupt face of ragged steps it plucked away. Same "
                   "mechanism as the karst block -- `rock.aspect` at 0.9 -- but "
                   "used at four metres on a long low dome, so what comes out "
                   "is a whaleback with a broken end rather than a slab with a "
                   "clean side.\n\n"
                   "The glacial striations are millimetre grooves and are not "
                   "attempted at 10 cm; the form carries it without them.",
             **r(size_m=4.0, lumps=5, spread=0.40, flatten=0.44, elongate=2.10,
                 angular=0.45, facets=4, rough=0.34, erode=0.42,
                 cavernous=0.10, aspect=0.90, joint_sets=2,
                 joint_scatter=0.14, block_size_m=1.8, block_relief_m=0.10,
                 bury=0.30, rubble=0.35, mat_rock="rock",
                 bio_tundra_alpine=1.0, bio_bare_rock=0.7, bio_taiga=0.5,
                 place_abundance=0.15, place_spacing_m=45.0,
                 place_slope_max_pct=60)),
    ),
    "moraine-boulder-ridge": (
        "2.60 m - unsorted material of every size, heaped along a crest",
        base(name="moraine-boulder-ridge",
             notes="IT LOOKS DUMPED RATHER THAN FALLEN OR ERODED, which is the "
                   "only description in the tundra file that is about PROCESS "
                   "rather than shape -- and the way to say it here is a very "
                   "high lump spread with heavy rubble and a strong clast "
                   "field, so the stone is an unsorted heap rather than one "
                   "mass. `rock.spread` 0.85 is the highest in the library and "
                   "it is the parameter whose own help text says that high "
                   "values make a broken pile rather than a single stone.\n\n"
                   "It is still ONE connected piece, which `tools/buildcheck.py` "
                   "enforces -- a moraine is a distribution and a distribution "
                   "is a placement feature. This is the biggest single lump of "
                   "one.",
             **r(size_m=2.6, lumps=9, spread=0.85, flatten=0.62, elongate=1.90,
                 angular=0.52, facets=5, rough=0.55, erode=0.30,
                 cavernous=0.0, clasts=180, clast_size_m=0.34,
                 clast_hardness=2.2, bury=0.34, rubble=0.85,
                 mat_rock="gravel",
                 bio_tundra_alpine=1.0, bio_taiga=0.6, bio_bare_rock=0.5,
                 place_abundance=0.35, place_spacing_m=14.0,
                 place_slope_max_pct=60)),
    ),
    "granite-kopje": (
        "4.50 m - a rounded boulder pile standing out of flat grass",
        base(name="granite-kopje",
             notes="ONE OF THE TWO MOST RECOGNISABLE OBJECTS ON A SAVANNA PLAIN "
                   "THAT ARE NOT A TREE, and the savanna file names it as a "
                   "landform rather than a species for exactly that reason. It "
                   "is the shipped `corestone-tor` at four metres with the "
                   "corestone rotting pushed hard: `rock.corestone` 0.72 rots "
                   "the rock inward FROM the joints so each block loses its "
                   "corners and a rounded core survives, which is how a granite "
                   "tor becomes a stack of boulders with the old fracture grid "
                   "still legible in how they sit.\n\n"
                   "`rock.settle_m` lets those blocks drop into the space "
                   "weathering took out. Without it they stay in perfect "
                   "alignment and the stone reads as one mass with grooves cut "
                   "into it.",
             **r(size_m=4.5, lumps=7, spread=0.52, flatten=0.85, elongate=1.25,
                 angular=0.42, facets=4, rough=0.52, erode=0.40,
                 cavernous=0.0, joint_sets=3, joint_scatter=0.14,
                 joint_dip_deg=0.0, block_size_m=1.9, block_relief_m=0.16,
                 corestone=0.72, settle_m=0.20,
                 bury=0.26, rubble=0.40, mat_rock="rock",
                 bio_savanna=1.0, bio_grassland=0.7, bio_bare_rock=0.6,
                 place_abundance=0.1, place_spacing_m=90.0,
                 place_slope_max_pct=60)),
    ),
    "termite-mound": (
        "2.80 m - a fluted spire with buttresses at its base",
        base(name="termite-mound",
             notes="NOT A ROCK AND FILED AS ONE, AND THE GRASSLAND FILE SAYS "
                   "SO EXPLICITLY: a termite mound is a biogenic structure and "
                   "would be a separate asset if wanted. It is here because the "
                   "savanna file names it alongside the kopje as one of the two "
                   "objects a player navigates a plain by, and because the rock "
                   "generator makes exactly its shape -- a single tall fluted "
                   "spire flaring at the foot.\n\n"
                   "THE FLUTES ARE `rock.flutes`, WHICH IS THE ONLY WEATHERING "
                   "IN THE GENERATOR THAT KNOWS WHICH WAY IS DOWN. It runs "
                   "water down the outside and cuts in proportion to how much "
                   "passes, so grooves deepen and collect more -- near-vertical "
                   "runnels that merge downhill, which is what a mound's "
                   "surface is. The curvature pass attacks a shape the same "
                   "from every direction and could never make a directional "
                   "mark.\n\n"
                   "`clay` for the material, which is the right substance and "
                   "the right colour for once.",
             **r(size_m=2.8, lumps=3, spread=0.20, flatten=2.60, elongate=1.10,
                 angular=0.20, facets=2, rough=0.40, erode=0.34,
                 cavernous=0.0, flutes=0.85, flute_width_m=0.30,
                 bury=0.20, rubble=0.12, mat_rock="clay",
                 bio_savanna=1.0, bio_grassland=0.3, bio_rainforest=0.25,
                 place_abundance=0.2, place_spacing_m=30.0,
                 place_slope_max_pct=30)),
    ),
    "leaf-littered-slab": (
        "1.90 m - a flat bedrock plate half-buried, only its high edge showing",
        base(name="leaf-littered-slab",
             notes="THE ONLY THING THIS ASSET DOES IS SIT LOW, and that is "
                   "worth a spec: a temperate forest floor is not made of "
                   "boulders, it is made of bedrock plates mostly under the "
                   "litter with one edge up. `rock.bury` at 0.62 is near the "
                   "ceiling -- almost two thirds of the stone is below ground "
                   "-- on a very flat elongated slab.\n\n"
                   "Deliberately plain: no joints, no clasts, low erosion. It "
                   "is a shape that does not want to be looked at, which is "
                   "most of a forest floor.",
             **r(size_m=1.9, lumps=4, spread=0.42, flatten=0.24, elongate=1.70,
                 angular=0.42, facets=4, rough=0.42, erode=0.28,
                 cavernous=0.0, bedding=0.25, bed_thickness_m=0.45,
                 bed_dip_deg=5.0, bury=0.62, rubble=0.18, mat_rock="rock",
                 bio_temperate_forest=1.0, bio_taiga=0.6,
                 bio_grassland=0.3, bio_rainforest=0.3,
                 place_abundance=0.5, place_spacing_m=7.0,
                 place_slope_max_pct=40)),
    ),
    "streambed-cascade-block": (
        "1.20 m - angular blocks wedged in a channel, tops water-polished",
        base(name="streambed-cascade-block",
             notes="ANGULAR EVERYWHERE EXCEPT ON TOP, which is the same "
                   "one-sided weathering the karst block and the roche "
                   "moutonnee use, at a third of the size and in the opposite "
                   "sense: `rock.aspect` 0.6 with high angularity, so the "
                   "faces stay sharp and one surface is worn smooth. That is a "
                   "block that fell into a stream recently enough to still be "
                   "sharp and has been stood on by water ever since.\n\n"
                   "Tight to water and on steeper ground than any other rock "
                   "here, because a cascade is a slope.",
             **r(size_m=1.2, lumps=4, spread=0.34, flatten=0.70, elongate=1.35,
                 angular=0.80, facets=6, rough=0.46, erode=0.30,
                 cavernous=0.0, aspect=0.60, joint_sets=2,
                 joint_scatter=0.12, block_size_m=1.0,
                 bury=0.24, rubble=0.45, mat_rock="rock",
                 bio_temperate_forest=0.9, bio_taiga=0.7,
                 bio_tundra_alpine=0.5, bio_rainforest=0.5,
                 bio_bare_rock=0.4,
                 place_abundance=0.6, place_spacing_m=3.0,
                 place_water_max_m=3, place_slope_max_pct=70)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "landform specs")
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
