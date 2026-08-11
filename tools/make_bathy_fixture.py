"""Generate the SECTION_BATHY_* conformance fixture (bake_ver 27).

WHY A FIXTURE AND NOT JUST A ROUND-TRIP TEST -- the argument
``make_water_fixture.py`` makes, unchanged: the Python encoder and the C++
decoder are written independently against docs/vxtl-v2-format.md, so a
round-trip in either language proves only that that language agrees with
itself. A committed file both sides parse is the only thing that catches a
disagreement.

It applies to bathymetry for the same reason it applies to the water plane:
the failure mode is SILENT. Nothing throws when a depth plane is read one LSB
out or when the dry sentinel is taken for a real depth -- the lake is simply
ten times too deep, or the whole tile is a lake 0.01 m deep, in a build nobody
thinks touched the codec.

The pair it writes is chosen to break a lazy decoder:

* **A DRY BLOCK and a WET BLOCK.** The lake is confined to the left half, so
  blocks (1,0) and (1,1) are entirely outside every extent and encode as
  MODE_CONSTANT -- the encoding a real tile is almost entirely made of, and the
  one where a decoder can most easily hand back zeros instead of the sentinel.
  Zero on the DEPTH plane is a wet cell, and zero on the SHORE plane is
  "exactly on the waterline", so a zero-filled block is wrong twice over.
* **THE DRY SENTINEL (-1) SURVIVING**, which is the one value on the depth
  plane that must never be multiplied by the LSB.
* **A DEPTH OF EXACTLY 0**, one LSB off the sentinel and genuinely WET: the
  outermost ring of the extent, where the datum meets the ground. That is the
  boundary an ``if (d <= 0) dry`` bug gets wrong.
* **BOTH SIGNS OF THE SHORE DISTANCE**, plus the saturated value at the clamp,
  because the sign is the only thing distinguishing "shallow water" from "wet
  ground beside water" and a decoder that read the plane as unsigned would pass
  every other check here.
* **Both codecs**, because CODEC_ZSTD is what production ships and its block
  payloads take a different path entirely.

WHERE THIS LIVES, and why it is not beside its siblings in
terrain-service/tools/: the terrain-service tree was frozen when the C++ side
of bake_ver 27 was written, and this script is a consumer-side artefact. It
imports terrain_service exactly as they do and writes to the same fixtures
directory; move it in next to make_water_fixture.py whenever that tree is open.

Run:
    python tools/make_bathy_fixture.py
It rewrites voxel-core/tests/fixtures/vxtl_v2_golden_bathy_512.vxtl and
...bathy_zstd_512.vxtl in place.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "terrain-service"))

from terrain_service import tile_codec as tc            # noqa: E402
from terrain_service.bake import basins as bs           # noqa: E402

FIX = ROOT / "voxel-core" / "tests" / "fixtures"
OUT_RAW = FIX / "vxtl_v2_golden_bathy_512.vxtl"
OUT_ZSTD = FIX / "vxtl_v2_golden_bathy_zstd_512.vxtl"

SIZE = 512
BLOCK_LOG2 = 8          # 256 px blocks -> 2x2 = 4 blocks, so "a dry block" is
                        # a quarter of the tile and easy to point at.
BASE_OFFSET_MM = 100_000
QUANT = tc.QUANT_100MM
CELL_M = 1.875          # the fine pixel pitch, what the distance transform is
                        # scaled by (bake/pipeline.py hands the same number in).

# A bowl centred here, entirely inside the left half of the tile.
LAKE_CX, LAKE_CY = 128.0, 200.0
LAKE_R = 60.0           # px
LAKE_DEPTH_M = 12.0     # how deep the BOWL is cut below the plain
# The water datum. BELOW THE LOWEST POINT OF THE SURROUNDING PLAIN (100 m at the
# tile's near corner) on purpose: the extent is the connected component of
# {ground <= surface} containing the seed, so a datum above the plain does not
# make a bigger lake, it makes the whole tile one -- which is what the first
# version of this fixture did, and it took the dry blocks with it.
LAKE_SURFACE_M = 99.0


def samples() -> np.ndarray:
    """A plain slope with one paraboloid bowl punched into its left half.

    Deliberately NOT the water fixture's valley: this fixture is about a LAKE
    (a closed extent with a shoreline all the way round), which is the shape a
    distance transform has something to say about. A river's shoreline is two
    parallel lines and would not exercise the sign flip at a curved boundary.
    """
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
    ground = 100.0 + 0.01 * y + 0.004 * x
    r = np.hypot(x - LAKE_CX, y - LAKE_CY) / LAKE_R
    bowl = np.where(r < 1.0, -LAKE_DEPTH_M * (1.0 - r * r), 0.0)
    return ground + bowl


def main() -> int:
    bed = samples()
    elev_cp, _b, _q = tc.elevation_control_points(
        bed, base_offset_mm=BASE_OFFSET_MM, quant=QUANT)

    surface_m = LAKE_SURFACE_M
    assert bed[int(LAKE_CY), int(LAKE_CX)] < surface_m < bed.max(), "the bowl must hold water"

    # THROUGH THE REAL PRODUCER, not a reimplementation. `bathymetry_planes` is
    # the definition of both planes (extent fill, Euclidean transform, clamp,
    # quantisation); a fixture built from a hand-rolled copy of it would pin the
    # copy. It takes BasinRecords, so build the one record this bowl is.
    # Only the five fields `bathymetry_planes` reads are meaningful (seed_px,
    # bbox_px, surface_m, kind via is_lake); the rest are the survey's own
    # bookkeeping and are filled with plausible zeros rather than omitted,
    # because BasinRecord requires them and inventing a lighter stand-in would
    # be the reimplementation this generator exists to avoid.
    rec = bs.BasinRecord(
        basin_id=0,
        seed_px=(int(LAKE_CX), int(LAKE_CY)),
        bbox_px=(0, 0, SIZE - 1, SIZE - 1),
        area_cells=0,
        area_m2=0.0,
        floor_m=float(bed[int(LAKE_CY), int(LAKE_CX)]),
        spill_m=surface_m,
        surface_m=surface_m,
        kind=bs.KIND_LAKE_TERMINAL,
        outlet_px=(int(LAKE_CX), int(LAKE_CY)),
        catchment_m2=0.0,
        near_padded_border=False,
        interior=True,
        temp_c=10.0,
        precip_mm=800.0,
        precip_cv=0.2,
        temp_seasonality=1.0,
        pet_mm=600.0,
        runoff_mm=200.0,
        inflow_m3_yr=0.0,
        balance_area_m2=0.0,
    )
    depth, shore = bs.bathymetry_planes(bed, [rec], cell_m=CELL_M)

    # -- the properties this fixture exists to carry, asserted before writing --
    assert depth.shape == (SIZE, SIZE) and depth.dtype == np.int16
    assert shore.shape == (SIZE, SIZE) and shore.dtype == np.int16

    wet = depth >= 0
    assert wet.any() and (~wet).any(), "need both wet and dry cells"
    # THE RIGHT HALF IS ENTIRELY DRY, so blocks (1,0) and (1,1) are CONSTANT at
    # the sentinel and a decoder that zero-fills produces a lake there.
    assert (depth[:, 256:] == bs.BATHY_DRY_DEPTH).all(), "the right half must be dry"
    assert (depth[depth < 0] == bs.BATHY_DRY_DEPTH).all(), "one dry sentinel only"

    # A DEPTH OF EXACTLY 0 -- wet, at the bed. Force one at the extent's edge if
    # the quantisation did not already land on it, and force it INSIDE the wet
    # mask so it stays a wet cell rather than becoming a second sentinel.
    if not (depth[wet] == 0).any():
        ys, xs = np.nonzero(wet & (depth == depth[wet].min()))
        depth[ys[0], xs[0]] = 0
        wet = depth >= 0
    assert (depth[wet] == 0).any(), "need a stored depth of exactly 0"
    assert depth.max() > 100, "need a real depth too, not just the boundary"

    # BOTH SIGNS, and the clamp. Positive inside water, negative on land,
    # saturating at +/-1000 units (100 m).
    assert (shore > 0).any() and (shore < 0).any(), "shore distance must be signed"
    assert shore.min() == -bs.BATHY_SHORE_CLAMP_M * 1000.0 / bs.BATHY_SHORE_LSB_MM, (
        "the land side must reach the clamp")
    assert shore.max() <= bs.BATHY_SHORE_CLAMP_M * 1000.0 / bs.BATHY_SHORE_LSB_MM
    # THE SIGN AGREES WITH THE DEPTH PLANE cell for cell, which is the cross-
    # check neither plane can make on its own.
    assert np.array_equal(shore > 0, wet), "sign disagrees with the wet mask"

    # No cell sits ON the waterline: the transform measures to the nearest cell
    # of the opposite class, so the closest anything gets is one pixel. This is
    # the measurement behind tilestore.h's "zero reads as land" note.
    nearest = int(np.abs(shore).min())
    assert nearest > 0, "a zero shore distance would leave the boundary undefined"

    print(f"  lake: {int(wet.sum()):,} wet px of {depth.size:,}, "
          f"max depth {depth.max() * bs.BATHY_DEPTH_LSB_MM / 1000.0:.2f} m")
    # int() before the multiply: these are int16 and 1000 * 100 overflows the
    # numpy scalar rather than promoting, which is the same trap the C++ side
    # avoids by converting to int32 in bathyShoreMm.
    print(f"  shore: [{int(shore.min())}, {int(shore.max())}] units "
          f"({int(shore.min()) * bs.BATHY_SHORE_LSB_MM / 1000.0:.0f} m .. "
          f"{int(shore.max()) * bs.BATHY_SHORE_LSB_MM / 1000.0:.0f} m), "
          f"nearest to the waterline {nearest} units")

    written = []
    for out, codec in ((OUT_RAW, tc.CODEC_RAW), (OUT_ZSTD, tc.CODEC_ZSTD)):
        if codec == tc.CODEC_ZSTD and not tc.HAVE_ZSTD:
            print(f"skipping {out.name}: zstandard is not installed")
            continue
        tile = tc.TileV2(
            seed=20260719, x=-2, y=-4, size=SIZE,
            elevation_cp=elev_cp,
            base_offset_mm=BASE_OFFSET_MM, quant=QUANT, codec=codec,
            bake_ver=27, block_log2=BLOCK_LOG2,
            bathy_depth=depth, bathy_shore=shore,
        )
        data = tc.encode_v2(tile)
        # Prove it before writing it: a fixture that does not round-trip through
        # its own encoder cannot be evidence about anyone else's.
        back = tc.decode_v2(data)
        assert back.bathy_depth is not None and back.bathy_shore is not None, (
            "the flag or the sections went missing")
        assert np.array_equal(back.bathy_depth, depth), "round trip changed depth"
        assert np.array_equal(back.bathy_shore, shore), "round trip changed shore"
        assert np.array_equal(back.elevation_cp, elev_cp)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        written.append((out, len(data)))

    # The pinned numbers the C++ test asserts, printed so they are transcribed
    # from a run rather than from an expectation.
    print("\n  C++ test pins:")
    print(f"    wet px                 {int(wet.sum())}")
    print(f"    depth[200][128]        {depth[200][128]}")
    print(f"    shore[200][128]        {shore[200][128]}")
    print(f"    depth[0][511]          {depth[0][511]}")
    print(f"    shore[0][511]          {shore[0][511]}")
    print(f"    max depth units        {depth.max()}")
    print(f"    min |shore| units      {nearest}")

    for out, n in written:
        print(f"wrote {out} ({n:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
