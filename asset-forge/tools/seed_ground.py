"""Author the ground-cover species: grass tufts, reeds and flowers.

ALL AUTHORED AT 5 cm, which is the fine lattice tier the engine will support.
5 cm nests 2:1 inside the terrain's 10 cm -- eight fine voxels per coarse one,
one subdivision level -- where 2 cm is a 5:1 nesting at 125x the voxels. The
saving is real: a 10 x 10 m patch of meadow grass drops from 239k voxels to
45k, and a reed bed from 1.4M to 333k.

Authoring FOR 5 cm is a different job from shrinking a 2 cm asset into it, and
the specs below reflect that. Three rules came out of the A/B:

- **Fewer, wider stems.** At 5 cm a blade is one voxel wide whatever you ask
  for, so thirty-four of them rooted in a 5 cm disc land on top of each other
  and the tuft fuses into a plate. Ten to fifteen, spread wider, is what the
  lattice can express.
- **Taller.** A 34 cm tuft is seven voxels. Pushing ground cover up a little
  buys the shape somewhere to exist.
- **Bigger heads.** A reed spike and a flower bloom both have to clear a couple
  of voxels to read as anything.

Grass and reeds are MAT_GRASS and MAT_SAVANNA_GRASS, which the engine already
has, so they need no material append. Flowers need a bloom colour and do.
"""
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def make(name, **changes):
    s, rep = sm.patch(sm.default_spec(), dict(name=name, resolution_cm="5", **changes))
    sm.save(s, SPECS / f"{name}.json")
    print(f"  {name:<26} " + ("! " + rep.warnings[0] if rep.warnings else "ok"))


