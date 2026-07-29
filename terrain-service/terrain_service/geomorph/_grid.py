"""Grid plumbing shared by every metric: validation, resolution guards, derivatives.

Three rules the rest of the package inherits from here.

**Every public entry point takes ``cell_m`` explicitly and every result carries it.**
Slope, curvature and the geomorphon histogram are all *resolution-dependent*, and it is
not a small effect. Measured on one Copernicus GLO-30 window of the Cumberland Plateau,
by block-averaging the same data to coarser grids:

    cell size     mean slope   frac > repose   curvature sd    geomorphon ridge+peak
    30.87 m        19.8 deg       0.029          5.8e-3 /m           0.18
    61.74 m        17.0 deg       0.005          3.5e-3 /m           0.25
    123.5 m        12.8 deg       0.000          1.9e-3 /m           0.32
    247.0 m         8.2 deg       0.000          8.4e-4 /m           0.34

Same landscape, every number different, several of them by more than the gap between
terrain classes. A comparison across cell sizes is therefore meaningless, and the API is
built so that making one is an exception rather than a plausible-looking number.
``require_same_resolution`` is the guard; ``report`` calls it.

**Edges are replicated, and the caller is told how many cells that affects.** A 3x3
operator on a padded edge produces a defensible but *biased* value there; a 1024^2 grid
has 0.4% edge cells so it does not matter for a histogram, but for the flow metrics the
bias is structural (truncated catchments) and those mask a margin instead.

**Everything is float64 inside.** These are second differences of a field whose dynamic
range is ~10^3 m; float32 second differences of that are ~10^-4 relative, which is fine
for a mean and not fine for a *skewness*, which is the statistic curvature is here for.
"""

from __future__ import annotations

from typing import Iterable, Sequence

import numpy as np

__all__ = [
    "ResolutionMismatch",
    "as_field",
    "check_cell_m",
    "require_same_resolution",
    "replicate_pad",
    "horn_gradient",
    "central_gradient",
    "zt_partials",
    "interior_mask",
    "weighted_loglog_fit",
    "moment_skew",
    "quantile_skew",
]


class ResolutionMismatch(ValueError):
    """Raised when fields or results at different cell sizes are compared.

    Deliberately a hard error and not a warning: the whole failure mode this package
    exists to prevent is a plausible-looking number that answers a different question
    than the one asked.
    """


def as_field(z, name: str = "z") -> np.ndarray:
    """Validate and normalise a heightfield to a contiguous 2-D float64 array.

    Heights are in **metres** by convention throughout this package; nothing here
    enforces that, but every documented unit downstream assumes it.
    """
    a = np.ascontiguousarray(z)
    if a.ndim != 2:
        raise ValueError(f"{name} must be 2-D, got shape {a.shape}")
    if a.size == 0:
        raise ValueError(f"{name} is empty")
    if a.shape[0] < 3 or a.shape[1] < 3:
        raise ValueError(f"{name} must be at least 3x3, got shape {a.shape}")
    a = a.astype(np.float64, copy=False)
    if not np.isfinite(a).all():
        raise ValueError(f"{name} contains NaN or infinity")
    return a


def check_cell_m(cell_m) -> float:
    """Validate a cell size in metres."""
    c = float(cell_m)
    if not np.isfinite(c) or c <= 0.0:
        raise ValueError(f"cell_m must be finite and > 0, got {cell_m!r}")
    return c


def _cell_of(obj) -> float:
    if isinstance(obj, (int, float, np.floating, np.integer)):
        return float(obj)
    cell = getattr(obj, "cell_m", None)
    if cell is None and isinstance(obj, dict):
        cell = obj.get("cell_m")
    if cell is None:
        raise TypeError(f"{obj!r} has no cell_m to compare")
    return float(cell)


