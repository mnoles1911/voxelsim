"""B0 carrier and B1 roughness for the Phase 2 geomorphic bake.

Two passes live here, and they are deliberately asymmetric in ambition:

**B0 `carrier`** is *contractual*. `docs/vxtl-v2-format.md` §2 says the fine plane is a
**control lattice, not samples**, and §8 fixes the exact cubic B-spline the client will
evaluate on it. This module produces that lattice, so it must (a) prefilter, and (b) use
§8's weights verbatim. Getting either wrong is not a quality regression, it is a
disagreement with the shipped decoder.

**B1 `roughness`** is *substrate*, not texture. It exists so the erosion passes have
something to act on — incision carved into a perfectly smooth ramp reads as lines scribed
on plastic. It is explicitly NOT trying to hit a self-affine spectrum; see the amplitude
note on `roughness` for why chasing the spectrum here made the terrain measurably worse.

Dependencies: numpy only at import time. scipy is imported lazily inside `carrier`
(the prefilter is a scipy IIR pass) so that this module imports cleanly in CI, which has
neither scipy nor numba. `roughness` needs neither.
"""

from __future__ import annotations

from typing import Tuple

import numpy as np

__all__ = [
    "carrier",
    "prefilter",
    "roughness",
    "repose_field",
    "repose_erodibility",
    "meso_relief",
    "octave_wavelengths",
    "bspline_weights",
    "SPLINE_DEN",
]

# --------------------------------------------------------------------------------------
# §8 of docs/vxtl-v2-format.md — normative, do not "simplify"
# --------------------------------------------------------------------------------------

_Q = 1024                      # cell fraction is quantised to q10, per §8
SPLINE_DEN = 6 * _Q ** 3       # the weights are integer numerators over this


def bspline_weights(scale: int) -> np.ndarray:
    """The `(scale, 4)` integer weight numerators from `vxtl-v2-format.md` §8.

    Returned as exact `int64` numerators over `SPLINE_DEN`, because that is the form the
    C++/HLSL decoder uses and the form in which the invariant `sum == 6*1024**3` is
    checkable rather than approximately checkable.

    For a fine sample `k` inside a coarse cell of pitch `pxMm`, §8 sets
    `tq = truncDiv(fx * 1024, pxMm)` with `fx = k * pxMm / scale`, which is exactly
    `tq = (k * 1024) // scale` — no dependence on the physical pitch, which is why this
    table is shared by the 3.75 m and 1.875 m tiers.

    The stencil is control points `px-1 .. px+2` (`vxc::kCarrierStencilLo/Hi`), i.e. the
    four columns here are the weights of `cp[px-1], cp[px], cp[px+1], cp[px+2]`.
    """
    if int(scale) != scale or scale < 1:
        raise ValueError(f"scale must be a positive integer, got {scale!r}")
    scale = int(scale)
    t = np.int64(_Q)
    tq = (np.arange(scale, dtype=np.int64) * t) // np.int64(scale)
    w = np.stack(
        [
            (t - tq) ** 3,
            3 * tq ** 3 - 6 * tq ** 2 * t + 4 * t ** 3,
            -3 * tq ** 3 + 3 * tq ** 2 * t + 3 * tq * t ** 2 + t ** 3,
            tq ** 3,
        ],
        axis=1,
    )
    return w


def _weights_f64(scale: int) -> np.ndarray:
    return bspline_weights(scale).astype(np.float64) / float(SPLINE_DEN)


# --------------------------------------------------------------------------------------
# B0 — the carrier
# --------------------------------------------------------------------------------------

_CHUNK_ROWS = 64      # keeps the float64 accumulator bounded regardless of tile size


def _prefilter(a: np.ndarray) -> np.ndarray:
    """Solve for the control points whose cubic B-spline *interpolates* `a`.

    THE PREFILTER IS NOT OPTIONAL. A B-spline approximates its control points, so feeding
    it raw samples low-passes the source. Measured on a real tile: detrended H degrades
    from 0.83 (120-240 m, real 30 m data) to 1.47 by 30-60 m without it — the carrier
    comes out *smoother than the raster it was built from*, right in the band the bake
    exists to extend. `docs/vxtl-v2-format.md` §2 records the same number, and it is the
    reason the wire format ships control points rather than samples: this is a float IIR
    pass (pole sqrt(3)-2) and cannot live in the integer-only client.
    """
    try:
        from scipy.ndimage import spline_filter
    except ImportError as exc:  # pragma: no cover - exercised only on a scipy-less box
        raise ImportError(
            "terrain_service.bake.noise.carrier needs scipy.ndimage.spline_filter for the "
            "B-spline prefilter. It is a bake-pod dependency and is deliberately not in "
            "terrain-service/requirements.txt; use the terrain-diffusion venv."
        ) from exc
    # mode="nearest" to match the edge replication used by the evaluation stencil below.
    return spline_filter(a, order=3, mode="nearest", output=np.float64)


