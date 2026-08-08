"""Generate the SECTION_WATER_* conformance fixture (watershed plan P2).

WHY A FIXTURE AND NOT JUST A ROUND-TRIP TEST -- the same argument
``make_basin_fixture.py`` makes, and it applies harder here. The Python encoder
and the C++ decoder are written independently against
docs/vxtl-v2-format.md, so a round-trip test in either language proves only
that that language agrees with itself. A committed file both sides parse is the
only thing that catches a disagreement.

It applies harder because the water plane's failure mode is SILENT. A basin
table that mis-parses raises; a water plane that mis-parses draws a river in the
wrong place, or -- much worse -- reads the dry sentinel as an ordinary control
point and floods the tile with water 3.2 km underground. Nothing throws.

The plane it writes is chosen to break a lazy decoder:

* **A DRY BLOCK and a WET BLOCK.** An all-wet plane never exercises
  MODE_CONSTANT, which is the encoding every real tile is mostly made of (a
  production tile is ~99% dry) and the one where a decoder can most easily
  return zeros instead of the sentinel.
* **THE SENTINEL AT A BLOCK BOUNDARY**, so a decoder that fills a fresh block
  with 0 rather than `kWaterDryCp` produces water exactly where the seam is.
* **A DEPTH OF ZERO**, one LSB off the `-1` sentinel, which must decode as
  WATER at exactly the bed. That is the boundary an `if (d <= 0) dry` bug gets
  wrong, and it is reachable: a reach's shallow edge quantises there.
* **A ROUGH BED under a SMOOTH reach.** The whole point of storing depth is
  that the bed's 100 mm quantisation cancels, so the fixture's bed carries
  sub-LSB roughness the water must NOT inherit. A decoder that added depth to
  the wrong bed, or to an amplified surface, reproduces the roughness and
  fails the smoothness check.
* **A monotonically descending reach**, so a decoder can be checked against
  the invariant the bake enforces (water falls downstream) rather than only
  against stored values.
* **Both codecs**, because CODEC_ZSTD is what production ships (24 of the 38
  resident tiles) and its block payloads take a different path entirely.

Run:
    python tools/make_water_fixture.py
It rewrites voxel-core/tests/fixtures/vxtl_v2_golden_water_512.vxtl and
...water_zstd_512.vxtl in place.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402

FIX = Path(__file__).resolve().parents[2] / "voxel-core" / "tests" / "fixtures"
OUT_RAW = FIX / "vxtl_v2_golden_water_512.vxtl"
OUT_ZSTD = FIX / "vxtl_v2_golden_water_zstd_512.vxtl"

SIZE = 512
BLOCK_LOG2 = 8          # 256 px blocks -> 2x2 = 4 blocks, so "a dry block" is
                        # a quarter of the tile and easy to point at.
BASE_OFFSET_MM = 100_000
QUANT = tc.QUANT_100MM


def samples() -> np.ndarray:
    """A valley running north-south, so a river down it is geometrically sane.

    The bed carries a small high-frequency term deliberately: it is what the
    stored depth has to cancel, and a fixture with a perfectly smooth bed would
    not exercise the cancellation at all.
    """
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
    return (100.0 + 0.20 * np.abs(x - 128.0) - 0.002 * y
            + 0.043 * np.sin(y * 1.7) * np.cos(x * 2.3))


def _reconstruct_ground_mm(elev_cp: np.ndarray) -> np.ndarray:
    """The client's read of the ground: the cubic B-spline on the lattice, mm.

    `prefilter=False` because `elevation_control_points` has ALREADY run the
    prefilter -- that is what makes these control points rather than samples.
    Running it again would filter twice and the reconstruction would no longer
    interpolate anything.
    """
    from scipy import ndimage

    n = elev_cp.shape[0]
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float64)
    mm = tc.control_points_to_mm(elev_cp.astype(np.int64), BASE_OFFSET_MM,
                                 QUANT).astype(np.float64)
    return ndimage.map_coordinates(mm, [yy, xx], order=3, mode="nearest",
                                   prefilter=False)


def water_surface() -> np.ndarray:
    """NaN everywhere except one descending reach, entirely inside block (0,*).

    Deliberately confined to the left half so blocks (1,0) and (1,1) are
    ENTIRELY DRY and must encode as MODE_CONSTANT at the sentinel.
    """
    w = np.full((SIZE, SIZE), np.nan)
    ys = np.arange(SIZE)
    # ONE LEVEL ACROSS THE REACH, taken from the CENTRELINE bed plus a head --
    # a water surface is flat in cross-section, which is the whole reason the
    # banks rise away from it. Referenced to the centreline's SMOOTH bed
    # (100 - 0.05y, without the roughness term) so the stored depth has real
    # roughness to cancel.
    surf = (100.0 - 0.002 * ys) + 0.8
    for dx in range(-2, 3):
        w[ys, 128 + dx] = surf
    return w


def main() -> int:
    w = water_surface()
    bed = samples()
    # THROUGH THE PREFILTER, exactly as `encode_fine` does. Building the
    # lattice with `mm_to_control_points` instead treats the samples AS control
    # points, so `spline(cp)` no longer interpolates them and the measured
    # reconstruction residual is a property of the fixture rather than of the
    # format -- 116.7 mm instead of the real 42 mm, which is how this was found.
    elev_cp, _b, _q = tc.elevation_control_points(
        bed, base_offset_mm=BASE_OFFSET_MM, quant=QUANT)
    cp = tc.water_depth_control_points(w, bed, BASE_OFFSET_MM, QUANT)
    cp_true = cp.copy()   # before the deliberate zero-depth poke below

    # -- the properties this fixture exists to carry, asserted before writing --
    # WETNESS IS `>= 0`, NOT `!= -1`. Identical today, because this fixture is
    # built without a level band and the encoder then emits only -1 and depths.
    # It stops being identical the moment anything hands this generator a plane
    # carrying levels: -1 is no longer the only negative (see
    # tile_codec.WATER_NO_LEVEL), and a level of -30000 would be classified WET
    # here and blow the depth_err assert below with a nonsense number.
    wet = cp >= 0
    assert wet.any() and (~wet).any(), "need both wet and dry cells"
    assert (cp[:, 256:] == tc.WATER_DRY_DEPTH).all(), "the right half must be dry"
    # The sentinel must sit ON a block boundary, both sides.
    assert cp[0, 255] == tc.WATER_DRY_DEPTH and cp[0, 256] == tc.WATER_DRY_DEPTH
    # A depth of exactly 0 -- one LSB off the sentinel, and WATER.
    forced = np.nonzero(wet)
    cp[forced[0][0], forced[1][0]] = 0
    assert (cp >= 0).sum() > 0

    # -- WHAT IS EXACT, AND WHAT IS NOT. Both measured, because the difference
    # is the whole of the depth-against-samples decision.
    #
    # EXACT: the stored DEPTH, to half an LSB against the surface the bake
    # computed. This is what a renderer reads to decide how deep the water is
    # over a column, and it is the quantity that must not wobble.
    depth_err = np.abs((cp_true[wet].astype(np.float64) * tc.WATER_DEPTH_LSB_MM)
                       - (np.rint(w[wet] * 1000.0) - np.rint(bed[wet] * 1000.0)))
    assert depth_err.max() <= tc.WATER_DEPTH_LSB_MM / 2.0 + 1e-9, (
        f"stored depth is off by {depth_err.max():.1f} mm, over half an LSB")

    # NOT EXACT: the ABSOLUTE water surface, which the client rebuilds as
    # spline(cp) + depth and which therefore carries the SPLINE's own
    # reconstruction residual. That residual is the ground's error too, so
    # water and ground wobble TOGETHER and the depth between them stays right --
    # but a reach is not perfectly flat in absolute terms, and pretending
    # otherwise would be the kind of claim this branch has had to retract
    # before. Bounded and measured here rather than asserted from theory.
    back = tc.water_surface_mm_from_depth(cp_true, _reconstruct_ground_mm(elev_cp))
    err = np.abs(back[wet] - np.rint(w[wet] * 1000.0))
    resid = np.abs(_reconstruct_ground_mm(elev_cp) - np.rint(bed * 1000.0))
    print(f"  stored depth error:      max {depth_err.max():.1f} mm "
          f"(half-LSB {tc.WATER_DEPTH_LSB_MM / 2.0:.0f})")
    print(f"  |spline(cp) + depth - water_true|: p50 {np.percentile(err, 50):.1f}"
          f"  p95 {np.percentile(err, 95):.1f}  max {err.max():.1f} mm "
          f"(one voxel = 100 mm)")
    print(f"  of which the spline's own residual: p95 "
          f"{np.percentile(resid, 95):.1f}  max {resid.max():.1f} mm")
    assert err.max() <= 100.0, (
        f"water reconstruction is off by {err.max():.0f} mm, over a whole "
        "voxel; referencing the depth to the sample field is not sound here")

    # The reach still DESCENDS in the quantity that is exact -- the surface the
    # bake stored -- which is the invariant the grading sweep guarantees.
    # DESCENT IS PRESERVED TO WITHIN ONE LSB, NOT STRICTLY, and the difference
    # is a real property of depth encoding rather than a slack tolerance.
    # `stored = bed + round((water - bed) / LSB) * LSB` rounds relative to a bed
    # that carries its own roughness, so on a reach gentler than that roughness
    # the rounding can step the reconstructed surface UP by one LSB between
    # adjacent pixels even though the true surface falls throughout. At a 10 mm
    # LSB that is a 1/10-voxel wobble on a surface the renderer draws at 100 mm,
    # and the DEPTH -- the quantity a renderer actually reads -- stays exact.
    # Stating it because "strictly descending" was the invariant an absolute
    # plane would have given, and this encoding does not give it.
    stored = (np.rint(bed[:, 128] * 1000.0)
              + cp_true[:, 128].astype(np.float64) * tc.WATER_DEPTH_LSB_MM)
    steps = np.diff(stored)
    assert steps.max() <= tc.WATER_DEPTH_LSB_MM, (
        f"stored reach rises by {steps.max():.0f} mm, more than one LSB")
    assert (steps < 0).any(), "the reach must actually descend somewhere"
    # And it descends OVERALL, which is the statement that matters at reach
    # scale: no chain of puddles.
    assert stored[-1] < stored[0] - 1000.0, "the reach does not descend at all"
    print(f"  stored reach: net fall {(stored[0] - stored[-1]) / 1000.0:.2f} m, "
          f"worst upward step {max(steps.max(), 0):.0f} mm "
          f"(LSB {tc.WATER_DEPTH_LSB_MM})")

    written = []
    for out, codec in ((OUT_RAW, tc.CODEC_RAW), (OUT_ZSTD, tc.CODEC_ZSTD)):
        if codec == tc.CODEC_ZSTD and not tc.HAVE_ZSTD:
            print(f"skipping {out.name}: zstandard is not installed")
            continue
        tile = tc.TileV2(
            seed=20260719, x=-2, y=-4, size=SIZE,
            elevation_cp=elev_cp,
            base_offset_mm=BASE_OFFSET_MM, quant=QUANT, codec=codec,
            bake_ver=9, block_log2=BLOCK_LOG2, water_cp=cp,
        )
        data = tc.encode_v2(tile)
        # Prove it before writing it: a fixture that does not round-trip
        # through its own encoder cannot be evidence about anyone else's.
        back = tc.decode_v2(data)
        assert back.water_cp is not None, "the flag or the section went missing"
        assert np.array_equal(back.water_cp, cp), "round trip changed the plane"
        assert np.array_equal(back.elevation_cp, tile.elevation_cp)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        written.append((out, len(data)))

    for out, n in written:
        print(f"wrote {out} ({n:,} bytes, {int(wet.sum()):,} wet px of "
              f"{cp.size:,})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
