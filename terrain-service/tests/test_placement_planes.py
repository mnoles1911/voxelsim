"""SECTION_PLACE_* (bake_ver 28): the codec round trip and the bake module.

Same posture as the bathymetry tests: the wire rules are pinned here against
docs/vxtl-v2-format.md section 6.2, and the geomorphology is pinned against
its own stated properties -- above all the ROUNDING DIRECTION of the distance
plane (it must OVER-cover wetness; see the lake-extent rounding rule).
"""

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402
from terrain_service.bake import placement as pl  # noqa: E402


def _planes(sub: int, fill=None):
    rng = np.random.default_rng(11)
    def one(v=None):
        if v is not None:
            return np.full((sub, sub), v, np.uint8)
        return rng.integers(0, 256, (sub, sub)).astype(np.uint8)
    return {
        "place_dist_water": one(fill),
        "place_twi": one(fill),
        "place_talus": one(fill),
        "place_curv": one(fill),
        "place_heat": one(fill),
    }


def _tile(size=512, planes=None, **kw):
    elev = np.zeros((size, size), np.int16)
    return tc.TileV2(seed=7, x=0, y=0, size=size, elevation_cp=elev,
                     bake_ver=28, **(planes or {}), **kw)


def test_round_trip_all_five_planes_exact():
    planes = _planes(128)
    d = tc.decode_v2(tc.encode_v2(_tile(planes=planes)))
    for k, v in planes.items():
        assert np.array_equal(getattr(d, k), v), k


def test_absent_planes_decode_to_none_and_flag_stays_clear():
    blob = tc.encode_v2(_tile())
    d = tc.decode_v2(blob)
    assert d.place_dist_water is None and d.place_heat is None
    # And the flag bit is genuinely clear on the wire.
    flags = int.from_bytes(blob[31:33], "little")
    assert not (flags & tc.FLAG_PLACEMENT_PRESENT)


def test_partial_planes_are_refused_at_construction():
    with pytest.raises(AssertionError):
        _tile(planes={"place_dist_water": np.zeros((128, 128), np.uint8)})


def test_flag_without_sections_and_sections_without_flag_are_refused():
    blob = bytearray(tc.encode_v2(_tile()))
    flags = int.from_bytes(blob[31:33], "little") | tc.FLAG_PLACEMENT_PRESENT
    blob[31:33] = flags.to_bytes(2, "little")
    with pytest.raises(ValueError, match="placement flag set"):
        tc.decode_v2(bytes(blob))

    blob2 = bytearray(tc.encode_v2(_tile(planes=_planes(128))))
    flags2 = int.from_bytes(blob2[31:33], "little") & ~tc.FLAG_PLACEMENT_PRESENT
    blob2[31:33] = flags2.to_bytes(2, "little")
    with pytest.raises(ValueError, match="placement sections present"):
        tc.decode_v2(bytes(blob2))


