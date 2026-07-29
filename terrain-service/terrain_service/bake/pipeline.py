"""B0-B3 bake orchestration: one coarse tile -> one 8192x8192 fine tier.

This module owns the *orchestration* only. The numerics live in sibling
modules written independently (``flow``, ``noise``, ``incise``, ``thermal``)
and are reached through ``BakeKernels``:

    fill_depressions(z, *, flat_eps=<auto>) -> z
    d8_receivers(z, cell_m) -> (int64 receiver y*w+x or -1, steepest_slope)
    accumulate_mfd(z, cell_m, p=1.1, inflow=None) -> float64 area_m2
    carrier(coarse, scale) -> fine
    roughness(carrier_z, cell_m, slope, seed, src_nyquist_m=30.0,
              origin_cells=(row0, col0)) -> delta
    stream_power(acc, slope, K=0.15, m=0.45, n=0.8, cap_m=25.0) -> depth_m
    relax(z, cell_m, repose_deg=36.0, iters=48, rate=0.4) -> z

``origin_cells`` is optional in the interface and MANDATORY here: without it
the roughness is generated in array coordinates and no apron can make two
bakes agree. ``bake_padded_domain`` refuses a ``roughness`` that cannot take
it. Likewise ``flat_eps``: its auto value is Barnes' epsilon variant, and a
plain fill (0) makes every filled pit a level lake on which MFD terminates --
measured on a real tile as 69.2% of land area stranded.

Nothing here imports them at module import time, and nothing here imports
numba or scipy at all: ``import terrain_service.bake.pipeline`` must work on
a bare CI box so ``providers/diffusion.py`` can hash the bake identity into
``provider_id`` without a numerics stack. See ``load_kernels()``.

WHAT THE PIPELINE DOES
----------------------
Bake tile T on T's footprint **plus a 960 m apron** taken from T's 3x3 coarse
ring, then write only the interior. At scale 16 (1.875 m/px) that is a
9216x9216 padded domain of which the middle 8192x8192 is kept.

**The apron is measured, not assumed** (``tools/bake_seam_check.py``, results
in docs/terrain-amplification-plan.md "Seams - aprons, not blending"):

    apron    mean err   max err    step across the join
     30 m    0.43 cm    9.78 m     +40.32 cm     <- control: the test CAN see a bad apron
    120 m    0.36 cm    6.05 m     -0.06 cm
    480 m    0.00 cm    2.93 cm    +0.01 cm
    960 m    0.00 cm    0.01 cm    +0.00 cm      <- this pipeline

At 960 m the per-tile bakes reproduce a single-domain bake over both tiles to
**0.1 mm**, and the accumulation ratio against truth is 1.000 at median, p01
AND min across 99,893 channel cells. ``APRON_M`` is a measurement, not a
tuning knob: do not shrink it. ``BakeGeometry`` is parameterised only so the
tests can run a 60x smaller domain; ``BakeGeometry.assert_production()``
exists to make an accidental production shrink fail loudly.

TWO THINGS THE APRON DOES **NOT** SOLVE
---------------------------------------
1. **Roughness anchoring.** An apron only helps if the field being computed is
   a function of *world* position. fBm generated in array coordinates is not:
   two overlapping domains disagree in their overlap for reasons that have
   nothing to do with aprons. See ``roughness_seed`` and ``NOISE_ANCHORING``.
2. **Flow accumulation is unbounded.** No practical apron captures a river
   whose catchment spans many tiles. See ``FlowLevel`` /
   ``build_flow_superblock`` / ``inject_edge_inflow`` for the hydrology
   pyramid, and ``HYDROLOGY_RESIDUALS`` for what it still gets wrong.

And it is exactly those two: ``APRON_BLIND_SPOT`` records a stage-by-stage
measurement through this pipeline against real numba/scipy kernels showing that
the carrier, the roughness and the depression fill come out **bit-identical**
between a per-tile bake and a single-domain bake, and that 100% of the residual
enters at accumulation and rides through incision into the surface. The plan
predicted this; nothing had attributed it to a stage before, because the
existing seam test compares only end-of-pipeline fields.

MEMORY
------
A production padded domain is 9216^2 = 84.9 Mcell. Every float32 grid is
340 MB and the D8 receiver array is int64, i.e. 680 MB. ``estimate_peak_bytes``
puts the live set at ~3 GB, which is well above the "~0.5 GB of bake grids"
the plan's offline-appendix assumes. Size the pod accordingly.

TIMING
------
Reported as ``time.process_time()`` only. Wall-clock on a contended box reads
exactly like a slow configuration; CPU-seconds cannot be stolen by another
process. (For the parallel kernels CPU-s exceeds wall-s by roughly the thread
count -- that is the point, it is the WORK.)

Measured at full 8192^2 by the flow module's owner: fill 12.5, D8 2.2,
accumulate 15.8 = ~30 CPU-s, ~2.4 GB transient. Against the ~165 CPU-s/tile
docs/vxtl-v2-format.md projects for the whole bake, **B2 is not the
bottleneck**; thermal relaxation still is, and it is the stage a GPU eats.
"""

from __future__ import annotations

import hashlib
import json
import struct
import time
from dataclasses import dataclass
from typing import Callable

import numpy as np

__all__ = [
    "BAKE_VERSION",
    "BakeConstants",
    "BakeGeometry",
    "BakeKernels",
    "BakeResult",
    "CONSTANTS",
    "FlowLevel",
    "FlowSuperblock",
    "PRODUCTION",
    "assemble_padded_coarse",
    "bake_identity_payload",
    "bake_fingerprint",
    "bake_tile",
    "build_flow_superblock",
    "decode_flow_superblock",
    "encode_flow_superblock",
    "estimate_peak_bytes",
    "flow_plane",
    "inject_edge_inflow",
    "load_kernels",
    "roughness_seed",
    "superblock_index",
]


# ---------------------------------------------------------------------------
# Version + identity.
# ---------------------------------------------------------------------------

#: Bake algorithm + constants version. This is the ``bake_ver`` field of the
#: .vxtl v2 header (docs/vxtl-v2-format.md section 3) AND part of
#: ``providers/diffusion.py::_tile_format_fingerprint``, so bumping it yields a
#: NEW provider_id, i.e. a new world with old caches untouched. That is the
#: only sanctioned way to change the bake: "Never regenerate a shipped tile"
#: (vxtl-v2-format.md section 7).
#:
#: **Bump it in the same commit as any change to:** the stage order below, the
#: apron, the world-anchoring scheme, the hydrology pyramid's shape, or any
#: value in ``BakeConstants``/``BakeGeometry``. The constants are hashed
#: automatically (``bake_identity_payload``) so a value change rolls the id
#: even if you forget; the counter exists for changes to the *code* that the
#: constants cannot describe.
BAKE_VERSION = 1


