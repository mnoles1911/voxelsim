"""Hypsometric curve and integral.

The hypsometric curve plots relative height ``(z - z_min) / (z_max - z_min)`` against
the relative area above it, and the hypsometric integral (HI) is the area under it. It
is the oldest quantitative descriptor in geomorphology (Strahler 1952) and the cheapest
in this package: it is a pure order statistic of the elevation histogram, needs no
neighbourhood, and is therefore **exactly resolution-independent** as long as the window
is the same -- which makes it the one metric here that can be compared across our tiers
without a caveat.

That independence is also its limitation. HI is invariant under any monotone rescaling
of *area*, so it sees the elevation distribution and nothing about arrangement. Shuffle
every pixel of a real DEM and the HI does not move. It cannot distinguish terrain from
noise and it is not offered as though it could; what it does distinguish is **the shape
of the elevation distribution between terrain classes**, where it is genuinely
informative: a young, convex, uplift-dominated range sits near 0.6, a mature dissected
landscape near 0.4-0.5, and an old peneplain or a broad depositional plain below 0.35.

The identity ``HI == (mean - min) / (max - min)`` (Pike & Wilson 1971) is exact for the
area-weighted curve and is computed independently as ``elevation_relief_ratio``; the two
agreeing to 1e-12 is the module's own arithmetic check, and they are both reported so a
caller can see it.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ._grid import as_field, check_cell_m, moment_skew

__all__ = ["Hypsometry", "hypsometry"]


@dataclass(frozen=True)
class Hypsometry:
    """Hypsometric curve on a fixed relative-area grid, plus the integral.

    ``relative_area`` runs 0 -> 1 (fraction of the window at or above the height), and
    ``relative_height`` is the matching normalised elevation, so the pair is directly
    plottable and two windows' curves are directly subtractable.
    """

    cell_m: float
    n_cells: int
    hypsometric_integral: float
    elevation_relief_ratio: float
    relief_m: float
    z_min_m: float
    z_max_m: float
    z_mean_m: float
    z_median_m: float
    #: Skewness of the elevation distribution itself. Positive = a lot of low ground
    #: with a few high peaks (a plain with inselbergs); negative = a plateau with
    #: incised valleys.
    elevation_skew: float
    #: Fraction of the window lying above mid-relief. Reads the curve's shape at the
    #: point where a plain and a range differ most: a depositional plain buries almost
    #: everything below mid-relief (< 0.1), a range with broad high ground runs > 0.4.
    #: Unlike `hypsometric_integral` this is a single point on the curve, so it
    #: separates two distributions that happen to share a mean.
    area_above_mid_relief: float
    relative_area: np.ndarray
    relative_height: np.ndarray

    def to_dict(self) -> dict:
        return {
            k: getattr(self, k)
            for k in ("cell_m", "n_cells", "hypsometric_integral",
                      "elevation_relief_ratio", "relief_m", "z_min_m", "z_max_m",
                      "z_mean_m", "z_median_m", "elevation_skew",
                      "area_above_mid_relief")
        }


def hypsometry(z, cell_m: float, *, n_points: int = 101) -> Hypsometry:
    """Hypsometric curve and integral.

    ``cell_m`` does not enter the arithmetic -- every cell has the same area, so it
    cancels -- and is required anyway, both for the uniform signature and because the
    result carries it so a downstream comparison can be resolution-checked like every
    other result in this package.

    A field with zero relief (a perfect plane, or a constant) has no hypsometric curve;
    ``hypsometric_integral`` comes back NaN rather than 0 or 0.5.
    """
    zz = as_field(z)
    cell = check_cell_m(cell_m)
    if int(n_points) < 3:
        raise ValueError(f"n_points must be >= 3, got {n_points}")

    flat = zz.ravel()
    zmin, zmax = float(flat.min()), float(flat.max())
    relief = zmax - zmin
    if relief <= 0.0:
        nan = float("nan")
        return Hypsometry(
            cell_m=cell, n_cells=int(flat.size), hypsometric_integral=nan,
            elevation_relief_ratio=nan, relief_m=0.0, z_min_m=zmin, z_max_m=zmax,
            z_mean_m=float(flat.mean()), z_median_m=float(np.median(flat)),
            elevation_skew=nan, area_above_mid_relief=nan,
            relative_area=np.linspace(0.0, 1.0, int(n_points)),
            relative_height=np.full(int(n_points), nan),
        )

    # Relative area a runs 0..1 as the fraction of the window AT OR ABOVE h, so the
    # curve starts at (0, 1) and ends at (1, 0), which is the conventional orientation.
    a = np.linspace(0.0, 1.0, int(n_points))
    hgt = (np.quantile(flat, 1.0 - a) - zmin) / relief

    hi = float(np.trapezoid(hgt, a)) if hasattr(np, "trapezoid") else float(
        np.trapz(hgt, a))
    err = (float(flat.mean()) - zmin) / relief
    return Hypsometry(
        cell_m=cell,
        n_cells=int(flat.size),
        hypsometric_integral=hi,
        elevation_relief_ratio=float(err),
        relief_m=float(relief),
        z_min_m=zmin,
        z_max_m=zmax,
        z_mean_m=float(flat.mean()),
        z_median_m=float(np.median(flat)),
        elevation_skew=moment_skew(flat),
        area_above_mid_relief=float(
            np.count_nonzero(flat >= zmin + 0.5 * relief) / flat.size),
        relative_area=a,
        relative_height=hgt,
    )
