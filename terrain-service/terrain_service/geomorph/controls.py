"""Synthetic control surfaces: the negatives every metric here has to be tested against.

A metric that returns a number is worthless until it has been shown to *separate* the
cases we care about, and the cases we care about include several kinds of
convincing-looking fake. This module generates them, deterministically, so the
discrimination table has controls and the test suite has fixtures.

The controls, in ascending order of how hard they are to catch:

``inclined_plane`` / ``paraboloid`` / ``cone``
    Analytic surfaces with known slope and curvature. Not realism controls -- they are
    *correctness* controls, the fixtures that catch a sign error in the curvature
    convention or a factor of ``cell_m`` in the gradient.

``value_noise``
    Multi-octave lattice noise, the classic "procedural terrain" of a hundred game
    engines and the thing our own B1 roughness pass is made of. Caught by everything.

``fbm``
    Spectral-synthesis fractional Brownian motion with a chosen Hurst exponent. Has a
    correct-looking power spectrum by construction and a plausible relief. Caught by the
    drainage metrics and by curvature skew, not by the variogram (which is a function of
    the spectrum it was built to match) and not by hypsometry.

``spectrum_matched_surrogate``
    **The one that matters.** A Gaussian field resynthesised from a real DEM's own
    measured spectrum, so that its variogram tracks the original's within 15% at every
    lag -- same variance, same roughness at every scale -- while having no drainage
    network at all. This is the field that the project's earlier "the spectrum looks
    right" reasoning could not tell from the original, and it is the reason this package
    exists. Any metric that cannot separate a real DEM from its own surrogate is not
    doing the job asked of it here. Getting this control *right* took two attempts; the
    function's docstring records why the obvious construction is a strawman.

``shuffled``
    The degenerate limit: the same elevation histogram, all structure destroyed. Exists
    to demonstrate what hypsometry does and does not see (it does not see this at all).

Every generator takes an explicit ``seed`` and uses ``numpy.random.default_rng``; no
wall-clock, no global RNG state, so a table regenerated tomorrow is the same table.
"""

from __future__ import annotations

import numpy as np

from ._grid import as_field, check_cell_m

__all__ = [
    "fbm",
    "value_noise",
    "spectrum_matched_surrogate",
    "shuffled",
    "inclined_plane",
    "paraboloid",
    "cone",
    "radial_power_spectrum",
]


def _radial_freq(shape, cell_m: float):
    h, w = shape
    fy = np.fft.fftfreq(h, d=cell_m)[:, None]
    fx = np.fft.fftfreq(w, d=cell_m)[None, :]
    return np.hypot(fy, fx)


def fbm(shape, cell_m: float, *, hurst: float = 0.75, seed: int = 0,
        relief_m: float | None = None, rms_m: float | None = None) -> np.ndarray:
    """Fractional Brownian motion by spectral synthesis, float64, metres.

    Power spectral density ~ ``f^-(2H+2)`` in 2-D, i.e. a variogram slope of exactly
    ``2H``. ``hurst=0.75`` is squarely inside the 0.5-0.9 band that real topography
    reports over its fluvial scales, so this is a control that will pass a spectral
    check, which is the whole point of having it.

    Scaled to ``rms_m`` (standard deviation) if given, else to ``relief_m``
    (max - min), else left at unit RMS. Give one, not both.
    """
    h, w = int(shape[0]), int(shape[1])
    cell = check_cell_m(cell_m)
    if rms_m is not None and relief_m is not None:
        raise ValueError("give rms_m or relief_m, not both")
    if not 0.0 < float(hurst) < 1.0:
        raise ValueError(f"hurst must be in (0, 1), got {hurst!r}")

    rng = np.random.default_rng(int(seed))
    white = rng.standard_normal((h, w))
    f = _radial_freq((h, w), cell)
    f[0, 0] = 1.0
    amp = f ** (-(float(hurst) + 1.0))   # amplitude = sqrt(PSD), PSD ~ f^-(2H+2)
    amp[0, 0] = 0.0                      # no DC: the mean is not part of the model
    z = np.real(np.fft.ifft2(np.fft.fft2(white) * amp))
    return _rescale(z, rms_m, relief_m)


def _rescale(z, rms_m, relief_m):
    z = z - z.mean()
    s = z.std()
    if s > 0:
        z = z / s
    if rms_m is not None:
        return z * float(rms_m)
    if relief_m is not None:
        span = z.max() - z.min()
        return z * (float(relief_m) / span) if span > 0 else z
    return z


