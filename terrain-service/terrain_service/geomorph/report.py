"""Reporting: the metric bundle, the discrimination table, and (only here) plots.

The library computes numbers. This module is the only place allowed to format or draw
them, and matplotlib is imported lazily inside the plotting functions so that neither a
headless bake pod nor CI has to have it.

`describe` runs every metric on one field with one shared flow pass and returns a flat
dict of scalars. `discrimination_table` takes several of those, keyed by case name, and
renders the metric x case table -- with `require_same_resolution` enforced across the
whole set, because a table whose columns were measured at different cell sizes is worse
than no table.

`separation` is the part that turns the table into a decision. For each row it reports
the **separation ratio**: the spread across the cases being separated, divided by the
within-case spread estimated by splitting each field into quadrants. A metric whose
between-class spread is smaller than its own within-class noise has not earned a place,
however geomorphologically respectable it sounds.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from ._grid import require_same_resolution
from .curvature import curvature_statistics
from .drainage import A_CRIT_M2 as A_CRIT_M2_
from .drainage import drainage_density, hacks_law, pit_statistics
from .flow_context import flow_context
from .geomorphon import geomorphon_histogram
from .hypsometry import hypsometry
from .slope import slope_statistics
from .slope_area import slope_area_relation
from .variogram import tier_continuity, variogram

__all__ = ["describe", "default_a_crit_m2", "HEADLINE_METRICS",
           "discrimination_table", "separation", "separation_table",
           "paired_contrast", "paired_contrast_table",
           "plot_slope_area", "plot_variogram", "plot_hypsometry"]


#: The rows of the discrimination table, in the order they are reported, each with the
#: unit and a one-line reading. Chosen by measurement, not by taste -- see the module
#: docstring of each metric and the validation tool's output for what earned a place.
HEADLINE_METRICS = (
    ("pit_density_per_km2", "1/km2", "raw local minima; the fill-effort verdict"),
    ("fill_volume_per_area_m", "m", "mean depression-fill depth over the whole domain"),
    ("fill_depth_p99_m", "m", "how deep the deepest few percent of the fill went"),
    ("theta", "-", "slope-area concavity; bake predicts 0.5625, Earth 0.35-0.6"),
    ("slope_area_r2", "-", "is the fluvial limb actually a power law"),
    ("channel_head_area_m2", "m2", "hillslope-to-fluvial break"),
    ("channel_head_resolved", "-", "was a break found above the smallest bin at all"),
    ("curvature_asymmetry", "-", "|Laplacian quantile skew|; the realism gate"),
    ("profile_quantile_skew", "-", "valley/ridge asymmetry, signed; sign is a class "
                                   "property, magnitude is the realism signal"),
    ("laplacian_quantile_skew", "-", "the same, needing no slope mask"),
    ("tail_asymmetry", "-", "|p01|/p99 of profile curvature; 1 = noise"),
    ("convex_frac", "-", "fraction of slope cells that are convex"),
    ("frac_masked", "-", "curvature cells below the slope floor"),
    ("mean_deg", "deg", "mean slope"),
    ("p99_deg", "deg", "slope p99"),
    ("frac_above_repose", "-", "fraction over the angle of repose"),
    ("frac_flat", "-", "geomorphon flat"),
    ("frac_slope", "-", "geomorphon slope"),
    ("frac_ridge_peak", "-", "geomorphon ridge + peak"),
    ("frac_valley_pit", "-", "geomorphon valley + pit"),
    ("frac_flat_coarse_thresh", "-", "geomorphon flat at 3x the flatness threshold"),
    ("dd_km_per_km2", "km/km2", "drainage density at a_crit"),
    ("saturated", "-", "is the drainage density describing the raster, not the terrain"),
    ("hack_h", "-", "Hack exponent; Earth 0.57"),
    ("hack_r2", "-", "quality of the Hack fit"),
    ("hypsometric_integral", "-", "Strahler HI"),
    ("area_above_mid_relief", "-", "fraction of the window above mid-relief"),
    ("hurst_overall", "-", "variogram Hurst over the whole lag range"),
    ("variogram_anisotropy", "-", "directional spread of gamma; 0 = isotropic"),
    ("variogram_max_kink", "-", "largest step in per-decade Hurst"),
)


def default_a_crit_m2(cell_m: float, min_cells: int = 100) -> float:
    """Channel-initiation area for a given grid: the bake's own, or 100 cells.

    `drainage.A_CRIT_M2` is 10^4 m^2, which at the bake's 1.875 m is 2840 cells -- amply
    resolved. At GLO-30's 30.87 m it is **ten** cells, and a channel definition ten cells
    across makes the whole raster a channel: measured on the Alpine window it put
    drainage density at 22.6 km/km^2 against a saturation ceiling of 32.4, i.e. the
    number described the pixel size and not the mountain. A hundred cells is the floor
    at which the network is resolved and the answer still lands in the 10^3-10^5 m^2
    band that real channel heads occupy. Returning it from one place keeps the choice
    visible instead of buried in a default argument.
    """
    return max(float(A_CRIT_M2_), float(min_cells) * float(cell_m) ** 2)


def describe(z, cell_m: float, *, search_m: float = 300.0, flat_deg: float = 1.0,
             a_crit_m2: float | None = None, repose_deg: float | None = None,
             skip_flow: bool = False) -> dict:
    """Run every metric on one field, sharing a single fill/route/accumulate pass.

    Returns a flat dict of scalars, with a ``cell_m`` key so the result can be
    resolution-checked like any other. ``skip_flow`` drops the four hydrological metrics,
    which are ~90% of the cost, for the cases where only the surface statistics are
    wanted.

    ``a_crit_m2`` defaults to `default_a_crit_m2` for the grid rather than to the bake's
    fixed 10^4 m^2, because at a 30 m cell that constant makes every cell a channel.
    """
    from .slope import REPOSE_DEG

    a_crit = default_a_crit_m2(cell_m) if a_crit_m2 is None else float(a_crit_m2)
    repose = REPOSE_DEG if repose_deg is None else float(repose_deg)

    out: dict = {"cell_m": float(cell_m)}
    s = slope_statistics(z, cell_m, repose_deg=repose)
    out.update(s.to_dict())
    cs = curvature_statistics(z, cell_m)
    out.update(cs.to_dict())
    # The realism signal is the SIZE of the asymmetry, not its direction: real scenes
    # run 0.01-0.15 either way and Gaussian fields sit under 0.003. See `curvature`.
    out["curvature_asymmetry"] = abs(cs.laplacian_quantile_skew)
    out.update(hypsometry(z, cell_m).to_dict())

    g = geomorphon_histogram(z, cell_m, search_m=search_m, flat_deg=flat_deg)
    out.update(g.to_dict())
    out["frac_ridge_peak"] = g.frac("ridge", "peak")
    out["frac_valley_pit"] = g.frac("valley", "pit")
    out["frac_hollow_footslope"] = g.frac("hollow", "footslope")
    # The flatness threshold is the parameter that decides what "flat" means; a plain
    # only reads as flat when the threshold is above its own regional gradient. Reported
    # at 3x as well so the table shows that dependence instead of hiding it.
    g3 = geomorphon_histogram(z, cell_m, search_m=search_m, flat_deg=3.0 * flat_deg)
    out["frac_flat_coarse_thresh"] = g3.fractions["flat"]
    out["frac_slope_coarse_thresh"] = g3.fractions["slope"]
    out["geomorphon_flat_deg_coarse"] = 3.0 * flat_deg

    vg = variogram(z, cell_m)
    out["hurst_overall"] = vg.hurst_overall
    out["hurst_overall_r2"] = vg.hurst_overall_r2
    out["variogram_max_kink"] = vg.max_kink
    out["variogram_kink_lag_m"] = vg.kink_lag_m
    out["variogram_range_m"] = vg.range_m
    out["variogram_anisotropy"] = float(np.nanmean(vg.anisotropy))
    out["tier_continuity"] = tier_continuity(vg)

    if not skip_flow:
        ctx = flow_context(z, cell_m)
        out.update(pit_statistics(ctx=ctx).to_dict())
        try:
            sa = slope_area_relation(ctx=ctx)
            d = sa.to_dict()
            d["slope_area_r2"] = d.pop("r2")
            out.update(d)
        except ValueError as exc:
            out["theta"] = float("nan")
            out["slope_area_r2"] = float("nan")
            out["channel_head_area_m2"] = float("nan")
            out["slope_area_error"] = str(exc)
        dd = drainage_density(ctx=ctx, a_crit_m2=a_crit)
        out.update(dd.to_dict())
        hk = hacks_law(ctx=ctx, a_crit_m2=a_crit)
        hd = hk.to_dict()
        out["hack_h"] = hd.pop("h")
        out["hack_r2"] = hd.pop("r2")
        out["hack_h_stderr"] = hd.pop("h_stderr")
        out["hack_n_bins_fit"] = hd["n_bins_fit"]
    return out


def _fmt(v, unit: str) -> str:
    if v is None:
        return "-"
    if isinstance(v, bool):
        return "yes" if v else "no"
    if isinstance(v, (int, np.integer)) and not isinstance(v, bool):
        return f"{int(v):,}"
    try:
        x = float(v)
    except (TypeError, ValueError):
        return str(v)
    if math.isnan(x):
        return "n/a"
    if unit in ("m2", "1/km2") or abs(x) >= 1e4 or (0 < abs(x) < 1e-3):
        return f"{x:.3g}"
    return f"{x:.3f}"


def discrimination_table(cases: dict, metrics=HEADLINE_METRICS,
                         *, title: str = "") -> str:
    """Render metric x case as a GitHub-flavoured markdown table.

    ``cases`` maps a case name to a `describe` dict. Every case must have been measured
    at the same ``cell_m``; this raises `ResolutionMismatch` otherwise, which is the
    whole reason the function takes the dicts rather than a pre-formatted table.
    """
    names = list(cases)
    if not names:
        raise ValueError("no cases")
    require_same_resolution([cases[n] for n in names], what="table columns")

    head = ["metric", "unit"] + names
    rows = [head, ["---"] * len(head)]
    for key, unit, _doc in metrics:
        rows.append([key, unit] + [_fmt(cases[n].get(key), unit) for n in names])
    widths = [max(len(r[i]) for r in rows) for i in range(len(head))]
    lines = []
    if title:
        lines += [f"### {title}", ""]
    for ri, r in enumerate(rows):
        if ri == 1:
            lines.append("|" + "|".join("-" * (widths[i] + 2) for i in range(len(head))) + "|")
        else:
            lines.append("| " + " | ".join(r[i].ljust(widths[i]) for i in range(len(head))) + " |")
    lines += ["", "Legend:"]
    for key, unit, doc in metrics:
        lines.append(f"- `{key}` ({unit}): {doc}")
    return "\n".join(lines)


@dataclass(frozen=True)
class Separation:
    """One metric's between-class spread against its within-class noise."""

    metric: str
    between_spread: float
    within_noise: float
    ratio: float
    values: dict

    def verdict(self, threshold: float = 3.0) -> str:
        if not math.isfinite(self.ratio):
            return "n/a"
        return "separates" if self.ratio >= threshold else "no"


