"""Hero assets: landmark-scale set pieces, placed rarely.

These are not big versions of the ordinary library. They are the shapes the
mechanisms were built for, run at the size those shapes occur at in the world —
an arch is not a boulder with a hole, it is a landform, and it only reads as
one at twenty metres and up.

Everything here is deliberately rare. A point of interest stops being one the
moment there are two in sight, so abundance runs a tenth to a hundredth of
ordinary scenery and the spacing is in hundreds of metres. Those numbers are
carried on the spec for whatever ends up consuming them.

One note on size. `rock.size_m` is the finished longest dimension, and the
generator measures the stone and corrects until it hits that. Stretching a
shape with `elongate` therefore does NOT make it bigger — it makes it longer
and thinner within the same overall size, and the fitting loop shrinks
everything else to compensate. That is why the arch asks for 40 m rather than
40 m times its stretch.
"""
import os
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(os.environ.get("ASSET_FORGE_SPECS",
                            Path(__file__).resolve().parents[1] / "specs"))
FORCE = seedspec.parse_force(sys.argv[1:])

# Heroes vary LESS than ordinary scenery, and they have to say so here rather
# than only on disk. A landmark is a landmark because it is the one you
# recognise, so two of them the world apart should be near enough the same
# thing; ordinary variation on a set piece just makes it look unreliable. This
# was carried on the saved specs and not in this file, so re-running the file
# reset every hero to library variation without saying anything -- along with
# the hand-tuning on hero-natural-arch, which is the reason the values below
# are written out rather than left at whatever the defaults happen to be.
HERO_VARIATION = {
    "variation.height": 0.18, "variation.crown_radius": 0.18,
    "variation.trunk_radius": 0.18, "variation.shape": 0.12,
    "variation.density": 0.12, "variation.droop": 0.25,
    "variation.lean_deg": 7.0,
}


def make(name, cm="5", **changes):
    """Write one hero spec.

    The lattice is per-hero and not a default. It used to be pinned at 5 cm
    here while the specs on disk had long since been coarsened to 10, so
    re-running this file silently made the 24-40 m heroes eight times the
    working set they were tuned at -- and a hero that will not build is
    indistinguishable from a hero nobody got round to.
    """
    s, rep = sm.patch(sm.default_spec(),
                      dict(name=name, resolution_cm=cm, **HERO_VARIATION, **changes))
    seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=FORCE)


