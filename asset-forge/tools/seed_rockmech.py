"""Author the rock classes the new mechanisms make possible.

Each of these exists because the pipeline gained something it could not express
before, not because a slider was nudged. The mechanism each one is here to
exercise is named in its notes, so that if a species stops looking like itself
there is a single thing to go and check.

    desert-hoodoo         caprock + notch     hard cap over an eaten neck
    wave-cut-stack        notch               attack in the tidal band only
    ventifact-boulder     notch + aspect      sand-blasted on one side, low down
    corestone-tor         corestone + settle  blocks rotted from their joints
    limestone-pinnacles   joint_taper+flutes  blades, fluted by running water
    karren-pavement       flutes + pans       runnels and standing-water hollows
    veined-granite        veins               harder sheets standing proud
    honeycomb-tafoni      rind + cavernous    hollows with an overhanging lip
    fault-breccia         clasts              lumps in a softer matrix
    cross-bedded-butte    cross_beds          wedges of layers cutting each other
    desert-arch           arch                a hole clean through
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
import seedspec
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"
FORCE = seedspec.parse_force(sys.argv[1:])


def make(name, **changes):
    s, rep = sm.patch(sm.default_spec(),
                      dict(name=name, kind="rock", resolution_cm="5", **changes))
    seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=FORCE, width=22)


def main():
    make("desert-hoodoo",
         notes="A hard cap on a thin neck. Weathering that only knows about "
               "curvature cannot make this: it attacks corners, and a neck is "
               "not a corner. What carves it is two things curvature is blind "
               "to — a caprock that resists while everything under it goes, "
               "and blown sand that only reaches the first half metre and saws "
               "a waist at that height.",
         **{"rock.size_m": 4.0, "rock.lumps": 3, "rock.spread": 0.2,
            "rock.flatten": 1.9, "rock.elongate": 1.05, "rock.angular": 0.35,
            "rock.facets": 4, "rock.rough": 0.3, "rock.erode": 0.7,
            "rock.caprock": 0.85, "rock.cap_frac": 0.7,
            "rock.notch": 2.2, "rock.notch_z_m": 0.9, "rock.notch_spread_m": 0.5,
            "rock.bedding": 0.35, "rock.bed_thickness_m": 0.6,
            "rock.bury": 0.16, "rock.rubble": 0.4, "materials.rock": "sand",
            "biomes.desert": 1.0, "biomes.bare_rock": 0.6,
            "placement.abundance": 0.08, "placement.spacing_m": 45.0,
            "placement.cluster": 0.6, "placement.slope_max_pct": 40})

    make("wave-cut-stack",
         notes="A sea stack, undercut in the band the waves reach and nowhere "
               "else. The notch is the whole shape; without it this is an "
               "ordinary tall rock standing in water.",
         **{"rock.size_m": 5.0, "rock.lumps": 3, "rock.spread": 0.22,
            "rock.flatten": 2.0, "rock.elongate": 1.2, "rock.angular": 0.6,
            "rock.facets": 5, "rock.rough": 0.36, "rock.erode": 0.6,
            "rock.notch": 3.0, "rock.notch_z_m": 1.1, "rock.notch_spread_m": 0.35,
            "rock.joint_sets": 2, "rock.joint_scatter": 0.1,
            "rock.bedding": 0.4, "rock.bed_thickness_m": 0.7,
            "rock.bury": 0.2, "rock.rubble": 0.5, "materials.rock": "bedrock",
            "biomes.beach": 1.0, "biomes.bare_rock": 0.5,
            "placement.abundance": 0.05, "placement.spacing_m": 70.0,
            "placement.cluster": 0.5, "placement.slope_max_pct": 30})

    make("ventifact-boulder",
         notes="Sand blasted from one prevailing direction, and only as high as "
               "it bounces. Gives a stone with a scoured windward flank and a "
               "waist near the ground, which is the pair of marks that says "
               "desert rather than merely dry.",
         **{"rock.size_m": 1.8, "rock.lumps": 4, "rock.spread": 0.35,
            "rock.flatten": 0.8, "rock.elongate": 1.4, "rock.angular": 0.55,
            "rock.facets": 5, "rock.rough": 0.38, "rock.erode": 0.5,
            "rock.aspect": 0.8, "rock.notch": 1.6, "rock.notch_z_m": 0.2,
            "rock.notch_spread_m": 0.14, "rock.bury": 0.24, "rock.rubble": 0.25,
            "materials.rock": "rock",
            "biomes.desert": 1.0, "biomes.savanna": 0.3, "biomes.bare_rock": 0.5,
            "placement.abundance": 0.2, "placement.spacing_m": 22.0,
            "placement.cluster": 0.35, "placement.slope_max_pct": 45})

    make("corestone-tor",
         notes="Granite rotted inward along its joints until only a rounded "
               "core survives in the middle of each block, then the rotten "
               "material stripped away. The result is a stack of boulders that "
               "still sits on the old fracture grid, which is what tells you it "
               "grew there rather than being dropped.",
         **{"rock.size_m": 4.5, "rock.lumps": 3, "rock.spread": 0.25,
            "rock.flatten": 1.15, "rock.elongate": 1.2, "rock.angular": 0.5,
            "rock.facets": 5, "rock.rough": 0.34, "rock.erode": 0.35,
            "rock.joint_sets": 3, "rock.joint_scatter": 0.08,
            "rock.joint_dip_deg": 6, "rock.block_size_m": 1.3,
            "rock.corestone": 0.42, "rock.settle_m": 0.12,
            "rock.bury": 0.24, "rock.rubble": 0.55, "materials.rock": "rock",
            "biomes.bare_rock": 1.0, "biomes.tundra_alpine": 0.85,
            "biomes.grassland": 0.45, "biomes.taiga": 0.5,
            "placement.abundance": 0.12, "placement.spacing_m": 40.0,
            "placement.cluster": 0.45, "placement.slope_max_pct": 55,
            "placement.elev_min_m": 120})

    make("limestone-pinnacles",
         notes="Rain works its way down the joints of a limestone pavement and "
               "is spent as it goes, so the gaps are wide at the top and nearly "
               "closed at the base — the blocks become blades on a shared "
               "plinth. Running water then flutes their flanks. Both halves are "
               "needed: blades without flutes read as a broken wall.",
         **{"rock.size_m": 4.0, "rock.lumps": 2, "rock.spread": 0.18,
            "rock.flatten": 1.7, "rock.elongate": 1.15, "rock.angular": 0.45,
            "rock.facets": 4, "rock.rough": 0.28, "rock.erode": 0.3,
            "rock.joint_sets": 2, "rock.joint_scatter": 0.06,
            "rock.block_size_m": 0.9, "rock.block_relief_m": 0.22,
            "rock.joint_taper": 0.85, "rock.flutes": 0.7,
            "rock.flute_width_m": 0.28, "rock.bury": 0.2, "rock.rubble": 0.3,
            "materials.rock": "rock",
            "biomes.bare_rock": 1.0, "biomes.grassland": 0.4,
            "biomes.temperate_forest": 0.3,
            "placement.abundance": 0.07, "placement.spacing_m": 55.0,
            "placement.cluster": 0.8, "placement.slope_max_pct": 45})

    make("karren-pavement",
         notes="A low limestone block worked entirely by running and standing "
               "water: runnels down the flanks, flat-floored pans on top. The "
               "only weathering here that knows which way is down, which is why "
               "nothing in the curvature pass resembles it.",
         **{"rock.size_m": 2.6, "rock.lumps": 3, "rock.spread": 0.3,
            "rock.flatten": 0.45, "rock.elongate": 1.4, "rock.angular": 0.6,
            "rock.facets": 5, "rock.rough": 0.26, "rock.erode": 0.28,
            "rock.flutes": 0.85, "rock.flute_width_m": 0.22,
            "rock.pans": 0.8, "rock.pan_depth_m": 0.22,
            "rock.joint_sets": 2, "rock.block_size_m": 1.1,
            "rock.block_relief_m": 0.1,
            "rock.bury": 0.3, "rock.rubble": 0.15, "materials.rock": "rock",
            "biomes.bare_rock": 1.0, "biomes.grassland": 0.5,
            "biomes.tundra_alpine": 0.5,
            "placement.abundance": 0.18, "placement.spacing_m": 25.0,
            "placement.cluster": 0.7, "placement.slope_max_pct": 35})

    make("veined-granite",
         notes="Quartz veins cutting the mass and standing proud once the rock "
               "around them has gone back. A continuous line across a face "
               "carries much further than its width suggests — the eye tracks "
               "lines at a resolution where it would lose a bump of the same "
               "size. Needs real weathering to show: a vein can only stand as "
               "proud as its surroundings have retreated.",
         **{"rock.size_m": 2.4, "rock.lumps": 4, "rock.spread": 0.38,
            "rock.flatten": 0.85, "rock.elongate": 1.2, "rock.angular": 0.4,
            "rock.facets": 4, "rock.rough": 0.36, "rock.erode": 0.6,
            "rock.veins": 2, "rock.vein_width_m": 0.14,
            "rock.vein_hardness": 4.0, "rock.joint_sets": 3,
            "rock.joint_scatter": 0.14, "rock.bury": 0.22, "rock.rubble": 0.3,
            "materials.rock": "rock",
            "biomes.bare_rock": 0.9, "biomes.tundra_alpine": 0.7,
            "biomes.temperate_forest": 0.5, "biomes.taiga": 0.5,
            "placement.abundance": 0.2, "placement.spacing_m": 20.0,
            "placement.cluster": 0.3, "placement.slope_max_pct": 55})

    make("honeycomb-tafoni",
         notes="Hollows with a thin lip hanging over them. Cavernous weathering "
               "alone digs the hollow and then retreats the rim along with it, "
               "leaving a smooth bowl; what makes it read as tafoni is a "
               "case-hardened skin that survives around the opening while the "
               "soft rock behind it scoops out.",
         **{"rock.size_m": 2.2, "rock.lumps": 4, "rock.spread": 0.33,
            "rock.flatten": 0.95, "rock.elongate": 1.25, "rock.angular": 0.45,
            "rock.facets": 4, "rock.rough": 0.34, "rock.erode": 0.65,
            "rock.cavernous": 0.85, "rock.rind": 0.75, "rock.rind_m": 0.09,
            "rock.aspect": 0.5, "rock.bedding": 0.25,
            "rock.bed_thickness_m": 0.4, "rock.bury": 0.24, "rock.rubble": 0.3,
            "materials.rock": "sand",
            "biomes.desert": 0.9, "biomes.beach": 0.6, "biomes.bare_rock": 0.7,
            "placement.abundance": 0.16, "placement.spacing_m": 26.0,
            "placement.cluster": 0.5, "placement.slope_max_pct": 50})

    make("fault-breccia",
         notes="Angular fragments set in a finer matrix, broken along the same "
               "joints as the parent rock. As the matrix goes back some clasts "
               "stand out and others rot away and leave sockets, and it is that "
               "mix of bumps and holes that reads as breccia. Note the fragments "
               "are decimetre-scale on purpose — a pebble conglomerate would be "
               "one voxel a clast and is a texture problem, not this.",
         **{"rock.size_m": 3.0, "rock.lumps": 3, "rock.spread": 0.28,
            "rock.flatten": 0.9, "rock.elongate": 1.35, "rock.angular": 0.65,
            "rock.facets": 6, "rock.rough": 0.32, "rock.erode": 0.55,
            "rock.clasts": 320, "rock.clast_size_m": 0.28,
            "rock.clast_hardness": 3.0, "rock.joint_sets": 3,
            "rock.joint_scatter": 0.12, "rock.bury": 0.25, "rock.rubble": 0.45,
            "materials.rock": "bedrock",
            "biomes.bare_rock": 1.0, "biomes.tundra_alpine": 0.6,
            "biomes.desert": 0.4, "biomes.grassland": 0.3,
            "placement.abundance": 0.12, "placement.spacing_m": 34.0,
            "placement.cluster": 0.6, "placement.slope_max_pct": 60})

    make("cross-bedded-butte",
         notes="Stacked wedges of steeply dipping layers, each planed off by "
               "the one above and each dipping a different way. Ordinary "
               "bedding keeps every layer parallel forever and can never cut "
               "one layer off against another, and that truncation is the "
               "entire signature of wind-laid sandstone.",
         **{"rock.size_m": 4.2, "rock.lumps": 2, "rock.spread": 0.18,
            "rock.flatten": 1.5, "rock.elongate": 1.3, "rock.angular": 0.55,
            "rock.facets": 5, "rock.rough": 0.3, "rock.erode": 0.5,
            "rock.cross_beds": 4, "rock.bed_thickness_m": 0.22,
            "rock.bedding": 0.55, "rock.aspect": 0.4,
            "rock.bury": 0.18, "rock.rubble": 0.4, "materials.rock": "sand",
            "biomes.desert": 1.0, "biomes.bare_rock": 0.7, "biomes.savanna": 0.3,
            "placement.abundance": 0.07, "placement.spacing_m": 50.0,
            "placement.cluster": 0.5, "placement.slope_max_pct": 40})

    make("desert-arch",
         notes="A fin with a hole through it. Nothing else in this pipeline can "
               "open one — hollows eaten from both flanks meet only by luck, "
               "and a fin thin enough for them to meet usually falls down "
               "first. The span is cut deliberately and then weathered, because "
               "the underside of an arch is the smoothest surface on it and the "
               "way it gets that way is by being worn after the hole exists.",
         **{"rock.size_m": 6.5, "rock.lumps": 3, "rock.spread": 0.2,
            "rock.flatten": 1.5, "rock.elongate": 2.3, "rock.angular": 0.5,
            "rock.facets": 5, "rock.rough": 0.3, "rock.erode": 0.55,
            "rock.arch": 0.8, "rock.bedding": 0.4,
            "rock.bed_thickness_m": 0.8, "rock.caprock": 0.4,
            "rock.bury": 0.16, "rock.rubble": 0.5, "materials.rock": "sand",
            "biomes.desert": 1.0, "biomes.bare_rock": 0.6,
            "placement.abundance": 0.03, "placement.spacing_m": 120.0,
            "placement.cluster": 0.2, "placement.slope_max_pct": 35})


if __name__ == "__main__":
    main()