def separation(cases: dict, replicates: dict, metrics=HEADLINE_METRICS,
               *, threshold: float = 3.0, subset=None) -> list:
    """Between-class spread over within-class noise, per metric.

    ``cases`` maps case name -> `describe` dict (one per class). ``replicates`` maps the
    same case names -> a list of `describe` dicts measured on sub-windows of the same
    field. The within-class noise is the pooled standard deviation of the replicates,
    the between-class spread is the standard deviation of the case values, and the ratio
    is the number of within-class sigmas the classes are apart.

    A metric with a ratio below ``threshold`` did not earn its place in a gate: whatever
    it is measuring, it is measuring it less precisely than the classes differ.

    ``subset`` restricts the comparison to some of the cases, which is how the two
    different questions get asked separately: pass the real terrain classes to ask "does
    this metric tell the Alps from Kansas", and use `paired_contrast` to ask the quite
    different question "does it tell real terrain from a good fake". A metric can pass
    one and fail the other, and several do.
    """
    out = []
    names = list(cases) if subset is None else [n for n in subset]
    cases = {n: cases[n] for n in names}
    replicates = {n: replicates[n] for n in names if n in replicates}
    for key, _unit, _doc in metrics:
        vals = {n: cases[n].get(key) for n in cases}
        arr = np.array([np.nan if v is None else float(v) for v in vals.values()],
                       dtype=float)
        arr = arr[np.isfinite(arr)]
        between = float(arr.std(ddof=1)) if arr.size >= 2 else float("nan")
        pooled = []
        for n, reps in replicates.items():
            r = np.array([float(x.get(key, np.nan)) for x in reps], dtype=float)
            r = r[np.isfinite(r)]
            if r.size >= 2:
                pooled.append(r.var(ddof=1))
        within = float(np.sqrt(np.mean(pooled))) if pooled else float("nan")
        ratio = between / within if (within and np.isfinite(within) and within > 0) else float("nan")
        out.append(Separation(key, between, within, ratio, vals))
    return out