def _upsample_axis1(src: np.ndarray, scale: int, w: np.ndarray, out: np.ndarray,
                    transposed: bool) -> None:
    """Upsample `src` (rows, n) along axis 1 by `scale`, writing into `out`.

    `transposed=True` writes `out[:, r0:r1] = block.T`, which lets the second separable
    pass land straight into a correctly oriented float32 result and skips a full-size
    transpose copy (at 8192^2 that copy is 268 MB of pure waste).

    Separable evaluation with an intermediate rounding is what §8 mandates for the integer
    decoder ("the exact tensor form overflows int64 by ~10 orders of magnitude"); doing the
    same thing in float here keeps the two halves structurally identical.
    """
    rows, n = src.shape
    pad = np.pad(src, ((0, 0), (1, 2)), mode="edge")
    for r0 in range(0, rows, _CHUNK_ROWS):
        r1 = min(r0 + _CHUNK_ROWS, rows)
        blk = np.zeros((r1 - r0, n, scale), dtype=np.float64)
        for k in range(4):
            blk += pad[r0:r1, k:k + n][:, :, None] * w[None, None, :, k]
        v = blk.reshape(r1 - r0, n * scale)
        if transposed:
            out[:, r0:r1] = v.T
        else:
            out[r0:r1] = v


def carrier(coarse: np.ndarray, scale: int) -> np.ndarray:
    """Prefiltered cubic B-spline upsample of `coarse` by `scale` (B0).

    Returns float32 of shape `(H*scale, W*scale)`. The output is *sample* elevations, i.e.
    what the client's spline evaluation yields; the control lattice that gets encoded is
    the prefiltered array, not this. (The bake's later passes act on samples, then the
    encoder re-prefilters the finished surface.)

    Phase convention: fine index `p*scale` coincides with coarse index `p` (tq == 0 there),
    so `carrier(a, s)[::s, ::s] == a` to the prefilter's own tolerance. Anything else would
    put a half-cell shift between the bake and the client.

    Arithmetic is float64 internally and float32 on output. That is not fussiness: at
    tq == 0 the weights are (1, 4, 1, 0)/6 and the prefilter's job is precisely to make
    that combination reproduce the sample, so accumulating in float32 would spend a
    meaningful fraction of the prefilter's accuracy before the erosion passes even start.
    """
    a = np.asarray(coarse)
    if a.ndim != 2:
        raise ValueError(f"carrier expects a 2-D array, got shape {a.shape}")
    if int(scale) != scale or scale < 1:
        raise ValueError(f"scale must be a positive integer, got {scale!r}")
    scale = int(scale)

    cp = _prefilter(a.astype(np.float64, copy=False))
    w = _weights_f64(scale)
    h, n = cp.shape

    tmp = np.empty((h, n * scale), dtype=np.float64)
    _upsample_axis1(cp, scale, w, tmp, transposed=False)

    out = np.empty((h * scale, n * scale), dtype=np.float32)
    _upsample_axis1(np.ascontiguousarray(tmp.T), scale, w, out, transposed=True)
    return out


def prefilter(samples: np.ndarray) -> np.ndarray:
    """Control points whose cubic B-spline INTERPOLATES `samples`. float64.

    The public name for the IIR pass `carrier` uses internally, exported so the
    .vxtl v2 encoder (`tile_codec.encode_fine`) re-prefilters the finished bake
    surface with *this* implementation rather than a second copy of it. The wire
    format ships a control lattice (docs/vxtl-v2-format.md §2); the bake's B2/B3
    passes operate on samples; so exactly one prefilter has to run at each end and
    both must be the same operator or the client's spline reproduces something
    other than what the bake computed.

    Needs scipy (`carrier` documents why it is a lazy import).
    """
    a = np.asarray(samples)
    if a.ndim != 2:
        raise ValueError(f"prefilter expects a 2-D array, got shape {a.shape}")
    return _prefilter(a.astype(np.float64, copy=False))


# --------------------------------------------------------------------------------------
# B1 — conditioned roughness
# --------------------------------------------------------------------------------------

# Amplitude of the octave sitting at the source Nyquist, in metres RMS, before the slope
# gain. KEEP THIS SMALL AND RESIST THE URGE TO TUNE IT UPWARDS.
#
# The prototype was once tuned to make the fine-end spectrum textbook-correct
# (H 1.65 -> 0.91) and the terrain got *worse*: the hillshade read as uniform crumpled
# paper and the dendritic flow network collapsed into a confetti of disconnected
# micro-catchments, because every noise dimple became its own sink. That took ~3.19 m of
# 30 m-wavelength noise. This is an order of magnitude below it — enough to give incision
# something to organise around, not enough to out-vote the landform.
#
# If the baked surface still looks too smooth at the fine end, the fix is more
# geomorphology (finer rills, more incision detail), not more amplitude here.
_REF_AMPLITUDE_M = 0.35

# Roll-off across octaves. 0.85 is a physical self-affine exponent, so the *shape* of the
# roll-off is right even though the overall level is deliberately below the spectral fit.
_ROUGHNESS_H = 0.85

# Coarsest octave is <= src_nyquist_m; finest has this many cells per wavelength. Four is
# the practical floor for a cubic reconstruction — below it the B-spline cannot render the
# octave and you get aliasing dressed up as detail. At 1.875 m/px this bottoms out at
# 7.5 m, which is exactly where the plan's band table hands over to the client amplifier.
_MIN_CELLS_PER_OCTAVE = 4

# Slope gain: steeper ground is rougher. Clipped so flat ground (where a dimple most
# easily becomes a sink) gets a quarter amplitude and cliffs get at most double.
_SLOPE_REF = 0.3
_SLOPE_GAIN_LO = 0.25
_SLOPE_GAIN_HI = 2.0

_M0 = np.uint64(0x9E3779B97F4A7C15)
_M1 = np.uint64(0xBF58476D1CE4E5B9)
_M2 = np.uint64(0x94D049BB133111EB)
_S30 = np.uint64(30)
_S27 = np.uint64(27)
_S31 = np.uint64(31)
_S11 = np.uint64(11)
_TWO_POW_M53 = 2.0 ** -53


