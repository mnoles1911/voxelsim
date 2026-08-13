"""Author the geologically-driven rock types.

Each of these exists to exercise one mechanism that the original generator had
no way to express. They are not re-tunings of the granite boulder; they are rock
classes it could not make.

    tafoni-sandstone        rock.cavernous     runaway pitting
    banded-sandstone-ledge  rock.bedding       differential erosion
    jointed-granite-tor     rock.joint_sets    shared fracture orientations
    fractured-outcrop       rock.block_relief  open joints between blocks
    basalt-colonnade        rock.columns       cooling-contraction network
    exfoliating-dome        rock.exfoliate     shells parallel to the surface
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
    make("jointed-granite-tor",
         notes="Granite fractured along three joint sets, so every face shares "
               "one of three orientations. That correlation between faces is "
               "what makes it read as quarried rather than lumpy, and no amount "
               "of independent random faceting produces it.",
         **{"rock.size_m": 4.0, "rock.lumps": 4, "rock.spread": 0.3,
            "rock.flatten": 1.0, "rock.elongate": 1.1, "rock.angular": 0.85,
            "rock.facets": 8, "rock.rough": 0.32, "rock.erode": 0.3,
            "rock.cavernous": 0.15, "rock.joint_sets": 3,
            "rock.joint_scatter": 0.09, "rock.joint_dip_deg": 8,
            "rock.bury": 0.2, "rock.rubble": 0.35, "materials.rock": "rock",
            "biomes.bare_rock": 1.0, "biomes.tundra_alpine": 0.9,
            "biomes.grassland": 0.4, "biomes.taiga": 0.5,
            "placement.abundance": 0.15, "placement.spacing_m": 35.0,
            "placement.cluster": 0.3, "placement.slope_max_pct": 60,
            "placement.elev_min_m": 150})

    make("fractured-outcrop",
         notes="Bedrock broken into separate blocks with the joints standing "
               "open. The visible GAP is the whole effect: a continuous mass "
               "with faces drawn on it still reads as one stone.",
         **{"rock.size_m": 5.5, "rock.lumps": 3, "rock.spread": 0.28,
            "rock.flatten": 0.7, "rock.elongate": 1.5, "rock.angular": 0.7,
            "rock.facets": 6, "rock.rough": 0.3, "rock.erode": 0.25,
            "rock.joint_sets": 3, "rock.joint_scatter": 0.08,
            "rock.joint_dip_deg": 12, "rock.block_size_m": 1.5,
            "rock.block_relief_m": 0.16, "rock.bury": 0.26, "rock.rubble": 0.5,
            "materials.rock": "bedrock",
            "biomes.bare_rock": 1.0, "biomes.tundra_alpine": 0.8,
            "biomes.grassland": 0.35, "biomes.savanna": 0.3,
            "placement.abundance": 0.16, "placement.spacing_m": 32.0,
            "placement.cluster": 0.55, "placement.slope_max_pct": 65})

    make("basalt-colonnade",
         notes="Cooling lava contracts into a polygonal crack network that "
               "propagates downward — the Giant's Causeway. The columns are a "
               "2D Voronoi network extruded, with uneven tops because a "
               "colonnade sawn flat reads as an extruded shape, not stone.",
         **{"rock.size_m": 4.5, "rock.lumps": 2, "rock.spread": 0.2,
            "rock.flatten": 1.7, "rock.elongate": 1.0, "rock.angular": 0.3,
            "rock.facets": 2, "rock.rough": 0.22, "rock.erode": 0.14,
            "rock.columns": 16, "rock.column_gap_m": 0.1,
            "rock.column_stagger": 0.22, "rock.bury": 0.18, "rock.rubble": 0.4,
            "materials.rock": "bedrock",
            "biomes.bare_rock": 1.0, "biomes.tundra_alpine": 0.5,
            "biomes.grassland": 0.3, "biomes.beach": 0.3,
            "placement.abundance": 0.08, "placement.spacing_m": 60.0,
            "placement.cluster": 0.75, "placement.slope_max_pct": 55})

    make("exfoliating-dome",
         notes="A granite dome shedding sheets parallel to its surface as the "
               "load above it erodes away. Concentric steps, which the ordinary "
               "weathering pass cannot make because it has no notion of depth.",
         **{"rock.size_m": 5.0, "rock.lumps": 3, "rock.spread": 0.25,
            "rock.flatten": 0.9, "rock.elongate": 1.15, "rock.angular": 0.2,
            "rock.facets": 2, "rock.rough": 0.3, "rock.erode": 0.2,
            "rock.cavernous": 0.0, "rock.exfoliate": 0.75, "rock.shell_m": 0.3,
            "rock.bury": 0.24, "rock.rubble": 0.45, "materials.rock": "rock",
            "biomes.bare_rock": 0.9, "biomes.grassland": 0.5,
            "biomes.temperate_forest": 0.4, "biomes.savanna": 0.4,
            "placement.abundance": 0.12, "placement.spacing_m": 40.0,
            "placement.cluster": 0.25, "placement.slope_max_pct": 45})


if __name__ == "__main__":
    main()