def paired_contrast(cases: dict, pairs, replicates: dict | None = None,
                    metrics=HEADLINE_METRICS, *, threshold: float = 3.0) -> list:
    """Does each metric see through a spectrum-matched fake?

    ``pairs`` is a sequence of ``(real_name, fake_name)``. For each metric this reports
    the value on each side of every pair and the **worst-case** ratio of the gap to the
    within-class noise, so a metric only passes if it separates *every* pair. That is
    the right test here: a realism gate that catches the Alps' surrogate but not
    Kentucky's is a gate that can be walked through.

    ``fold`` is the size of the gap in units of the value itself -- ``|a - b| /
    max(|a|, |b|)`` -- which is the number to read when the within-class noise is not
    available or is dominated by genuine heterogeneity inside the scene.
    """
    replicates = replicates or {}
    out = []
    for key, unit, _doc in metrics:
        rows, ratios, folds = [], [], []
        for real, fake in pairs:
            a = _as_float(cases.get(real, {}).get(key))
            b = _as_float(cases.get(fake, {}).get(key))
            rows.append((real, fake, a, b))
            if not (math.isfinite(a) and math.isfinite(b)):
                ratios.append(float("nan"))
                folds.append(float("nan"))
                continue
            pooled = []
            for n in (real, fake):
                r = np.array([_as_float(x.get(key)) for x in replicates.get(n, [])])
                r = r[np.isfinite(r)]
                if r.size >= 2:
                    pooled.append(r.var(ddof=1))
            within = float(np.sqrt(np.mean(pooled))) if pooled else float("nan")
            ratios.append(abs(a - b) / within if within > 0 else float("nan"))
            scale = max(abs(a), abs(b))
            folds.append(abs(a - b) / scale if scale > 0 else float("nan"))
        worst = min([r for r in ratios if math.isfinite(r)], default=float("nan"))
        worst_fold = min([f for f in folds if math.isfinite(f)], default=float("nan"))
        out.append({"metric": key, "unit": unit, "rows": rows,
                    "worst_ratio": worst, "worst_fold": worst_fold,
                    "verdict": _verdict(worst, worst_fold, threshold)})
    return out


