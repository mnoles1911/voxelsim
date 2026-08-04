"""Prove the TERRAIN_VERSION / BAKE_VERSION split did its job, on a REAL tile.

    python tools/verify_terrain_identity.py \
        --tile -13 -5 --seed 20260719 \
        --cache-dir D:/voxelsim/tile-cache \
        --provider-id terrain-diffusion-unlabeled-80b9ca451a23eae4

WHAT IT ASSERTS, AND WHY IT IS A TOOL RATHER THAN A TEST
--------------------------------------------------------
P2 adds a water plane and moves no height. The entire argument for splitting
the version counter is that its re-bake is an ADDITION rather than a new
world -- the 256-tile lake survey, the 25-tile bank probe, the vista archive
and the owner's spawn sites all keep describing ground that still exists.

That argument is worth exactly as much as the evidence for it. So this re-bakes
a tile that is ALREADY ON DISK, under the split, and compares the elevation
plane it produces against the plane the shipped file carries:

    CONTROL POINTS, byte for byte.

Not "close", not a tolerance, not the sample field -- the encoded int16 control
lattice, which is what the client actually reads and what every measurement on
this world was taken against. A tolerance here would be a way of not noticing.

It is a tool and not a test because it needs the tile cache (~30 GB, not in
CI) and ~265 CPU-s plus 5.5 GiB of RSS per tile. The fast synthetic form of the
same claim runs in the suite as
``tests/test_bake_terrain_identity.py::test_water_plane_moves_no_height``, and
the two are not substitutes: that one proves the water CODE PATH perturbs
nothing, this one proves the IDENTITY SPLIT is wired right end to end. Either
can pass while the other fails.

READING THE RESULT
------------------
* **IDENTICAL** -- the split works. The re-bake is safe to spend: every tile
  comes back on the same ground, and the only new bytes are the water plane.
* **DIFFERS** -- STOP. Something has leaked from the product half into the
  terrain half. Do not spend the re-bake; the world would move under every
  screenshot and site the owner holds, which is the exact outcome the split was
  taken to avoid. The tool prints where and by how much, because "differs"
  alone does not say whether it is one block or the whole tile.

A NOTE ON THE RESIDENT SET, learned the expensive way today: 24 of the 38
resident fine tiles are CODEC_ZSTD with a mean size of 43 MB, so a naive
``size > 50 MB`` filter hides them entirely. This tool takes tiles by
coordinate and reports each one's codec, so a run cannot silently sample only
the uncompressed ones.
"""

from __future__ import annotations

import argparse
import dataclasses
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec  # noqa: E402
from terrain_service.bake import pipeline as bp  # noqa: E402
from terrain_service.cache import TileCache  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lake_survey import _fetchers, _resolve_fine_provider_id, _superblock  # noqa: E402


def _decompressor():
    try:
        import zstandard
    except ImportError:
        return None
    return lambda frame, n: zstandard.ZstdDecompressor().decompress(
        frame, max_output_size=n
    )