@dataclass(frozen=True)
class BakeGeometry:
    """Tile/apron geometry. ``PRODUCTION`` is the shipped one.

    Parameterised ONLY so tests can bake a domain that fits in a CI runner.
    Every field is hashed into the bake identity, so a "temporary" production
    change cannot hide.
    """

    #: Coarse (s1) tile edge in pixels. tile_codec.TILE_SIZE.
    coarse_tile_px: int = 512
    #: Coarse pixel size. The learned cascade ends at 30 m.
    coarse_pixel_m: float = 30.0
    #: Fine-tier supersample factor. 16 => 1.875 m/px, 8192x8192 per tile.
    scale: int = 16
    #: Apron in COARSE pixels. 32 * 30 m = 960 m -- see the module docstring's
    #: measurement table before touching this.
    apron_coarse_px: int = 32

    def __post_init__(self) -> None:
        if self.coarse_tile_px <= 0 or self.scale <= 0 or self.apron_coarse_px < 0:
            raise ValueError("BakeGeometry fields must be positive")
        if self.apron_coarse_px > self.coarse_tile_px:
            raise ValueError(
                f"apron_coarse_px={self.apron_coarse_px} exceeds one coarse tile "
                f"({self.coarse_tile_px}px): the padded domain would need more "
                "than the 3x3 coarse ring assemble_padded_coarse gathers"
            )

    # -- derived -----------------------------------------------------------
    @property
    def fine_tile_px(self) -> int:
        return self.coarse_tile_px * self.scale

    @property
    def fine_pixel_m(self) -> float:
        return self.coarse_pixel_m / self.scale

    @property
    def apron_fine_px(self) -> int:
        return self.apron_coarse_px * self.scale

    @property
    def apron_m(self) -> float:
        return self.apron_coarse_px * self.coarse_pixel_m

    @property
    def padded_coarse_px(self) -> int:
        return self.coarse_tile_px + 2 * self.apron_coarse_px

    @property
    def padded_fine_px(self) -> int:
        return self.padded_coarse_px * self.scale

    @property
    def tile_span_m(self) -> float:
        return self.coarse_tile_px * self.coarse_pixel_m

    def interior(self) -> slice:
        """The slice (both axes) that turns a padded fine grid into the tile."""
        a = self.apron_fine_px
        return slice(a, a + self.fine_tile_px)

    def padded_origin_cells(self, tile_x: int, tile_y: int) -> tuple[int, int]:
        """World fine-cell index of padded[0, 0], as **(row0, col0)**.

        Row-major (y first), matching ``noise.roughness``'s ``origin_cells``
        contract and the arrays themselves. It is the origin of the PADDED
        domain -- the apron included -- because that is the array handed to
        the kernel, and getting this wrong by the apron would offset the noise
        by 960 m and reintroduce exactly the seam it exists to remove.

        Every tile's padded origin is congruent modulo ``fine_tile_px``
        (``t * fine_tile_px - apron_fine_px``), which is why an apron and its
        neighbour's interior can agree at all -- see ``NOISE_ANCHORING``.
        """
        a = self.apron_fine_px
        return (tile_y * self.fine_tile_px - a, tile_x * self.fine_tile_px - a)

    def padded_origin_fine_px(self, tile_x: int, tile_y: int) -> tuple[int, int]:
        """As ``padded_origin_cells`` but (x, y), for the metric helpers."""
        row0, col0 = self.padded_origin_cells(tile_x, tile_y)
        return (col0, row0)

    def padded_origin_m(self, tile_x: int, tile_y: int) -> tuple[float, float]:
        ox, oy = self.padded_origin_fine_px(tile_x, tile_y)
        return (ox * self.fine_pixel_m, oy * self.fine_pixel_m)

    def assert_production(self) -> None:
        """Fail loudly if this is not the measured, shipped geometry."""
        if (self.coarse_tile_px, self.coarse_pixel_m, self.scale, self.apron_coarse_px) != (
            512,
            30.0,
            16,
            32,
        ):
            raise ValueError(
                f"non-production BakeGeometry {self}: the 960 m apron and the "
                "scale-16 fine tier are MEASURED values (see the module "
                "docstring's seam table and docs/vxtl-v2-format.md). A smaller "
                "apron measured 6 m of error at 120 m and 9.8 m at 30 m."
            )

    def as_payload(self) -> dict:
        return {
            "coarse_tile_px": self.coarse_tile_px,
            "coarse_pixel_m": self.coarse_pixel_m,
            "scale": self.scale,
            "apron_coarse_px": self.apron_coarse_px,
        }


@dataclass(frozen=True)
class BakeConstants:
    """Every number the bake's physics depends on. All hashed into the id."""

    # -- B1 roughness ------------------------------------------------------
    #: Source Nyquist handed to ``roughness``: above this the 30 m raster
    #: already holds real data and synthesising there fights the model
    #: ("replace, do not layer").
    src_nyquist_m: float = 30.0
    #: World pitch, in fine px, on which the roughness field is re-anchored.
    #: 0 = ONE field for the whole world, which is the only exactly seamless
    #: choice available through a scalar seed. See ``roughness_seed``.
    noise_anchor_pitch_fine_px: int = 0

    # -- B2 flow + incision ------------------------------------------------
    #: MFD exponent. D8 for the AREA field gives dead-straight 45-degree
    #: channels tens of pixels long (measured in the prototype); D8 stays
    #: right for a channel centreline.
    mfd_p: float = 1.1
    #: Flat-resolution epsilon for ``fill_depressions``. None = the module's
    #: auto value (Barnes' epsilon variant, spill + 2 ULP per newly discovered
    #: cell). 0 reproduces a plain fill, which makes every filled pit a LEVEL
    #: lake on which MFD terminates -- measured on a real tile as 341,368
    #: inland dead-ends and 69.2% of land area stranded. Do not set 0.
    flat_eps: float | None = None
    #: Erodibility.
    #:
    #: **CALIBRATED 2026-07-29 on correctly-routed drainage** (real tile (-5,3),
    #: 4096^2 at 1.875 m/px, ``tools/calibrate_stream_k.py``). The earlier sweep
    #: that first chose 0.15 ran on the plain-fill field above, with two thirds
    #: of the drainage stranded; the epsilon fill grows catchments ~2.9x, so the
    #: same K cuts ~1.6x deeper and the expectation was that K had to fall to
    #: ~0.09. On a hillshade that was **wrong**: 0.09 leaves trunk channels
    #: legible but tributaries not, 0.15 gives a legible dendritic network at
    #: both 7.7 km and 1.4 km, and 0.25+ begins showing parallel grooving. The
    #: 1.6x arithmetic is right about the depth and wrong about the conclusion,
    #: because the original judgement was made on a different tile at 3.75 m/px
    #: -- there was never a like-for-like appearance to preserve.
    #:
    #: Changing it is cheap -- it rolls the bake identity, hence provider_id,
    #: hence the world, with old caches untouched, which is the sanctioned way.
    stream_K: float = 0.15
    stream_m: float = 0.45
    stream_n: float = 0.8
    #: Over-carving is its own failure mode; this is the sub-threshold cap.
    #: NOTE it binds at every K tested including 0.03, so ``max`` incision is
    #: censored and p99 is the only usable tail statistic when tuning.
    incision_cap_m: float = 25.0
    #: Channel-initiation area, m^2. Without it ``K*A^m*S^n`` incises every cell
    #: that has any upslope area, which at 1.875 m/px is every cell in the tile:
    #: measured 77.6% of the domain incised past one voxel at K=0.03 and 98.6%
    #: at K=0.15. That is a slope-dependent lowering of the whole surface with a
    #: network faintly embedded in it, not a drainage network. This is the knob
    #: that sets DRAINAGE DENSITY; without it K had to set both how deep
    #: channels cut and how many there are, which are not the same question.
    #: At 1e4 the same tile drops to 25.4% incised while p99 depth is unchanged
    #: (8.66 -> 8.34 m), i.e. hillslopes are released and channels are not.
    #: 0 disables the gate and reproduces the pre-2026-07-29 bake exactly.
    channel_init_area_m2: float = 1.0e4
    #: Gate sharpness. The gate is SOFT deliberately: a hard cutoff puts a step
    #: in incision depth along the contour where area crosses the threshold,
    #: which is a visible seam along a curve -- the same failure class as the
    #: 30 m grid seams, just not on the grid.
    channel_init_q: float = 2.0
    #: Depth over which fluvial incision fades out below sea level. Nothing
    #: gated on depth before this, so the bake cut dendritic river valleys into
    #: the seafloor: measured on a 100%-ocean tile, 39.7% of the tile flagged as
    #: channel (against 4.1% alpine) and 0.87 m mean incision (against 0.13 m),
    #: at three kilometres depth. It made the OCEAN tile the largest of three
    #: baked -- 28.35 MB against 22.62 for alpine -- because the invented detail
    #: has to be encoded. Tapered rather than cut: a hard stop at z=0 would put a
    #: step in incision along the entire coastline. -200 m is the shelf break,
    #: which keeps river mouths, deltas and genuinely incised shelf valleys.
    sea_taper_top_m: float = 0.0
    sea_taper_bottom_m: float = -200.0

    # -- B3 thermal --------------------------------------------------------
    repose_deg: float = 36.0
    thermal_iters: int = 48
    #: Must stay <= 0.5 -- ``thermal.relax`` rejects more outright. The shed is
    #: capped by the STEEPEST over-repose pair, so that pair can at most be
    #: driven TO repose, never through it; above 0.5 that bound is gone and a
    #: prototype run diverged to ~1e23 in 48 iterations.
    #:
    #: The steepest-pair rule bounds what a cell GIVES, not what it RECEIVES:
    #: an isolated one-cell pit is filled by all eight neighbours in one step,
    #: so max slope can spike before it converges (measured 128 -> 164 in a
    #: single step). That is directly relevant here, because ``incision_cap_m``
    #: at 1.875 m/px carves exactly such narrow pit-like channels. If thermal
    #: misbehaves after incision the fix is a LOWER rate or more iterations,
    #: never a higher rate.
    thermal_rate: float = 0.4

    # -- hydrology pyramid -------------------------------------------------
    #: Coarse tiles per side of a level-0 flow superblock. 4 => 61.4 km,
    #: 2048x2048 cells at 30 m, catchments to ~3,700 km2.
    superblock_tiles: int = 4
    #: Highest pyramid level built. Level L has cell size 30 * 4^L m and spans
    #: 4^(L+1) tiles; level 1 spans 245 km at 120 m/px.
    superblock_max_level: int = 1

    # -- flow plane (vxtl v2 section 6) ------------------------------------
    #: Accumulation at which a cell is flagged as channel.
    channel_area_m2: float = 1.0e5
    #: ...or this much stream-power incision.
    channel_depth_m: float = 0.25
    #: Thermal net gain that counts as deposition.
    deposition_m: float = 0.10

    def __post_init__(self) -> None:
        if not 0.0 < self.thermal_rate <= 0.5:
            raise ValueError(
                f"thermal_rate={self.thermal_rate} is outside (0, 0.5]; "
                "thermal.relax rejects it, and above 0.5 the steepest-pair "
                "bound no longer holds"
            )
        if self.superblock_tiles < 1 or self.superblock_max_level < 0:
            raise ValueError("superblock_tiles >= 1 and max_level >= 0")

    def as_payload(self) -> dict:
        return {
            "src_nyquist_m": self.src_nyquist_m,
            "noise_anchor_pitch_fine_px": self.noise_anchor_pitch_fine_px,
            "mfd_p": self.mfd_p,
            "flat_eps": self.flat_eps,
            "stream_K": self.stream_K,
            "stream_m": self.stream_m,
            "stream_n": self.stream_n,
            "incision_cap_m": self.incision_cap_m,
            "channel_init_area_m2": self.channel_init_area_m2,
            "channel_init_q": self.channel_init_q,
            "sea_taper_top_m": self.sea_taper_top_m,
            "sea_taper_bottom_m": self.sea_taper_bottom_m,
            "repose_deg": self.repose_deg,
            "thermal_iters": self.thermal_iters,
            "thermal_rate": self.thermal_rate,
            "superblock_tiles": self.superblock_tiles,
            "superblock_max_level": self.superblock_max_level,
            "channel_area_m2": self.channel_area_m2,
            "channel_depth_m": self.channel_depth_m,
            "deposition_m": self.deposition_m,
        }


