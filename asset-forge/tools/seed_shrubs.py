"""Author thirty shrubs: the layer between the ground cover and the canopy.

WHY THE MIDDLE DISTANCE MATTERS. The library had FOUR bush specs for ten
biomes, and a shrub is what fills the band a player walks through: too tall to
step over, too short to be a tree. The desert file puts it directly -- scattered
low grey mounds at wide spacing are what an arid plain actually looks like, and
they fill the middle distance that trees cannot. The temperate-forest file wants
the same layer for the opposite reason: a wood with a canopy and a floor and
nothing between them reads as a park.

A BUSH IS THE TREE MACHINERY AUTHORED SHORT WITH BRANCHES TO THE GROUND
(`forge/kinds.py:72-75`), and the three settings that make it one rather than a
small tree are `trunk.clear_frac` near zero, a crown centred low, and a trunk
radius small enough that no single stem dominates. Every spec here does all
three.

FIVE CENTIMETRES, not the trees' ten. A bush is a DETAIL entity -- it carries
its own grid and its own transform and never joins the terrain lattice -- so it
is free to be finer, and at 10 cm a knee-high shrub is five voxels tall with
nothing in it. That is the same split `forge/kinds.py:29-58` states and the
reason `bush` is a separate kind from `tree` at all.

WHAT THE THIRTY ARE FOR. Not coverage of botany: each one is a distinct
SILHOUETTE at ten to sixty voxels, and the axes they spread along are

  * DENSITY -- a box or a rhododendron reads as a solid mass; a creosote bush
    or a mopane-country shrub is more twig than leaf and you see through it.
  * HABIT -- upright bundles (broom, ocotillo), arching canes (dog rose,
    bramble), flat mats (dwarf pine, arctic willow), tight domes (saltbush).
  * COLOUR -- the palette's greys carry the arid species and its greens the
    wet ones, and `leaf_dry` doing duty as a grey-green is the weakest part of
    that. A true grey-silver foliage material would improve eight of these.

    python tools/seed_shrubs.py
    python tools/seed_shrubs.py --force

SIZES ARE APPROXIMATE. Every height is the approximate figure from the biome
file it came from; those are unsourced general-knowledge estimates by their own
admission. Where a spec departs from the listed size the reason is in its
`notes`.
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def base(**over):
    changes = {
        "kind": "bush",
        "resolution_cm": "5",
        "variation.amount": 1.0,
        "variation.height": 0.24,
        "variation.crown_radius": 0.24,
        "variation.trunk_radius": 0.20,
        "variation.shape": 0.16,
        "variation.proportion": 0.26,
        "variation.lean_deg": 10.0,
        "variation.rotate": True,
        # A bush has no bare bole, so the crown starts at the ground and the
        # growth targets have to reach down to it.
        "trunk.clear_frac": 0.05,
    }
    changes.update(over)
    return changes


def t(**kw):
    groups = {
        "radius_base_m": "trunk", "clear_frac": "trunk", "lean_deg": "trunk",
        "wander": "trunk", "buttress": "trunk",
        "shape": "crown", "radius_m": "crown", "height_frac": "crown",
        "center_frac": "crown", "shell_upper": "crown",
        "shell_lower": "crown", "squash": "crown", "asymmetry": "crown",
        "offset": "crown", "points": "crown",
        "model": "growth", "step_m": "growth", "influence_m": "growth",
        "kill_m": "growth", "gravity": "growth", "phototropism": "growth",
        "inertia": "growth", "jitter": "growth", "max_iter": "growth",
        "shade": "growth", "tip_radius_m": "growth", "radius_exp": "growth",
        "enabled": "foliage", "min_order": "foliage",
        "clump_radius_m": "foliage", "density": "foliage", "rough": "foliage",
        "habit": "foliage", "stretch": "foliage", "clustering": "foliage",
        "top_bias": "foliage", "coverage": "foliage", "separation": "foliage",
        "clump_jitter": "foliage", "droop_m": "foliage",
    }
    out = {}
    for k, v in kw.items():
        if k.startswith("mat_"):
            out["materials." + k[len("mat_"):]] = v
        elif k.startswith("bio_"):
            out["biomes." + k[len("bio_"):]] = v
        elif k.startswith("place_"):
            out["placement." + k[len("place_"):]] = v
        elif k.startswith("strand_"):
            out["strand." + k[len("strand_"):]] = v
        elif k == "squash_f":
            out["foliage.squash"] = v
        else:
            out[f"{groups[k]}.{k}"] = v
    return out


def mound(**over):
    """The default shrub: low, wide, branching from the ground, no clear bole."""
    d = dict(clear_frac=0.05, shape="sphere", center_frac=0.52,
             height_frac=0.92, model="colonize", tip_radius_m=0.05,
             radius_exp=2.0, max_iter=260, habit="spiral", stretch=2.0,
             squash_f=0.85, shell_upper=0.75, shell_lower=0.60)
    d.update(over)
    return t(**d)


SPECIES = {
    # --- grassland and heath -------------------------------------------------
    "gorse": (
        "1.80 m - a dense spiny mound in solid yellow bloom",
        base(name="gorse", height_m=1.80,
             notes="THE SPINES ARE THE LEAVES, so there is no distinction "
                   "between wood and foliage to draw: this is authored as the "
                   "densest shrub in the set (coverage 0.98 at separation 1.3) "
                   "so it reads as one solid mass with no branch structure "
                   "visible at all. The blaze of yellow is `leaf_dry`, which is "
                   "the nearest the LEAF palette has -- the bright yellow that "
                   "the flowers now use lives on the tuft head menu and not "
                   "here, which is a real asymmetry worth closing.",
             **mound(radius_base_m=0.06, radius_m=0.95, lean_deg=4.0,
                     wander=0.35, asymmetry=0.35, offset=0.16, points=1400,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=0.05,
                     inertia=0.34, jitter=0.16,
                     clump_radius_m=0.14, density=0.80, coverage=0.98,
                     separation=1.30, clump_jitter=0.30,
                     mat_bark="bark", mat_leaf="leaf_dry",
                     bio_grassland=1.0, bio_beach=0.5,
                     bio_temperate_forest=0.3,
                     place_abundance=0.6, place_spacing_m=2.0,
                     place_cluster=0.9, place_slope_max_pct=55)),
    ),
    "common-broom": (
        "2.00 m - an upright bundle of green whippy stems, nearly leafless",
        base(name="common-broom", height_m=2.00,
             notes="A BUNDLE OF VERTICALS, which is the opposite of the gorse "
                   "beside it on the same heath: very high `inertia` and "
                   "`phototropism` keep the stems straight and pointing up, and "
                   "the coverage is halved so the wood shows. The two share a "
                   "biome and must not read as one plant.",
             **mound(radius_base_m=0.05, radius_m=0.55, lean_deg=3.0,
                     wander=0.18, shape="column", height_frac=0.94,
                     center_frac=0.52, asymmetry=0.22, offset=0.10,
                     points=1000,
                     step_m=0.10, influence_m=0.8, kill_m=0.18, gravity=0.20,
                     inertia=0.70, phototropism=0.45, jitter=0.08,
                     clump_radius_m=0.11, density=0.55, coverage=0.55,
                     separation=1.8, clump_jitter=0.35,
                     mat_bark="bark", mat_leaf="leaf_dry",
                     bio_grassland=1.0, bio_temperate_forest=0.3,
                     place_abundance=0.5, place_spacing_m=2.0,
                     place_cluster=0.8)),
    ),
    "big-sagebrush": (
        "1.50 m - a silver-grey mound on a short twisted woody trunk",
        base(name="big-sagebrush", height_m=1.50,
             notes="THE DEFINING SHRUB OF COLD SEMI-DESERT STEPPE, which in "
                   "this world is grassland -- the desert gate needs 24 C and "
                   "everything arid and cool falls to grassland instead. So "
                   "this carries a grassland weight first and a desert one "
                   "second, which is the opposite of how it reads on a map and "
                   "is correct for this engine.\n\n"
                   "A visible short twisted trunk under a grey mound: "
                   "`clear_frac` 0.18, the highest in this file, because a "
                   "sagebrush is the one shrub here that has a bole worth "
                   "seeing.",
             **mound(radius_base_m=0.08, radius_m=0.75, clear_frac=0.18,
                     lean_deg=7.0, wander=0.55, asymmetry=0.42, offset=0.22,
                     points=1100,
                     step_m=0.09, influence_m=0.8, kill_m=0.18, gravity=0.02,
                     inertia=0.32, jitter=0.20,
                     clump_radius_m=0.16, density=0.62, coverage=0.80,
                     separation=1.6, clump_jitter=0.38,
                     mat_bark="bark_pale", mat_leaf="leaf_dry",
                     bio_grassland=1.0, bio_desert=0.5, bio_savanna=0.2,
                     place_abundance=0.8, place_spacing_m=2.5,
                     place_cluster=0.5, place_slope_max_pct=50)),
    ),
    "rubber-rabbitbrush": (
        "1.20 m - a pale blue-green ball topped by a flat yellow mass",
        base(name="rubber-rabbitbrush", height_m=1.20,
             notes="A TWO-LAYER PLANT, which the generator gets from crown "
                   "geometry rather than from two materials: an `umbrella` "
                   "envelope with a very thin lower shell puts nearly all the "
                   "foliage in a flat plate on top of a bare-ish ball. The "
                   "yellow is that plate.",
             **mound(radius_base_m=0.06, radius_m=0.70, lean_deg=4.0,
                     wander=0.30, shape="umbrella", height_frac=0.80,
                     center_frac=0.62, shell_upper=0.60, shell_lower=0.20,
                     asymmetry=0.30, offset=0.14, points=1000,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=0.08,
                     inertia=0.42, jitter=0.14,
                     clump_radius_m=0.14, density=0.66, coverage=0.88,
                     separation=1.5, clump_jitter=0.30,
                     mat_bark="bark_pale", mat_leaf="leaf_dry",
                     bio_grassland=1.0, bio_desert=0.5,
                     place_abundance=0.6, place_spacing_m=2.0,
                     place_cluster=0.6)),
    ),
    "dog-rose": (
        "2.50 m - long arching thorny canes with scarlet flask-shaped hips",
        base(name="dog-rose", height_m=2.50,
             notes="ARCHING CANES, which is a strong NEGATIVE gravity on a "
                   "sparse crown -- the branches leave the base and fall away, "
                   "and there are few enough of them to see each one. The hips "
                   "are the species and there is no fruit primitive, so the "
                   "foliage is drawn in `leaf_autumn` at a low coverage, which "
                   "reads as scarlet dots in an open bush. Same substitution "
                   "`rowan` records and the same feature request behind it.",
             **mound(radius_base_m=0.05, radius_m=1.20, lean_deg=6.0,
                     wander=0.40, shape="vase", height_frac=0.88,
                     center_frac=0.56, asymmetry=0.50, offset=0.28,
                     points=900,
                     step_m=0.11, influence_m=0.9, kill_m=0.20, gravity=-0.30,
                     inertia=0.55, jitter=0.14,
                     clump_radius_m=0.13, density=0.50, coverage=0.62,
                     separation=2.0, clump_jitter=0.40,
                     mat_bark="bark", mat_leaf="leaf_autumn",
                     bio_grassland=0.9, bio_temperate_forest=0.7,
                     bio_beach=0.3,
                     place_abundance=0.5, place_spacing_m=2.5,
                     place_cluster=0.6)),
    ),
    "blackthorn-scrub": (
        "3.00 m - a dense black thorny suckering thicket",
        base(name="blackthorn-scrub", height_m=3.00,
             notes="A THICKET RATHER THAN A BUSH, and the difference is "
                   "placement: 1.2 m spacing at maximum clustering, so a patch "
                   "is impassable and has a hard edge. The wood is dark and "
                   "shows through a thin leaf cover, which is what makes a "
                   "blackthorn hedge look black in winter.",
             **mound(radius_base_m=0.06, radius_m=1.00, lean_deg=5.0,
                     wander=0.45, shape="ovoid", height_frac=0.92,
                     center_frac=0.54, asymmetry=0.45, offset=0.24,
                     points=1500,
                     step_m=0.10, influence_m=0.8, kill_m=0.17, gravity=0.02,
                     inertia=0.36, jitter=0.20,
                     clump_radius_m=0.13, density=0.60, coverage=0.72,
                     separation=1.7, clump_jitter=0.40,
                     mat_bark="bark", mat_leaf="leaf_broadleaf",
                     bio_grassland=0.9, bio_temperate_forest=0.8,
                     place_abundance=0.7, place_spacing_m=1.2,
                     place_cluster=1.0)),
    ),
    "sea-buckthorn": (
        "2.50 m - grey thorny thicket, silver leaves, heavy orange berries",
        base(name="sea-buckthorn", height_m=2.50,
             notes="SILVER LEAVES AND ORANGE BERRIES AT ONCE, and one leaf "
                   "material has to be both. `leaf_dry` is chosen because it is "
                   "the pale one and the berries are the more distinctive of "
                   "the two -- so the plant reads pale-tawny rather than silver "
                   "or orange, which is a compromise and is recorded.\n\n"
                   "Upright, dense and thorny; it binds dunes, so it carries a "
                   "beach weight first.",
             **mound(radius_base_m=0.07, radius_m=0.95, lean_deg=5.0,
                     wander=0.35, shape="ovoid", height_frac=0.90,
                     center_frac=0.56, asymmetry=0.38, offset=0.20,
                     points=1300,
                     step_m=0.10, influence_m=0.8, kill_m=0.18, gravity=0.06,
                     inertia=0.42, jitter=0.16,
                     clump_radius_m=0.13, density=0.66, coverage=0.82,
                     separation=1.6, clump_jitter=0.36,
                     mat_bark="bark_pale", mat_leaf="leaf_dry",
                     bio_beach=1.0, bio_grassland=0.6,
                     place_abundance=0.6, place_spacing_m=1.8,
                     place_cluster=0.9, place_elev_max_m=60)),
    ),
    "snowberry": (
        "1.50 m - loose arching twiggy shrub with white marble berries",
        base(name="snowberry", height_m=1.50,
             notes="WHITE BERRIES ON BARE TWIGS, AND THERE IS NO WHITE LEAF "
                   "MATERIAL. `materials.leaf` offers six leaf colours and "
                   "none of them is white or near it -- the engine HAS a snow "
                   "material and a true white plume, and neither is on that "
                   "menu. `leaf_blossom`, the pale cherry pink, is the "
                   "closest and it is pinker than the berries. A white or "
                   "near-white foliage entry is a one-row choice-list ask and "
                   "this species is the argument for it.\n\n"
                   "The frame is authored open and twiggy at a coverage of "
                   "0.55, so the berries read as scattered dots rather than "
                   "as a mass.",
             **mound(radius_base_m=0.05, radius_m=0.72, lean_deg=5.0,
                     wander=0.38, shape="vase", height_frac=0.90,
                     center_frac=0.55, asymmetry=0.44, offset=0.26,
                     points=900,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=-0.16,
                     inertia=0.46, jitter=0.16,
                     clump_radius_m=0.09, density=0.48, coverage=0.55,
                     separation=2.2, clump_jitter=0.45,
                     mat_bark="bark_pale", mat_leaf="leaf_blossom",
                     bio_temperate_forest=0.8, bio_grassland=0.5,
                     place_abundance=0.4, place_spacing_m=2.0,
                     place_cluster=0.7)),
    ),
    "box": (
        "2.00 m - very dense small dark leaves on a tight frame: a solid mass",
        base(name="box", height_m=2.00,
             notes="THE DENSEST THING IN THE LIBRARY AT ANY SCALE: coverage "
                   "1.0 at separation 1.1, with the smallest clumps here. It "
                   "exists to be the control at the solid end of the density "
                   "axis -- if `box` and `creosote-bush` are not obviously "
                   "different plants on a contact sheet, the axis is not doing "
                   "anything.",
             **mound(radius_base_m=0.06, radius_m=0.80, lean_deg=3.0,
                     wander=0.25, shape="ovoid", height_frac=0.92,
                     center_frac=0.52, asymmetry=0.24, offset=0.12,
                     points=1600,
                     step_m=0.08, influence_m=0.6, kill_m=0.14, gravity=0.0,
                     inertia=0.38, jitter=0.12,
                     clump_radius_m=0.08, density=0.85, coverage=1.0,
                     separation=1.10, clump_jitter=0.22,
                     mat_bark="bark", mat_leaf="leaf_needle",
                     bio_temperate_forest=0.8, bio_grassland=0.5,
                     place_abundance=0.35, place_spacing_m=1.5,
                     place_cluster=0.7)),
    ),
    # --- temperate forest understorey ---------------------------------------
    "hazel-coppice": (
        "3.00 m - many straight poles rising from one stool, no central trunk",
        base(name="hazel-coppice", height_m=3.00,
             notes="AUTHORED AT 3 m AGAINST THE BIOME LIST'S 5, and the reason "
                   "is cost: a bush is on the 5 cm detail lattice, so a 5 m "
                   "one is a hundred voxels tall and expensive for an "
                   "understorey plant that is placed in hundreds. 3 m keeps the "
                   "read and a third of the volume.\n\n"
                   "NO CENTRAL TRUNK is what a coppice stool is, and the "
                   "generator always grows one -- so the trunk radius is the "
                   "smallest here (0.035 m, under one voxel) and the poles are "
                   "the branches, kept straight by high inertia and low "
                   "jitter. That gets a sheaf rather than a tree.",
             **mound(radius_base_m=0.05, radius_m=0.90, lean_deg=4.0,
                     wander=0.20, shape="column", height_frac=0.94,
                     center_frac=0.52, asymmetry=0.30, offset=0.16,
                     points=1300,
                     step_m=0.12, influence_m=1.0, kill_m=0.22, gravity=0.12,
                     inertia=0.68, phototropism=0.35, jitter=0.07,
                     clump_radius_m=0.17, density=0.62, coverage=0.78,
                     separation=1.7, clump_jitter=0.32,
                     mat_bark="bark", mat_leaf="leaf_broadleaf",
                     bio_temperate_forest=1.0, bio_grassland=0.35,
                     bio_taiga=0.2,
                     place_abundance=0.7, place_spacing_m=2.5,
                     place_cluster=0.85)),
    ),
    "holly-understorey": (
        "3.00 m - a dense dark evergreen cone with glossy spined leaves",
        base(name="holly-understorey", height_m=3.00,
             notes="THE ONLY CONE-SHAPED BUSH IN THE SET, which is what makes a "
                   "holly readable in a wood full of round shrubs. Dark, dense "
                   "and clothed to the ground, so it is a solid dark triangle "
                   "under a canopy -- which is exactly how one is spotted from "
                   "thirty metres in winter.",
             **mound(radius_base_m=0.07, radius_m=0.75, lean_deg=3.0,
                     wander=0.20, shape="cone", height_frac=0.94,
                     center_frac=0.50, asymmetry=0.22, offset=0.10,
                     points=1400,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=-0.04,
                     inertia=0.42, jitter=0.12,
                     clump_radius_m=0.12, density=0.78, coverage=0.95,
                     separation=1.30, clump_jitter=0.26,
                     mat_bark="bark_pale", mat_leaf="leaf_needle",
                     bio_temperate_forest=1.0,
                     place_abundance=0.5, place_spacing_m=3.0,
                     place_cluster=0.5, place_elev_max_m=700)),
    ),
    "elder": (
        "3.00 m - loose open shrub with flat white flower plates",
        base(name="elder", height_m=3.00,
             notes="FLAT PLATES OF FLOWER HELD ON TOP, which is an `umbrella` "
                   "envelope with a thin lower shell -- the same geometry the "
                   "rabbitbrush uses for the same reason. Soft pithy stems mean "
                   "it is loose and open rather than twiggy, so the coverage is "
                   "low and the clumps are large.",
             **mound(radius_base_m=0.06, radius_m=1.10, lean_deg=6.0,
                     wander=0.35, shape="umbrella", height_frac=0.86,
                     center_frac=0.60, shell_upper=0.65, shell_lower=0.25,
                     asymmetry=0.42, offset=0.24, points=1000,
                     step_m=0.12, influence_m=1.0, kill_m=0.22, gravity=-0.08,
                     inertia=0.40, jitter=0.16,
                     clump_radius_m=0.20, density=0.56, coverage=0.72,
                     separation=1.8, clump_jitter=0.38,
                     mat_bark="bark_pale", mat_leaf="leaf_broadleaf",
                     bio_temperate_forest=1.0, bio_grassland=0.4,
                     place_abundance=0.5, place_spacing_m=2.5,
                     place_cluster=0.6)),
    ),
    "common-dogwood": (
        "2.50 m - upright stems that go BLOOD-RED in winter",
        base(name="common-dogwood", height_m=2.50,
             notes="THE WINTER COLOUR IS THE SPECIES AND IT IS IN THE WOOD, "
                   "NOT THE LEAF -- which is the one place in this file where "
                   "`materials.bark` is doing the identification. `heartwood` "
                   "is the warmest wood the palette has and it is a tan-brown "
                   "rather than a red; a red bark is a one-row materials ask "
                   "and this species is the argument.\n\n"
                   "Authored bare-ish, at a low leaf coverage, because the "
                   "stems are what is being looked at.",
             **mound(radius_base_m=0.05, radius_m=0.80, lean_deg=4.0,
                     wander=0.22, shape="column", height_frac=0.92,
                     center_frac=0.54, asymmetry=0.28, offset=0.14,
                     points=1100,
                     step_m=0.10, influence_m=0.8, kill_m=0.18, gravity=0.14,
                     inertia=0.62, phototropism=0.30, jitter=0.10,
                     clump_radius_m=0.12, density=0.50, coverage=0.50,
                     separation=2.0, clump_jitter=0.36,
                     mat_bark="heartwood", mat_leaf="leaf_autumn",
                     bio_temperate_forest=1.0, bio_grassland=0.4,
                     place_abundance=0.5, place_spacing_m=2.0,
                     place_cluster=0.85, place_water_max_m=40)),
    ),
    "rhododendron-thicket": (
        "3.00 m - an impenetrable dark evergreen mound of leathery leaves",
        base(name="rhododendron-thicket", height_m=3.00,
             notes="IT FORMS AN IMPENETRABLE UNDERSTOREY and that is a "
                   "placement fact: maximum clustering at 1.5 m spacing, so a "
                   "patch is a wall. Very large leathery leaf clumps -- the "
                   "biggest in this file at 0.26 m -- on a dense low mound.",
             **mound(radius_base_m=0.07, radius_m=1.15, lean_deg=5.0,
                     wander=0.32, shape="sphere", height_frac=0.92,
                     center_frac=0.52, asymmetry=0.36, offset=0.18,
                     points=1500,
                     step_m=0.11, influence_m=0.9, kill_m=0.20, gravity=-0.06,
                     inertia=0.38, jitter=0.14,
                     clump_radius_m=0.26, density=0.78, coverage=0.94,
                     separation=1.35, clump_jitter=0.28,
                     mat_bark="bark", mat_leaf="leaf_needle",
                     bio_temperate_forest=1.0, bio_taiga=0.3,
                     place_abundance=0.6, place_spacing_m=1.5,
                     place_cluster=1.0)),
    ),
    "salal": (
        "1.20 m - a low dense glossy-leaved evergreen mat under conifers",
        base(name="salal", height_m=1.20,
             notes="THE PACIFIC-FOREST FLOOR SHRUB, and it is a MAT rather than "
                   "a bush: wider than it is tall, clothed to the ground, and "
                   "placed as a continuous layer under a canopy. It is the "
                   "shrub-scale counterpart to `sword-fern` and the two share "
                   "the same ground.",
             **mound(radius_base_m=0.05, radius_m=0.85, lean_deg=6.0,
                     wander=0.35, shape="umbrella", height_frac=0.92,
                     center_frac=0.48, shell_upper=0.85, shell_lower=0.70,
                     asymmetry=0.40, offset=0.22, points=1200,
                     step_m=0.08, influence_m=0.6, kill_m=0.14, gravity=-0.10,
                     inertia=0.32, jitter=0.18,
                     clump_radius_m=0.15, density=0.76, coverage=0.92,
                     separation=1.35, clump_jitter=0.30,
                     mat_bark="bark", mat_leaf="leaf_jungle",
                     bio_temperate_forest=1.0, bio_taiga=0.3,
                     place_abundance=0.9, place_spacing_m=1.2,
                     place_cluster=0.95)),
    ),
    "witch-hazel": (
        "3.00 m - spreading zigzag branches with spidery ribbon flowers",
        base(name="witch-hazel", height_m=3.00,
             notes="ZIGZAG BRANCHES, which is high `jitter` with low inertia -- "
                   "the branch changes direction at every step instead of "
                   "running. Nothing else in the file is authored above 0.20 "
                   "there, and the effect is a shrub whose wood is visibly "
                   "crooked at ten voxels. Flowers on bare wood, so the "
                   "coverage is low and the material is the yellow.",
             **mound(radius_base_m=0.05, radius_m=1.20, lean_deg=6.0,
                     wander=0.40, shape="vase", height_frac=0.88,
                     center_frac=0.56, asymmetry=0.48, offset=0.26,
                     points=1000,
                     step_m=0.10, influence_m=0.8, kill_m=0.18, gravity=-0.02,
                     inertia=0.24, jitter=0.42,
                     clump_radius_m=0.12, density=0.48, coverage=0.52,
                     separation=2.1, clump_jitter=0.44,
                     mat_bark="bark", mat_leaf="leaf_dry",
                     bio_temperate_forest=1.0,
                     place_abundance=0.35, place_spacing_m=3.0,
                     place_cluster=0.5)),
    ),
    # --- beach ----------------------------------------------------------------
    "beach-rose": (
        "1.50 m - a rounded dense bush with big magenta flowers and red hips",
        base(name="beach-rose", height_m=1.50,
             notes="THE DENSE ROSE, against the `dog-rose`'s arching open one, "
                   "and the pair is deliberate: same genus, opposite habit. "
                   "This is a tight ball with the flowers ON it; that one is a "
                   "spray of canes with hips hanging off. Coverage 0.9 against "
                   "0.62 and gravity positive rather than negative.",
             **mound(radius_base_m=0.05, radius_m=0.80, lean_deg=4.0,
                     wander=0.32, shape="sphere", height_frac=0.92,
                     center_frac=0.52, asymmetry=0.30, offset=0.16,
                     points=1200,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=0.04,
                     inertia=0.36, jitter=0.16,
                     clump_radius_m=0.13, density=0.72, coverage=0.90,
                     separation=1.45, clump_jitter=0.32,
                     mat_bark="bark", mat_leaf="leaf_blossom",
                     bio_beach=1.0, bio_grassland=0.25,
                     place_abundance=0.5, place_spacing_m=2.0,
                     place_cluster=0.8, place_elev_max_m=40)),
    ),
    "saltbush": (
        "1.00 m - a low grey-white mealy mound with no thorns and no gaps",
        base(name="saltbush", height_m=1.00,
             notes="A LIGHT-GREY HEMISPHERE ON BARE GROUND, and that is all it "
                   "is: so tightly packed that no branch structure shows, "
                   "uniformly pale, no thorns and no flowers. It is the "
                   "shrub-set's null case and it is worth having for the same "
                   "reason `pale-minnow` is -- something has to be the plain "
                   "one.",
             **mound(radius_base_m=0.05, radius_m=0.62, lean_deg=3.0,
                     wander=0.28, shape="sphere", height_frac=0.90,
                     center_frac=0.50, asymmetry=0.22, offset=0.10,
                     points=1200,
                     step_m=0.08, influence_m=0.6, kill_m=0.13, gravity=0.02,
                     inertia=0.34, jitter=0.14,
                     clump_radius_m=0.10, density=0.80, coverage=0.96,
                     separation=1.25, clump_jitter=0.24,
                     mat_bark="bark_pale", mat_leaf="leaf_dry",
                     bio_beach=0.9, bio_desert=0.9, bio_grassland=0.6,
                     place_abundance=0.7, place_spacing_m=1.8,
                     place_cluster=0.5)),
    ),
    "bayberry": (
        "2.00 m - upright dark aromatic shrub with pale waxy berry clusters",
        base(name="bayberry", height_m=2.00,
             notes="UPRIGHT AND DARK on a dune back-slope, with the berries "
                   "held tight against bare stem rather than out on the "
                   "foliage. Drawn as a dark leaf mass on an upright frame "
                   "with a low top bias, so the wood shows near the top where "
                   "the berries would be.",
             **mound(radius_base_m=0.05, radius_m=0.68, lean_deg=4.0,
                     wander=0.26, shape="ovoid", height_frac=0.92,
                     center_frac=0.54, asymmetry=0.30, offset=0.16,
                     points=1200,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=0.10,
                     inertia=0.52, jitter=0.12,
                     clump_radius_m=0.12, density=0.70, coverage=0.85,
                     separation=1.5, clump_jitter=0.30, top_bias=0.15,
                     mat_bark="bark", mat_leaf="leaf_needle",
                     bio_beach=1.0, bio_grassland=0.2,
                     place_abundance=0.45, place_spacing_m=2.0,
                     place_cluster=0.75, place_elev_max_m=50)),
    ),
    "tamarisk-scrub": (
        "2.00 m - a feathery grey haze on whippy stems, no readable edge",
        base(name="tamarisk-scrub", height_m=2.00,
             notes="NO READABLE EDGE, authored the same way `casuarina` is: "
                   "very small clumps at a long stretch, so the outline "
                   "dissolves. It is the only shrub here built to be blurry, "
                   "and on a river margin in dry country that softness is what "
                   "identifies it against everything hard-edged around it.",
             **mound(radius_base_m=0.05, radius_m=0.90, lean_deg=6.0,
                     wander=0.42, shape="ovoid", height_frac=0.90,
                     center_frac=0.54, asymmetry=0.44, offset=0.24,
                     points=1400,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=-0.18,
                     inertia=0.42, jitter=0.18,
                     clump_radius_m=0.075, density=0.60, coverage=0.92,
                     separation=1.4, clump_jitter=0.36, stretch=4.5,
                     mat_bark="bark_pale", mat_leaf="leaf_dry",
                     bio_beach=0.8, bio_desert=0.8, bio_grassland=0.4,
                     place_abundance=0.5, place_spacing_m=2.2,
                     place_cluster=0.85, place_water_max_m=30)),
    ),
    # --- desert ---------------------------------------------------------------
    "creosote-bush": (
        "1.80 m - straight stems in a narrow V, bare ground swept clear round it",
        base(name="creosote-bush", height_m=1.80,
             notes="MORE BRANCH THAN LEAF, AND A RING OF BARE GROUND. The "
                   "second half is placement: a 4 m minimum spacing with very "
                   "low clustering, which is how a creosote flat actually "
                   "looks -- evenly spaced plants with swept ground between. "
                   "Getting that wrong gives scrub.\n\n"
                   "Foliage only in the top third (`top_bias` 0.85, the highest "
                   "here) on straight stems rising in a narrow V.",
             **mound(radius_base_m=0.05, radius_m=0.60, lean_deg=4.0,
                     wander=0.20, shape="vase", height_frac=0.92,
                     center_frac=0.56, asymmetry=0.30, offset=0.16,
                     points=900,
                     step_m=0.10, influence_m=0.8, kill_m=0.18, gravity=0.16,
                     inertia=0.62, phototropism=0.30, jitter=0.10,
                     clump_radius_m=0.10, density=0.50, coverage=0.55,
                     separation=2.1, clump_jitter=0.36, top_bias=0.85,
                     mat_bark="bark", mat_leaf="leaf_needle",
                     bio_desert=1.0, bio_grassland=0.3,
                     place_abundance=0.8, place_spacing_m=4.0,
                     place_cluster=0.15, place_slope_max_pct=40)),
    ),
    "four-wing-saltbush": (
        "1.20 m - a dense rounded grey-white mound, uniformly pale",
        base(name="four-wing-saltbush", height_m=1.20,
             notes="THE DESERT'S OWN PALE HEMISPHERE, and deliberately close to "
                   "`saltbush` -- the two are the same plant genus in two "
                   "biomes, and having both tests whether a 20% size difference "
                   "and a slightly looser frame separate them at all. If they "
                   "do not, one of them should go, and that is a cheap thing to "
                   "learn from a contact sheet.",
             **mound(radius_base_m=0.05, radius_m=0.70, lean_deg=4.0,
                     wander=0.32, shape="sphere", height_frac=0.90,
                     center_frac=0.50, asymmetry=0.28, offset=0.14,
                     points=1100,
                     step_m=0.08, influence_m=0.6, kill_m=0.14, gravity=0.04,
                     inertia=0.36, jitter=0.16,
                     clump_radius_m=0.11, density=0.74, coverage=0.90,
                     separation=1.35, clump_jitter=0.30,
                     mat_bark="bark_pale", mat_leaf="leaf_dry",
                     bio_desert=1.0, bio_grassland=0.35,
                     place_abundance=0.7, place_spacing_m=2.5,
                     place_cluster=0.4)),
    ),
    "ocotillo": (
        "3.00 m - a splayed fan of long straight unbranched thorny wands",
        base(name="ocotillo", height_m=3.00,
             notes="NOT A MASS AT ALL, which makes it the hardest shape in this "
                   "file: ten to twenty separate straight wands from one crown, "
                   "with nothing between them. It is got by pushing four "
                   "sliders to their ends at once -- `inertia` 0.90 and "
                   "`phototropism` 0.55 keep each wand dead straight, "
                   "`jitter` 0.02 stops it wandering, and a `spire` envelope "
                   "with a big kill radius means very few branches survive to "
                   "start with.\n\n"
                   "The scarlet flower spikes at the tips are `leaf_autumn` at "
                   "the very top only -- `top_bias` 1.0 -- so the wands are "
                   "bare grey wood with red ends, which is the plant for most "
                   "of the year.",
             **mound(radius_base_m=0.05, radius_m=0.85, lean_deg=3.0,
                     wander=0.14, shape="spire", height_frac=0.96,
                     center_frac=0.52, asymmetry=0.34, offset=0.10,
                     points=420,
                     step_m=0.16, influence_m=1.4, kill_m=0.55, gravity=0.28,
                     inertia=0.90, phototropism=0.55, jitter=0.02,
                     max_iter=300,
                     clump_radius_m=0.10, density=0.60, coverage=0.65,
                     separation=2.4, clump_jitter=0.30, top_bias=1.0,
                     mat_bark="deadwood", mat_leaf="leaf_autumn",
                     bio_desert=1.0,
                     place_abundance=0.4, place_spacing_m=3.5,
                     place_cluster=0.45, place_slope_max_pct=40)),
    ),
    "brittlebush": (
        "0.80 m - a silver mound with a halo of bare stalks and yellow heads",
        base(name="brittlebush", height_m=0.80,
             notes="A TWO-LAYER PLANT LIKE THE RABBITBRUSH, taken further: the "
                   "grey mound is the lower crown and the yellow flower halo is "
                   "held well clear ABOVE it, which is `crown.height_frac` 0.96 "
                   "with a very thin lower shell and a hard top bias. The gap "
                   "between the two layers is the species.",
             **mound(radius_base_m=0.05, radius_m=0.52, lean_deg=4.0,
                     wander=0.30, shape="umbrella", height_frac=0.96,
                     center_frac=0.58, shell_upper=0.55, shell_lower=0.30,
                     asymmetry=0.30, offset=0.14, points=900,
                     step_m=0.08, influence_m=0.6, kill_m=0.13, gravity=0.14,
                     inertia=0.48, jitter=0.14,
                     clump_radius_m=0.09, density=0.62, coverage=0.72,
                     separation=1.7, clump_jitter=0.34, top_bias=0.70,
                     mat_bark="bark_pale", mat_leaf="leaf_dry",
                     bio_desert=1.0, bio_grassland=0.25,
                     place_abundance=0.6, place_spacing_m=2.0,
                     place_cluster=0.5)),
    ),
    # --- savanna --------------------------------------------------------------
    "sicklebush": (
        "2.50 m - dense thorny shrub with fine feathery leaves",
        base(name="sicklebush", height_m=2.50,
             notes="FINE FEATHERY FOLIAGE ON A DENSE THORNY FRAME, which is "
                   "small clumps at high coverage -- the acacia texture at "
                   "shrub scale. It is the savanna's answer to a hedge and the "
                   "biome had nothing between `desert-shrub` at 1 m and "
                   "`hawthorn-scrub` at 3.8.",
             **mound(radius_base_m=0.05, radius_m=0.95, lean_deg=5.0,
                     wander=0.34, shape="ovoid", height_frac=0.92,
                     center_frac=0.54, asymmetry=0.38, offset=0.20,
                     points=1500,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=0.02,
                     inertia=0.38, jitter=0.16,
                     clump_radius_m=0.085, density=0.66, coverage=0.90,
                     separation=1.45, clump_jitter=0.34, stretch=2.6,
                     mat_bark="bark", mat_leaf="leaf_dry",
                     bio_savanna=1.0, bio_grassland=0.4,
                     place_abundance=0.7, place_spacing_m=2.5,
                     place_cluster=0.7)),
    ),
    "buffalo-thorn": (
        "2.50 m - zigzag branches with paired thorns, a rounded solid mass",
        base(name="buffalo-thorn", height_m=2.50,
             notes="ZIGZAG WOOD INSIDE A SOLID OUTLINE, which is the "
                   "combination that separates it from the sicklebush beside "
                   "it: high jitter like the witch-hazel, but at a coverage "
                   "high enough that the crookedness only shows at the edges. "
                   "Impenetrable and rounded.",
             **mound(radius_base_m=0.06, radius_m=1.00, lean_deg=5.0,
                     wander=0.40, shape="sphere", height_frac=0.92,
                     center_frac=0.52, asymmetry=0.36, offset=0.18,
                     points=1500,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=-0.02,
                     inertia=0.26, jitter=0.36,
                     clump_radius_m=0.15, density=0.74, coverage=0.92,
                     separation=1.40, clump_jitter=0.32,
                     mat_bark="bark", mat_leaf="leaf_broadleaf",
                     bio_savanna=1.0, bio_grassland=0.3,
                     place_abundance=0.6, place_spacing_m=3.0,
                     place_cluster=0.75)),
    ),
    "wild-sage-bush": (
        "2.00 m - silvery grey multi-stem shrub with off-white seed tips",
        base(name="wild-sage-bush", height_m=2.00,
             notes="SILVER OVERALL WITH PALE TIPS, which is the leaf material "
                   "doing the body and `snow` doing nothing -- there is one "
                   "leaf slot, so the off-white seed mass at the branch tips "
                   "cannot be a second colour. It is drawn as a pale dry "
                   "foliage throughout with a hard top bias, so the mass "
                   "gathers where the seed heads are. Recorded as the "
                   "compromise it is.",
             **mound(radius_base_m=0.05, radius_m=0.80, lean_deg=5.0,
                     wander=0.36, shape="ovoid", height_frac=0.92,
                     center_frac=0.54, asymmetry=0.36, offset=0.20,
                     points=1200,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=0.06,
                     inertia=0.44, jitter=0.16,
                     clump_radius_m=0.11, density=0.62, coverage=0.82,
                     separation=1.55, clump_jitter=0.34, top_bias=0.70,
                     mat_bark="bark_pale", mat_leaf="leaf_dry",
                     bio_savanna=1.0, bio_grassland=0.4, bio_desert=0.3,
                     place_abundance=0.7, place_spacing_m=2.5,
                     place_cluster=0.6)),
    ),
    # --- taiga and tundra: everything here is pressed flat ------------------
    "labrador-tea": (
        "0.80 m - upright open evergreen with rusty-woolly leaf undersides",
        base(name="labrador-tea", height_m=0.80,
             notes="A BOG-MARGIN SHRUB and placed as one -- close to water on "
                   "flat ground, heavily clustered. Upright and open rather "
                   "than mounded, which is what separates it from the bilberry "
                   "layer it grows through.",
             **mound(radius_base_m=0.05, radius_m=0.40, lean_deg=4.0,
                     wander=0.28, shape="column", height_frac=0.94,
                     center_frac=0.54, asymmetry=0.30, offset=0.16,
                     points=900,
                     step_m=0.08, influence_m=0.55, kill_m=0.13, gravity=0.10,
                     inertia=0.52, jitter=0.14,
                     clump_radius_m=0.10, density=0.66, coverage=0.80,
                     separation=1.5, clump_jitter=0.32,
                     mat_bark="bark", mat_leaf="leaf_needle",
                     bio_taiga=1.0, bio_tundra_alpine=0.4,
                     place_abundance=0.7, place_spacing_m=1.2,
                     place_cluster=0.95, place_water_max_m=25,
                     place_slope_max_pct=20)),
    ),
    "dwarf-birch": (
        "0.80 m - an ankle-to-knee-high tangle of stiff dark twigs",
        base(name="dwarf-birch", height_m=0.80,
             notes="FILED AS A BUSH RATHER THAN A TREE, DELIBERATELY. The "
                   "tundra file lists it under trees because it is a birch, and "
                   "a tree is locked to the 10 cm terrain lattice -- at which a "
                   "0.8 m plant is eight voxels tall and its 1 cm leaves do not "
                   "exist. As a bush it is on 5 cm and gets sixteen, which is "
                   "enough for a twiggy tangle to be twiggy. The kind is a "
                   "lattice decision here, not a botanical one.\n\n"
                   "The round scalloped leaves are 1 cm and read as texture "
                   "rather than shape at any lattice; they are drawn as small "
                   "clumps and nothing more.",
             **mound(radius_base_m=0.05, radius_m=0.55, lean_deg=6.0,
                     wander=0.45, shape="umbrella", height_frac=0.94,
                     center_frac=0.48, asymmetry=0.45, offset=0.24,
                     points=1100,
                     step_m=0.08, influence_m=0.5, kill_m=0.11, gravity=-0.02,
                     inertia=0.30, jitter=0.22,
                     clump_radius_m=0.08, density=0.60, coverage=0.78,
                     separation=1.55, clump_jitter=0.38,
                     mat_bark="bark", mat_leaf="leaf_autumn",
                     bio_tundra_alpine=1.0, bio_taiga=0.7,
                     place_abundance=0.9, place_spacing_m=1.0,
                     place_cluster=0.95, place_slope_max_pct=55)),
    ),
    "alpenrose": (
        "0.80 m - a rounded dense evergreen with deep pink-red bell flowers",
        base(name="alpenrose", height_m=0.80,
             notes="THE ALPINE BAND'S ONE PIECE OF STRONG COLOUR AT SHRUB "
                   "SCALE, and it is a seasonal palette swap on fixed geometry: "
                   "`leaf_blossom` in flower, `leaf_needle` out of it, same "
                   "spec otherwise. Authored in flower, because a green mound "
                   "is what `juniper-scrub` already gives that biome.",
             **mound(radius_base_m=0.05, radius_m=0.50, lean_deg=5.0,
                     wander=0.32, shape="sphere", height_frac=0.92,
                     center_frac=0.50, asymmetry=0.30, offset=0.16,
                     points=1000,
                     step_m=0.08, influence_m=0.5, kill_m=0.12, gravity=0.0,
                     inertia=0.34, jitter=0.16,
                     clump_radius_m=0.09, density=0.74, coverage=0.92,
                     separation=1.35, clump_jitter=0.28,
                     mat_bark="bark", mat_leaf="leaf_blossom",
                     bio_tundra_alpine=1.0, bio_taiga=0.3,
                     place_abundance=0.6, place_spacing_m=1.5,
                     place_cluster=0.85, place_elev_min_m=600,
                     place_slope_max_pct=60)),
    ),
    "dwarf-mountain-pine": (
        "1.00 m tall and four across - a pine lying almost flat",
        base(name="dwarf-mountain-pine", height_m=1.00,
             notes="THREE TIMES WIDER THAN IT IS TALL, which is the widest "
                   "aspect ratio in the library: a crown radius of 1.6 m on a "
                   "1 m plant. Snow load and wind press it flat, and the stems "
                   "curve up only at the tips -- `gravity` -0.30 with the crown "
                   "centred at 0.42 of the height, so the mass sits low and "
                   "spreads.\n\n"
                   "It is the treeline plant that is NOT `alpine-krummholz`: "
                   "krummholz is a deformed upright conifer streaming to one "
                   "side, and this is a species that grows prostrate on "
                   "purpose. Both belong at the same altitude and they look "
                   "different.",
             **mound(radius_base_m=0.05, radius_m=1.60, lean_deg=4.0,
                     wander=0.40, shape="umbrella", height_frac=0.94,
                     center_frac=0.42, shell_upper=0.85, shell_lower=0.75,
                     asymmetry=0.42, offset=0.26, points=1400,
                     step_m=0.08, influence_m=0.7, kill_m=0.15, gravity=-0.30,
                     inertia=0.40, jitter=0.14,
                     clump_radius_m=0.14, density=0.72, coverage=0.90,
                     separation=1.4, clump_jitter=0.30, stretch=3.2,
                     mat_bark="bark", mat_leaf="leaf_needle",
                     bio_tundra_alpine=1.0, bio_taiga=0.5,
                     place_abundance=0.7, place_spacing_m=3.0,
                     place_cluster=0.85, place_elev_min_m=500,
                     place_slope_max_pct=60)),
    ),
    "arctic-willow-thicket": (
        "0.50 m - many thin upright twigs with silvery leaf undersides",
        base(name="arctic-willow-thicket", height_m=0.50,
             notes="THE SHORTEST WOODY ASSET IN THE LIBRARY at ten voxels, and "
                   "it is authored as a THICKET rather than a plant: many very "
                   "thin upright twigs, tight spacing, maximum clustering, so a "
                   "patch is a low grey-green haze over permafrost ground. The "
                   "upright fuzzy catkins are the only vertical element in life "
                   "and they are under a voxel; they are not drawn.",
             **mound(radius_base_m=0.05, radius_m=0.34, lean_deg=5.0,
                     wander=0.35, shape="column", height_frac=0.94,
                     center_frac=0.52, asymmetry=0.34, offset=0.18,
                     points=900,
                     step_m=0.08, influence_m=0.45, kill_m=0.10, gravity=0.06,
                     inertia=0.48, jitter=0.16,
                     clump_radius_m=0.07, density=0.62, coverage=0.78,
                     separation=1.5, clump_jitter=0.34,
                     mat_bark="bark_pale", mat_leaf="leaf_dry",
                     bio_tundra_alpine=1.0, bio_taiga=0.4,
                     place_abundance=0.9, place_spacing_m=0.8,
                     place_cluster=1.0, place_slope_max_pct=55)),
    ),
    # --- rainforest understorey ----------------------------------------------
    "coffee-shrub": (
        "2.50 m - upright multi-stem with FLAT HORIZONTAL branch tiers",
        base(name="coffee-shrub", height_m=2.50,
             notes="HORIZONTAL TIERS ON AN UPRIGHT STEM, which is the one shrub "
                   "shape in this file that is layered rather than massed. It "
                   "is got from `distichous` foliage -- leaves two-ranked into "
                   "one flat plane -- on branches held level by a gravity near "
                   "zero and a high inertia. Glossy dark leaves in opposite "
                   "pairs, and red berries against the stems that are drawn as "
                   "nothing, because there is one leaf slot.",
             **mound(radius_base_m=0.05, radius_m=0.75, lean_deg=3.0,
                     wander=0.20, shape="column", height_frac=0.92,
                     center_frac=0.54, asymmetry=0.26, offset=0.14,
                     points=1300,
                     step_m=0.09, influence_m=0.75, kill_m=0.17, gravity=0.0,
                     inertia=0.66, jitter=0.08,
                     clump_radius_m=0.16, density=0.72, coverage=0.90,
                     separation=1.5, clump_jitter=0.24, stretch=3.0,
                     mat_bark="bark", mat_leaf="leaf_jungle",
                     bio_rainforest=1.0,
                     place_abundance=0.6, place_spacing_m=2.0,
                     place_cluster=0.8)),
    ),
    "wild-hibiscus": (
        "2.50 m - a loose open shrub with a few large flat five-petal blooms",
        base(name="wild-hibiscus", height_m=2.50,
             notes="FEW LARGE FLOWERS ON A SPARSE FRAME, which is the lowest "
                   "coverage of any rainforest plant here at 0.55 with the "
                   "largest clumps -- so the plant is mostly air with a handful "
                   "of big red masses in it. That is the opposite of every "
                   "other understorey species and it is what a hibiscus looks "
                   "like.",
             **mound(radius_base_m=0.05, radius_m=0.95, lean_deg=6.0,
                     wander=0.38, shape="vase", height_frac=0.90,
                     center_frac=0.56, asymmetry=0.44, offset=0.26,
                     points=800,
                     step_m=0.11, influence_m=0.9, kill_m=0.20, gravity=-0.10,
                     inertia=0.40, jitter=0.18,
                     clump_radius_m=0.22, density=0.55, coverage=0.55,
                     separation=2.1, clump_jitter=0.40,
                     mat_bark="bark", mat_leaf="leaf_blossom",
                     bio_rainforest=1.0, bio_beach=0.3,
                     place_abundance=0.4, place_spacing_m=3.0,
                     place_cluster=0.6)),
    ),
    "dracaena-thicket": (
        "2.00 m - bare canes leaning apart, each topped by a strap-leaf tuft",
        base(name="dracaena-thicket", height_m=2.00,
             notes="A SHRUB BUILT LIKE A SMALL PALM: several bare canes leaning "
                   "out from one base with all the foliage at the tips. "
                   "`top_bias` 1.0 and a coverage of 0.5 put every clump at the "
                   "ends, and a `vase` envelope leans the canes apart. Nothing "
                   "else in the shrub set is bare in the middle.",
             **mound(radius_base_m=0.05, radius_m=0.85, lean_deg=8.0,
                     wander=0.24, shape="vase", height_frac=0.94,
                     center_frac=0.56, asymmetry=0.40, offset=0.22,
                     points=600,
                     step_m=0.13, influence_m=1.1, kill_m=0.30, gravity=0.10,
                     inertia=0.72, jitter=0.08,
                     clump_radius_m=0.24, density=0.72, coverage=0.50,
                     separation=2.0, clump_jitter=0.26, top_bias=1.0,
                     mat_bark="bark_pale", mat_leaf="leaf_jungle",
                     bio_rainforest=1.0,
                     place_abundance=0.5, place_spacing_m=2.5,
                     place_cluster=0.75)),
    ),
    "monstera-clump": (
        "1.50 m - a sprawling clump of very large dark heart-shaped leaves",
        base(name="monstera-clump", height_m=1.50,
             notes="THE LARGEST LEAF-TO-PLANT RATIO IN THE LIBRARY: clumps of "
                   "0.30 m on a 1.5 m plant, which is a fifth of its height per "
                   "leaf mass. The holes and slashes that name it are a leaf "
                   "shape and there is no leaf shape at 5 cm; what carries it "
                   "is that ratio plus a low sprawling habit.",
             **mound(radius_base_m=0.05, radius_m=0.85, lean_deg=8.0,
                     wander=0.45, shape="umbrella", height_frac=0.94,
                     center_frac=0.48, asymmetry=0.48, offset=0.28,
                     points=700,
                     step_m=0.10, influence_m=0.85, kill_m=0.22, gravity=-0.14,
                     inertia=0.34, jitter=0.18,
                     clump_radius_m=0.30, density=0.76, coverage=0.85,
                     separation=1.45, clump_jitter=0.30, stretch=1.5,
                     mat_bark="bark", mat_leaf="leaf_jungle",
                     bio_rainforest=1.0,
                     place_abundance=0.6, place_spacing_m=2.0,
                     place_cluster=0.85, place_water_max_m=60)),
    ),
}


def main() -> int:
    force = seedspec.parse_force(sys.argv[1:])
    seedspec.announce(force, "shrub specs")
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
