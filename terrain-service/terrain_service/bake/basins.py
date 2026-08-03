"""Basin registry and the lake / playa water balance.

WHAT THIS IS FOR
----------------
``docs/watershed-system-plan.md`` §4.2-§4.3. The bake fills every depression
into rock (`flow.fill_depressions` at B2a, again at B4b) because *drainage
enforcement* is a contract the whole downstream cascade depends on. The
watershed plan re-opens the ones a player would notice, as a final step, and
ships a small per-tile table describing them. This module owns the two
decisions that step needs and nothing else:

1. **Which holes are basins.** Connected components of the depression mask on
   the FINAL surface with the B2a fill undone, filtered by depth and area.
2. **Whether a basin holds water.** A water balance, not a random roll: a
   depression is a lake when its catchment's runoff exceeds what the lake
   surface can evaporate, and a playa when it does not. Same hole, wet climate
   ⇒ lake, desert ⇒ salt pan — the "caused variety, not placed variety"
   principle, and the player can trace the inflow valley that explains it.

It deliberately does **not** import ``pipeline``: ``tools/lake_survey.py``
(the sizing instrument, work item 2) and the bake's own B5 step (work item 3)
are both callers, and the survey must be able to sweep thresholds without
re-running a bake. ``flow`` is the only bake import, for the fill.

WHY THE SPILL IS RECOMPUTED ON THE FINAL SURFACE
------------------------------------------------
``basin_depth`` is ``filled - carrier_plus_roughness`` at **B2a**, i.e. the
fill of the *pre-erosion* carrier. Between B2a and the shipped surface the
bake incises channels (which can cut the outlet gorge metres deeper through
the rim), relaxes thermally, and adds a meso band. So the B2a fill level is
NOT the level the shipped ground would pond to, and a table built from it
would float lakes above their own outlets. Every level in a `BasinRecord`
therefore comes from a second ``fill_depressions`` run on the re-opened final
surface, with ``flat_eps=0.0`` so a filled depression is exactly level at its
spill elevation and the spill can be read off the raster instead of inferred.

UNITS
-----
Elevations and depths in metres (float32/float64 rasters), areas in m^2,
climate in its WorldClim physical units (bio_1 degC, bio_12 mm/yr, bio_15 CV
percent), discharge and evaporation as depths in mm/yr or volumes in m^3/yr.
The record's ``*_mm`` fields are the only integers, and they are what the wire
format carries.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from . import flow

__all__ = [
    "KIND_NAMES",
    "KIND_DRY_PLAYA",
    "KIND_SALT_FLAT",
    "KIND_SEASONAL",
    "KIND_LAKE_TERMINAL",
    "KIND_LAKE_OVERFLOWING",
    "BasinFilter",
    "WaterBalance",
    "BasinRecord",
    "pet_mm_yr",
    "budyko_runoff_mm_yr",
    "aridity_index",
    "reopened_surface",
    "depression_components",
    "SEA_LEVEL_M",
    "BasinSurvey",
    "hypsometry",
    "equilibrium_level",
    "classify",
    "survey_basins",
]


# --------------------------------------------------------------------------- kinds

#: A depression that is dry: no standing water, no salt, just a bowl of rock.
KIND_DRY_PLAYA = 0
#: Endorheic and evaporating: inflow arrives and leaves as vapour, so solutes
#: stay. A salt pan, not a lake.
KIND_SALT_FLAT = 1
#: Holds water for part of the year. Precipitation seasonality (bio_15) is
#: what separates this from a dry playa at the same annual mean.
KIND_SEASONAL = 2
#: A permanent lake with no outlet -- the balance settles below the spill.
KIND_LAKE_TERMINAL = 3
#: A permanent lake that spills: the outlet channel carries the residual, and
#: it is already in the flow plane because routing ran on the filled surface.
KIND_LAKE_OVERFLOWING = 4

KIND_NAMES: tuple[str, ...] = (
    "dry_playa",
    "salt_flat",
    "seasonal",
    "lake_terminal",
    "lake_overflowing",
)


# --------------------------------------------------------------------------- tunables


#: Sea level in metres, the fourth copy of ``voxelcore/core.h``'s
#: ``kSeaLevelMm`` (the others are worldgen.ush, the bake's sea taper and
#: ``tools/world_map.py``). ``tests/test_sea_level_contract.py`` asserts they
#: agree. It is here because a depression whose SPILL is below sea level is
#: the sea floor, not a lake basin, and shipping it as a lake would put a
#: second water surface underneath the ocean.
SEA_LEVEL_M = 0.0


@dataclass(frozen=True)
class BasinFilter:
    """The registry filter: which depressions are worth a table row.

    Defaults are the plan's *intent* ("a basin a player would notice", order
    2 m deep / 50 m across) and are meant to be re-pinned from the work-item-2
    survey rather than trusted as-is. ``tools/lake_survey.py`` sweeps them.
    """

    #: Minimum depth of the deepest cell, metres. Below this a "basin" is a
    #: dimple the detail band would swallow anyway.
    min_depth_m: float = 2.0
    #: Minimum footprint at the spill level, m^2. 2500 m^2 is 50 m across.
    min_area_m2: float = 2500.0
    #: Refuse basins whose extent leaves the tile INTERIOR (§4.2.4). Such a
    #: basin is registered independently by each tile that sees it, from a
    #: different padded domain, and the two need not agree; it also has a
    #: catchment crossing the seam. v1 excludes them and COUNTS them, so the
    #: cost of the exclusion is a number rather than an assumption.
    exclude_spanning: bool = True
    #: Refuse depressions whose spill is at or below sea level. Those are sea
    #: floor: the ocean already covers them, and a "lake surface" there would
    #: be a second water plane under the first.
    require_above_sea: bool = True
    #: How near the padded border counts as "the boundary had a say".
    #:
    #: THIS REPLACES A DEAD TEST. ``pipeline.py``'s shipped
    #: ``basin_reaches_padded_border`` / ``padded_border_basin_frac`` stats sum
    #: the basin mask along the four padded EDGES -- and that sum is
    #: structurally ZERO for every tile ever baked. ``fill_depressions`` never
    #: raises a border cell (flow.py says so in its own docstring), so
    #: ``filled - fine`` is identically 0 there, so no depression can contain a
    #: border cell, so the flag can never fire. Verified on a synthetic hollow
    #: whose corner IS the domain corner: 0 basin cells, because a hollow that
    #: reaches the border drains out of the domain and is not a depression at
    #: all. The honest question is the near-miss one -- how much of the
    #: registry sits close enough to the edge that a wider apron could have
    #: found it a different outlet -- and that is what this margin measures.
    border_margin_px: int = 8


@dataclass(frozen=True)
class WaterBalance:
    """Constants of the lake rule (§4.3). Pinned by the work-item-2 survey.

    ``pet_*`` are the Langbein/Turc L(T) coefficients; ``budyko_n`` selects
    the Turc-Pike member of the Budyko family. The classification thresholds
    below them are what turn a continuous balance into the five ``kind``
    values the wire format carries.
    """

    #: L(T) = pet_a + pet_b*T + pet_c*T^3, mm/yr. Turc's potential-evaporation
    #: proxy, monotone in T for all real T (dL/dT = pet_b + 3*pet_c*T^2 > 0).
    pet_a: float = 300.0
    pet_b: float = 25.0
    pet_c: float = 0.05
    #: Floor, mm/yr. L(T) goes negative below about -11 degC, where the
    #: formula is out of its fitted range; a polar basin evaporates a little,
    #: not a negative amount.
    pet_floor_mm: float = 100.0
    #: Budyko exponent. n = 2 is Turc-Pike: E/P = 1/sqrt(1 + (P/PET)^2).
    budyko_n: float = 2.0
    #: A lake is "permanent" once the equilibrium level stands this deep over
    #: the basin floor. Below it the balance is satisfied by a damp floor.
    min_lake_depth_m: float = 0.5
    #: Aridity index (P/PET) below which a dry basin with real inflow is a
    #: salt flat rather than a plain dry bowl.
    salt_aridity: float = 0.35
    #: Precipitation CV (bio_15, percent) above which a dry basin that WOULD
    #: hold water on a wet-season inflow is classified seasonal.
    seasonal_cv_pct: float = 55.0


# --------------------------------------------------------------------------- climate


def pet_mm_yr(temp_c, wb: WaterBalance = WaterBalance()):
    """Potential evaporation from mean annual temperature, mm/yr.

    Turc's L(T) = 300 + 25T + 0.05T^3, the denominator of the Turc runoff
    formula and the "Langbein-class" monotone table §4.3 asks for. It is a
    published empirical relation, not a fit invented here -- which is the
    point: an invented curve has no defensible shape outside the range it was
    fitted on, and this world spans -40 to +40 degC.

    Floored at ``pet_floor_mm``; see that field.
    """
    t = np.asarray(temp_c, dtype=np.float64)
    l = wb.pet_a + wb.pet_b * t + wb.pet_c * t**3
    return np.maximum(l, wb.pet_floor_mm)


def budyko_runoff_mm_yr(precip_mm, pet_mm, wb: WaterBalance = WaterBalance()):
    """Runoff depth from precipitation and PET, mm/yr -- the Turc-Pike curve.

        E = P / (1 + (P/PET)^n)^(1/n)          Q = P - E

    The two limits are the reason to use a Budyko curve at all rather than
    ``max(P - PET, 0)``: as P/PET -> 0 (a desert) E -> P and Q -> 0, so an
    arid catchment yields nothing rather than a negative amount; as P/PET ->
    inf (a rainforest) E -> PET and Q -> P - PET, energy-limited. At P == PET
    it gives E/P = 0.71, which is where Earth's data sit.
    """
    p = np.asarray(precip_mm, dtype=np.float64)
    e0 = np.asarray(pet_mm, dtype=np.float64)
    ratio = np.divide(p, np.maximum(e0, 1e-6))
    evap = p / np.power(1.0 + np.power(ratio, wb.budyko_n), 1.0 / wb.budyko_n)
    return np.maximum(p - evap, 0.0)


def aridity_index(precip_mm, pet_mm):
    """P/PET, the UNEP aridity index. <0.05 hyper-arid ... >0.65 humid."""
    return np.asarray(precip_mm, dtype=np.float64) / np.maximum(
        np.asarray(pet_mm, dtype=np.float64), 1e-6
    )


# --------------------------------------------------------------------------- rasters


def reopened_surface(z_final: np.ndarray, basin_depth: np.ndarray) -> np.ndarray:
    """The shipped surface with B2a's depression fill undone (§4.2.2).

    ``basin_depth`` is continuous and 0 at the rim by construction (it is
    ``filled - carrier_plus_roughness``, and the fill only raises), so
    subtracting it re-opens every hole with no seam at any edge.
    """
    z = np.asarray(z_final)
    d = np.asarray(basin_depth)
    if z.shape != d.shape:
        raise ValueError(f"shape mismatch: z {z.shape} vs basin_depth {d.shape}")
    if np.any(d < 0.0):
        raise ValueError("basin_depth must be non-negative (the fill only raises)")
    return np.asarray(z - d, dtype=np.float32)


def depression_components(z_open: np.ndarray) -> tuple[np.ndarray, np.ndarray, int]:
    """Label every depression of ``z_open``, 8-connected.

    Returns ``(labels, filled_flat, n)``:

    * ``labels`` int32, same shape, 0 = drains freely, 1..n = depression id.
    * ``filled_flat`` the ``flat_eps=0.0`` fill, so inside a depression it is
      EXACTLY the spill elevation -- a flat lake surface, which is useless for
      routing (that is what the epsilon staircase is for) and exactly right
      for reading a spill level off.
    * ``n`` the number of depressions.

    Component ids are assigned in raster scan order, which makes them a pure
    function of the surface. `component_stats` re-sorts to the plan's
    ``(min_y, min_x)`` order before anything is written down.
    """
    z = np.ascontiguousarray(z_open, dtype=np.float32)
    filled = np.asarray(flow.fill_depressions(z, flat_eps=0.0), dtype=np.float32)
    mask = filled > z
    labels = np.zeros(z.shape, dtype=np.int32)
    n = _label8(mask, labels)
    return labels, filled, n


@flow._jit(cache=True)
def _label8(mask, labels):
    """8-connected flood fill labelling, explicit stack (no recursion depth).

    One int32 label grid plus a stack bounded by the mask's own cell count.
    Chosen over union-find because the mask is ~2% of the domain: the stack
    never approaches the grid size in practice and the code is short enough to
    be obviously right.
    """
    h, w = mask.shape
    stack = np.empty(h * w, np.int64)
    dy = np.array((-1, 1, 0, 0, -1, -1, 1, 1), np.int64)
    dx = np.array((0, 0, -1, 1, -1, 1, -1, 1), np.int64)
    nlab = 0
    for y0 in range(h):
        for x0 in range(w):
            if not mask[y0, x0] or labels[y0, x0] != 0:
                continue
            nlab += 1
            labels[y0, x0] = nlab
            top = 0
            stack[top] = y0 * w + x0
            top += 1
            while top > 0:
                top -= 1
                c = stack[top]
                cy = c // w
                cx = c - cy * w
                for k in range(8):
                    ny = cy + dy[k]
                    nx = cx + dx[k]
                    if ny < 0 or ny >= h or nx < 0 or nx >= w:
                        continue
                    if mask[ny, nx] and labels[ny, nx] == 0:
                        labels[ny, nx] = nlab
                        stack[top] = ny * w + nx
                        top += 1
    return nlab


@flow._jit(cache=True)
def _component_stats(labels, z_open, filled, acc, n, margin):
    """One pass for every per-component scalar, plus the outlet.

    Returns, all indexed by ``label - 1``:
      area, floor_m, spill_m, seed_y, seed_x, y0, x0, y1, x1,
      catchment_m2, touches_border, outlet_y, outlet_x

    The outlet is the LOWEST cell on the final surface that is adjacent to the
    component but outside it -- the saddle the basin spills over, i.e. the
    head of the outlet channel. Found here rather than by a second pass
    because a component's rim is not a slice and re-scanning per component
    would be O(n_components * area).
    """
    h, w = labels.shape
    area = np.zeros(n, np.int64)
    floor = np.full(n, np.inf, np.float64)
    spill = np.full(n, -np.inf, np.float64)
    seed_y = np.zeros(n, np.int64)
    seed_x = np.zeros(n, np.int64)
    y0 = np.full(n, h, np.int64)
    x0 = np.full(n, w, np.int64)
    y1 = np.full(n, -1, np.int64)
    x1 = np.full(n, -1, np.int64)
    catch = np.zeros(n, np.float64)
    border = np.zeros(n, np.uint8)
    out_y = np.full(n, -1, np.int64)
    out_x = np.full(n, -1, np.int64)
    out_z = np.full(n, np.inf, np.float64)
    dy = np.array((-1, 1, 0, 0, -1, -1, 1, 1), np.int64)
    dx = np.array((0, 0, -1, 1, -1, 1, -1, 1), np.int64)
    for y in range(h):
        for x in range(w):
            li = labels[y, x]
            if li > 0:
                i = li - 1
                area[i] += 1
                zv = z_open[y, x]
                if zv < floor[i]:
                    floor[i] = zv
                    seed_y[i] = y
                    seed_x[i] = x
                fv = filled[y, x]
                if fv > spill[i]:
                    spill[i] = fv
                if y < y0[i]:
                    y0[i] = y
                if x < x0[i]:
                    x0[i] = x
                if y > y1[i]:
                    y1[i] = y
                if x > x1[i]:
                    x1[i] = x
                av = acc[y, x]
                if av > catch[i]:
                    catch[i] = av
                if y < margin or x < margin or y >= h - margin or x >= w - margin:
                    border[i] = 1
                continue
            # Outside a component: is this a rim cell of one? A saddle can be
            # adjacent to two basins, and it is a legitimate outlet for both,
            # so every neighbouring label is considered rather than one.
            zv = z_open[y, x]
            for k in range(8):
                ny = y + dy[k]
                nx = x + dx[k]
                if ny < 0 or ny >= h or nx < 0 or nx >= w:
                    continue
                lj = labels[ny, nx]
                if lj > 0 and zv < out_z[lj - 1]:
                    out_z[lj - 1] = zv
                    out_y[lj - 1] = y
                    out_x[lj - 1] = x
    return (area, floor, spill, seed_y, seed_x, y0, x0, y1, x1,
            catch, border, out_y, out_x)


# --------------------------------------------------------------------------- records


@dataclass
class BasinRecord:
    """One depression, everything the survey and the wire format need.

    Pixel coordinates are in the coordinate space of the array passed in --
    ``survey_basins`` is normally handed the PADDED domain, and converts to
    interior pixels itself so a record's ``seed_px`` means the same thing as
    the tile's own elevation index.
    """

    basin_id: int
    #: Deepest cell, the client's flood-fill seed. Interior pixels.
    seed_px: tuple[int, int]
    #: (x0, y0, x1, y1) inclusive, interior pixels.
    bbox_px: tuple[int, int, int, int]
    #: Cells in the depression at the spill level, and that area in m^2.
    area_cells: int
    area_m2: float
    #: Lowest point and spill point of the depression on the FINAL surface.
    floor_m: float
    spill_m: float
    #: Equilibrium water surface from the balance; == spill_m when overflowing.
    surface_m: float
    kind: int
    #: The spill cell -- the head of the outlet channel. Meaningful for
    #: KIND_LAKE_OVERFLOWING; recorded for every basin because it is what the
    #: exclusion diagnostics compare against.
    outlet_px: tuple[int, int]
    #: Upstream area delivered to the basin, m^2 (max of ``acc`` inside it).
    catchment_m2: float
    #: Whether the component comes within ``border_margin_px`` of the padded
    #: edge -- see BasinFilter.border_margin_px for why this is a near-miss
    #: test and not the (structurally impossible) touches-the-border one.
    near_padded_border: bool
    #: Whether the component lies wholly inside the tile interior. Its
    #: negation is "tile-spanning", the v1 exclusion of §4.2.4.
    interior: bool
    #: Climate at the basin, physical units.
    temp_c: float
    precip_mm: float
    precip_cv: float
    temp_seasonality: float
    #: Derived climate.
    pet_mm: float
    runoff_mm: float
    #: Inflow volume and the lake area that balances it, m^3/yr and m^2.
    inflow_m3_yr: float
    balance_area_m2: float
    #: Hypsometry: level in metres, wetted area in m^2, both ascending.
    hyps_levels_m: tuple[float, ...] = ()
    hyps_areas_m2: tuple[float, ...] = ()

    @property
    def depth_m(self) -> float:
        return self.spill_m - self.floor_m

    @property
    def water_depth_m(self) -> float:
        return max(self.surface_m - self.floor_m, 0.0)

    @property
    def is_lake(self) -> bool:
        return self.kind >= KIND_LAKE_TERMINAL

    @property
    def kind_name(self) -> str:
        return KIND_NAMES[self.kind]

    def as_dict(self) -> dict:
        d = {
            "basin_id": self.basin_id,
            "seed_px": list(self.seed_px),
            "bbox_px": list(self.bbox_px),
            "area_cells": self.area_cells,
            "area_m2": self.area_m2,
            "floor_m": self.floor_m,
            "spill_m": self.spill_m,
            "surface_m": self.surface_m,
            "depth_m": self.depth_m,
            "water_depth_m": self.water_depth_m,
            "kind": self.kind,
            "kind_name": self.kind_name,
            "outlet_px": list(self.outlet_px),
            "catchment_m2": self.catchment_m2,
            "near_padded_border": self.near_padded_border,
            "interior": self.interior,
            "temp_c": self.temp_c,
            "precip_mm": self.precip_mm,
            "precip_cv": self.precip_cv,
            "temp_seasonality": self.temp_seasonality,
            "pet_mm": self.pet_mm,
            "runoff_mm": self.runoff_mm,
            "inflow_m3_yr": self.inflow_m3_yr,
            "balance_area_m2": self.balance_area_m2,
            "hyps_levels_m": list(self.hyps_levels_m),
            "hyps_areas_m2": list(self.hyps_areas_m2),
        }
        return d


def hypsometry(z_in_basin: np.ndarray, spill_m: float, cell_area_m2: float,
               levels: int = 32) -> tuple[np.ndarray, np.ndarray]:
    """A(h) for one basin: wetted area at each of ``levels`` levels to the spill.

    ``A(h)`` is monotone non-decreasing by construction (it is a CDF of the
    basin's own elevations scaled by cell area), which is what makes the
    equilibrium level in §4.3 unique.
    """
    z = np.asarray(z_in_basin, dtype=np.float64).ravel()
    floor = float(z.min())
    if spill_m <= floor:
        return (np.array([floor], np.float64),
                np.array([z.size * cell_area_m2], np.float64))
    hs = np.linspace(floor, spill_m, int(levels) + 1)
    zs = np.sort(z)
    counts = np.searchsorted(zs, hs, side="right").astype(np.float64)
    return hs, counts * cell_area_m2


def equilibrium_level(hyps_levels: np.ndarray, hyps_areas: np.ndarray,
                      inflow_m3_yr: float, precip_mm: float, pet_mm: float,
                      ) -> tuple[float, float]:
    """Solve ``Q_in + P*A(h) == PET*A(h)`` on the hypsometric curve (§4.3).

    Rearranged, the balance is a pure area condition::

        A(h*) = Q_in / ((PET - P) / 1000)          [m^2, from mm/yr]

    which is why the solve is an interpolation and not an iteration: A(h) is
    monotone, so the level is found by inverting the curve at that one area.

    Returns ``(h_star, balance_area_m2)``. ``h_star`` is ``+inf`` when the
    basin cannot evaporate its inflow at any level it can reach -- the caller
    clamps to the spill and calls it overflowing. ``balance_area_m2`` is
    ``+inf`` in the same case, which is the honest statement of "there is no
    finite lake area that closes this balance".
    """
    net_mm = float(pet_mm) - float(precip_mm)
    if net_mm <= 0.0:
        # Direct precipitation alone exceeds evaporation: the lake grows until
        # it spills regardless of inflow.
        return float("inf"), float("inf")
    need_m2 = float(inflow_m3_yr) / (net_mm / 1000.0)
    areas = np.asarray(hyps_areas, dtype=np.float64)
    levels = np.asarray(hyps_levels, dtype=np.float64)
    if need_m2 >= areas[-1]:
        return float("inf"), need_m2
    if need_m2 <= areas[0]:
        return float(levels[0]), need_m2
    return float(np.interp(need_m2, areas, levels)), need_m2


def classify(h_star: float, floor_m: float, spill_m: float, *,
             precip_mm: float, pet_mm: float, precip_cv: float,
             wb: WaterBalance = WaterBalance()) -> tuple[int, float]:
    """Basin ``kind`` and the surface elevation that ships (§4.3).

    Returns ``(kind, surface_m)``. ``surface_m`` is ``min(spill, h*)`` clamped
    to the floor, so a dry basin's "surface" is its floor rather than a
    meaningless number below the ground.
    """
    if h_star >= spill_m:
        return KIND_LAKE_OVERFLOWING, float(spill_m)
    surface = max(float(h_star), float(floor_m))
    depth = surface - float(floor_m)
    if depth >= wb.min_lake_depth_m:
        return KIND_LAKE_TERMINAL, surface
    # Dry at the annual mean. Two ways a dry bowl is still not a plain dry
    # bowl: it fills seasonally, or it concentrates solutes.
    if precip_cv >= wb.seasonal_cv_pct:
        return KIND_SEASONAL, surface
    if aridity_index(precip_mm, pet_mm) < wb.salt_aridity:
        return KIND_SALT_FLAT, surface
    return KIND_DRY_PLAYA, surface


# --------------------------------------------------------------------------- driver


@dataclass
class BasinSurvey:
    """Everything ``survey_basins`` measured, kept and excluded alike."""

    #: Registered basins, ordered by (min_y, min_x) of extent, ids from 0.
    basins: list[BasinRecord] = field(default_factory=list)
    #: How many components each filter stage removed, so the cost of every
    #: threshold is a number rather than an assertion.
    n_components: int = 0
    excluded_shallow: int = 0
    excluded_small: int = 0
    #: Depressions whose spill is at or below sea level: sea floor, not lakes.
    excluded_submarine: int = 0
    #: Components that pass depth+area+sea but leave the tile interior. This
    #: is the "cost of the tile-spanning exclusion" §4.2.4 asks to be counted,
    #: with their area and depth so it is a size and not just a count.
    excluded_spanning: int = 0
    excluded_spanning_area_m2: float = 0.0
    excluded_spanning_max_depth_m: float = 0.0
    #: Registered basins that come within ``border_margin_px`` of the padded
    #: edge. A diagnostic, not an exclusion -- see BasinFilter.
    kept_near_padded_border: int = 0
    #: The re-opened surface's total depression volume, m^3, and the share of
    #: it the registry keeps.
    total_depression_volume_m3: float = 0.0
    kept_volume_m3: float = 0.0
    #: Component label raster over the grid that was surveyed (0 = drains
    #: freely). Kept so the bake's B5 step can re-open exactly the registered
    #: basins without labelling twice; the survey tool drops it.
    labels: "np.ndarray | None" = None
    #: Component label of each registered basin, indexed by ``basin_id``.
    #: ``labels == kept_labels[i]`` is basin i's mask.
    kept_labels: tuple[int, ...] = ()

    def keep_mask(self) -> "np.ndarray":
        """Boolean mask of every registered basin over the surveyed grid.

        This is what B5 subtracts ``basin_depth`` under. Built from the label
        raster rather than from the bounding boxes because two basins can
        share a bbox and a bbox is not a basin.
        """
        if self.labels is None:
            raise ValueError("survey was run without keeping labels")
        if not self.kept_labels:
            return np.zeros(self.labels.shape, dtype=bool)
        keep = np.zeros(int(self.labels.max()) + 1, dtype=bool)
        keep[np.asarray(self.kept_labels, dtype=np.int64)] = True
        return keep[self.labels]

    def counts_by_kind(self) -> dict[str, int]:
        out = {k: 0 for k in KIND_NAMES}
        for b in self.basins:
            out[b.kind_name] += 1
        return out


def survey_basins(
    *,
    z_final: np.ndarray,
    basin_depth: np.ndarray,
    accumulation_m2: np.ndarray,
    climate: dict | None,
    cell_m: float,
    interior: slice | None = None,
    filt: BasinFilter = BasinFilter(),
    wb: WaterBalance = WaterBalance(),
    hyps_levels: int = 32,
    keep_labels: bool = False,
    keep_hypsometry: bool = True,
) -> BasinSurvey:
    """Re-open, re-fill, register and classify every depression in one grid.

    ``z_final``/``basin_depth``/``accumulation_m2`` are the PADDED domain (the
    border test in §4.2.4 is about the padded border, and a fill run on the
    interior alone would invent its own boundary condition). ``climate`` is
    the de-quantised padded climate dict from
    ``province.dequantize_climate``, at the COARSE pitch; it is sampled at
    ``//16`` exactly as ``province`` does. Pass None for no climate, in which
    case every basin classifies as if at 0 degC / 0 mm and the kinds are
    meaningless -- the survey reports that rather than guessing.

    ``interior`` is the slice that crops the padded domain to the tile, used
    only to convert pixel coordinates and to decide ``BasinRecord.interior``.
    """
    z_open = reopened_surface(z_final, basin_depth)
    labels, filled, n = depression_components(z_open)
    cell_area = float(cell_m) * float(cell_m)
    survey = BasinSurvey(n_components=n)
    survey.total_depression_volume_m3 = float(
        np.sum((filled - z_open)[labels > 0], dtype=np.float64) * cell_area
    )
    if keep_labels:
        survey.labels = labels
    if n == 0:
        return survey

    acc = np.ascontiguousarray(accumulation_m2, dtype=np.float32)
    (area, floor, spill, seed_y, seed_x, by0, bx0, by1, bx1,
     catch, border, out_y, out_x) = _component_stats(
         labels, z_open, filled, acc, n, int(filt.border_margin_px))

    ox = oy = 0
    iw = z_open.shape[1]
    ih = z_open.shape[0]
    if interior is not None:
        ox = int(interior.start or 0)
        oy = ox
        iw = int(interior.stop) - ox
        ih = iw

    coarse_ratio = 1
    if climate is not None:
        any_plane = next(iter(climate.values()))
        coarse_ratio = max(int(round(z_open.shape[0] / any_plane.shape[0])), 1)

    keep: list[tuple[tuple[int, int], int]] = []
    for i in range(n):
        depth = float(spill[i] - floor[i])
        if depth < filt.min_depth_m:
            survey.excluded_shallow += 1
            continue
        if float(area[i]) * cell_area < filt.min_area_m2:
            survey.excluded_small += 1
            continue
        if float(spill[i]) <= SEA_LEVEL_M:
            survey.excluded_submarine += 1
            if filt.require_above_sea:
                continue
        inside = (int(bx0[i]) >= ox and int(by0[i]) >= oy
                  and int(bx1[i]) - ox < iw and int(by1[i]) - oy < ih)
        if not inside:
            survey.excluded_spanning += 1
            survey.excluded_spanning_area_m2 += float(area[i]) * cell_area
            survey.excluded_spanning_max_depth_m = max(
                survey.excluded_spanning_max_depth_m, depth)
            if filt.exclude_spanning:
                continue
        if border[i]:
            survey.kept_near_padded_border += 1
        keep.append(((int(by0[i]), int(bx0[i])), i))

    keep.sort()
    for basin_id, (_, i) in enumerate(keep):
        ys = slice(int(by0[i]), int(by1[i]) + 1)
        xs = slice(int(bx0[i]), int(bx1[i]) + 1)
        sub = labels[ys, xs] == (i + 1)
        zsub = z_open[ys, xs][sub]
        levels_m, areas_m2 = hypsometry(zsub, float(spill[i]), cell_area,
                                        levels=hyps_levels)

        if climate is not None:
            cy = min(int(seed_y[i]) // coarse_ratio, climate["temperature"].shape[0] - 1)
            cx = min(int(seed_x[i]) // coarse_ratio, climate["temperature"].shape[1] - 1)
            temp_c = float(climate["temperature"][cy, cx])
            precip = float(climate["precipitation"][cy, cx])
            cv = float(climate["precip_variability"][cy, cx])
            tseas = float(climate["seasonality"][cy, cx])
        else:
            temp_c = precip = cv = tseas = 0.0

        pet = float(pet_mm_yr(temp_c, wb))
        runoff = float(budyko_runoff_mm_yr(precip, pet, wb))
        # Catchment MINUS the lake itself: direct precipitation on the water
        # surface is the P*A(h) term of the balance and must not be counted
        # twice. ``acc`` was computed on the FILLED surface, so its maximum
        # inside the basin is the whole upstream network's delivered area.
        land_m2 = max(float(catch[i]) - float(area[i]) * cell_area, 0.0)
        inflow = runoff / 1000.0 * land_m2
        h_star, need = equilibrium_level(levels_m, areas_m2, inflow, precip, pet)
        kind, surface = classify(h_star, float(floor[i]), float(spill[i]),
                                 precip_mm=precip, pet_mm=pet, precip_cv=cv, wb=wb)

        sy = int(seed_y[i]) - oy
        sx = int(seed_x[i]) - ox
        rec = BasinRecord(
            basin_id=basin_id,
            seed_px=(sx, sy),
            bbox_px=(int(bx0[i]) - ox, int(by0[i]) - oy,
                     int(bx1[i]) - ox, int(by1[i]) - oy),
            area_cells=int(area[i]),
            area_m2=float(area[i]) * cell_area,
            floor_m=float(floor[i]),
            spill_m=float(spill[i]),
            surface_m=surface,
            kind=kind,
            outlet_px=(int(out_x[i]) - ox, int(out_y[i]) - oy),
            catchment_m2=float(catch[i]),
            near_padded_border=bool(border[i]),
            interior=(int(bx0[i]) >= ox and int(by0[i]) >= oy
                      and int(bx1[i]) - ox < iw and int(by1[i]) - oy < ih),
            temp_c=temp_c,
            precip_mm=precip,
            precip_cv=cv,
            temp_seasonality=tseas,
            pet_mm=pet,
            runoff_mm=runoff,
            inflow_m3_yr=inflow,
            balance_area_m2=need,
            hyps_levels_m=(tuple(float(v) for v in levels_m)
                           if keep_hypsometry else ()),
            hyps_areas_m2=(tuple(float(v) for v in areas_m2)
                           if keep_hypsometry else ()),
        )
        survey.basins.append(rec)
        survey.kept_volume_m3 += float(
            np.sum(np.maximum(float(spill[i]) - zsub, 0.0), dtype=np.float64)
        ) * cell_area
    survey.kept_labels = tuple(int(i) + 1 for _, i in keep)
    return survey
