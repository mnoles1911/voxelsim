"""Author the river bed and the lake bottom: the stone fresh water shapes.

Ten specs. The enumeration is in `docs/aquatic-species.md`; this file is the
build. Six freshwater rocks already ship -- `river-cobble`, `gravel-bar`,
`quartz-vein-cobble-bar`, `rapids-whaleback`, `streambed-cascade-block` and
`tufa-curtain` -- and this adds what a fall, a plunge pool, a still lake bottom
and a hard-water spring are made of.

TEN CENTIMETRES, AND NOTHING ELSE (`forge/kinds.py:29-59`,
`forge.cli.selftest`). A rock joins the world's own voxel grid and is
destructible as terrain is.

THERE IS NO FRESHWATER BIOME. Every weight below is on a LAND biome, because
that is what the engine classifies a river's column as (`forge/biomes.py:37-42`)
and it is what the shipped freshwater fish and the six shipped freshwater rocks
already do. `placement.water_max_m` is what makes these river rocks rather than
field rocks, and every spec here sets it.

TWO THINGS THE GENERATOR WILL NOT DO, AND THEY COST THREE OF THE TEN:

  * IT ONLY CUTS. Travertine and tufa are limestone coming OUT of spring water
    and accreting where it splashes -- growth forms, built up rather than eroded
    down. `tufa-curtain` already ships recording exactly this about itself and
    the two new precipitate rocks here inherit it. What stands in is
    `rock.bedding` at a thin bed thickness, which gives the right BANDED LOOK by
    the wrong mechanism, and both specs say so.
  * THERE IS NO THROUGH-BORE. `rock.pans` hollows a TOP surface where water has
    nowhere lower to go; it does not drill. A bedrock pothole is a smooth
    cylindrical shaft and ships as a deep dished basin. `rock.arch` is the only
    through-cut in the generator and it is a horizontal span.
    `docs/aquatic-species.md` §8.4 books both of these against one subtraction
    feature, which would also fix the barrel sponge and the black smoker.

`rock.size_m` IS MEASURED, NOT ESTIMATED -- the builder measures the result and
corrects on the same seed, so the number in each spec is what comes out.

ONE CONNECTED PIECE, enforced by `tools/buildcheck.py` at 26-connectivity. A
cobble BED, a boulder field and a rimstone TERRACE are all plural; what is
authored is one block, one dam lip, one bench, and placement makes the rest.

    python tools/seed_freshwater_rocks.py
    python tools/seed_freshwater_rocks.py --force

SIZES ARE APPROXIMATE, per `docs/biomes/README.md` §8.
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

    "plunge-pool-boulder": (
        "2.20 m - a huge rounded block half drowned in the basin below a fall",
        base(name="plunge-pool-boulder",
             notes="THE BIGGEST STONE A RIVER MOVES, and it only moves it once. "
                   "A plunge pool collects the blocks a waterfall has undercut "
                   "and dropped, and they sit in the basin being polished by "
                   "everything the river carries past them for centuries.\n\n"
                   "SO IT IS THE `rapids-whaleback` BUILD AT TWICE THE SIZE AND "
                   "IN THREE DIMENSIONS. That one is long, low and aligned with "
                   "the flow; a plunge-pool block is equidimensional, because "
                   "it tumbled rather than sat. `elongate` 1.25 against the "
                   "whaleback's 2.40 is the whole difference, and both share the "
                   "very low roughness and zero facets that say water-polished.\n\n"
                   "`bury` 0.45 is high: half of it is under water and gravel, "
                   "which is what 'half drowned' means and what stops it "
                   "reading as a boulder that happens to be wet.\n\n"
                   "Steep ground and hard against water. Placed rarely and far "
                   "apart -- there is one of these per fall, not a field of "
                   "them.",
             **r(size_m=2.2, lumps=4, spread=0.30, flatten=0.85, elongate=1.25,
                 angular=0.06, facets=0, rough=0.28, erode=0.40,
                 cavernous=0.0, notch=0.9, notch_z_m=0.30,
                 notch_spread_m=0.16, bury=0.45, rubble=0.20,
                 mat_rock="rock",
                 bio_temperate_forest=1.0, bio_taiga=0.8,
                 bio_tundra_alpine=0.6, bio_rainforest=0.6,
                 place_abundance=0.25, place_spacing_m=18.0,
                 place_water_max_m=3, place_slope_max_pct=60)),
    ),
    "waterfall-lip-ledge": (
        "3.00 m - a hard capping bed with a clean straight edge, undercut below",
        base(name="waterfall-lip-ledge",
             notes="A WATERFALL IS A HARDNESS CONTRAST AND NOTHING ELSE. It "
                   "exists where a hard bed lies over a soft one: the soft bed "
                   "retreats, the hard bed is left standing as a lip, and the "
                   "fall migrates upstream. That is `rock.bedding` doing "
                   "precisely what it was written for, and this is the spec in "
                   "the library that uses it most literally.\n\n"
                   "`bedding` 0.75 at a 0.55 m bed thickness with `caprock` 0.70 "
                   "gives one thick hard layer on top of thinner soft ones. The "
                   "caprock parameter is what keeps the TOP surviving while "
                   "everything under it retreats, and without it a heavily "
                   "bedded rock just gets stripey.\n\n"
                   "THE UNDERCUT IS `rock.notch` AT 2.0 AND IT IS THE ASSET. A "
                   "lip with no overhang is a step. The notch is set low "
                   "(`notch_z_m` 0.40) and narrow, so the void is at the foot "
                   "and the hard bed juts over it.\n\n"
                   "Very flat and very long -- `flatten` 0.32, `elongate` 2.4 -- "
                   "because a fall's lip runs across the channel rather than "
                   "along it.",
             **r(size_m=3.0, lumps=6, spread=0.42, flatten=0.32, elongate=2.40,
                 angular=0.58, facets=5, rough=0.44, erode=0.26,
                 cavernous=0.15, bedding=0.75, bed_thickness_m=0.55,
                 bed_dip_deg=3.0, caprock=0.70, cap_frac=0.70,
                 notch=2.0, notch_z_m=0.40, notch_spread_m=0.16,
                 bury=0.34, rubble=0.30, mat_rock="rock",
                 bio_temperate_forest=1.0, bio_taiga=0.8,
                 bio_tundra_alpine=0.7, bio_rainforest=0.7,
                 place_abundance=0.2, place_spacing_m=40.0,
                 place_water_max_m=2, place_slope_max_pct=70)),
    ),
    "bedrock-pothole": (
        "1.60 m - a block with a deep smooth bowl ground into it",
        base(name="bedrock-pothole",
             notes="A POTHOLE IS A HOLE AND THE GENERATOR CANNOT MAKE ONE. A "
                   "grinding stone caught in an eddy drills a smooth cylindrical "
                   "shaft down into bedrock -- often deeper than it is wide, "
                   "sometimes right through. `rock.pans` is the nearest thing: "
                   "it runs water over the stone and hollows out wherever there "
                   "is nowhere lower to go, which produces a DISH. So what ships "
                   "is a deep smooth basin rather than a shaft, and that reads "
                   "from above and is wrong from the side.\n\n"
                   "`pans` 0.95 and `pan_depth_m` 0.45 are both near the "
                   "hardest settings available, which is the honest way to say "
                   "'this parameter is being asked for more than it has'. "
                   "`docs/aquatic-species.md` §8.4 books it with the barrel "
                   "sponge and the black smoker: three species, one missing "
                   "subtraction.\n\n"
                   "SMOOTH IS THE OTHER HALF OF THE SPECIES. A pothole's inner "
                   "wall is polished glass-smooth by the stone that cut it, so "
                   "`rough` is 0.26 -- among the lowest in the library, the same "
                   "relaxation `wave-polished-boulder` and `rapids-whaleback` "
                   "make and for the same physical reason.\n\n"
                   "Flat-topped and deeply set in the channel floor.",
             **r(size_m=1.6, lumps=4, spread=0.34, flatten=0.62, elongate=1.25,
                 angular=0.30, facets=2, rough=0.26, erode=0.34,
                 cavernous=0.16, pans=0.62, pan_depth_m=0.26,
                 bury=0.30, rubble=0.12, mat_rock="rock",
                 bio_temperate_forest=1.0, bio_taiga=0.7,
                 bio_tundra_alpine=0.6, bio_rainforest=0.5,
                 place_abundance=0.3, place_spacing_m=12.0,
                 place_water_max_m=2, place_slope_max_pct=45)),
    ),
    "riffle-slab": (
        "1.20 m - a low flat plate breaking the surface in fast shallow water",
        base(name="riffle-slab",
             notes="THE STONE THAT MAKES THE NOISE. A riffle is the shallow "
                   "fast reach between two pools, and what makes it a riffle "
                   "rather than a glide is a floor of flat plates just breaking "
                   "the surface. A player hears this reach before seeing it.\n\n"
                   "DELIBERATELY PLAIN, and that is the whole design -- the same "
                   "argument `leaf-littered-slab` makes for a forest floor. No "
                   "joints, no clasts, no bedding, no cavernous weathering: a "
                   "flat plate lying almost level with its top worn smooth and "
                   "one edge slightly up. It is a shape that does not want to be "
                   "looked at, which is most of a river bed.\n\n"
                   "`aspect` 0.55 weathers ONE side harder than the rest, which "
                   "on a plate lying in fast water gives a smooth polished top "
                   "over rougher sides. `streambed-cascade-block` uses the same "
                   "parameter at 0.60 for the same reason at a steeper "
                   "gradient.\n\n"
                   "Tight to water at high abundance and short spacing, because "
                   "a riffle is many of these and one is meaningless.",
             **r(size_m=1.6, lumps=5, spread=0.42, flatten=0.46, elongate=1.65,
                 angular=0.44, facets=4, rough=0.38, erode=0.30,
                 cavernous=0.0, aspect=0.55, bury=0.34, rubble=0.20,
                 mat_rock="rock",
                 bio_temperate_forest=1.0, bio_grassland=0.8, bio_taiga=0.8,
                 bio_tundra_alpine=0.5, bio_rainforest=0.5,
                 place_abundance=0.8, place_spacing_m=2.0,
                 place_water_max_m=2, place_slope_max_pct=25)),
    ),
    "step-pool-boulder": (
        "1.50 m - a wedged angular block forming one step of a mountain stream",
        base(name="step-pool-boulder",
             notes="A STEEP MOUNTAIN STREAM IS A STAIRCASE, and each step is one "
                   "or two big blocks jammed across the channel with a small "
                   "pool scoured out behind. That step-pool sequence is the "
                   "commonest channel form above about a 4% gradient and the "
                   "library had nothing for it.\n\n"
                   "WEDGED, NOT ROUNDED, WHICH IS WHAT SEPARATES IT FROM EVERY "
                   "OTHER RIVER ROCK HERE. The plunge-pool boulder and the "
                   "whaleback are polished because water has worked them for "
                   "centuries; a step-pool block arrived last winter off a "
                   "hillside and is still sharp. `angular` 0.76 with six facets "
                   "and `erode` 0.18, against the plunge-pool block's 0.06 and "
                   "0.40.\n\n"
                   "`joint_sets` 3 so every face shares one fracture frame, "
                   "which is what makes stone read as quarried out of a "
                   "hillside rather than merely lumpy.\n\n"
                   "The steepest placement in either aquatic file at 70% -- the "
                   "engine's own cliff gate, above which ground classifies as "
                   "bare rock -- because that is where step-pool streams are, "
                   "and it carries a bare-rock weight for the same reason.",
             **r(size_m=1.5, lumps=4, spread=0.32, flatten=0.78, elongate=1.30,
                 angular=0.76, facets=6, rough=0.50, erode=0.18,
                 cavernous=0.0, joint_sets=3, joint_scatter=0.12,
                 block_size_m=1.2, bury=0.28, rubble=0.40,
                 mat_rock="rock",
                 bio_tundra_alpine=1.0, bio_taiga=0.8, bio_bare_rock=0.6,
                 bio_temperate_forest=0.6,
                 place_abundance=0.6, place_spacing_m=4.0,
                 place_water_max_m=2, place_slope_max_pct=70)),
    ),
    "lake-bed-slab": (
        "2.00 m - a broad flat plate lying level under still water, silt-edged",
        base(name="lake-bed-slab",
             notes="STILL WATER LEAVES A ROCK ALONE, and that absence is the "
                   "asset. Everything else in this file has been shaped by "
                   "moving water -- polished, undercut, wedged, drilled. A lake "
                   "bed has no current at all, so what is there is whatever the "
                   "ice or the cliff left, gradually silting up.\n\n"
                   "SO IT IS THE LEAST WEATHERED ROCK IN THE FILE and the most "
                   "deeply buried: `erode` 0.20 and `bury` 0.58, which is near "
                   "the 0.70 ceiling -- three fifths of the stone is under silt "
                   "and only a broad flat top shows. `leaf-littered-slab` uses "
                   "the same trick at 0.62 for a forest floor and the two are "
                   "the same idea in two places.\n\n"
                   "`bed_dip_deg` 2 keeps it very nearly level, because a lake "
                   "bed is the flattest surface in the natural world and a "
                   "tilted slab reads as a hillside.\n\n"
                   "Placed at high abundance across four biomes on almost flat "
                   "ground: it is the default floor of any still water, and its "
                   "job is to stop a lake bottom being bare mud.",
             **r(size_m=2.0, lumps=5, spread=0.50, flatten=0.26, elongate=1.50,
                 angular=0.40, facets=4, rough=0.36, erode=0.20,
                 cavernous=0.0, bedding=0.25, bed_thickness_m=0.50,
                 bed_dip_deg=2.0, bury=0.58, rubble=0.15,
                 mat_rock="rock",
                 bio_temperate_forest=1.0, bio_grassland=0.9, bio_taiga=0.9,
                 bio_tundra_alpine=0.7,
                 place_abundance=0.7, place_spacing_m=5.0,
                 place_water_max_m=2, place_slope_max_pct=12)),
    ),
    "undercut-bank-block": (
        "2.00 m - a bank block with a horizontal slot cut beneath it",
        base(name="undercut-bank-block",
             notes="THE BEST COVER A RIVER HAS. An undercut bank is where every "
                   "large trout in a stream lives -- the water has cut a "
                   "horizontal slot under the bank at the waterline and left a "
                   "roofed shelter -- and the library ships eleven freshwater "
                   "fish with nothing to hide under.\n\n"
                   "`rock.notch` 2.6 AT ONE HEIGHT IS THE WHOLE ASSET, exactly "
                   "as it is for `tidal-notch` on a shore. The difference is "
                   "which height: a tidal notch is at THE SAME height on every "
                   "rock along a coast, because the sea has one level, and a "
                   "river's undercut is at whatever the summer waterline is on "
                   "that reach. Nothing in `placement` can express either, and "
                   "the two will look identical unless they are placed "
                   "differently.\n\n"
                   "Tall and deeply buried so it reads as part of a bank rather "
                   "than as a free-standing stone: `flatten` 1.30 with `bury` "
                   "0.42. `bedding` 0.45 at a thin bed, because a bank is "
                   "usually the layered stuff a river cuts through most easily.",
             **r(size_m=2.0, lumps=4, spread=0.32, flatten=1.30, elongate=1.55,
                 angular=0.50, facets=4, rough=0.44, erode=0.30,
                 cavernous=0.20, notch=2.6, notch_z_m=0.45,
                 notch_spread_m=0.14, bedding=0.45, bed_thickness_m=0.35,
                 bury=0.42, rubble=0.28, mat_rock="rock",
                 bio_temperate_forest=1.0, bio_grassland=0.9,
                 bio_rainforest=0.6, bio_taiga=0.5,
                 place_abundance=0.5, place_spacing_m=8.0,
                 place_water_max_m=2, place_slope_max_pct=45)),
    ),
    "travertine-rimstone-dam": (
        "2.40 m - a curved banded lip holding back a shallow terrace pool",
        base(name="travertine-rimstone-dam",
             notes="THE SECOND ACCRETIONARY ROCK IN THE LIBRARY AND IT INHERITS "
                   "THE FIRST ONE'S PROBLEM. `tufa-curtain` already records it: "
                   "this generator only CUTS, and travertine is limestone coming "
                   "out of solution and building UP -- a rimstone dam grows "
                   "highest where the water runs fastest over its lip, which is "
                   "a positive feedback with no equivalent anywhere in "
                   "`forge/rock.py`.\n\n"
                   "WHAT STANDS IN IS `rock.bedding` AT A VERY THIN BED, and it "
                   "is the right LOOK by the wrong MECHANISM. Bedding lays "
                   "alternating hard and soft layers and weathers the soft ones "
                   "back; travertine's banding is deposition layers that were "
                   "never cut at all. Set to 0.70 at a 0.16 m bed, which is "
                   "banding about one and a half voxels apart -- the finest the "
                   "terrain lattice carries. Real travertine laminae are "
                   "millimetres.\n\n"
                   "THE CURVED LIP IS THE SPECIES AND IT IS ONLY HALF THERE. A "
                   "rimstone dam is a convex arc bulging downstream with a pool "
                   "behind it; `elongate` 2.6 with a high lump spread gives a "
                   "long low ridge, and nothing here bends a rock in plan. "
                   "`pans` 0.60 hollows the upstream side, which is the pool.\n\n"
                   "`pans` IS AT 0.15 AND IT WAS AT 0.60, AND THE REASON IS "
                   "MEASURED VARIANCE RATHER THAN LOOKS. Bedding and pans "
                   "interact badly in the size-correction search on an "
                   "elongated rock: at `bedding` 0.45 with `pans` 0.35 this spec "
                   "built 373 / 655 / 6,824 / 473 / 791 / 476 voxels over six "
                   "seeds -- an 18.3x swing, one seed in six an order of "
                   "magnitude off the rest. Swept:\n\n"
                   "    bedding 0.45  pans 0.35   18.3x\n"
                   "    bedding 0.30  pans 0.35    3.1x\n"
                   "    bedding 0.30  pans 0.00    1.6x\n"
                   "    bedding 0.45  pans 0.20    5.5x\n"
                   "    bedding 0.45  pans 0.15    2.0x   <- authored\n\n"
                   "The banding is the species, so bedding was kept and the "
                   "pool was given up: 2.0x over eight seeds, against a 4.2x "
                   "measured on the shipped `banded-sandstone-ledge` at "
                   "`bedding` 0.90. So this is now INSIDE the library's existing "
                   "variance for a heavily bedded rock. The upstream pool is a "
                   "shallow dish rather than a basin.\n\n"
                   "`sand` (214,192,140) for the pale cream travertine is, for "
                   "once, close.",
             **r(size_m=2.4, lumps=7, spread=0.50, flatten=0.52, elongate=2.10,
                 angular=0.16, facets=1, rough=0.42, erode=0.20,
                 cavernous=0.14, bedding=0.45, bed_thickness_m=0.22,
                 bed_dip_deg=6.0, pans=0.15, pan_depth_m=0.24,
                 bury=0.30, rubble=0.18, mat_rock="sand",
                 bio_grassland=1.0, bio_temperate_forest=0.8,
                 bio_savanna=0.7, bio_rainforest=0.4,
                 place_abundance=0.3, place_spacing_m=8.0,
                 place_water_max_m=2, place_slope_max_pct=40)),
    ),
    "tufa-spring-mound": (
        "1.80 m - a lumpy pale mound around a hard-water spring, full of holes",
        base(name="tufa-spring-mound",
             notes="THE THIRD ACCRETIONARY ROCK, and the one where the "
                   "generator's inability to build up matters LEAST -- because a "
                   "tufa mound's surface really is mostly HOLES. It forms by "
                   "precipitating around moss and leaves and twigs, which then "
                   "rot away and leave their own moulds, so the rock is a porous "
                   "sponge of a stone. `rock.cavernous` at 0.75 pits it hard, "
                   "and cavernous weathering runs away -- once a pit exists it "
                   "traps water and deepens faster -- which is a genuinely "
                   "similar feedback to the real one even though it is "
                   "subtractive and the real one is not.\n\n"
                   "That is a better fit than the travertine dam beside it and "
                   "it is worth saying which of the two is the honest one.\n\n"
                   "LUMPY RATHER THAN LAYERED: nine lumps at a 0.56 spread with "
                   "no bedding at all, because a spring mound accretes in "
                   "irregular knobs around wherever the water happens to run, "
                   "not in beds. That is the opposite build from the dam.\n\n"
                   "`sand` for the pale cream, and `rind` 0.50 for the hard "
                   "crust over a softer interior that tufa always has.",
             **r(size_m=1.8, lumps=9, spread=0.56, flatten=1.05, elongate=1.20,
                 angular=0.10, facets=0, rough=0.60, erode=0.34,
                 cavernous=0.75, rind=0.50, rind_m=0.14,
                 bury=0.26, rubble=0.25, mat_rock="sand",
                 bio_grassland=1.0, bio_temperate_forest=0.8,
                 bio_savanna=0.6,
                 place_abundance=0.25, place_spacing_m=20.0,
                 place_water_max_m=3, place_slope_max_pct=45)),
    ),
    "marl-bench": (
        "1.80 m - a soft pale chalky shelf of lake carbonate, crumbling at the edge",
        base(name="marl-bench",
             notes="WHAT A HARD-WATER LAKE MAKES OF ITSELF. Marl is lime "
                   "precipitated out of the water column and settled as a soft "
                   "white ooze that hardens into a shelf; a marl lake has a "
                   "startling pale blue-green colour because of it, and the "
                   "benches around its edge are almost white.\n\n"
                   "THE SOFTEST ROCK IN THE LIBRARY, AND `erode` SAYS SO AT "
                   "0.60. Everything else here is authored to survive; this one "
                   "is authored to be crumbling, with `rubble` 0.55 and "
                   "`angular` 0.22, so the edges break down rather than holding "
                   "a face. That is a real distinction -- a marl bench you "
                   "stand on gives way, which is not true of any other stone "
                   "here.\n\n"
                   "`sand` (214,192,140) is the palest of the five rock "
                   "materials and marl is closer to white; `chalk-outcrop` "
                   "already records that there is no white in the menu and that "
                   "a chalk material is a one-row ask. This is the second spec "
                   "asking for it.\n\n"
                   "Flat and shelf-like, at the margin of still water on almost "
                   "level ground.",
             **r(size_m=1.8, lumps=6, spread=0.52, flatten=0.34, elongate=1.60,
                 angular=0.22, facets=3, rough=0.48, erode=0.60,
                 cavernous=0.25, bedding=0.40, bed_thickness_m=0.30,
                 bed_dip_deg=2.0, bury=0.40, rubble=0.55,
                 mat_rock="sand",
                 bio_grassland=1.0, bio_temperate_forest=0.8,
                 place_abundance=0.4, place_spacing_m=9.0,
                 place_water_max_m=3, place_slope_max_pct=20)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "freshwater rock specs")
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