PRODUCTION = BakeGeometry()
CONSTANTS = BakeConstants()

#: Stage order, hashed into the identity so a reordering rolls the world even
#: though no constant changed.
STAGE_ORDER: tuple[str, ...] = (
    "B0.carrier",
    "B1.roughness",
    "B2a.fill_depressions",
    "B2b.d8_receivers",
    "B2c.accumulate_mfd",
    "B2d.stream_power",
    "B3.relax",
)


def bake_identity_payload(
    geom: BakeGeometry = PRODUCTION, consts: BakeConstants = CONSTANTS
) -> dict:
    """JSON-serialisable description of everything that decides baked bytes.

    Folded into ``providers/diffusion.py::_tile_format_fingerprint`` so that a
    bake change yields a new ``provider_id`` -- a new world with old caches
    untouched -- rather than a cache holding two mutually incompatible bakes
    under one identity.
    """
    return {
        "bake_version": BAKE_VERSION,
        "stage_order": list(STAGE_ORDER),
        "geometry": geom.as_payload(),
        "constants": consts.as_payload(),
    }


def bake_fingerprint(
    geom: BakeGeometry = PRODUCTION, consts: BakeConstants = CONSTANTS
) -> str:
    return hashlib.sha256(
        json.dumps(bake_identity_payload(geom, consts), sort_keys=True).encode("utf-8")
    ).hexdigest()


def estimate_peak_bytes(geom: BakeGeometry = PRODUCTION) -> int:
    """Rough live-set of ``bake_padded_domain`` in bytes.

    Counted, not timed, so box contention cannot touch it: 8 float32 grids
    (carrier, roughness delta, slope, filled, acc, depth, eroded, relaxed) plus
    one int64 receiver grid over the PADDED domain.
    """
    n = geom.padded_fine_px**2
    return 8 * 4 * n + 8 * n


# ---------------------------------------------------------------------------
# B1 world anchoring.
# ---------------------------------------------------------------------------

NOISE_ANCHORING = """
How the roughness is world-anchored, and why it is BIT-EXACT rather than close.

The prototype's fBm is generated in array coordinates, so two overlapping
domains disagree in their overlap no matter how large the apron is. An apron
only helps a field that is a function of WORLD position. Three pieces:

1. **The domain's world origin is passed explicitly.** ``noise.roughness``
   takes ``origin_cells=(row0, col0)`` -- the fine-cell index of the array's
   [0, 0] in the world lattice, apron INCLUDED, because the apron is part of
   the array handed to the kernel. ``bake_padded_domain`` passes
   ``geom.padded_origin_cells(...)``. Omitting it is legal and reproduces the
   prototype's array-anchored behaviour, i.e. the bug; this pipeline never
   omits it and refuses to run against a ``roughness`` that cannot accept it.

2. **The seed carries no per-tile entropy.** Deriving the seed from the tile
   being baked would GUARANTEE that tile T's apron and tile T+1's interior
   disagree, because the seed is exactly the thing that must not vary across a
   shared world region. ``roughness_seed`` hashes only
   (world_seed, bake_version). ``noise_anchor_pitch_fine_px`` > 0 re-anchors on
   a world lattice -- "derive the seed from world position" in the literal
   sense -- but any non-zero pitch puts a noise discontinuity on that lattice,
   so it is 0 in the shipped configuration.

3. **Every padded domain is congruent on the global fine lattice.** A tile's
   padded origin is ``t * fine_tile_px - apron_fine_px``, so two tiles' domains
   differ by an exact multiple of one tile (8192 fine px). That is what makes
   (1) implementable at all: the offset handed to the kernel is always an
   integer world lattice index, never a fractional phase.

Given all three the overlap is **bit-exact**, not approximately equal.
``noise.roughness``'s octave lattices are a splitmix64 counter hash of integer
world lattice coordinates -- no RNG walk, so no dependence on how many values
were drawn before -- and its normaliser is derived analytically from the octave
weights rather than as ``band.std()`` over the domain.

That second one deserves its own line, because it is a SEPARATE anchoring bug
from the RNG walk and it survives fixing the first: a domain-wide ``std()``
gives the same world location a different AMPLITUDE in two overlapping bakes
even when the noise field itself is world-anchored. The plan's seam test could
not see either one, because it injected a single shared pre-computed noise
field into every bake specifically to isolate apron adequacy.
"""


