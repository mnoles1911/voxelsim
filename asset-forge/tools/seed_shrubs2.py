"""Author the last eight shrubs on the biome lists.

WHAT IS LEFT AFTER `tools/seed_shrubs.py`. That file took the thirty rows that
were a mound of foliage on a woody frame -- the shape the bush generator was
written for. The eight here are the ones that are NOT that, and each is here
because it needs one thing the thirty did not:

  * TWO ARE FRUIT SHRUBS whose whole identity is a colour the leaf palette does
    not have. `spindle`'s pink-and-orange fruit and `guelder-rose`'s
    translucent red berries are what anyone recognises them by, and
    `materials.leaf` has six entries, all of them leaves. This is the same
    substitution `dog-rose`, `rowan` and `snowberry` already record, and the
    same one-row ask behind all of them.
  * ONE IS A SHRUB WHOSE LEAVES ARE STEMS. `butchers-broom` carries flattened
    green shoots that do the work of leaves, with a berry sitting in the middle
    of each. At 5 cm the berry is under a voxel; what can be said is the
    stiffness and the spine-tipped outline.
  * ONE STANDS ON ITS ROOTS. `mangrove-sapling` is the first bush in the
    library to use `roots.*` at all -- the stilt-root cone is bigger than the
    plant on top of it, and `roots.rise` is the only parameter here that makes
    a root leave the ground and come back to it.
  * ONE IS A PALM, and it is authored with `growth.model: frond` rather than
    the `colonize` every other bush uses. THAT IS WORTH KNOWING BEFORE ANYONE
    OPENS IT IN THE APP: `forge/kinds.py:72-75` does not list `frond` among the
    bush parameter groups, so the app will not show those sliders for this
    spec even though `forge/pipeline.py:298` dispatches on the model for any
    kind and builds it correctly. It is authored that way because a fan palm's
    identity is a COUNT -- three to six discrete fans -- and `frond.count` is
    that number, where a colonised crown gives leaf clumps that no setting
    resolves into individual fans. `dracaena-thicket` is the precedent for
    faking a palm with clumps, and it works there because its leaf tufts are
    small; a 1 m fan is not a tuft.

FIVE CENTIMETRES, not the trees' ten, exactly as `tools/seed_shrubs.py` states:
a bush is a DETAIL entity, carries its own grid and transform and never joins
the terrain lattice (`forge/kinds.py:29-58`), so it is free to be finer, and at
10 cm a knee-high shrub is five voxels tall with nothing in it. `butchers-broom`
at 0.8 m is the species that most needs that and would be unauthorable without
it.

SPINDLE AND GUELDER ROSE ARE ONE SPEC EACH, NOT TWO. Both appear on the
grassland list and the temperate-forest list, and a species that grows in two
biomes is one asset with two `biomes.*` weights -- the weights are what a
scatterer reads. Authoring the same plant twice would put two subtly different
individuals of one species in the library and no way to tell which a placement
should use.

    python tools/seed_shrubs2.py
    python tools/seed_shrubs2.py --force

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
    """The same keyword-to-dotted-path translation `seed_shrubs.py` uses.

    Two prefixes are added here because this file is the first to touch either
    group from a bush: `root_` becomes `roots.` and `frond_` becomes `frond.`.
    Both groups carry a `count`, which is why they are prefixed rather than
    added to the table below.
    """
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
        elif k.startswith("root_"):
            out["roots." + k[len("root_"):]] = v
        elif k.startswith("frond_"):
            out["frond." + k[len("frond_"):]] = v
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
    # --- the hedgerow three: grassland AND temperate forest ------------------
    "spindle": (
        "3.00 m - slim green four-angled twigs hung with hot-pink fruit",
        base(name="spindle", height_m=3.00,
             notes="THE FRUIT IS THE SPECIES AND IT IS THE ONE COLOUR THE LEAF "
                   "PALETTE CANNOT MAKE. A spindle in autumn is hot pink "
                   "four-lobed capsules splitting to show orange seed, on a "
                   "shrub that is otherwise unremarkable -- nobody identifies "
                   "one by its leaf. `leaf_blossom` is the palette's pale "
                   "cherry pink and it is the closest of six leaf colours; it "
                   "is paler and softer than the real thing, and the orange "
                   "inside the split cannot be said at all. There is one leaf "
                   "slot. Same substitution `dog-rose` and `rowan` record.\n\n"
                   "Authored OPEN, at a coverage of 0.58, because the fruit "
                   "hangs on visible twigs rather than sitting in a mass -- and "
                   "because it is the only way scattered pink dots read as "
                   "fruit rather than as blossom. The green four-angled twigs "
                   "that name it are 5 mm across and are not attempted; the "
                   "bark is the pale one, which is as near as the wood palette "
                   "gets to green.\n\n"
                   "One spec, two biomes: it is a hedgerow and wood-edge shrub "
                   "and appears on both the grassland and the temperate-forest "
                   "lists.",
             **mound(radius_base_m=0.05, radius_m=1.00, lean_deg=5.0,
                     wander=0.34, shape="vase", height_frac=0.90,
                     center_frac=0.56, asymmetry=0.42, offset=0.24,
                     points=1000,
                     step_m=0.11, influence_m=0.9, kill_m=0.20, gravity=-0.06,
                     inertia=0.52, jitter=0.14,
                     clump_radius_m=0.11, density=0.52, coverage=0.58,
                     separation=2.0, clump_jitter=0.40,
                     mat_bark="bark_pale", mat_leaf="leaf_blossom",
                     bio_grassland=0.9, bio_temperate_forest=1.0,
                     place_abundance=0.4, place_spacing_m=2.5,
                     place_cluster=0.7, place_elev_max_m=900)),
    ),
    "guelder-rose": (
        "3.00 m - an open maple-leaved shrub hung with translucent red berries",
        base(name="guelder-rose", height_m=3.00,
             notes="AUTHORED IN AUTUMN RATHER THAN IN FLOWER, WHICH IS A "
                   "CHOICE AND NOT AN OVERSIGHT. A guelder rose has two "
                   "distinct looks -- flat white flower plates in spring, "
                   "heavy translucent red berries and red foliage in autumn -- "
                   "and one leaf material has to pick. The white plates are "
                   "already what `elder` is built to show, using an `umbrella` "
                   "crown with a thin lower shell, and two shrubs doing the "
                   "same trick in the same wood would read as one plant. So "
                   "this one takes the red: `leaf_autumn` on an open vase "
                   "frame, which is unlike anything else in the understorey.\n\n"
                   "OPEN IS THE OTHER HALF. A guelder rose is a loose "
                   "see-through shrub, so the coverage is low and the clumps "
                   "are large -- a few heavy red masses on visible wood rather "
                   "than a solid ball. The maple-shaped leaf is a leaf shape "
                   "and there are no leaf shapes at 5 cm.\n\n"
                   "One spec, two biomes: damp hedgerow and wood edge, so it "
                   "carries a grassland weight and a temperate-forest one, and "
                   "places near water.",
             **mound(radius_base_m=0.05, radius_m=1.15, lean_deg=6.0,
                     wander=0.38, shape="vase", height_frac=0.88,
                     center_frac=0.58, asymmetry=0.46, offset=0.26,
                     points=950,
                     step_m=0.11, influence_m=0.9, kill_m=0.20, gravity=-0.12,
                     inertia=0.44, jitter=0.16,
                     clump_radius_m=0.19, density=0.54, coverage=0.66,
                     separation=1.9, clump_jitter=0.40,
                     mat_bark="bark", mat_leaf="leaf_autumn",
                     bio_grassland=0.8, bio_temperate_forest=1.0,
                     bio_taiga=0.25,
                     place_abundance=0.45, place_spacing_m=2.5,
                     place_cluster=0.75, place_water_max_m=60)),
    ),
    "wild-privet": (
        "2.50 m - a dense twiggy upright hedge shrub with black berry clusters",
        base(name="wild-privet", height_m=2.50,
             notes="THE HEDGE SHAPE, AND IT IS UPRIGHT WHERE `box` IS ROUND. "
                   "The two are the library's two solid-mass evergreens and "
                   "they have to be told apart: box is a tight ball of the "
                   "smallest clumps at coverage 1.0, and privet is a taller "
                   "narrower column of slightly larger clumps at 0.88, with "
                   "enough twig showing at the edges to look twiggy. If they "
                   "are not obviously different on a contact sheet, one of them "
                   "is redundant.\n\n"
                   "The black berries are drawn as nothing. `materials.leaf` "
                   "has no dark colour that is not a green, and a berry cluster "
                   "on a dark evergreen would be invisible against it anyway -- "
                   "which is honest, because that is roughly true in life "
                   "until the leaves drop.",
             **mound(radius_base_m=0.05, radius_m=0.72, lean_deg=4.0,
                     wander=0.26, shape="column", height_frac=0.94,
                     center_frac=0.54, asymmetry=0.28, offset=0.14,
                     points=1500,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=0.08,
                     inertia=0.56, phototropism=0.25, jitter=0.12,
                     clump_radius_m=0.10, density=0.78, coverage=0.88,
                     separation=1.30, clump_jitter=0.26,
                     mat_bark="bark", mat_leaf="leaf_needle",
                     bio_grassland=1.0, bio_temperate_forest=0.7,
                     place_abundance=0.5, place_spacing_m=1.6,
                     place_cluster=0.9, place_elev_max_m=800)),
    ),
    # --- temperate forest understorey ----------------------------------------
    "red-huckleberry": (
        "2.00 m - so open it is mostly air: fine green twigs and red dots",
        base(name="red-huckleberry", height_m=2.00,
             notes="THE MOST OPEN SHRUB IN THE LIBRARY, and that is the whole "
                   "point of it. Coverage 0.42 on the smallest clumps here "
                   "against `box` at 1.0 -- the two are the ends of the density "
                   "axis the shrub set is built along, and this one exists to "
                   "hold the sparse end in a temperate wood where every other "
                   "understorey plant is a mass. A red huckleberry seen against "
                   "the light is a green haze of twigs with a few red dots in "
                   "it and daylight through the middle.\n\n"
                   "The bright green TWIGS are the field mark and the wood "
                   "palette has four browns; `bark_pale` is the lightest and "
                   "the twigs will read as pale rather than green. A green bark "
                   "is a one-row ask and this species and `common-broom` are "
                   "the two arguments for it.\n\n"
                   "Often rooted on a rotting stump in life, which nothing here "
                   "can say -- there is no substrate in a spec.",
             **mound(radius_base_m=0.05, radius_m=0.85, lean_deg=5.0,
                     wander=0.36, shape="vase", height_frac=0.92,
                     center_frac=0.56, asymmetry=0.46, offset=0.26,
                     points=800,
                     step_m=0.09, influence_m=0.75, kill_m=0.16, gravity=-0.10,
                     inertia=0.50, jitter=0.16,
                     clump_radius_m=0.07, density=0.42, coverage=0.42,
                     separation=2.3, clump_jitter=0.45,
                     mat_bark="bark_pale", mat_leaf="leaf_autumn",
                     bio_temperate_forest=1.0, bio_taiga=0.35,
                     place_abundance=0.45, place_spacing_m=2.2,
                     place_cluster=0.7)),
    ),
    "mountain-laurel": (
        "3.00 m - a dense evergreen dome on visibly crooked stems",
        base(name="mountain-laurel", height_m=3.00,
             notes="GNARLED WOOD INSIDE A DENSE OUTLINE, which is the same "
                   "combination `buffalo-thorn` uses on a savanna and is the "
                   "only way this generator says 'twisted': high `jitter` with "
                   "low `inertia` makes the branch change direction at every "
                   "step, and a high coverage then hides most of it, so the "
                   "crookedness shows only where the wood leaves the mass. A "
                   "mountain laurel's stems are its best feature and they are "
                   "mostly buried in its own foliage, which is true in life "
                   "too.\n\n"
                   "AUTHORED IN FLOWER, like `alpenrose`, and for the same "
                   "reason: this is a seasonal palette swap on fixed geometry "
                   "-- `leaf_blossom` in flower, `leaf_needle` out of it, "
                   "nothing else changes. The dark green mound is what "
                   "`rhododendron-thicket` already gives this biome, and the "
                   "two are close enough relatives that the flowering state is "
                   "what separates them at a distance.\n\n"
                   "Steep rocky slopes: the highest slope ceiling of any shrub "
                   "on the temperate list.",
             **mound(radius_base_m=0.06, radius_m=1.05, lean_deg=6.0,
                     wander=0.45, shape="sphere", height_frac=0.92,
                     center_frac=0.52, asymmetry=0.38, offset=0.20,
                     points=1400,
                     step_m=0.09, influence_m=0.75, kill_m=0.17, gravity=-0.04,
                     inertia=0.26, jitter=0.38,
                     clump_radius_m=0.14, density=0.76, coverage=0.92,
                     separation=1.40, clump_jitter=0.30,
                     mat_bark="bark", mat_leaf="leaf_blossom",
                     bio_temperate_forest=1.0, bio_taiga=0.3,
                     place_abundance=0.5, place_spacing_m=2.0,
                     place_cluster=0.85, place_slope_max_pct=65,
                     place_elev_max_m=1400)),
    ),
    "butchers-broom": (
        "0.80 m - stiff dark spine-tipped shoots that are pretending to be leaves",
        base(name="butchers-broom", height_m=0.80,
             notes="THE LEAVES ARE STEMS AND THAT IS THE SPECIES. What look "
                   "like small stiff spine-tipped leaves are flattened SHOOTS, "
                   "each with a berry sitting in the middle of its face -- and "
                   "there is nothing in this generator that puts a berry on a "
                   "leaf, nor a leaf material that would show one. At 5 cm the "
                   "berry is under a voxel. So what is authored is the rest of "
                   "it: a small, very stiff, very dark, upright bundle whose "
                   "foliage is hard-edged rather than soft.\n\n"
                   "STIFFNESS IS THE READ AND IT IS BOUGHT WITH FOUR SETTINGS "
                   "AT ONCE -- high `inertia`, low `jitter`, low `wander` and a "
                   "`column` envelope -- so the shoots run straight from the "
                   "base to the top with no curve in them. Nothing else this "
                   "short in the library is that rigid; `arctic-willow-thicket` "
                   "is the other 0.5 m woody asset and it is a soft haze.\n\n"
                   "Deep shade under beech and holm oak, which is why the "
                   "abundance is high and the spacing tight: it grows in "
                   "colonies on ground nothing else will take.",
             **mound(radius_base_m=0.05, radius_m=0.34, lean_deg=3.0,
                     wander=0.16, shape="column", height_frac=0.96,
                     center_frac=0.54, asymmetry=0.26, offset=0.12,
                     points=900,
                     step_m=0.08, influence_m=0.5, kill_m=0.11, gravity=0.14,
                     inertia=0.72, phototropism=0.30, jitter=0.05,
                     clump_radius_m=0.09, density=0.72, coverage=0.86,
                     separation=1.35, clump_jitter=0.22,
                     mat_bark="bark", mat_leaf="leaf_needle",
                     bio_temperate_forest=1.0, bio_grassland=0.2,
                     place_abundance=0.7, place_spacing_m=1.0,
                     place_cluster=0.95, place_elev_max_m=900)),
    ),
    # --- beach -----------------------------------------------------------------
    "mangrove-sapling": (
        "1.50 m - a small crown on a stilt-root cone bigger than the plant",
        base(name="mangrove-sapling", height_m=1.50,
             notes="THE FIRST BUSH IN THE LIBRARY TO USE `roots.*` -- eleven "
                   "trees already do -- and the one with the highest ROOT "
                   "RATIO of any of them: 0.9 m of root on a 1.5 m plant is "
                   "0.60, against `red-mangrove`'s 0.43 and `screwpine`'s 0.37, "
                   "which are the next two. That ratio is the species and it is "
                   "what the beach file means by 'proportionally huge'.\n\n"
                   "A mangrove sapling is a thin stem with a handful of leaves "
                   "standing on a cone of arching stilts that meet the mud well "
                   "outside it, and `roots.rise` is the parameter that makes a "
                   "root leave the ground and come back down to it -- "
                   "`trunk.buttress` only thickens the bole and would give a "
                   "flare, not a cone. The roots are authored thick (0.7 of the "
                   "trunk) because at 5 cm a thin one is a single voxel and "
                   "breaks up into dashes. MEASURED: with `roots.count` at 7 "
                   "the lowest three voxel layers hold about 230 voxels and the "
                   "plant is 1.9 m across; at 0 they hold 18 and it is 1.0 m "
                   "across. The cone is real and it is the wider half.\n\n"
                   "THIS IS THE SAPLING, NOT THE TREE. `red-mangrove` already "
                   "ships as a 7 m tree on the 10 cm terrain lattice with "
                   "fourteen roots; this is the seedling stage that grows in "
                   "the water margin in front of it, on the 5 cm detail "
                   "lattice, where thirty voxels of height is enough to carry "
                   "stilts that a 10 cm lattice would lose.\n\n"
                   "Placed in the water margin: hard against water, flat "
                   "ground, at sea level.",
             **mound(radius_base_m=0.05, radius_m=0.50, clear_frac=0.30,
                     lean_deg=4.0, wander=0.22, shape="ovoid",
                     height_frac=0.62, center_frac=0.74,
                     asymmetry=0.32, offset=0.16, points=700,
                     step_m=0.09, influence_m=0.7, kill_m=0.16, gravity=-0.08,
                     inertia=0.54, jitter=0.12,
                     clump_radius_m=0.15, density=0.70, coverage=0.85,
                     separation=1.5, clump_jitter=0.28,
                     root_count=7, root_length_m=0.9, root_rise=0.85,
                     root_thickness=0.70, root_irregular=0.35,
                     mat_bark="bark_pale", mat_leaf="leaf_jungle",
                     bio_beach=1.0, bio_rainforest=0.3,
                     place_abundance=0.6, place_spacing_m=1.2,
                     place_cluster=0.95, place_water_max_m=5,
                     place_elev_max_m=4, place_slope_max_pct=10)),
    ),
    # --- rainforest understorey ------------------------------------------------
    "understory-fan-palm": (
        "3.00 m - a stubby trunk carrying five stiff near-flat fans",
        base(name="understory-fan-palm", height_m=3.00,
             notes="THE ONLY BUSH IN THE LIBRARY BUILT WITH `growth.model: "
                   "frond`, and the reason is a COUNT. A fan palm is three to "
                   "six discrete fans and nothing else; `frond.count` is "
                   "literally that number, where a colonised crown gives leaf "
                   "clumps that no density setting ever resolves into "
                   "individual leaves. `dracaena-thicket` fakes a palm with "
                   "clumps and it works there because its leaf tufts are small "
                   "-- a 1.2 m fan is not a tuft.\n\n"
                   "KNOW THIS BEFORE OPENING IT IN THE APP: `forge/kinds.py:"
                   "72-75` does not list `frond` among the bush parameter "
                   "groups, so the app shows no frond sliders for this spec. "
                   "The build is correct regardless -- `forge/pipeline.py:298` "
                   "dispatches on `growth.model` for any kind -- but tuning the "
                   "fans means editing the JSON or this file until that group "
                   "is added to the bush kind. That is a one-line kind-list "
                   "question and it is the only thing standing between this "
                   "spec and ordinary tuning.\n\n"
                   "A FAN IS A DISC AND A FROND IS A BLADE, which is the honest "
                   "approximation here: the generator's blade is lofted along a "
                   "midrib, so it is authored short and very wide (1.1 m long, "
                   "0.62 m half-width) to come out nearly circular. The pleats "
                   "and the split into wedges are 2-3 cm features and are not "
                   "drawn at any lattice this project has.\n\n"
                   "Short stubby trunk: `clear_frac` 0.55, the highest in the "
                   "shrub set, because everything a palm has is at the top of "
                   "its stem.",
             **t(clear_frac=0.55, radius_base_m=0.10, lean_deg=6.0, wander=0.20,
                 shape="umbrella", radius_m=1.30, height_frac=0.45,
                 center_frac=0.78, shell_upper=0.70, shell_lower=0.35,
                 asymmetry=0.30, offset=0.14, points=140,
                 model="frond", step_m=0.30, influence_m=2.0, kill_m=0.70,
                 gravity=-0.15, inertia=0.70, jitter=0.05, max_iter=260,
                 tip_radius_m=0.05, radius_exp=2.4,
                 frond_count=5, frond_length_m=1.10, frond_width_m=0.62,
                 frond_rise=0.55, frond_droop=0.42, frond_dead=0.12,
                 frond_irregular=0.30,
                 clump_radius_m=0.20, density=0.75, coverage=0.95,
                 separation=1.5, clump_jitter=0.25,
                 mat_bark="bark_pale", mat_leaf="leaf_jungle",
                 bio_rainforest=1.0, bio_beach=0.25,
                 place_abundance=0.6, place_spacing_m=2.5,
                 place_cluster=0.8, place_water_max_m=80)),
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
