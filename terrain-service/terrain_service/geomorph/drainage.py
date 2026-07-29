"""Pit statistics, drainage density, and Hack's law.

Three metrics that share one flow pass, and they are deliberately in one module because
the first of them is the licence to believe the other two.

**`pit_statistics` is the verdict metric of this whole package.** Everything else here
is measured on a depression-filled surface, and Barnes' epsilon priority-flood will fill
*anything* -- hand it white noise and it hands back a connected, dendritic, entirely
plausible drainage network, because inventing a downhill path is precisely its job.
What the fill cannot hide is **how much work it had to do**. A landscape that water
actually carved is already almost everywhere drained: the fill raises a few percent of
cells by decimetres, mostly real closed basins and DEM noise. A field that was never
drained is a carpet of local minima: the fill raises a third of the cells by metres.
That difference survives into no other statistic in this package, which is why the raw
pit numbers carry the verdict and every post-fill number is descriptive.

**Drainage density** is total channel length per unit area, the classic measure of how
finely dissected a landscape is -- 2-5 km/km^2 for humid soil-mantled uplands, under
1 for plains and deserts with no integrated network, and 50-500 in badlands. It depends
entirely on where you put the channel head, so ``a_crit_m2`` is explicit and must be
held fixed across any comparison. It is also bounded above by the grid: with
``a_crit_m2`` at a few cell areas every cell is a channel and the density saturates at
~1/cell_m, which is a statement about the raster and not the terrain.

**Drainage density did not earn a place as a realism check, and the module says so.**
Measured at 30.87 m on 1024^2 windows with the channel head at 100 cells: the five real
scenes give 2.4-10.7 km/km^2 and the synthetic controls give 5.6-6.5, sitting squarely
inside the real range. It was tried at four thresholds spanning two decades of area and
the overlap never went away. That is not surprising in hindsight -- once the epsilon fill
has invented a network, "how much network is there above a given area" is close to a
geometric identity -- but it is the kind of thing that has to be measured rather than
assumed. It remains a good **descriptor** of dissection at a stated ``a_crit_m2``, and
between real classes it does separate (ratio 3.1 against the within-scene spread).

**Hack's law**, ``L = c * A^h``, relates a basin's mainstem length to its area. Real
networks give h ~ 0.57, above the 0.5 that geometric self-similarity would give,
because larger basins are systematically more elongate. It is measured here along the
network itself -- every channel cell contributes its own ``(A, L)`` pair, with L the
longest upstream flow path reaching it -- rather than over a set of hand-picked basins,
which is the standard single-network method and needs no basin delineation.

**Hack's law was the surprise of the validation sweep.** It was included with low
expectations -- an exponent fitted along a single network on a 30 km window has every
reason to be noisy -- and it turned out to be one of the three metrics that sees through
a spectrum-matched surrogate. Measured: the five real scenes give h = 0.475-0.557, sitting
right on the literature's 0.57, while the surrogates of two of those same scenes give
0.161 and 0.287. The fill invents a network with the right *area* statistics and the
wrong *shape*: its basins are round, because nothing organised them into long ones. It
needs a large window to say that -- on quarter-sized windows the within-scene spread
swamps the difference -- so treat ``n_bins_fit`` and ``r2`` as preconditions and do not
quote an h from anything under ~1000 cells on a side.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ._grid import check_cell_m, interior_mask, weighted_loglog_fit
from .flow_context import FlowContext, resolve_context

__all__ = [
    "PitStats",
    "DrainageDensity",
    "HackLaw",
    "pit_statistics",
    "drainage_density",
    "hacks_law",
    "A_CRIT_M2",
    "HACK_EXPONENT_EARTH",
]

#: `bake.incise.A_CRIT_M2`, the channel-initiation area the bake's own incision gate
#: uses. Held here so drainage density is measured against the same channel definition
#: the bake carves to.
A_CRIT_M2 = 1.0e4

#: Hack (1957) and the large body of work since: real drainage networks give an
#: exponent near this, above the 0.5 of strict geometric similarity.
HACK_EXPONENT_EARTH = 0.57

_R2 = 1.4142135623730951


@dataclass(frozen=True)
class PitStats:
    """How much work the depression fill had to do. Measured on the **raw** surface.

    ``pit_density_per_km2`` is the headline: interior cells with no strictly lower
    8-neighbour, per km^2. It is resolution-dependent (a finer grid resolves more real
    micro-depressions *and* more noise), so compare only at matched ``cell_m``.

    ``filled_frac`` and ``fill_depth_*`` describe the same thing from the other side --
    how much of the domain the fill had to raise, and by how much.
    """

    cell_m: float
    n_cells: int
    raw_pits: int
    pit_density_per_km2: float
    filled_frac: float
    fill_depth_mean_m: float
    fill_depth_p50_filled_m: float   # median over the cells that were raised at all
    fill_depth_p99_m: float
    fill_depth_max_m: float
    #: Total volume the fill added, per unit area, in metres. The single number that
    #: combines "how much of the map" with "by how much", and the strongest separator
    #: measured here between real terrain and spectrum-matched noise.
    fill_volume_per_area_m: float
    #: Largest MFD contributing area anywhere in the domain, m^2. On a fragmented field
    #: this stays near the window area only because the fill integrated it.
    largest_catchment_m2: float

    def to_dict(self) -> dict:
        return {k: getattr(self, k) for k in self.__dataclass_fields__}


@dataclass(frozen=True)
class DrainageDensity:
    """Channel length per unit area at a stated channel-initiation area."""

    cell_m: float
    a_crit_m2: float
    channel_cells: int
    channel_frac: float
    channel_length_m: float
    domain_area_m2: float
    #: Drainage density in 1/m ...
    dd_per_m: float
    #: ... and in the units the literature uses.
    dd_km_per_km2: float
    #: 1/cell_m -- the density a fully channelised raster would report. If
    #: ``dd_per_m`` is within a factor of ~2 of this, the measurement is describing the
    #: grid, not the terrain.
    dd_saturation_per_m: float
    saturated: bool

    def to_dict(self) -> dict:
        return {k: getattr(self, k) for k in self.__dataclass_fields__}


@dataclass(frozen=True)
class HackLaw:
    """``L = c * A^h`` fitted along the network. ``h`` near 0.57 on real Earth."""

    cell_m: float
    h: float
    h_stderr: float
    r2: float
    c: float
    a_crit_m2: float
    n_bins_fit: int
    n_cells: int
    earth_h: float
    h_minus_earth: float
    area_m2: np.ndarray
    length_median_m: np.ndarray
    count: np.ndarray

    def to_dict(self) -> dict:
        return {
            k: getattr(self, k)
            for k in ("cell_m", "h", "h_stderr", "r2", "c", "a_crit_m2",
                      "n_bins_fit", "n_cells", "earth_h", "h_minus_earth")
        }


def pit_statistics(z=None, cell_m: float = None, *,
                   ctx: FlowContext | None = None) -> PitStats:
    """Raw pit counts and fill effort. **Read this before believing anything else.**"""
    c = resolve_context(z, cell_m, ctx)
    d = c.fill_depth
    raised = d > 0.0
    n_raised = int(np.count_nonzero(raised))
    area_km2 = c.z_raw.size * c.cell_area_m2 / 1e6
    return PitStats(
        cell_m=check_cell_m(c.cell_m),
        n_cells=int(c.z_raw.size),
        raw_pits=int(c.raw_pits),
        pit_density_per_km2=float(c.raw_pits / area_km2),
        filled_frac=float(n_raised / d.size),
        fill_depth_mean_m=float(d.mean()),
        fill_depth_p50_filled_m=float(np.median(d[raised])) if n_raised else 0.0,
        fill_depth_p99_m=float(np.quantile(d, 0.99)),
        fill_depth_max_m=float(d.max()),
        fill_volume_per_area_m=float(d.mean()),
        largest_catchment_m2=float(c.area_m2.max()),
    )


def drainage_density(z=None, cell_m: float = None, *, ctx: FlowContext | None = None,
                     a_crit_m2: float = A_CRIT_M2,
                     border_margin_cells: int | None = None) -> DrainageDensity:
    """Channel length per unit area, channels being cells with ``A > a_crit_m2``.

    Length is summed over the D8 step each channel cell takes -- ``cell_m`` for a
    cardinal step and ``sqrt(2) * cell_m`` for a diagonal one -- rather than counted as
    one cell each. The naive count understates a diagonal channel by 29%, and terrain
    with a preferred diagonal grain (which is exactly what a D8-routed synthetic field
    has) would otherwise measure a systematically lower density than the same terrain
    rotated 45 degrees.

    ``a_crit_m2`` must be many times ``cell_m**2`` or the answer describes the raster;
    ``saturated`` flags the case where it does.
    """
    c = resolve_context(z, cell_m, ctx)
    cell = check_cell_m(c.cell_m)
    a_crit = float(a_crit_m2)
    if not np.isfinite(a_crit) or a_crit <= 0.0:
        raise ValueError(f"a_crit_m2 must be finite and > 0, got {a_crit_m2!r}")
    h, w = c.shape
    if border_margin_cells is None:
        border_margin_cells = max(4, int(round(0.02 * min(h, w))))
    mask = interior_mask((h, w), int(border_margin_cells))

    channel = mask & (c.area_m2 > a_crit) & (c.receiver >= 0)
    n_chan = int(np.count_nonzero(channel))
    ci = np.flatnonzero(channel.ravel())
    rec = c.receiver.ravel()[ci]
    same_row = (ci // w) == (rec // w)
    same_col = (ci % w) == (rec % w)
    diag = ~(same_row | same_col)
    length = float(cell * (n_chan - diag.sum()) + cell * _R2 * diag.sum())

    area_m2 = float(np.count_nonzero(mask) * c.cell_area_m2)
    dd = length / area_m2 if area_m2 > 0 else float("nan")
    sat = 1.0 / cell
    return DrainageDensity(
        cell_m=cell,
        a_crit_m2=a_crit,
        channel_cells=n_chan,
        channel_frac=float(n_chan / max(1, np.count_nonzero(mask))),
        channel_length_m=length,
        domain_area_m2=area_m2,
        dd_per_m=float(dd),
        dd_km_per_km2=float(dd * 1000.0),
        dd_saturation_per_m=float(sat),
        saturated=bool(dd > 0.5 * sat),
    )


def hacks_law(z=None, cell_m: float = None, *, ctx: FlowContext | None = None,
              a_crit_m2: float = A_CRIT_M2, bins_per_decade: int = 8,
              min_bin_count: int = 30,
              border_margin_cells: int | None = None) -> HackLaw:
    """Fit ``L = c * A^h`` over channel cells, binned by log area.

    ``L`` is the longest upstream D8 flow path reaching the cell, which is the mainstem
    length of the catchment draining through it. Cells below ``a_crit_m2`` are excluded:
    on a hillslope L is set by the grid (a cell one step from the divide has L = cell_m)
    and including them fits the raster rather than the network.

    The estimate is only as long as the window: a 30 km window cannot contain a basin
    whose mainstem is 40 km, so the largest-area bins are censored and h is biased low
    there. `border_margin_cells` removes the worst of it; a window under ~500 cells on a
    side does not have enough decades of area above ``a_crit_m2`` to fit at all and will
    return NaN with ``n_bins_fit`` below 3.
    """
    c = resolve_context(z, cell_m, ctx)
    cell = check_cell_m(c.cell_m)
    h, w = c.shape
    if border_margin_cells is None:
        border_margin_cells = max(4, int(round(0.02 * min(h, w))))
    mask = interior_mask((h, w), int(border_margin_cells))
    mask &= c.area_m2 > float(a_crit_m2)

    L = c.flow_length_m()[mask]
    A = c.area_m2[mask]
    ok = (L > 0.0) & (A > 0.0)
    L, A = L[ok], A[ok]
    n_cells = int(A.size)

    nanres = HackLaw(
        cell_m=cell, h=float("nan"), h_stderr=float("nan"), r2=float("nan"),
        c=float("nan"), a_crit_m2=float(a_crit_m2), n_bins_fit=0, n_cells=n_cells,
        earth_h=HACK_EXPONENT_EARTH, h_minus_earth=float("nan"),
        area_m2=np.empty(0), length_median_m=np.empty(0), count=np.empty(0, np.int64),
    )
    if n_cells < 100:
        return nanres

    la = np.log10(A)
    lo = np.floor(la.min() * bins_per_decade) / bins_per_decade
    hi = np.ceil(la.max() * bins_per_decade) / bins_per_decade
    nb = max(1, int(round((hi - lo) * bins_per_decade)))
    edges = np.linspace(lo, hi, nb + 1)
    idx = np.clip(np.digitize(la, edges) - 1, 0, nb - 1)
    centres = 10.0 ** (0.5 * (edges[:-1] + edges[1:]))
    med = np.full(nb, np.nan)
    cnt = np.zeros(nb, dtype=np.int64)
    order = np.argsort(idx, kind="stable")
    bounds = np.searchsorted(idx[order], np.arange(nb + 1))
    for b in range(nb):
        v = L[order[bounds[b]:bounds[b + 1]]]
        cnt[b] = v.size
        if v.size:
            med[b] = np.median(v)

    use = cnt >= int(min_bin_count)
    if use.sum() < 3:
        return HackLaw(**{**nanres.__dict__, "area_m2": centres,
                          "length_median_m": med, "count": cnt})
    slope, intercept, r2, stderr, _rmse = weighted_loglog_fit(
        centres[use], med[use], cnt[use].astype(np.float64))
    return HackLaw(
        cell_m=cell,
        h=float(slope),
        h_stderr=float(stderr),
        r2=float(r2),
        c=float(10.0 ** intercept) if np.isfinite(intercept) else float("nan"),
        a_crit_m2=float(a_crit_m2),
        n_bins_fit=int(use.sum()),
        n_cells=n_cells,
        earth_h=HACK_EXPONENT_EARTH,
        h_minus_earth=float(slope - HACK_EXPONENT_EARTH),
        area_m2=centres,
        length_median_m=med,
        count=cnt,
    )