def require_same_resolution(items: Iterable, *, rtol: float = 1e-9,
                            what: str = "results") -> float:
    """Return the common cell size of ``items``, or raise `ResolutionMismatch`.

    ``items`` may be floats, any object with a ``cell_m`` attribute (every result
    dataclass in this package has one), or dicts with a ``"cell_m"`` key. Use this
    before any cross-field comparison -- see the module docstring for why this is an
    error rather than a caveat in a report footnote.
    """
    cells = [_cell_of(o) for o in items]
    if not cells:
        raise ValueError("require_same_resolution needs at least one item")
    ref = cells[0]
    for c in cells[1:]:
        if not np.isclose(c, ref, rtol=rtol, atol=0.0):
            raise ResolutionMismatch(
                f"cannot compare {what} at different cell sizes: "
                f"{sorted(set(cells))} m. Slope, curvature and geomorphon histograms "
                "are all resolution-dependent; resample to a common cell size first."
            )
    return ref


def replicate_pad(z: np.ndarray, n: int = 1) -> np.ndarray:
    """Edge-replicate pad, the convention every 3x3 operator here uses."""
    return np.pad(z, n, mode="edge")


def _neighbourhood(z: np.ndarray):
    """The nine 3x3 stencil planes, edge-replicated, in the usual raster layout::

        a b c      (row-1)
        d e f      (row  )
        g h i      (row+1)

    Row index increases *downward* (south) as in every raster this repo handles. Slope
    magnitude, both curvatures and the geomorphon pattern are all invariant to that
    choice -- flipping the y axis flips ``zy`` and ``zxy`` together, and every term in
    which they appear is of even total degree in them -- so no caller needs to care.
    """
    p = replicate_pad(z, 1)
    return (
        p[0:-2, 0:-2], p[0:-2, 1:-1], p[0:-2, 2:],
        p[1:-1, 0:-2], p[1:-1, 1:-1], p[1:-1, 2:],
        p[2:, 0:-2], p[2:, 1:-1], p[2:, 2:],
    )


def horn_gradient(z: np.ndarray, cell_m: float) -> tuple[np.ndarray, np.ndarray]:
    """Horn (1981) third-order finite difference: ``(dz_dx, dz_dy)``, dimensionless.

    This is what GDAL, ArcGIS and QGIS all compute for "slope", so it is the method to
    use whenever a number here is going to be set beside a published one. It weights the
    two diagonal neighbours at half the cardinal, which makes it a mild low-pass
    compared with a plain central difference -- on a noisy field it reports a *smaller*
    slope, by ~4% on white noise, and that difference is a real methodological choice
    rather than an error in either.

    ``dz_dy`` is positive where elevation increases with **row index** (i.e. downward /
    southward); only the sign convention differs from a north-up gradient and no
    statistic in this package depends on it.
    """
    a, b, c, d, _e, f, g, h, i = _neighbourhood(z)
    dzdx = ((c + 2.0 * f + i) - (a + 2.0 * d + g)) / (8.0 * cell_m)
    dzdy = ((g + 2.0 * h + i) - (a + 2.0 * b + c)) / (8.0 * cell_m)
    return dzdx, dzdy


def central_gradient(z: np.ndarray, cell_m: float) -> tuple[np.ndarray, np.ndarray]:
    """Plain 2nd-order central difference gradient. Noisier than Horn, unsmoothed."""
    p = replicate_pad(z, 1)
    dzdx = (p[1:-1, 2:] - p[1:-1, 0:-2]) / (2.0 * cell_m)
    dzdy = (p[2:, 1:-1] - p[0:-2, 1:-1]) / (2.0 * cell_m)
    return dzdx, dzdy


def zt_partials(z: np.ndarray, cell_m: float):
    """Zevenbergen & Thorne (1987) partial derivatives on the 3x3 stencil.

    Returns ``(zx, zy, zxx, zyy, zxy)``: first derivatives dimensionless, second
    derivatives in **1/m**. This is the standard basis for profile and planform
    curvature; the quadratic surface it implies interpolates the four cardinal
    neighbours exactly, which is why it is preferred to Evans' least-squares fit when
    the point of the exercise is curvature at the cell rather than a smoothed field.
    """
    a, b, c, d, e, f, g, h, i = _neighbourhood(z)
    L = cell_m
    zx = (f - d) / (2.0 * L)
    zy = (h - b) / (2.0 * L)
    zxx = (d - 2.0 * e + f) / (L * L)
    zyy = (b - 2.0 * e + h) / (L * L)
    zxy = (a - c - g + i) / (4.0 * L * L)
    return zx, zy, zxx, zyy, zxy