def ab_tile(cache_dir: Path, provider_id: str, fine_id: str, seed: int,
            tx: int, ty: int) -> dict:
    """Bake ONE REAL tile twice, water on and off, against ONE superblock.

    THE CLEAN FORM OF THE GATE, and the only one available for this world.
    Comparing a re-bake against the shipped .vxtl is permanently confounded
    here: no flow superblock was retained under this fine namespace (the only
    cached blocks belong to provider -71e2b362, a different world), so the
    re-bake must build its own parentless L0, whose accumulation differs from
    the one the shipped tile baked against. That difference rides through
    incision and moves a scattering of control points by 1 LSB -- 58 of 67.1M
    on (-11,-3), 113 on (-12,-3) -- which says nothing about the version split.

    This asks the question the split actually has to answer, with no confound:
    holding the superblock, the coarse ring, the seed and every constant fixed
    and toggling ONE term, does the P2 bake produce the same ground? Single-term
    control, one world, which is the only form of this claim that has survived
    on this branch.

    RUN IT ON A WET TILE. An A/B whose water plane comes out empty compares
    nothing to nothing and passes vacuously, so this asserts the plane is
    non-empty BEFORE it reports agreement. The two tiles this gate was first
    run on, (-11,-3) and (-12,-3), turned out to be the driest in the resident
    set (median runoff 1 and 2 mm/yr, 95% and 63% ARID province) and drew no
    water at all -- correct physics, and a vacuous test.
    """
    cache = TileCache(cache_dir)
    geom = bp.PRODUCTION
    geom.assert_production()
    fetch, fetch_climate = _fetchers(cache, provider_id, seed)
    if fetch(tx, ty) is None:
        raise SystemExit(f"error: coarse tile ({tx},{ty}) is not in {cache_dir}")
    kernels = bp.load_kernels()
    sb, cached_sb = _superblock(cache, fine_id, seed, tx, ty, geom,
                                bp.CONSTANTS, fetch, kernels)

    out = {"tile": (tx, ty), "superblock_cached": bool(cached_sb)}
    res = {}
    for label, enabled in (("water ON", True), ("water OFF", False)):
        consts = dataclasses.replace(bp.CONSTANTS, water_plane_enabled=enabled)
        print(f"  [{tx},{ty}] baking, {label}", flush=True)
        c0 = time.process_time()
        res[enabled] = bp.bake_tile(
            world_seed=seed, tile_x=tx, tile_y=ty,
            coarse_fetch=fetch, climate_fetch=fetch_climate,
            kernels=kernels, geom=geom, consts=consts, inflow_source=sb,
        )
        print(f"    {time.process_time() - c0:.0f} s cpu", flush=True)

    on, off = res[True], res[False]
    wet = (0 if on.water_surface_m is None
           else int(np.isfinite(on.water_surface_m).sum()))
    out["water_wet_cells"] = wet
    out["water_wet_frac"] = wet / float(on.elevation_m.size)
    if wet == 0:
        print("  VACUOUS: this tile drew no water, so the comparison below "
              "proves nothing. Re-run on a wetter tile.")
        out["identical"] = False
        out["vacuous"] = True
        return out
    print(f"  water plane: {wet:,} wet px "
          f"({100.0 * wet / on.elevation_m.size:.4f}%)")

    same_field = bool(np.array_equal(on.elevation_m, off.elevation_m))
    cp_on, base, quant = tile_codec.elevation_control_points(on.elevation_m)
    cp_off, _, _ = tile_codec.elevation_control_points(
        off.elevation_m, base_offset_mm=base, quant=quant)
    same_cp = bool(np.array_equal(cp_on, cp_off))
    out["identical"] = bool(same_field and same_cp)
    out["same_sample_field"] = same_field
    out["same_control_points"] = same_cp
    out["same_flow"] = bool(np.array_equal(on.flow, off.flow))
    out["same_accumulation"] = bool(
        np.array_equal(on.accumulation_m2, off.accumulation_m2))
    if out["identical"]:
        print(f"  ELEVATION: IDENTICAL with the water plane on "
              f"({cp_on.size:,} control points, byte for byte)")
        print(f"  flow identical {out['same_flow']}; "
              f"accumulation identical {out['same_accumulation']}; "
              f"basins {len(on.basins)} vs {len(off.basins)}")
    else:
        d = cp_on.astype(np.int64) - cp_off.astype(np.int64)
        nz = d != 0
        out["diff_cells"] = int(nz.sum())
        out["diff_max_lsb"] = int(np.abs(d).max())
        print(f"  ELEVATION: DIFFERS on {nz.sum():,} control points, max "
              f"{np.abs(d).max()} LSB -- the water stage moved the ground")
    for k in sorted(on.stats):
        if k.startswith("water_"):
            out[k] = on.stats[k]
    return out


