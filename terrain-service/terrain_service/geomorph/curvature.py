"""Profile and planform curvature, and above all their **asymmetry**.

The magnitude of curvature is a resolution artefact -- second differences over
``cell_m`` grow as the grid refines, roughly as ``1/cell_m`` on a self-affine surface --
so the *level* of any curvature statistic says almost nothing on its own. Its **sign
structure** says a great deal, and is what this module is for.

A landscape shaped by erosion is not symmetric under ``z -> -z``. Water collects, so
concavities are narrow, deep and few (valley floors, hollows, channel heads) while
convexities are broad, gentle and many (the soil-creep-rounded hilltops and interfluves
that occupy most of the map). Turn the landscape upside down and it looks wrong
immediately, and the curvature histogram is where that shows up.

Noise has no such preference. A Gaussian random field -- fBm, value noise, a
spectrum-matched surrogate of a real DEM -- has a curvature distribution that is
symmetric to *within sampling error*, because the field and its negation are equally
likely draws. That is the whole discriminating power here, and it is a difference of
**one to two orders of magnitude in the size of the asymmetry, not in its sign**:

    field                             profile skew   Laplacian skew   tail asymmetry
    Cumberland Plateau (fluvial)        -0.081          -0.041             1.32
    High Plains, Kansas                 -0.142          -0.111             1.15
    Bernese Oberland (alpine)           +0.028          +0.068             0.87
    Rub' al Khali (dunes)               +0.086          +0.095             0.90
    spectrum-matched surrogate of       +0.002          +0.0004            0.99
      the Bernese Oberland
    spectrum-matched surrogate of       +0.0005         +0.0006            1.00
      the Cumberland Plateau
    fBm, H = 0.75                       -0.002          -0.0007            1.00

Read that table carefully, because it says something the textbook version does not. The
**sign is a property of the terrain class, not of "being real"**: fluvially dissected
ground and plains are concave-skewed as expected, but glaciated alpine terrain and a
dune field come out convex-skewed -- arêtes and dune crests are sharp features and
U-shaped troughs and interdune flats are broad ones, which is the erosional asymmetry
running the other way. What separates real from synthetic is the **magnitude**: every
real scene is 0.03-0.15 and every Gaussian control is under 0.003. A realism check on
curvature must therefore be on ``abs(skew)``; a *class* check can use the sign.

Both surrogates in that table have the real scene's own variogram to within 15% at every
lag. Curvature asymmetry is one of only three things in this package that sees through
them.

Sign convention, stated once because every GIS picks a different one: **positive is
convex** (a hilltop has positive profile curvature, a valley floor negative). ArcGIS's
"profile curvature" is the negative of this. Units are 1/m throughout.

Profile and planform curvature both divide by the gradient magnitude and are undefined
where the surface is flat, so cells below ``min_slope`` are excluded and the excluded
fraction is reported. On flat terrain that fraction is large, and the skew computed from
what remains describes the non-flat part -- which is the honest answer, but read
``frac_masked`` before comparing two classes with very different flat fractions. The
Laplacian is reported alongside precisely because it needs no such mask.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ._grid import as_field, check_cell_m, moment_skew, quantile_skew, zt_partials

__all__ = [
    "CurvatureFields",
    "CurvatureStats",
    "curvature_fields",
    "curvature_statistics",
]


@dataclass(frozen=True)
class CurvatureFields:
    """Per-cell curvature, all in **1/m**, positive = convex.

    ``profile``   curvature in the vertical plane containing the gradient (along-slope).
    ``planform``  curvature of the contour line through the cell (across-slope).
    ``laplacian`` ``zxx + zyy``; defined everywhere, needs no slope mask.
    ``valid``     False where the gradient is below ``min_slope`` and profile/planform
                  are therefore NaN.
    """

    cell_m: float
    profile: np.ndarray
    planform: np.ndarray
    laplacian: np.ndarray
    valid: np.ndarray
    min_slope: float


@dataclass(frozen=True)
class CurvatureStats:
    """Curvature summary. All curvatures in 1/m; skews dimensionless."""

    cell_m: float
    n_valid: int
    frac_masked: float
    min_slope: float

    profile_mean: float
    profile_std: float
    profile_skew: float
    profile_quantile_skew: float
    profile_p01: float
    profile_p99: float

    planform_mean: float
    planform_std: float
    planform_skew: float
    planform_quantile_skew: float

    laplacian_mean: float
    laplacian_std: float
    laplacian_skew: float
    laplacian_quantile_skew: float

    #: Fraction of *valid* cells with convex profile curvature. Erosional landscapes
    #: run above 0.5 -- most of the map is interfluve -- while a symmetric field sits
    #: on 0.5 by construction.
    convex_frac: float
    #: Ratio of the concave tail's reach to the convex tail's, |p01| / p99 on profile
    #: curvature. Real terrain > 1 (valleys are sharper than ridges), noise == 1.
    tail_asymmetry: float

    def to_dict(self) -> dict:
        return {k: getattr(self, k) for k in self.__dataclass_fields__}


def curvature_fields(z, cell_m: float, *, min_slope: float = 0.01) -> CurvatureFields:
    """Profile, planform and Laplacian curvature in 1/m, positive = convex.

    ``min_slope`` is a gradient-magnitude floor (dimensionless rise/run; the default
    0.01 is 0.57 deg). Profile and planform curvature both carry ``|grad z|`` in the
    denominator and blow up as it goes to zero -- not as a numerical accident but
    because the *direction* of steepest descent, which both are defined relative to, is
    genuinely undefined on a flat. Cells below the floor come back NaN and are flagged
    in ``valid``.
    """
    zz = as_field(z)
    cell = check_cell_m(cell_m)
    if not np.isfinite(min_slope) or min_slope < 0.0:
        raise ValueError(f"min_slope must be finite and >= 0, got {min_slope!r}")

    zx, zy, zxx, zyy, zxy = zt_partials(zz, cell)
    p = zx * zx + zy * zy          # squared gradient magnitude
    valid = p >= float(min_slope) ** 2

    with np.errstate(invalid="ignore", divide="ignore"):
        denom_prof = p * np.power(1.0 + p, 1.5)
        denom_plan = np.power(p, 1.5)
        # Signs: these are the standard Zevenbergen-Thorne forms negated, so that a
        # summit (zxx, zyy < 0) comes out POSITIVE. See the module docstring.
        profile = -(zxx * zx * zx + 2.0 * zxy * zx * zy + zyy * zy * zy) / denom_prof
        planform = -(zxx * zy * zy - 2.0 * zxy * zx * zy + zyy * zx * zx) / denom_plan

    # The Zevenbergen-Thorne zxx/zyy above are one-sided at the replicated border, which
    # makes the border ring identically zero rather than merely biased. Mark it invalid
    # instead of letting a ring of exact zeros distort the skew of a small window.
    valid[0, :] = valid[-1, :] = False
    valid[:, 0] = valid[:, -1] = False

    profile = np.where(valid, profile, np.nan)
    planform = np.where(valid, planform, np.nan)
    laplacian = zxx + zyy
    # Same convention: positive = convex.
    laplacian = -laplacian
    return CurvatureFields(
        cell_m=cell,
        profile=profile,
        planform=planform,
        laplacian=laplacian,
        valid=valid,
        min_slope=float(min_slope),
    )


def _lap_interior(lap: np.ndarray) -> np.ndarray:
    return lap[1:-1, 1:-1].ravel()


def curvature_statistics(z, cell_m: float, *, min_slope: float = 0.01) -> CurvatureStats:
    """Curvature moments and, the point of the exercise, their skewness.

    Both a moment skew (third standardised moment, comparable with the literature and
    dominated by a handful of cells) and a `quantile_skew` (p10/p50/p90, bounded and
    robust) are reported for each field. **Use the quantile skew to compare classes.**
    On curvature the moment skew of a 1024^2 window swings by more than its own value
    between two halves of the same scene; the quantile skew moves by ~0.02.
    """
    f = curvature_fields(z, cell_m, min_slope=min_slope)
    prof = f.profile[f.valid]
    plan = f.planform[f.valid]
    lap = _lap_interior(f.laplacian)
    n_valid = int(prof.size)
    total = int(f.valid.size)

    if n_valid < 8:
        nan = float("nan")
        return CurvatureStats(
            cell_m=f.cell_m, n_valid=n_valid,
            frac_masked=1.0 - n_valid / total, min_slope=f.min_slope,
            profile_mean=nan, profile_std=nan, profile_skew=nan,
            profile_quantile_skew=nan, profile_p01=nan, profile_p99=nan,
            planform_mean=nan, planform_std=nan, planform_skew=nan,
            planform_quantile_skew=nan,
            laplacian_mean=float(lap.mean()), laplacian_std=float(lap.std()),
            laplacian_skew=moment_skew(lap), laplacian_quantile_skew=quantile_skew(lap),
            convex_frac=nan, tail_asymmetry=nan,
        )

    p01, p99 = np.quantile(prof, [0.01, 0.99])
    tail = float(abs(p01) / p99) if p99 > 0 else float("nan")
    return CurvatureStats(
        cell_m=f.cell_m,
        n_valid=n_valid,
        frac_masked=float(1.0 - n_valid / total),
        min_slope=f.min_slope,
        profile_mean=float(prof.mean()),
        profile_std=float(prof.std()),
        profile_skew=moment_skew(prof),
        profile_quantile_skew=quantile_skew(prof),
        profile_p01=float(p01),
        profile_p99=float(p99),
        planform_mean=float(plan.mean()),
        planform_std=float(plan.std()),
        planform_skew=moment_skew(plan),
        planform_quantile_skew=quantile_skew(plan),
        laplacian_mean=float(lap.mean()),
        laplacian_std=float(lap.std()),
        laplacian_skew=moment_skew(lap),
        laplacian_quantile_skew=quantile_skew(lap),
        convex_frac=float(np.count_nonzero(prof > 0.0) / n_valid),
        tail_asymmetry=tail,
    )