def main():
    make("hero-natural-arch", kind="rock", cm="10",
         notes="A land bridge with a span you can walk under. The hole is cut "
               "deliberately and then weathered, because the underside of an "
               "arch is the smoothest surface on it and the way it gets that "
               "way is by being worn after the opening exists. A soft bed at "
               "the springing line is what decides where it opens.",
         # An arch is cut out of a FIN. Cut the same opening out of a rounded
         # loaf and what is left is a stone doughnut -- the roof and legs can
         # both be the right thickness and the thing still reads wrong, because
         # the blank was wrong. `flatten` raises z, `elongate` stretches x and
         # narrows y, so between them they make the wall the span needs.
         # These four are NOT the values this file first wrote. The arch was
         # tuned on the built asset afterwards and the tuning lived only on the
         # saved spec, so re-running this file quietly put a finished hero back
         # to a draft. Whatever is on disk and works belongs here.
         **{"rock.size_m": 40.0, "rock.lumps": 3, "rock.spread": 0.18,
            "rock.flatten": 1.7, "rock.elongate": 2.6, "rock.angular": 0.38,
            "rock.facets": 5, "rock.rough": 0.28, "rock.erode": 0.5,
            "rock.arch": 0.66, "rock.bedding": 0.45,
            "rock.bed_thickness_m": 4.5, "rock.bed_dip_deg": 4,
            "rock.caprock": 0.45, "rock.cap_frac": 0.72, "rock.aspect": 0.35,
            "rock.bury": 0.22, "rock.rubble": 0.35, "materials.rock": "sand",
            "biomes.desert": 1.0, "biomes.bare_rock": 0.7,
            "placement.abundance": 0.004, "placement.spacing_m": 1800.0,
            "placement.cluster": 0.1, "placement.slope_max_pct": 35})

    make("hero-arch-colossal", kind="rock", cm="10",
         notes="A colossal land bridge with a tunnel you walk through rather "
               "than a window you look through. The footprint is deliberately "
               "close to square: the bore goes through the SHORTER horizontal "
               "axis, so a stretched rock gives a thin opening in a long wall, "
               "and it is the un-stretched one that gives a long passage.",
         # This hero existed only as a file on disk. Every other one is written
         # here, so re-running this generator regenerated seven heroes and left
         # the eighth alone -- which looks like it worked, and means the one
         # asset whose values lived in a single place was the one place they
         # could be lost. Restored from `specs/hero-arch-colossal.json`; the
         # round-trip check in the commit message is what proves it.
         #
         # Why these numbers, briefly. `arch` 0.9 is near the top of the slider
         # because the opening has to be a tunnel rather than a window, and the
         # bore is taken across the shorter horizontal axis. `flatten` 2.3 and
         # `elongate` 2.4 are what make a fin tall and long enough to bore --
         # the same reasoning as `hero-natural-arch`, one notch less stretched
         # so the passage has length. `angular` is LOW at 0.30, lower than any
         # other rock hero: at 90 m a facet cut is a flat wall tens of metres
         # across, and the flat-cut look that reads as fracture on a 5 m block
         # reads as a quarried slab at this size. `bury` 0.28 is high for the
         # same reason a colonnade's is -- the legs of a 90 m span each need a
         # footing, and a shallower cut leaves the downhill one starting part
         # way up the underside of the blank.
         **{"rock.size_m": 90.0, "rock.lumps": 4, "rock.spread": 0.2,
            "rock.flatten": 2.3, "rock.elongate": 2.4, "rock.angular": 0.30,
            "rock.facets": 6, "rock.rough": 0.3, "rock.erode": 0.45,
            "rock.arch": 0.9, "rock.bedding": 0.5,
            "rock.bed_thickness_m": 9.0, "rock.bed_dip_deg": 5.0,
            "rock.caprock": 0.5, "rock.cap_frac": 0.75, "rock.aspect": 0.35,
            "rock.bury": 0.28, "rock.rubble": 0.25, "materials.rock": "sand",
            "biomes.desert": 1.0, "biomes.bare_rock": 0.7,
            "placement.abundance": 0.001, "placement.spacing_m": 3000.0,
            "placement.cluster": 0.05, "placement.slope_max_pct": 30})

    make("hero-sea-stack", kind="rock", cm="10",
         notes="A needle standing off the coast, sawn in at the tide line. The "
               "notch is the whole asset: without a band of attack at one "
               "height this is an ordinary tall rock in water, and no amount "
               "of curvature weathering makes an undercut because curvature "
               "cannot tell how high off the ground it is.",
         # On the notch settings. The band is NARROW and the attack is
         # moderate, and both of those are the result of overshooting: at
         # spread 1.0 and attack 4.0 the same stone came back sawn clean
         # through, with 26 m of stack hanging in the air above its own
         # rubble and every other number about it still healthy. Rocks skip
         # the orphan sweep so their rubble survives, so nothing downstream
         # would ever have told you. `tools/waistprobe.py` reports it now.
         **{"rock.size_m": 34.0, "rock.lumps": 3, "rock.spread": 0.2,
            "rock.flatten": 3.6, "rock.elongate": 1.15, "rock.angular": 0.6,
            "rock.facets": 6, "rock.rough": 0.34, "rock.erode": 0.55,
            "rock.notch": 2.2, "rock.notch_z_m": 3.0,
            "rock.notch_spread_m": 1.2, "rock.cavernous": 0.25,
            "rock.joint_sets": 2,
            "rock.joint_scatter": 0.1, "rock.joint_dip_deg": 6,
            "rock.bedding": 0.4, "rock.bed_thickness_m": 3.0,
            "rock.aspect": 0.5, "rock.bury": 0.16, "rock.rubble": 0.45,
            "materials.rock": "bedrock",
            "biomes.beach": 1.0, "biomes.bare_rock": 0.5,
            "placement.abundance": 0.006, "placement.spacing_m": 900.0,
            "placement.cluster": 0.45, "placement.slope_max_pct": 25})

    make("hero-basalt-colonnade", kind="rock", cm="10",
         notes="A cliff section of cooling columns. The crack network is a "
               "Voronoi tessellation extruded downward, which is what the "
               "Giant's Causeway actually is; the uneven tops matter as much "
               "as the columns, because a colonnade sawn flat reads as an "
               "extruded shape rather than as stone.",
         # `angular` is over 0.35 deliberately: that is the threshold where the
         # faceting pass spends its first cut on a flat top, and a colonnade
         # needs a plateau of column tops rather than a dome with grooves down
         # it. The stagger is LOW for the same reason -- it is a fraction of
         # the whole grid height, so a quarter of it drops columns by metres
         # and the causeway comes back looking bombed.
         # `bury` is high because the columns are separate prisms and each one
         # needs its own footing. On a domed mass the ones round the rim start
         # partway up the dome's underside and stand on nothing: at the
         # ordinary 0.2 there were seventeen columns hanging in the air. Sinking
         # the mass puts the cut through the widest part and most of them land.
         # Seven still float, which is the honest state of this asset.
         **{"rock.size_m": 30.0, "rock.lumps": 2, "rock.spread": 0.16,
            "rock.flatten": 2.0, "rock.elongate": 1.5, "rock.angular": 0.55,
            "rock.facets": 5, "rock.rough": 0.2, "rock.erode": 0.18,
            "rock.columns": 260, "rock.column_gap_m": 0.35,
            "rock.column_stagger": 0.06, "rock.bury": 0.42,
            "rock.rubble": 0.35, "materials.rock": "bedrock",
            "biomes.bare_rock": 1.0, "biomes.beach": 0.5,
            "biomes.tundra_alpine": 0.45, "biomes.grassland": 0.25,
            "placement.abundance": 0.004, "placement.spacing_m": 1400.0,
            "placement.cluster": 0.7, "placement.slope_max_pct": 55})

    make("hero-tsingy-pinnacles", kind="rock", cm="10",
         notes="A limestone pinnacle forest. Rain works down the joints and is "
               "spent as it goes, so the gaps are wide at the top and nearly "
               "closed at the base and the blocks between them become blades "
               "on a shared plinth. Running water then flutes the flanks. Both "
               "halves are needed — blades without flutes read as a broken "
               "wall, flutes without blades as a scratched boulder.",
         # `joint_dip_deg` 90 is the whole asset and it is not a tilt. The
         # joint frame is built as one plane at the dip plus two perpendicular
         # to it, and `joint_sets` takes them IN ORDER -- so two sets at dip 0
         # means the bedding plane and one vertical set, and the joints cut
         # horizontal shelves. At 90 the first two are both vertical, which is
         # the grid a pinnacle forest is quarried out of. Every earlier attempt
         # here came back as a loaf with a net drawn on it for this one reason.
         **{"rock.size_m": 24.0, "rock.lumps": 2, "rock.spread": 0.15,
            "rock.flatten": 1.9, "rock.elongate": 1.25, "rock.angular": 0.45,
            "rock.facets": 4, "rock.rough": 0.24, "rock.erode": 0.32,
            "rock.joint_sets": 2, "rock.joint_scatter": 0.05,
            "rock.joint_dip_deg": 90,
            "rock.block_size_m": 3.0, "rock.block_relief_m": 1.4,
            "rock.joint_taper": 0.9, "rock.flutes": 0.8,
            "rock.flute_width_m": 0.55, "rock.pans": 0.4,
            "rock.pan_depth_m": 0.6, "rock.bury": 0.18, "rock.rubble": 0.3,
            "materials.rock": "rock",
            "biomes.bare_rock": 1.0, "biomes.grassland": 0.3,
            "biomes.rainforest": 0.3,
            "placement.abundance": 0.005, "placement.spacing_m": 1200.0,
            "placement.cluster": 0.8, "placement.slope_max_pct": 45})

    make("hero-balanced-rock", kind="rock",
         notes="A hard cap left standing on a neck the weather ate out from "
               "under it. Two mechanisms curvature knows nothing about, "
               "working together: a caprock that resists while everything "
               "below it goes, and blown sand that only reaches so high and "
               "saws a waist at that height.",
         # Two numbers here are counter-intuitive and both were measured.
         # `cap_frac` is LOW: it is the height the hard rock starts at, and at
         # 0.72 the cap was the nose of an egg rather than a head, so there was
         # nothing wide enough to overhang anything. And `notch_spread_m` is
         # SMALL: the attack is a rate multiplier normalised over the whole
         # exposed surface, so widening the band spends the same budget over
         # more rock and cuts a shallow dish. At spread 1.8 the overhang
         # measured 26%; at 0.7, on the same settings, 90%.
         **{"rock.size_m": 13.0, "rock.lumps": 3, "rock.spread": 0.2,
            "rock.flatten": 1.7, "rock.elongate": 1.1, "rock.angular": 0.4,
            "rock.facets": 4, "rock.rough": 0.3, "rock.erode": 0.85,
            "rock.caprock": 0.95, "rock.cap_frac": 0.45,
            "rock.notch": 4.0, "rock.notch_z_m": 3.6,
            "rock.notch_spread_m": 0.7, "rock.cavernous": 0.3,
            "rock.bedding": 0.4,
            "rock.bed_thickness_m": 2.2, "rock.aspect": 0.4,
            "rock.bury": 0.14, "rock.rubble": 0.5, "materials.rock": "sand",
            "biomes.desert": 1.0, "biomes.bare_rock": 0.65,
            "biomes.savanna": 0.25,
            "placement.abundance": 0.01, "placement.spacing_m": 600.0,
            "placement.cluster": 0.3, "placement.slope_max_pct": 35})

    make("hero-tor-stack", kind="rock",
         notes="A summit tor: granite rotted inward along its joints until "
               "only rounded cores survived, then the rotten rock stripped "
               "away, leaving a heap of boulders still sitting on the old "
               "fracture grid. That grid showing through in how they stack is "
               "what says the tor grew here rather than being dropped here.",
         # `corestone` sits just under the value where the cores come apart.
         # A core radius of half a block is the ball that exactly fits inside
         # it, which is 0.51 on this slider; below that the boulders touch and
         # the tor is one leaning stack, above it they separate and hang in the
         # air, because nothing in this pipeline drops a loose block onto the
         # one below it. At 0.55 the same tor came back as twenty-three pieces
         # with five of them floating.
         **{"rock.size_m": 17.0, "rock.lumps": 3, "rock.spread": 0.22,
            "rock.flatten": 1.7, "rock.elongate": 1.2, "rock.angular": 0.5,
            "rock.facets": 5, "rock.rough": 0.3, "rock.erode": 0.35,
            "rock.joint_sets": 3, "rock.joint_scatter": 0.07,
            "rock.joint_dip_deg": 7, "rock.block_size_m": 4.0,
            "rock.corestone": 0.5, "rock.settle_m": 0.3,
            "rock.aspect": 0.35, "rock.bury": 0.2, "rock.rubble": 0.3,
            "materials.rock": "rock",
            "biomes.bare_rock": 1.0, "biomes.tundra_alpine": 0.9,
            "biomes.grassland": 0.35, "biomes.taiga": 0.4,
            "placement.abundance": 0.008, "placement.spacing_m": 800.0,
            "placement.cluster": 0.35, "placement.slope_max_pct": 55,
            "placement.elev_min_m": 300})

    make("hero-sequoia", kind="tree",
         notes="An 80 m giant. Everything about it is scaled, not just the "
               "height: the growth step, the reach between branches and the "
               "twig radius all move with the tree, because a 35 cm segment "
               "on an 80 m trunk is a hundred thousand nodes and a week of "
               "waiting. The root flare falls off with the tree rather than "
               "over a fixed metre and a half, which is what lets the same "
               "setting read correctly at this size.",
         # The trunk is the asset. A sequoia is recognised from a long way off
         # by a fluted cinnamon column with a flared foot and a clear bole
         # halfway up, and the crown is a narrow spire behind it -- so the
         # crown is deliberately smaller and set higher than the first pass
         # had it, where a 13 m ball of foliage on a bare pole read as
         # broccoli. `trunk.lobes` is what puts the fluting in.
         **{"height_m": 80.0, "crown.shape": "spire", "crown.radius_m": 10.0,
            "crown.height_frac": 0.55, "crown.center_frac": 0.74,
            "crown.shell_upper": 0.45, "crown.shell_lower": 0.35,
            "crown.asymmetry": 0.22, "crown.offset": 0.12,
            "crown.points": 5200, "crown.squash": 1.0,
            "trunk.radius_base_m": 4.2, "trunk.clear_frac": 0.5,
            "trunk.wander": 0.08, "trunk.buttress": 0.75,
            "trunk.lobes": 7, "trunk.lobe_depth": 0.14,
            "trunk.lean_deg": 1.5,
            "growth.model": "colonize", "growth.step_m": 1.1,
            "growth.influence_m": 11.0, "growth.kill_m": 2.6,
            "growth.tip_radius_m": 0.22, "growth.shade": 0.5,
            "foliage.habit": "radial",
            "foliage.clump_radius_m": 1.5, "foliage.separation": 1.6,
            "foliage.clustering": 0.55, "foliage.top_bias": 0.45,
            "foliage.density": 0.55, "foliage.droop_m": 0.4,
            "materials.bark": "bark", "materials.core": "heartwood",
            "materials.leaf": "leaf_needle",
            "biomes.temperate_forest": 1.0, "biomes.taiga": 0.5,
            "placement.abundance": 0.006, "placement.spacing_m": 700.0,
            "placement.cluster": 0.5, "placement.slope_max_pct": 45,
            "placement.water_max_m": 0})


if __name__ == "__main__":
    main()