def _verdict(ratio: float, fold: float, threshold: float) -> str:
    """Three levels, because neither criterion is honest on its own.

    ``gap/noise`` under-rates a **signed** metric whose sign is a property of the terrain
    class: curvature skew is +0.068 on the Alps and -0.041 on the Cumberland Plateau, so
    the quadrant-to-quadrant spread inside one scene is as large as the metric itself and
    the ratio lands near 3 even though the surrogate reads 0.0004 -- a hundred times
    smaller. ``fold`` catches exactly that, and in turn over-rates anything sitting near
    zero: geomorphon ``frac_flat`` is 0.006 against 0.000, a fold of 1.0 and a difference
    of nothing. So a large fold only counts when the gap also clears its own noise, and
    a metric that clears the noise without a large fold is reported as **marginal**
    rather than promoted or dismissed.
    """
    r_ok = math.isfinite(ratio)
    f_ok = math.isfinite(fold)
    if r_ok and (ratio >= threshold or (ratio >= 1.5 and f_ok and fold >= 0.5)):
        return "sees through"
    if r_ok and ratio >= 2.0:
        return "marginal"
    return "blind"


def _as_float(v) -> float:
    if v is None or isinstance(v, str):
        return float("nan")
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    try:
        return float(v)
    except (TypeError, ValueError):
        return float("nan")


