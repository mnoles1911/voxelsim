"""Author the large boulders: 4.5 m to 9 m, a size class above anything else.

The existing rocks top out at a 3.4 m slab, which is scenery you walk past. A
boulder in the 5-9 m range is scenery you walk AROUND -- it blocks a line, it
casts a shadow you stand in, it is a landmark rather than a detail. That is a
different job and it wants its own species rather than the granite boulder
slider dragged right.

All at 5 cm, like everything else. Costs and one limit worth knowing:

- A 6 m boulder is 120 voxels on its longest axis and a few hundred thousand
  voxels of stone. That is a large tree's worth, for one object.
- The .vox format caps a model at 256 voxels per axis, which at 5 cm is 12.8 m.
  Nothing here reaches it; the biggest is 9 m at 180.
- Rocks need NO new engine material. These are MAT_ROCK and MAT_BEDROCK, which
  the terrain is already made of, so the whole class is one blocker (the
  streaming bound) from the world rather than two.
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
    seedspec.write(s, SPECS / f"{name}.json", rep.warnings, force=FORCE)


def main():
    make("glacial-erratic",
         notes="A 5 m rounded granite block dropped by ice and left sitting "
               "proud on open ground. Low angularity and heavy roughness: it "
               "has been weathering in the open for a very long time.",
         **{"rock.size_m": 5.0, "rock.lumps": 8, "rock.spread": 0.5,
            "rock.flatten": 0.82, "rock.elongate": 1.2, "rock.angular": 0.3,
            "rock.facets": 3, "rock.rough": 0.6, "rock.erode": 0.3,
            "rock.bury": 0.22, "rock.rubble": 0.25, "materials.rock": "rock",
            "biomes.grassland": 1.0, "biomes.tundra_alpine": 0.9,
            "biomes.taiga": 0.7, "biomes.temperate_forest": 0.5,
            "biomes.bare_rock": 0.6,
            "placement.abundance": 0.1, "placement.spacing_m": 45.0,
            "placement.cluster": 0.15, "placement.slope_max_pct": 35,
            "placement.elev_max_m": 2600})

    make("cliff-fall-block",
         notes="A 6.5 m block that came off a cliff and has not travelled far: "
               "sharply fractured, barely eroded, sitting where it landed. "
               "Belongs at the foot of steep ground.",
         **{"rock.size_m": 6.5, "rock.lumps": 5, "rock.spread": 0.35,
            "rock.flatten": 0.95, "rock.elongate": 1.1, "rock.angular": 0.92,
            "rock.facets": 7, "rock.rough": 0.3, "rock.erode": 0.12,
            "rock.bury": 0.16, "rock.rubble": 0.55, "materials.rock": "rock",
            "biomes.bare_rock": 1.0, "biomes.tundra_alpine": 0.9,
            "biomes.taiga": 0.4, "biomes.temperate_forest": 0.3,
            "placement.abundance": 0.18, "placement.spacing_m": 30.0,
            "placement.cluster": 0.7, "placement.slope_max_pct": 70,
            "placement.elev_min_m": 200})

    make("desert-mesa-block",
         notes="Wind-carved sandstone, 5.5 m and wide rather than tall, with "
               "the flat bedded top the desert gives everything. Sits shallow "
               "because there is no soil to sink into.",
         **{"rock.size_m": 5.5, "rock.lumps": 4, "rock.spread": 0.4,
            "rock.flatten": 0.55, "rock.elongate": 1.6, "rock.angular": 0.7,
            "rock.facets": 5, "rock.rough": 0.42, "rock.erode": 0.35,
            "rock.bury": 0.14, "rock.rubble": 0.3, "materials.rock": "sand",
            "biomes.desert": 1.0, "biomes.savanna": 0.6,
            "biomes.bare_rock": 0.5, "biomes.grassland": 0.2,
            "placement.abundance": 0.15, "placement.spacing_m": 40.0,
            "placement.cluster": 0.4, "placement.slope_max_pct": 30})

    make("mossy-forest-boulder",
         notes="A 4.5 m rounded block under the canopy, softened by a long time "
               "in damp shade. The most eroded of the large rocks and the one "
               "that sits deepest, because forest soil accumulates around it.",
         **{"rock.size_m": 4.5, "rock.lumps": 9, "rock.spread": 0.55,
            "rock.flatten": 0.75, "rock.elongate": 1.3, "rock.angular": 0.22,
            "rock.facets": 3, "rock.rough": 0.65, "rock.erode": 0.42,
            "rock.bury": 0.34, "rock.rubble": 0.2, "materials.rock": "rock",
            "biomes.temperate_forest": 1.0, "biomes.rainforest": 0.8,
            "biomes.taiga": 0.6, "biomes.grassland": 0.3,
            "placement.abundance": 0.2, "placement.spacing_m": 28.0,
            "placement.cluster": 0.45, "placement.slope_max_pct": 45,
            "placement.elev_max_m": 1600})

    make("summit-tor",
         notes="The biggest: a 9 m stack of weathered blocks on high open "
               "ground, tall rather than wide. A landmark you navigate by, so "
               "it is authored rare and widely spaced.",
         **{"rock.size_m": 9.0, "rock.lumps": 7, "rock.spread": 0.42,
            "rock.flatten": 1.35, "rock.elongate": 0.9, "rock.angular": 0.75,
            "rock.facets": 8, "rock.rough": 0.5, "rock.erode": 0.28,
            "rock.bury": 0.2, "rock.rubble": 0.45, "materials.rock": "bedrock",
            "biomes.bare_rock": 1.0, "biomes.tundra_alpine": 0.9,
            "biomes.grassland": 0.25, "biomes.taiga": 0.3,
            "placement.abundance": 0.05, "placement.spacing_m": 120.0,
            "placement.cluster": 0.1, "placement.slope_max_pct": 60,
            "placement.elev_min_m": 400})


if __name__ == "__main__":
    main()
