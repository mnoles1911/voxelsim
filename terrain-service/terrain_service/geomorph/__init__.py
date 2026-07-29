"""Geomorphometric statistics that tell natural terrain from unnatural.

We amplify 30 m diffusion-model tiles down to 10 cm voxels and cannot read the answer
off a screenshot -- that has given the wrong answer repeatedly. This package is the
measurement instrument instead: plain functions over plain float heightfields, each
taking a cell size in metres and returning numbers with stated units.

Every metric here has been checked against the only question that matters -- **does it
separate the cases we care about** -- against five real Copernicus GLO-30 scenes (alpine,
dissected fluvial upland, high plains, sand sea, arid mountains) and against four kinds
of synthetic fake, all at matched resolution. ``tools/geomorph_validate.py`` is that
sweep as a file; ``docs/geomorph-validation.md`` is its output with the conclusions, and
re-running the tool rewrites both.

The hard test every metric was put to is a **spectrum-matched surrogate**: a Gaussian
field carrying a real scene's own variogram to within 15% at every lag, and no drainage
network at all. Three metric families see through it, one is marginal, and the rest do
not. The short version, because it changes how you should use this:

**The ones that work.**

* **`drainage.pit_statistics`, specifically ``fill_volume_per_area_m``.** Measured before
  the depression fill, so it is the one number the fill cannot launder. Real scenes need
  0.07-0.97 m of mean fill; every synthetic control needs 14-235 m. A 15x gap with no
  overlap. Note it is the fill *volume* and not the pit *count*: the alpine surrogate has
  fewer local minima than the real Alps, they are just vastly deeper.
* **`curvature`, on ``abs(laplacian_quantile_skew)``.** Real scenes 0.010-0.111,
  Gaussian controls under 0.003. The sign is a terrain-class property (fluvial and
  plains concave-skewed, alpine and dunes convex-skewed) so a realism gate must take the
  magnitude. Cheapest metric in the package -- no flow routing at all.
* **`drainage.hacks_law`.** The surprise: real scenes give h = 0.475-0.557 against the
  literature's 0.57, surrogates give 0.161-0.287. The fill invents basins with the right
  areas and the wrong shape. Needs a window of ~1000 cells or more to say so.
* **`curvature.tail_asymmetry`** as a second, independent curvature signal: 0.87-1.32 on
  real scenes against 0.99-1.00 on the surrogates. Small in absolute terms but clean.
* **`slope_area_relation`, on theta -- marginal, and kept for a different reason.** Real
  0.18-0.40 by this method, surrogates 0.13-0.15: a consistent ~1.8x on both pairs, but
  only 2.6 within-scene sigmas, so on one window it is suggestive rather than decisive.
  It stays because it is the only thing here that checks our own erosion model against
  its own prediction of theta = m/n = 0.5625. **Read its docstring before comparing that
  number to anything**: real Earth measures 0.18-0.40 by this method, not the textbook
  0.35-0.6, and a baked surface hitting 0.5625 would be twice as concave as the Alps.

**The ones that do not, and why they are still here.**

* **`slope`** is blind to a well-scaled fake -- the Cumberland Plateau and its surrogate
  agree on mean slope to 7% and on the repose fraction to 3%. It catches a badly scaled
  one, and it is the vocabulary `bake.thermal`'s calibration is quoted in.
* **`geomorphon`** separates terrain *classes* superbly -- the High Plains are 58% flat
  at a 3 deg threshold against the Cumberland Plateau's 0.02% -- and does not separate
  real from surrogate at all. Use it to answer "do plains look like plains", not "is this
  a landscape".
* **`variogram` is for tier bookkeeping, not realism**, and by construction: its answer
  is a function of the power spectrum, which is exactly what the surrogate matches. Use
  `variogram.tier_continuity` to catch a band double-counted or dropped at 30 m /
  1.875 m / 10 cm. Read that function's calibration note before trusting a kink on a
  terrain-scale variogram.
* **`hypsometry` does not discriminate anything.** It cannot see structure at all -- a
  pixel-shuffled DEM has an identical curve, which the test suite asserts -- and its
  spread across the five real classes is *smaller* than its spread between quadrants of
  one scene. It is four lines of code and it is honest about what it is; do not gate on
  it.
* **`drainage.drainage_density`** was tried at four channel-head thresholds spanning two
  decades and never separated real from synthetic at any of them. Kept as a descriptor.

Resolution is the trap this API is shaped to avoid: slope, curvature, geomorphon
histograms, pit density and drainage density all change with ``cell_m``, so every
function takes it explicitly, every result carries it, and
`require_same_resolution` turns a cross-resolution comparison into an exception.

Import cost: numpy only. scipy is never used; numba and matplotlib are imported lazily
and only by the flow-length sweep and the plotting helpers respectively, so this package
imports cleanly in CI, which has none of the three.
"""

from ._grid import ResolutionMismatch, require_same_resolution
from .controls import (
    cone,
    fbm,
    inclined_plane,
    paraboloid,
    radial_power_spectrum,
    shuffled,
    spectrum_matched_surrogate,
    value_noise,
)
from .curvature import CurvatureStats, curvature_fields, curvature_statistics
from .drainage import (
    A_CRIT_M2,
    DrainageDensity,
    HackLaw,
    PitStats,
    drainage_density,
    hacks_law,
    pit_statistics,
)
from .flow_context import FlowContext, flow_context
from .geomorphon import (
    CLASS_NAMES,
    GeomorphonStats,
    geomorphon_field,
    geomorphon_histogram,
)
from .hypsometry import Hypsometry, hypsometry
from .slope import REPOSE_DEG, SlopeStats, slope_field, slope_statistics
from .slope_area import PREDICTED_THETA, SlopeAreaResult, slope_area_relation
from .variogram import TIER_BOUNDARIES_M, VariogramResult, tier_continuity, variogram

__all__ = [
    # guards
    "ResolutionMismatch",
    "require_same_resolution",
    # 1. slope-area
    "slope_area_relation",
    "SlopeAreaResult",
    "PREDICTED_THETA",
    # 2. geomorphons
    "geomorphon_field",
    "geomorphon_histogram",
    "GeomorphonStats",
    "CLASS_NAMES",
    # 3. slope
    "slope_field",
    "slope_statistics",
    "SlopeStats",
    "REPOSE_DEG",
    # 4. curvature
    "curvature_fields",
    "curvature_statistics",
    "CurvatureStats",
    # 5. variogram
    "variogram",
    "tier_continuity",
    "VariogramResult",
    "TIER_BOUNDARIES_M",
    # 6. hypsometry
    "hypsometry",
    "Hypsometry",
    # 7. drainage
    "pit_statistics",
    "drainage_density",
    "hacks_law",
    "PitStats",
    "DrainageDensity",
    "HackLaw",
    "A_CRIT_M2",
    # shared flow pass
    "flow_context",
    "FlowContext",
    # controls
    "fbm",
    "value_noise",
    "spectrum_matched_surrogate",
    "shuffled",
    "inclined_plane",
    "paraboloid",
    "cone",
    "radial_power_spectrum",
]
