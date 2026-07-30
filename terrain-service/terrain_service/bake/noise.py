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
    zero = np.zeros_like(ki)
    g0 = _hash_lattice(seed, octave, ki, zero)
    g1 = _hash_lattice(seed, octave, ki + 1, zero)
    del ki, zero
    s = f * f * (3.0 - 2.0 * f)
    return g0 * (1.0 - s) + g1 * s


def repose_field(z_m: np.ndarray, cell_m: float, seed: int,
                 origin_cells: Tuple[int, int], *,
                 base_deg: float = 36.0,
                 spatial_amp_deg: float = 6.0,
                 spatial_wavelength_m: float = 160.0,
                 strata_amp_deg: float = 14.0,
                 strata_wavelength_m: float = 30.0,
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
        p = max(4, int(round(spatial_wavelength_m / float(cell_m))))
        sp = _octave_field((n0, n1), p, seed, _REPOSE_SPATIAL_OCTAVE_KEY, oy, ox).astype(np.float64)
        sp += 0.5 * _octave_field((n0, n1), max(4, p // 4), seed,
                                  _REPOSE_SPATIAL_OCTAVE_KEY + 1, oy, ox)
        out += (float(spatial_amp_deg) / _TWO_OCTAVE_NORM) * sp
        del sp

    if strata_amp_deg > 0.0:
        st = _strata_1d(z_m, seed, _REPOSE_STRATA_OCTAVE_KEY, strata_wavelength_m)
        st += 0.5 * _strata_1d(z_m, seed, _REPOSE_STRATA_OCTAVE_KEY + 1,
                               strata_wavelength_m / 4.0)
        out += (float(strata_amp_deg) / _TWO_OCTAVE_NORM) * st
        del st

    np.clip(out, float(min_deg), float(max_deg), out=out)
    return out.astype(np.float32)


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