def _u64(v: int) -> int:
    return int(v) & 0xFFFFFFFFFFFFFFFF


def roughness_seed(
    world_seed: int,
    origin_cells: tuple[int, int] = (0, 0),
    *,
    bake_version: int = BAKE_VERSION,
    anchor_pitch_fine_px: int = 0,
) -> int:
    """The seed handed to ``noise.roughness``. Read ``NOISE_ANCHORING`` first.

    With the default ``anchor_pitch_fine_px == 0`` the result depends only on
    the world seed and the bake version -- deliberately NOT on which tile is
    being baked, because per-tile entropy is precisely what makes an apron and
    its neighbour's interior disagree. World POSITION is carried by
    ``origin_cells``, which the kernel takes directly; the seed's job is to
    separate worlds, not places.

    A positive pitch quantises the domain origin onto a world lattice and mixes
    it in. That is "derive the seed from world position" in the literal sense,
    and it is why the origin is an argument here at all, but it buys nothing
    and costs a noise discontinuity every ``anchor_pitch_fine_px`` px, so it is
    off by default.
    """
    if anchor_pitch_fine_px > 0:
        a0 = int(origin_cells[0]) // anchor_pitch_fine_px
        a1 = int(origin_cells[1]) // anchor_pitch_fine_px
    else:
        a0 = a1 = 0
    canonical = f"vxbake-roughness:{_u64(world_seed)}:{int(bake_version)}:{a0}:{a1}"
    return int.from_bytes(hashlib.sha256(canonical.encode("utf-8")).digest()[:8], "big")


#: ``noise.roughness``'s world-origin parameter. All spellings take
#: **(row0, col0)**, row-major, matching the arrays. The aliases exist only so
#: a test double can be written without importing the package.
_ORIGIN_KWARGS = ("origin_cells", "origin_px", "world_origin_px", "origin")

_ROUGHNESS_NO_ORIGIN = (
    "noise.roughness has no world-origin parameter (looked for "
    f"{_ORIGIN_KWARGS}). Without it the field is generated in ARRAY "
    "coordinates, so tile T's apron and tile T+1's interior disagree in their "
    "overlap for reasons no apron size can fix -- which is the prototype's "
    "defect, not a tolerance. Refusing to bake; see pipeline.NOISE_ANCHORING."
)


def roughness_origin_kwarg(roughness_fn: Callable) -> str | None:
    """Name of ``roughness``'s world-origin parameter, or None."""
    import inspect

    try:
        params = inspect.signature(roughness_fn).parameters
    except (TypeError, ValueError):  # builtins / numba dispatchers
        return None
    for name in _ORIGIN_KWARGS:
        if name in params:
            return name
    return None


# ---------------------------------------------------------------------------
# Kernels.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class BakeKernels:
    """The seven numerics entry points, injectable.

    Injection exists so the orchestration (apron cropping, seed derivation,
    inflow injection, flow-plane packing) can be exercised on a CI box with no
    numba/scipy, using reference implementations defined in the TEST module.
    Nothing here ships a fallback: ``load_kernels()`` is the only production
    path and it imports the real siblings.
    """

    carrier: Callable
    roughness: Callable
    fill_depressions: Callable
    d8_receivers: Callable
    accumulate_mfd: Callable
    stream_power: Callable
    relax: Callable


_MISSING_KERNELS = (
    "terrain_service.bake.{mod} is not importable ({err}). The bake numerics "
    "live in flow.py / noise.py / incise.py / thermal.py and need the "
    "terrain-diffusion venv (numba + scipy); pipeline.py itself deliberately "
    "imports none of it so the bake identity can be hashed on a bare box."
)


def load_kernels() -> BakeKernels:
    """Import the real numerics. Raises RuntimeError with a legible message."""
    try:
        from .flow import accumulate_mfd, d8_receivers, fill_depressions
    except ImportError as exc:  # pragma: no cover - depends on sibling agents
        raise RuntimeError(_MISSING_KERNELS.format(mod="flow", err=exc)) from exc
    try:
        from .noise import carrier, roughness
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(_MISSING_KERNELS.format(mod="noise", err=exc)) from exc
    try:
        from .incise import stream_power
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(_MISSING_KERNELS.format(mod="incise", err=exc)) from exc
    try:
        from .thermal import relax
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(_MISSING_KERNELS.format(mod="thermal", err=exc)) from exc
    return BakeKernels(
        carrier=carrier,
        roughness=roughness,
        fill_depressions=fill_depressions,
        d8_receivers=d8_receivers,
        accumulate_mfd=accumulate_mfd,
        stream_power=stream_power,
        relax=relax,
    )


# ---------------------------------------------------------------------------
# Coarse assembly.
# ---------------------------------------------------------------------------

#: Elevation used where a coarse tile has never been generated. Sea level is
#: the CONSERVATIVE choice: an absent neighbour becomes a sink that absorbs
#: flow rather than a ridge that invents upstream area. It underestimates
#: inflow and never overestimates it. See HYDROLOGY_RESIDUALS #1.
MISSING_ELEVATION_M = 0.0

CoarseFetch = Callable[[int, int], "np.ndarray | None"]


def assemble_padded_coarse(
    coarse_fetch: CoarseFetch,
    tile_x: int,
    tile_y: int,
    geom: BakeGeometry = PRODUCTION,
) -> tuple[np.ndarray, list[tuple[int, int]]]:
    """Build the (padded_coarse_px)^2 coarse domain for tile (x, y).

    Gathers the 3x3 coarse ring and crops the apron out of the neighbours.
    Returns (domain, missing) where ``missing`` lists the ring tiles that were
    not available -- an empty list is the only case in which the bake is
    order-independent (see HYDROLOGY_RESIDUALS #1).
    """
    n = geom.coarse_tile_px
    a = geom.apron_coarse_px
    dom = np.full((geom.padded_coarse_px, geom.padded_coarse_px), MISSING_ELEVATION_M, np.float32)
    missing: list[tuple[int, int]] = []
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            src = coarse_fetch(tile_x + dx, tile_y + dy)
            if src is None:
                missing.append((tile_x + dx, tile_y + dy))
                continue
            src = np.asarray(src, dtype=np.float32)
            if src.shape != (n, n):
                raise ValueError(
                    f"coarse tile ({tile_x + dx},{tile_y + dy}) has shape "
                    f"{src.shape}, expected ({n},{n})"
                )
            # Destination window of this ring tile inside the padded domain,
            # clipped to the domain (the corner/edge tiles contribute only
            # their apron strip).
            dy0, dx0 = a + dy * n, a + dx * n
            sy0 = max(0, -dy0)
            sx0 = max(0, -dx0)
            sy1 = min(n, dom.shape[0] - dy0)
            sx1 = min(n, dom.shape[1] - dx0)
            if sy0 >= sy1 or sx0 >= sx1:
                continue
            dom[dy0 + sy0 : dy0 + sy1, dx0 + sx0 : dx0 + sx1] = src[sy0:sy1, sx0:sx1]
    return dom, missing


# ---------------------------------------------------------------------------
# The hydrology pyramid.
# ---------------------------------------------------------------------------