def _splitmix64(x: np.ndarray) -> np.ndarray:
    # Wraparound IS the algorithm here, so the overflow warning is noise, not a diagnostic.
    with np.errstate(over="ignore"):
        z = (x + _M0).astype(np.uint64)
        z = ((z ^ (z >> _S30)) * _M1).astype(np.uint64)
        z = ((z ^ (z >> _S27)) * _M2).astype(np.uint64)
        return (z ^ (z >> _S31)).astype(np.uint64)


def _hash_lattice(seed: int, octave: int, ii: np.ndarray, jj: np.ndarray) -> np.ndarray:
    """iid N(0,1) lattice values keyed by *integer lattice coordinates*.

    A counter hash rather than an RNG stream, and that is the whole point: the plan
    requires B1 to be "hashed in world coordinates so it is bake-batch invariant", and the
    apron argument (`docs/terrain-amplification-plan.md`, seam section) only works if two
    overlapping bake domains agree exactly where they overlap. A sequential
    `default_rng(seed)` walk cannot do that — its values depend on the domain's origin and
    extent, so the prototype's noise was array-anchored and its seam test had to inject a
    shared pre-computed field to work around it.
    """
    with np.errstate(over="ignore"):
        base = _splitmix64(np.uint64(seed & 0xFFFFFFFFFFFFFFFF)
                           ^ (np.uint64(octave & 0xFFFF) * _M2))
        hi = _splitmix64(base ^ (ii.astype(np.int64).view(np.uint64) * _M0))
        h1 = _splitmix64(hi ^ (jj.astype(np.int64).view(np.uint64) * _M1))
        h2 = _splitmix64(h1 ^ _M2)
    u1 = np.maximum((h1 >> _S11).astype(np.float64) * _TWO_POW_M53, _TWO_POW_M53)
    u2 = (h2 >> _S11).astype(np.float64) * _TWO_POW_M53
    return np.sqrt(-2.0 * np.log(u1)) * np.cos(2.0 * np.pi * u2)


def _lattice_upsample(g: np.ndarray, p: int, w: np.ndarray) -> np.ndarray:
    """Cubic B-spline upsample of a lattice by the integer factor `p`, both axes.

    THIS IS THE PASS THAT MUST NOT BE `np.kron`. The first prototype box-upsampled every
    octave — nearest neighbour — and the hillshade came out as a lattice of hard rectangles
    at every octave scale: precisely the grid artifact this whole project exists to remove,
    reintroduced by the pass whose job is natural roughness. It is invisible in any
    statistic that averages over the field and obvious the instant the field is shaded, so
    `tests/test_bake_geomorph.py` checks it structurally (phase-binned second differences)
    rather than statistically.

    `g` covers lattice indices `c-1 .. c+2` for output cells `c` in `0 .. g.shape-4`, so the
    result is `((g.shape[0]-3)*p, (g.shape[1]-3)*p)`.
    """
    # float32 throughout: this is noise at ~0.3 m amplitude, and at the fine tier's real
    # 8192^2 the float64 version of these two accumulators alone is ~1 GB. Cross-domain
    # bit-exactness (the seam guarantee) is unaffected -- each output value depends only on
    # its own 4x4 stencil, summed in a fixed order, so a window and its parent agree
    # exactly in float32 just as they do in float64.
    g = g.astype(np.float32, copy=False)
    w = w.astype(np.float32)
    cells0 = g.shape[0] - 3
    cells1 = g.shape[1] - 3
    rows = np.zeros((cells0, p, g.shape[1]), dtype=np.float32)
    for k in range(4):
        rows += g[k:k + cells0][:, None, :] * w[None, :, None, k]
    rows = rows.reshape(cells0 * p, g.shape[1])
    out = np.zeros((rows.shape[0], cells1, p), dtype=np.float32)
    for k in range(4):
        out += rows[:, k:k + cells1][:, :, None] * w[None, None, :, k]
    return out.reshape(rows.shape[0], cells1 * p)


def _octave_norm(w: np.ndarray) -> float:
    """Phase-averaged std of the B-spline of iid unit-variance control points.

    Computed analytically instead of measured, because measuring `band.std()` over the
    domain — what the prototype did — makes the amplitude depend on the domain, which
    silently breaks the seam guarantee that `_hash_lattice` exists to provide.
    """
    q = float(np.mean(np.sum(w * w, axis=1)))
    return q  # 2-D variance is q*q for a separable evaluation, so std == q


def octave_wavelengths(cell_m: float, src_nyquist_m: float = 30.0):
    """The octave wavelengths `roughness` will synthesise, coarsest first, in metres.

    Two rules, both structural rather than tunable:

    * nothing above `src_nyquist_m` — the carrier holds real diffusion-model data there;
    * every wavelength is a power-of-two multiple of `cell_m`, which is what makes the
      lattice pitch an exact integer number of cells and therefore makes world anchoring
      (see `_hash_lattice`) an integer index plus a phase crop rather than a resampling.
    """
    cell_m = float(cell_m)
    if cell_m <= 0.0:
        raise ValueError(f"cell_m must be positive, got {cell_m}")
    p = 1
    while p * 2 * cell_m <= float(src_nyquist_m):
        p *= 2
    out = []
    while p >= _MIN_CELLS_PER_OCTAVE:
        out.append(p * cell_m)
        p //= 2
    return out


