"""One depression-fill + routing pass, shared by every hydrological metric.

`slope_area_relation`, `drainage_density`, `hacks_law` and `pit_statistics` all need the
same three expensive things -- an epsilon-filled surface, D8 receivers, and an MFD
accumulation -- and at 1024^2 that is ~2 s and at 4096^2 ~40 s. Computing it once and
passing a `FlowContext` around is the difference between a validation sweep that runs in
minutes and one that runs in an hour.

**The routing itself is `terrain_service.bake.flow`, unmodified and unreimplemented.**
That module carries a fix this package would otherwise have had to rediscover: a plain
priority-flood leaves every filled pit as a level lake, on which both D8 and MFD simply
terminate, which stranded 69% of the domain's area on a real tile. We call
`fill_depressions` with its default (epsilon) behaviour, which is the variant that
routes.

**The epsilon fill will manufacture a plausible drainage network on a Gaussian field.**
This is the single most important caveat in this package and it is why `pit_statistics`
exists. Run the fill on noise and you get a connected, dendritic, entirely believable
network with a respectable slope-area relation, because priority-flood's job is
precisely to *invent* a downhill path where none exists. So:

* statistics measured on the **raw** surface -- how much of the domain the fill had to
  raise, and by how much -- carry the verdict;
* statistics measured on the **filled** surface are descriptive, and are only evidence
  about realism once the raw fill statistics have shown the network was already there.

Measured on 1024^2 windows at 30.87 m, mean fill depth over the whole domain::

    five real GLO-30 scenes                             0.07 - 0.97 m
    fBm, H = 0.75                                      38.6 m
    multi-octave value noise                           14.6 m
    spectrum-matched surrogate of the alpine scene     64.8 m
    spectrum-matched surrogate of the fluvial scene    19.9 m

A 15x gap with no overlap, on fields whose variograms agree with the real ones to 15%.

**It is the fill *volume* that carries this, not the pit count.** Counting raw local
minima is the obvious statistic and it does not work: the alpine surrogate has 4.4 pits
per km^2 against the real alpine scene's 7.7 -- *fewer*. A Gaussian field's minima are
few and enormous, each a broad basin with no outlet; a real landscape's are many and
shallow, because it drains. `PitStats.pit_density_per_km2` is therefore reported for
description and `PitStats.fill_volume_per_area_m` is the number to gate on.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from ._grid import as_field, check_cell_m

__all__ = ["FlowContext", "flow_context", "MFD_EXPONENT"]

#: The bake's own MFD exponent (`bake.pipeline.BakeConstants.mfd_p`). Held here so the
#: measurement uses the same routing the bake does; if the bake changes it, so should
#: this, and a comparison across the change is not a comparison.
MFD_EXPONENT = 1.1

_R2 = 1.4142135623730951


@dataclass(frozen=True)
class FlowContext:
    """Everything the hydrological metrics share, computed once.

    Attributes, all shaped like the input except where noted:

    ``cell_m``      cell size in metres.
    ``z_raw``       the input surface, float64, metres.
    ``z_filled``    epsilon-filled surface, metres.
    ``fill_depth``  ``z_filled - z_raw``, metres, >= 0.
    ``receiver``    D8 steepest-descent receiver as a *flat* index, -1 where none.
    ``slope_d8``    the matching drop over true distance, dimensionless. This is the
                    ``S`` of the bake's ``K * A^m * S^n``, which is why the slope-area
                    relation uses it rather than a gradient magnitude.
    ``area_m2``     MFD contributing area in m^2, >= ``cell_m**2`` everywhere.
    ``raw_pits``    number of cells on the **raw** surface with no strictly lower
                    neighbour and not on the domain border. The verdict statistic.
    """

    cell_m: float
    z_raw: np.ndarray
    z_filled: np.ndarray
    fill_depth: np.ndarray
    receiver: np.ndarray
    slope_d8: np.ndarray
    area_m2: np.ndarray
    raw_pits: int
    mfd_p: float = MFD_EXPONENT
    _cache: dict = field(default_factory=dict, repr=False, compare=False)

    @property
    def shape(self):
        return self.z_raw.shape

    @property
    def cell_area_m2(self) -> float:
        return self.cell_m * self.cell_m

    def flow_length_m(self) -> np.ndarray:
        """Longest upstream D8 flow-path distance reaching each cell, in metres.

        Zero at every cell with no upslope contributor (a divide or a local high), and
        the mainstem length of the catchment at its outlet -- which is exactly the ``L``
        of Hack's law. Computed lazily and cached, because only `hacks_law` needs it.
        """
        out = self._cache.get("flow_length_m")
        if out is None:
            out = _flow_length(self.z_filled, self.receiver, self.cell_m)
            self._cache["flow_length_m"] = out
        return out


def _raw_pit_count(z: np.ndarray) -> int:
    """Interior cells with no strictly lower 8-neighbour, on the surface as given.

    Border cells are excluded because a border cell legitimately has nowhere to send
    water inside the domain, so counting them would make the statistic depend on the
    window size rather than on the terrain.
    """
    lower = np.zeros(z.shape, dtype=bool)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dy == 0 and dx == 0:
                continue
            sl_c = (slice(max(0, -dy), z.shape[0] - max(0, dy)),
                    slice(max(0, -dx), z.shape[1] - max(0, dx)))
            sl_n = (slice(max(0, dy), z.shape[0] - max(0, -dy)),
                    slice(max(0, dx), z.shape[1] - max(0, -dx)))
            lower[sl_c] |= z[sl_n] < z[sl_c]
    interior = np.zeros(z.shape, dtype=bool)
    interior[1:-1, 1:-1] = True
    return int(np.count_nonzero(interior & ~lower))


def _flow_length(z_filled: np.ndarray, receiver: np.ndarray, cell_m: float) -> np.ndarray:
    """Longest upstream flow distance, by one ascending-elevation sweep of the D8 tree.

    Sequential by nature (a cell must be final before it pays out to its receiver), so
    it is compiled when numba is available and falls back to a plain Python loop when it
    is not. The fallback is ~1.5 us/cell, i.e. fine for the 128^2 grids the tests use
    and hopeless for a real tile, which is the same trade `bake.flow` makes.
    """
    h, w = z_filled.shape
    order = np.argsort(z_filled, axis=None)
    rec = receiver.ravel()
    length = np.zeros(z_filled.size, dtype=np.float64)
    _flow_length_sweep(order, rec, length, np.int64(w), float(cell_m))
    return length.reshape(h, w)


def _flow_length_sweep_py(order, rec, length, w, cell_m):
    # DESCENDING elevation. `order` is ascending, so it is walked backwards -- a cell
    # must have received from every one of its (higher) donors before it pays out to its
    # own receiver. Sweeping it forwards instead is silently wrong rather than loud: it
    # produces a length field of exactly one step everywhere, which fits Hack's law with
    # an exponent of 0.00 and an r2 of 0.
    diag = cell_m * _R2
    for k in range(order.size - 1, -1, -1):
        c = order[k]
        r = rec[c]
        if r < 0:
            continue
        step = cell_m if (c // w == r // w or c % w == r % w) else diag
        cand = length[c] + step
        if cand > length[r]:
            length[r] = cand
    return length


_flow_length_sweep = _flow_length_sweep_py
_SWEEP_RESOLVED = False


def _try_compile() -> None:
    """Swap the sweep for a numba build on first use, if numba is importable.

    Mirrors `bake.flow._jit` deliberately: numba is a bake-pod dependency and absent
    from ``terrain-service/requirements.txt``, so importing this module must not need
    it. Compiling at module scope would take the whole CI job down instead of making
    one test slow.
    """
    global _flow_length_sweep, _SWEEP_RESOLVED
    if _SWEEP_RESOLVED:
        return
    _SWEEP_RESOLVED = True
    try:
        import numba
    except ImportError:  # pragma: no cover - the CI path
        return
    _flow_length_sweep = numba.njit(cache=True)(_flow_length_sweep_py)


def flow_context(z, cell_m: float, *, mfd_p: float = MFD_EXPONENT,
                 flat_eps: float | None = None) -> FlowContext:
    """Fill, route and accumulate once. See `FlowContext` for the fields.

    ``mfd_p`` is the MFD partition exponent and defaults to the bake's own 1.1. Do not
    change it for a comparison against numbers measured at the default: the contributing
    area field, and therefore the slope-area relation and the drainage density, both
    move with it.

    ``flat_eps`` is passed straight through to `bake.flow.fill_depressions`; ``None``
    means its default two-ULP epsilon, which is the only value that routes. Passing
    ``0.0`` reproduces the plain flat fill and is useful for exactly one thing --
    asserting a spill elevation -- and useless for everything this package measures.
    """
    from terrain_service.bake.flow import accumulate_mfd, d8_receivers, fill_depressions

    zz = as_field(z)
    cell = check_cell_m(cell_m)
    raw_pits = _raw_pit_count(zz)
    filled = fill_depressions(zz, flat_eps=flat_eps)
    rec, slope = d8_receivers(filled, cell)
    area = accumulate_mfd(filled, cell, p=float(mfd_p))
    _try_compile()
    return FlowContext(
        cell_m=cell,
        z_raw=zz,
        z_filled=np.asarray(filled, dtype=np.float64),
        fill_depth=np.asarray(filled, dtype=np.float64) - zz,
        receiver=np.asarray(rec),
        slope_d8=np.asarray(slope, dtype=np.float64),
        area_m2=np.asarray(area, dtype=np.float64),
        raw_pits=raw_pits,
        mfd_p=float(mfd_p),
    )


def resolve_context(z, cell_m, ctx: "FlowContext | None") -> FlowContext:
    """Accept either ``(z, cell_m)`` or a precomputed ``ctx``, and guard the cell size.

    With ``ctx`` given, ``z`` and ``cell_m`` are optional but are *checked* if supplied:
    passing a context built at one resolution together with a different ``cell_m`` is a
    `ResolutionMismatch`, not a silent rescale.
    """
    if ctx is None:
        if z is None or cell_m is None:
            raise ValueError("pass either (z, cell_m) or ctx=<FlowContext>")
        return flow_context(z, cell_m)
    if cell_m is None:
        return ctx
    cell = check_cell_m(cell_m)
    from ._grid import ResolutionMismatch

    if not np.isclose(ctx.cell_m, cell, rtol=1e-9, atol=0.0):
        raise ResolutionMismatch(
            f"flow context was built at {ctx.cell_m} m but cell_m={cell} was passed"
        )
    if z is not None and np.asarray(z).shape != ctx.shape:
        raise ValueError(
            f"z has shape {np.asarray(z).shape} but the flow context is {ctx.shape}"
        )
    return ctx
