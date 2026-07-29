"""Slope distribution: the full PDF, its order statistics, and the repose tail.

Why the *distribution* and not the mean: real hillslopes are **threshold-limited**. Rock
and soil cannot stand steeper than their angle of repose for long, so on a landscape
that has been eroding for any length of time the slope histogram piles up below repose
and then stops -- a mode in the 20-35 deg range with a sharp right shoulder, not a
gentle taper. A synthetic field built by adding noise has no such threshold, so its
slope histogram is whatever the noise amplitude made it: measured, an fBm scaled to the
Alpine window's own relief puts 36% of its cells past repose, and a pixel-shuffled
version of that window puts 99.7% past it.

**But be clear about what this metric cannot do.** Against a *spectrum-matched*
surrogate -- a Gaussian field with the real scene's own variogram -- the slope
distribution is nearly identical: measured on the Cumberland Plateau window, mean slope
19.8 deg against the surrogate's 18.4, p99 39.1 against 39.8, and fraction above repose
0.029 against 0.030. Slope statistics catch a *badly scaled* fake and are blind to a
well-scaled one. They are here because they describe the terrain, and because the repose
tail is what `bake.thermal`'s calibration is quoted against -- not because they can
answer "is this a landscape".

**This is a resolution-dependent measurement and there is no fixing that.** Slope is a
finite difference over ``cell_m``, so coarsening the grid averages away the steep cells:
the same Cumberland Plateau scene measures 19.8 deg mean at 30.9 m and 8.2 deg at
247 m, with the repose fraction falling from 2.9% to zero. Any comparison against a
reference must be at matched cell size -- see `_grid.require_same_resolution`.

Two caveats on `frac_above_repose`, both inherited from `bake.incise`'s calibration
notes:

* the gradient magnitude of a 2-D field can exceed the repose angle by up to sqrt(2) on
  a ridge or a corner without violating the per-axis rule `bake.thermal.relax` actually
  enforces, so this fraction is an upper bound on "material that should have failed";
* it is a *tail* statistic on a few percent of cells, so it is noisy on small windows.
  512^2 at 30 m is about the smallest window on which it is stable to +-0.3 pp.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ._grid import (
    as_field,
    central_gradient,
    check_cell_m,
    horn_gradient,
    moment_skew,
    quantile_skew,
)

__all__ = ["SlopeStats", "slope_field", "slope_statistics", "REPOSE_DEG"]

#: Angle of repose used as the default threshold. 36 deg is `bake.thermal`'s own limit
#: (see `tests/test_bake_geomorph.py`), so "fraction above repose" here means the same
#: thing it means in the bake's calibration table rather than something adjacent to it.
REPOSE_DEG = 36.0


@dataclass(frozen=True)
class SlopeStats:
    """Slope distribution summary. Angles in **degrees**, ``mean_tan`` dimensionless."""

    cell_m: float
    method: str
    n: int
    mean_deg: float
    median_deg: float
    p95_deg: float
    p99_deg: float
    std_deg: float
    mean_tan: float
    max_deg: float
    skew: float
    quantile_skew: float
    repose_deg: float
    frac_above_repose: float
    #: Bin edges of the PDF, degrees, length ``len(pdf) + 1``.
    pdf_edges_deg: np.ndarray
    #: Probability density per degree; ``(pdf * diff(edges)).sum() == 1``.
    pdf: np.ndarray
    #: Modal slope, taken as the centre of the fullest PDF bin.
    mode_deg: float

    def to_dict(self) -> dict:
        return {
            "cell_m": self.cell_m,
            "method": self.method,
            "n": self.n,
            "mean_deg": self.mean_deg,
            "median_deg": self.median_deg,
            "p95_deg": self.p95_deg,
            "p99_deg": self.p99_deg,
            "std_deg": self.std_deg,
            "mean_tan": self.mean_tan,
            "max_deg": self.max_deg,
            "skew": self.skew,
            "quantile_skew": self.quantile_skew,
            "repose_deg": self.repose_deg,
            "frac_above_repose": self.frac_above_repose,
            "mode_deg": self.mode_deg,
        }


def slope_field(z, cell_m: float, method: str = "horn") -> np.ndarray:
    """Slope magnitude as **tan(angle)**, dimensionless, shaped like ``z``.

    ``method`` is ``"horn"`` (default, matches GDAL/ArcGIS/QGIS) or ``"central"``
    (plain central difference, noisier and unsmoothed). Border cells use edge
    replication, so they are defensible but biased low; on any window this package is
    meant for they are under 0.5% of cells.
    """
    zz = as_field(z)
    cell = check_cell_m(cell_m)
    if method == "horn":
        gx, gy = horn_gradient(zz, cell)
    elif method == "central":
        gx, gy = central_gradient(zz, cell)
    else:
        raise ValueError(f"method must be 'horn' or 'central', got {method!r}")
    return np.hypot(gx, gy)


def slope_statistics(z, cell_m: float, *, method: str = "horn",
                     repose_deg: float = REPOSE_DEG,
                     bins: int = 180) -> SlopeStats:
    """Full slope PDF plus the order statistics and the repose tail.

    ``bins`` histogram bins spanning 0-90 deg; the default 180 gives 0.5 deg bins, which
    resolves the repose shoulder without making the PDF noisy on a 512^2 window.
    """
    tan = slope_field(z, cell_m, method=method)
    deg = np.degrees(np.arctan(tan))
    flat = deg.ravel()
    if int(bins) < 1:
        raise ValueError(f"bins must be >= 1, got {bins}")
    edges = np.linspace(0.0, 90.0, int(bins) + 1)
    counts, _ = np.histogram(flat, bins=edges)
    width = edges[1] - edges[0]
    pdf = counts / (flat.size * width)
    imode = int(np.argmax(counts))
    q = np.quantile(flat, [0.5, 0.95, 0.99])
    return SlopeStats(
        cell_m=check_cell_m(cell_m),
        method=method,
        n=int(flat.size),
        mean_deg=float(flat.mean()),
        median_deg=float(q[0]),
        p95_deg=float(q[1]),
        p99_deg=float(q[2]),
        std_deg=float(flat.std()),
        mean_tan=float(tan.mean()),
        max_deg=float(flat.max()),
        skew=moment_skew(flat),
        quantile_skew=quantile_skew(flat),
        repose_deg=float(repose_deg),
        frac_above_repose=float(np.count_nonzero(flat > float(repose_deg)) / flat.size),
        pdf_edges_deg=edges,
        pdf=pdf,
        mode_deg=float(0.5 * (edges[imode] + edges[imode + 1])),
    )