#: Folded-normal constants for the constructional term: for X ~ N(0,1),
#: E|X| = sqrt(2/pi) and std|X| = sqrt(1 - 2/pi). Folding a unit-RMS smooth
#: field as (E|X| - |X|) / std|X| yields a unit-RMS field whose SHARP features
#: are crests (the zero-contours of X, a connected curvilinear network) and
#: whose lows are smooth — the convex/concave asymmetry isotropic fBm cannot
#: have, in the crest-up direction.
_ABS_MEAN = float(np.sqrt(2.0 / np.pi))
_ABS_STD = float(np.sqrt(1.0 - 2.0 / np.pi))

#: Octave-key offset for the constructional term's lattices, so its values are
#: independent of the substrate octaves at the same wavelengths (the key is
#: hashed as ``octave & 0xFFFF``; substrate octaves count from 0 and there are
#: never more than a handful).
_CONSTRUCTIONAL_OCTAVE_KEY = 100


def roughness(carrier_z: np.ndarray, cell_m: float, slope: np.ndarray, seed: int,
              src_nyquist_m: float = 30.0,
              origin_cells: Tuple[int, int] = (0, 0),
              constructional_amp: float = 0.0,
              constructional_slope_lo: float = 0.10,
              constructional_slope_hi: float = 0.30) -> np.ndarray:
    """B1: slope-conditioned fBm, in metres, to be ADDED to the carrier.

    Only octaves at or below `src_nyquist_m` are synthesised. Above it the carrier already
    holds real diffusion-model data, and adding noise there fights the model rather than
    extending it — the same "replace, do not layer" rule the plan applies to the client's
    landform octaves.

    Amplitude is deliberately modest; see `_REF_AMPLITUDE_M`. This is substrate for
    erosion, not final texture.

    `origin_cells` is the domain's `(row, col)` offset in fine cells within the world
    lattice. It is optional and defaults to `(0, 0)`, so callers written against the bare
    four-argument form are unaffected. Passing the true offset makes the noise
    world-anchored, which is what the plan's apron/seam argument requires: two overlapping
    bake domains then agree *exactly* in their overlap instead of approximately.

    `carrier_z` is used for its shape and dtype only. It is in the signature because B1 is
    specified as conditioned on the carrier, and curvature conditioning (not implemented
    here) would need it.

    **The constructional term** (``constructional_amp > 0``; 0 reproduces the
    prior surface bit-for-bit). Gentle real landscapes owe their fine-scale
    ridge-and-knoll relief to CONSTRUCTIONAL processes — glacial till knolls,
    hummocky moraine, playa/dune surfaces — not to erosion, and such relief is
    genuinely uncorrelated with the modern drainage network. Isotropic fBm
    cannot supply it: it is symmetric by construction, so it has no crests.
    This term adds crest-up folded noise (see ``_ABS_MEAN``), at
    ``constructional_amp`` times the reference amplitude per octave, gated to
    ZERO on steep ground by the regional slope: full strength at or below
    ``constructional_slope_lo``, fading linearly to nothing at
    ``constructional_slope_hi``. Steep ground is erosional — its ridges must be
    left by incision (interfluves), not painted — and folded noise on a
    mountainside was measured (2026-07-29, ridge-deficit investigation) to read
    as exactly the "uniform crumpled paper" failure the amplitude note above
    records, with ridge cells uncorrelated with the flow field (placement
    ratio 1.01–1.07 against 1.7–3.0 for erosional ridges).
    """
    z = np.asarray(carrier_z)
    if z.ndim != 2:
        raise ValueError(f"roughness expects a 2-D carrier, got shape {z.shape}")
    cell_m = float(cell_m)
    if cell_m <= 0.0:
        raise ValueError(f"cell_m must be positive, got {cell_m}")
    if constructional_amp < 0.0:
        raise ValueError(
            f"constructional_amp must be >= 0, got {constructional_amp}")
    if not 0.0 <= constructional_slope_lo < constructional_slope_hi:
        raise ValueError(
            f"need 0 <= constructional_slope_lo < constructional_slope_hi, got "
            f"{constructional_slope_lo}, {constructional_slope_hi}")
    src_nyquist_m = float(src_nyquist_m)
    oy, ox = int(origin_cells[0]), int(origin_cells[1])
    n0, n1 = z.shape

    out = np.zeros((n0, n1), dtype=np.float32)

    for octave, wavelength in enumerate(octave_wavelengths(cell_m, src_nyquist_m)):
        p = int(round(wavelength / cell_m))
        amp = np.float32(_REF_AMPLITUDE_M * (wavelength / src_nyquist_m) ** _ROUGHNESS_H)
        out += amp * _octave_field((n0, n1), p, seed, octave, oy, ox)

    s = np.asarray(slope, dtype=np.float32)
    gain = np.clip(s / np.float32(_SLOPE_REF),
                   np.float32(_SLOPE_GAIN_LO), np.float32(_SLOPE_GAIN_HI))
    out *= gain

    if constructional_amp > 0.0:
        c = np.zeros((n0, n1), dtype=np.float32)
        for octave, wavelength in enumerate(octave_wavelengths(cell_m, src_nyquist_m)):
            p = int(round(wavelength / cell_m))
            amp = np.float32(constructional_amp * _REF_AMPLITUDE_M
                             * (wavelength / src_nyquist_m) ** _ROUGHNESS_H)
            g = _octave_field((n0, n1), p, seed,
                              _CONSTRUCTIONAL_OCTAVE_KEY + octave, oy, ox)
            c += amp * ((np.float32(_ABS_MEAN) - np.abs(g)) / np.float32(_ABS_STD))
        cgain = np.clip(
            (np.float32(constructional_slope_hi) - s)
            / np.float32(constructional_slope_hi - constructional_slope_lo),
            np.float32(0.0), np.float32(1.0))
        out += c * cgain

    return out