def value_noise(shape, cell_m: float, *, seed: int = 0, octaves: int = 8,
                base_wavelength_m: float | None = None, lacunarity: float = 2.0,
                gain: float = 0.5, rms_m: float | None = None,
                relief_m: float | None = None) -> np.ndarray:
    """Multi-octave smooth lattice ("value") noise -- procedural terrain's default.

    Each octave is iid Gaussian lattice values smoothed by a separable cubic B-spline
    upsample, which is the same reconstruction our own B1 roughness uses, so this
    control is a fair stand-in for "what the bake would look like if only B0 and B1
    ran and the erosion passes did not".
    """
    h, w = int(shape[0]), int(shape[1])
    cell = check_cell_m(cell_m)
    if base_wavelength_m is None:
        base_wavelength_m = max(h, w) * cell / 4.0
    rng = np.random.default_rng(int(seed))
    out = np.zeros((h, w), dtype=np.float64)
    amp = 1.0
    lam = float(base_wavelength_m)
    for _ in range(int(octaves)):
        p = max(2, int(round(lam / cell)))
        gh, gw = h // p + 4, w // p + 4
        g = rng.standard_normal((gh, gw))
        out += amp * _bspline_upsample(g, p)[:h, :w]
        amp *= float(gain)
        lam /= float(lacunarity)
        if lam < 2.0 * cell:
            break
    return _rescale(out, rms_m, relief_m)


def _bspline_upsample(g: np.ndarray, p: int) -> np.ndarray:
    """Separable cubic B-spline upsample of a lattice by the integer factor ``p``.

    Nearest-neighbour upsampling here would put a lattice of hard rectangles into every
    octave -- the exact grid artefact the bake's own `noise._lattice_upsample` docstring
    records having shipped once -- and a control with visible grid artefacts is a
    strawman rather than a control.
    """
    t = (np.arange(p) + 0.5) / p
    w = np.stack([
        ((1 - t) ** 3) / 6.0,
        (3 * t ** 3 - 6 * t ** 2 + 4) / 6.0,
        (-3 * t ** 3 + 3 * t ** 2 + 3 * t + 1) / 6.0,
        (t ** 3) / 6.0,
    ], axis=1)
    n0, n1 = g.shape[0] - 3, g.shape[1] - 3
    rows = np.zeros((n0, p, g.shape[1]))
    for k in range(4):
        rows += g[k:k + n0][:, None, :] * w[None, :, None, k]
    rows = rows.reshape(n0 * p, g.shape[1])
    out = np.zeros((rows.shape[0], n1, p))
    for k in range(4):
        out += rows[:, k:k + n1][:, :, None] * w[None, None, :, k]
    return out.reshape(rows.shape[0], n1 * p)