HYDROLOGY_RESIDUALS = """\
What the pyramid still gets wrong. Recorded so nobody rediscovers it.

1. **Exploration order.** A superblock is built from whatever coarse tiles
   exist when the first tile inside it is baked. A river entering from a
   never-generated region contributes nothing (MISSING_ELEVATION_M puts a sea
   level sink there), and because a shipped tile is never regenerated, later
   exploration does NOT retroactively correct it. A world pre-generated with
   ``pregen.py --mode bake`` is order-independent because pregen builds every
   superblock over the requested radius before baking anything; an on-demand
   frontier is not. This is the largest residual, and it is as much a policy
   question as a numerical one.

2. **The top of the pyramid is open.** Level ``superblock_max_level`` receives
   no inflow at its own edges, so a catchment larger than that level's span
   (245 km at the default level 1) is truncated. The plan's answer is to seed
   the top level from terrain-diffusion's own coarse map at 7.7 km/px; the
   hook is ``build_flow_superblock(parent=...)`` and it is UNWIRED -- reading
   the model's coarse map needs WorldPipeline internals this service does not
   have. Until then the top level is the truncation point.

3. **Routing disagreement between levels.** Injected area arrives where the
   30 m D8 routing says it crosses, not where the 1.875 m MFD routing would
   have put it. The lateral error is bounded by one source cell (30 m) and
   heals within a few hundred metres downstream, but a channel can visibly
   kink at the injection point if the fine surface disagrees with its coarse
   parent about which side of an interfluve the water is on.

4. **MFD area vs D8 crossings.** ``inject_edge_inflow`` locates boundary
   crossings with the parent's single-receiver D8 field (so each flow path
   crosses exactly once and nothing is double counted), but the AMOUNT it
   injects is the parent's MFD accumulation, which may have split that path
   several ways. Where MFD diverges strongly just outside the boundary the
   injected total can exceed the true through-flow. Bounded by the divergence
   fraction; not measured.

5. **Fine rills below the coarse level.** A rill that meanders more than 960 m
   before rejoining a coarse-resolved channel can still kink at a tile edge --
   the plan's own predicted residual, sub-metre and 1-2 px wide.

6. **Wide flats route by epsilon staircase.** A flat wider than the apron can
   be crossed in different directions by two neighbouring bakes, because the
   staircase runs outward from whichever border the flood reached it from.
   Heights still agree (sub-ULP, well under the 100 mm wire LSB); accumulation
   need not. At 1.875 m/px "wider than the apron" is 512 px -- an ordinary
   lake. Detected per tile via ``basin_exceeds_apron``; see
   ``APRON_BLIND_SPOT``. The coarse superblock is the natural fix here too,
   since it is world-anchored and shared between neighbours, but that is not
   wired.
"""


@dataclass(frozen=True)
class FlowLevel:
    """One level of the hydrology pyramid.

    Level L: cell size ``coarse_pixel_m * superblock_tiles^L``, footprint
    ``superblock_tiles^(L+1)`` coarse tiles per side. The raster edge is
    therefore CONSTANT across levels (``superblock_tiles * coarse_tile_px``),
    which is what keeps the pyramid affordable -- each level costs the same
    priority-flood while covering 4x the ground per axis.
    """

    level: int
    geom: BakeGeometry = PRODUCTION
    consts: BakeConstants = CONSTANTS

    @property
    def downsample(self) -> int:
        """Coarse pixels per level pixel."""
        return self.consts.superblock_tiles**self.level

    @property
    def tiles_per_side(self) -> int:
        return self.consts.superblock_tiles ** (self.level + 1)

    @property
    def cell_m(self) -> float:
        return self.geom.coarse_pixel_m * self.downsample

    @property
    def size_px(self) -> int:
        return self.tiles_per_side * self.geom.coarse_tile_px // self.downsample

    @property
    def span_m(self) -> float:
        return self.tiles_per_side * self.geom.tile_span_m