def test_constant_planes_cost_zero_data_bytes():
    # The commonest real case -- a tile far from water with no cliffs -- must
    # encode each plane as CONSTANT blocks: index only, no payload.
    bare = len(tc.encode_v2(_tile()))
    flat = len(tc.encode_v2(_tile(planes=_planes(128, fill=255))))
    n_blocks = (128 // (1 << tc.placement_block_log2(512, 8))) ** 2
    # 5 planes x (index entry per block + one 20-byte section-table entry x2).
    overhead = 5 * (n_blocks * 20 + 2 * 20)
    assert flat == bare + overhead


def test_derived_block_log2_rule():
    assert tc.placement_block_log2(8192, 8) == 8   # production: 8x8 blocks
    assert tc.placement_block_log2(512, 8) == 7    # fixture: one 128-px block
    assert tc.placement_block_log2(1024, 8) == 8   # 256-px plane, one block


# --- the bake module ---------------------------------------------------------

def _bake(z_pad, acc=None, river=None, lake=None, pad=16):
    full = z_pad.shape[0]
    sl = slice(pad, full - pad)
    return pl.placement_planes(
        z_pad=z_pad,
        acc_pad=acc if acc is not None else np.full(z_pad.shape, 10.0),
        interior=sl, cell_m=1.875,
        lake_wet_interior=lake, river_wet_pad=river,
    )


def test_distance_over_covers_the_wet_set():
    # A single wet FINE pixel must make its whole 4x4 subsampled cell read 0
    # (min-pool), and floor quantisation must never round a distance UP.
    pad, edge = 16, 256
    full = edge + 2 * pad
    z = np.full((full, full), 100.0, np.float32)  # everything above the sea
    river = np.zeros((full, full), bool)
    river[pad + 33, pad + 65] = True  # one wet pixel, interior (33, 65)
    out = _bake(z, river=river)
    d = out["dist_water"]
    assert d[33 // 4, 65 // 4] == 0
    # A cell 64 fine px (~120 m) away reads at most its true distance
    # (over-cover: min-pool + floor can only round DOWN).
    assert d[8, 32] * 2.0 <= 64 * 1.875 + 1e-6
    # Far corner is far but KNOWN (there is water in this tile).
    assert d[63, 0] > 50


def test_no_water_anywhere_is_the_unknown_code_everywhere():
    pad, edge = 16, 256
    full = edge + 2 * pad
    z = np.full((full, full), 100.0, np.float32)
    out = _bake(z)
    assert (out["dist_water"] == tc.PLACEMENT_DIST_UNKNOWN).all()


def test_the_sea_is_wet_without_any_sampler():
    pad, edge = 16, 256
    full = edge + 2 * pad
    z = np.full((full, full), 100.0, np.float32)
    z[:, : pad + 40] = -5.0  # a coast along the low-x edge
    out = _bake(z)
    assert out["dist_water"][32, 0] == 0
    assert out["dist_water"][32, 60] > 0


def test_twi_is_higher_in_the_channel_than_on_the_ridge():
    pad, edge = 16, 256
    full = edge + 2 * pad
    yy, xx = np.mgrid[0:full, 0:full].astype(np.float32)
    # A valley: ground rises away from the x = centre line.
    z = 100 + np.abs(xx - full / 2) * 0.5
    acc = np.full((full, full), 10.0)
    acc[:, full // 2 - 2 : full // 2 + 2] = 1e6  # the channel accumulates
    out = _bake(z, acc=acc)
    twi = out["twi"].astype(int)
    channel = twi[32, (full // 2 - pad) // 4]
    ridge = twi[32, 8]
    assert channel > ridge + 8  # > 1.0 TWI unit at the 8-per-unit scale


def test_talus_deposits_below_the_cliff_not_on_it():
    pad, edge = 16, 256
    full = edge + 2 * pad
    yy = np.mgrid[0:full, 0:full][0].astype(np.float32)
    cell = 1.875 * 4  # the subsampled cell the sweep runs on
    # A steep face between two shelves: rows 30-40 (subsampled) drop hard.
    z = np.where(yy < pad + 120, 300.0, np.where(yy < pad + 160, 0.0, 0.0)).astype(np.float32)
    # Smooth the cliff over a few rows so the gradient is finite but steep.
    ramp = np.clip((yy - (pad + 120)) / 40.0, 0, 1)
    z = 300.0 * (1.0 - ramp).astype(np.float32)
    out = _bake(z)
    talus = out["talus"].astype(int)
    cliff_rows = talus[31:39, :].sum()
    below_rows = talus[40:48, :].sum()
    assert below_rows > 0, "no deposit below the face"
    assert below_rows > cliff_rows, "debris parked on the cliff face"


def test_curvature_reads_concave_in_a_bowl_and_convex_on_a_dome():
    pad, edge = 16, 256
    full = edge + 2 * pad
    yy, xx = np.mgrid[0:full, 0:full].astype(np.float32)
    r2 = (xx - full / 2) ** 2 + (yy - full / 2) ** 2
    bowl = (r2 * 0.001).astype(np.float32)
    dome = (-r2 * 0.001).astype(np.float32)
    centre = (edge // 8, edge // 8)  # subsampled centre
    assert _bake(bowl)["curv"][centre] > 128
    assert _bake(dome)["curv"][centre] < 128


def test_heat_load_is_hot_on_the_sw_face_and_cold_on_the_ne():
    pad, edge = 16, 256
    full = edge + 2 * pad
    yy, xx = np.mgrid[0:full, 0:full].astype(np.float32)
    # Ground falling toward -x,-y: the FACING direction is SW (-x = west,
    # -y = south with +y north), so this slope takes the afternoon sun. Steep
    # (tan ~2.8) so the steepness weight s/(s+1) is near saturation.
    swFace = ((xx + yy) * 2.0).astype(np.float32)
    neFace = (-(xx + yy) * 2.0).astype(np.float32)
    c = (32, 32)
    hot = int(_bake(swFace)["heat"][c])
    cold = int(_bake(neFace)["heat"][c])
    assert hot > 190 and cold < 65, (hot, cold)