def spectrum_matched_surrogate(z, cell_m: float, *, seed: int = 0,
                               method: str = "radial") -> np.ndarray:
    """A Gaussian field with ``z``'s own roughness at every scale, and no landscape.

    **This is the control that matters.** It is built so that its variogram tracks the
    real field's, and that is measured rather than asserted -- ``tools/geomorph_validate``
    prints the ratio per scene and refuses to draw conclusions without it. On four of the
    five reference windows the surrogate's semivariance stays within 0.64-1.44 of the
    original's at *every* lag, with medians of 0.89-1.16. The fifth, Death Valley, comes
    out 1.22-1.79 too rough: that scene is half playa and half mountain range and one
    stationary Gaussian model cannot be both. Treat a surrogate of a strongly
    heterogeneous scene as the weaker control it is, and read the fidelity ratio before
    quoting a separation from it.

    What the surrogate does not have is any drainage network, any valley/ridge asymmetry
    or any threshold slopes. Any metric that cannot separate a real DEM from this is
    measuring the power spectrum and calling it realism, which is the specific mistake
    this whole package exists to stop repeating.

    Method ``"radial"`` (the default): remove a planar trend, apply a separable Hann
    taper, estimate the radially averaged power spectral density, then resynthesise by
    driving that spectrum with complex white noise. The taper and the detrend are the
    load-bearing parts, not hygiene:

        A DEM window is **not periodic**. Three kilometres of alpine relief across a
        15 km window is, to the DFT, a 3 km cliff at the wrap, and the leakage from it
        is reported as broadband high-frequency power. The original field does not
        contain that roughness -- it lives entirely in a wrap that does not exist -- but
        a surrogate built from the raw spectrum *does*, spread evenly over the whole
        field. Measured on the Alpine window, the untapered surrogate came out at a
        58 deg mean slope against the original's 27, with a one-cell semivariance 17x
        too large, **while its radially averaged PSD still matched to 15%**. It was not
        a control, it was a strawman, and every metric would have "caught" it for
        entirely the wrong reason.

    Method ``"phase"`` is that untapered version -- keep ``|FFT(z)|``, randomise the
    phases -- kept only so the test suite can assert it is the worse control.

    The surrogate is **isotropic** by construction, because the radial average throws
    the directional structure away. For a dune field, whose whole character is
    directional, that means the surrogate is not merely un-eroded but un-oriented;
    `variogram`'s ``anisotropy`` is the field that sees the difference.
    """
    zz = as_field(z)
    cell = check_cell_m(cell_m)
    if method not in ("radial", "phase"):
        raise ValueError(f"method must be 'radial' or 'phase', got {method!r}")
    rng = np.random.default_rng(int(seed))
    h, w = zz.shape

    if method == "phase":
        F = np.fft.rfft2(zz)
        phase = rng.uniform(-np.pi, np.pi, size=F.shape)
        out = np.fft.irfft2(np.abs(F) * np.exp(1j * phase), s=zz.shape)
        # The naive path has no absolute level of its own worth keeping, so it is
        # forced to the target variance. That is part of why it comes out too rough.
        out = out - out.mean()
        s = out.std()
        if s > 0:
            out = out * (zz.std() / s)
    else:
        f_c, psd = _tapered_radial_psd(zz, cell)
        f = _radial_freq((h, w), cell)
        fmin = f[f > 0].min()
        amp = np.sqrt(np.exp(np.interp(
            np.log(np.maximum(f, fmin)), np.log(f_c), np.log(psd))))
        amp[0, 0] = 0.0
        out = np.real(np.fft.ifft2(np.fft.fft2(rng.standard_normal((h, w))) * amp))
        out = out - out.mean()
        # DELIBERATELY NOT rescaled to the original's variance. The window-corrected
        # periodogram already carries the right absolute power, and the surrogate's
        # variance is legitimately a little below the original's because the planar
        # trend that was removed before the transform is not resynthesised. Forcing the
        # variances equal instead put the surrogate 1.9x too tall and therefore 3.6x
        # too rough at every lag -- which is a control that fails for the wrong reason,
        # exactly like the ``"phase"`` method it was written to replace.
    return np.ascontiguousarray(out + zz.mean())


def _detrend_plane(z: np.ndarray) -> np.ndarray:
    """Subtract the least-squares plane. The single largest source of DFT leakage."""
    h, w = z.shape
    yy, xx = np.mgrid[0:h, 0:w]
    A = np.stack([np.ones(z.size), yy.ravel().astype(float), xx.ravel().astype(float)], 1)
    coef, *_ = np.linalg.lstsq(A, z.ravel() - z.mean(), rcond=None)
    return z - z.mean() - (A @ coef).reshape(h, w)


def _tapered_radial_psd(z: np.ndarray, cell_m: float, n_bins: int = 60):
    """``(freq_centres, psd)`` from the detrended, Hann-tapered field.

    The taper costs power, so the periodogram is divided by the mean square of the
    window -- the standard normalisation, without which the resynthesised field would
    come out systematically smooth.
    """
    a = _detrend_plane(z)
    h, w = a.shape
    win = np.hanning(h)[:, None] * np.hanning(w)[None, :]
    u2 = float((win * win).mean())
    P = (np.abs(np.fft.fft2(a * win)) ** 2) / (a.size * u2)
    f = _radial_freq((h, w), cell_m).ravel()
    fmin = f[f > 0].min()
    edges = np.logspace(np.log10(fmin * 0.99), np.log10(f.max() * 1.01), n_bins + 1)
    idx = np.clip(np.digitize(f, edges) - 1, 0, n_bins - 1)
    s = np.bincount(idx, weights=P.ravel(), minlength=n_bins)
    c = np.bincount(idx, minlength=n_bins)
    prof = np.where(c > 0, s / np.maximum(c, 1), np.nan)
    ctr = np.sqrt(edges[:-1] * edges[1:])
    ok = np.isfinite(prof) & (prof > 0)
    if ok.sum() < 2:
        raise ValueError("the field has no usable spectrum to match")
    return ctr[ok], prof[ok]


