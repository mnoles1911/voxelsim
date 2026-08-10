"""One-off: tag existing specs as trees and author the rock/bush/gap species."""
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def make(name, **changes):
    s, rep = sm.patch(sm.default_spec(), dict(name=name, **changes))
    sm.save(s, SPECS / f"{name}.json")
    bad = [w for w in rep.warnings if "tip_radius" not in w]
    print(f"  {name:<20} " + ("! " + bad[0] if bad else "ok"))


def main():
    for fp in sorted(SPECS.glob("*.json")):
        s, _ = sm.load(fp)
        if sm.get(s, "kind") != "tree":
            s2, _ = sm.patch(s, {"kind": "tree"})
            sm.save(s2, fp)

    print("rocks:")
    make("granite-boulder", kind="rock",
         notes="Weathered granite boulder. Rocks need no new engine material - this is "
               "MAT_ROCK, the same stone the terrain is already made of.",
         **{"rock.size_m": 2.4, "rock.lumps": 7, "rock.spread": 0.55, "rock.angular": 0.5,
            "rock.facets": 4, "rock.erode": 0.45, "rock.flatten": 0.78, "rock.bury": 0.25,
            "rock.rubble": 0.2, "materials.rock": "rock",
            "biomes.bare_rock": 1.0, "biomes.tundra_alpine": 0.8,
            "biomes.temperate_forest": 0.35, "biomes.taiga": 0.4, "biomes.grassland": 0.3,
            "placement.abundance": 0.5, "placement.spacing_m": 9.0, "placement.cluster": 0.5,
            "placement.slope_max_pct": 70, "placement.elev_max_m": 4000})
    make("river-cobble", kind="rock",
         notes="Rounded waterworn stone: low angularity, heavy erosion, clustered by water.",
         **{"rock.size_m": 0.9, "rock.lumps": 4, "rock.spread": 0.5, "rock.angular": 0.12,
            "rock.facets": 2, "rock.erode": 0.55, "rock.flatten": 0.6, "rock.bury": 0.3,
            "rock.rubble": 0.35, "materials.rock": "gravel",
            "biomes.beach": 1.0, "biomes.temperate_forest": 0.5, "biomes.grassland": 0.35,
            "placement.abundance": 0.9, "placement.spacing_m": 2.0, "placement.cluster": 0.85,
            "placement.water_max_m": 40, "placement.slope_max_pct": 25,
            "placement.elev_max_m": 900})
    make("limestone-slab", kind="rock",
         notes="Flat bedded slab, part sunk. Reads as exposed bedrock in open country.",
         **{"rock.size_m": 3.4, "rock.lumps": 3, "rock.spread": 0.4, "rock.angular": 0.8,
            "rock.facets": 5, "rock.erode": 0.25, "rock.flatten": 0.3, "rock.elongate": 1.8,
            "rock.bury": 0.35, "materials.rock": "bedrock",
            "biomes.grassland": 1.0, "biomes.savanna": 0.6, "biomes.bare_rock": 0.5,
            "biomes.desert": 0.4,
            "placement.abundance": 0.35, "placement.spacing_m": 14.0,
            "placement.cluster": 0.35, "placement.slope_max_pct": 30})
    make("standing-stone", kind="rock",
         notes="Tall fractured monolith. A third buried, so it looks settled rather than dropped.",
         **{"rock.size_m": 3.2, "rock.lumps": 4, "rock.spread": 0.3, "rock.angular": 0.85,
            "rock.facets": 6, "rock.erode": 0.3, "rock.flatten": 1.8, "rock.elongate": 0.8,
            "rock.bury": 0.32, "materials.rock": "rock",
            "biomes.grassland": 0.7, "biomes.tundra_alpine": 0.6, "biomes.bare_rock": 0.6,
            "biomes.taiga": 0.3,
            "placement.abundance": 0.12, "placement.spacing_m": 40.0,
            "placement.cluster": 0.1, "placement.slope_max_pct": 40})
    make("alpine-scree", kind="rock",
         notes="Small angular fragments for talus slopes. Authored tiny and clustered hard.",
         **{"rock.size_m": 0.5, "rock.lumps": 3, "rock.spread": 0.45, "rock.angular": 0.95,
            "rock.facets": 6, "rock.erode": 0.15, "rock.flatten": 0.7, "rock.bury": 0.2,
            "rock.rubble": 0.5, "materials.rock": "gravel",
            "biomes.tundra_alpine": 1.0, "biomes.bare_rock": 1.0, "biomes.taiga": 0.3,
            "placement.abundance": 1.0, "placement.spacing_m": 1.2, "placement.cluster": 0.95,
            "placement.slope_max_pct": 70, "placement.elev_min_m": 300})

    print("bushes:")
    make("bramble-thicket", kind="bush", height_m=1.5,
         notes="Dense low thicket. Same growth machinery as a tree, authored short with "
               "branches all the way to the ground.",
         **{"trunk.radius_base_m": 0.06, "trunk.clear_frac": 0.02, "trunk.wander": 0.5,
            "trunk.lean_deg": 8, "crown.shape": "sphere", "crown.radius_m": 1.3,
            "crown.height_frac": 0.9, "crown.center_frac": 0.5, "crown.points": 700,
            "crown.shell": 0.2, "growth.step_m": 0.12, "growth.influence_m": 0.9,
            "growth.kill_m": 0.2, "growth.gravity": -0.05, "growth.tip_radius_m": 0.02,
            "growth.jitter": 0.14, "foliage.min_order": 1, "foliage.clump_radius_m": 0.22,
            "foliage.separation": 1.5, "foliage.density": 0.62, "foliage.coverage": 0.9,
            "biomes.temperate_forest": 0.9, "biomes.grassland": 1.0, "biomes.taiga": 0.3,
            "placement.abundance": 0.9, "placement.spacing_m": 1.8, "placement.cluster": 0.85,
            "placement.slope_max_pct": 55})
    make("juniper-scrub", kind="bush", height_m=1.1,
         notes="Hardy cold-country scrub, wind-shaped and ground-hugging.",
         **{"trunk.radius_base_m": 0.07, "trunk.clear_frac": 0.03, "trunk.wander": 0.65,
            "trunk.lean_deg": 22, "crown.shape": "wedge", "crown.radius_m": 1.5,
            "crown.height_frac": 0.85, "crown.center_frac": 0.45, "crown.points": 600,
            "crown.shell": 0.45, "crown.lean_deg": 18, "growth.step_m": 0.11,
            "growth.influence_m": 0.9, "growth.kill_m": 0.2, "growth.gravity": -0.2,
            "growth.tip_radius_m": 0.02, "growth.jitter": 0.16, "foliage.min_order": 1,
            "foliage.clump_radius_m": 0.2, "foliage.separation": 1.4, "foliage.density": 0.7,
            "foliage.coverage": 0.92, "materials.leaf": "leaf_needle",
            "biomes.taiga": 1.0, "biomes.tundra_alpine": 0.9, "biomes.grassland": 0.25,
            "placement.abundance": 0.8, "placement.spacing_m": 2.2, "placement.cluster": 0.7,
            "placement.slope_max_pct": 60, "placement.elev_max_m": 2600})
    make("desert-shrub", kind="bush", height_m=1.0,
         notes="Sparse woody desert scrub: mostly bare stems under a thin dry canopy.",
         **{"trunk.radius_base_m": 0.05, "trunk.clear_frac": 0.08, "trunk.wander": 0.7,
            "trunk.lean_deg": 12, "crown.shape": "vase", "crown.radius_m": 0.9,
            "crown.height_frac": 0.85, "crown.center_frac": 0.55, "crown.points": 380,
            "crown.shell": 0.55, "growth.step_m": 0.11, "growth.influence_m": 0.9,
            "growth.kill_m": 0.24, "growth.gravity": 0.02, "growth.tip_radius_m": 0.02,
            "growth.jitter": 0.2, "foliage.min_order": 1, "foliage.clump_radius_m": 0.16,
            "foliage.separation": 2.0, "foliage.density": 0.45, "foliage.coverage": 0.6,
            "materials.leaf": "leaf_dry",
            "biomes.desert": 1.0, "biomes.savanna": 0.7, "biomes.grassland": 0.3,
            "placement.abundance": 0.8, "placement.spacing_m": 3.5, "placement.cluster": 0.4,
            "placement.slope_max_pct": 50})
    make("coastal-scrub", kind="bush", height_m=0.9,
         notes="Salt-tolerant wind-flattened scrub for the beach band.",
         **{"trunk.radius_base_m": 0.05, "trunk.clear_frac": 0.02, "trunk.wander": 0.6,
            "trunk.lean_deg": 26, "crown.shape": "wedge", "crown.radius_m": 1.4,
            "crown.height_frac": 0.8, "crown.center_frac": 0.45, "crown.points": 520,
            "crown.shell": 0.5, "crown.lean_deg": 22, "growth.step_m": 0.1,
            "growth.influence_m": 0.85, "growth.kill_m": 0.2, "growth.gravity": -0.15,
            "growth.tip_radius_m": 0.02, "growth.jitter": 0.15, "foliage.min_order": 1,
            "foliage.clump_radius_m": 0.18, "foliage.separation": 1.5, "foliage.density": 0.6,
            "foliage.coverage": 0.88,
            "biomes.beach": 1.0, "biomes.grassland": 0.4,
            "placement.abundance": 0.85, "placement.spacing_m": 2.0, "placement.cluster": 0.8,
            "placement.elev_max_m": 60, "placement.slope_max_pct": 45})

    print("trees for the empty biomes:")
    make("alpine-krummholz", kind="tree", height_m=1.8,
         notes="Treeline krummholz: a conifer beaten flat by wind and snow. The one "
               "species authored FOR tundra/alpine rather than borrowed into it.",
         **{"trunk.radius_base_m": 0.11, "trunk.clear_frac": 0.04, "trunk.wander": 0.5,
            "trunk.lean_deg": 32, "trunk.lean_dir_deg": 210, "crown.shape": "wedge",
            "crown.radius_m": 2.2, "crown.height_frac": 0.85, "crown.center_frac": 0.45,
            "crown.shell": 0.5, "crown.lean_deg": 34, "crown.lean_dir_deg": 210,
            "growth.model": "whorl", "growth.step_m": 0.12, "growth.tip_radius_m": 0.02,
            "growth.jitter": 0.1, "whorl.count": 7, "whorl.branches": 5, "whorl.droop": 0.15,
            "whorl.rise": 0.05, "whorl.sub": 1, "whorl.irregular": 0.5, "whorl.leader": 0.02,
            "foliage.min_order": 1, "foliage.clump_radius_m": 0.2, "foliage.separation": 1.4,
            "foliage.density": 0.72, "foliage.coverage": 0.95, "materials.leaf": "leaf_needle",
            "biomes.tundra_alpine": 1.0, "biomes.taiga": 0.6,
            "placement.abundance": 0.8, "placement.spacing_m": 2.6, "placement.cluster": 0.8,
            "placement.elev_min_m": 600, "placement.elev_max_m": 3200,
            "placement.slope_max_pct": 60})
    make("field-elm", kind="tree", height_m=11.0,
         notes="Lone spreading hedgerow tree for open country. Authored FOR grassland "
               "rather than borrowed from the forest set.",
         **{"trunk.radius_base_m": 0.42, "trunk.clear_frac": 0.34, "trunk.wander": 0.16,
            "crown.shape": "vase", "crown.radius_m": 5.8, "crown.height_frac": 0.6,
            "crown.center_frac": 0.72, "crown.shell": 0.3, "crown.points": 1500,
            "growth.step_m": 0.3, "growth.influence_m": 2.6, "growth.kill_m": 0.5,
            "growth.gravity": -0.06, "growth.tip_radius_m": 0.045,
            "foliage.clump_radius_m": 0.78, "foliage.separation": 1.9, "foliage.density": 0.6,
            "roots.count": 4, "roots.length_m": 1.3, "roots.rise": 0.28,
            "roots.thickness": 0.4,
            "biomes.grassland": 1.0, "biomes.temperate_forest": 0.4,
            "placement.abundance": 0.5, "placement.spacing_m": 18.0, "placement.cluster": 0.15,
            "placement.elev_max_m": 900, "placement.slope_max_pct": 30})


if __name__ == "__main__":
    main()
