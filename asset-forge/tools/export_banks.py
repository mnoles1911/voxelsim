"""Bake VXA v3 species banks for the engine.

    python tools/export_banks.py --kind tree rock --seeds 1 2 3 4 [--skip-heavy]

One file per (species, seed) at out/engine/banks/<name>/<name>-NNNN.vxa, which
is the bank reference the species manifest carries and the layout
vxc::AssetBankLibrary scans. Terrain kinds only by default: they are the ones
that join the world voxel grid; detail kinds render as their own objects and
their bank path is the UE side's business.

EVERY BAKE IS CHECKED AGAINST THE LAYER IT IS FILED ON, here, by the tool that
files it. assetplacement.h is blunt about why: "baking an asset taller than
its layer's maxHeightMm puts a hole in the world at the top of that asset,
silently" -- the streaming bound proves the crown's chunks are air and they
never generate. So a baked grid whose extent exceeds its layer's height,
depth, or radius ceiling is REFUSED with the species and the numbers, and the
file is not written. The C++ loader re-checks at load (the file could be
edited); this check is the one that names the species while a human is
watching.

Skips (species, seed) files that already exist, so an interrupted run resumes
rather than re-baking half an hour of trees. --force rebakes.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import cli, manifest, parts as partslib, pipeline, spec as sm, vxa

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"


def check_against_layer(name: str, grid, layer_index: int) -> "str | None":
    """None if the baked grid fits its layer's declared box, else why not.

    The grid's own numbers, not the spec's: origin.z is where the base sits
    relative to the anchor voxel, so height above the anchor is
    origin.z + size.z voxels and depth below it is -origin.z."""
    L = manifest.LAYERS[layer_index]
    voxel_mm = int(round(grid.voxel_m * 1000.0))
    nx, ny, nz = grid.shape
    ox, oy, oz = (int(v) for v in grid.origin)
    height_mm = (oz + nz) * voxel_mm
    depth_mm = -oz * voxel_mm
    # Horizontal reach from the anchor on either axis, either sign: the layer's
    # max_radius_mm is the dilation every rect query applies, so a voxel
    # further out than it is a voxel queries can MISS -- a sliced-off crown.
    reach_mm = max(abs(ox), abs(oy), abs(ox + nx), abs(oy + ny)) * voxel_mm
    if height_mm > L.max_height_mm:
        return (f"{name}: baked height {height_mm} mm exceeds layer L{layer_index} "
                f"maxHeightMm {L.max_height_mm} -- a hole in the world at its crown")
    if depth_mm > L.max_depth_mm:
        return (f"{name}: baked depth {depth_mm} mm exceeds layer L{layer_index} "
                f"maxDepthMm {L.max_depth_mm}")
    if reach_mm > L.max_radius_mm:
        return (f"{name}: baked reach {reach_mm} mm exceeds layer L{layer_index} "
                f"maxRadiusMm {L.max_radius_mm} -- rect queries would miss its edge")
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--kind", nargs="*", default=list(manifest.KINDS_TERRAIN))
    ap.add_argument("--seeds", nargs="*", type=int, default=[1, 2, 3, 4])
    ap.add_argument("--only", nargs="*", default=None, help="species names")
    ap.add_argument("--skip-heavy", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--out", default=str(ROOT / "out" / "engine" / "banks"))
    args = ap.parse_args()

    banks = Path(args.out)
    banks.mkdir(parents=True, exist_ok=True)

    baked = refused = skipped = failed = 0
    t0 = time.time()
    report = manifest.ExportReport()

    for p in sorted(SPECS.glob("*.json")):
        name = p.stem
        if args.only is not None and name not in args.only:
            continue
        if args.skip_heavy and name in cli.HEAVY_SPECS:
            print(f"  {name}: heavy, skipped by flag")
            continue
        body, _ = sm.load(p)
        kind = sm.get(body, "kind")
        if kind not in args.kind:
            continue
        layer = manifest.assign_layer(
            kind, manifest.nominal_height_m(body, kind), report, name,
            float(sm.get(body, "placement.spacing_m")))
        if layer < 0 or layer == manifest.LAYER_NOT_SCATTERED:
            print(f"  {name}: not on a scatter layer, skipped")
            continue
        if manifest.folded_top_per_mille(body, layer) == 0:
            # The manifest exports this species with every folded weight at
            # zero: it cannot appear anywhere, so a bank for it is dead bytes
            # -- and for the widest heroes it would also force the layer's
            # radius (hence every bound query's dilation) up to carry it.
            print(f"  {name}: folds to zero per-mille on L{layer} "
                  f"(absent from the world), bank skipped")
            continue

        for seed in args.seeds:
            dst = banks / name / f"{name}-{seed:04d}.vxa"
            if dst.exists() and not args.force:
                skipped += 1
                continue
            try:
                a = pipeline.build(body, seed)
            except Exception as e:  # noqa: BLE001 -- a bake batch must name and move on
                print(f"  {name} seed {seed}: BUILD FAILED: {e}")
                failed += 1
                continue
            why = check_against_layer(name, a.grid, layer)
            if why is not None:
                print(f"  REFUSED {why}")
                refused += 1
                break  # every seed of this species has the same box class
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(vxa.encode(a.grid, a.parts, partslib.joints(a.parts)))
            baked += 1
            print(f"  {name} seed {seed}: {dst.stat().st_size:,} B "
                  f"({time.time() - t0:,.0f} s elapsed)")

    print(f"banks: baked {baked}, skipped {skipped} existing, refused {refused}, "
          f"failed {failed}, in {time.time() - t0:,.0f} s")
    # Refusals are DELIBERATE and named above; build failures mean the bake is
    # incomplete and the manifest's seeds_baked will say so per species.
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
