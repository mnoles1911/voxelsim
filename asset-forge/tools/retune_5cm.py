"""Move the last seven off-lattice assets to the 5 cm tier.

Everything that ships has to sit on 10 cm or 5 cm. These seven were authored at
2 cm and 2.5 cm before that was settled, so they could not be put in the world
at all.

It is not just a resolution swap. Two things were tuned against a 2 cm voxel and
mean something different at 5 cm:

- **`growth.tip_radius_m` of 0.02 is under half a voxel at 5 cm.** Twigs would
  be drawn at the one-voxel minimum and the health check would say so on every
  save. Raised past the floor so the number describes what is actually drawn.
- **Stems an eighth of a voxel wide vanish.** A 5 cm trunk radius is one voxel;
  the main stems of a shrub want to read as stems, so the smallest go up.

Foliage clump radii are left alone: 0.15-0.22 m is three to four voxels at
5 cm, which is still a clump.
"""
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"

CHANGES = {
    # -- bushes -------------------------------------------------------------
    "bramble-thicket": {
        "resolution_cm": "5", "growth.tip_radius_m": 0.03,
        "trunk.radius_base_m": 0.08,
    },
    "juniper-scrub": {
        "resolution_cm": "5", "growth.tip_radius_m": 0.03,
        "trunk.radius_base_m": 0.09,
    },
    "desert-shrub": {
        # This one already sat over the loose-voxel threshold on some seeds at
        # 2 cm: a sparse canopy of small clumps is the shape most able to shed a
        # piece. A slightly larger clump with slightly tighter separation keeps
        # the sparse look and gives each clump more twig to hold on to.
        "resolution_cm": "5", "growth.tip_radius_m": 0.03,
        "trunk.radius_base_m": 0.07, "foliage.clump_radius_m": 0.19,
        "foliage.separation": 1.25, "foliage.density": 0.58,
    },
    "coastal-scrub": {
        "resolution_cm": "5", "growth.tip_radius_m": 0.03,
        "trunk.radius_base_m": 0.07,
    },
    # -- tree ---------------------------------------------------------------
    "alpine-krummholz": {
        "resolution_cm": "5", "growth.tip_radius_m": 0.03,
    },
    # -- rocks --------------------------------------------------------------
    "alpine-scree": {
        # 0.5 m is ten voxels at 5 cm, and after faceting there is not much
        # stone left to be a stone. Scree is small on purpose, but this is the
        # floor of what the lattice can carry.
        "resolution_cm": "5", "rock.size_m": 0.65,
    },
    "river-cobble": {
        "resolution_cm": "5",
    },
}


def main():
    for name, changes in CHANGES.items():
        fp = SPECS / f"{name}.json"
        s, _ = sm.load(fp)
        s2, rep = sm.patch(s, changes)
        sm.save(s2, fp)
        print(f"  {name:<20} 5 cm  " + ("! " + rep.warnings[0] if rep.warnings else "ok"))


if __name__ == "__main__":
    main()