def shuffled(z, cell_m: float, *, seed: int = 0) -> np.ndarray:
    """Same elevation histogram, every spatial relationship destroyed."""
    zz = as_field(z)
    check_cell_m(cell_m)
    rng = np.random.default_rng(int(seed))
    flat = zz.ravel().copy()
    rng.shuffle(flat)
    return flat.reshape(zz.shape)


def inclined_plane(shape, cell_m: float, *, slope: float = 0.2,
                   azimuth_deg: float = 0.0) -> np.ndarray:
    """Exact plane of the given gradient magnitude. Zero curvature everywhere."""
    h, w = int(shape[0]), int(shape[1])
    cell = check_cell_m(cell_m)
    y = np.arange(h)[:, None] * cell
    x = np.arange(w)[None, :] * cell
    a = np.radians(float(azimuth_deg))
    return float(slope) * (x * np.cos(a) + y * np.sin(a))


def paraboloid(shape, cell_m: float, *, k: float = 1e-4, sign: float = -1.0) -> np.ndarray:
    """``z = 0.5 * sign * k * r^2``. ``sign=-1`` is a dome (convex, positive curvature).

    Profile and planform curvature of a dome of this form are both ``sign * -k`` at the
    apex in the convention used by `curvature`; that is the analytic fixture the
    curvature sign convention is pinned to.
    """
    h, w = int(shape[0]), int(shape[1])
    cell = check_cell_m(cell_m)
    y = (np.arange(h)[:, None] - (h - 1) / 2.0) * cell
    x = (np.arange(w)[None, :] - (w - 1) / 2.0) * cell
    return 0.5 * float(sign) * float(k) * (x * x + y * y)


def cone(shape, cell_m: float, *, slope: float = 0.3, sign: float = 1.0) -> np.ndarray:
    """Constant-slope cone. ``sign=+1`` is a bowl (low centre), ``-1`` a hill.

    The bowl is the fixture for `drainage.pit_statistics`: it has **exactly one** raw
    pit, at the centre, and nothing else. It is also the fixture that demonstrates what
    the epsilon fill does -- a bowl has no outlet, so `fill_depressions` raises its whole
    interior into a ramp to the border and the post-fill drainage bears no relation to
    the surface it came from. That is the correct behaviour and it is exactly why the
    raw pit count, and not any post-fill number, is the verdict.

    The hill (``sign=-1``) is the complementary fixture: zero raw pits, radial drainage,
    and the fill leaves it untouched.
    """
    h, w = int(shape[0]), int(shape[1])
    cell = check_cell_m(cell_m)
    y = (np.arange(h)[:, None] - (h - 1) / 2.0) * cell
    x = (np.arange(w)[None, :] - (w - 1) / 2.0) * cell
    return float(sign) * float(slope) * np.hypot(x, y)


def radial_power_spectrum(z, cell_m: float, *, n_bins: int = 40):
    """Radially averaged power spectral density: ``(freq_per_m, psd)``.

    Not a landform metric -- it is here so the validation harness can *demonstrate* that
    `spectrum_matched_surrogate` really does match, which is what makes the surrogate a
    fair control rather than an assertion.
    """
    zz = as_field(z)
    cell = check_cell_m(cell_m)
    F = np.fft.fft2(zz - zz.mean())
    psd = (np.abs(F) ** 2) / zz.size
    f = _radial_freq(zz.shape, cell)
    fmax = f.max()
    fmin = f[f > 0].min()
    edges = np.logspace(np.log10(fmin), np.log10(fmax), int(n_bins) + 1)
    idx = np.digitize(f.ravel(), edges) - 1
    ok = (idx >= 0) & (idx < int(n_bins))
    sums = np.bincount(idx[ok], weights=psd.ravel()[ok], minlength=int(n_bins))
    cnts = np.bincount(idx[ok], minlength=int(n_bins))
    with np.errstate(invalid="ignore", divide="ignore"):
        mean = np.where(cnts > 0, sums / np.maximum(cnts, 1), np.nan)
    centres = np.sqrt(edges[:-1] * edges[1:])
    return centres, mean
