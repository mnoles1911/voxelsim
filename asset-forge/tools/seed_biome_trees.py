"""Author thirty trees, one biome at a time.

WHY TREES ARE WORTH THE COST. A tree is the most expensive asset in the library
by an order of magnitude and it is also the one a player looks at longest: in
savanna it is a single silhouette against a big sky, in grassland it is a lone
object in open grass, and in taiga it is the whole canopy. The savanna file puts
it plainly -- a biome with three tree species in it reads as wallpaper -- and
that biome is 20.76% of the world's land with four tree specs.

TEN CENTIMETRES, AND NOTHING ELSE. Trees join the world's own voxel grid and
are destructible as terrain is (`forge/kinds.py:29-58`), the grid has exactly
one cell size, and `forge.cli.selftest` refuses a terrain-lattice spec at any
other resolution. Every spec here is `resolution_cm` "10" and that is not a
choice.

WHAT THAT LATTICE COSTS THE LIST, STATED ONCE. At 10 cm a twig is one voxel and
a leaf does not exist, so a species whose identity is a LEAF cannot be built
here at all -- and several of the most-wanted trees in the biome files are
exactly that. What survives is bark colour, crown envelope, branch architecture
and bole shape, and every spec below is authored against those four. Where a
species' real field mark is finer than the lattice, the `notes` say so rather
than pretending.

THE THREE GROWTH MODELS DO THE WORK AND THEY ARE NOT INTERCHANGEABLE
(`README.md`, *Growth models*). `colonize` grows branches toward scattered
targets and suits irregular broadleaf crowns. `whorl` lays down rings of
branches up a straight leader, which is what a conifer IS -- colonization can
only make a crown that is cone-SHAPED, never one that is actually tiered.
`frond` is an unbranched trunk carrying long arcing leaves, which is a palm. Of
the thirty here, thirteen are whorl, five are frond and twelve are colonize.

TWO SPECIES ARE AUTHORED AGAINST A PALETTE GAP AND SAY SO. A saguaro and a
Joshua tree are green columns, and the wood palette has bark, pale bark,
heartwood and deadwood -- no green. Both are drawn as a bare whorl skeleton
CLOTHED in a thin tight shell of jungle-leaf foliage, which greens the columns
at the cost of a soft edge. That is a workaround for a missing material and it
is recorded in each spec.

    python tools/seed_biome_trees.py
    python tools/seed_biome_trees.py --force

SIZES ARE APPROXIMATE. Every height is the approximate figure from the biome
file it came from; those are unsourced general-knowledge estimates by their own
admission. Nothing here is quoted as measured.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(**over):
    changes = {
        "kind": "tree",
        "resolution_cm": "10",
        "variation.amount": 1.0,
        "variation.height": 0.18,
        "variation.crown_radius": 0.18,
        "variation.trunk_radius": 0.18,
        "variation.shape": 0.12,
        "variation.proportion": 0.30,
        "variation.lean_deg": 7.0,
        "variation.density": 0.12,
        "variation.droop": 0.25,
        "variation.rotate": True,
    }
    changes.update(over)
    return changes


def t(**kw):
    """`trunk.*`/`crown.*`/... from keywords, with a group prefix per name."""
    groups = {
        "radius_base_m": "trunk", "clear_frac": "trunk", "lean_deg": "trunk",
        "wander": "trunk", "buttress": "trunk",
        "shape": "crown", "radius_m": "crown", "height_frac": "crown",
        "center_frac": "crown", "shell_upper": "crown",
        "shell_lower": "crown", "squash": "crown", "asymmetry": "crown",
        "offset": "crown", "points": "crown", "lean_deg_crown": "crown",
        "model": "growth", "step_m": "growth", "influence_m": "growth",
        "kill_m": "growth", "gravity": "growth", "phototropism": "growth",
        "inertia": "growth", "jitter": "growth", "max_iter": "growth",
        "shade": "growth", "tip_radius_m": "growth", "radius_exp": "growth",
        "enabled": "foliage", "min_order": "foliage",
        "clump_radius_m": "foliage", "density": "foliage", "rough": "foliage",
        "habit": "foliage", "stretch": "foliage", "clustering": "foliage",
        "top_bias": "foliage", "coverage": "foliage", "separation": "foliage",
        "clump_jitter": "foliage", "droop_m": "foliage",
        "squash_f": "foliage",
    }
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials." + k[len("mat_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        elif k.startswith("whorl_"):
            out["whorl." + k[len("whorl_"):]] = v
        elif k.startswith("frond_"):
            out["frond." + k[len("frond_"):]] = v
        elif k.startswith("roots_"):
            out["roots." + k[len("roots_"):]] = v
        elif k.startswith("strand_"):
            out["strand." + k[len("strand_"):]] = v
        elif k == "squash_f":
            out["foliage.squash"] = v
        elif k == "lean_deg_crown":
            out["crown.lean_deg"] = v
        else:
            out[f"{groups[k]}.{k}"] = v
    return out


SPECIES = {
    # --- taiga: the biome's own build priority is spruce, pine, larch -------
    "norway-spruce": (
        "35 m - a narrow cone from the ground to a single spike",
        base(name="norway-spruce", height_m=35.0,
             notes="THE SINGLE MOST IDENTITY-DEFINING OBJECT IN THE TAIGA, and "
                   "the taiga file says so: a boreal forest is a field of "
                   "narrow dark spires and nothing shipped made that shape. "
                   "The shipped `tundra-pine` is 9 m with wide-spaced whorls "
                   "and open branch ends; a spruce is four times taller, has "
                   "twenty-two rings instead of fourteen, and is clothed to the "
                   "ground.\n\n"
                   "`whorl` AND NOT `colonize`, which is the whole reason the "
                   "second growth model exists: a spruce's tiers are a real "
                   "structure laid down one ring a year, and colonization can "
                   "only make a crown that is cone-shaped. `radial` foliage "
                   "habit -- needles all round the shoot and kept five to seven "
                   "years, so the whole shoot is clothed, which is what makes a "
                   "spruce the densest conifer there is.",
             **t(radius_base_m=0.55, clear_frac=0.06, lean_deg=1.5, wander=0.04,
                 buttress=0.12,
                 shape="cone", radius_m=3.6, height_frac=0.95,
                 center_frac=0.50, shell_upper=0.80, shell_lower=0.75,
                 asymmetry=0.12, offset=0.06, points=1600,
                 model="whorl", step_m=0.26, influence_m=2.4, kill_m=0.5,
                 gravity=-0.34, inertia=0.6, jitter=0.05, max_iter=340,
                 tip_radius_m=0.05, radius_exp=2.5,
                 whorl_count=22, whorl_branches=6, whorl_droop=1.05,
                 whorl_rise=0.20, whorl_sub=2, whorl_irregular=0.20,
                 whorl_leader=0.05,
                 clump_radius_m=0.34, density=0.76, habit="radial",
                 stretch=4.2, coverage=0.98, separation=1.45,
                 clump_jitter=0.28, squash_f=0.55, droop_m=0.14,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_taiga=1.0, bio_tundra_alpine=0.3,
                 bio_temperate_forest=0.35,
                 place_abundance=1.0, place_spacing_m=5.0, place_cluster=0.85,
                 place_slope_max_pct=55, place_elev_max_m=2000)),
    ),
    "scots-pine": (
        "25 m - bare orange-red upper trunk under a high flat-topped crown",
        base(name="scots-pine", height_m=25.0,
             notes="THE BARK IS THE SPECIES AND THE LATTICE CANNOT COLOUR HALF "
                   "A TRUNK. A Scots pine is grey-brown below and flaking "
                   "orange-red on the upper third, and there is one bark "
                   "material per spec -- so the trunk is drawn in the pale bark "
                   "throughout, which gets the orange at the top right and the "
                   "bottom wrong. A per-height bark split is a real feature "
                   "request and it would serve the birch, the aspen and the "
                   "fever tree as well as this.\n\n"
                   "What IS expressible is the crown: bare for two thirds of "
                   "the height, then a high open irregular flat top -- "
                   "`clear_frac` 0.62 with a `wedge` envelope and only nine "
                   "whorls, so the tiers are far apart and daylight goes "
                   "through.",
             **t(radius_base_m=0.50, clear_frac=0.62, lean_deg=4.0, wander=0.18,
                 buttress=0.10,
                 shape="wedge", radius_m=4.2, height_frac=0.34,
                 center_frac=0.82, shell_upper=0.45, shell_lower=0.30,
                 asymmetry=0.35, offset=0.22, points=1100,
                 model="whorl", step_m=0.30, influence_m=2.8, kill_m=0.55,
                 gravity=-0.10, inertia=0.5, jitter=0.10, max_iter=300,
                 tip_radius_m=0.06, radius_exp=2.4,
                 whorl_count=9, whorl_branches=5, whorl_droop=0.40,
                 whorl_rise=0.34, whorl_sub=3, whorl_irregular=0.40,
                 whorl_leader=0.02,
                 clump_radius_m=0.55, density=0.62, habit="tuft",
                 stretch=2.6, coverage=0.88, separation=1.7,
                 clump_jitter=0.40, squash_f=0.70,
                 mat_bark="bark_pale", mat_leaf="leaf_needle",
                 bio_taiga=1.0, bio_grassland=0.35,
                 bio_temperate_forest=0.4, bio_tundra_alpine=0.2,
                 place_abundance=0.8, place_spacing_m=7.0, place_cluster=0.7,
                 place_slope_max_pct=55)),
    ),
    "siberian-larch": (
        "30 m - a conifer that goes BARE: open conical crown, gold in autumn",
        base(name="siberian-larch", height_m=30.0,
             notes="THE ONLY DECIDUOUS CONIFER, AND THE ONLY SEASONAL COLOUR "
                   "CHANGE IN THE BIOME. It is authored in its autumn state -- "
                   "`leaf_autumn`, the gold -- because that is the phase nobody "
                   "confuses with a spruce; the summer green and the bare "
                   "winter skeleton are the same geometry with `materials.leaf` "
                   "changed and `foliage.enabled` off, which is two one-field "
                   "forks if they are ever wanted.\n\n"
                   "Much more open than the spruce beside it: half the "
                   "coverage, a `rosette` habit that puts foliage in clusters "
                   "on older wood rather than clothing the shoot, and a wide "
                   "separation. A larch you can see through is a larch.",
             **t(radius_base_m=0.48, clear_frac=0.22, lean_deg=2.0, wander=0.08,
                 buttress=0.10,
                 shape="cone", radius_m=3.4, height_frac=0.80,
                 center_frac=0.58, shell_upper=0.60, shell_lower=0.45,
                 asymmetry=0.22, offset=0.14, points=1300,
                 model="whorl", step_m=0.30, influence_m=2.6, kill_m=0.55,
                 gravity=-0.22, inertia=0.55, jitter=0.10, max_iter=320,
                 tip_radius_m=0.05, radius_exp=2.4,
                 whorl_count=17, whorl_branches=5, whorl_droop=0.75,
                 whorl_rise=0.26, whorl_sub=2, whorl_irregular=0.34,
                 whorl_leader=0.05,
                 clump_radius_m=0.40, density=0.52, habit="rosette",
                 stretch=2.0, coverage=0.72, separation=2.0,
                 clump_jitter=0.42, squash_f=0.80,
                 mat_bark="bark", mat_leaf="leaf_autumn",
                 bio_taiga=1.0, bio_tundra_alpine=0.35,
                 place_abundance=0.8, place_spacing_m=6.0, place_cluster=0.8,
                 place_slope_max_pct=55)),
    ),
    # --- temperate forest: bark and bole, which the shipped set lacks -------
    "european-beech": (
        "30 m - smooth pewter bole and a crown so dense nothing grows under it",
        base(name="european-beech", height_m=30.0,
             notes="A CROWN THAT DARKENS THE GROUND BENEATH IT, which is a "
                   "PLACEMENT property as much as a shape one and is authored "
                   "in both: the densest foliage of any broadleaf here "
                   "(coverage 0.95 at separation 1.55, against the oak's 0.85 "
                   "at 1.9) and a wide minimum spacing so a beech stand is "
                   "beeches and nothing else.\n\n"
                   "`distichous` habit -- leaves two-ranked into one flat plane "
                   "held level to the light -- which is what makes a beech "
                   "spray a spray and is the single most beech-like thing the "
                   "generator can say. The smooth unfissured pewter bark cannot "
                   "be said at all: bark texture is below the lattice, so what "
                   "carries it is the very clean unbranched lower bole "
                   "(`clear_frac` 0.45 with almost no wander).",
             **t(radius_base_m=0.70, clear_frac=0.45, lean_deg=2.0, wander=0.06,
                 buttress=0.30,
                 shape="ovoid", radius_m=7.0, height_frac=0.55,
                 center_frac=0.72, shell_upper=0.75, shell_lower=0.35,
                 asymmetry=0.30, offset=0.30, points=2000,
                 model="colonize", step_m=0.32, influence_m=2.8, kill_m=0.50,
                 gravity=-0.12, inertia=0.48, jitter=0.06, max_iter=320,
                 shade=0.55, tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.85, density=0.72, habit="distichous",
                 stretch=2.6, coverage=0.95, separation=1.55,
                 clump_jitter=0.30, top_bias=0.45, squash_f=0.72,
                 mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_taiga=0.2,
                 place_abundance=0.9, place_spacing_m=9.0, place_cluster=0.9,
                 place_slope_max_pct=50, place_elev_max_m=1200)),
    ),
    "hornbeam": (
        "20 m - a fluted muscled grey bole under a dense low crown",
        base(name="hornbeam", height_m=20.0,
             notes="THE BOLE LOOKS TWISTED UNDER TENSION, and the one parameter "
                   "that would say it -- `trunk.lobes`, which runs vertical "
                   "grooves up the trunk -- IS NOT SAFE AND IS LEFT AT ZERO. "
                   "Its own help text says so: grooving the trunk severs the "
                   "joins where limbs and roots attach and leaves wood "
                   "floating, and four separate restrictions each reduced the "
                   "damage and none removed it. Doing this properly means "
                   "fluting the capsule as it is drawn rather than carving "
                   "afterwards, and that is generator work.\n\n"
                   "So the hornbeam is authored on the other half of its "
                   "identity: a short heavy bole with a very dense low crown "
                   "starting under half its height, which is a different "
                   "silhouette from the beech even without the fluting.",
             **t(radius_base_m=0.52, clear_frac=0.26, lean_deg=3.0, wander=0.22,
                 buttress=0.35,
                 shape="ovoid", radius_m=5.2, height_frac=0.70,
                 center_frac=0.62, shell_upper=0.72, shell_lower=0.45,
                 asymmetry=0.38, offset=0.28, points=1800,
                 model="colonize", step_m=0.28, influence_m=2.4, kill_m=0.44,
                 gravity=-0.14, inertia=0.42, jitter=0.10, max_iter=300,
                 shade=0.48, tip_radius_m=0.05, radius_exp=2.2,
                 clump_radius_m=0.70, density=0.70, habit="distichous",
                 stretch=2.4, coverage=0.92, separation=1.6,
                 clump_jitter=0.34, squash_f=0.80,
                 mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0,
                 place_abundance=0.7, place_spacing_m=6.5, place_cluster=0.8,
                 place_elev_max_m=900)),
    ),
    "common-alder": (
        "18 m - multi-stemmed, dark, standing in water on arched roots",
        base(name="common-alder", height_m=18.0,
             notes="THE ROOTS ARE THE POINT AND THEY ARE REAL GEOMETRY. "
                   "`roots.count` 7 with a long reach and a high arch draws "
                   "ridges humping out of the base and back down to the "
                   "ground, which is what an alder standing in water looks "
                   "like. That is distinct from `trunk.buttress`, which only "
                   "thickens the cylinder -- the alder uses both.\n\n"
                   "Placed hard against water: `water_max_m` 8 and a very low "
                   "slope ceiling, so it lines channels rather than filling "
                   "woods. Dark, narrow, and irregular.",
             **t(radius_base_m=0.34, clear_frac=0.18, lean_deg=6.0, wander=0.32,
                 buttress=0.35,
                 shape="ovoid", radius_m=3.2, height_frac=0.76,
                 center_frac=0.60, shell_upper=0.60, shell_lower=0.42,
                 asymmetry=0.45, offset=0.30, points=1400,
                 model="colonize", step_m=0.26, influence_m=2.2, kill_m=0.42,
                 gravity=-0.14, inertia=0.40, jitter=0.14, max_iter=280,
                 tip_radius_m=0.05, radius_exp=2.2,
                 roots_count=7, roots_length_m=2.4, roots_rise=0.55,
                 roots_thickness=0.50, roots_irregular=0.45,
                 clump_radius_m=0.55, density=0.66, habit="spiral",
                 stretch=2.2, coverage=0.85, separation=1.7,
                 clump_jitter=0.38, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=1.0, bio_taiga=0.5, bio_grassland=0.3,
                 place_abundance=0.6, place_spacing_m=4.5, place_cluster=0.9,
                 place_water_max_m=8, place_slope_max_pct=20)),
    ),
    "european-yew": (
        "12 m - enormously thick fused bole, dark, wider than it is tall",
        base(name="european-yew", height_m=12.0,
             notes="WIDER THAN IT IS TALL, which almost nothing else in the "
                   "library is: a crown radius of 6.5 m on a 12 m tree. The "
                   "trunk is 1.1 m of radius on that height, which is the "
                   "thickest trunk-to-height ratio here after the baobab and is "
                   "what many fused stems look like from outside.\n\n"
                   "Very dark and very dense -- coverage 0.96 with a low "
                   "separation -- so it reads as a solid mass with a trunk "
                   "under it rather than as a crown with structure.",
             **t(radius_base_m=1.10, clear_frac=0.16, lean_deg=5.0, wander=0.30,
                 buttress=0.75,
                 shape="umbrella", radius_m=6.5, height_frac=0.72,
                 center_frac=0.62, shell_upper=0.85, shell_lower=0.60,
                 asymmetry=0.40, offset=0.20, points=1700,
                 model="colonize", step_m=0.24, influence_m=2.0, kill_m=0.38,
                 gravity=-0.06, inertia=0.38, jitter=0.14, max_iter=300,
                 tip_radius_m=0.05, radius_exp=2.2,
                 clump_radius_m=0.50, density=0.80, habit="distichous",
                 stretch=3.0, coverage=0.96, separation=1.4,
                 clump_jitter=0.30, squash_f=0.70,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_temperate_forest=1.0, bio_grassland=0.25,
                 place_abundance=0.25, place_spacing_m=14.0,
                 place_cluster=0.35, place_elev_max_m=800)),
    ),
    "douglas-fir": (
        "40 m - a very tall straight spire with drooping outer branchlets",
        base(name="douglas-fir", height_m=40.0,
             notes="THE TALLEST TREE IN THE LIBRARY AFTER `hero-sequoia`, and "
                   "the shape that makes a Pacific forest look like one: a "
                   "spire rather than a cone, so the widest point is a sixth of "
                   "the way up rather than a third. Deeply corky bark -- below "
                   "the lattice -- so the read is the profile plus the drooping "
                   "outer branchlets, which is `whorl.droop` at 1.25.",
             **t(radius_base_m=0.80, clear_frac=0.34, lean_deg=1.5, wander=0.05,
                 buttress=0.22,
                 shape="spire", radius_m=4.6, height_frac=0.72,
                 center_frac=0.62, shell_upper=0.62, shell_lower=0.42,
                 asymmetry=0.18, offset=0.10, points=1600,
                 model="whorl", step_m=0.34, influence_m=3.0, kill_m=0.6,
                 gravity=-0.30, inertia=0.6, jitter=0.06, max_iter=340,
                 tip_radius_m=0.06, radius_exp=2.5,
                 whorl_count=20, whorl_branches=6, whorl_droop=1.25,
                 whorl_rise=0.20, whorl_sub=2, whorl_irregular=0.24,
                 whorl_leader=0.04,
                 clump_radius_m=0.45, density=0.70, habit="tuft",
                 stretch=3.6, coverage=0.92, separation=1.55,
                 clump_jitter=0.32, squash_f=0.60, droop_m=0.20,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_temperate_forest=1.0, bio_taiga=0.4,
                 place_abundance=0.7, place_spacing_m=8.0, place_cluster=0.8,
                 place_slope_max_pct=50)),
    ),
    "western-hemlock": (
        "35 m - the leader DROOPS OVER at the tip, which is the whole species",
        base(name="western-hemlock", height_m=35.0,
             notes="THE DROOPING TOP IS THE IDENTIFICATION AT ANY DISTANCE, and "
                   "it is the one thing about this tree that cannot be got from "
                   "the whorl model directly -- a leader is drawn straight. "
                   "What is authored instead is everything around it: "
                   "`whorl.leader` is 0 so there is no bare spire above the top "
                   "ring, `whorl.droop` is at 1.5 (the highest here) so the "
                   "uppermost branches fall away, and `trunk.wander` is raised "
                   "to bend the last stretch of the stem. That gets a soft "
                   "nodding top rather than a hard hook, and the hook is a "
                   "generator feature nobody has.\n\n"
                   "Otherwise the finest-textured conifer here: small clumps, "
                   "very long sprays, deeply shading.",
             **t(radius_base_m=0.62, clear_frac=0.20, lean_deg=2.0, wander=0.16,
                 buttress=0.18,
                 shape="cone", radius_m=4.0, height_frac=0.88,
                 center_frac=0.54, shell_upper=0.72, shell_lower=0.55,
                 asymmetry=0.20, offset=0.12, points=1700,
                 model="whorl", step_m=0.28, influence_m=2.6, kill_m=0.52,
                 gravity=-0.40, inertia=0.58, jitter=0.08, max_iter=340,
                 tip_radius_m=0.05, radius_exp=2.5,
                 whorl_count=24, whorl_branches=6, whorl_droop=1.50,
                 whorl_rise=0.14, whorl_sub=3, whorl_irregular=0.26,
                 whorl_leader=0.0,
                 clump_radius_m=0.30, density=0.74, habit="distichous",
                 stretch=4.6, coverage=0.95, separation=1.45,
                 clump_jitter=0.30, squash_f=0.55, droop_m=0.22,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_temperate_forest=1.0, bio_taiga=0.4,
                 place_abundance=0.75, place_spacing_m=6.0, place_cluster=0.9,
                 place_slope_max_pct=55)),
    ),
    "rowan": (
        "8 m - small, upright, feathery, with heavy orange-red berry clusters",
        base(name="rowan", height_m=8.0,
             notes="THE BERRIES ARE THE SPECIES AND THERE IS NO FRUIT "
                   "PRIMITIVE. What the generator has is a leaf material, so "
                   "the crown is drawn in `leaf_autumn` -- the orange -- at a "
                   "low density with wide separation, which reads as an open "
                   "airy crown loaded with orange rather than as an autumn "
                   "tree. That is a deliberate substitution and the honest way "
                   "to get a berry tree at 10 cm; a fruit pass would be the "
                   "real fix and would serve the crab apple, the hawthorn and "
                   "the sea buckthorn too.\n\n"
                   "Small, upright and slim, so it fits under a canopy edge.",
             **t(radius_base_m=0.16, clear_frac=0.30, lean_deg=5.0, wander=0.24,
                 buttress=0.06,
                 shape="ovoid", radius_m=1.9, height_frac=0.62,
                 center_frac=0.70, shell_upper=0.55, shell_lower=0.35,
                 asymmetry=0.35, offset=0.28, points=900,
                 model="colonize", step_m=0.20, influence_m=1.6, kill_m=0.32,
                 gravity=-0.10, inertia=0.38, jitter=0.14, max_iter=260,
                 tip_radius_m=0.05, radius_exp=2.1,
                 clump_radius_m=0.34, density=0.58, habit="rosette",
                 stretch=2.0, coverage=0.78, separation=1.9,
                 clump_jitter=0.42, squash_f=0.85,
                 mat_bark="bark_pale", mat_leaf="leaf_autumn",
                 bio_temperate_forest=0.9, bio_taiga=0.6,
                 bio_grassland=0.5, bio_tundra_alpine=0.25,
                 place_abundance=0.4, place_spacing_m=5.0, place_cluster=0.4,
                 place_elev_max_m=1400)),
    ),
    # --- grassland: a lone tree is the most-looked-at object in the biome ---
    "holm-oak": (
        "12 m - a dense dark evergreen dome on a short trunk",
        base(name="holm-oak", height_m=12.0,
             notes="THE DEHESA TREE, and deliberately the OPPOSITE of the "
                   "shipped `temperate-oak` in the one axis that matters at "
                   "distance: it is a dome, not a vase. Same family, same "
                   "climate band, and a scatter of both across open grass reads "
                   "as two species where a scatter of one reads as a texture.\n\n"
                   "Evergreen, so the crown is dense and dark all year, and it "
                   "stands alone -- wide spacing, low clustering.",
             **t(radius_base_m=0.46, clear_frac=0.22, lean_deg=5.0, wander=0.24,
                 buttress=0.35,
                 shape="sphere", radius_m=5.4, height_frac=0.66,
                 center_frac=0.68, shell_upper=0.80, shell_lower=0.50,
                 asymmetry=0.32, offset=0.18, points=1800,
                 model="colonize", step_m=0.28, influence_m=2.4, kill_m=0.44,
                 gravity=-0.08, inertia=0.42, jitter=0.12, max_iter=300,
                 tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.65, density=0.74, habit="spiral",
                 stretch=2.2, coverage=0.92, separation=1.55,
                 clump_jitter=0.32, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_grassland=1.0, bio_savanna=0.4,
                 bio_temperate_forest=0.35,
                 place_abundance=0.5, place_spacing_m=12.0,
                 place_cluster=0.35, place_elev_max_m=1200)),
    ),
    "stone-pine": (
        "18 m - a bare trunk carrying ONE flat parasol at the very top",
        base(name="stone-pine", height_m=18.0,
             notes="THE PARASOL, AND IT IS THE MOST EXTREME CROWN PROPORTION IN "
                   "THE LIBRARY: `clear_frac` 0.74 with `crown.height_frac` "
                   "0.20 and an umbrella envelope, so four fifths of the tree "
                   "is bare stem and the last fifth is a flat plate wider than "
                   "the tree is tall at the crown. Nothing else here looks "
                   "remotely like it and it needs no colour at all.",
             **t(radius_base_m=0.50, clear_frac=0.74, lean_deg=5.0, wander=0.16,
                 buttress=0.15,
                 shape="umbrella", radius_m=5.6, height_frac=0.20,
                 center_frac=0.90, shell_upper=0.40, shell_lower=0.14,
                 asymmetry=0.28, offset=0.16, points=1600,
                 model="colonize", step_m=0.30, influence_m=2.8, kill_m=0.40,
                 gravity=0.06, inertia=0.42, jitter=0.10, max_iter=300,
                 tip_radius_m=0.06, radius_exp=2.4,
                 clump_radius_m=0.46, density=0.72, habit="tuft",
                 stretch=2.8, coverage=0.94, separation=1.5,
                 clump_jitter=0.30, squash_f=0.55,
                 mat_bark="bark_pale", mat_leaf="leaf_needle",
                 bio_grassland=1.0, bio_beach=0.5, bio_savanna=0.25,
                 place_abundance=0.4, place_spacing_m=10.0,
                 place_cluster=0.45, place_elev_max_m=900)),
    ),
    "quaking-aspen": (
        "18 m - slim white-green trunk, narrow crown, dense clonal stands",
        base(name="quaking-aspen", height_m=18.0,
             notes="IT GROWS IN CLONES AND THAT IS A PLACEMENT STATEMENT: "
                   "`cluster` 0.95 with a 3 m spacing, so a stand is a thicket "
                   "of near-identical slim white poles rather than scattered "
                   "trees. Getting that wrong gives a birch wood.\n\n"
                   "The black scar bands on the white bark are below the "
                   "lattice, so what carries it is the trunk proportion -- "
                   "0.18 m of radius on 18 m of height, the slimmest tree in "
                   "the library -- plus the pale bark and a narrow column "
                   "crown.",
             **t(radius_base_m=0.18, clear_frac=0.48, lean_deg=3.0, wander=0.10,
                 buttress=0.04,
                 shape="column", radius_m=2.0, height_frac=0.50,
                 center_frac=0.76, shell_upper=0.50, shell_lower=0.32,
                 asymmetry=0.26, offset=0.28, points=1200,
                 model="colonize", step_m=0.24, influence_m=1.9, kill_m=0.38,
                 gravity=-0.16, inertia=0.50, jitter=0.10, max_iter=280,
                 tip_radius_m=0.05, radius_exp=2.2,
                 clump_radius_m=0.46, density=0.62, habit="rosette",
                 stretch=2.0, coverage=0.76, separation=1.9,
                 clump_jitter=0.38, squash_f=0.90,
                 mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                 bio_grassland=0.9, bio_taiga=0.7, bio_temperate_forest=0.5,
                 bio_tundra_alpine=0.2,
                 place_abundance=0.9, place_spacing_m=3.0, place_cluster=0.95,
                 place_slope_max_pct=50)),
    ),
    "olive": (
        "8 m - short gnarled multi-stemmed trunk under a small grey crown",
        base(name="olive", height_m=8.0,
             notes="THE GNARL IS THE SPECIES and `trunk.wander` is what says "
                   "it: 0.85, the highest in the library, so the bole wobbles "
                   "hard on its way up rather than running straight. Combined "
                   "with a very short clear length and a heavy root flare, that "
                   "gives the twisted hollow-looking base an old olive has.\n\n"
                   "The crown is small, open and grey-green -- `leaf_dry`, "
                   "which is the nearest the palette has to a grey leaf and is "
                   "warmer than the real thing.",
             **t(radius_base_m=0.42, clear_frac=0.14, lean_deg=9.0, wander=0.85,
                 buttress=0.55,
                 shape="sphere", radius_m=3.0, height_frac=0.70,
                 center_frac=0.66, shell_upper=0.55, shell_lower=0.35,
                 asymmetry=0.50, offset=0.32, points=1100,
                 model="colonize", step_m=0.22, influence_m=1.9, kill_m=0.36,
                 gravity=-0.04, inertia=0.34, jitter=0.20, max_iter=280,
                 tip_radius_m=0.05, radius_exp=2.1,
                 clump_radius_m=0.42, density=0.56, habit="opposite",
                 stretch=2.0, coverage=0.80, separation=1.8,
                 clump_jitter=0.45, squash_f=0.85,
                 mat_bark="bark_pale", mat_leaf="leaf_dry",
                 bio_grassland=1.0, bio_savanna=0.35, bio_desert=0.25,
                 place_abundance=0.35, place_spacing_m=8.0, place_cluster=0.5,
                 place_elev_max_m=900)),
    ),
    "common-ash": (
        "20 m - straight trunk, high open crown, branch tips curling UP",
        base(name="common-ash", height_m=20.0,
             notes="ASCENDING BRANCHES THAT CURL UP AT THE TIPS, which is "
                   "`growth.gravity` POSITIVE -- 0.14 here, where nearly every "
                   "broadleaf in the library is negative. That single sign flip "
                   "is what makes an ash silhouette read as reaching upward "
                   "where an oak's reads as spreading, and it is visible on a "
                   "bare winter tree at two hundred metres.\n\n"
                   "`opposite` habit, which is the decussate pairing an ash and "
                   "a maple share and the oaks do not.",
             **t(radius_base_m=0.44, clear_frac=0.42, lean_deg=3.0, wander=0.12,
                 buttress=0.20,
                 shape="vase", radius_m=4.6, height_frac=0.54,
                 center_frac=0.74, shell_upper=0.55, shell_lower=0.28,
                 asymmetry=0.34, offset=0.30, points=1500,
                 model="colonize", step_m=0.30, influence_m=2.6, kill_m=0.46,
                 gravity=0.14, inertia=0.50, jitter=0.08, max_iter=300,
                 tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.60, density=0.60, habit="opposite",
                 stretch=2.2, coverage=0.80, separation=1.9,
                 clump_jitter=0.35, squash_f=0.90,
                 mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                 bio_temperate_forest=0.9, bio_grassland=0.8,
                 place_abundance=0.6, place_spacing_m=8.0, place_cluster=0.5,
                 place_elev_max_m=1000)),
    ),
    # --- savanna: silhouette against a big sky is nearly the whole job ------
    "marula": (
        "10 m - a rounded dense crown as wide as the tree is tall",
        base(name="marula", height_m=10.0,
             notes="THE ROUND ONE, and savanna needs it: the biome's four "
                   "shipped trees are a flat-topped acacia, a fat baobab, a "
                   "dead snag and a low scrub, and there is no ordinary "
                   "well-behaved dome among them. A crown radius equal to the "
                   "tree's own height on a single short trunk, mottled pale "
                   "bark, and dense enough to cast a real shadow.",
             **t(radius_base_m=0.42, clear_frac=0.28, lean_deg=4.0, wander=0.22,
                 buttress=0.30,
                 shape="sphere", radius_m=5.0, height_frac=0.68,
                 center_frac=0.68, shell_upper=0.70, shell_lower=0.42,
                 asymmetry=0.34, offset=0.20, points=1700,
                 model="colonize", step_m=0.28, influence_m=2.4, kill_m=0.44,
                 gravity=-0.06, inertia=0.42, jitter=0.12, max_iter=300,
                 tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.62, density=0.68, habit="spiral",
                 stretch=2.2, coverage=0.88, separation=1.65,
                 clump_jitter=0.34, squash_f=0.88,
                 mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                 bio_savanna=1.0, bio_grassland=0.35,
                 place_abundance=0.5, place_spacing_m=14.0,
                 place_cluster=0.4, place_slope_max_pct=45)),
    ),
    "jackalberry": (
        "18 m - the big riverine tree: a tall dense dark evergreen dome",
        base(name="jackalberry", height_m=18.0,
             notes="THE TALLEST TREE IN THE SAVANNA and a river tree, so it is "
                   "placed against water (`water_max_m` 60) rather than "
                   "scattered across the plain -- which is what makes a "
                   "watercourse visible from a kilometre away as a dark line of "
                   "big trees. A heavy fluted trunk with a strong root flare "
                   "under a very dense dark crown.",
             **t(radius_base_m=0.70, clear_frac=0.34, lean_deg=3.0, wander=0.16,
                 buttress=0.55,
                 shape="ovoid", radius_m=6.0, height_frac=0.62,
                 center_frac=0.70, shell_upper=0.78, shell_lower=0.45,
                 asymmetry=0.30, offset=0.22, points=1900,
                 model="colonize", step_m=0.30, influence_m=2.6, kill_m=0.46,
                 gravity=-0.10, inertia=0.44, jitter=0.10, max_iter=300,
                 tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.72, density=0.74, habit="spiral",
                 stretch=2.4, coverage=0.92, separation=1.6,
                 clump_jitter=0.32, squash_f=0.82,
                 mat_bark="bark", mat_leaf="leaf_jungle",
                 bio_savanna=1.0, bio_rainforest=0.25,
                 place_abundance=0.35, place_spacing_m=16.0,
                 place_cluster=0.7, place_water_max_m=60)),
    ),
    "fever-tree": (
        "12 m - an acacia shape in unmistakable powdery lime-yellow bark",
        base(name="fever-tree", height_m=12.0,
             notes="THE BARK IS THE IDENTIFICATION AND THE PALETTE ALMOST "
                   "CANNOT DO IT. A fever tree is smooth powdery lime-yellow on "
                   "trunk AND branches, and the wood materials are bark, PALE "
                   "bark, heartwood and deadwood -- none of them yellow-green. "
                   "`bark_pale` is used, which is a white-silver and gets the "
                   "brightness right and the hue wrong. A yellow-green bark is "
                   "a one-row materials ask and this species is the argument "
                   "for it.\n\n"
                   "Shape-wise it is an acacia grown taller and finer than the "
                   "shipped one, and it grows in STANDS near water, which is "
                   "the other half of how it is recognised.",
             **t(radius_base_m=0.32, clear_frac=0.46, lean_deg=4.0, wander=0.14,
                 buttress=0.15,
                 shape="umbrella", radius_m=4.4, height_frac=0.36,
                 center_frac=0.80, shell_upper=0.42, shell_lower=0.18,
                 asymmetry=0.30, offset=0.20, points=1800,
                 model="colonize", step_m=0.24, influence_m=2.4, kill_m=0.34,
                 gravity=0.04, inertia=0.44, jitter=0.10, max_iter=300,
                 tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.32, density=0.58, habit="distichous",
                 stretch=2.6, coverage=0.88, separation=1.7,
                 clump_jitter=0.34, squash_f=0.60,
                 mat_bark="bark_pale", mat_leaf="leaf_dry",
                 bio_savanna=1.0, bio_grassland=0.2,
                 place_abundance=0.4, place_spacing_m=8.0, place_cluster=0.9,
                 place_water_max_m=50)),
    ),
    "mopane": (
        "10 m - crooked trunk, sparse open crown, a grey twig cage when bare",
        base(name="mopane", height_m=10.0,
             notes="THE SPARSEST BROADLEAF IN THE LIBRARY at a coverage of "
                   "0.55, and that is the species: a mopane drops its leaves in "
                   "the dry season and what is left is a cage of grey twigs. "
                   "Authored in the sparse phase, because a fully leafed mopane "
                   "is a generic small tree and the open one is not.",
             **t(radius_base_m=0.28, clear_frac=0.26, lean_deg=8.0, wander=0.45,
                 buttress=0.20,
                 shape="ovoid", radius_m=3.2, height_frac=0.66,
                 center_frac=0.68, shell_upper=0.50, shell_lower=0.30,
                 asymmetry=0.48, offset=0.32, points=1300,
                 model="colonize", step_m=0.24, influence_m=2.0, kill_m=0.38,
                 gravity=-0.04, inertia=0.36, jitter=0.18, max_iter=280,
                 tip_radius_m=0.05, radius_exp=2.1,
                 clump_radius_m=0.34, density=0.44, habit="opposite",
                 stretch=2.0, coverage=0.55, separation=2.1,
                 clump_jitter=0.45, squash_f=0.85,
                 mat_bark="bark", mat_leaf="leaf_dry",
                 bio_savanna=1.0, bio_desert=0.3,
                 place_abundance=0.7, place_spacing_m=7.0, place_cluster=0.75,
                 place_slope_max_pct=45)),
    ),
    "doum-palm": (
        "12 m - THE ONLY PALM THAT FORKS: two forks, four fan crowns",
        base(name="doum-palm", height_m=12.0,
             notes="THE FORK IS THE WHOLE IDENTITY AND THE `frond` MODEL CANNOT "
                   "FORK. `frond` is an unbranched trunk carrying a crown of "
                   "arcing leaves, by definition. So this is built on `whorl` "
                   "instead with a very small number of very long branches "
                   "leaving high up -- three rings of two -- which gives a stem "
                   "that splits and splits again, each arm ending in a tuft. "
                   "That is the doum's structure got from the wrong model, and "
                   "it is the nearest the generator reaches.\n\n"
                   "A true forking palm would be a `frond` variant that allows "
                   "a branching trunk. Recorded as the feature request it is.",
             **t(radius_base_m=0.30, clear_frac=0.42, lean_deg=6.0, wander=0.18,
                 buttress=0.20,
                 shape="umbrella", radius_m=3.0, height_frac=0.46,
                 center_frac=0.76, shell_upper=0.45, shell_lower=0.30,
                 asymmetry=0.30, offset=0.16, points=700,
                 model="whorl", step_m=0.34, influence_m=3.0, kill_m=0.7,
                 gravity=-0.02, inertia=0.68, jitter=0.06, max_iter=280,
                 tip_radius_m=0.07, radius_exp=2.8,
                 whorl_count=3, whorl_branches=2, whorl_droop=0.30,
                 whorl_rise=0.55, whorl_sub=1, whorl_sub_angle=42.0,
                 whorl_irregular=0.35, whorl_leader=0.10,
                 clump_radius_m=0.85, density=0.70, habit="tuft",
                 stretch=2.4, coverage=1.0, separation=1.5,
                 clump_jitter=0.25, squash_f=0.55,
                 mat_bark="bark_pale", mat_leaf="leaf_dry",
                 bio_savanna=0.9, bio_desert=0.8,
                 place_abundance=0.3, place_spacing_m=8.0, place_cluster=0.7,
                 place_water_max_m=40)),
    ),
    # --- desert: the list is short on purpose -------------------------------
    "date-palm": (
        "18 m - a scarred column under an arching fountain of stiff fronds",
        base(name="date-palm", height_m=18.0,
             notes="THE TALLEST PALM IN THE LIBRARY and deliberately STIFFER "
                   "than the shipped `coast-palm`: `frond.rise` 1.25 against "
                   "0.85 and a much lower droop, so the crown is an upright "
                   "fountain rather than a spreading spray. A coconut palm "
                   "leans and hangs; a date palm stands.\n\n"
                   "One of the two silhouettes the desert file says say "
                   "'desert' from a kilometre away, and it places against the "
                   "biome's three water exceptions rather than across sand.",
             **t(radius_base_m=0.34, clear_frac=0.82, lean_deg=3.0, wander=0.10,
                 buttress=0.25,
                 shape="umbrella", radius_m=4.0, height_frac=0.16,
                 center_frac=0.93, shell_upper=0.60, shell_lower=0.40,
                 asymmetry=0.18, offset=0.08, points=120,
                 model="frond", step_m=0.55, influence_m=6.5, kill_m=1.7,
                 gravity=-0.30, inertia=0.70, max_iter=260,
                 tip_radius_m=0.06, radius_exp=2.6,
                 frond_count=22, frond_length_m=3.6, frond_width_m=0.34,
                 frond_rise=1.25, frond_droop=0.75, frond_dead=0.22,
                 frond_irregular=0.25,
                 clump_radius_m=0.95, density=0.75, habit="pendulous",
                 coverage=1.0, separation=1.7,
                 mat_bark="bark", mat_leaf="leaf_dry",
                 bio_desert=1.0, bio_savanna=0.4, bio_beach=0.5,
                 place_abundance=0.4, place_spacing_m=6.0, place_cluster=0.9,
                 place_water_max_m=25, place_slope_max_pct=25)),
    ),
    "desert-ironwood": (
        "8 m - short thick fluted grey trunk under a dense grey-green cloud",
        base(name="desert-ironwood", height_m=8.0,
             notes="A DARK ROUND MASS AT RANGE, which is what an ironwood is on "
                   "an empty plain, and it is got from a very short bole "
                   "splitting low into many stiff branches: `clear_frac` 0.16 "
                   "with a high branch density and a low crown centre. Denser "
                   "than anything else in the desert set, which is the point -- "
                   "it is the one desert tree that casts real shade.",
             **t(radius_base_m=0.40, clear_frac=0.16, lean_deg=6.0, wander=0.42,
                 buttress=0.45,
                 shape="sphere", radius_m=3.4, height_frac=0.76,
                 center_frac=0.62, shell_upper=0.72, shell_lower=0.48,
                 asymmetry=0.42, offset=0.24, points=1500,
                 model="colonize", step_m=0.22, influence_m=1.9, kill_m=0.34,
                 gravity=-0.02, inertia=0.36, jitter=0.16, max_iter=280,
                 tip_radius_m=0.05, radius_exp=2.1,
                 clump_radius_m=0.40, density=0.68, habit="distichous",
                 stretch=2.2, coverage=0.90, separation=1.55,
                 clump_jitter=0.36, squash_f=0.88,
                 mat_bark="bark_pale", mat_leaf="leaf_dry",
                 bio_desert=1.0, bio_savanna=0.3,
                 place_abundance=0.3, place_spacing_m=16.0,
                 place_cluster=0.4)),
    ),
    "saguaro": (
        "12 m - a fluted green column with arms that bend vertical",
        base(name="saguaro", height_m=12.0,
             notes="THE ARM ARRANGEMENT IS EXACTLY WHAT `whorl` DOES -- "
                   "branches off a single dominant trunk at set heights -- and "
                   "the desert file makes that argument rather than filing this "
                   "under a missing succulent generator. Three rings of one to "
                   "two arms, leaving low and lifting hard (`whorl.rise` 0.95, "
                   "the highest here) so each arm turns vertical a short way "
                   "out and runs alongside the trunk.\n\n"
                   "THE GREEN IS A WORKAROUND FOR A MISSING MATERIAL AND IT IS "
                   "NOT FREE. The wood palette is bark, pale bark, heartwood "
                   "and deadwood -- all browns and greys -- so a bare saguaro "
                   "skeleton reads as a dead tree. This spec clothes the "
                   "columns in a thin tight shell of `leaf_jungle` foliage "
                   "instead: small clumps, full coverage, low separation, "
                   "hugging the wood. It greens the plant at the cost of a soft "
                   "edge where a cactus has a hard one. A GREEN STEM MATERIAL "
                   "IS THE REAL FIX and it would also serve the Joshua tree, "
                   "the ocotillo and every euphorbia in the savanna list.\n\n"
                   "The vertical ribbing is finer than 10 cm and is not "
                   "attempted.",
             **t(radius_base_m=0.36, clear_frac=0.24, lean_deg=1.0, wander=0.05,
                 buttress=0.10,
                 shape="column", radius_m=1.3, height_frac=0.80,
                 center_frac=0.58, shell_upper=0.95, shell_lower=0.95,
                 asymmetry=0.12, offset=0.05, points=400,
                 model="whorl", step_m=0.22, influence_m=2.2, kill_m=0.5,
                 gravity=0.55, inertia=0.80, jitter=0.03, max_iter=300,
                 tip_radius_m=0.16, radius_exp=3.2,
                 whorl_count=3, whorl_branches=2, whorl_droop=0.0,
                 whorl_rise=0.95, whorl_sub=0, whorl_irregular=0.45,
                 whorl_leader=0.28,
                 clump_radius_m=0.24, density=0.92, habit="radial",
                 stretch=3.4, coverage=1.0, separation=1.05,
                 clump_jitter=0.12, squash_f=1.0, droop_m=0.0, rough=0.25,
                 mat_bark="bark", mat_leaf="leaf_jungle",
                 bio_desert=1.0,
                 place_abundance=0.35, place_spacing_m=6.0, place_cluster=0.5,
                 place_slope_max_pct=40)),
    ),
    "joshua-tree": (
        "8 m - a shaggy trunk forking every metre into spiky rosettes",
        base(name="joshua-tree", height_m=8.0,
             notes="REPEATED FORKS ENDING IN SPIKY BALLS, which is `whorl` with "
                   "many rings of few branches and a big foliage clump on each "
                   "tip -- the opposite of the saguaro's few rings of arms held "
                   "vertical. The two share a model and a workaround and look "
                   "nothing alike, which is the point of building both.\n\n"
                   "SAME GREEN WORKAROUND as the saguaro: the columns are "
                   "clothed in `leaf_jungle` because the wood palette has no "
                   "green. See that spec's notes for the materials ask.",
             **t(radius_base_m=0.32, clear_frac=0.18, lean_deg=5.0, wander=0.30,
                 buttress=0.35,
                 shape="ovoid", radius_m=2.6, height_frac=0.80,
                 center_frac=0.58, shell_upper=0.70, shell_lower=0.55,
                 asymmetry=0.40, offset=0.18, points=700,
                 model="whorl", step_m=0.26, influence_m=2.2, kill_m=0.5,
                 gravity=0.22, inertia=0.62, jitter=0.10, max_iter=280,
                 tip_radius_m=0.10, radius_exp=2.8,
                 whorl_count=7, whorl_branches=2, whorl_droop=0.20,
                 whorl_rise=0.60, whorl_sub=1, whorl_sub_angle=48.0,
                 whorl_irregular=0.50, whorl_leader=0.06,
                 clump_radius_m=0.52, density=0.85, habit="tuft",
                 stretch=1.4, coverage=1.0, separation=1.3,
                 clump_jitter=0.25, squash_f=1.0, rough=0.70,
                 mat_bark="deadwood", mat_leaf="leaf_needle",
                 bio_desert=1.0, bio_grassland=0.15,
                 place_abundance=0.3, place_spacing_m=8.0, place_cluster=0.55,
                 place_slope_max_pct=40)),
    ),
    # --- rainforest: the biome is read from inside, at head height ----------
    "kapok": (
        "40 m - pale near-white trunk on plank buttresses, flat crown above all",
        base(name="kapok", height_m=40.0,
             notes="THE BIGGEST SILHOUETTE IN THE RAINFOREST, and its identity "
                   "is at the BOTTOM rather than the top: thin plank buttresses "
                   "standing out from the base like walls. `trunk.buttress` at "
                   "1.0 thickens the cylinder and `roots.count` 8 with a low "
                   "arch and a long reach draws the planks themselves, and it "
                   "is the pair that gets it -- buttress alone is a fat trunk.\n\n"
                   "AUTHORED AT 40 m AGAINST THE BIOME LIST'S 45, and the "
                   "reason is cost rather than botany: at 10 cm a 45 m tree "
                   "with an 11 m crown is a large grid and this is a scenery "
                   "tree rather than a hero. Recorded so the number is a "
                   "decision.\n\n"
                   "Pale grey-white bark and a flat umbrella crown held above "
                   "everything else, on a bole clear for two thirds of its "
                   "height.",
             **t(radius_base_m=1.10, clear_frac=0.66, lean_deg=2.0, wander=0.08,
                 buttress=1.0,
                 shape="umbrella", radius_m=9.5, height_frac=0.28,
                 center_frac=0.86, shell_upper=0.42, shell_lower=0.12,
                 asymmetry=0.36, offset=0.24, points=2200,
                 model="colonize", step_m=0.42, influence_m=3.8, kill_m=0.80,
                 gravity=-0.04, inertia=0.46, jitter=0.08, max_iter=320,
                 tip_radius_m=0.07, radius_exp=2.4,
                 roots_count=8, roots_length_m=3.6, roots_rise=0.25,
                 roots_thickness=0.42, roots_irregular=0.35,
                 clump_radius_m=1.0, density=0.60, habit="spiral",
                 stretch=2.4, coverage=0.86, separation=1.7,
                 clump_jitter=0.34, squash_f=0.55,
                 mat_bark="bark_pale", mat_leaf="leaf_jungle",
                 bio_rainforest=1.0,
                 place_abundance=0.2, place_spacing_m=25.0,
                 place_cluster=0.4, place_slope_max_pct=45)),
    ),
    "cecropia": (
        "15 m - a candelabra: a few thick straight branches, very few leaves",
        base(name="cecropia", height_m=15.0,
             notes="A CANDELABRA IS A WHORL TREE WITH ALMOST NOTHING ON IT: "
                   "four rings of three thick branches leaving nearly "
                   "horizontally and lifting at the tips, each carrying ONE "
                   "large foliage clump and no sub-branching at all "
                   "(`whorl.sub` 0). Coverage 1.0 on a separation of 2.6, so "
                   "there are a dozen big plates and daylight everywhere else. "
                   "That is the opposite of how every other tree here is "
                   "authored and it is exactly what a cecropia looks like.",
             **t(radius_base_m=0.22, clear_frac=0.40, lean_deg=3.0, wander=0.10,
                 buttress=0.30,
                 shape="umbrella", radius_m=3.4, height_frac=0.50,
                 center_frac=0.76, shell_upper=0.60, shell_lower=0.40,
                 asymmetry=0.30, offset=0.18, points=500,
                 model="whorl", step_m=0.30, influence_m=2.6, kill_m=0.6,
                 gravity=0.10, inertia=0.66, jitter=0.06, max_iter=280,
                 tip_radius_m=0.08, radius_exp=2.7,
                 whorl_count=4, whorl_branches=3, whorl_droop=0.15,
                 whorl_rise=0.30, whorl_sub=0, whorl_irregular=0.30,
                 whorl_leader=0.05,
                 clump_radius_m=1.0, density=0.62, habit="radial",
                 stretch=1.2, coverage=1.0, separation=2.6,
                 clump_jitter=0.22, squash_f=0.45,
                 mat_bark="bark_pale", mat_leaf="leaf_jungle",
                 bio_rainforest=1.0,
                 place_abundance=0.5, place_spacing_m=7.0, place_cluster=0.6)),
    ),
    "tree-fern": (
        "6 m - a slender fibrous trunk with NO taper under a flat rosette",
        base(name="tree-fern", height_m=6.0,
             notes="NO TAPER IS THE SPECIES. A tree fern's trunk is the same "
                   "width top to bottom -- it is a mass of old frond bases, not "
                   "wood -- so `radius_exp` is 3.5, the top of the range, which "
                   "keeps the parent thick all the way up, and the root flare "
                   "is zero. Everything else here tapers.\n\n"
                   "A `frond` crown that is FLAT rather than a fountain: rise "
                   "0.35 with a low droop, so the fronds go out rather than up. "
                   "Head-height understorey, which is the layer the rainforest "
                   "file says the biome is missing.",
             **t(radius_base_m=0.13, clear_frac=0.72, lean_deg=3.0, wander=0.10,
                 buttress=0.0,
                 shape="umbrella", radius_m=2.2, height_frac=0.22,
                 center_frac=0.90, shell_upper=0.60, shell_lower=0.40,
                 asymmetry=0.20, offset=0.08, points=100,
                 model="frond", step_m=0.40, influence_m=4.0, kill_m=1.2,
                 gravity=-0.20, inertia=0.70, max_iter=260,
                 tip_radius_m=0.05, radius_exp=3.5,
                 frond_count=13, frond_length_m=2.0, frond_width_m=0.42,
                 frond_rise=0.35, frond_droop=0.55, frond_dead=0.30,
                 frond_irregular=0.28,
                 clump_radius_m=0.60, density=0.72, habit="pendulous",
                 coverage=1.0, separation=1.7,
                 mat_bark="deadwood", mat_leaf="leaf_jungle",
                 bio_rainforest=1.0, bio_temperate_forest=0.2,
                 place_abundance=0.6, place_spacing_m=3.5, place_cluster=0.85,
                 place_water_max_m=80)),
    ),
    "wild-banana": (
        "5 m - no wood at all: a fat green pseudostem and huge torn paddles",
        base(name="wild-banana", height_m=5.0,
             notes="NOT A TREE AND FILED AS ONE, because `frond` is what builds "
                   "it: an unbranched stem carrying a crown of very long very "
                   "wide blades. Six fronds at 1.1 m of half-width, which is "
                   "double the width the palm uses -- the blade rasterizer's "
                   "own rule is that blades must be narrower than their spacing "
                   "or the crown closes into a disc, so six is the count that "
                   "keeps them separate at that width.\n\n"
                   "The pseudostem is green and the wood palette is not, so it "
                   "is drawn in `deadwood` -- the palest, greyest option -- and "
                   "the crown carries the colour. Same gap the saguaro records.",
             **t(radius_base_m=0.22, clear_frac=0.44, lean_deg=5.0, wander=0.12,
                 buttress=0.30,
                 shape="umbrella", radius_m=2.4, height_frac=0.40,
                 center_frac=0.78, shell_upper=0.60, shell_lower=0.40,
                 asymmetry=0.25, offset=0.10, points=90,
                 model="frond", step_m=0.35, influence_m=3.5, kill_m=1.0,
                 gravity=-0.35, inertia=0.70, max_iter=260,
                 tip_radius_m=0.06, radius_exp=3.0,
                 frond_count=6, frond_length_m=2.2, frond_width_m=1.10,
                 frond_rise=0.95, frond_droop=1.35, frond_dead=0.25,
                 frond_irregular=0.35,
                 clump_radius_m=0.70, density=0.80, habit="pendulous",
                 coverage=1.0, separation=1.7,
                 mat_bark="deadwood", mat_leaf="leaf_jungle",
                 bio_rainforest=1.0,
                 place_abundance=0.5, place_spacing_m=3.0, place_cluster=0.9,
                 place_water_max_m=60)),
    ),
    # --- beach: the band that wraps every shore in the world ----------------
    "casuarina": (
        "15 m - a dark smudge, not a crown: fine drooping wind-combed needles",
        base(name="casuarina", height_m=15.0,
             notes="THE SILHOUETTE IS A SMUDGE AND THAT IS AUTHORED, not a "
                   "failure: very small clumps at a very long stretch (5.0, the "
                   "highest here) with a heavy droop, so the crown has no "
                   "readable edge anywhere. Every other tree in the library is "
                   "authored to have an outline; this one is authored not to.\n\n"
                   "Wind-combed to one side, which is `crown.lean_deg` 16 -- "
                   "the crown leaning off the trunk axis rather than the trunk "
                   "leaning.",
             **t(radius_base_m=0.34, clear_frac=0.36, lean_deg=8.0, wander=0.14,
                 buttress=0.15, lean_deg_crown=16.0,
                 shape="ovoid", radius_m=3.4, height_frac=0.62,
                 center_frac=0.72, shell_upper=0.55, shell_lower=0.35,
                 asymmetry=0.42, offset=0.34, points=1500,
                 model="colonize", step_m=0.26, influence_m=2.2, kill_m=0.40,
                 gravity=-0.34, inertia=0.48, jitter=0.10, max_iter=300,
                 tip_radius_m=0.05, radius_exp=2.2,
                 clump_radius_m=0.24, density=0.64, habit="pendulous",
                 stretch=5.0, coverage=0.92, separation=1.5,
                 clump_jitter=0.34, squash_f=0.65, droop_m=0.40,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_beach=1.0, bio_savanna=0.3, bio_rainforest=0.2,
                 place_abundance=0.5, place_spacing_m=6.0, place_cluster=0.75,
                 place_elev_max_m=60)),
    ),
    "monterey-cypress": (
        "12 m - a flat-topped crown shorn to one side by wind, trunk leaning",
        base(name="monterey-cypress", height_m=12.0,
             notes="THE ASYMMETRY IS THE SPECIES and it takes THREE parameters "
                   "pulling the same way: `trunk.lean_deg` 16, "
                   "`crown.lean_deg` 28 and `crown.offset` 0.60 -- the trunk "
                   "leans, the crown leans further off it, and then slides "
                   "sideways again. Nothing shipped uses all three, and a "
                   "wind-shorn tree that is only slightly lopsided reads as a "
                   "mistake rather than as weather.\n\n"
                   "Flat on top, because the wind takes the leading edge off: "
                   "an umbrella envelope with a very thin lower shell.",
             **t(radius_base_m=0.44, clear_frac=0.30, lean_deg=16.0,
                 wander=0.28, buttress=0.35, lean_deg_crown=28.0,
                 shape="umbrella", radius_m=4.6, height_frac=0.44,
                 center_frac=0.76, shell_upper=0.60, shell_lower=0.18,
                 asymmetry=0.65, offset=0.60, points=1600,
                 model="colonize", step_m=0.26, influence_m=2.2, kill_m=0.40,
                 gravity=-0.06, inertia=0.44, jitter=0.14, max_iter=300,
                 tip_radius_m=0.05, radius_exp=2.3,
                 clump_radius_m=0.44, density=0.70, habit="distichous",
                 stretch=2.8, coverage=0.90, separation=1.55,
                 clump_jitter=0.36, squash_f=0.55,
                 mat_bark="bark", mat_leaf="leaf_needle",
                 bio_beach=1.0, bio_temperate_forest=0.25,
                 place_abundance=0.35, place_spacing_m=8.0, place_cluster=0.5,
                 place_elev_max_m=120, place_slope_max_pct=55)),
    ),
    "sea-grape": (
        "5 m - low, wide, multi-stemmed, with big round leathery leaves",
        base(name="sea-grape", height_m=5.0,
             notes="WIDER THAN IT IS TALL ON A BEACH, which is the shape wind "
                   "and salt make of anything that tries to grow at the "
                   "strandline: a crown radius of 3.6 m on a 5 m plant, "
                   "branching from just above the ground. The big round leaves "
                   "are drawn as unusually large clumps (0.75 m) on a small "
                   "tree, which is the closest the lattice gets to leaf size.",
             **t(radius_base_m=0.20, clear_frac=0.10, lean_deg=10.0,
                 wander=0.45, buttress=0.30,
                 shape="umbrella", radius_m=3.6, height_frac=0.80,
                 center_frac=0.58, shell_upper=0.70, shell_lower=0.50,
                 asymmetry=0.50, offset=0.30, points=1200,
                 model="colonize", step_m=0.20, influence_m=1.8, kill_m=0.34,
                 gravity=-0.04, inertia=0.34, jitter=0.18, max_iter=260,
                 tip_radius_m=0.05, radius_exp=2.1,
                 clump_radius_m=0.75, density=0.72, habit="spiral",
                 stretch=1.6, coverage=0.90, separation=1.6,
                 clump_jitter=0.34, squash_f=0.75,
                 mat_bark="bark_pale", mat_leaf="leaf_jungle",
                 bio_beach=1.0, bio_rainforest=0.2,
                 place_abundance=0.6, place_spacing_m=4.0, place_cluster=0.8,
                 place_elev_max_m=25)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "tree specs")
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