def interior_mask(shape: Sequence[int], margin: int) -> np.ndarray:
    """Boolean mask that is True except within ``margin`` cells of the border."""
    h, w = int(shape[0]), int(shape[1])
    m = int(margin)
    if m < 0:
        raise ValueError(f"margin must be >= 0, got {margin}")
    out = np.zeros((h, w), dtype=bool)
    if 2 * m >= h or 2 * m >= w:
        raise ValueError(
            f"margin {m} leaves nothing of a {h}x{w} grid; use a bigger grid or a "
            "smaller margin"
        )
    out[m:h - m, m:w - m] = True
    return out


def weighted_loglog_fit(x: np.ndarray, y: np.ndarray, w: np.ndarray | None = None):
    """Weighted least squares of ``log10(y) = intercept + slope * log10(x)``.

    Returns ``(slope, intercept, r2, stderr_slope, rmse_log10)``. Weights are treated as
    *counts* (so ``stderr`` is the usual WLS standard error), which is what the binned
    slope-area and Hack fits want: a bin holding 10^5 cells should not be outvoted by a
    bin holding 12.

    Returns NaNs rather than raising when fewer than three usable points survive -- the
    callers all report a fit-quality field and a caller that ignores it is going to get
    a NaN rather than a fabricated exponent.
    """
    x = np.asarray(x, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)
    w = np.ones_like(x) if w is None else np.asarray(w, dtype=np.float64)
    ok = np.isfinite(x) & np.isfinite(y) & np.isfinite(w) & (x > 0) & (y > 0) & (w > 0)
    n = int(ok.sum())
    nan5 = (float("nan"),) * 5
    if n < 3:
        return nan5
    lx = np.log10(x[ok])
    ly = np.log10(y[ok])
    ww = w[ok]
    sw = ww.sum()
    mx = float((ww * lx).sum() / sw)
    my = float((ww * ly).sum() / sw)
    sxx = float((ww * (lx - mx) ** 2).sum())
    if sxx <= 0.0:
        return nan5
    sxy = float((ww * (lx - mx) * (ly - my)).sum())
    slope = sxy / sxx
    intercept = my - slope * mx
    resid = ly - (intercept + slope * lx)
    ss_res = float((ww * resid ** 2).sum())
    ss_tot = float((ww * (ly - my) ** 2).sum())
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0.0 else float("nan")
    # Unweighted-in-count sense: sigma^2 estimated from the weighted residuals.
    dof = n - 2
    sigma2 = ss_res / (sw * dof / n) / dof if dof > 0 and sw > 0 else float("nan")
    stderr = float(np.sqrt(sigma2 / sxx)) if np.isfinite(sigma2) and sxx > 0 else float("nan")
    rmse = float(np.sqrt(ss_res / sw))
    return float(slope), float(intercept), float(r2), stderr, rmse


def moment_skew(v: np.ndarray) -> float:
    """Fisher-Pearson moment skewness, ``E[(x-mu)^3] / sigma^3``.

    Sensitive to the extreme tail by construction -- on curvature fields a handful of
    cells set it -- which is why every caller reports `quantile_skew` beside it.
    """
    v = np.asarray(v, dtype=np.float64).ravel()
    v = v[np.isfinite(v)]
    if v.size < 3:
        return float("nan")
    d = v - v.mean()
    s = float(np.sqrt((d * d).mean()))
    if s <= 0.0:
        return float("nan")
    return float((d ** 3).mean() / s ** 3)


def quantile_skew(v: np.ndarray, q: float = 0.10) -> float:
    """Bowley-style skewness on the ``q`` / 0.5 / ``1-q`` quantiles, in [-1, 1].

    ``(p_hi + p_lo - 2*median) / (p_hi - p_lo)``. Bounded, and immune to the handful of
    cells that dominate the third moment, so this is the statistic to *compare* between
    terrain classes; the moment skew is reported for continuity with the literature.
    """
    v = np.asarray(v, dtype=np.float64).ravel()
    v = v[np.isfinite(v)]
    if v.size < 8:
        return float("nan")
    lo, mid, hi = np.quantile(v, [q, 0.5, 1.0 - q])
    span = hi - lo
    if span <= 0.0:
        return float("nan")
    return float((hi + lo - 2.0 * mid) / span)
