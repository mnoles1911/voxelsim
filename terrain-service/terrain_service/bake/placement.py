"""The placement channel planes (bake_ver 28) -- SECTION_PLACE_*.

Five uint8 rasters per fine tile, subsampled 4x (7.5 m/px at production),
consumed by ASSET PLACEMENT in voxel-core (`assetpolicy.h`): distance to
water, TWI moisture, talus flux, curvature, heat load. The wire encodings are
normative in docs/vxtl-v2-format.md section 6.2 and mirrored in
voxelcore/tilestore.h; this module owns the GEOMORPHOLOGY, tile_codec owns the
bytes -- the same split the bathymetry pair uses.

Why these five, and why baked: docs/placement-research.md section 3-4 (every
shipped pipeline surveyed there -- Horizon, Far Cry 5, Witcher -- conditions
vegetation and rocks on baked terrain channels), and
docs/asset-placement-audit.md section 3 (the water-distance gap: 112 authored
riparian/reed/shore species fail closed because nothing serves a distance).
Placement is worldgen -- a pure function of (seed, tile bytes) -- so anything
it reads must be tile bytes; a runtime search over neighbours would be
order-dependent and is banned by the determinism contract.

THE ROUNDING DIRECTION IS THE LAKE-EXTENT LESSON. Water masks round two ways:
a mask used to VETO must over-cover (prefer wet), a mask used to grant a
near-water BONUS must under-cover (never land the bonus in open water). The
distance plane here serves a tolerance gate and is built to OVER-cover --
distance is computed at full fine resolution and MIN-pooled 4x4, then
FLOOR-quantised, so a subsampled cell containing any wet fine pixel reads 0
and no consumer ever reads "farther from water than true". The other
direction is not this plane's job: what keeps a reed out of open water is the
standing-water veto in `assetSpeciesTolerates`, which reads the SAME basin
datum the renderer draws, not a raster.
"""

from __future__ import annotations

import numpy as np

from ..tile_codec import (
    PLACEMENT_CURV_SCALE,
    PLACEMENT_DIST_LSB_M,
    PLACEMENT_DIST_UNKNOWN,
    PLACEMENT_HEAT_SCALE,
    PLACEMENT_SUBSAMPLE,
    PLACEMENT_TALUS_SCALE,
    PLACEMENT_TWI_OFFSET,
    PLACEMENT_TWI_SCALE,
)

#: Slope (rise/run) at and above which a cell SHEDS debris -- the talus
#: sweep's source set. tan 45 deg: comfortably past the 70% grade at which
#: `classifyBiome` reads BARE_ROCK, so a source cell is unambiguously cliff.
TALUS_CLIFF_TAN = 1.0
#: Slope below which debris SETTLES -- the angle of repose, ~38 deg. Scree
#: stands at 34-38 deg in the field (docs/placement-research.md section 4.1);
#: flux keeps moving on ground steeper than this and deposits where the
#: ground first relents.
TALUS_REPOSE_TAN = 0.78
#: Fixed pass count for the talus sweep. 64 passes at 7.5 m/px is a 480 m
#: maximum runout, generous against real scree aprons (tens to ~200 m).
#: FIXED rather than run-to-convergence, deliberately: the plane must be a
#: pure function of the inputs, and "iterate until quiet" is a data-dependent
#: iteration count that would make two bakes of one world disagree over a
#: float wobble.
TALUS_PASSES = 64
#: tan(beta) floor inside the TWI logarithm -- the standard guard against
#: ln(a/0) on flat cells (0.1% grade).
TWI_TAN_FLOOR = 1e-3

# The eight D8 neighbours, with their step distance in cells.
_D8 = (
    (-1, 0, 1.0), (1, 0, 1.0), (0, -1, 1.0), (0, 1, 1.0),
    (-1, -1, 1.4142135623730951), (-1, 1, 1.4142135623730951),
    (1, -1, 1.4142135623730951), (1, 1, 1.4142135623730951),
)