def superblock_index(tile_x: int, tile_y: int, level: FlowLevel) -> tuple[int, int]:
    """Which superblock at ``level`` owns coarse tile (x, y).

    Floor division, so the grid is WORLD-anchored: two tiles that share an edge
    read the same superblock (or two superblocks that were each built from a
    world-anchored window), which is the whole reason this is a cached artifact
    rather than a per-tile neighbourhood computed on the fly. A per-tile window
    centred on the tile being baked would give the two sides of a shared edge
    different answers.
    """
    n = level.tiles_per_side
    return (int(tile_x) // n, int(tile_y) // n)


@dataclass
class FlowSuperblock:
    """Cached coarse-hydrology artifact for one world-anchored superblock."""

    level: int
    sx: int
    sy: int
    tiles_per_side: int
    cell_m: float
    #: World origin of ``acc[0, 0]`` in metres, (x, y).
    origin_m: tuple[float, float]
    #: MFD accumulation, m^2.
    acc: np.ndarray
    #: Depression-filled elevation, m. Kept so a child can recompute D8
    #: receivers for ``inject_edge_inflow`` without re-running the fill.
    filled: np.ndarray
    #: Coarse tiles that were unavailable when this was built.
    missing_tiles: tuple[tuple[int, int], ...] = ()

    @property
    def size_px(self) -> int:
        return int(self.acc.shape[0])


_FLOW_MAGIC = b"VXFL"
_FLOW_VERSION = 1
#: magic, version, seed, bake_ver, level, pad, sx, sy, tiles_per_side, size,
#: cell_m, origin_x_m, origin_y_m, n_missing
_FLOW_HEADER = struct.Struct("<4sHQHBBiiHIfddI")


def encode_flow_superblock(sb: FlowSuperblock, seed: int) -> bytes:
    """Serialise a superblock for the content-addressed cache.

    Deliberately trivial and self-describing: this is a server-side
    intermediate, not a wire format, and nothing in voxel-core parses it.
    """
    acc = np.ascontiguousarray(sb.acc, dtype="<f4")
    filled = np.ascontiguousarray(sb.filled, dtype="<f4")
    if acc.shape != filled.shape or acc.ndim != 2 or acc.shape[0] != acc.shape[1]:
        raise ValueError("flow superblock rasters must be square and same-shaped")
    head = _FLOW_HEADER.pack(
        _FLOW_MAGIC,
        _FLOW_VERSION,
        _u64(seed),
        BAKE_VERSION,
        sb.level,
        0,
        sb.sx,
        sb.sy,
        sb.tiles_per_side,
        acc.shape[0],
        float(sb.cell_m),
        float(sb.origin_m[0]),
        float(sb.origin_m[1]),
        len(sb.missing_tiles),
    )
    missing = np.asarray(sb.missing_tiles, dtype="<i4").reshape(-1, 2)
    return head + missing.tobytes() + acc.tobytes() + filled.tobytes()


def decode_flow_superblock(data: bytes) -> tuple[FlowSuperblock, int]:
    """Inverse of ``encode_flow_superblock``. Returns (superblock, seed)."""
    (
        magic,
        version,
        seed,
        bake_ver,
        level,
        _pad,
        sx,
        sy,
        tiles_per_side,
        size,
        cell_m,
        ox,
        oy,
        n_missing,
    ) = _FLOW_HEADER.unpack_from(data)
    if magic != _FLOW_MAGIC:
        raise ValueError("bad flow superblock magic")
    if version != _FLOW_VERSION:
        raise ValueError(f"unsupported flow superblock version {version}")
    if bake_ver != BAKE_VERSION:
        # Cannot happen through the cache (provider_id covers BAKE_VERSION),
        # but a stale file handed in directly must not be silently mixed in.
        raise ValueError(
            f"flow superblock was built by bake_ver {bake_ver}, this is {BAKE_VERSION}"
        )
    off = _FLOW_HEADER.size
    missing = np.frombuffer(data, dtype="<i4", count=2 * n_missing, offset=off)
    off += 8 * n_missing
    n = size * size
    acc = np.frombuffer(data, dtype="<f4", count=n, offset=off).reshape(size, size)
    off += 4 * n
    filled = np.frombuffer(data, dtype="<f4", count=n, offset=off).reshape(size, size)
    off += 4 * n
    if off != len(data):
        raise ValueError("trailing bytes in flow superblock")
    sb = FlowSuperblock(
        level=level,
        sx=sx,
        sy=sy,
        tiles_per_side=tiles_per_side,
        cell_m=cell_m,
        origin_m=(ox, oy),
        acc=np.array(acc),
        filled=np.array(filled),
        missing_tiles=tuple((int(a), int(b)) for a, b in missing.reshape(-1, 2)),
    )
    return sb, seed


def _mean_downsample(a: np.ndarray, factor: int) -> np.ndarray:
    if factor == 1:
        return a
    h, w = a.shape
    return a.reshape(h // factor, factor, w // factor, factor).mean(axis=(1, 3))


def build_flow_superblock(
    coarse_fetch: CoarseFetch,
    sx: int,
    sy: int,
    level: FlowLevel,
    kernels: BakeKernels,
    parent: "FlowSuperblock | None" = None,
) -> FlowSuperblock:
    """Coarse hydrology over one world-anchored superblock.

    The pyramid, bottom up:

      * level 0 assembles the superblock's coarse tiles at 30 m/px;
      * level L>0 assembles the same footprint scaled up by ``4^L`` tiles and
        mean-downsamples to ``30 * 4^L`` m/px, so the raster stays 2048^2 while
        the ground covered grows 4x per axis;
      * each level fills depressions, routes MFD, and (if ``parent`` is given)
        injects the parent's through-flow at its own edges first.

    Mean, not min, for the downsample: min-pooling biases every coarse cell to
    its channel and would route flow along an elevation field no real cell has.
    Mean keeps the drainage divides where the 30 m data put them, which is what
    the accumulation is for.
    """
    n_tiles = level.tiles_per_side
    tile_px = level.geom.coarse_tile_px
    ds = level.downsample
    if tile_px % ds:
        raise ValueError(
            f"level {level.level} downsample {ds} does not divide the coarse "
            f"tile ({tile_px} px)"
        )
    tx0, ty0 = sx * n_tiles, sy * n_tiles
    # Downsample each tile AS IT ARRIVES rather than assembling the full-res
    # mosaic first. At level 1 that is the difference between a 2048^2 working
    # array and an 8192^2 one, and level 2 would otherwise want 4.3 GB.
    sub = tile_px // ds
    z = np.full((level.size_px, level.size_px), MISSING_ELEVATION_M, np.float32)
    missing: list[tuple[int, int]] = []
    for j in range(n_tiles):
        for i in range(n_tiles):
            src = coarse_fetch(tx0 + i, ty0 + j)
            if src is None:
                missing.append((tx0 + i, ty0 + j))
                continue
            z[j * sub : (j + 1) * sub, i * sub : (i + 1) * sub] = _mean_downsample(
                np.asarray(src, np.float32), ds
            )
    origin_m = (tx0 * level.geom.tile_span_m, ty0 * level.geom.tile_span_m)

    filled = np.asarray(kernels.fill_depressions(z), dtype=np.float32)
    inflow = None
    if parent is not None:
        inflow = inject_edge_inflow(
            child_z=filled,
            child_origin_m=origin_m,
            child_cell_m=level.cell_m,
            src=parent,
            d8_fn=kernels.d8_receivers,
        )
    acc = np.asarray(
        kernels.accumulate_mfd(
            filled, level.cell_m, p=level.consts.mfd_p, inflow=inflow
        ),
        dtype=np.float32,
    )
    return FlowSuperblock(
        level=level.level,
        sx=sx,
        sy=sy,
        tiles_per_side=n_tiles,
        cell_m=level.cell_m,
        origin_m=origin_m,
        acc=acc,
        filled=filled,
        missing_tiles=tuple(missing),
    )


def inject_edge_inflow(
    child_z: np.ndarray,
    child_origin_m: tuple[float, float],
    child_cell_m: float,
    src: FlowSuperblock,
    d8_fn: Callable,
) -> np.ndarray:
    """Upstream area entering the child domain, as an m^2 field on the child.

    The construction, and why it is exactly conservative:

      * take the parent's SINGLE-receiver D8 field. Every flow path therefore
        crosses the child's boundary exactly once, at the unique parent cell
        that is outside the child and whose receiver is inside it;
      * that cell delivers its whole accumulated area. Nothing upstream of it
        is counted separately, because those cells' receivers are outside;
      * deposit the amount in the LOWEST child cell inside the receiving
        parent cell's footprint, i.e. the thalweg. Mass is preserved exactly
        and the water starts in the channel rather than smeared across 16 px
        of bank.

    The result is meant for ``accumulate_mfd(..., inflow=...)``, which treats
    it as extra drainage area (m^2) present at that cell before routing.

    See HYDROLOGY_RESIDUALS #3 and #4 for what this still gets wrong.
    """
    child_z = np.asarray(child_z, dtype=np.float32)
    h, w = child_z.shape
    inflow = np.zeros((h, w), np.float32)

    scell = float(src.cell_m)
    sox, soy = src.origin_m
    sh, sw = src.acc.shape
    ox, oy = child_origin_m
    x1 = ox + w * child_cell_m
    y1 = oy + h * child_cell_m

    # Parent cells whose CENTRE lies inside the child extent.
    cx = sox + (np.arange(sw, dtype=np.float64) + 0.5) * scell
    cy = soy + (np.arange(sh, dtype=np.float64) + 0.5) * scell
    inside_x = (cx >= ox) & (cx < x1)
    inside_y = (cy >= oy) & (cy < y1)
    inside = inside_y[:, None] & inside_x[None, :]
    if not inside.any():
        return inflow

    rec = np.asarray(d8_fn(src.filled, scell)[0]).reshape(-1)
    flat_inside = inside.reshape(-1)
    valid = rec >= 0
    # An entry cell: outside the child, draining to a cell inside it.
    entry = np.zeros(rec.shape, bool)
    entry[valid] = (~flat_inside[valid]) & flat_inside[rec[valid]]
    idx = np.flatnonzero(entry)
    if idx.size == 0:
        return inflow

    acc_flat = np.asarray(src.acc, dtype=np.float32).reshape(-1)
    tgt = rec[idx]
    # Perimeter-sized loop (a few thousand cells at most): the argmin over a
    # parent cell's child footprint does not vectorise cleanly and is not on
    # any hot path.
    for src_cell, amount in zip(tgt.tolist(), acc_flat[idx].tolist()):
        si, sj = divmod(int(src_cell), sw)
        # Child index window covering this parent cell.
        j0 = int(np.floor((sox + sj * scell - ox) / child_cell_m))
        j1 = int(np.ceil((sox + (sj + 1) * scell - ox) / child_cell_m))
        i0 = int(np.floor((soy + si * scell - oy) / child_cell_m))
        i1 = int(np.ceil((soy + (si + 1) * scell - oy) / child_cell_m))
        i0, i1 = max(0, i0), min(h, i1)
        j0, j1 = max(0, j0), min(w, j1)
        if i0 >= i1 or j0 >= j1:
            continue
        window = child_z[i0:i1, j0:j1]
        li, lj = np.unravel_index(int(np.argmin(window)), window.shape)
        inflow[i0 + li, j0 + lj] += np.float32(amount)
    return inflow


# ---------------------------------------------------------------------------
# The flow plane (.vxtl v2 section 6).
# ---------------------------------------------------------------------------


def _max_axis_run(mask: np.ndarray) -> int:
    """Longest run of True along either axis.

    A cheap stand-in for "widest connected flat", chosen because it needs no
    scipy (CI has none) and because the quantity that matters is a WIDTH
    against the apron, not a component label. It under-reports a diagonal flat
    and over-reports two flats that share a row, both of which are acceptable
    for a detector whose job is to say "look at this tile".
    """
    if mask.size == 0 or not mask.any():
        return 0
    best = 0
    for axis in (0, 1):
        m = mask if axis == 1 else mask.T
        # Run lengths per row via a cumulative counter reset on False.
        run = np.zeros(m.shape, np.int32)
        run[:, 0] = m[:, 0]
        for j in range(1, m.shape[1]):
            run[:, j] = (run[:, j - 1] + 1) * m[:, j]
        best = max(best, int(run.max()))
    return best


def flow_plane(
    acc: np.ndarray,
    incision_m: np.ndarray,
    thermal_gain_m: np.ndarray,
    consts: BakeConstants = CONSTANTS,
) -> np.ndarray:
    """Pack the optional uint8 flow plane.

    bits 0-4 = log2(accumulation in m^2) clamped 0-31, bit 5 = channel,
    bit 6 = bank, bit 7 = deposition (docs/vxtl-v2-format.md section 6).
    """
    acc = np.asarray(acc, dtype=np.float32)
    with np.errstate(divide="ignore", invalid="ignore"):
        mag = np.log2(np.maximum(acc, 1.0))
    out = np.clip(mag, 0, 31).astype(np.uint8)

    channel = (acc >= consts.channel_area_m2) | (
        np.asarray(incision_m, dtype=np.float32) >= consts.channel_depth_m
    )
    out |= channel.astype(np.uint8) << 5

    # A bank is a cell touching a channel but not in it -- the 8-neighbour
    # dilation minus the channel itself. Zero-padded shifts, NOT np.roll: roll
    # wraps, which would paint a bank on the north edge from a channel on the
    # south one.
    h, w = channel.shape
    pad = np.zeros((h + 2, w + 2), bool)
    pad[1:-1, 1:-1] = channel
    dil = np.zeros_like(channel)
    for di in (0, 1, 2):
        for dj in (0, 1, 2):
            dil |= pad[di : di + h, dj : dj + w]
    out |= (dil & ~channel).astype(np.uint8) << 6

    out |= (np.asarray(thermal_gain_m, dtype=np.float32) >= consts.deposition_m).astype(
        np.uint8
    ) << 7
    return out


# ---------------------------------------------------------------------------
# The bake.
# ---------------------------------------------------------------------------


APRON_BLIND_SPOT = """
Which stage the apron does NOT bound -- attributed by measurement, not guessed.

MEASURED through this pipeline against the prototype's real numba/scipy
kernels: two adjacent 1.92 km tiles at 1.875 m/px with the full 960 m apron,
each compared stage by stage against a single domain spanning both. Interior
only, i.e. the bytes that would actually ship:

    stage                        |per-tile - single-domain|  mean / max
    B0+B1 carrier + roughness         0.000000 m / 0.0000 m   BIT-IDENTICAL
    B2a depression fill               0.000000 m / 0.0000 m   BIT-IDENTICAL
    B2c MFD accumulation            607.6 m2    / 121,661 m2
    B2d incision                      0.071937 m / 2.8540 m
    B3  final surface                 0.071952 m / 2.0190 m

Read the first two rows before the last three. **Everything bounded is exact**
-- not "small error", bit-identical, including the B-spline prefilter (an IIR
pass over the whole array) and the priority-flood (which is a global algorithm
and could have been non-local in principle, and measurably is not at this
apron). The apron does exactly what the plan claims.

**100% of the residual enters at accumulation**, and then rides through
incision into the surface. That is the one dependency the plan already names as
unbounded, now isolated to a single stage rather than inferred from an
end-of-pipeline number: the plan's own seam test compared only the final height
and the accumulation field, so it could confirm the total but not attribute it.

The size of it depends entirely on whether catchments close inside the domain:

    terrain                  acc ratio vs truth      final height error
    draining (global tilt)   1.000 median/p01/min    0.0000 m mean / 0.00018 m max
    basin-rich, no outlet    0.511 median            0.0719 m mean / 2.0190 m max

The draining row is the same regime as the plan's own 960 m measurement (real
diffusion tiles drain to sea level) and reproduces its 0.1 mm. The basin-rich
row is an adversarial synthetic case and is what the hydrology pyramid is for:
feeding the same two tiles from a level-0 superblock moved the median ratio
0.511 -> 0.574 with 0.47 and 2.69 km2 injected, i.e. the mechanism works and
its reach is set by the superblock span (7.7 km in that harness against 61 km
in production, and 245 km at level 1).

**A separate, routing-only case this pipeline could NOT test.** The epsilon
fill's staircase across a flat runs outward from whichever border the flood
reached it from, so a flat WIDER THAN THE APRON can be crossed in different
directions by two neighbouring bakes. In elevation that is sub-ULP and
disappears at the 100 mm wire LSB, so shipped heights still agree; accumulation
and therefore incision need not. The prototype kernels used for the
measurements above do a plain fill with no epsilon, so none of those numbers
speak to it.

How wide before it bites: **about 960 m, which at 1.875 m/px is 512 px -- an
ordinary lake, floodplain or playa, not a corner case.** That is why every
BakeResult reports ``max_basin_run_m`` and ``basin_exceeds_apron``: the
condition is cheap to detect even though it is not cheap to fix. Genuinely
tile-spanning water bodies want a real lake/outlet model, which is beyond
Phase 2. The principled fix has the same shape as the pyramid -- the coarse
superblock is world-anchored and SHARED between neighbours, so its routing
across a flat is consistent by construction.

``interior_dead_ends`` is the companion detector, and it has already earned its
place: run against the prototype's plain (epsilon-free) fill it reports 357 and
1,719 interior cells with no receiver on those two tiles, which is exactly the
level-lake failure the epsilon variant exists to remove.
"""


@dataclass
class BakeResult:
    """Interior-only outputs, plus the numbers worth recording."""

    tile_x: int
    tile_y: int
    #: (fine_tile_px, fine_tile_px) float32 metres -- the SAMPLES. The .vxtl v2
    #: encoder turns these into B-spline control points; that prefilter lives
    #: in tile_codec.py, not here.
    elevation_m: np.ndarray
    #: MFD accumulation, m^2, interior.
    accumulation_m2: np.ndarray
    #: uint8 flow plane, interior.
    flow: np.ndarray
    #: process_time() per stage. NEVER wall-clock: this box is contended.
    cpu_seconds: dict[str, float]
    #: Diagnostics that a contended box cannot distort (counts and extents).
    stats: dict[str, float]
    #: Coarse ring tiles that were unavailable.
    missing_coarse: tuple[tuple[int, int], ...] = ()


def bake_padded_domain(
    padded_coarse: np.ndarray,
    *,
    world_seed: int,
    tile_x: int,
    tile_y: int,
    kernels: BakeKernels,
    geom: BakeGeometry = PRODUCTION,
    consts: BakeConstants = CONSTANTS,
    inflow_source: "FlowSuperblock | None" = None,
) -> dict:
    """Run B0-B3 over the padded domain and return every padded grid.

    Separated from ``bake_tile`` so a seam test can compare a per-tile bake's
    apron against a neighbour's interior directly, which is the check that
    caught the array-coordinate fBm in the first place.
    """
    if padded_coarse.shape != (geom.padded_coarse_px, geom.padded_coarse_px):
        raise ValueError(
            f"padded coarse domain is {padded_coarse.shape}, expected "
            f"{(geom.padded_coarse_px, geom.padded_coarse_px)}"
        )
    cell_m = geom.fine_pixel_m
    cpu: dict[str, float] = {}

    def tick(name, c0):
        cpu[name] = time.process_time() - c0

    # -- B0: C2 carrier. The B-spline prefilter is an IIR pass over the WHOLE
    # array, so the carrier itself has a domain dependence -- which is one of
    # the things the 960 m apron measurement covers, since the seam test ran
    # this same code path.
    c0 = time.process_time()
    fine = np.asarray(kernels.carrier(padded_coarse, geom.scale), dtype=np.float32)
    tick("B0.carrier", c0)
    if fine.shape != (geom.padded_fine_px, geom.padded_fine_px):
        raise ValueError(
            f"carrier() returned {fine.shape}, expected "
            f"{(geom.padded_fine_px, geom.padded_fine_px)}"
        )

    # -- B1: world-anchored conditioned roughness.
    #
    # origin_cells is the PADDED domain's world fine-cell origin, apron
    # included, because the apron is part of the array the kernel sees. Passing
    # the tile interior's origin instead would offset the noise by exactly the
    # apron (960 m) and reintroduce the seam this whole mechanism removes, so
    # it is worth the extra line to be explicit about which one it is.
    c0 = time.process_time()
    gy, gx = np.gradient(fine, cell_m)
    slope = np.hypot(gx, gy).astype(np.float32)
    origin_cells = geom.padded_origin_cells(tile_x, tile_y)
    seed = roughness_seed(
        world_seed,
        origin_cells,
        bake_version=BAKE_VERSION,
        anchor_pitch_fine_px=consts.noise_anchor_pitch_fine_px,
    )
    origin_kw = roughness_origin_kwarg(kernels.roughness)
    if origin_kw is None:
        raise RuntimeError(_ROUGHNESS_NO_ORIGIN)
    delta = np.asarray(
        kernels.roughness(
            fine,
            cell_m,
            slope,
            seed,
            src_nyquist_m=consts.src_nyquist_m,
            **{origin_kw: origin_cells},
        ),
        dtype=np.float32,
    )
    fine = fine + delta
    tick("B1.roughness", c0)

    # -- B2a: depressions. The epsilon variant is the DEFAULT and must stay
    # that way: a plain fill makes every filled pit a level lake, and on a
    # level lake no cell has a lower neighbour, so MFD terminates there.
    c0 = time.process_time()
    fill_kwargs = {} if consts.flat_eps is None else {"flat_eps": consts.flat_eps}
    filled = np.asarray(
        kernels.fill_depressions(fine, **fill_kwargs), dtype=np.float32
    )
    tick("B2a.fill_depressions", c0)

    # -- B2b: D8 for the channel-defining slope. (D8 is right for a
    # centreline and wrong for the area field; MFD does the area below.)
    c0 = time.process_time()
    rec, d8_slope = kernels.d8_receivers(filled, cell_m)
    d8_slope = np.asarray(d8_slope, dtype=np.float32)
    # After an epsilon fill, receiver == -1 means "border cell draining out of
    # the domain" and nothing else, so an INTERIOR -1 is a routing bug rather
    # than a landscape feature. Counted here (a count, not a timing, so box
    # contention cannot touch it) and surfaced in stats; pregen refuses a run
    # that produces any.
    rec = np.asarray(rec)
    interior_dead_ends = int((rec[1:-1, 1:-1] < 0).sum())
    del rec
    tick("B2b.d8_receivers", c0)

    # -- B2c: MFD area, with the pyramid's inflow at the domain edge. THIS is
    # the unbounded dependency the apron cannot cover.
    c0 = time.process_time()
    inflow = None
    if inflow_source is not None:
        inflow = inject_edge_inflow(
            child_z=filled,
            child_origin_m=geom.padded_origin_m(tile_x, tile_y),
            child_cell_m=cell_m,
            src=inflow_source,
            d8_fn=kernels.d8_receivers,
        )
    # accumulate_mfd returns float64 m^2. Incision consumes it at full width;
    # the STORED field is narrowed to float32 (measured to agree to 2 ppm, and
    # 340 MB against 680 MB over the padded domain matters here) since its
    # consumers are a log2 flow plane and threshold tests.
    acc64 = np.asarray(
        kernels.accumulate_mfd(filled, cell_m, p=consts.mfd_p, inflow=inflow)
    )
    tick("B2c.accumulate_mfd", c0)

    # -- B2d: stream-power incision.
    c0 = time.process_time()
    depth = np.asarray(
        kernels.stream_power(
            acc64,
            d8_slope,
            K=consts.stream_K,
            m=consts.stream_m,
            n=consts.stream_n,
            cap_m=consts.incision_cap_m,
            a_crit_m2=consts.channel_init_area_m2,
            gate_q=consts.channel_init_q,
            elev_m=filled,
            sea_taper_top_m=consts.sea_taper_top_m,
            sea_taper_bottom_m=consts.sea_taper_bottom_m,
        ),
        dtype=np.float32,
    )
    eroded = filled - depth
    tick("B2d.stream_power", c0)

    # -- B3: slope-limited thermal relaxation, AFTER incision so gully walls
    # weather and spoil forms talus.
    c0 = time.process_time()
    z = np.asarray(
        kernels.relax(
            eroded,
            cell_m,
            repose_deg=consts.repose_deg,
            iters=consts.thermal_iters,
            rate=consts.thermal_rate,
        ),
        dtype=np.float32,
    )
    tick("B3.relax", c0)

    return {
        "z": z,
        "carrier_plus_roughness": fine,
        "filled": filled,
        "eroded": eroded,
        "acc": acc64.astype(np.float32),
        "incision": depth,
        "thermal_gain": (z - eroded).astype(np.float32),
        "d8_slope": d8_slope,
        "inflow": inflow,
        "roughness_seed": seed,
        "interior_dead_ends": interior_dead_ends,
        "cpu_seconds": cpu,
    }


def bake_tile(
    *,
    world_seed: int,
    tile_x: int,
    tile_y: int,
    coarse_fetch: CoarseFetch,
    kernels: "BakeKernels | None" = None,
    geom: BakeGeometry = PRODUCTION,
    consts: BakeConstants = CONSTANTS,
    inflow_source: "FlowSuperblock | None" = None,
) -> BakeResult:
    """Bake one coarse tile's fine tier. Interior only; the apron is discarded.

    ``coarse_fetch(x, y)`` returns that coarse tile's elevation in METRES as a
    (coarse_tile_px, coarse_tile_px) array, or None if it does not exist.
    ``inflow_source`` is the level-0 flow superblock covering this tile; None
    means "no cross-tile hydrology", which is only correct for a test.
    """
    kernels = kernels or load_kernels()
    padded, missing = assemble_padded_coarse(coarse_fetch, tile_x, tile_y, geom)
    out = bake_padded_domain(
        padded,
        world_seed=world_seed,
        tile_x=tile_x,
        tile_y=tile_y,
        kernels=kernels,
        geom=geom,
        consts=consts,
        inflow_source=inflow_source,
    )
    sl = geom.interior()
    z = np.ascontiguousarray(out["z"][sl, sl])
    acc = np.ascontiguousarray(out["acc"][sl, sl])
    incision = out["incision"][sl, sl]
    gain = out["thermal_gain"][sl, sl]
    plane = flow_plane(acc, incision, gain, consts)

    basin = (out["filled"] - out["carrier_plus_roughness"])[sl, sl] > 0.0
    run_px = _max_axis_run(basin)
    stats = {
        # Extents and counts only -- a contended box cannot distort these.
        "relief_m": float(z.max() - z.min()),
        "max_accumulation_km2": float(acc.max()) / 1e6,
        "cells_above_1km2": float((acc > 1e6).sum()),
        "incision_mean_m": float(incision.mean()),
        "incision_p99_m": float(np.percentile(incision, 99)),
        "channel_cells": float((plane & 0x20 != 0).sum()),
        "injected_inflow_km2": (
            float(out["inflow"].sum()) / 1e6 if out["inflow"] is not None else 0.0
        ),
        # A routing bug after an epsilon fill, not a landscape feature.
        "interior_dead_ends": float(out["interior_dead_ends"]),
        # THE APRON'S BLIND SPOT, per tile. See APRON_BLIND_SPOT.
        "basin_cells_frac": float(basin.mean()),
        "max_basin_run_m": float(run_px) * geom.fine_pixel_m,
        "basin_exceeds_apron": float(run_px * geom.fine_pixel_m > geom.apron_m),
        "peak_bytes_estimate": float(estimate_peak_bytes(geom)),
    }
    return BakeResult(
        tile_x=tile_x,
        tile_y=tile_y,
        elevation_m=z,
        accumulation_m2=acc,
        flow=plane,
        cpu_seconds=out["cpu_seconds"],
        stats=stats,
        missing_coarse=tuple(missing),
    )
