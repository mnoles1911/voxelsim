"""Author twelve more rocks: the coastal and forest-floor landforms.

WHAT THIS ADDS AND WHY IT IS NOT MORE BOULDERS. `tools/seed_landforms.py` says
the rock set is the best-served kind in the library and that what is missing is
not another lump. That is still true, so every spec here is a rock the existing
generator can already SAY and the existing thirty-odd cannot:

  * SIX FOR THE BEACH, which wraps every shore in the world and had three
    shipped rocks. The two that matter most are the ones with a hole in them --
    the sea arch and the blowhole -- because nothing in the library is pierced
    except the two heroes, and a coastline is where a player expects it.
  * FIVE FOR THE FOREST FLOOR AND ONE FOR A RIVER BAR. These are ordinary stone
    distinguished by WHERE THEY SIT and what water has done to them: a bench
    with a moss top, a spring deposit, the lip of a collapse, a fire-spalled
    block, a cobble bar.

TEN CENTIMETRES, AND NOTHING ELSE. A rock joins the world's own voxel grid and
is destructible as terrain is (`forge/kinds.py:29-58`), the world grid has one
cell size, and `forge.cli.selftest` refuses a terrain-lattice spec at any other
value. Note that `tools/seed_rocktypes.py` writes `resolution_cm: "5"`; that is
older than the rule and is not a precedent to copy.

THE ARCH HAS A HARD FLOOR AND IT SETS THE SEA ARCH'S SIZE. `forge/rock.py:368`
only runs the arch cut when `rock.size_m` is at least 4.0, and `_arch` itself
refuses an opening under two voxels across. The beach file lists the sea arch at
a 1.2 m block size; a 1.2 m arch is not buildable and would not be credible if
it were, so `sea-arch` is authored at 6 m -- which is the smallest stone that
can carry a hole and still read as a promontory rather than as a boulder with a
tunnel in it.

WHAT IS NOT HERE, AND THE EXACT REASON.

  * THE ROOT-SPLIT BLOCK IS NOT AUTHORED. Its own row in
    `docs/biomes/03-temperate-forest.md` carries a ⚠ that says the asset needs
    the rock generator and the tree generator in ONE grid, that nothing in
    `forge/pipeline.py` composes two generators, and -- in those words -- to
    "treat it as a question for the kind-list owner, not as authoring work".
    A rock with a joint opened wide and no root in it is not the asset; the
    root is the story. Left visible.
  * THE BLOCKFIELD, TALUS CONE, ROCK GLACIER, PATTERNED GROUND, ERRATIC TRAIN
    AND SHINGLE BANK are still out for the reason `seed_landforms.py` records:
    they are DISTRIBUTIONS of blocks over ground rather than one block, and one
    placement feature serves all six. `boulder-beach` below is the closest
    thing here to that line and it stays on the right side of it, for a reason
    written into its own notes.

`rock.size_m` IS MEASURED, NOT ESTIMATED. Every step of the build takes mass
away, so the raw lumps start larger and the builder measures the result and
corrects on the same seed. The number in each spec is what comes out. The
BLOCK SIZES in the biome tables are not that -- they are unsourced approximate
figures like every other size in those files -- and where a spec departs from
one its notes say so.

    python tools/seed_landforms2.py
    python tools/seed_landforms2.py --force
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
    # --- beach: 5.54% of land and it wraps every shore ------------------------
    "sea-arch": (
        "6.00 m - a coastal promontory pierced through at the waterline",
        base(name="sea-arch",
             notes="THE ONLY PIERCED ROCK IN THE LIBRARY THAT IS NOT A HERO. "
                   "`hero-natural-arch` is 40 m and a landmark; this is the "
                   "same mechanism at the size a shore actually has them, so a "
                   "coast can carry several rather than one.\n\n"
                   "SIX METRES IS A FLOOR, NOT A CHOICE. `forge/rock.py:368` "
                   "only runs the arch cut at `rock.size_m` 4.0 or more, and "
                   "`_arch` then refuses an opening under two voxels across, so "
                   "the beach file's 1.2 m block size cannot be built and would "
                   "not be credible if it could. Six metres is where the "
                   "opening is wide enough to see daylight through and the legs "
                   "are still thin enough to read as legs.\n\n"
                   "WHAT MAKES IT COASTAL RATHER THAN DESERT is the pair of "
                   "attacks at the bottom: `rock.notch` cuts a wave band at "
                   "0.6 m on both legs, which is what actually undercuts and "
                   "eventually fells a sea arch, and the desert version has "
                   "nothing of the sort. Grey `rock` rather than the arch "
                   "specs' sandstone, for the same reason.\n\n"
                   "Bedded and flattish-topped: an arch forms in a headland "
                   "with beds in it, and the bedding is what gives the span its "
                   "horizontal grain.\n\n"
                   "`tools/archprobe.py` REPORTS ZERO ENCLOSED DAYLIGHT ON THIS "
                   "SPEC AND THE ARCH IS STILL THERE. Read that before "
                   "'fixing' it. The probe counts holes in the silhouette that "
                   "are surrounded by stone, and this opening reaches the "
                   "GROUND -- a walk-through gap rather than a window -- so it "
                   "touches the bottom edge and is not enclosed. The cut itself "
                   "reports an opening at 40% of the face with both legs "
                   "standing and the stone in one piece, and measuring the air "
                   "under the span directly gives 620-740 voxels against "
                   "840-1040 of stone on seeds 1-3. `hero-natural-arch` scores "
                   "enclosed daylight because its opening stops short of the "
                   "ground; at 6 m an opening that did the same would be a "
                   "letterbox.",
             **r(size_m=6.0, lumps=3, spread=0.20, flatten=1.45, elongate=2.60,
                 angular=0.40, facets=5, rough=0.32, erode=0.45,
                 cavernous=0.10, arch=0.62,
                 bedding=0.40, bed_thickness_m=0.70, bed_dip_deg=4.0,
                 notch=1.40, notch_z_m=0.60, notch_spread_m=0.30,
                 bury=0.24, rubble=0.40, mat_rock="rock",
                 bio_beach=1.0, bio_bare_rock=0.5,
                 place_abundance=0.08, place_spacing_m=70.0,
                 place_elev_max_m=18, place_slope_max_pct=70)),
    ),
    "chalk-cliff-foot": (
        "2.80 m - a blinding white bedded face with a rubble apron at its base",
        base(name="chalk-cliff-foot",
             notes="THE SECOND WHITE ROCK, AND IT IS A FACE WHERE "
                   "`chalk-outcrop` IS A LUMP. That one is a low downland "
                   "outcrop sitting in grass; this is the FOOT OF A CLIFF -- "
                   "tall, flat-fronted, deeply buried so it reads as the bottom "
                   "of something much larger, with the horizontal flint bands "
                   "running across it. The two share a lithology and a material "
                   "and nothing else about how they sit.\n\n"
                   "`sand` FOR THE SAME REASON `chalk-outcrop` GIVES: there is "
                   "no white in `materials.rock`'s five choices, and sand is the "
                   "palest and carries sandstone's patch mottle, which reads as "
                   "chalk better than a flat grey. It is a cream and not a "
                   "white, and a chalk material is still a one-row ask.\n\n"
                   "THE SCREE OF WHITE RUBBLE AT ITS BASE IS NOT DRAWN, AND "
                   "THAT IS NOT A TUNING MISS. `rock.rubble` is authored high "
                   "here, but `forge/rock.py:50` turns the rubble ring OFF for "
                   "asset builds on purpose -- one generation makes one entity, "
                   "and a ring of loose stones is a second asset the library "
                   "cannot address or place. The number is carried so it is "
                   "right the day a rubble asset exists.\n\n"
                   "Thin beds and heavy bedding for the flint bands; high "
                   "angularity because chalk fails in blocks rather than "
                   "rounding.",
             **r(size_m=2.8, lumps=5, spread=0.38, flatten=1.55, elongate=1.60,
                 angular=0.64, facets=5, rough=0.40, erode=0.38,
                 cavernous=0.05, bedding=0.62, bed_thickness_m=0.20,
                 bed_dip_deg=2.0, notch=0.80, notch_z_m=0.35,
                 notch_spread_m=0.22, bury=0.40, rubble=0.90,
                 mat_rock="sand",
                 bio_beach=1.0, bio_bare_rock=0.6, bio_grassland=0.4,
                 place_abundance=0.25, place_spacing_m=14.0,
                 place_elev_max_m=25, place_slope_max_pct=70)),
    ),
    "beachrock-slab": (
        "2.20 m - thin cemented sand sheets dipping seaward, broken into plates",
        base(name="beachrock-slab",
             notes="THE FLATTEST BEDDED THING IN THE LIBRARY, and the whole "
                   "asset is three parameters agreeing: `flatten` 0.26 for the "
                   "sheet, `bed_thickness_m` 0.15 for how thin one layer is, "
                   "and `bed_dip_deg` 10 for the seaward tilt. Beachrock is "
                   "beach sand cemented in place and then exhumed, so it keeps "
                   "the beach's own slope -- that tilt is what tells it from "
                   "every other flat rock on the shore, and it is the only "
                   "reason the dip parameter is here.\n\n"
                   "IT BREAKS INTO RECTANGLES, which is `joint_sets` 2 with a "
                   "small opening: two families of near-vertical cracks across "
                   "a flat sheet give plates rather than blocks. The opening is "
                   "kept small (0.05 m) because at 10 cm that is half a voxel "
                   "of gap -- enough to read as a crack and not enough to part "
                   "the slab into pieces, which `tools/buildcheck.py` would "
                   "reject.\n\n"
                   "THE BEDDING BARELY OPERATES AND THAT IS MEASURED, NOT "
                   "SUSPECTED. Turning `rock.bedding` off changes this build by "
                   "11 voxels out of 215 -- about 5% -- because the finished "
                   "sheet is only two or three voxels thick and differential "
                   "erosion has nothing to bite on. It is left at 0.70 because "
                   "it is true of the rock and starts working the moment anyone "
                   "thickens the slab; what carries the asset today is the "
                   "seaward DIP and the plate joints. Recorded so a wired-up "
                   "slider doing almost nothing is not mistaken for one doing "
                   "the work, which is a failure this project has shipped "
                   "before.\n\n"
                   "`sand` is the right material for once rather than a "
                   "substitute: the rock is cemented sand.",
             **r(size_m=2.2, lumps=4, spread=0.45, flatten=0.26, elongate=1.80,
                 angular=0.70, facets=6, rough=0.30, erode=0.26,
                 cavernous=0.10, bedding=0.70, bed_thickness_m=0.15,
                 bed_dip_deg=10.0, joint_sets=2, joint_scatter=0.08,
                 block_size_m=0.9, block_relief_m=0.05,
                 notch=0.40, notch_z_m=0.10, notch_spread_m=0.10,
                 bury=0.38, rubble=0.25, mat_rock="sand",
                 bio_beach=1.0, bio_bare_rock=0.3,
                 place_abundance=0.45, place_spacing_m=6.0,
                 place_elev_max_m=6, place_slope_max_pct=20)),
    ),
    "coquina-shell-rock": (
        "1.40 m - a pale coarse block visibly made of broken shell",
        base(name="coquina-shell-rock",
             notes="THE SAME PARAMETER `conglomerate-boulder` USES AND THE SAME "
                   "HONEST LIMIT. Coquina is cemented shell fragment -- a rock "
                   "whose entire identity is that you can see what it is made "
                   "of -- and `rock.clasts` is the only thing here that embeds "
                   "separate lumps in a softer matrix so some stand proud and "
                   "others rot out and leave sockets.\n\n"
                   "CLAST SIZE IS AT THE FLOOR AND THE REAL ROCK IS FINER THAN "
                   "THE FLOOR. The parameter's own note says that under about "
                   "15 cm a clast is one to three voxels and reads as noise; "
                   "0.20 m is two voxels of relief at the terrain lattice. Real "
                   "shell hash is centimetre-scale, so what this spec gives is "
                   "the right FABRIC at the wrong grain -- a visibly lumpy "
                   "sockety pale block -- and the actual shell texture is a job "
                   "for the texture pass, not for geometry. Written down so "
                   "nobody tries to fix it by turning the clast size down: "
                   "0.08 m is under one voxel and would produce nothing at all.\n\n"
                   "Soft: high erosion, high roughness, low angularity, and a "
                   "cavernous lean so the weathered face pits rather than "
                   "rounds.",
             **r(size_m=1.4, lumps=4, spread=0.36, flatten=0.68, elongate=1.30,
                 angular=0.30, facets=3, rough=0.55, erode=0.52,
                 cavernous=0.40, clasts=520, clast_size_m=0.20,
                 clast_hardness=2.0, bury=0.28, rubble=0.30,
                 mat_rock="sand",
                 bio_beach=1.0, bio_bare_rock=0.3,
                 place_abundance=0.4, place_spacing_m=8.0,
                 place_elev_max_m=12, place_slope_max_pct=50)),
    ),
    "boulder-beach": (
        "3.00 m - well-rounded blocks of one size packed tight, no gaps",
        base(name="boulder-beach",
             notes="A PACKED CLUSTER, AND IT STAYS ON THE RIGHT SIDE OF THE "
                   "DISTRIBUTION LINE. The blockfield, the talus cone and the "
                   "shingle bank are all refused in this library because they "
                   "are distributions of stones over ground and belong to "
                   "placement; a boulder beach could be read the same way. What "
                   "makes it authorable is that its stones TOUCH -- they are "
                   "packed rim to rim with no sand between, which is the beach "
                   "file's own description -- so one connected mass of rounded "
                   "blocks is an honest unit of it, and a scatterer laying "
                   "several of these edge to edge gets the landform. A talus "
                   "cone has air and ground between its blocks and cannot be "
                   "said this way.\n\n"
                   "THE BLOCKS COME FROM `rock.corestone`, NOT FROM LUMPS. "
                   "Corestone rotting works inward from the joint planes, so "
                   "each block in the fracture grid loses its corners and a "
                   "rounded core survives -- which is exactly a heap of "
                   "well-rounded stones with the old grid still legible in how "
                   "they sit. `settle_m` then lets them drop into the space "
                   "that was taken out, because blocks in perfect alignment "
                   "read as one stone with grooves cut in it.\n\n"
                   "THE JOINT OPENING IS DELIBERATELY SMALL. At 0.05 m it is "
                   "half a voxel: enough that the blocks read as separate, not "
                   "enough to part them, which is the one-piece rule and the "
                   "most likely way this spec would fail. All one size, which "
                   "is what `block_size_m` with a low scatter gives and what "
                   "distinguishes a boulder beach from a moraine.\n\n"
                   "CORESTONE IS AT 0.42 AND NOT HIGHER, WHICH IS A MEASURED "
                   "LIMIT AND NOT CAUTION. Its own help says that turned up far "
                   "enough the cores dissolve too and what is left is a gravel "
                   "pile; at 0.80 with this erosion that is exactly what "
                   "happened -- the stone came out 1.2 m instead of 3.0 and got "
                   "SMALLER on the next seed, because `rock.build`'s "
                   "measure-and-correct loop caps its scale at 4.0 and could no "
                   "longer reach the asked-for size. If these blocks ever look "
                   "too square, raise `erode` before touching corestone again.",
             **r(size_m=3.0, lumps=6, spread=0.48, flatten=0.55, elongate=1.60,
                 angular=0.16, facets=2, rough=0.50, erode=0.34,
                 cavernous=0.0, joint_sets=3, joint_scatter=0.10,
                 joint_dip_deg=0.0, block_size_m=1.00, block_relief_m=0.05,
                 corestone=0.42, settle_m=0.10,
                 notch=0.50, notch_z_m=0.20, notch_spread_m=0.16,
                 bury=0.34, rubble=0.45, mat_rock="rock",
                 bio_beach=1.0, bio_bare_rock=0.4, bio_tundra_alpine=0.2,
                 place_abundance=0.6, place_spacing_m=3.5,
                 place_elev_max_m=10, place_slope_max_pct=40)),
    ),
    "blowhole": (
        "2.60 m - a low platform with a flared rim round a deep dark shaft",
        base(name="blowhole",
             notes="THE VOID IS THE ASSET AND THE VOID IS AS FAR AS THIS "
                   "GENERATOR REACHES. A blowhole is a vertical shaft through a "
                   "wave-cut platform into a sea cave below, and there is no "
                   "vertical bore in the rock generator: `rock.arch` is the only "
                   "through-going cut and it works across the thinner "
                   "HORIZONTAL axis with an opening that runs down to the "
                   "ground (`forge/rock.py:1219-1330`), which is a doorway and "
                   "not a chimney. It is also refused under 4 m.\n\n"
                   "SO THE SHAFT HERE IS A DEEP CLOSED HOLLOW, NOT A HOLE "
                   "THROUGH. `rock.pans` dissolves flat-floored hollows with "
                   "slightly overhung rims wherever water running down the "
                   "stone has nowhere lower to go, and that overhung mouth in a "
                   "flat bench is what a player standing on the platform "
                   "actually sees. What is missing is daylight from below and "
                   "the spout, and neither is reachable: RECORD RATHER THAN "
                   "RETUNE.\n\n"
                   "PAN DEPTH AND BENCH THICKNESS ARE ONE SETTING IN TWO "
                   "PLACES, AND THAT IS MEASURED. A pan as deep as the stone is "
                   "thick does not make a shaft -- it removes the platform. "
                   "Authored at `pan_depth_m` 1.10 on a 0.9 m bench it did "
                   "exactly that: the stone measured 4.1 m against an asked-for "
                   "2.6 on seed 1 and came out COMPLETELY EMPTY on seed 2, "
                   "because the pans ate the body that `rock.build`'s "
                   "size-fitting loop measures. What is authored instead is a "
                   "THICKER bench -- `flatten` 0.70, which lands 1.0-1.4 m of "
                   "stone above ground -- with a 0.60 m pan in it, and the "
                   "deepest hollow then measures 0.9-1.3 m across seeds 1-3 "
                   "with a mouth of half a square metre. That is a shaft. "
                   "ANYONE DEEPENING THE HOLE MUST THICKEN THE BENCH IN THE "
                   "SAME EDIT.\n\n"
                   "The pans are found rather than placed, so which hollow "
                   "becomes the shaft varies by seed and six to ten shallower "
                   "ones come with it. That is correct for the landform -- real "
                   "platforms are pitted all round a blowhole -- and it is not "
                   "worth trying to force to exactly one.",
             **r(size_m=2.6, lumps=5, spread=0.36, flatten=0.70, elongate=1.25,
                 angular=0.44, facets=4, rough=0.36, erode=0.28,
                 cavernous=0.20, pans=0.75, pan_depth_m=0.60,
                 bedding=0.30, bed_thickness_m=0.40,
                 notch=0.45, notch_z_m=0.12, notch_spread_m=0.12,
                 bury=0.38, rubble=0.20, mat_rock="rock",
                 bio_beach=1.0, bio_bare_rock=0.4,
                 place_abundance=0.15, place_spacing_m=25.0,
                 place_elev_max_m=10, place_slope_max_pct=25)),
    ),
    # --- grassland ------------------------------------------------------------
    "gravel-bar": (
        "2.40 m - a low lens of well-sorted rounded stone on a river bend",
        base(name="gravel-bar",
             notes="A LENS, NOT A STONE, and the shape is the species: very "
                   "flat, strongly elongated, deeply buried, so what shows is a "
                   "shallow bank of loose rounded material rather than an "
                   "object sitting on the ground. It goes on the inside of a "
                   "bend where the water slows, which is a placement statement "
                   "-- hard against water, on nearly flat ground.\n\n"
                   "`gravel` FOR THE MATERIAL, WHICH IS THE POINT OF THAT "
                   "CHOICE: its high per-voxel jitter is what makes a heap read "
                   "as loose stones rather than as one carved mass, and no "
                   "geometry setting does that.\n\n"
                   "THE BIOME FILE'S 0.15 m IS A CLAST SIZE AND IT IS UNDER TWO "
                   "VOXELS AT THE TERRAIN LATTICE. `rock.clast_size_m`'s own "
                   "note puts the floor of what reads at about 0.15 m and this "
                   "spec authors 0.22 -- so the individual stones are coarser "
                   "here than on a real bar, in exchange for being visible at "
                   "all. The alternative is a smooth mound, which is not a "
                   "gravel bar. Same trade `conglomerate-boulder` records.\n\n"
                   "Rounded and unsorted-looking at the edges: angularity near "
                   "zero, no facets, high roughness, high lump spread.",
             **r(size_m=2.4, lumps=7, spread=0.52, flatten=0.34, elongate=1.90,
                 angular=0.06, facets=0, rough=0.60, erode=0.30,
                 cavernous=0.0, clasts=600, clast_size_m=0.22,
                 clast_hardness=3.0, bury=0.40, rubble=0.60,
                 mat_rock="gravel",
                 bio_grassland=1.0, bio_temperate_forest=0.7, bio_taiga=0.5,
                 bio_savanna=0.4, bio_tundra_alpine=0.3,
                 place_abundance=0.5, place_spacing_m=5.0,
                 place_water_max_m=3, place_slope_max_pct=12)),
    ),
    # --- temperate forest floor ----------------------------------------------
    "mossy-sandstone-bench": (
        "2.40 m - a bedded step with a flat mossy top and a shaded riser",
        base(name="mossy-sandstone-bench",
             notes="A STEP, WHICH IS ONE THICK HARD BED OVER SOFTER ONES. "
                   "`bedding` 0.55 at a 0.70 m bed thickness gives few massive "
                   "layers rather than fine banding, and the soft ones retreat "
                   "and leave the hard one standing out as a tread with a "
                   "shaded riser under it. That overhang is where the ferns go "
                   "in life and it is the reason this is a bench and not a "
                   "slab.\n\n"
                   "THE MOSS CARPET IS NOT A MATERIAL AND CANNOT BE. There is "
                   "one `materials.rock` per spec and none of its five choices "
                   "is green -- `mossy-forest-boulder` ships with exactly the "
                   "same gap and its notes say the same thing. What this asset "
                   "gives is the SHAPE that carries moss: a broad flat "
                   "near-level top, which is where water sits and moss grows, "
                   "against a steep dry riser. Greening it is a renderer or "
                   "surface-pass job.\n\n"
                   "`aspect` 0.5 weathers one side harder than the rest, which "
                   "on a bench is the wet shaded face -- the riser rounds and "
                   "undercuts while the dry top stays square.",
             **r(size_m=2.4, lumps=4, spread=0.34, flatten=0.60, elongate=1.70,
                 angular=0.55, facets=5, rough=0.42, erode=0.36,
                 cavernous=0.20, aspect=0.50,
                 bedding=0.55, bed_thickness_m=0.70, bed_dip_deg=3.0,
                 bury=0.32, rubble=0.30, mat_rock="sand",
                 bio_temperate_forest=1.0, bio_taiga=0.5,
                 bio_rainforest=0.4, bio_grassland=0.3,
                 place_abundance=0.45, place_spacing_m=9.0,
                 place_water_max_m=60, place_slope_max_pct=50)),
    ),
    "tufa-curtain": (
        "2.00 m - a pale porous drapery of lobes hanging where a spring runs over",
        base(name="tufa-curtain",
             notes="THE ONLY ROCK IN THE LIBRARY THAT IS BUILT UP RATHER THAN "
                   "CUT DOWN, and the generator only cuts. Tufa is limestone "
                   "coming OUT of spring water and accreting where it splashes, "
                   "so its lobed drapery is a growth form -- and the nearest "
                   "thing here that knows which way is down is `rock.flutes`, "
                   "which runs water over the outside and cuts vertical runnels "
                   "that merge downhill. Run at 0.9 on a tall flattened stone "
                   "it gives a hanging fluted curtain, which is the same "
                   "SILHOUETTE arrived at from the opposite direction. That is "
                   "the honest description of this spec: the shape is right and "
                   "the process is inverted.\n\n"
                   "POROUS IS THE OTHER HALF and it is `cavernous` 0.55 with a "
                   "light rind: tufa is full of holes where it grew around moss "
                   "and twigs, and cavernous weathering deepens hollows faster "
                   "than the rock around them, which is the same look.\n\n"
                   "`sand` for the pale cream, the same substitute the chalk "
                   "specs take, and for once the real rock is genuinely a "
                   "porous cream colour rather than white.\n\n"
                   "Hard against water on steep ground: a tufa curtain only "
                   "exists at a lip a spring runs over.",
             **r(size_m=2.0, lumps=4, spread=0.28, flatten=2.00, elongate=2.20,
                 angular=0.22, facets=2, rough=0.50, erode=0.40,
                 cavernous=0.55, rind=0.35, rind_m=0.10,
                 flutes=0.90, flute_width_m=0.30,
                 bury=0.30, rubble=0.25, mat_rock="sand",
                 bio_temperate_forest=1.0, bio_rainforest=0.5,
                 bio_bare_rock=0.4, bio_grassland=0.3,
                 place_abundance=0.2, place_spacing_m=25.0,
                 place_water_max_m=3, place_slope_max_pct=70)),
    ),
    "sinkhole-rim": (
        "3.00 m - the overhanging lip of a collapse, undercut and going nowhere",
        base(name="sinkhole-rim",
             notes="THIS IS A SEGMENT OF RIM AND NOT A RING, WHICH IS THE WHOLE "
                   "DESIGN DECISION. A sinkhole is a hole in the GROUND: the "
                   "ring and the void belong to terrain, and an asset that "
                   "tried to be the whole collapse would have to carve the "
                   "world, which nothing in a spec does. What is authorable is "
                   "the part that reads from a few metres away -- a long "
                   "waist-high block of limestone whose face is undercut so it "
                   "overhangs.\n\n"
                   "IT IS AUTHORED TALLER AND LESS BURIED THAN IT FIRST WAS, "
                   "AND THE REASON IS THAT AN OVERHANG NEEDS SOMETHING TO HANG "
                   "OVER. At `flatten` 0.55 with `bury` 0.52 and the notch at "
                   "2.6, the three of them between them left a ribbon of stone "
                   "three voxels tall across seeds 1-3 -- a rim with no face on "
                   "it. `flatten` 0.90, `bury` 0.34 and a gentler notch give "
                   "0.7-1.2 m of standing stone with the undercut still "
                   "visible.\n\n"
                   "THE OVERHANG IS `rock.notch` AT 1.6 WITH A NARROW SPREAD, "
                   "the same parameter the tidal notch uses and for the same "
                   "reason: it is the only way to remove stone at one height "
                   "and leave what is above it standing. Here the attack is "
                   "high rather than at the foot, because a collapse rim is "
                   "eaten from underneath by the void, not by the sea.\n\n"
                   "A scatterer laying several of these round a depression is "
                   "what makes the ring. That is a placement job and it is the "
                   "same answer the erratic train and the shingle bank get.",
             **r(size_m=3.0, lumps=5, spread=0.38, flatten=0.90, elongate=1.90,
                 angular=0.66, facets=6, rough=0.40, erode=0.30,
                 cavernous=0.25, notch=1.60, notch_z_m=0.50,
                 notch_spread_m=0.20,
                 joint_sets=2, joint_scatter=0.10, block_size_m=1.4,
                 bedding=0.45, bed_thickness_m=0.50,
                 bury=0.34, rubble=0.40, mat_rock="rock",
                 bio_temperate_forest=1.0, bio_grassland=0.5,
                 bio_bare_rock=0.5, bio_rainforest=0.3,
                 place_abundance=0.2, place_spacing_m=16.0,
                 place_slope_max_pct=45)),
    ),
    "charcoal-blackened-block": (
        "1.60 m - a dark block that has been through a fire and spalled",
        base(name="charcoal-blackened-block",
             notes="THE BIOME FILE CALLS THIS A PALETTE VARIANT AND THE PALETTE "
                   "CANNOT DO IT, SO IT IS AUTHORED AS A SHAPE INSTEAD. There "
                   "is one `materials.rock` per spec, so 'sooted on one side "
                   "only' is unsayable -- `rock.aspect` weathers a side harder, "
                   "it does not colour one -- and none of the five rock "
                   "materials is black. `bedrock` is the darkest and reads as a "
                   "fresh unweathered face, which is half right.\n\n"
                   "WHAT FIRE ACTUALLY DOES TO STONE IS SPALL IT, and that IS "
                   "sayable: heat drives sheets off parallel to the surface, "
                   "which is `rock.exfoliate` -- the same mechanism as a "
                   "granite dome, at a much thinner shell (0.12 m against "
                   "`exfoliating-dome`'s 0.30) and on a small angular block. "
                   "The result is a dark stone with fresh curved flakes off its "
                   "faces and sharp arrises between them, which is what a "
                   "hearth stone or a burnt outcrop looks like and what nothing "
                   "else in the library looks like.\n\n"
                   "So this spec earns its place on geometry and NOT on colour, "
                   "and the black is still an ask. If a soot material ever "
                   "lands, this is the spec that wants it.",
             **r(size_m=1.6, lumps=4, spread=0.32, flatten=0.70, elongate=1.35,
                 angular=0.70, facets=6, rough=0.38, erode=0.26,
                 cavernous=0.0, exfoliate=0.65, shell_m=0.12,
                 aspect=0.45, joint_sets=2, joint_scatter=0.12,
                 block_size_m=1.1,
                 bury=0.30, rubble=0.35, mat_rock="bedrock",
                 bio_temperate_forest=1.0, bio_taiga=0.6, bio_savanna=0.4,
                 bio_grassland=0.4, bio_bare_rock=0.3,
                 place_abundance=0.25, place_spacing_m=12.0,
                 place_slope_max_pct=55)),
    ),
    # --- rainforest -----------------------------------------------------------
    "quartz-vein-cobble-bar": (
        "1.80 m - a low bar of rounded cobbles, some cut by white quartz fins",
        base(name="quartz-vein-cobble-bar",
             notes="AUTHORED AS THE BAR, NOT AS ONE COBBLE, AND THE LATTICE "
                   "FORCED IT. The rainforest file lists this at 0.35 m, which "
                   "at the terrain lattice's 10 cm is three and a half voxels "
                   "-- and the white vein that names it would then be "
                   "sub-voxel, because `rock.vein_width_m`'s own note says a "
                   "vein under 0.10 m comes and goes along its length. A 0.35 m "
                   "quartz-veined cobble is not buildable here in any useful "
                   "sense. The row also says the stones are 'sized to sit in a "
                   "bar rather than stand alone', so the bar is what is "
                   "authored: one low spread of packed cobbles at 1.8 m, with "
                   "veins wide enough to survive.\n\n"
                   "TWO VEINS AT 0.14 m AND HARDNESS 4.5. Veins resist the "
                   "weathering that takes the rock around them and end up "
                   "standing proud as fins, and the eye follows a continuous "
                   "line at far lower resolution than it reads a blob -- which "
                   "is why a 1.4-voxel white stripe carries further than its "
                   "width suggests and is worth spending the whole asset on.\n\n"
                   "`gravel` for the material, whose per-voxel jitter is what "
                   "makes a packed heap read as separate stones. The mixed dark "
                   "grey and white is the vein against the matrix and nothing "
                   "else; two rock colours in one spec is not available.\n\n"
                   "Hard against water on flat ground, like the gravel bar it "
                   "is the tropical cousin of.",
             **r(size_m=1.8, lumps=6, spread=0.44, flatten=0.62, elongate=1.50,
                 angular=0.10, facets=0, rough=0.55, erode=0.34,
                 cavernous=0.0, veins=2, vein_width_m=0.14,
                 vein_hardness=4.5, clasts=260, clast_size_m=0.24,
                 clast_hardness=2.4, bury=0.28, rubble=0.55,
                 mat_rock="gravel",
                 bio_rainforest=1.0, bio_temperate_forest=0.5,
                 bio_savanna=0.3, bio_bare_rock=0.3,
                 place_abundance=0.5, place_spacing_m=5.0,
                 place_water_max_m=3, place_slope_max_pct=15)),
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