def _pool(a: np.ndarray, k: int, reduce: str) -> np.ndarray:
    """k x k block pooling. `reduce` is 'min' or 'mean'."""
    h, w = a.shape
    assert h % k == 0 and w % k == 0
    b = a.reshape(h // k, k, w // k, k)
    if reduce == "min":
        return b.min(axis=(1, 3))
    if reduce == "mean":
        return b.mean(axis=(1, 3), dtype=np.float64)
    raise ValueError(reduce)


def _steepest_descent(z: np.ndarray, cell_m: float):
    """Per-cell D8 steepest-descent receiver offsets and the drop rate.

    Returns (rec_dy, rec_dx int8, drop float32 m/m). Cells with no lower
    neighbour keep (0, 0) and drop 0 -- a pit or a flat, where flux settles.
    """
    h, w = z.shape
    best = np.zeros((h, w), np.float32)
    rec_dy = np.zeros((h, w), np.int8)
    rec_dx = np.zeros((h, w), np.int8)
    zi = np.asarray(z, np.float32)
    for dy, dx, dist in _D8:
        shifted = np.full((h, w), np.float32(np.inf))
        ys = slice(max(dy, 0), h + min(dy, 0))
        yd = slice(max(-dy, 0), h + min(-dy, 0))
        xs = slice(max(dx, 0), w + min(dx, 0))
        xd = slice(max(-dx, 0), w + min(-dx, 0))
        shifted[yd, xd] = zi[ys, xs]
        drop = (zi - shifted) / np.float32(dist * cell_m)
        sel = drop > best
        best[sel] = drop[sel]
        rec_dy[sel] = dy
        rec_dx[sel] = dx
    return rec_dy, rec_dx, best


def talus_flux(z_sub: np.ndarray, tan_sub: np.ndarray, cell_m: float,
               passes: int = TALUS_PASSES) -> np.ndarray:
    """Deposited debris flux on the subsampled grid (float32, >= 0).

    A LOCAL, FIXED-PASS raster sweep, not a simulation: every cell at or above
    TALUS_CLIFF_TAN sheds one unit of flux scaled by its own steepness; each
    pass, flux standing on ground steeper than the angle of repose moves one
    cell along the steepest descent; flux on gentler ground settles where it
    is. After `passes` passes anything still moving settles in place -- a
    bounded runout, not a loss, and the bound is the purity argument for the
    fixed count (see TALUS_PASSES).
    """
    rec_dy, rec_dx, drop = _steepest_descent(z_sub, cell_m)
    carry = np.where(tan_sub >= TALUS_CLIFF_TAN, tan_sub, 0.0).astype(np.float32)
    deposit = np.zeros_like(carry)
    h, w = carry.shape
    moving_ground = (tan_sub > TALUS_REPOSE_TAN) & (drop > 0)
    for _ in range(passes):
        iy, ix = np.nonzero(carry > 0)
        if iy.size == 0:
            break
        m = moving_ground[iy, ix]
        sy, sx = iy[~m], ix[~m]
        deposit[sy, sx] += carry[sy, sx]
        my, mx = iy[m], ix[m]
        moved = carry[my, mx]
        carry = np.zeros_like(carry)
        ny = my + rec_dy[my, mx]
        nx = mx + rec_dx[my, mx]
        np.add.at(carry, (ny, nx), moved)
    # Whatever is still in flight after the runout bound settles where it is.
    deposit += carry
    # The face itself stays clean WITHOUT an explicit erase: flux never
    # settles where the descent continues (tan > repose AND a lower
    # neighbour), so the only steep-by-central-difference cells that hold
    # deposit are the ones where the descent DIES -- cliff toes and pits,
    # which is exactly where a talus apron begins. (An earlier draft erased
    # deposit at every tan >= cliff cell and deleted the toe row's apron
    # wholesale -- the toe's centred slope still reads steep while its
    # forward drop is already zero. test_talus_deposits_below_the_cliff_
    # not_on_it is the regression that caught it.)
    return deposit


def placement_planes(
    *,
    z_pad: np.ndarray,
    acc_pad: np.ndarray,
    interior: slice,
    cell_m: float,
    lake_wet_interior: "np.ndarray | None",
    river_wet_pad: "np.ndarray | None",
) -> dict[str, np.ndarray]:
    """The five SECTION_PLACE_* planes, already in wire units.

    `z_pad` / `acc_pad` are the bake's PADDED final surface (m) and MFD
    accumulation (m^2); `interior` slices the shipped window. The wet set for
    the distance plane is the union docs/asset-placement-audit.md section 12.6
    demanded -- and getting the union wrong in exactly one of these three ways
    is why the audit calls the lake-only `bathy_shore` plane a trap:

      * `lake_wet_interior` -- bool/int16 wet-lake mask over the INTERIOR
        (`bathy_depth >= 0` from the B5b pass). Interior-only, so a lake that
        exists solely in a neighbour's tile is invisible here; the error
        saturates toward "far", the fail-closed direction (the neighbour's own
        plane carries its shoreline).
      * `river_wet_pad` -- finite cells of the graded water plane, PADDED, so
        a river across the seam still pushes a true distance into this tile.
        None when the tile baked without a water plane.
      * the sea -- `z_pad <= 0` (kSeaLevelMm is the datum, never baked).

    Returns {"dist_water", "twi", "talus", "curv", "heat"} uint8 at
    (edge // PLACEMENT_SUBSAMPLE)^2.
    """
    from scipy import ndimage  # bake-only dependency; the client never runs this

    k = PLACEMENT_SUBSAMPLE
    sl = interior
    z_int = np.asarray(z_pad[sl, sl], np.float32)
    edge = z_int.shape[0]
    assert z_int.shape == (edge, edge) and edge % k == 0
    sub = edge // k

    out: dict[str, np.ndarray] = {}

    # --- dist_water -------------------------------------------------------
    wet_pad = np.asarray(z_pad, np.float32) <= np.float32(0.0)  # the sea
    if river_wet_pad is not None:
        wet_pad |= river_wet_pad
    if lake_wet_interior is not None:
        wet_pad[sl, sl] |= np.asarray(lake_wet_interior, bool)
    if wet_pad.any():
        dist_px = ndimage.distance_transform_edt(~wet_pad)
        d_m = np.asarray(dist_px[sl, sl], np.float32) * np.float32(cell_m)
        del dist_px
        d_min = _pool(d_m, k, "min")
        del d_m
        q = np.floor(d_min / PLACEMENT_DIST_LSB_M)
        out["dist_water"] = np.minimum(q, PLACEMENT_DIST_UNKNOWN).astype(np.uint8)
        del d_min
    else:
        # No water anywhere in the padded domain: every cell is "no water
        # knowable", which the consumer treats as the fail-closed sentinel.
        # Uniform, so it costs zero data bytes.
        out["dist_water"] = np.full((sub, sub), PLACEMENT_DIST_UNKNOWN, np.uint8)
    del wet_pad

    # --- twi --------------------------------------------------------------
    # ln(a / tan beta): a = accumulation per unit contour width (m^2/m), at
    # FULL resolution -- the hollows TWI exists to find are one or two fine
    # cells wide -- then MEAN-pooled, which is the right pooling for a value
    # already in log space (the gate is a smooth weight, not a threshold).
    gy, gx = np.gradient(z_int, cell_m)
    tan_int = np.hypot(gx, gy)
    del gx, gy
    a_w = np.asarray(acc_pad[sl, sl], np.float32) / np.float32(cell_m)
    twi = np.log(np.maximum(a_w, np.float32(cell_m))
                 / np.maximum(tan_int, np.float32(TWI_TAN_FLOOR)))
    del a_w
    q = np.rint((_pool(twi, k, "mean") + PLACEMENT_TWI_OFFSET)
                * PLACEMENT_TWI_SCALE)
    out["twi"] = np.clip(q, 0, 254).astype(np.uint8)
    del twi

    # --- the three slope-family planes, on the subsampled grid ------------
    z_sub = _pool(z_int, k, "mean").astype(np.float32)
    cell_sub = cell_m * k
    gy, gx = np.gradient(z_sub, cell_sub)
    tan_sub = np.hypot(gx, gy).astype(np.float32)

    flux = talus_flux(z_sub, tan_sub, cell_sub)
    q = np.rint(PLACEMENT_TALUS_SCALE * np.log1p(flux))
    out["talus"] = np.clip(q, 0, 254).astype(np.uint8)
    del flux

    # Total curvature: the 4-neighbour Laplacian over the subsampled surface,
    # 1/m. Positive = concave (hollow, foot-slope: deep soil, big trees);
    # negative = convex (ridge nose: thin soil, rocks). Zevenbergen-Thorne's
    # plan/profile split is overkill for a weight multiplier.
    lap = ndimage.laplace(z_sub.astype(np.float64)) / (cell_sub * cell_sub)
    q = 128.0 + np.rint(lap * PLACEMENT_CURV_SCALE)
    out["curv"] = np.clip(q, 0, 255).astype(np.uint8)
    del lap

    # Heat load: aspect folded about SW (225 deg from +y = north), weighted by
    # steepness s/(s+1) so flat ground reads neutral. McCune-Keon in spirit;
    # the exact published equation needs latitude, which this world does not
    # model.
    aspect = np.arctan2(-gx, -gy)  # facing direction, 0 = north, +pi/2 = east
    s = tan_sub / (tan_sub + np.float32(1.0))
    # PLUS the cosine: at aspect == SW the cosine is 1 and the face is
    # hottest. (The first draft subtracted it and shipped an inverted
    # hemisphere; the SW/NE test below is the pin.)
    heat = 0.5 + 0.5 * np.cos(aspect - np.float32(np.pi * 1.25)) * s
    q = np.rint(np.clip(heat, 0.0, 1.0) * PLACEMENT_HEAT_SCALE)
    out["heat"] = q.astype(np.uint8)
    del gx, gy, aspect, s, heat, z_sub, tan_sub

    for name, p in out.items():
        assert p.shape == (sub, sub), (name, p.shape)
    return out