def main():
    print("grass:")
    make("meadow-grass", kind="grass", height_m=0.45,
         notes="The baseline tuft. MAT_GRASS, which the terrain already uses, so "
               "this needs no new engine material.",
         **{"tuft.stems": 13, "tuft.spread_m": 0.09, "tuft.splay_deg": 18,
            "tuft.arc": 0.6, "tuft.width_m": 0.05, "tuft.taper": 0.5,
            "tuft.wander": 0.4, "tuft.length_var": 0.4, "tuft.base_m": 0.09,
            "tuft.head": "none", "materials.stem": "grass",
            "biomes.grassland": 1.0, "biomes.temperate_forest": 0.7,
            "biomes.beach": 0.3, "biomes.taiga": 0.4,
            "placement.abundance": 1.0, "placement.spacing_m": 0.6,
            "placement.cluster": 0.7, "placement.slope_max_pct": 60,
            "placement.elev_max_m": 1400})
    make("dry-tussock", kind="grass", height_m=0.62,
         notes="Coarse pale bunchgrass for savanna and steppe. Stiffer and "
               "taller than meadow grass, and it stands rather than lies over.",
         **{"tuft.stems": 12, "tuft.spread_m": 0.1, "tuft.splay_deg": 24,
            "tuft.arc": 0.38, "tuft.width_m": 0.05, "tuft.taper": 0.4,
            "tuft.wander": 0.3, "tuft.length_var": 0.45, "tuft.base_m": 0.1,
            "tuft.head": "none", "materials.stem": "savanna_grass",
            "biomes.savanna": 1.0, "biomes.grassland": 0.6, "biomes.desert": 0.35,
            "placement.abundance": 0.9, "placement.spacing_m": 0.9,
            "placement.cluster": 0.6, "placement.slope_max_pct": 55})
    make("alpine-sedge", kind="grass", height_m=0.3,
         notes="Short hardy cushion for above the treeline. Raised from 16 cm "
               "so it is six voxels rather than three; below that there is no "
               "shape left to author.",
         **{"tuft.stems": 14, "tuft.spread_m": 0.09, "tuft.splay_deg": 32,
            "tuft.arc": 0.7, "tuft.width_m": 0.05, "tuft.taper": 0.5,
            "tuft.wander": 0.5, "tuft.length_var": 0.3, "tuft.base_m": 0.09,
            "tuft.head": "none", "materials.stem": "leaf_needle",
            "biomes.tundra_alpine": 1.0, "biomes.taiga": 0.5,
            "placement.abundance": 1.0, "placement.spacing_m": 0.5,
            "placement.cluster": 0.8, "placement.elev_min_m": 500,
            "placement.slope_max_pct": 65})
    make("jungle-groundcover", kind="grass", height_m=0.8,
         notes="Broad-bladed rainforest floor cover: fewer, wider, more arched "
               "blades than a meadow tuft.",
         **{"tuft.stems": 11, "tuft.spread_m": 0.11, "tuft.splay_deg": 26,
            "tuft.arc": 0.62, "tuft.width_m": 0.075, "tuft.taper": 0.35,
            "tuft.wander": 0.35, "tuft.length_var": 0.35, "tuft.base_m": 0.11,
            "tuft.head": "none", "materials.stem": "leaf_jungle",
            "biomes.rainforest": 1.0, "biomes.temperate_forest": 0.3,
            "placement.abundance": 1.0, "placement.spacing_m": 0.8,
            "placement.cluster": 0.65})

    print("reeds:")
    make("water-reed", kind="reed", height_m=2.0,
         notes="Tall near-vertical waterside stems with seed heads. Also "
               "MAT_GRASS: no material append needed. The kind that loses least "
               "to the coarser lattice, because a 2 m stem has voxels to spare.",
         **{"tuft.stems": 12, "tuft.spread_m": 0.22, "tuft.splay_deg": 7,
            "tuft.arc": 0.12, "tuft.width_m": 0.055, "tuft.taper": 0.6,
            "tuft.wander": 0.2, "tuft.length_var": 0.3, "tuft.base_m": 0.12,
            "tuft.head": "spike", "tuft.head_m": 0.18, "tuft.head_frac": 0.2,
            "tuft.head_share": 0.8, "materials.stem": "grass",
            "materials.head": "leaf_dry",
            "biomes.temperate_forest": 0.7, "biomes.grassland": 0.8,
            "biomes.rainforest": 0.5, "biomes.beach": 0.4,
            "placement.abundance": 1.0, "placement.spacing_m": 0.7,
            "placement.cluster": 0.95, "placement.water_max_m": 4,
            "placement.slope_max_pct": 12, "placement.elev_max_m": 1200})
    make("bulrush", kind="reed", height_m=1.4,
         notes="Shorter, thicker stems with a fat dark head. Reads at distance "
               "where a thin reed does not.",
         **{"tuft.stems": 9, "tuft.spread_m": 0.17, "tuft.splay_deg": 9,
            "tuft.arc": 0.18, "tuft.width_m": 0.06, "tuft.taper": 0.75,
            "tuft.wander": 0.25, "tuft.length_var": 0.25, "tuft.base_m": 0.11,
            "tuft.head": "spike", "tuft.head_m": 0.22, "tuft.head_frac": 0.18,
            "tuft.head_share": 0.9, "materials.stem": "grass",
            "materials.head": "leaf_autumn",
            "biomes.grassland": 0.7, "biomes.temperate_forest": 0.6,
            "biomes.beach": 0.5,
            "placement.abundance": 0.8, "placement.spacing_m": 0.8,
            "placement.cluster": 0.95, "placement.water_max_m": 3,
            "placement.slope_max_pct": 10})
    make("pampas-plume", kind="reed", height_m=1.7,
         notes="Feathery seed heads on tall stems, for open dry ground away "
               "from water. The plume head, not the spike.",
         **{"tuft.stems": 14, "tuft.spread_m": 0.18, "tuft.splay_deg": 16,
            "tuft.arc": 0.3, "tuft.width_m": 0.05, "tuft.taper": 0.5,
            "tuft.wander": 0.3, "tuft.length_var": 0.35, "tuft.base_m": 0.12,
            "tuft.head": "plume", "tuft.head_m": 0.4, "tuft.head_frac": 0.28,
            "tuft.head_share": 0.55, "materials.stem": "savanna_grass",
            "materials.head": "leaf_dry",
            "biomes.savanna": 1.0, "biomes.grassland": 0.5,
            "placement.abundance": 0.5, "placement.spacing_m": 1.6,
            "placement.cluster": 0.6, "placement.slope_max_pct": 35})

    print("flowers:")
    make("meadow-daisy", kind="flower", height_m=0.42,
         notes="The baseline flower. Some stems carry a bloom, the rest are "
               "leaves -- which is how the plant gets foliage without a second "
               "pass. Needs the bloom material appended to vxc::Material.",
         **{"tuft.stems": 9, "tuft.spread_m": 0.07, "tuft.splay_deg": 20,
            "tuft.arc": 0.3, "tuft.width_m": 0.05, "tuft.taper": 0.8,
            "tuft.wander": 0.3, "tuft.length_var": 0.35, "tuft.base_m": 0.07,
            "tuft.head": "bloom", "tuft.head_m": 0.13, "tuft.head_share": 0.45,
            "materials.stem": "grass", "materials.head": "leaf_blossom",
            "biomes.grassland": 1.0, "biomes.temperate_forest": 0.5,
            "placement.abundance": 0.7, "placement.spacing_m": 0.9,
            "placement.cluster": 0.8, "placement.elev_max_m": 1600})
    make("alpine-cushion-flower", kind="flower", height_m=0.24,
         notes="Tiny high-altitude cushion. Raised from 12 cm: at 5 cm that was "
               "two voxels and there was nothing to draw.",
         **{"tuft.stems": 12, "tuft.spread_m": 0.08, "tuft.splay_deg": 34,
            "tuft.arc": 0.35, "tuft.width_m": 0.05, "tuft.taper": 0.8,
            "tuft.wander": 0.4, "tuft.length_var": 0.3, "tuft.base_m": 0.08,
            "tuft.head": "bloom", "tuft.head_m": 0.1, "tuft.head_share": 0.7,
            "materials.stem": "leaf_needle", "materials.head": "leaf_blossom",
            "biomes.tundra_alpine": 1.0, "biomes.taiga": 0.3,
            "placement.abundance": 0.6, "placement.spacing_m": 0.7,
            "placement.cluster": 0.9, "placement.elev_min_m": 700})
    make("desert-bloom", kind="flower", height_m=0.35,
         notes="Sparse dry-country flower: few stems, small heads, wide gaps "
               "between plants.",
         **{"tuft.stems": 8, "tuft.spread_m": 0.06, "tuft.splay_deg": 22,
            "tuft.arc": 0.18, "tuft.width_m": 0.05, "tuft.taper": 0.8,
            "tuft.wander": 0.25, "tuft.length_var": 0.4, "tuft.base_m": 0.06,
            "tuft.head": "bloom", "tuft.head_m": 0.11, "tuft.head_share": 0.5,
            "materials.stem": "leaf_dry", "materials.head": "leaf_autumn",
            "biomes.desert": 1.0, "biomes.savanna": 0.5,
            "placement.abundance": 0.25, "placement.spacing_m": 2.5,
            "placement.cluster": 0.5})
    make("coastal-thrift", kind="flower", height_m=0.26,
         notes="Low salt-tolerant cushion for the beach band: short stems, "
               "tight pink heads.",
         **{"tuft.stems": 12, "tuft.spread_m": 0.07, "tuft.splay_deg": 26,
            "tuft.arc": 0.3, "tuft.width_m": 0.05, "tuft.taper": 0.8,
            "tuft.wander": 0.35, "tuft.length_var": 0.3, "tuft.base_m": 0.07,
            "tuft.head": "bloom", "tuft.head_m": 0.1, "tuft.head_share": 0.6,
            "materials.stem": "grass", "materials.head": "leaf_blossom",
            "biomes.beach": 1.0, "biomes.grassland": 0.3,
            "placement.abundance": 0.7, "placement.spacing_m": 0.8,
            "placement.cluster": 0.85, "placement.elev_max_m": 40})
    make("jungle-understory-flower", kind="flower", height_m=0.75,
         notes="Rainforest floor flower: taller leaf stems under a few big "
               "bright heads. The one species the coarse lattice suits, because "
               "its blooms were always large.",
         **{"tuft.stems": 8, "tuft.spread_m": 0.09, "tuft.splay_deg": 18,
            "tuft.arc": 0.4, "tuft.width_m": 0.055, "tuft.taper": 0.7,
            "tuft.wander": 0.3, "tuft.length_var": 0.4, "tuft.base_m": 0.09,
            "tuft.head": "bloom", "tuft.head_m": 0.3, "tuft.head_share": 0.4,
            "materials.stem": "leaf_jungle", "materials.head": "leaf_autumn",
            "biomes.rainforest": 1.0, "biomes.temperate_forest": 0.25,
            "placement.abundance": 0.5, "placement.spacing_m": 1.4,
            "placement.cluster": 0.7})


if __name__ == "__main__":
    main()
