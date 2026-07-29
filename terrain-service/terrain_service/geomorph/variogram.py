"""Omnidirectional variogram, and the local Hurst exponent decade by decade.

``gamma(h) = 0.5 * E[(z(x+h) - z(x))^2]`` over log-spaced lags. On a self-affine surface
``gamma ~ h^(2H)``, so the local log-log slope halved is a scale-local **Hurst
exponent**. Real topography is not self-affine over all scales -- H drifts, and above
the correlation length gamma flattens to the field variance -- and the useful content is
exactly in how it drifts.

**What this metric is for here: tier continuity.** We build a surface in bands -- 30 m
from the diffusion model, 1.875 m from the bake, 10 cm from the client amplifier -- and
each hand-off is a chance to double-count a band or drop one. Neither shows up in a
hillshade and neither shows up in slope or curvature statistics, but both put a **kink**
in gamma at the hand-off lag: a dropped band is a local flat spot (H falls towards 0), a
double-counted one is a local step up (H rises above 1, which is impossible for a real
surface and is therefore a proof rather than a suspicion). `tier_continuity` reports the
change in local H across each named boundary and flags H > 1.

**What this metric is NOT for: realism.** The variogram is a second-order statistic and
is therefore a function of the power spectrum alone. A phase-randomised surrogate of a
real DEM -- same amplitude spectrum, random phases -- has a variogram *identical* to the
original to within sampling error, and no drainage network whatsoever. It is the exact
control this project has been burned by before. Do not gate realism on a variogram; gate
band bookkeeping on it, and gate realism on the pit and curvature statistics.

**One bias worth knowing before reading an absolute H.** On a field that is genuinely
periodic -- which every synthetic control in `controls` is, because they are synthesised
on a torus -- the variogram cannot keep growing past half the window, and the fitted
exponent is pulled towards the middle of its range: measured, ``fbm(hurst=0.3)`` reports
0.34 and ``fbm(hurst=0.9)`` reports 0.76. A real DEM window has no such wrap, and on a
non-periodic crop of the same fields the estimator recovers 0.37, 0.65 and 0.90 against
targets of 0.3, 0.6 and 0.9. So: absolute H is trustworthy on real data, compressed on
the synthetic controls, and the *ordering* is intact either way. `tier_continuity` is
unaffected because it compares two windows of the same field, where the bias cancels.

Method: exact, not sampled. For each target lag magnitude the four lag vectors
``(k,0) (0,k) (m,m) (m,-m)`` are evaluated by array shifts over the whole grid, so there
is no seed and no sampling noise; ``n_pairs`` is the true pair count. The four are
averaged, so a strongly anisotropic field (dune fields especially) reports the
directional mean -- `directional_anisotropy` reports the spread that hides.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ._grid import as_field, check_cell_m

__all__ = [
    "VariogramResult",
    "variogram",
    "tier_continuity",
    "TIER_BOUNDARIES_M",
]

#: The hand-off lags of our own pipeline: the diffusion model's pixel, the bake's fine
#: tier, and the client amplifier's voxel. A kink at one of these is a bookkeeping bug.
TIER_BOUNDARIES_M = (30.0, 1.875, 0.10)


@dataclass(frozen=True)
class VariogramResult:
    """Omnidirectional variogram plus per-decade Hurst fits.

    ``lag_m``    lag distances, metres, log-spaced.
    ``gamma_m2`` semivariance, m^2.
    ``hurst_decades`` one entry per decade of lag that held >= ``min_pts_per_decade``
                 lags: ``(lag_lo_m, lag_hi_m, H, r2)``.
    ``max_kink`` largest |H_i - H_{i+1}| between consecutive decade windows, and
                 ``kink_lag_m`` where it happened. This is the tier-continuity alarm.
    """

    cell_m: float
    lag_m: np.ndarray
    gamma_m2: np.ndarray
    n_pairs: np.ndarray
    #: Per-lag spread across the four directions, as a coefficient of variation.
    anisotropy: np.ndarray
    hurst_decades: tuple
    hurst_overall: float
    hurst_overall_r2: float
    max_kink: float
    kink_lag_m: float
    #: Field variance, m^2. gamma saturates at this once lags exceed the correlation
    #: length; lags beyond that carry no shape information.
    variance_m2: float
    #: Smallest lag at which gamma exceeds 95% of the variance, i.e. the range. NaN if
    #: the variogram never gets there inside the window.
    range_m: float

    def to_dict(self) -> dict:
        return {
            "cell_m": self.cell_m,
            "hurst_overall": self.hurst_overall,
            "hurst_overall_r2": self.hurst_overall_r2,
            "max_kink": self.max_kink,
            "kink_lag_m": self.kink_lag_m,
            "variance_m2": self.variance_m2,
            "range_m": self.range_m,
            "n_lags": int(self.lag_m.size),
            "mean_anisotropy": float(np.nanmean(self.anisotropy)),
        }

    def local_hurst(self, lag_m: float, window_decades: float = 0.5) -> float:
        """Hurst exponent fitted over lags within ``window_decades`` of ``lag_m``.

        The primitive `tier_continuity` is built from: fit H just below a boundary and
        just above it and compare.
        """
        return _fit_h(self.lag_m, self.gamma_m2,
                      10.0 ** (np.log10(lag_m) - window_decades),
                      10.0 ** (np.log10(lag_m) + window_decades))[0]


def _fit_h(lag, gamma, lo, hi):
    sel = np.isfinite(gamma) & (gamma > 0) & (lag >= lo) & (lag <= hi)
    if int(sel.sum()) < 3:
        return float("nan"), float("nan"), int(sel.sum())
    x = np.log10(lag[sel])
    y = np.log10(gamma[sel])
    n = x.size
    mx, my = x.mean(), y.mean()
    sxx = float(((x - mx) ** 2).sum())
    if sxx <= 0:
        return float("nan"), float("nan"), n
    s = float(((x - mx) * (y - my)).sum()) / sxx
    resid = y - (my + s * (x - mx))
    sst = float(((y - my) ** 2).sum())
    r2 = 1.0 - float((resid ** 2).sum()) / sst if sst > 0 else float("nan")
    return s / 2.0, r2, n


def _shift_gamma(z: np.ndarray, dr: int, dc: int):
    """``(gamma, n_pairs)`` for one integer lag vector, over every in-bounds pair."""
    h, w = z.shape
    ys = slice(max(0, dr), h + min(0, dr))
    xs = slice(max(0, dc), w + min(0, dc))
    yd = slice(max(0, -dr), h + min(0, -dr))
    xd = slice(max(0, -dc), w + min(0, -dc))
    if ys.start >= ys.stop or xs.start >= xs.stop:
        return float("nan"), 0
    d = z[ys, xs] - z[yd, xd]
    return 0.5 * float(np.mean(d * d)), int(d.size)


def variogram(z, cell_m: float, *, n_lags: int = 24, max_lag_frac: float = 0.25,
              min_pts_per_decade: int = 3) -> VariogramResult:
    """Omnidirectional variogram over log-spaced lags from one cell to ``max_lag_frac``.

    ``max_lag_frac`` of the shorter side is the largest lag evaluated. A quarter is the
    conventional limit: beyond it the pair count collapses and the estimate is dominated
    by the few longest-baseline pairs, which on a field with any trend is a measurement
    of the trend.

    ``n_lags`` log-spaced targets; duplicates after rounding to integer cells are
    collapsed, so at small grids the returned array is shorter than ``n_lags``.
    """
    zz = as_field(z)
    cell = check_cell_m(cell_m)
    h, w = zz.shape
    max_k = max(1, int(min(h, w) * float(max_lag_frac)))
    if max_k < 2:
        raise ValueError(
            f"a {h}x{w} grid at max_lag_frac={max_lag_frac} gives no usable lags"
        )
    targets = np.unique(np.round(
        np.logspace(0.0, np.log10(max_k), int(n_lags))).astype(int))
    targets = targets[targets >= 1]

    lags, gammas, pairs, aniso = [], [], [], []
    for k in targets:
        m = max(1, int(round(k / 1.4142135623730951)))
        vecs = [(k, 0), (0, k), (m, m), (m, -m)]
        # Nominal distance per vector differs slightly (m*sqrt2 vs k); group them by
        # their own true distance so the reported lag is honest.
        gs, ns, ds = [], [], []
        for dr, dc in vecs:
            g, n = _shift_gamma(zz, dr, dc)
            if n == 0 or not np.isfinite(g):
                continue
            gs.append(g)
            ns.append(n)
            ds.append(np.hypot(dr, dc) * cell)
        if not gs:
            continue
        gs = np.asarray(gs, float)
        ns = np.asarray(ns, float)
        lags.append(float(np.average(ds, weights=ns)))
        gammas.append(float(np.average(gs, weights=ns)))
        pairs.append(int(ns.sum()))
        mean_g = gs.mean()
        aniso.append(float(gs.std() / mean_g) if mean_g > 0 else float("nan"))

    lag = np.asarray(lags)
    gam = np.asarray(gammas)
    npair = np.asarray(pairs, dtype=np.int64)
    ani = np.asarray(aniso)

    # Collapse any duplicate lags produced by the integer rounding.
    order = np.argsort(lag)
    lag, gam, npair, ani = lag[order], gam[order], npair[order], ani[order]

    decades = []
    if lag.size:
        d0 = int(np.floor(np.log10(lag.min())))
        d1 = int(np.ceil(np.log10(lag.max())))
        for d in range(d0, d1):
            lo, hi = 10.0 ** d, 10.0 ** (d + 1)
            H, r2, n = _fit_h(lag, gam, lo, hi)
            if n >= int(min_pts_per_decade):
                decades.append((lo, hi, H, r2))

    kink, kink_lag = float("nan"), float("nan")
    if len(decades) >= 2:
        diffs = [abs(decades[i + 1][2] - decades[i][2]) for i in range(len(decades) - 1)]
        i = int(np.nanargmax(diffs)) if np.any(np.isfinite(diffs)) else 0
        kink = float(diffs[i])
        kink_lag = float(decades[i][1])

    H_all, r2_all, _ = _fit_h(lag, gam, lag.min(), lag.max()) if lag.size else (
        float("nan"), float("nan"), 0)
    var = float(zz.var())
    above = np.flatnonzero(gam >= 0.95 * var)
    rng = float(lag[above[0]]) if above.size else float("nan")

    return VariogramResult(
        cell_m=cell,
        lag_m=lag,
        gamma_m2=gam,
        n_pairs=npair,
        anisotropy=ani,
        hurst_decades=tuple(decades),
        hurst_overall=float(H_all),
        hurst_overall_r2=float(r2_all),
        max_kink=kink,
        kink_lag_m=kink_lag,
        variance_m2=var,
        range_m=rng,
    )


def tier_continuity(vg: VariogramResult, boundaries_m=TIER_BOUNDARIES_M,
                    *, window_decades: float = 0.5, kink_tol: float = 0.25) -> list:
    """Local Hurst just below and just above each tier boundary, and the step between.

    Returns one dict per boundary that the variogram actually straddles::

        {"boundary_m", "h_below", "h_above", "delta_h", "kink", "impossible"}

    ``kink`` is ``|delta_h| > kink_tol``; ``impossible`` is ``H > 1`` on either side,
    which no self-affine surface can produce and which is therefore direct evidence of a
    band being added twice rather than a judgement call about a threshold.

    **``kink_tol`` is calibrated, and the calibration says to be careful.** Put a
    fictitious boundary in the middle of a real GLO-30 scene's lag range and the local
    Hurst exponent still steps across it, because real landscapes genuinely break scale
    there: measured, ``dH`` = +0.01 (High Plains), -0.05 (Death Valley), -0.14 (Alps),
    -0.23 (Cumberland Plateau), **-0.47 (Rub' al Khali)**. So on a terrain-scale
    variogram there is no tolerance that separates a bookkeeping bug from a dune field,
    and this function should not be used as one.

    Where it does work is the case it was written for: our own **fine tier crossing a
    band hand-off**, where the surface either side of the boundary is the same process
    and the only thing that should change is which synthesiser produced it. Measured on
    a real baked 1.875 m tile across its 30 m boundary, ``dH`` = -0.025, and on its
    un-eroded B0+B1 predecessor -0.029. Against a natural drift of ~0.03 there, the
    default 0.25 is an order of magnitude of headroom and a step that size is a bug.

    ``impossible`` needs no calibration at all and is the stronger signal: no self-affine
    surface can have H > 1, so a window reporting one has had a band added to it twice.
    """
    out = []
    if vg.lag_m.size == 0:
        return out
    lo_lim, hi_lim = float(vg.lag_m.min()), float(vg.lag_m.max())
    for b in boundaries_m:
        b = float(b)
        lo_c = 10.0 ** (np.log10(b) - window_decades)
        hi_c = 10.0 ** (np.log10(b) + window_decades)
        if b <= lo_lim or b >= hi_lim:
            continue
        h_below, r2_b, n_b = _fit_h(vg.lag_m, vg.gamma_m2, lo_c, b)
        h_above, r2_a, n_a = _fit_h(vg.lag_m, vg.gamma_m2, b, hi_c)
        if n_b < 3 or n_a < 3:
            continue
        delta = h_above - h_below
        out.append({
            "boundary_m": b,
            "h_below": float(h_below),
            "h_above": float(h_above),
            "r2_below": float(r2_b),
            "r2_above": float(r2_a),
            "delta_h": float(delta),
            "kink": bool(abs(delta) > kink_tol),
            "impossible": bool(h_below > 1.0 or h_above > 1.0),
        })
    return out
