"""Geomorphon classification: Jasiewicz & Stepinski (2013), ten landform classes.

The method is line-of-sight openness. From each cell, look out along the eight compass
directions to a fixed ``search_m``; in each direction record the highest and the lowest
elevation *angle* seen anywhere along the ray. If neither exceeds ``flat_deg`` the
direction is flat (0); otherwise the direction takes the sign of whichever angle is
larger in magnitude (+1 = terrain rises, -1 = terrain falls). The eight ternary digits
reduce to a count of plus and minus, and that pair maps to one of ten landforms.

Two properties make this the right classifier for "do plains look like plains":

**It is a shape classifier, not a slope classifier.** A cell on a uniform 20 deg
hillside is SLOPE whether the hillside is 20 deg or 5 deg, because every direction's
sign is what matters, not its magnitude. So the histogram describes *organisation* --
how the map divides into ridges, hollows, footslopes and open slope -- rather than
steepness, which `slope` already measures.

**It is explicitly multi-scale, and the scale is the parameter.** ``search_m`` is the
size of landform being classified: at 300 m on a 30 m grid you are classifying
hillslope-scale form, at 3 km you are classifying whole ranges and every gully has
vanished into SLOPE. There is no scale-free answer and a histogram quoted without its
``search_m`` and ``cell_m`` is meaningless. Both are in the result.

**Measured, on five 1024^2 GLO-30 windows at 30.87 m with a 300 m lookout**, this is
what it answers and what it does not:

    class fraction        alpine  fluvial  plains  dunes  Death Valley
    flat (1 deg)           0.006   0.00001  0.088  0.037    0.248
    flat (3 deg)           0.016   0.0002   0.577  0.422    0.331
    slope                  0.520   0.295    0.252  0.489    0.386
    ridge + peak           0.051   0.179    0.161  0.105    0.056
    valley + pit           0.051   0.166    0.167  0.093    0.059

Two readings. First, **the flatness threshold is the whole answer to "do plains look
like plains"**: at 1 deg the High Plains are 9% flat, at 3 deg they are 58%, and the
dissected Cumberland Plateau stays at 0.02% either way. A plain is flat *relative to a
gradient you have to name*, and this parameter is where you name it. Second, alpine
terrain is the most SLOPE-dominated class here and the *least* ridge-and-valley one,
which is the opposite of the intuition -- at a 300 m lookout a 2 km alpine face is one
enormous uniform slope, and its arêtes are a thin fraction of the map.

**What geomorphons cannot do is tell real terrain from a spectrum-matched fake.** On the
Cumberland Plateau and its surrogate: ridge+peak 0.179 against 0.166, valley+pit 0.166
against 0.166. Line-of-sight openness is a second-order property of the surface, and the
surrogate matches the second-order properties by construction. Use this to describe a
landscape's character, not to certify it.

The lookup table and the ternary rule here are ported from GRASS ``r.geomorphon``
(``geom.c::determine_form`` and ``pattern.c::calc_pattern``, ANGLEV2 comparison mode),
not from a summary of the paper, because the published figure omits the tie-breaking and
the ``__`` impossible cells and reproducing it from memory gets SPUR and HOLLOW wrong.

Cost is ``8 * (search_m / cell_m)`` passes over the grid. At the default 300 m on a 30 m
grid that is 80 passes, ~1 s on 1024^2. Raising ``search_m`` to 3 km makes it 800.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ._grid import as_field, check_cell_m, interior_mask

__all__ = [
    "CLASS_NAMES",
    "CLASS_CODES",
    "GeomorphonStats",
    "geomorphon_field",
    "geomorphon_histogram",
]

#: Class codes 1..10, in GRASS's ``FORMS`` enum order. Code 0 means "not classified"
#: (a border cell excluded by ``border_mode='mask'``).
CLASS_NAMES = (
    "flat",       # 1  FL
    "peak",       # 2  PK
    "ridge",      # 3  RI
    "shoulder",   # 4  SH
    "spur",       # 5  SP
    "slope",      # 6  SL
    "hollow",     # 7  HL
    "footslope",  # 8  FS
    "valley",     # 9  VL
    "pit",        # 10 PT
)
CLASS_CODES = {name: i + 1 for i, name in enumerate(CLASS_NAMES)}

_FL, _PK, _RI, _SH, _SP, _SL, _HL, _FS, _VL, _PT = range(1, 11)
_XX = 0  # impossible (num_minus + num_plus > 8)

#: ``FORMS[num_minus][num_plus]`` -- GRASS ``geom.c::determine_form``, verbatim.
_FORMS = np.array([
    [_FL, _FL, _FL, _FS, _FS, _VL, _VL, _VL, _PT],
    [_FL, _FL, _FS, _FS, _FS, _VL, _VL, _VL, _XX],
    [_FL, _SH, _SL, _SL, _HL, _HL, _VL, _XX, _XX],
    [_SH, _SH, _SL, _SL, _SL, _HL, _XX, _XX, _XX],
    [_SH, _SH, _SP, _SL, _SL, _XX, _XX, _XX, _XX],
    [_RI, _RI, _SP, _SP, _XX, _XX, _XX, _XX, _XX],
    [_RI, _RI, _RI, _XX, _XX, _XX, _XX, _XX, _XX],
    [_RI, _RI, _XX, _XX, _XX, _XX, _XX, _XX, _XX],
    [_PK, _XX, _XX, _XX, _XX, _XX, _XX, _XX, _XX],
], dtype=np.int8)

# The eight rays, (drow, dcol). Diagonals are sqrt(2) cells apart per step.
_DIRS = ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1))
_R2 = 1.4142135623730951


@dataclass(frozen=True)
class GeomorphonStats:
    """Geomorphon class histogram at a stated scale.

    ``fractions`` is a dict keyed by `CLASS_NAMES`, summing to 1 over classified cells.
    ``search_m`` and ``cell_m`` are part of the answer, not metadata: see the module
    docstring.
    """

    cell_m: float
    search_m: float
    flat_deg: float
    skip_m: float
    n_classified: int
    counts: dict
    fractions: dict

    def to_dict(self) -> dict:
        out = {
            "cell_m": self.cell_m,
            "search_m": self.search_m,
            "flat_deg": self.flat_deg,
            "skip_m": self.skip_m,
            "n_classified": self.n_classified,
        }
        out.update({f"frac_{k}": v for k, v in self.fractions.items()})
        return out

    def frac(self, *names: str) -> float:
        """Summed fraction of the named classes, e.g. ``s.frac('ridge', 'peak')``."""
        return float(sum(self.fractions[n] for n in names))


def geomorphon_field(z, cell_m: float, *, search_m: float = 300.0,
                     flat_deg: float = 1.0, skip_m: float = 0.0,
                     border_mode: str = "mask") -> np.ndarray:
    """Per-cell landform code, int8 in 1..10 (see `CLASS_NAMES`), 0 = unclassified.

    ``search_m``  lookout distance. Sets the size of landform classified; there is no
                  default that is right for every question, and 300 m is only a
                  reasonable hillslope-scale choice on a 30 m grid.
    ``flat_deg``  flatness threshold in degrees. A direction whose extreme elevation
                  angle is within this of horizontal counts as flat. 1 deg is the GRASS
                  default; raising it makes plains flatter and, on steep ground, does
                  almost nothing.
    ``skip_m``    inner radius, in metres, excluded from the line of sight. Use it to
                  suppress a known small-scale artefact (or DEM noise) without giving up
                  the outer scale. 0 disables it.
    ``border_mode`` ``'mask'`` (default) leaves cells within the search radius of the
                  edge unclassified (code 0), because their rays are truncated and the
                  resulting bias is systematic -- truncated rays see less relief, so the
                  border reads as flat. ``'truncate'`` classifies them from whatever
                  rays fit, which is what GRASS does.
    """
    zz = as_field(z)
    cell = check_cell_m(cell_m)
    search_m = float(search_m)
    if not np.isfinite(search_m) or search_m <= 0.0:
        raise ValueError(f"search_m must be finite and > 0, got {search_m!r}")
    skip_m = float(skip_m)
    if not np.isfinite(skip_m) or skip_m < 0.0:
        raise ValueError(f"skip_m must be finite and >= 0, got {skip_m!r}")
    if skip_m >= search_m:
        raise ValueError(f"skip_m {skip_m} must be below search_m {search_m}")
    if border_mode not in ("mask", "truncate"):
        raise ValueError(f"border_mode must be 'mask' or 'truncate', got {border_mode!r}")

    radius = int(search_m // cell)
    if radius < 1:
        raise ValueError(
            f"search_m={search_m} m is under one cell at cell_m={cell} m: the "
            "classification would have no line of sight at all"
        )
    h, w = zz.shape
    if border_mode == "mask" and (2 * radius >= h or 2 * radius >= w):
        raise ValueError(
            f"search radius {radius} cells does not fit a {h}x{w} grid with "
            "border_mode='mask'; use a bigger window or a smaller search_m"
        )

    flat_tan = np.tan(np.radians(float(flat_deg)))
    if flat_tan < 0.0:
        raise ValueError(f"flat_deg must be >= 0, got {flat_deg!r}")

    num_plus = np.zeros((h, w), dtype=np.int8)
    num_minus = np.zeros((h, w), dtype=np.int8)

    # Elevation angle is monotone in dz/d for d > 0, so the extreme *angle* along a ray
    # is the extreme *tangent*: one arctan per direction instead of one per step.
    for dr, dc in _DIRS:
        step_m = cell * (_R2 if (dr and dc) else 1.0)
        j0 = max(1, int(np.ceil(skip_m / step_m)) if skip_m > 0 else 1)
        n_steps = int(search_m // step_m)
        if n_steps < j0:
            continue
        zen = np.full((h, w), -np.inf)   # max tan(elevation angle)
        nad = np.full((h, w), np.inf)    # min tan(elevation angle)
        for j in range(j0, n_steps + 1):
            oy, ox = dr * j, dc * j
            # Source and destination windows for the shift; cells whose ray leaves the
            # grid simply stop contributing, which is the 'truncate' behaviour.
            ys = slice(max(0, oy), h + min(0, oy))
            xs = slice(max(0, ox), w + min(0, ox))
            yd = slice(max(0, -oy), h + min(0, -oy))
            xd = slice(max(0, -ox), w + min(0, -ox))
            if ys.start >= ys.stop or xs.start >= xs.stop:
                break
            t = (zz[ys, xs] - zz[yd, xd]) / (j * step_m)
            np.maximum(zen[yd, xd], t, out=zen[yd, xd])
            np.minimum(nad[yd, xd], t, out=nad[yd, xd])
        seen = np.isfinite(zen)
        az = np.where(seen, np.abs(zen), 0.0)
        an = np.where(seen, np.abs(nad), 0.0)
        # GRASS pattern.c ANGLEV2: compare_multi(|nadir|, |zenith|, t, t).
        zen_over = seen & (zen > flat_tan)
        nad_over = seen & (nad < -flat_tan)
        # +1 when only the zenith clears the threshold, or when both do and the zenith
        # is the larger (ties go to +1, as GRASS does).
        plus = zen_over & (~nad_over | (az >= an))
        minus = nad_over & (~zen_over | (an > az))
        num_plus += plus
        num_minus += minus

    forms = _FORMS[num_minus, num_plus]
    if border_mode == "mask":
        forms = np.where(interior_mask(zz.shape, radius), forms, np.int8(0))
    return forms.astype(np.int8, copy=False)


def geomorphon_histogram(z, cell_m: float, *, search_m: float = 300.0,
                         flat_deg: float = 1.0, skip_m: float = 0.0,
                         border_mode: str = "mask") -> GeomorphonStats:
    """`geomorphon_field` reduced to a class histogram. Arguments as there."""
    forms = geomorphon_field(z, cell_m, search_m=search_m, flat_deg=flat_deg,
                             skip_m=skip_m, border_mode=border_mode)
    counts_arr = np.bincount(forms.ravel(), minlength=11)
    n = int(counts_arr[1:].sum())
    counts = {name: int(counts_arr[i + 1]) for i, name in enumerate(CLASS_NAMES)}
    fractions = {name: (counts[name] / n if n else float("nan")) for name in CLASS_NAMES}
    return GeomorphonStats(
        cell_m=check_cell_m(cell_m),
        search_m=float(search_m),
        flat_deg=float(flat_deg),
        skip_m=float(skip_m),
        n_classified=n,
        counts=counts,
        fractions=fractions,
    )