# --------------------------------------------------------------------------------------
# Material strength -> per-cell angle of repose (B3's threshold field)
# --------------------------------------------------------------------------------------

#: Octave-key offsets for the repose field's lattices, disjoint from the
#: substrate octaves (0..) and the constructional term (100..) — the key is
#: hashed as ``octave & 0xFFFF`` and there are never more than a handful of
#: octaves in any family.
_REPOSE_SPATIAL_OCTAVE_KEY = 200
_REPOSE_STRATA_OCTAVE_KEY = 300
#: The strata-fold field's key (see ``strata_fold_amp_m`` below).
_REPOSE_FOLD_OCTAVE_KEY = 320

#: Two octaves per family, weights (1, 0.5); this normalises their sum back to
#: unit RMS.
_TWO_OCTAVE_NORM = float(np.sqrt(1.0 + 0.25))


def _strata_1d(z_m: np.ndarray, seed: int, octave: int, wavelength_m: float) -> np.ndarray:
    """Smooth unit-variance-ish 1-D value noise over ELEVATION, elementwise.

    Keyed on physical elevation only, so it is world-anchored by construction:
    two overlapping bake domains agree bit-for-bit wherever their surfaces
    agree. Smoothstep-faded linear interpolation between iid normal lattice
    values at integer multiples of ``wavelength_m`` — C1 in z, which is all a
    threshold field needs.
    """
    inv = 1.0 / float(wavelength_m)
    zf = z_m.astype(np.float64, copy=False) * inv
    k = np.floor(zf)
    f = (zf - k).astype(np.float64)
    ki = k.astype(np.int64)
    del k, zf

    # EVALUATE THE LATTICE ONCE PER DISTINCT INDEX, NOT ONCE PER CELL.
    #
    # The second argument to _hash_lattice is `np.zeros_like(ki)` -- there is no
    # horizontal lattice here, this is 1-D noise over ELEVATION. So the value is
    # a pure function of the scalar `ki`, and `ki` takes very few distinct values:
    # it is elevation divided by the wavelength, so a 3 km range at the 30 m
    # wavelength spans ~101 indices and at 7.5 m spans ~401. Evaluating splitmix64
    # three times plus a Box-Muller sqrt/log/cos for all 85 M cells was computing
    # a ~100-entry lookup table 85 million times.
    #
    # Measured at 9216^2: 22.1 s -> 2.70 s for this function, 8.2x, and it made
    # repose_field (which calls this twice, and is itself called twice per bake --
    # once in B2d for erodibility, once in B3) go from 50.7 CPU-s to ~11.
    #
    # BIT-IDENTICAL, and that is why this needs no BAKE_VERSION bump: the table
    # entries are the same _hash_lattice calls with the same arguments, merely
    # deduplicated, and the gather reproduces them exactly. Verified with
    # np.array_equal against the direct form at both wavelengths.
    lo = int(ki.min())
    hi = int(ki.max())
    span = hi - lo + 2  # +1 for the ki+1 lookup, +1 because both ends are inclusive
    if span <= 0 or span > ki.size:
        # PATHOLOGICAL RANGE FALLBACK. The table only wins when distinct indices
        # are far fewer than cells; a degenerate wavelength or an absurd
        # elevation range could invert that and make the table the larger
        # allocation. Falling back keeps this a pure optimisation with no input
        # for which it is worse, rather than a fast path with a cliff.
        zero = np.zeros_like(ki)
        g0 = _hash_lattice(seed, octave, ki, zero)
        g1 = _hash_lattice(seed, octave, ki + 1, zero)
        del zero
    else:
        idx = np.arange(lo, lo + span, dtype=np.int64)
        table = _hash_lattice(seed, octave, idx, np.zeros_like(idx))
        off = ki - lo
        g0 = table[off]
        g1 = table[off + 1]
        del idx, table, off
    del ki
    s = f * f * (3.0 - 2.0 * f)
    return g0 * (1.0 - s) + g1 * s