def paired_contrast_table(rows, *, title: str = "") -> str:
    """Render `paired_contrast` as markdown, real and fake side by side."""
    if not rows:
        return ""
    pair_names = [f"{a} / {b}" for a, b, _x, _y in rows[0]["rows"]]
    head = ["metric"] + [n for p in pair_names for n in (f"{p} real", f"{p} fake")] \
        + ["worst gap/noise", "worst fold", "verdict"]
    table = [head, ["---"] * len(head)]
    for r in rows:
        cells = [r["metric"]]
        for _a, _b, x, y in r["rows"]:
            cells += [_fmt(x, r["unit"]), _fmt(y, r["unit"])]
        cells += [_fmt(r["worst_ratio"], "-"), _fmt(r["worst_fold"], "-"), r["verdict"]]
        table.append(cells)
    widths = [max(len(t[i]) for t in table) for i in range(len(head))]
    lines = ([f"### {title}", ""] if title else [])
    for ri, t in enumerate(table):
        if ri == 1:
            lines.append("|" + "|".join("-" * (widths[i] + 2)
                                        for i in range(len(head))) + "|")
        else:
            lines.append("| " + " | ".join(t[i].ljust(widths[i])
                                           for i in range(len(head))) + " |")
    return "\n".join(lines)


def separation_table(seps, *, threshold: float = 3.0, title: str = "") -> str:
    """Render `separation` output as markdown."""
    rows = [["metric", "between (sd)", "within (sd)", "ratio", "verdict"],
            ["---"] * 5]
    for s in seps:
        rows.append([s.metric, _fmt(s.between_spread, "-"), _fmt(s.within_noise, "-"),
                     _fmt(s.ratio, "-"), s.verdict(threshold)])
    widths = [max(len(r[i]) for r in rows) for i in range(5)]
    lines = ([f"### {title}", ""] if title else [])
    for ri, r in enumerate(rows):
        if ri == 1:
            lines.append("|" + "|".join("-" * (widths[i] + 2) for i in range(5)) + "|")
        else:
            lines.append("| " + " | ".join(r[i].ljust(widths[i]) for i in range(5)) + " |")
    return "\n".join(lines)


# ----------------------------------------------------------------------- plotting
def _plt():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    return plt


def plot_slope_area(results: dict, path: str, *, title: str = "slope-area") -> str:
    """Log-log slope-area curves for several cases, with each fitted fluvial limb."""
    plt = _plt()
    fig, ax = plt.subplots(figsize=(7.5, 5.5), dpi=140)
    for name, sa in results.items():
        ok = sa.count >= 30
        ax.plot(sa.area_m2[ok], sa.slope_median[ok], lw=1.6, label=f"{name} (θ={sa.theta:.3f})")
        if np.isfinite(sa.theta) and sa.in_fit.any():
            a = sa.area_m2[sa.in_fit]
            ax.plot(a, sa.ks * a ** (-sa.theta), ls="--", lw=1.0, color="k", alpha=0.45)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("drainage area A (m²)")
    ax.set_ylabel("D8 slope S (median per bin)")
    ax.set_title(title)
    ax.grid(alpha=0.25, which="both")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    return path


def plot_variogram(results: dict, path: str, *, boundaries=(30.0, 1.875),
                   title: str = "variogram") -> str:
    """Log-log variograms with the tier boundaries marked."""
    plt = _plt()
    fig, ax = plt.subplots(figsize=(7.5, 5.5), dpi=140)
    for name, vg in results.items():
        ax.plot(vg.lag_m, vg.gamma_m2, lw=1.6, label=f"{name} (H={vg.hurst_overall:.2f})")
    for b in boundaries:
        ax.axvline(b, color="0.4", ls=":", lw=1.0)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("lag h (m)")
    ax.set_ylabel("γ(h) (m²)")
    ax.set_title(title)
    ax.grid(alpha=0.25, which="both")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    return path


def plot_hypsometry(results: dict, path: str, *, title: str = "hypsometry") -> str:
    """Hypsometric curves."""
    plt = _plt()
    fig, ax = plt.subplots(figsize=(6.0, 5.5), dpi=140)
    for name, hy in results.items():
        ax.plot(hy.relative_area, hy.relative_height, lw=1.6,
                label=f"{name} (HI={hy.hypsometric_integral:.3f})")
    ax.set_xlabel("relative area a/A")
    ax.set_ylabel("relative height h/H")
    ax.set_title(title)
    ax.grid(alpha=0.25)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    return path