def verify_tile(cache_dir: Path, provider_id: str, fine_id: str, seed: int,
                tx: int, ty: int, *, write_water: bool) -> dict:
    cache = TileCache(cache_dir)
    geom = bp.PRODUCTION
    geom.assert_production()

    blob = cache.get_fine(fine_id, seed, tx, ty)
    if blob is None:
        raise SystemExit(
            f"error: no resident fine tile ({tx},{ty}) under {fine_id}"
        )
    shipped = tile_codec.decode_v2(blob, decompressor=_decompressor())
    print(f"[{tx},{ty}] resident tile: {len(blob) / 1e6:.1f} MB, "
          f"codec={'ZSTD' if shipped.codec == tile_codec.CODEC_ZSTD else 'RAW'}, "
          f"bake_ver={shipped.bake_ver}, quant={shipped.quant}, "
          f"base_offset={shipped.base_offset_mm} mm, flags: "
          f"flow={shipped.flow is not None} basins={shipped.basins is not None} "
          f"water={shipped.water_cp is not None}")

    # BAKE WITH THE WATER PLANE ON. That is the point: the claim under test is
    # that the P2 bake reproduces the pre-P2 ground, not that a bake with the
    # new stage disabled does (which would prove only that the flag works).
    consts = dataclasses.replace(bp.CONSTANTS, water_plane_enabled=write_water)
    fetch, fetch_climate = _fetchers(cache, provider_id, seed)
    if fetch(tx, ty) is None:
        raise SystemExit(f"error: coarse tile ({tx},{ty}) is not in {cache_dir}")
    kernels = bp.load_kernels()
    sb, cached_sb = _superblock(cache, fine_id, seed, tx, ty, geom, consts,
                                fetch, kernels)
    if not cached_sb:
        print("  warning: superblock was REBUILT, not the cached one the "
              "shipped tile baked against; a difference below may be the "
              "superblock rather than the split.", file=sys.stderr)

    print(f"  re-baking (~{bp.estimate_peak_bytes(geom) / 2**30:.1f} GiB peak)",
          flush=True)
    c0 = time.process_time()
    r = bp.bake_tile(
        world_seed=seed, tile_x=tx, tile_y=ty,
        coarse_fetch=fetch, climate_fetch=fetch_climate,
        kernels=kernels, geom=geom, consts=consts, inflow_source=sb,
    )
    cpu = time.process_time() - c0
    print(f"  bake {cpu:.0f} s cpu", flush=True)

    # Re-encode on the SHIPPED TILE'S OWN DATUM. `choose_datum` is a function of
    # the control-point range, so it would land on the same answer anyway --
    # but pinning it makes this a comparison of the SURFACE rather than a
    # comparison of the surface and the datum picker at once, and if the datum
    # ever did move that is a separate finding worth its own line.
    cp, base, quant = tile_codec.elevation_control_points(
        r.elevation_m,
        base_offset_mm=shipped.base_offset_mm,
        quant=shipped.quant,
    )
    auto_base, auto_quant = tile_codec.choose_datum(
        int(np.rint(r.elevation_m.min() * 1000.0)),
        int(np.rint(r.elevation_m.max() * 1000.0)),
    )

    same = np.array_equal(cp, shipped.elevation_cp)
    out = {
        "tile": (tx, ty),
        "identical": bool(same),
        "codec": int(shipped.codec),
        "bake_ver_shipped": int(shipped.bake_ver),
        "datum_pinned": (shipped.base_offset_mm, shipped.quant),
        "datum_auto": (auto_base, auto_quant),
        "cpu_seconds": cpu,
        "superblock_cached": bool(cached_sb),
    }
    if same:
        print("  ELEVATION CONTROL POINTS: IDENTICAL "
              f"({cp.size:,} points, byte for byte)")
    else:
        d = cp.astype(np.int64) - shipped.elevation_cp.astype(np.int64)
        nz = d != 0
        out["diff_cells"] = int(nz.sum())
        out["diff_frac"] = float(nz.mean())
        out["diff_max_lsb"] = int(np.abs(d).max())
        out["diff_max_mm"] = int(np.abs(d).max() * tile_codec.QUANT_MM[quant])
        print(f"  ELEVATION CONTROL POINTS: DIFFER on {nz.sum():,} of "
              f"{cp.size:,} ({100 * nz.mean():.4f}%), max "
              f"{np.abs(d).max()} LSB = "
              f"{np.abs(d).max() * tile_codec.QUANT_MM[quant]} mm")
        ys, xs = np.nonzero(nz)
        print(f"    first differing pixel: ({xs[0]}, {ys[0]})")

    # And what P2 actually added, which is the other half of the report: the
    # re-bake is only worth spending if it produces the plane it promised.
    if r.water_surface_m is not None:
        wet = np.isfinite(r.water_surface_m)
        out["water_wet_cells"] = int(wet.sum())
        out["water_wet_frac"] = float(wet.mean())
        blob_new = tile_codec.encode_fine(
            seed=seed, x=tx, y=ty, elevation_m=r.elevation_m, flow=r.flow,
            basins=r.basins, water_surface_m=r.water_surface_m,
            codec=shipped.codec, base_offset_mm=shipped.base_offset_mm,
            quant=shipped.quant,
        )
        blob_dry = tile_codec.encode_fine(
            seed=seed, x=tx, y=ty, elevation_m=r.elevation_m, flow=r.flow,
            basins=r.basins, water_surface_m=None,
            codec=shipped.codec, base_offset_mm=shipped.base_offset_mm,
            quant=shipped.quant,
        )
        out["water_bytes"] = len(blob_new) - len(blob_dry)
        out["tile_bytes"] = len(blob_new)
        print(f"  water plane: {wet.sum():,} wet px ({100 * wet.mean():.3f}%), "
              f"costs {(len(blob_new) - len(blob_dry)) / 1e6:.3f} MB of "
              f"{len(blob_new) / 1e6:.1f} MB "
              f"({100 * (len(blob_new) - len(blob_dry)) / len(blob_new):.2f}%)")
        for k in sorted(r.stats):
            if k.startswith("water_"):
                out[k] = r.stats[k]
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tile", type=int, nargs=2, action="append", required=True,
                    metavar=("X", "Y"), help="repeatable")
    ap.add_argument("--seed", type=int, default=20260719)
    ap.add_argument("--cache-dir", type=Path, required=True)
    ap.add_argument("--provider-id", required=True)
    ap.add_argument("--fine-provider-id", default=None)
    ap.add_argument("--ab", action="store_true",
                    help="bake each tile TWICE (water on/off) against "
                         "one superblock and compare, instead of "
                         "comparing against the shipped .vxtl.")
    ap.add_argument("--no-water", action="store_true",
                    help="bake with water_plane_enabled=False; isolates the "
                         "split itself from the new stage")
    args = ap.parse_args()

    fine_id = _resolve_fine_provider_id(args.cache_dir, args.provider_id,
                                        args.fine_provider_id)
    print(f"fine namespace: {fine_id}")
    print(f"TERRAIN_VERSION={bp.TERRAIN_VERSION}  BAKE_VERSION={bp.BAKE_VERSION}")
    print(f"terrain fingerprint {bp.bake_fingerprint()[:16]}  "
          f"product fingerprint {bp.product_fingerprint()[:16]}")

    if args.ab:
        results = [ab_tile(args.cache_dir, args.provider_id, fine_id,
                           args.seed, tx, ty) for tx, ty in args.tile]
    else:
        results = [
            verify_tile(args.cache_dir, args.provider_id, fine_id, args.seed,
                        tx, ty, write_water=not args.no_water)
            for tx, ty in args.tile
        ]
    ok = sum(1 for r in results if r["identical"])
    what = ("reproduce their water-OFF ground byte for byte" if args.ab
            else "reproduce their shipped elevation plane byte for byte")
    print(f"\n{ok}/{len(results)} tiles {what}")
    if ok != len(results):
        print("REFUSE THE RE-BAKE: the split is not doing its job.")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