def repose_field(z_m: np.ndarray, cell_m: float, seed: int,
                 origin_cells: Tuple[int, int], *,
                 base_deg: float = 36.0,
                 spatial_amp_deg: float = 6.0,
                 spatial_wavelength_m: float = 160.0,
                 strata_amp_deg: float = 14.0,
                 strata_wavelength_m: float = 30.0,
                 strata_fold_amp_m: float = 0.0,
                 strata_fold_wavelength_m: float = 300.0,
                 min_deg: float = 26.0,
                 max_deg: float = 60.0) -> np.ndarray:
    """Per-cell angle of repose, in degrees float32 — ``thermal.relax``'s field form.

    WHY THIS EXISTS. B3 with one global angle planes every face it touches to
    the SAME slope: measured on the g35 exemplar, incision hands B3 a broad,
    heavy-tailed slope distribution (p90 grade 1.25) and B3 returns one with a
    third of the mountainside within +-10% of tan(36 deg). A constant-slope
    face voxelises to evenly spaced parallel contour terraces — the corduroy
    artifact — and no client-side term may roughen it (the drainage cap forbids
    exactly the gradients that would). So the variation has to live in the
    THRESHOLD: rock strength is not one number in nature and stops being one
    here.

    Two components, both world-anchored, both unit-RMS before their amplitude:

    * **spatial** — smooth 2-D octaves at ``spatial_wavelength_m`` and a
      quarter of it, on the same integer world lattice as ``roughness`` (so
      apron overlaps agree exactly). Reads as lithology patches: one nose of a
      ridge ravels at 30 deg while its neighbour holds 45.
    * **strata** — 1-D value noise over ELEVATION at ``strata_wavelength_m``
      and a quarter of it. Reads as sub-horizontal bedding: bands of strong
      rock that hold near-cliff faces (thermal leaves everything the incision
      carved into them) alternating with weak bands that ravel back to talus —
      bench-and-cliff structure, which is precisely the metre-scale slope
      CHANGE a smooth ramp lacks. Keyed on the pre-relaxation surface, i.e.
      strata are glued to the rock, not to the finished skin.

    Both amplitudes at 0 reproduce a constant field (the scalar bake).

    **The strata FOLD** (``strata_fold_amp_m`` > 0; 0 reproduces the flat-lying
    form bit-for-bit): the strata are keyed on ``z + w(x, y)`` where ``w`` is a
    bounded, world-anchored 2-D undulation. Flat-lying strata are a banding
    machine one level up: an elevation-keyed band binds at the SAME elevation
    everywhere, so its bench traces are horizontal, contour-parallel, and
    quasi-evenly spaced at the strata wavelength -- the owner's artifact,
    manufactured by the very field that was added to break it (and the
    strength-modulated incision sharpens the benches further). Folding the
    datum by +-amp over a few hundred metres makes every bench trace wander
    ACROSS contours, exactly as folded sedimentary bedding does. It is a
    bounded VERTICAL warp, deliberately not a dip field: a dip term
    ``dip(x,y) * x`` carries the lever-arm defect documented in
    detail_rill.h (its phase derivative grows with |x| and the strata
    degenerate to noise tens of km out), while ``w`` and its derivative are
    bounded by construction at any distance from the origin.
    """
    if spatial_amp_deg < 0.0 or strata_amp_deg < 0.0:
        raise ValueError("repose amplitudes must be >= 0")
    if spatial_wavelength_m <= 0.0 or strata_wavelength_m <= 0.0:
        raise ValueError("repose wavelengths must be positive")
    if not (0.0 < min_deg <= base_deg <= max_deg < 85.0):
        raise ValueError(
            f"need 0 < min {min_deg} <= base {base_deg} <= max {max_deg} < 85"
        )
    n0, n1 = z_m.shape
    oy, ox = int(origin_cells[0]), int(origin_cells[1])
    out = np.full((n0, n1), float(base_deg), dtype=np.float64)

    if spatial_amp_deg > 0.0:
        # Chunked over rows exactly like the strata pass below, and for the
        # same reason: the unchunked form held two full-domain float64 fields
        # plus the upsample transients at once, which is most of why the first
        # v5b production bake peaked at 11 GiB against the 8 GiB pod. Chunking
        # is bit-identical because ``_octave_field`` is world-anchored (each
        # cell's value is its own 4x4 stencil at absolute lattice indices, so
        # the block origin only selects WHICH cells are evaluated, never what
        # any cell evaluates to).
        p = max(4, int(round(spatial_wavelength_m / float(cell_m))))
        p4 = max(4, p // 4)
        coef = float(spatial_amp_deg) / _TWO_OCTAVE_NORM
        for r0 in range(0, n0, 512):
            r1 = min(r0 + 512, n0)
            sp = _octave_field((r1 - r0, n1), p, seed,
                               _REPOSE_SPATIAL_OCTAVE_KEY, oy + r0, ox).astype(np.float64)
            sp += 0.5 * _octave_field((r1 - r0, n1), p4, seed,
                                      _REPOSE_SPATIAL_OCTAVE_KEY + 1, oy + r0, ox)
            out[r0:r1] += coef * sp
        del sp

    if strata_amp_deg > 0.0:
        if strata_fold_amp_m < 0.0 or strata_fold_wavelength_m <= 0.0:
            raise ValueError("strata fold amplitude must be >= 0 and wavelength > 0")
        # Chunked over rows, bit-identically (everything here is elementwise):
        # the unchunked form holds ~8 full-domain float64/int64 temporaries at
        # once, which measured 13.4 GiB peak working set on a production tile
        # against the 8 GiB bake-pod sizing. 512-row blocks keep the same math
        # inside a few hundred MB.
        coef = float(strata_amp_deg) / _TWO_OCTAVE_NORM
        pf = max(4, int(round(strata_fold_wavelength_m / float(cell_m))))
        for r0 in range(0, n0, 512):
            r1 = min(r0 + 512, n0)
            blk = z_m[r0:r1].astype(np.float64, copy=False)
            if strata_fold_amp_m > 0.0:
                # Two fold octaves, self-similar (A at lambda, A/3 at
                # lambda/3). One octave tilts every bench on a 300 m face
                # section TOGETHER -- locally the rows stay parallel, just
                # inclined, which the capture still read as a stack. The
                # shorter octave changes the local dip every ~100 m, so
                # adjacent bench traces pinch and swell within a single face.
                blk = blk + float(strata_fold_amp_m) * _octave_field(
                    (r1 - r0, n1), pf, seed, _REPOSE_FOLD_OCTAVE_KEY, oy + r0, ox)
                blk = blk + (float(strata_fold_amp_m) / 3.0) * _octave_field(
                    (r1 - r0, n1), max(4, pf // 3), seed,
                    _REPOSE_FOLD_OCTAVE_KEY + 1, oy + r0, ox)
            st = _strata_1d(blk, seed, _REPOSE_STRATA_OCTAVE_KEY, strata_wavelength_m)
            st += 0.5 * _strata_1d(blk, seed, _REPOSE_STRATA_OCTAVE_KEY + 1,
                                   strata_wavelength_m / 4.0)
            out[r0:r1] += coef * st

    np.clip(out, float(min_deg), float(max_deg), out=out)
    return out.astype(np.float32)


#: Octave-key offset for the post-thermal meso band, disjoint from substrate
#: (0..), constructional (100..), and the repose families (200.., 300..).
_MESO_OCTAVE_KEY = 400


def meso_relief(z_m: np.ndarray, cell_m: float, seed: int,
                origin_cells: Tuple[int, int], *,
                amp15_m: float = 0.8,
                amp11_m: float = 0.4,
                slope_lo: float = 0.20,
                slope_hi: float = 0.40,
                flow_slope: "np.ndarray | None" = None) -> np.ndarray:
    """B4: steep-gated meso relief (15 m / 11.25 m), in metres, POST-thermal.

    WHY THIS STAGE EXISTS, AND WHY IT RUNS AFTER B3. The residual contour
    banding lives on steep faces whose grade is near-constant over tens of
    metres; band spacing is 100 mm / grade, so what kills the rhythm is grade
    VARIATION at the 6-30 m wavelength. Both prior owners of that band are
    structurally unable to provide it there:

    * B1 substrate at these wavelengths is planed away by B3 -- on an
      at-threshold face, thermal converts any pre-existing meso bump back into
      the threshold field's own pattern, which is exactly the surface that
      banded;
    * the CLIENT's capped ladder cannot carry it safely: a band coherent over
      6-13 m makes pits the drainage lattice resolves, and a point-sampled
      gradient cap cannot prevent a 13 m feature from damming a neighbouring
      dip (measured 2026-07-30: a client 12.8/6.4 m band under a budgeted cap
      stranded 0.04-0.33% of the fine repro site, realization-dependent).

    Post-thermal, pre-refill is the one slot where the relief SURVIVES (thermal
    never sees it) and drainage is still GUARANTEED (the B4b epsilon refill
    runs on the summed surface, so every basin this band could create is
    resolved before the codec ever sees the ground).

    Slope-gated to steep ground: zero at or below ``slope_lo`` (a plain keeps
    its calibrated ridge/knoll statistics untouched), full at ``slope_hi``.
    World-anchored on the same integer lattice machinery as B1, so apron
    overlaps agree exactly. Both amplitudes at 0 reproduce the prior surface
    bit-for-bit.

    ``z_m`` is the post-thermal surface; the gate reads ITS local slope,
    computed here (chunked, with a one-row halo) so the gate and the field
    cannot be computed against different stages.

    ``flow_slope`` (optional, same shape): the DOWNSTREAM (D8) slope of the
    surface. When given, the gate takes ``min(local_slope, flow_slope)`` --
    and this distinction is a measured drainage requirement, not a nicety. The
    hypot slope on a gully BED between steep walls is steep (it reads the
    walls), so the bed passes the steep gate and the band perturbs the bed's
    own long profile; a bed descending at under ~100 mm/cell is then at the
    mercy of the codec's 100 mm quantization even when no pit exists at float
    precision (measured: 2 carrier sinks behind sub-quantization sills,
    stranding 5% of the repro window). The D8 slope reads the bed's own
    descent, so gentle-profile reaches shut the gate no matter how steep their
    walls -- "the channels stay open", which is also what real meso roughness
    does: talus and benches yield to the thalweg.

    WHY THE SECOND OCTAVE IS 11.25 m AND NOT 7.5. The first cut used 7.5 m --
    4 cells per wavelength, exactly ``_MIN_CELLS_PER_OCTAVE`` -- and the
    shipped SURFACE grew a 230 mm closed basin that exists only BETWEEN the
    native samples: the encoder's sharpening prefilter rings on near-Nyquist
    content, and a basin the native lattice cannot see is one the B4b refill
    cannot fill (it stranded 10.4% of the repro window through a sub-pixel
    sill in a trunk channel). At 6 cells per wavelength the spline renders the
    octave without inter-sample ringing at this amplitude, measured by a
    half-pixel pit census on the DECODED tile: 0 basins.
    """
    if amp15_m < 0.0 or amp11_m < 0.0:
        raise ValueError("meso amplitudes must be >= 0")
    if not 0.0 <= slope_lo < slope_hi:
        raise ValueError(f"need 0 <= slope_lo < slope_hi, got {slope_lo}, {slope_hi}")
    z = np.asarray(z_m)
    if z.ndim != 2:
        raise ValueError(f"meso_relief expects a 2-D surface, got shape {z.shape}")
    cell_m = float(cell_m)
    if cell_m <= 0.0:
        raise ValueError(f"cell_m must be positive, got {cell_m}")
    n0, n1 = z.shape
    oy, ox = int(origin_cells[0]), int(origin_cells[1])
    fs = None
    if flow_slope is not None:
        fs = np.asarray(flow_slope)
        if fs.shape != z.shape:
            raise ValueError(f"flow_slope {fs.shape} must match z {z.shape}")
        # ERODED, then smoothed, and both operations are drainage load-bearing:
        # a pointwise gate puts full-amplitude 11-15 m content ONE CELL from a
        # gentle bed, and the codec's reconstruction error is content-local --
        # a 30 mm/cell reach that survives quantization under smooth
        # surroundings dams when the band rings next to it (measured: the last
        # residual sink sat exactly so). The minimum filter keeps the band a
        # buffer away from every gentle cell; the uniform filter makes the
        # fade band-limited so the gate itself cannot re-introduce the
        # high-frequency content it exists to keep away. scipy is a bake-pod
        # dependency (see _prefilter); lazy import for the same CI reason.
        from scipy.ndimage import minimum_filter, uniform_filter

        fs = uniform_filter(
            minimum_filter(fs.astype(np.float32, copy=False), size=9,
                           mode="nearest"),
            size=5, mode="nearest")

    out = np.zeros((n0, n1), dtype=np.float32)
    if amp15_m == 0.0 and amp11_m == 0.0:
        return out

    p15 = max(6, int(round(15.0 / cell_m)))
    p11 = max(6, int(round(11.25 / cell_m)))
    inv_span = 1.0 / (float(slope_hi) - float(slope_lo))
    for r0 in range(0, n0, 512):
        r1 = min(r0 + 512, n0)
        blk = np.zeros((r1 - r0, n1), dtype=np.float64)
        if amp15_m > 0.0:
            blk += float(amp15_m) * _octave_field(
                (r1 - r0, n1), p15, seed, _MESO_OCTAVE_KEY, oy + r0, ox)
        if amp11_m > 0.0:
            blk += float(amp11_m) * _octave_field(
                (r1 - r0, n1), p11, seed, _MESO_OCTAVE_KEY + 1, oy + r0, ox)
        # Local slope of z over this block with a one-row halo, so the block
        # layout cannot change any cell's gate (np.gradient uses central
        # differences interior, one-sided at the array edge -- the halo keeps
        # every interior row central regardless of chunking).
        h0 = max(0, r0 - 1)
        h1 = min(n0, r1 + 1)
        gy, gx = np.gradient(z[h0:h1].astype(np.float64, copy=False), cell_m)
        s = np.hypot(gx, gy)[r0 - h0:(r0 - h0) + (r1 - r0)]
        if fs is not None:
            s = np.minimum(s, fs[r0:r1].astype(np.float64, copy=False))
        gate = np.clip((s - float(slope_lo)) * inv_span, 0.0, 1.0)
        out[r0:r1] = blk * gate
    return out


def repose_erodibility(repose_deg: np.ndarray, *, base_deg: float = 36.0,
                       max_deg: float = 72.0, ratio: float = 6.0) -> np.ndarray:
    """Map the repose (material-strength) field to a per-cell K multiplier.

    ``incise.profile_incision``'s ``erodibility`` argument (bake_ver 6): the
    SAME field that sets thermal's threshold also sets how fast fluvial
    incision cuts, because both are the same physical fact -- rock strength.
    Strong strata resisting the carve is what puts knickpoints and bench
    treads where streams cross them; with strength in thermal only, incision
    still carves every profile through hard and soft rock at one rate and the
    structure exists only where thermal binds.

    Log-linear in the field, pinned at 1.0 on baseline rock so the calibrated
    mean carve (K_dt, the concavity numbers, the class scorecard) is
    preserved:

        mult = ratio ** (-(repose_deg - base_deg) / (max_deg - base_deg))

    i.e. the strongest rock (``max_deg``) erodes ``1/ratio`` as fast as
    baseline and the weakest correspondingly faster (the sub-base range is
    narrower, so the boost is milder than the resistance -- deliberate:
    holding rock up is the visible mechanism, digging soft rock out faster is
    just its complement). ``ratio = 1`` disables (returns all ones).
    """
    if ratio <= 0.0:
        raise ValueError(f"ratio must be positive, got {ratio}")
    if not base_deg < max_deg:
        raise ValueError(f"need base_deg {base_deg} < max_deg {max_deg}")
    f = np.asarray(repose_deg)
    out = np.empty(f.shape, dtype=np.float32)
    # Chunked over the leading axis for the bake pod's memory budget; per-cell
    # float64 with one final cast, so bit-identical to the whole-array form.
    fr = f.reshape(-1, f.shape[-1]) if f.ndim > 1 else f.reshape(1, -1)
    outr = out.reshape(fr.shape)
    for r0 in range(0, fr.shape[0], 512):
        r1 = min(r0 + 512, fr.shape[0])
        outr[r0:r1] = np.power(
            float(ratio),
            -(fr[r0:r1].astype(np.float64, copy=False) - float(base_deg))
            / (float(max_deg) - float(base_deg)))
    return out


def _octave_field(shape: Tuple[int, int], p: int, seed: int, octave: int,
                  oy: int, ox: int) -> np.ndarray:
    """One unit-RMS octave with lattice pitch `p` cells, anchored at world `(oy, ox)`."""
    n0, n1 = shape
    w = _weights_f64(p)

    # Python's // floors, which is what world anchoring needs for negative world offsets.
    b0 = oy // p
    e0 = (oy + n0 - 1) // p
    b1 = ox // p
    e1 = (ox + n1 - 1) // p

    ii = np.arange(b0 - 1, e0 + 3, dtype=np.int64)[:, None]
    jj = np.arange(b1 - 1, e1 + 3, dtype=np.int64)[None, :]
    g = _hash_lattice(seed, octave, ii, jj)

    field = _lattice_upsample(g, p, w) / _octave_norm(w)
    y0 = oy - b0 * p
    x0 = ox - b1 * p
    return field[y0:y0 + n0, x0:x0 + n1]
