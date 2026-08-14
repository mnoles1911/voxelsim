"""Put trees and rocks back on the 10 cm terrain lattice.

THE DECISION (owner, 2026-08-13). Assets divide into two classes by how they
live in the world, and the class decides the lattice:

  * **Terrain-lattice assets -- rocks and trees.** They join the world's own
    voxel grid and are destructible exactly as terrain is. A destructible voxel
    has to be addressable in the world grid, the world grid has one cell size,
    and that size is `vxc::kVoxelSizeMm` = 100 mm. So these are authored at
    10 cm, full stop. Authoring them finer bought a nicer preview and nothing
    else, because anything that ships through `AssetGrid::at` is read at one
    voxel per terrain voxel -- there is no resampling anywhere in voxel-core.

  * **Detail assets -- bushes, grass, reeds, flowers, fish, birds, cetaceans.**
    They never enter the terrain grid. They carry their own voxel grid and
    their own transform, they are not destructible in the terrain sense, and
    their lattice is therefore free. They keep the sizes their own measurements
    chose: 5 cm for ground cover, 1-10 cm for animals by the rule in
    `docs/marine-megafauna-research.md`.

This reverses `tools/all_to_5cm.py`, which moved everything to 5 cm on the
reasoning that one lattice for every asset means nothing downstream has to ask
which lattice an object is on. That was right about the goal and wrong about
the number: the lattice that matters is the one the WORLD uses, and half the
library cannot join it anyway.

WHAT THIS IS NOT: a resolution swap. A feature finer than half a voxel cannot
be drawn at the size it claims -- it comes out at the one-voxel minimum and the
parameter silently stops describing the asset. `retune_5cm.py` hit exactly this
going the other way and said so: "growth.tip_radius_m of 0.02 is under half a
voxel at 5 cm. Twigs would be drawn at the one-voxel minimum and the health
check would say so on every save. Raised past the floor so the number describes
what is actually drawn."

At 10 cm the floor is 0.05 m, and ELEVEN OF SEVENTEEN TREES sit under it --
tip radii from 0.03 to 0.045. So they are raised to the floor. This makes the
twigs no thicker than they were already being drawn; it makes the spec honest
about it.

Rocks need no such correction. Their `growth.tip_radius_m` is the default
sitting in a spec that has no skeleton to apply it to -- every validated spec
carries every parameter -- and reading that as a defect was a false alarm the
first audit produced. The rock parameters that could go sub-voxel
(`block_relief`, `vein_width`, `clast_size`, `notch`, `rind_m`) are all
authored above the floor already.

    python tools/all_to_10cm.py            # report what would change
    python tools/all_to_10cm.py --apply
"""
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"

# Kinds that join the world's voxel grid. Everything else is a detail asset.
TERRAIN_LATTICE_KINDS = ("tree", "rock")
TERRAIN_CM = "10"

# Half a 10 cm voxel. A feature under this cannot be drawn at the size it
# claims, so the claim is what gets corrected, not the drawing.
FLOOR_M = 0.05

# Parameters that are a RADIUS OR HALF-WIDTH IN METRES on a tree, and so are
# subject to the floor. Deliberately not a scan of every float in the spec:
# `spread`, `droop_m` and the rest are displacements, not thicknesses, and a
# displacement smaller than a voxel is a real and useful thing -- it biases
# where a voxel lands rather than asking for a voxel that cannot exist.
TREE_THICKNESS_KEYS = (
    "growth.tip_radius_m",
    "trunk.radius_base_m",
    "foliage.clump_radius_m",
    "strand.radius_m",
)


def main() -> int:
    apply = "--apply" in sys.argv
    moved = raised = kept = 0
    detail = []

    for fp in sorted(SPECS.glob("*.json")):
        s, rep = sm.load(fp)
        kind = sm.get(s, "kind")
        if kind not in TERRAIN_LATTICE_KINDS:
            detail.append((fp.stem, kind, str(sm.get(s, "resolution_cm"))))
            continue

        changes = {}
        if str(sm.get(s, "resolution_cm")) != TERRAIN_CM:
            changes["resolution_cm"] = TERRAIN_CM

        if kind == "tree":
            for key in TREE_THICKNESS_KEYS:
                try:
                    v = float(sm.get(s, key))
                except (KeyError, TypeError, ValueError):
                    continue
                if 0.0 < v < FLOOR_M:
                    changes[key] = FLOOR_M

        if not changes:
            kept += 1
            continue

        bits = ", ".join(f"{k} {sm.get(s, k)} -> {v}" for k, v in changes.items())
        print(f"  {fp.stem:26} {kind:5} {bits}")
        if "resolution_cm" in changes:
            moved += 1
        if len(changes) > (1 if "resolution_cm" in changes else 0):
            raised += 1
        if apply:
            out, rep2 = sm.patch(s, changes)
            for w in rep2.warnings:
                print(f"    ! {fp.stem}: {w}")
            sm.save(out, fp)

    print(f"\n{moved} moved to {TERRAIN_CM} cm, {raised} had a sub-voxel feature "
          f"raised to the {FLOOR_M:g} m floor, {kept} already correct")
    print(f"{len(detail)} detail assets left on their own lattice:")
    by_cm: dict[str, list[str]] = {}
    for name, kind, cm in detail:
        by_cm.setdefault(cm, []).append(f"{name}({kind})")
    for cm in sorted(by_cm, key=float):
        names = by_cm[cm]
        print(f"  {cm:>2} cm  {len(names):>2}  {', '.join(sorted(names)[:6])}"
              + (" ..." if len(names) > 6 else ""))
    if not apply:
        print("\n(dry run -- pass --apply to write)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
