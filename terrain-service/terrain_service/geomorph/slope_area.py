"""The slope-area relation: the one measurement that can falsify our own erosion model.

Fluvial geomorphology's central empirical law is that along a channel network, local
slope falls as a power of upslope contributing area::

    S = k_s * A^(-theta)

with a **concavity index** theta that real rivers put in the 0.35-0.6 range. Below the
channel head the relation inverts -- on a hillslope, slope *increases* with contributing
area, because creep and landsliding, not water, set the form -- so a plot of binned
median S against log A has a characteristic shape: a rising hillslope limb, a peak at
the channel-initiation area, and a straight falling fluvial limb. The location of that
peak is the drainage density's twin, and the slope of that limb is theta.

**Why this matters more here than anywhere else in the package.** Our bake incises with
``depth = K * A^m * S^n`` at ``m = 0.45, n = 0.8`` (`bake.incise.stream_power`). At
topographic steady state the stream-power law implies ``theta = m/n``, which for our
constants is exactly **0.5625**. That is a numerical prediction our own code makes about
its own output, and it is checkable on the final surface. It is the only place in this
project where a metric can say the erosion model did or did not do what the erosion
model claims.

Three honest caveats on reading a theta:

* ``theta = m/n`` holds *at steady state under uniform uplift*. The bake applies one
  incision pass to a diffusion-model surface, not a relaxation to steady state, so the
  prediction is an attractor the surface is being pushed towards rather than a value it
  must hit. A measured theta between the input surface's and 0.5625 is the expected
  result; a measured theta that has not moved towards 0.5625 at all means the incision
  pass is not shaping the network.
* Below the channel head the fit is meaningless, so the fit range starts at the detected
  break. If the break is not resolved -- which happens when ``cell_m`` is so coarse that
  every cell already has a channel-sized catchment -- ``channel_head_resolved`` is False
  and the theta is over whatever range was available.
* **A theta is not evidence of a real drainage network.** The epsilon depression fill
  manufactures a routable network on white noise, and that manufactured network has a
  perfectly respectable slope-area curve. Read `flow_context.pit_statistics` first.

**What this method actually reads on real Earth, which is not the textbook number.**
Measured on five 1024^2 GLO-30 windows at 30.87 m with everything below at its default:

    Cumberland Plateau (dissected fluvial)    theta = 0.275   r2 0.98
    High Plains, Kansas                       theta = 0.278   r2 0.98
    Bernese Oberland (alpine)                 theta = 0.234   r2 0.96
    Panamint Range (arid, fans)               theta = 0.400   r2 0.88
    Rub' al Khali (dunes, no network)         theta = 0.179   r2 0.67
    spectrum-matched surrogates of the        theta = 0.13, 0.15
      first two, which have no network

Real Earth reads 0.18-0.40 here, not the 0.35-0.6 the literature quotes, and the gap is
methodological rather than an error: published concavities are fitted to *channel cells
only*, usually on D8 area, over five or six decades. This fits binned medians of every
cell above the detected break, on MFD area (because that is the ``A`` the bake's
``A^m`` consumes), over three or four decades. Switching to a near-D8 routing (``p = 8``)
moves theta by less than 0.06 and does not close the gap.

**So compare like with like.** A baked surface measuring theta = 0.5625 by this method
would not be "correct", it would be twice as concave as the Alps measured the same way.
The number to hold in mind for our own output is the 0.18-0.40 above; ``0.5625`` is what
the stream-power law predicts at steady state, and the distance between those two is a
statement about how far one incision pass is from steady state, not a target.

Method notes: ``A`` is the MFD accumulation (the bake's own routing) and ``S`` is the D8
steepest-descent slope, which is the exact pair the stream-power term uses -- an
MFD-weighted slope is systematically gentler and would bias theta. Bins are logarithmic
in area and the statistic per bin is the **median**, not the mean: within-bin slope is
strongly right-skewed and a mean tracks the tail.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ._grid import check_cell_m, interior_mask, weighted_loglog_fit
from .flow_context import FlowContext, resolve_context

__all__ = [
    "SlopeAreaResult",
    "slope_area_relation",
    "PREDICTED_THETA",
    "STREAM_POWER_M",
    "STREAM_POWER_N",
]

#: `bake.incise.stream_power`'s defaults, and the prediction they imply.
STREAM_POWER_M = 0.45
STREAM_POWER_N = 0.8
PREDICTED_THETA = STREAM_POWER_M / STREAM_POWER_N  # 0.5625


@dataclass(frozen=True)
class SlopeAreaResult:
    """Binned slope-area curve plus the fluvial-limb power-law fit.

    ``theta``  concavity index, the negative of the fitted log-log slope. Compare with
               `PREDICTED_THETA` (0.5625) for our own model, and with 0.35-0.6 for real
               rivers.
    ``r2``     of the weighted fit on bin medians. Below ~0.8 the "power law" is a line
               drawn through a curve and the theta should not be quoted.
    ``channel_head_area_m2`` area at the peak of the smoothed binned curve, i.e. the
               hillslope-to-fluvial transition. Real humid landscapes: 10^3-10^5 m^2.
    """

    cell_m: float
    theta: float
    theta_stderr: float
    r2: float
    rmse_log10: float
    ks: float                      # intercept: S at A = 1 m^2, extrapolated
    channel_head_area_m2: float
    channel_head_resolved: bool
    fit_area_lo_m2: float
    fit_area_hi_m2: float
    n_bins_fit: int
    n_cells: int
    predicted_theta: float
    theta_minus_predicted: float
    #: Bin centres, m^2 (geometric centre of each log bin).
    area_m2: np.ndarray
    slope_median: np.ndarray
    slope_p25: np.ndarray
    slope_p75: np.ndarray
    count: np.ndarray
    #: True for bins that entered the fit.
    in_fit: np.ndarray

    def to_dict(self) -> dict:
        return {
            k: getattr(self, k)
            for k in (
                "cell_m", "theta", "theta_stderr", "r2", "rmse_log10", "ks",
                "channel_head_area_m2", "channel_head_resolved",
                "fit_area_lo_m2", "fit_area_hi_m2", "n_bins_fit", "n_cells",
                "predicted_theta", "theta_minus_predicted",
            )
        }


def _running_median(v: np.ndarray, k: int) -> np.ndarray:
    """Centred running median of odd width ``k``, edges shortened rather than padded."""
    if k <= 1:
        return v.copy()
    half = k // 2
    out = np.empty_like(v)
    for i in range(v.size):
        lo = max(0, i - half)
        hi = min(v.size, i + half + 1)
        out[i] = np.median(v[lo:hi])
    return out


def slope_area_relation(z=None, cell_m: float = None, *, ctx: FlowContext | None = None,
                        bins_per_decade: int = 8, min_bin_count: int = 30,
                        border_margin_cells: int | None = None,
                        min_slope: float = 1e-6,
                        smooth_bins: int = 3,
                        fit_max_area_frac: float = 0.05,
                        fit_min_area_m2: float | None = None,
                        fit_max_area_m2: float | None = None) -> SlopeAreaResult:
    """Bin (A, S) logarithmically, find the channel head, fit the fluvial limb.

    ``ctx`` reuses a `FlowContext` rather than filling and routing again; when it is
    given, ``z`` may be None but ``cell_m`` must still match, so a mismatched
    resolution is an error rather than a silent rescale.

    ``border_margin_cells`` excludes cells near the domain edge, whose contributing area
    is truncated by the window rather than by the terrain. Defaults to 2% of the shorter
    side, minimum 4 cells. This is a real bias and not a rounding one: without it, the
    largest-area bins are dominated by edge cells whose upslope area was cut off, which
    drags the fluvial limb's right end up and flattens theta.

    ``min_slope`` drops cells whose D8 slope is at or below the floor -- filled flats,
    where the only gradient is the fill's own epsilon. Including them puts a floor of
    ~1e-7 in the slope distribution and bends the fluvial limb down at large A.

    ``fit_max_area_frac`` caps the fit at that fraction of the window's own area, and it
    is load-bearing rather than tidy. The largest-area bins of any finite window are one
    or two trunk channels near the outlet: a handful of cells, every one of them close
    to an edge that truncated its catchment, and on a valley floor whose gradient the
    window cannot resolve. Measured on the baked 1.875 m tier, letting the fit run to
    the largest populated bin gave **theta = 0.083 with an r2 of 0.44**; capping it at
    5% of the domain gave **0.27 with an r2 of 0.95** over the same data, because the
    last three bins were a cliff, not a limb. Five percent is the point at which the
    reported theta stopped moving on both the 30 m reference scenes and the baked tier;
    ``fit_max_area_m2`` overrides it outright.
    """
    c = resolve_context(z, cell_m, ctx)
    cell = check_cell_m(c.cell_m)
    h, w = c.shape

    if border_margin_cells is None:
        border_margin_cells = max(4, int(round(0.02 * min(h, w))))
    mask = interior_mask((h, w), int(border_margin_cells))
    mask &= c.slope_d8 > float(min_slope)
    mask &= c.receiver >= 0

    area = c.area_m2[mask]
    slope = c.slope_d8[mask]
    n_cells = int(area.size)
    if n_cells < 100:
        raise ValueError(
            f"only {n_cells} cells survived masking; the window is too small or too "
            "flat for a slope-area relation"
        )

    la = np.log10(area)
    lo = np.floor(la.min() * bins_per_decade) / bins_per_decade
    hi = np.ceil(la.max() * bins_per_decade) / bins_per_decade
    nb = max(1, int(round((hi - lo) * bins_per_decade)))
    edges = np.linspace(lo, hi, nb + 1)
    idx = np.clip(np.digitize(la, edges) - 1, 0, nb - 1)

    centres = 10.0 ** (0.5 * (edges[:-1] + edges[1:]))
    med = np.full(nb, np.nan)
    p25 = np.full(nb, np.nan)
    p75 = np.full(nb, np.nan)
    cnt = np.zeros(nb, dtype=np.int64)
    order = np.argsort(idx, kind="stable")
    bounds = np.searchsorted(idx[order], np.arange(nb + 1))
    for b in range(nb):
        sl = slope[order[bounds[b]:bounds[b + 1]]]
        cnt[b] = sl.size
        if sl.size:
            med[b], p25[b], p75[b] = np.quantile(sl, [0.5, 0.25, 0.75])

    usable = cnt >= int(min_bin_count)
    if usable.sum() < 4:
        raise ValueError(
            f"only {int(usable.sum())} area bins hold >= {min_bin_count} cells; "
            "lower min_bin_count or use a larger window"
        )

    # --- channel head: the peak of the smoothed binned curve ------------------------
    sm = np.full(nb, np.nan)
    sm[usable] = _running_median(med[usable], int(smooth_bins))
    peak = int(np.nanargmax(np.where(usable, sm, -np.inf)))
    first_usable = int(np.argmax(usable))
    resolved = peak > first_usable
    head_area = float(centres[peak])

    # --- fluvial limb: from the break to the largest well-populated bin -------------
    lo_area = float(fit_min_area_m2) if fit_min_area_m2 is not None else head_area
    last_usable = int(np.max(np.flatnonzero(usable)))
    if fit_max_area_m2 is not None:
        hi_area = float(fit_max_area_m2)
    else:
        domain_area = float(h) * float(w) * cell * cell
        hi_area = min(float(centres[last_usable]),
                      float(fit_max_area_frac) * domain_area)
    in_fit = usable & (centres >= lo_area * (1.0 - 1e-9)) & (centres <= hi_area * (1.0 + 1e-9))
    if in_fit.sum() < 3:
        # Fall back to every usable bin at or above the peak; still reported honestly
        # through n_bins_fit and r2.
        in_fit = usable & (np.arange(nb) >= peak)

    slope_fit, intercept, r2, stderr, rmse = weighted_loglog_fit(
        centres[in_fit], med[in_fit], cnt[in_fit].astype(np.float64)
    )
    theta = -slope_fit if np.isfinite(slope_fit) else float("nan")

    fit_centres = centres[in_fit]
    return SlopeAreaResult(
        cell_m=cell,
        theta=float(theta),
        theta_stderr=float(stderr),
        r2=float(r2),
        rmse_log10=float(rmse),
        ks=float(10.0 ** intercept) if np.isfinite(intercept) else float("nan"),
        channel_head_area_m2=head_area,
        channel_head_resolved=bool(resolved),
        fit_area_lo_m2=float(fit_centres.min()) if fit_centres.size else float("nan"),
        fit_area_hi_m2=float(fit_centres.max()) if fit_centres.size else float("nan"),
        n_bins_fit=int(in_fit.sum()),
        n_cells=n_cells,
        predicted_theta=PREDICTED_THETA,
        theta_minus_predicted=float(theta - PREDICTED_THETA),
        area_m2=centres,
        slope_median=med,
        slope_p25=p25,
        slope_p75=p75,
        count=cnt,
        in_fit=in_fit,
    )
