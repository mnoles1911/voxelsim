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
    profile_incision(filled, receivers, acc, cell_m, K_dt=..., slope=...) -> eroded_z
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

THREE THINGS THE APRON DOES **NOT** SOLVE
-----------------------------------------
1. **Roughness anchoring.** An apron only helps if the field being computed is
   a function of *world* position. fBm generated in array coordinates is not:
   two overlapping domains disagree in their overlap for reasons that have
   nothing to do with aprons. See ``roughness_seed`` and ``NOISE_ANCHORING``.
2. **Flow accumulation is unbounded.** No practical apron captures a river
   whose catchment spans many tiles. See ``FlowLevel`` /
   ``build_flow_superblock`` / ``inject_edge_inflow`` for the hydrology
   pyramid, and ``HYDROLOGY_RESIDUALS`` for what it still gets wrong.
3. **The depression fill is unbounded too, and it was thought not to be.**
   A cell's filled elevation is the minimum over all paths to the DOMAIN
   BORDER of the highest point on that path, so cutting the domain closer does
   not truncate information, it *invents an outlet*. Measured on real tile
   (-5,2): widening the apron from 960 m to 1920 m moves **1.05% of the shipped
   interior by more than the 100 mm wire LSB**, by up to 78.79 m, with the
   influence reaching ~3.8 km inward. ``APRON_BLIND_SPOT`` has the table, the
   distance decay, and why ``inject_edge_inflow`` cannot fix it (it runs after
   the fill).

The first two were the plan's own prediction; ``APRON_BLIND_SPOT`` records the
stage-by-stage measurement that isolated them, and the later one that found the
third. The carrier and the roughness are bit-identical between a per-tile bake
and a wider-domain bake on real terrain -- that part of the apron argument is
exactly right and is now confirmed against the real kernels, not just the
prototype's.

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
from dataclasses import dataclass, field
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
    "STAGE_SINK_FIELDS",
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
    "superblock_inputs_fingerprint",
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
#:
#: 1 -> 2 (2026-07-29, Wave E): the flow plane's magnitude field is now gated
#: (``flow_mag_min_area_m2``) and its channel flag tapers below sea level
#: (``flow_flag_sea_taper``); ``FlowSuperblock`` carries an inputs fingerprint
#: and the .vxfl container grew a field for it. The constants roll the id on
#: their own; the counter moves because the .vxfl layout and the flow-plane
#: packing code changed with them.
#:
#: 2 -> 3 (2026-07-29): B2d incision is now the IMPLICIT profile solve
#: (``incise.profile_incision``, ``incision_mode = "profile"``) rather than the
#: per-cell depth law. The depth law paints depth onto the carrier and cannot
#: re-grade a channel's long profile, which left the sub-30 m band with no
#: concavity (theta 0.028-0.086 against 0.177-0.318 for matched real 1 m DTMs
#: -- docs/terrain-validation-2026-07.md section 7.1, confirmed per-stage by
#: the 2026-07-29 stage dumps). ``incision_mode = "depth"`` reproduces the
#: bake_ver-2 surface exactly. The constants roll the id on their own; the
#: counter moves because the B2d code path changed with them.
#:
#: 3 -> 4 (2026-07-29, ridge deficit): the shipped defaults change to the
#: measured composite that closes the 6-10x ridge+peak deficit on gentle
#: ground -- ``incision_mode = "profile"``, ``channel_init_area_m2 = 156``,
#: ``profile_regional_p = 2.0``, ``b1_constructional_amp = 0.40`` -- and the
#: B1/B2d code grew the two default-off mechanisms those constants switch on
#: (crest-up constructional octaves gated to gentle ground; a separately
#: settable exponent on the regional-energy factor). Measured on the pinned
#: 060b0c927ccc807e/000000000135276f exemplars at the matched 10-cell lookout:
#: plains ridge+peak 0.0158 -> 0.0443 (real Illinois 0.0434), valley+pit
#: 0.0118 -> 0.0336 (real 0.034-0.038), mean slope 2.13 deg (real 2.134), with
#: the slope-by-scale ladder within 11.7% of Illinois at every rung (base:
#: 11.8%); alpine 0.0030 -> 0.0213 and rolling 0.0034 -> ~0.05 (real alpine
#: 0.0332 -- improved 6-7x, still short; see the B2d comment). The old
#: surfaces are reproducible exactly with the bake_ver-3 constants.
#:
#: 4 -> 5 (2026-07-30, contour corduroy): B3's angle of repose becomes a
#: per-cell MATERIAL STRENGTH FIELD (``noise.repose_field``: world-anchored
#: spatial lithology octaves plus elevation-keyed strata bands), consumed by
#: ``thermal.relax``'s new 2-D ``repose_deg`` form. Measured motivation: with
#: one global angle, B3 takes the broad slope distribution incision hands it
#: (g35 exemplar p90 grade 1.25) and returns a third of the mountainside
#: within +-10% of tan(36 deg) -- and a constant-slope face voxelises to the
#: evenly spaced parallel contour terraces the owner rejects, upstream of
#: every client term (docs/measurements/contour-crookedness-2026-07-30.txt).
#: ``repose_spatial_amp_deg = repose_strata_amp_deg = 0`` with repose_deg 36
#: reproduces the bake_ver-4 surface exactly. The constants roll the id on
#: their own; the counter moves because thermal.relax and the B3 call grew the
#: field path.
#:
#: 5 -> 6 (2026-07-30, contour corduroy, second front): the SAME material
#: strength field now modulates B2d INCISION (``noise.repose_erodibility`` ->
#: ``incise.profile_incision(erodibility=...)``), keyed on the filled surface
#: being carved. Motivation, from the owner's in-engine verdict on bake_ver 5:
#: the repose field restructures faces where thermal BINDS (gullies --
#: confirmed by eye), but thermal is translation-invariant on a uniform
#: sub-threshold ramp, so open faces kept their banding. Strength-modulated
#: incision extends the structure below the repose regime: streams crossing
#: strong strata hold their bed (knickpoints) while weak bands cut treads, so
#: slope now varies at strata wavelength wherever there is any drainage at
#: all, not only where thermal moves mass. ``incision_strength_ratio = 1``
#: reproduces the bake_ver-5 surface exactly. The constant rolls the id on its
#: own; the counter moves because the B2d call grew the field path.
#: Also at 6 (same unshipped series): a B3b micro-refill -- the B2a epsilon
#: fill re-run on the relaxed surface -- because thermal can receive-overfill
#: a channel cell into a dam (its stability rule bounds what a cell GIVES, not
#: what it receives; measured at one 41 mm interior sink on the exemplar), and
#: "the carrier drains" is a contract, not a statistic. STAGE_ORDER carries
#: the new stage, so the identity rolls with it. The refill then moved to
#: B4b behind the B4 meso stage (post-thermal 15/7.5 m relief, steep-gated;
#: noise.meso_relief carries the full why), and incision_strength_ratio
#: shipped at 3 rather than 6 -- at 6 the knickpoint treads it makes are flat
#: enough to fall under the codec's 100 mm quantization floor, which
#: manufactures reconstruction-level pits the raw surface does not have
#: (measured: 1 carrier sink at ratio 6, 0 at ratio 3, same window).
BAKE_VERSION = 6


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
    #: B1 constructional term (``noise.roughness``'s crest-up folded octaves,
    #: gated to gentle ground). 0 disables and reproduces the prior surface
    #: bit-for-bit. Units: multiples of the substrate reference amplitude per
    #: octave. See the ridge-deficit investigation: gentle real landscapes get
    #: their fine-scale ridge/knoll relief from CONSTRUCTIONAL processes
    #: (till knolls, hummocky moraine), which fBm cannot supply (symmetric, no
    #: crests) and which incision must not supply either (a plain's erosion is
    #: sub-metre by construction).
    #:
    #: 0.40 measured 2026-07-29 on the plains exemplar (-55,20): ridge+peak at
    #: the matched 10-cell lookout 0.0158 -> 0.0443 (real Illinois 0.0434,
    #: Llano 0.0484), valley+pit 0.0118 -> 0.0336 (real 0.034-0.038), mean
    #: slope 2.13 deg against Illinois' 2.134, slope-by-scale within 11.7% at
    #: every rung. Its ridge cells sit at 1.70x the median distance-to-channel
    #: (interfluve/knoll-positioned, not painted: routing runs AFTER B1 and
    #: organises around the knolls). 0.30 measured 0.0319 (under the real
    #: band); 0.55+ overshoots the ladder. NOT swept above 0.55 at production:
    #: the S1a-level slope overshoot is absorbed by fill+incision+thermal, so
    #: calibrate against the BAKED surface, not B0+B1.
    b1_constructional_amp: float = 0.40
    #: Regional (carrier) slope below which the constructional term is at full
    #: strength, and at/above which it is zero, with a linear fade between.
    #: Steep ground is erosional: its ridges must be interfluves left by the
    #: incision passes, and folded noise there reads as crumpled paper with
    #: drainage-uncorrelated ridges (measured; see noise.roughness).
    b1_constructional_slope_lo: float = 0.10
    b1_constructional_slope_hi: float = 0.30

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
    #:
    #: MEASURED NOT GUILTY of the concavity deficit (2026-07-29 stage dumps):
    #: it binds only at A >= 1e6 m^2 -- 0.06% of channel cells on the plains
    #: exemplar, 0.4% rolling, 2.5% alpine -- and removing it moved theta by
    #: 2e-5. It is kept because the profile solve makes deep coherent canyons
    #: REACHABLE (K_dt = 15 uncapped measured 679 m of incision); under
    #: ``incision_mode = "profile"`` it bounds total lowering per cell while
    #: the solve's own z >= z(receiver) guarantee is what prevents shafts.
    incision_cap_m: float = 25.0
    #: B2d formulation. "profile" = ``incise.profile_incision``, the implicit
    #: stream-power solve along the D8 tree: same law, same gate, same taper,
    #: but the carve is a GRADED LONG PROFILE (concave, monotone, no
    #: carve-created pits) instead of a per-cell depth. "depth" reproduces the
    #: bake_ver-2 surface exactly, and is the DEFAULT -- deliberately, after
    #: measuring both at production scale on the three exemplar tiles:
    #:
    #:   * the depth law leaves the invented sub-30 m band with no concavity
    #:     (theta 0.040-0.065 against 0.177-0.318 for matched real 1 m DTMs)
    #:     and moves it AWAY from real (B0+B1 0.083 -> B2d 0.040 on alpine);
    #:   * the profile solve at K_dt=1.5 UNSCALED took alpine theta to 0.143
    #:     (r^2 0.96, target 0.177) and put the plains geomorphon fractions in
    #:     or near the real range -- but tripled the plains mean slope
    #:     (1.82 -> 5.36 deg at 1.875 m against Illinois' 2.13) because a
    #:     no-uplift solve grades every big catchment toward a peneplain,
    #:     which digs 12 m trenches through a till plain;
    #:   * with the regional-energy scale (``profile_regional_s_ref``) the
    #:     trade improves, production-measured on all three exemplars at
    #:     K_dt 4.5: alpine theta 0.146 (r^2 0.97, real 0.177), alpine
    #:     valley+pit 0.0070 -> 0.0166 (real 0.023), plains valley+pit
    #:     0.0103 -> 0.0338 (real 0.034-0.038, IN range), rolling theta
    #:     0.048 -> 0.070 with valley+pit doubled -- at plains mean slope
    #:     1.82 -> 2.78 deg at 1.875 m (Illinois reads 2.13: a 16% deficit
    #:     became a 30% excess) -- at ``profile_regional_p`` = stream_n, NO
    #:     measured configuration met the acceptance bars on every class at
    #:     once, which is why bake_ver 3 shipped with "depth";
    #:   * ``profile_regional_p = 2.0`` (2026-07-29, ridge deficit) resolves
    #:     that blocker: the sharper exponent leaves a till plain 2.3% of the
    #:     erosion energy instead of 22%, so the dense network carves
    #:     sub-metre swales (plains carved-past-0.1m fraction 6.5%, mean
    #:     slope 2.13 deg = Illinois' 2.134) while steep ground, at or above
    #:     ``profile_regional_s_ref``, erodes exactly as before.
    #:
    #: "profile" became the default at bake_ver 4 with that scorecard in hand.
    incision_mode: str = "profile"
    #: Erosion number for the profile solve: K times the pass's pseudo-time.
    #: Small values reproduce the explicit law to first order (0.15 measured
    #: theta 0.084 on the alpine exemplar window -- barely moved); large
    #: values run every channel to the steady-state graded profile (15
    #: measured theta with a broken fit and, uncapped, 679 m of incision).
    #: 4.5 with the regional scale below is the measured compromise --
    #: production-confirmed alpine theta 0.146 (r^2 0.97). On the steep
    #: classes the cap, not this constant, then sets the carve: 65-82% of
    #: channel cells reach ``incision_cap_m`` (against 0.4-2.5% under the
    #: depth law), so ``max`` incision is fully censored there and the cap is
    #: the depth knob that matters.
    profile_K_dt: float = 4.5
    #: Regional-slope reference for the profile solve's energy scale:
    #: erodibility is multiplied by ``min(1, S_30m / this)^n`` where S_30m is
    #: the carrier's 30 m-scale slope. This is the solve's stand-in for the
    #: missing uplift term -- erosion energy follows regional relief -- and it
    #: is what keeps a plain a plain while the mountains re-grade. 0 disables.
    profile_regional_s_ref: float = 0.2
    #: Exponent of the regional-energy factor. 0 = use ``stream_n`` (the prior
    #: behaviour). Above ``stream_n`` it sharpens the gentle/steep separation:
    #: at n = 0.8 a till plain keeps 22% of the erosion energy, which under a
    #: dense channel network (low ``channel_init_area_m2``) trenches it
    #: (measured 2.03 -> 3.59 deg mean slope at 1.875 m against a real 2.13);
    #: at 2.0 it keeps 2.3% and the same network carves sub-metre swales.
    #: Ground at or above ``profile_regional_s_ref`` is unaffected.
    #: 2.0 is the bake_ver-4 production value; it is what lets
    #: ``channel_init_area_m2`` sit at 156 without trenching a plain.
    profile_regional_p: float = 2.0
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
    #:
    #: 1e4 -> 156 (2026-07-29, ridge deficit; bake_ver 4). Drainage density is
    #: also INTERFLUVE density: at the matched 10-cell (18.75 m) lookout a rib
    #: crest only classifies as ridge when the gullies either side are within
    #: a lookout of it, so ridge+peak tracks a_crit directly (alpine exemplar:
    #: 0.0042 at 1e4, 0.0155 at 625, 0.0213 at 156; real Teton DTM 0.0332 --
    #: the largest lever found, and it saturates below the real value, see
    #: the module-level RIDGE_DEFICIT note). 156 m^2 is 44 fine cells -- still
    #: resolved, and at the low end of the 10^2-10^3 m^2 channel-initiation
    #: areas reported for steep badland/alpine terrain. THE COST, stated
    #: plainly: on steep tiles most of the surface drains more than 156 m^2
    #: (68% of the alpine window; carved past 0.1 m: 98% against 93% at 625),
    #: so the bake is dissection-dominated there. On gentle ground
    #: ``profile_regional_p`` keeps the same network sub-metre (plains carved
    #: fraction 6.5%). The flow-plane magnitude gate deliberately does NOT
    #: follow this constant down -- see ``flow_mag_min_area_m2``.
    channel_init_area_m2: float = 156.0
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
    #: Material-strength modulation of the repose angle (bake_ver 5). Both at
    #: 0 reproduce the scalar bake_ver-4 relaxation exactly. See
    #: ``noise.repose_field`` for the construction and the measured motivation
    #: (one global angle planes a third of a mountainside to a single slope,
    #: which voxelises as the parallel contour-terrace corduroy).
    #:
    #: The SPATIAL term is lithology patches: smooth world-anchored octaves at
    #: this wavelength and a quarter of it. The STRATA term is sub-horizontal
    #: bedding: 1-D noise over elevation, so strong bands hold near-cliff
    #: faces (keeping what incision carved into them) and weak bands ravel
    #: back to talus -- bench-and-cliff structure at metre scale.
    #: Amplitudes are degrees per unit-RMS field; the result is clamped to
    #: [repose_min_deg, repose_max_deg].
    #: First cut was 6/14 deg amplitude clamped to [26, 60]. The in-engine A/B
    #: (docs/measurements/repose-field-2026-07-30.txt) showed the banding
    #: broken into patches but still legible on the big distant faces, and the
    #: mechanism says why: thermal transport is translation-invariant on a
    #: uniform ramp (shed == receive), so restructuring happens near THRESHOLD
    #: BOUNDARIES, and a 60 deg ceiling is still just a steeper uniform ramp.
    #: So: stronger strata, a genuinely cliff-holding ceiling (72 deg,
    #: tan = 3.08 -- still far under relax's 85 deg validity bound), and a
    #: slightly softer floor so weak treads cut back further.
    repose_spatial_amp_deg: float = 8.0
    repose_spatial_wavelength_m: float = 160.0
    repose_strata_amp_deg: float = 20.0
    repose_strata_wavelength_m: float = 30.0
    #: bake_ver 6: the strata are FOLDED -- keyed on z + a bounded
    #: world-anchored undulation of this amplitude, so bench traces wander
    #: across contours instead of ringing the hill at one elevation. Measured
    #: motivation: flat-lying strata bind at constant elevation, so their
    #: benches (sharpened by the strength-modulated incision) are horizontal,
    #: contour-parallel and quasi-evenly spaced at the 30 m wavelength -- the
    #: capture showed the owner's banding artifact re-manufactured at bench
    #: scale on the distant faces. +-12 m at 300 m gives apparent local dips
    #: up to ~15 deg, well inside folded-sedimentary reality. 0 disables and
    #: reproduces flat-lying strata exactly.
    repose_strata_fold_amp_m: float = 15.0
    repose_strata_fold_wavelength_m: float = 300.0
    #: 24 deg is a weathered debris slope; 72 deg holds jointed-rock faces.
    repose_min_deg: float = 24.0
    repose_max_deg: float = 72.0
    #: bake_ver 6: how much harder strong rock is to CARVE than baseline
    #: (``noise.repose_erodibility``; consumed by ``profile_incision`` in
    #: "profile" mode). The strongest strata (repose_max_deg) erode 1/this as
    #: fast; 1 disables and reproduces the bake_ver-5 incision exactly. 6 is
    #: within the measured range of lithologic erodibility contrasts (an order
    #: of magnitude between shale and well-jointed sandstone is conservative)
    #: and, at the 30 m strata wavelength, gives channels crossing a full
    #: strong band a knickpoint the cap does not censor on hillslope gullies.
    incision_strength_ratio: float = 3.0
    #: bake_ver 6, B4: post-thermal meso relief (noise.meso_relief). RMS metres
    #: at the 15 m / 11.25 m octaves, gated to zero at or below meso_slope_lo
    #: (regional plains keep their calibrated statistics) and full at
    #: meso_slope_hi. This is the band that varies local grade at the
    #: band-spacing wavelength on steep faces -- the contour-banding residual's
    #: named mechanism -- placed AFTER thermal so relaxation cannot plane it
    #: back into the threshold pattern, and BEFORE the B4b refill so it cannot
    #: cost drainage. 0/0 disables and reproduces the prior surface exactly.
    meso_amp15_m: float = 0.8
    meso_amp11_m: float = 0.4
    #: First cut was 0.20/0.40 -- full amplitude only on 40%+ grades. The
    #: in-engine A/B showed exactly what that arithmetic predicts: the
    #: mid-slope bowl (steep) lost its banding and read as talus, while the
    #: moderate-grade upper faces -- which band just as legibly, spacing =
    #: 100 mm / grade -- kept their rhythm because the gate was barely open
    #: there. 0.12/0.28 puts full amplitude on every face that can band
    #: visibly while the plains classes (2-6% grade) stay untouched.
    meso_slope_lo: float = 0.12
    meso_slope_hi: float = 0.28
    #: The B4 descent-enforcement step, metres: after the meso band is added,
    #: every cell is raised until it keeps at least min(its pre-meso drop,
    #: this) over its pre-meso D8 receiver. One codec LSB (100 mm) plus both
    #: half-LSB reconstruction errors, so an enforced reach stays monotone
    #: through encode/decode worst-case. NOTE this constant must NOT be used
    #: as a flood-fill epsilon: an epsilon this large DOMES every wide flat it
    #: refloods (measured: lake floors tilted into 5-17 m cones, flat-shaded
    #: scars in the hillshade). The enforcement form preserves gentle reaches
    #: exactly because it takes min(original drop, this).
    refill_eps_m: float = 0.11
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
    #: Accumulation (AFTER the sea-level taper) below which the flow plane's
    #: log2 magnitude, bits 0-4, is written as ZERO.
    #:
    #: The format assumes the plane is "mostly zeros, ~5-10 KB compressed"
    #: (docs/vxtl-v2-format.md section 6); measured on the first real bakes it
    #: was 5.75 MB on an alpine tile and 8.88 MB on an ocean one, of which 87%
    #: is this magnitude field -- because at 1.875 m/px every land cell has
    #: upslope area, so bits 0-4 were dense. Originally set to the same value
    #: as ``channel_init_area_m2`` (both 1e4): below the initiation gate the
    #: bake had declined to cut a channel, so the magnitude there described a
    #: river that does not exist. 5.745 -> 1.518 MB alpine, 7.677 -> 0.000 MB
    #: ocean. 0 disables the gate and reproduces the pre-2026-07-29 plane.
    #:
    #: DELIBERATELY NOT lowered with ``channel_init_area_m2`` at bake_ver 4:
    #: at 156 m^2 the majority of a steep tile is above the initiation area
    #: (68% of the alpine exemplar), so following it down would make bits 0-4
    #: dense again and reintroduce the multi-MB plane this gate exists to
    #: prevent. The sub-1e4 network the bake now carves is hillslope-scale
    #: swale/gully texture -- landform, not hydrology a client needs to query.
    #: The plane keeps describing rivers from 1e4 m^2 up, exactly as at
    #: bake_ver 2-3. See FLOW_PLANE_SIZE.
    flow_mag_min_area_m2: float = 1.0e4
    #: Apply the sea-level taper (``sea_taper_top_m``/``sea_taper_bottom_m``)
    #: to the flow plane's CHANNEL flag and magnitude gate, not only to
    #: incision. Without it the plane kept flagging dendritic drainage at 3 km
    #: depth that the tapered incision no longer cuts -- a lie about the ground
    #: as well as 7.7 MB of it. See FLOW_PLANE_SIZE.
    flow_flag_sea_taper: bool = True

    def __post_init__(self) -> None:
        if not 0.0 < self.thermal_rate <= 0.5:
            raise ValueError(
                f"thermal_rate={self.thermal_rate} is outside (0, 0.5]; "
                "thermal.relax rejects it, and above 0.5 the steepest-pair "
                "bound no longer holds"
            )
        if self.repose_spatial_amp_deg < 0.0 or self.repose_strata_amp_deg < 0.0:
            raise ValueError("repose amplitudes must be >= 0 (0 disables)")
        if self.repose_spatial_wavelength_m <= 0.0 or self.repose_strata_wavelength_m <= 0.0:
            raise ValueError("repose wavelengths must be positive")
        if self.repose_strata_fold_amp_m < 0.0 or self.repose_strata_fold_wavelength_m <= 0.0:
            raise ValueError("strata fold amplitude must be >= 0 and wavelength > 0")
        if not (0.0 < self.repose_min_deg <= self.repose_deg <= self.repose_max_deg < 85.0):
            raise ValueError(
                f"need 0 < repose_min_deg {self.repose_min_deg} <= repose_deg "
                f"{self.repose_deg} <= repose_max_deg {self.repose_max_deg} < 85"
            )
        if self.superblock_tiles < 1 or self.superblock_max_level < 0:
            raise ValueError("superblock_tiles >= 1 and max_level >= 0")
        if self.incision_mode not in ("profile", "depth"):
            raise ValueError(
                f"incision_mode={self.incision_mode!r} is neither 'profile' "
                "nor 'depth'"
            )
        if self.profile_K_dt < 0.0:
            raise ValueError(f"profile_K_dt must be >= 0, got {self.profile_K_dt}")
        if self.profile_regional_s_ref < 0.0:
            raise ValueError(
                f"profile_regional_s_ref must be >= 0 (0 disables), got "
                f"{self.profile_regional_s_ref}"
            )
        if self.profile_regional_p < 0.0:
            raise ValueError(
                f"profile_regional_p must be >= 0 (0 = use stream_n), got "
                f"{self.profile_regional_p}"
            )
        if self.meso_amp15_m < 0.0 or self.meso_amp11_m < 0.0:
            raise ValueError("meso amplitudes must be >= 0 (0 disables)")
        if not 0.0 <= self.meso_slope_lo < self.meso_slope_hi:
            raise ValueError(
                f"need 0 <= meso_slope_lo < meso_slope_hi, got "
                f"{self.meso_slope_lo}, {self.meso_slope_hi}"
            )
        if self.refill_eps_m < 0.0:
            raise ValueError(f"refill_eps_m must be >= 0, got {self.refill_eps_m}")
        if self.incision_strength_ratio <= 0.0:
            raise ValueError(
                f"incision_strength_ratio must be positive (1 disables), got "
                f"{self.incision_strength_ratio}"
            )
        if self.b1_constructional_amp < 0.0:
            raise ValueError(
                f"b1_constructional_amp must be >= 0 (0 disables), got "
                f"{self.b1_constructional_amp}"
            )
        if not 0.0 <= self.b1_constructional_slope_lo < self.b1_constructional_slope_hi:
            raise ValueError(
                "need 0 <= b1_constructional_slope_lo < "
                f"b1_constructional_slope_hi, got "
                f"{self.b1_constructional_slope_lo}, {self.b1_constructional_slope_hi}"
            )

    def as_payload(self) -> dict:
        return {
            "src_nyquist_m": self.src_nyquist_m,
            "noise_anchor_pitch_fine_px": self.noise_anchor_pitch_fine_px,
            "b1_constructional_amp": self.b1_constructional_amp,
            "b1_constructional_slope_lo": self.b1_constructional_slope_lo,
            "b1_constructional_slope_hi": self.b1_constructional_slope_hi,
            "mfd_p": self.mfd_p,
            "flat_eps": self.flat_eps,
            "stream_K": self.stream_K,
            "stream_m": self.stream_m,
            "stream_n": self.stream_n,
            "incision_cap_m": self.incision_cap_m,
            "incision_mode": self.incision_mode,
            "profile_K_dt": self.profile_K_dt,
            "profile_regional_s_ref": self.profile_regional_s_ref,
            "profile_regional_p": self.profile_regional_p,
            "channel_init_area_m2": self.channel_init_area_m2,
            "channel_init_q": self.channel_init_q,
            "sea_taper_top_m": self.sea_taper_top_m,
            "sea_taper_bottom_m": self.sea_taper_bottom_m,
            "repose_deg": self.repose_deg,
            "repose_spatial_amp_deg": self.repose_spatial_amp_deg,
            "repose_spatial_wavelength_m": self.repose_spatial_wavelength_m,
            "repose_strata_amp_deg": self.repose_strata_amp_deg,
            "repose_strata_wavelength_m": self.repose_strata_wavelength_m,
            "repose_strata_fold_amp_m": self.repose_strata_fold_amp_m,
            "repose_strata_fold_wavelength_m": self.repose_strata_fold_wavelength_m,
            "repose_min_deg": self.repose_min_deg,
            "repose_max_deg": self.repose_max_deg,
            "incision_strength_ratio": self.incision_strength_ratio,
            "meso_amp15_m": self.meso_amp15_m,
            "meso_amp11_m": self.meso_amp11_m,
            "meso_slope_lo": self.meso_slope_lo,
            "meso_slope_hi": self.meso_slope_hi,
            "refill_eps_m": self.refill_eps_m,
            "thermal_iters": self.thermal_iters,
            "thermal_rate": self.thermal_rate,
            "superblock_tiles": self.superblock_tiles,
            "superblock_max_level": self.superblock_max_level,
            "channel_area_m2": self.channel_area_m2,
            "channel_depth_m": self.channel_depth_m,
            "deposition_m": self.deposition_m,
            "flow_mag_min_area_m2": self.flow_mag_min_area_m2,
            "flow_flag_sea_taper": self.flow_flag_sea_taper,
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
    "B4.meso",
    "B4b.refill",
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
    """Live-set HIGH-WATER MARK of ``bake_padded_domain``, in bytes.

    Counted, not timed, so box contention cannot touch it -- but counted from
    the CODE, which the previous version was not. It counted "8 float32 grids
    plus one int64 receiver grid" = 3.16 GiB against a measured 6.90 GiB peak
    commit, a 2.2x undercount, and this is the number pod sizing reads.

    Three things it missed, all of them named arrays or first-class kernel
    working sets rather than allocator noise:

    1. **Four B1 temporaries that are never freed.** ``gy``, ``gx`` (the
       gradient pair), ``slope`` and ``delta`` are dead after B1's roughness
       call, but nothing drops the references, so all four stay live to the
       end of the function. That is 4 padded float32 grids -- 1.27 GiB at
       production geometry, ~19% of the peak -- of pure ballast. (Not fixed
       here: it is a live-range change in a file three sessions are editing.
       ``del gy, gx`` after ``slope``, ``del slope`` after the roughness call
       and ``del delta`` after ``fine = fine + delta`` reclaim all of it and
       cannot move a baked byte.)
    2. **Only ONE wide grid was counted, and the peak holds four.** The D8
       receivers are int64 (8 bytes/cell, 680 MB), MFD accumulation is float64
       (another 680 MB), and B4's descent enforcement materialises THREE
       int64 index grids at once -- ``recf``, ``self_idx`` and ``tgt``.
    3. **The kernels' own working sets.** ``profile_incision`` argsorts
       84.9 M cells (an int64 order array, then an int32 copy of it) and
       builds ``kfac32``/``rec32``/``dist``; ``repose_field`` and
       ``repose_erodibility`` each yield a full grid; the priority flood
       carries ``done``/``hz``/``hi``.

    The high-water is reached TWICE, at nearly the same height, which is why
    the total below is not attributed to one stage: B2d (measured 6.65 GiB,
    inside ``profile_incision``'s argsort) and B4 (measured 6.90 GiB, at the
    three-index-grid line). Both enumerate to the same 12 float32-equivalent
    grids + 4 wide grids, i.e. 80 bytes per padded cell.

    MEASURED, 9216^2 padded domain, one tile per process, Windows
    PeakPagefileUsage: 6.90 GiB commit / 6.24 GiB working set, against 6.33 GiB
    from this count. The residual is the interpreter, numba and the compiled
    kernel cache -- 0.39 GiB of commit before the bake starts -- so a pod still
    wants headroom over this number, but it is now headroom and not a factor
    of two. Size a bake worker at 8 GiB.
    """
    n = geom.padded_fine_px**2
    grid32 = 4 * n          # one padded float32 grid: 340 MB at production
    grid64 = 8 * n          # one padded int64 / float64 grid: 680 MB

    # Live at the B4 high-water. B2d's high-water enumerates to the same
    # total with different names (regional + erodibility + kfac32 + rec32 +
    # dist in place of z_pre/d8s4/z, and profile_incision's int64 order array
    # in place of one of B4's index grids).
    held = 6 * grid32       # fine, filled, d8_slope, eroded, depth, z
    leaked_b1 = 4 * grid32  # gy, gx, slope, delta -- see (1) above
    stage_tmp = 2 * grid32  # z_pre, d8s4 (B4) / kfac32, dist (B2d)
    wide = 4 * grid64       # acc64 + three int64 index grids
    return held + leaked_b1 + stage_tmp + wide


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
    #: The implicit profile solve (``incise.profile_incision``). Optional with
    #: a None default so pre-existing test doubles keep constructing; the
    #: pipeline refuses to bake ``incision_mode = "profile"`` without it.
    profile_incision: "Callable | None" = None


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
        from .incise import profile_incision, stream_power
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
        profile_incision=profile_incision,
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

   NOT FIXED, but no longer SILENT (2026-07-29): every superblock now carries
   ``inputs_fingerprint``, every ``BakeResult`` carries the fingerprint of the
   superblock it used plus ``superblock_complete``, and ``pregen`` refuses to
   reuse a cached superblock whose fingerprint disagrees with the coarse world
   that exists now. See ``ORDER_DEPENDENCE``.

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

6. **The fill LEVEL, not just the flat's routing.** This entry used to say
   that a flat wider than the apron can be crossed in different directions by
   two neighbouring bakes while heights still agree to sub-ULP. Measured, the
   heights do NOT agree: the domain border is an invented outlet, so a basin
   whose true rim lies outside the padded domain drains through the cut instead
   of filling, and widening the apron on real tile (-5,2) moves 1.05% of the
   shipped interior past the 100 mm wire LSB (max 78.79 m). Directly across the
   (-5,2)/(-5,3) join the elevation seam is nonetheless small -- 27.41 cm mean
   step against the terrain's own 26.61 cm, i.e. **0.80 cm of excess**, because
   no large basin happens to straddle that particular edge -- while the
   ACCUMULATION step is 4.56x the terrain's own and the channel flag disagrees
   on 0.92% of the join cells. Numbers and method in ``APRON_BLIND_SPOT``.

   The fix is a boundary condition on the FILL taken from the superblock's
   ``filled`` raster (world-anchored, shared, 61 km at level 0), NOT more
   inflow: ``inject_edge_inflow`` runs after the fill and cannot change where
   the surface lets water go. Not wired.
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


ORDER_DEPENDENCE = """\
Why a superblock carries a fingerprint of what it was built FROM.

HYDROLOGY_RESIDUALS #1 is not a numerical error, it is a *provenance* problem:
a superblock is built from whatever coarse tiles existed when the first tile
inside it baked, a shipped tile is never regenerated, so an on-demand frontier
freezes one arbitrary exploration order into the river network forever. Nothing
about that is detectable after the fact from the tile bytes -- two worlds
explored in different orders are both self-consistent and neither says so.

``superblock_inputs_fingerprint`` is the thing that says so. It is a 16-byte
digest over EVERY input the superblock's hydrology depends on, in a canonical
world order that has nothing to do with the order the caller fetched them:

  * the block's identity and geometry (level, index, tiles per side, cell size)
    and the routing constants the accumulation used (``mfd_p``, ``flat_eps``);
  * for each coarse tile of the block, in world (row, column) order, either a
    present-marker plus the tile's float32 bytes, or an absent-marker;
  * the PARENT level's fingerprint, so the digest covers the whole upstream
    chain rather than one level of it.

Three things follow, and all three are the point:

1. **Absence is hashed, not skipped.** A block built while its north-west
    quadrant did not yet exist has a different fingerprint from the same block
    built after, even though both are "valid". That difference is exactly the
    order dependence.
2. **Fetch order cannot change it.** The loop is over world coordinates, so a
    caller that walks its tiles in a different order gets the same digest. A
    fingerprint that moved with iteration order would report order dependence
    everywhere and mean nothing.
3. **It is checkable against the present.** Recomputing it from the coarse
    tiles that exist NOW and comparing against the digest stored in a cached
    ``.vxfl`` answers "was this superblock built against a smaller world than
    the one we are in?" -- which is the question ``pregen`` now asks before it
    reuses one, and the answer it used to discard silently.

What it deliberately does NOT do is fix anything. A tile baked against an
incomplete superblock stays baked that way; ``pregen --mode bake`` over a
radius is still the only way to be order-independent. This makes the residual
visible and attributable instead of invisible, which is the difference between
a known limitation and a mystery.
"""

#: Digest length of ``superblock_inputs_fingerprint``. 16 bytes: this is a
#: provenance tag compared for equality, never a security boundary, and it
#: rides in every .vxfl header.
FLOW_FINGERPRINT_BYTES = 16

_FP_DOMAIN = b"vxbake-flow-inputs:v1"


def superblock_inputs_fingerprint(
    coarse_fetch: CoarseFetch,
    sx: int,
    sy: int,
    level: "FlowLevel",
    parent_fingerprint: bytes = b"",
) -> bytes:
    """Digest of every input one superblock's hydrology is built from.

    Read ``ORDER_DEPENDENCE`` for why this exists. Deterministic, canonical in
    WORLD order (not fetch order), and it hashes a missing tile as a missing
    tile rather than skipping it -- the whole value is that "this block was
    built before that region existed" changes the answer.

    Costs one pass over the block's coarse tiles: 16 MB of sha256 at level 0
    with the default 4x4 block, 256 MB at level 1. That is I/O and hashing
    against a priority flood over the same ground, so it is not what decides
    whether the pyramid is affordable.
    """
    h = hashlib.sha256()
    h.update(_FP_DOMAIN)
    h.update(
        json.dumps(
            {
                "bake_version": BAKE_VERSION,
                "level": int(level.level),
                "sx": int(sx),
                "sy": int(sy),
                "tiles_per_side": int(level.tiles_per_side),
                "downsample": int(level.downsample),
                "cell_m": float(level.cell_m),
                "coarse_tile_px": int(level.geom.coarse_tile_px),
                "mfd_p": float(level.consts.mfd_p),
                "flat_eps": level.consts.flat_eps,
            },
            sort_keys=True,
        ).encode("utf-8")
    )
    # Length-prefixed so "no parent" and "a parent whose digest happens to
    # start with zeros" cannot collide.
    h.update(struct.pack("<B", len(parent_fingerprint)))
    h.update(parent_fingerprint)

    n = level.tiles_per_side
    tx0, ty0 = sx * n, sy * n
    for j in range(n):
        for i in range(n):
            src = coarse_fetch(tx0 + i, ty0 + j)
            if src is None:
                h.update(b"\x00")
                continue
            h.update(b"\x01")
            h.update(np.ascontiguousarray(src, dtype=np.float32).tobytes())
    return h.digest()[:FLOW_FINGERPRINT_BYTES]


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
    #: ``superblock_inputs_fingerprint`` of the world this was built from.
    #: See ORDER_DEPENDENCE. Empty only for a hand-built test fixture.
    inputs_fingerprint: bytes = b""

    @property
    def size_px(self) -> int:
        return int(self.acc.shape[0])

    @property
    def complete(self) -> bool:
        """True when every coarse tile of the block existed at build time.

        The ONLY case in which the hydrology this superblock feeds is
        independent of exploration order.
        """
        return not self.missing_tiles

    @property
    def fingerprint_hex(self) -> str:
        return self.inputs_fingerprint.hex()


_FLOW_MAGIC = b"VXFL"
#: 1 -> 2: added the 16-byte inputs fingerprint. Old blobs are REFUSED rather
#: than read with a zero fingerprint, because a zero fingerprint would read as
#: "provenance unknown" everywhere and defeat the check it was added for.
_FLOW_VERSION = 2
#: magic, version, seed, bake_ver, level, pad, sx, sy, tiles_per_side, size,
#: cell_m, origin_x_m, origin_y_m, inputs_fingerprint, n_missing
_FLOW_HEADER = struct.Struct("<4sHQHBBiiHIfdd16sI")


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
        bytes(sb.inputs_fingerprint).ljust(FLOW_FINGERPRINT_BYTES, b"\x00")[
            :FLOW_FINGERPRINT_BYTES
        ],
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
        fingerprint,
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
        inputs_fingerprint=bytes(fingerprint),
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

    The result carries ``inputs_fingerprint`` -- a digest of the coarse tiles
    (present AND absent) it was actually built from, chained to the parent's.
    See ``ORDER_DEPENDENCE``: without it, "this block froze an earlier world"
    is undetectable, and it is the residual the plan calls the largest one.
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
        inputs_fingerprint=superblock_inputs_fingerprint(
            coarse_fetch,
            sx,
            sy,
            level,
            parent_fingerprint=(
                parent.inputs_fingerprint if parent is not None else b""
            ),
        ),
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


FLOW_PLANE_SIZE = """\
What the flow plane costs, and why the format's "~5-10 KB" was out by 1000x.

docs/vxtl-v2-format.md section 6 specifies one uint8 per fine pixel -- bits 0-4
``log2(accumulation in m2)``, bit 5 channel, bit 6 bank, bit 7 deposition --
and predicts "Mostly zeros; ~5-10 KB compressed". MEASURED on the first real
bakes (zstd-19 over the actual block payloads, 8192^2, the accounting in
tools/bake_real_tile.py):

    tile              flow plane   elevation plane
    (-5,2) alpine        5.75 MB       16.85 MB
    (-7,4) ocean         8.88 MB       19.44 MB

Three orders of magnitude over the spec, and a quarter of the whole tile.

THE SPEC IS RIGHT ABOUT THE FLAGS AND WRONG ABOUT BITS 0-4. "Mostly zeros"
holds only if the magnitude is zero away from channels, and it never was: at
1.875 m/px EVERY land cell has upslope area, so ``log2(A)`` is a dense field of
small integers that steps by +-1 between neighbours nearly everywhere. That is
close to the worst case for a MED predictor -- too correlated to be noise, too
noisy to predict -- and it is paid on all 67.1 M pixels. Decomposed on the
alpine tile by encoding each half of the byte alone:

    (-5,2)  bits 0-4 alone (magnitude)   5.015 MB   <- 87% of the plane
            bits 5-7 alone (flags)       0.920 MB
            whole byte                   5.745 MB

TWO LEVERS, MEASURED SEPARATELY. Both are BakeConstants, so either rolls the
bake fingerprint, hence provider_id, hence the world.

1. ``flow_flag_sea_taper`` -- the channel flag now tests a DEPTH-TAPERED
   accumulation, the same smoothstep ``incise.stream_power`` already applies to
   incision. Without it the plane described dendritic rivers at 3 km depth that
   the (already tapered) incision no longer cuts, which is not just expensive,
   it is a lie about the ground.
2. ``flow_mag_min_area_m2`` -- bits 0-4 are written only where the tapered
   accumulation reaches the channel-initiation area, or on a channel/bank cell
   whatever its own area. Below ``channel_init_area_m2`` the bake has ALREADY
   decided there is no channel (that is what the gate in ``stream_power``
   means), so the magnitude there describes a river the bake declined to cut.
   Setting it to the same 1e4 m2 is the principled choice, not a tuned one:
   the plane's magnitude covers exactly the domain where the bake believes
   channels exist. 0 disables it and reproduces the pre-Wave-E plane exactly.

    variant                          alpine (-5,2)   ocean (-7,4)
    pre-Wave-E (dense, untapered)       5.745 MB       7.677 MB
                                                      (8.88 MB before
                                                       stream_power's
                                                       own sea taper)
    flag taper only                     5.745 MB       7.235 MB
    magnitude gate 1e3 only             2.589 MB       5.800 MB
    magnitude gate 1e4 only             1.518 MB       3.109 MB
    magnitude gate 1e5 only             1.489 MB       1.075 MB
    SHIPPED (1e4 + flag taper)          1.518 MB       0.000 MB
                                          -73.6 %       -100.0 %

Read the two columns against each other, because they say different things.

  * On the ALPINE tile the taper does nothing at all -- the tile's minimum
    elevation is 1017 m, so no cell is below sea level -- and the whole saving
    is the magnitude gate. Note also that 1e5 buys almost nothing over 1e4
    (1.489 vs 1.518): past the channel head the field is no longer dense, so
    the gate has already done its work and tightening it only discards real
    tributaries.
  * On the OCEAN tile the reverse: the taper is what matters, and it only
    works because the magnitude gate reads the TAPERED area too. Gating on raw
    accumulation left the ocean tile at 2.673 MB; gating on tapered
    accumulation takes it to **1024 CONSTANT blocks and zero data bytes**,
    which is what section 4 assumed an ocean tile would produce ("Zero data
    bytes. Common: ocean, flat basin") and what tools/bake_real_tile.py
    measured as 0 of 1024 before any of this.

Deposition is NOT tapered. Thermal relaxation is mass wasting, which happens
underwater; and there was nothing to win anyway -- measured 0.00% of the ocean
tile flagged as deposition against 2.98% alpine.

WHAT IS LOST. Hillslope accumulation below the channel head is no longer on
the wire. The consumers section 6 names are "client alluvium/cut-bank
materials" and "flow-conditioned rill synthesis and bank undercuts"; the first
two live on channel and bank cells, which keep their magnitude. Rill synthesis
on open hillslopes now has flags but no magnitude there -- deliberately, since
the drainage the bake actually resolved does not extend below 1e4 m2 and a
client rill field at that scale is procedural either way. If that turns out to
be wrong, ``flow_mag_min_area_m2`` is the one number to move, and moving it
rolls the world.

WHAT IT DOES TO THE TILE. zstd-19 over the real block payloads, against the
three tiles tools/bake_real_tile.py first measured:

    tile              was      elev now   flow now   now      delta
    (-5,2) alpine   22.62 MB    16.82       1.52    18.34 MB  -18.9 %
    (-5,3) mixed    23.13 MB    16.52       2.35    18.86 MB  -18.5 %
    (-7,4) OCEAN    28.35 MB    11.10       0.00    11.10 MB  -60.8 %

Attribute the ocean row carefully, because most of it is not this change. Its
ELEVATION plane fell 19.44 -> 11.10 MB because ``stream_power``'s sea taper
(landed before this work) stopped cutting river valleys into the seafloor, so
there is less invented detail to encode; the flow plane's 7.68 -> 0.00 is this
change. The two together turn the largest tile of the three into the smallest,
which is what a mostly-ocean world needs and the opposite of what was measured
before either.

WHAT IS STILL NOT 5-10 KB. Even at the shipped setting the alpine plane is
1.518 MB, ~150-300x the spec. The spec number is unreachable with ANY per-pixel
magnitude field on dissected terrain: 4.13% of the alpine tile is channel and
another 1.44% is bank, i.e. 3.7 M pixels of genuinely varying data however it
is coded. "Mostly zeros; ~5-10 KB" is right for deep ocean and wrong by two
orders of magnitude for land; that is a defect in docs/vxtl-v2-format.md
section 6, which this module cannot fix (docs/ is not ours) and records here so
the next person measures rather than trusts it.
"""


def _sea_taper(elev_m: np.ndarray, consts: BakeConstants) -> np.ndarray:
    """``incise.stream_power``'s smoothstep, reused for the flow-plane flags.

    Duplicated here rather than imported: ``pipeline`` must stay importable
    without the numerics stack (see the module docstring), and this is six
    lines of numpy. The two MUST agree -- a flow plane that flagged channels
    the incision no longer cuts would be describing a river that is not in the
    ground -- so ``tests/test_bake_pipeline.py`` asserts they match.
    """
    top = np.float32(consts.sea_taper_top_m)
    bot = np.float32(consts.sea_taper_bottom_m)
    if not bot < top:
        return np.ones(np.shape(elev_m), np.float32)
    t = (np.asarray(elev_m, dtype=np.float32) - bot) / (top - bot)
    t = np.clip(t, 0.0, 1.0)
    return t * t * (np.float32(3.0) - np.float32(2.0) * t)


def flow_plane(
    acc: np.ndarray,
    incision_m: np.ndarray,
    thermal_gain_m: np.ndarray,
    consts: BakeConstants = CONSTANTS,
    elev_m: "np.ndarray | None" = None,
) -> np.ndarray:
    """Pack the optional uint8 flow plane.

    bits 0-4 = log2(accumulation in m^2) clamped 0-31, bit 5 = channel,
    bit 6 = bank, bit 7 = deposition (docs/vxtl-v2-format.md section 6).

    ``elev_m`` is the depression-filled surface, i.e. the same field
    ``stream_power`` tapers against. Optional only so a unit test can pack a
    plane without one; ``bake_tile`` always passes it, and without it
    ``flow_flag_sea_taper`` cannot apply. See ``FLOW_PLANE_SIZE``.
    """
    acc = np.asarray(acc, dtype=np.float32)
    with np.errstate(divide="ignore", invalid="ignore"):
        mag = np.log2(np.maximum(acc, 1.0))
    out = np.clip(mag, 0, 31).astype(np.uint8)

    # The channel test sees a DEPTH-TAPERED accumulation, so the flag's contour
    # migrates continuously seaward instead of stepping along the coastline --
    # the same reason ``stream_power`` tapers rather than cuts. Multiplying the
    # area by the taper (rather than testing depth against a threshold) is what
    # keeps it continuous: at the shelf break the effective threshold is
    # infinite, at sea level it is unchanged, and in between it slides.
    a_eff = acc
    if consts.flow_flag_sea_taper and elev_m is not None:
        a_eff = acc * _sea_taper(elev_m, consts)

    channel = (a_eff >= consts.channel_area_m2) | (
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
    bank = dil & ~channel
    out |= bank.astype(np.uint8) << 6

    out |= (np.asarray(thermal_gain_m, dtype=np.float32) >= consts.deposition_m).astype(
        np.uint8
    ) << 7

    # The magnitude gate, applied LAST so it can see the channel/bank flags.
    # Zeroing bits 0-4 below the channel-initiation area is what makes the
    # plane "mostly zeros" as the format assumes; see FLOW_PLANE_SIZE for the
    # measurement and for what it costs.
    if consts.flow_mag_min_area_m2 > 0.0:
        # a_eff, not acc: below the shelf break the bake has decided there is
        # no fluvial drainage (incision is tapered out and the channel flag
        # with it), so storing its accumulation is storing a number about a
        # river that does not exist. On a 100%-ocean tile this is what turns
        # the plane into the CONSTANT blocks the format assumed it would be.
        keep = (a_eff >= np.float32(consts.flow_mag_min_area_m2)) | channel | bank
        out &= np.where(keep, np.uint8(0xFF), np.uint8(0xE0))
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

-----------------------------------------------------------------------------
2026-07-29, Wave E: THE FILL IS NOT BOUNDED BY THE APRON ON REAL TERRAIN
-----------------------------------------------------------------------------
Everything above about the depression fill was measured with the PROTOTYPE's
kernels -- a plain fill, no epsilon -- on a synthetic domain with a global
tilt, i.e. terrain that drains. Re-measured with the real ``flow.py``
priority-flood on a real diffusion tile, it does not hold.

The measurement is an apron-sensitivity test, which is stronger than a seam
test and needs no neighbour: bake the SAME tile twice at the SAME 1.875 m/px
with two different apron widths and diff the INTERIORS. If the shipped
interior is the infinite-domain answer, widening the domain cannot move it.
Real tile (-5,2) (alpine, 1571 m relief, complete 3x3 ring), 960 m apron
against 1920 m, 8192^2 interior only:

    stage                        cells differing   mean       max
    B0+B1 carrier + roughness          0.000 %     0.0000 m   0.00 m  EXACT
    B2a depression fill                0.940 %     0.3252 m  64.99 m
    B2c MFD accumulation               1.559 %     3663 m2   49.3 km2
    B2d incision                       1.535 %     0.0039 m  25.00 m (capped)
    B3  final surface                  1.439 %     0.3289 m  78.79 m

**1.05 % of the shipped tile moves by more than the 100 mm wire LSB.** The
carrier and the roughness are still bit-identical -- the apron argument is
exactly right for everything it was measured on. What breaks is the FILL, and
it breaks first: accumulation and incision inherit it.

Where it lives, as a function of distance from the tile edge:

    band from tile edge   cells differing   mean       max
        0 -  960 m             2.228 %      0.9337 m   64.99 m
      960 - 1920 m             1.355 %      0.3722 m   64.99 m
     1920 - 3840 m             0.455 %      0.0983 m   47.16 m
     3840 - 7680 m             0.000 %      0.0000 m    0.00 m  EXACT

So the domain boundary's influence on the fill reaches **about 3.8 km into the
interior -- four apron widths** -- and is then exactly zero. The plan's "error
collapses between 120 m and 480 m, so the influence radius is a few hundred
metres and 960 m carries real margin" is a true statement about the CARRIER and
about a plain fill on draining terrain, and a false one about the epsilon
priority-flood on alpine terrain with real closed depressions.

WHY, and why no apron fixes it. A cell's filled elevation is the minimum over
all paths to the DOMAIN BORDER of the highest point on the path. Cutting the
domain closer does not merely truncate information, it INVENTS AN OUTLET: the
border is an escape at whatever elevation the border happens to be, so a basin
whose true rim lies outside the domain drains through the cut instead of
filling. Measured on this tile the smaller apron under-fills -- basin fraction
1.705 % against 2.440 %, deepest filled basin 55.21 m against 88.79 m. This is
the same unboundedness the plan already grants flow accumulation, one stage
earlier, and it has the same cure and the same non-cure:

  * ``inject_edge_inflow`` CANNOT fix it. Inflow is added to ``accumulate_mfd``
    AFTER the fill, so it changes how much water is routed and never where the
    surface lets it go. Anything that only supplies boundary area is answering
    a different question.
  * The superblock's ``filled`` raster CAN, and is the thing to use: it is
    world-anchored, shared between neighbours, and spans 61 km at level 0 --
    far enough to contain the rim of any basin a 15 km tile can hold. The fix
    is a boundary CONDITION ON THE FILL, not on the accumulation: clamp the
    fine fill's border seed to the coarse filled surface (upsampled) rather
    than to the fine surface's own border values, so the fine domain cannot
    invent an outlet the coarse hydrology says is not there. That is a change
    to ``flow.fill_depressions``' seeding, it is not wired, and it is the right
    next piece of work here.

WHAT THE PER-TILE DETECTORS ARE WORTH. ``max_basin_run_m`` measures the longest
run of filled cells along a row or column. On tile (-5,2) it reports 2091 m
against a 960 m apron and fires ``basin_exceeds_apron`` -- but the object it
found is a valley floor 2258 m long and 759 m across, 52 % of its own bounding
box, one of 1316 filled components covering 1.71 % of the tile. It measured a
LENGTH and the flag reads it as a WIDTH. It is kept because the plan quotes it,
and it is not the condition that matters.

The condition that matters is not cheaply detectable either, and it is worth
saying why rather than shipping a detector that looks sound and is not. "No
filled flat touches the padded border" sounds like it would prove the fill is
domain-independent; it is VACUOUS, because priority-flood seeds the border with
its own elevation and never raises it, so no border cell is ever in the filled
mask -- measured 0 border basin cells on both a 100 %-ocean tile and the alpine
tile whose interior demonstrably does move. ``padded_border_basin_frac`` is
reported for exactly that reason: it is a control that must read 0, and a
nonzero value would mean the fill kernel had changed under us. Even a correct
connectivity test would not be sound, because widening the domain can always
reveal a LOWER saddle that no cell inside the current domain knows about.
``basin_max_depth_m`` is the honest cheap proxy -- a tile with a 55 m-deep
filled basin near its edge is a tile whose fill is at risk -- and the
definitive test is the apron-sensitivity diff above, which costs a second bake.

THE RESIDUAL SEAM, measured directly rather than inferred. Tiles (-5,2) and
(-5,3) baked independently at the production apron; their interiors are
adjacent world rows, so the step across the join can be read against the
terrain's own one-cell step on both sides:

    field           join step   terrain's own   ratio
    final surface    27.41 cm      26.61 cm      1.03
    depression fill  25.28 cm      25.30 cm      1.00
    accumulation     16939 m2       3713 m2      4.56
    incision          3.20 cm       2.12 cm      1.51
    channel flag     75 of 8192 join cells disagree (0.92%)

Shipped HEIGHTS across this join are fine -- 0.80 cm of excess step over the
terrain's own gradient, against the plan's claimed 0.00 cm. ACCUMULATION is
not: 4.56x the natural step, which is the same unbounded dependency the plan
names, still unfixed, and now with a number on real terrain. Heights are fine
HERE because no large basin straddles this particular edge; the
apron-sensitivity table above is what says they are not fine in general.
"""


RIDGE_DEFICIT = """\
The 6-10x ridge+peak deficit, what closed it, and what remains (2026-07-29).

Measured at a matched 10-cell (18.75 m) lookout on the pinned
060b0c927ccc807e/000000000135276f exemplars, ridge+peak was plains 0.0142-0.0158
against a real 0.0434-0.0484 and alpine 0.0023-0.0030 against 0.0332, and the
deficit is fully present after B0+B1 -- the un-eroded surface is a smooth
spline that has no crests for ANY later pass to expose. Two mechanisms were
raced head to head, and each won on the ground the other lost:

* GENTLE ground: dissection cannot do it. A till plain's real erosion is
  sub-metre; every energetic-enough carve either trenched the plain (mean
  slope 2.03 -> 3.59 deg; ridge cells were trench LIPS, at 0.56x the median
  distance-to-channel) or, made shallow via ``profile_regional_p``, produced
  no ridges at all (0.0168). What works is the CONSTRUCTIONAL term
  (``b1_constructional_amp``): real till knolls are built by ice, not carved
  by water. Baked: plains 0.0443 / valley+pit 0.0336 / mean 2.13 deg -- all
  three inside or at the real Illinois values -- with ridge cells at 1.70x
  the median distance-to-channel because the routing organises around them.
* STEEP ground: noise cannot do it. Folded noise strong enough to register at
  the lookout (4x amplitude) reads as uniform crumpled paper and its ridges
  are uncorrelated with drainage (placement 1.01-1.07); it is gated off by
  slope. What works is DISSECTION DENSITY (``channel_init_area_m2`` down to
  156): alpine 0.0030 -> 0.0213, rolling 0.0034 -> ~0.05, with an organised
  gully-and-rib hillshade and theta 0.089-0.147 (r^2 0.84-0.93).

STILL OPEN: alpine saturates at ~0.021-0.025 against the real 0.0332 --
raising the cap to 40 m buys 0.003-0.004 for +3 deg of 30 m-band drift, and
denser initiation than 156 m^2 is sub-Nyquist. The remaining gap is glacial
rock morphology (aretes, gendarmes, benches) that a stream-power + repose bake
does not model; closing it honestly needs a steep-ground process term
(threshold-slope regrade or rock-structure anisotropy tied to the carrier's
ridgelines), not more of either mechanism above. The 30 m band also drifts
under dense dissection (alpine mean 21.1 -> 23.2 deg, ridge+peak 0.057 ->
0.124 at 30 m; the direction is Copernicus-DSM-like -> 3DEP-DTM-like, within
the real span but away from the S0 input) -- read the per-class scorecard in
the 2026-07-29 ridge investigation before re-tuning any of it.
"""

PROFILE_SEAM = """\
What the profile solve does to the tile-to-tile seam -- measured, not assumed.

``incise.profile_incision`` reads its receiver's SOLVED elevation, so the
carve propagates base-level information upstream along the whole channel:
an unbounded dependency of the same class as flow accumulation
(HYDROLOGY_RESIDUALS), and one the apron cannot bound in principle. The
magnitude, however, is bounded by ``incision_cap_m``, and the join was
measured directly (2026-07-29, the APRON_BLIND_SPOT residual-seam method):
two ADJACENT 3.84 km tiles of the real alpine exemplar baked independently at
the production 960 m apron with a shared level-0 superblock, step across the
join against the terrain's own one-cell step:

    mode      join mean step   terrain's own   ratio    join max
    depth        0.599 m          0.600 m      0.999     1.36 m
    profile      0.681 m          0.681 m      1.000     2.36 m

The profile row was measured at K_dt = 1.5 with NO regional scale -- a more
aggressive carve than the shipped profile constants, so it bounds them from
above. The mean join step is AT the terrain's own gradient in both modes --
the profile solve does not add an elevation seam at this apron on this
terrain.
Its footprint is in the tail (max step 1.36 -> 2.36 m): where the join cuts a
carved channel, the two sides can disagree about the graded profile by up to
the cap. The reference-kernel apron tests deliberately use a LOCAL incision
double for the same reason ``ref_accumulate`` does not route: a tree-walking
reference would make those tests measure the reference, not the pipeline.
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
    #: process_time() per stage. The contention-robust unit, and the one to
    #: quote -- but see ``wall_seconds``: for a stage running numba's OpenMP
    #: layer this over-counts, because idle workers spin at the barrier.
    cpu_seconds: dict[str, float]
    #: Diagnostics that a contended box cannot distort (counts and extents).
    stats: dict[str, float]
    #: Coarse ring tiles that were unavailable.
    missing_coarse: tuple[tuple[int, int], ...] = ()
    #: Hex ``superblock_inputs_fingerprint`` of the level-0 superblock this
    #: tile's cross-tile hydrology came from; "" when it baked without one.
    #: This is the tile's hydrology PROVENANCE -- see ORDER_DEPENDENCE.
    superblock_fingerprint: str = ""
    #: perf_counter() per stage, same keys as ``cpu_seconds``. Recorded so the
    #: PER-STAGE parallel factor (cpu/wall) is visible: only B2b and B3 are
    #: numba parallel=True, so the bake's overall ratio is an Amdahl figure and
    #: says nothing about how well those two scale. Defaulted so an existing
    #: constructor keeps working.
    wall_seconds: dict[str, float] = field(default_factory=dict)


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
    wall: dict[str, float] = {}

    # WALL alongside CPU, per stage. process_time() stays the headline unit --
    # this box is contended and wall reads like a slow configuration -- but on
    # its own it is UNINTERPRETABLE for the two parallel stages, and that has
    # already cost one wrong diagnosis:
    #
    #   * numba's OpenMP layer SPIN-WAITS at the parallel barrier, so a worker
    #     that has finished its prange slice keeps billing CPU until the last
    #     one lands. B3.relax's CPU total is therefore ~1.7x the useful work it
    #     did (measured: the same 9216^2 step bills 2.83 CPU-s at 1 thread and
    #     4.83 CPU-s at 12 for identical output).
    #   * With only B2b and B3 parallel, the bake's headline cpu/wall is an
    #     AMDAHL number, not a scaling number: ~58% of the CPU total is in the
    #     parallel stages, so even a perfect 7.5x there caps the whole bake at
    #     2.0x -- which is exactly what it reports. Reading that 2.0x as
    #     "the parallel kernels only get 2x" is the mistake this makes
    #     impossible, because now every stage carries its own ratio.
    #
    # Stages are contiguous: each one opens with ``c0 = time.process_time()``
    # on the line after the previous ``tick``, with only comments between, so
    # wall-since-the-last-tick IS that stage's wall and no second timestamp has
    # to be threaded through nine call sites. Timings only -- no baked byte
    # moves.
    _wall_mark = time.perf_counter()

    def tick(name, c0):
        nonlocal _wall_mark
        cpu[name] = time.process_time() - c0
        now = time.perf_counter()
        wall[name] = now - _wall_mark
        _wall_mark = now

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
    # The constructional kwargs are forwarded only when the term is ON, so a
    # test double written against the plain five-argument form keeps working
    # and an amp of 0 exercises the identical call the prior bake made. A
    # kernel that CANNOT take them while the constants demand the term is a
    # hard error: silently skipping would bake a surface the identity hash
    # says has knolls and does not.
    constructional_kwargs = {}
    if consts.b1_constructional_amp > 0.0:
        import inspect

        try:
            params = inspect.signature(kernels.roughness).parameters
        except (TypeError, ValueError):  # pragma: no cover - numba dispatchers
            params = {}
        has_kw = "constructional_amp" in params or any(
            p.kind is inspect.Parameter.VAR_KEYWORD for p in params.values()
        )
        if not has_kw:
            raise RuntimeError(
                "b1_constructional_amp="
                f"{consts.b1_constructional_amp} but this roughness kernel has "
                "no 'constructional_amp' parameter; inject noise.roughness or "
                "bake with b1_constructional_amp=0"
            )
        constructional_kwargs = {
            "constructional_amp": consts.b1_constructional_amp,
            "constructional_slope_lo": consts.b1_constructional_slope_lo,
            "constructional_slope_hi": consts.b1_constructional_slope_hi,
        }
    delta = np.asarray(
        kernels.roughness(
            fine,
            cell_m,
            slope,
            seed,
            src_nyquist_m=consts.src_nyquist_m,
            **{origin_kw: origin_cells},
            **constructional_kwargs,
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
    # rec stays live through B2d for the profile solve; it is in
    # estimate_peak_bytes' count, so this frees nothing the estimate claims.
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
    #
    # "profile" solves the same law implicitly along the D8 tree, so the carve
    # is a graded long profile rather than a per-cell depth -- see the
    # incise.py module docstring for the measurements that forced it. The
    # solve reads its receiver's SOLVED elevation, so profile information
    # travels upstream along the network: a non-locality of the same class as
    # flow accumulation (HYDROLOGY_RESIDUALS), bounded in magnitude by
    # ``incision_cap_m``, and measured rather than assumed -- see
    # PROFILE_SEAM below for the per-tile-vs-single-domain numbers.
    c0 = time.process_time()
    if consts.incision_mode == "profile":
        if kernels.profile_incision is None:
            raise RuntimeError(
                "incision_mode='profile' but this BakeKernels has no "
                "profile_incision kernel; inject incise.profile_incision or "
                "bake with incision_mode='depth'"
            )
        regional = None
        if consts.profile_regional_s_ref > 0.0:
            # The carrier's 30 m-scale slope, expanded back to the fine grid:
            # a pure function of the (world-anchored, apron-consistent)
            # carrier, so it cannot introduce a seam of its own. The scale
            # factor 16 is the coarse/fine ratio whatever the geometry.
            f = geom.scale
            h_f, w_f = fine.shape
            cb = fine[:h_f - h_f % f, :w_f - w_f % f].reshape(
                h_f // f, f, w_f // f, f).mean(axis=(1, 3))
            gyc, gxc = np.gradient(cb.astype(np.float64), cell_m * f)
            regional = np.zeros(fine.shape, np.float32)
            rs = np.repeat(np.repeat(np.hypot(gxc, gyc), f, axis=0), f, axis=1)
            regional[:rs.shape[0], :rs.shape[1]] = rs
            if rs.shape[0] < h_f:
                regional[rs.shape[0]:, :] = regional[rs.shape[0] - 1: rs.shape[0], :]
            if rs.shape[1] < w_f:
                regional[:, rs.shape[1]:] = regional[:, rs.shape[1] - 1: rs.shape[1]]
            del cb, gyc, gxc, rs
        # regional_p forwarded only when set, for the same test-double reason
        # as the constructional kwargs above; 0 means "use stream_n".
        regional_p_kwargs = (
            {"regional_p": consts.profile_regional_p}
            if consts.profile_regional_p > 0.0 else {}
        )
        # bake_ver 6: material strength modulates the CARVE, not only the
        # threshold. Same field construction as B3 below, keyed on the FILLED
        # surface -- the rock actually being carved -- and world-anchored, so
        # apron overlaps agree exactly. Forwarded only when live, for the
        # test-double reason above; ratio 1 (or a zero-amplitude field)
        # reproduces the bake_ver-5 incision bit-for-bit.
        strength_kwargs = {}
        if consts.incision_strength_ratio != 1.0 and (
                consts.repose_spatial_amp_deg > 0.0
                or consts.repose_strata_amp_deg > 0.0):
            from .noise import repose_erodibility, repose_field

            strength_kwargs = {
                "erodibility": repose_erodibility(
                    repose_field(
                        filled,
                        cell_m,
                        seed,
                        origin_cells,
                        base_deg=consts.repose_deg,
                        spatial_amp_deg=consts.repose_spatial_amp_deg,
                        spatial_wavelength_m=consts.repose_spatial_wavelength_m,
                        strata_amp_deg=consts.repose_strata_amp_deg,
                        strata_wavelength_m=consts.repose_strata_wavelength_m,
                        strata_fold_amp_m=consts.repose_strata_fold_amp_m,
                        strata_fold_wavelength_m=consts.repose_strata_fold_wavelength_m,
                        min_deg=consts.repose_min_deg,
                        max_deg=consts.repose_max_deg,
                    ),
                    base_deg=consts.repose_deg,
                    max_deg=consts.repose_max_deg,
                    ratio=consts.incision_strength_ratio,
                )
            }
        eroded = np.asarray(
            kernels.profile_incision(
                filled,
                rec,
                acc64,
                cell_m,
                K_dt=consts.profile_K_dt,
                m=consts.stream_m,
                n=consts.stream_n,
                cap_m=consts.incision_cap_m,
                a_crit_m2=consts.channel_init_area_m2,
                gate_q=consts.channel_init_q,
                regional_slope=regional,
                regional_s_ref=consts.profile_regional_s_ref,
                sea_taper_top_m=consts.sea_taper_top_m,
                sea_taper_bottom_m=consts.sea_taper_bottom_m,
                **regional_p_kwargs,
                **strength_kwargs,
            ),
            dtype=np.float32,
        )
        del regional, strength_kwargs
        depth = filled - eroded
    else:
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
    del rec
    tick("B2d.stream_power", c0)

    # -- B3: slope-limited thermal relaxation, AFTER incision so gully walls
    # weather and spoil forms talus.
    #
    # bake_ver 5: the threshold is a per-cell MATERIAL STRENGTH FIELD rather
    # than one global angle (see the repose_* constants and noise.repose_field
    # for the measured motivation: a single angle planes every relaxed face to
    # the same slope, which voxelises as parallel contour-terrace corduroy).
    # The field is keyed on the PRE-relaxation surface — strata are glued to
    # the rock — and on the same world-anchored lattice/seed machinery as B1,
    # so apron overlaps agree exactly. Both amplitudes at 0 reproduce the
    # scalar call bit-for-bit.
    c0 = time.process_time()
    repose = consts.repose_deg
    if consts.repose_spatial_amp_deg > 0.0 or consts.repose_strata_amp_deg > 0.0:
        from .noise import repose_field  # lazy, like load_kernels: numerics stay out of import time

        repose = repose_field(
            eroded,
            cell_m,
            seed,
            origin_cells,
            base_deg=consts.repose_deg,
            spatial_amp_deg=consts.repose_spatial_amp_deg,
            spatial_wavelength_m=consts.repose_spatial_wavelength_m,
            strata_amp_deg=consts.repose_strata_amp_deg,
            strata_wavelength_m=consts.repose_strata_wavelength_m,
            strata_fold_amp_m=consts.repose_strata_fold_amp_m,
            strata_fold_wavelength_m=consts.repose_strata_fold_wavelength_m,
            min_deg=consts.repose_min_deg,
            max_deg=consts.repose_max_deg,
        )
    z = np.asarray(
        kernels.relax(
            eroded,
            cell_m,
            repose_deg=repose,
            iters=consts.thermal_iters,
            rate=consts.thermal_rate,
        ),
        dtype=np.float32,
    )
    del repose
    tick("B3.relax", c0)

    # -- B4: post-thermal meso relief. See noise.meso_relief for why this band
    # can live NOWHERE else: B1 substrate at these wavelengths is planed by
    # thermal back into the threshold pattern, and a client-side band coherent
    # over 6-13 m makes pits the drainage lattice resolves (measured
    # 0.04-0.33% stranded, realization-dependent). Here thermal never sees it
    # and the refill below resolves every basin it could create.
    c0 = time.process_time()
    if consts.meso_amp15_m > 0.0 or consts.meso_amp11_m > 0.0:
        from .noise import meso_relief  # lazy, same reason as repose_field

        # The gate needs the DOWNSTREAM slope of this exact surface: a gully
        # bed between steep walls must read its own gentle profile, or the
        # band perturbs bed long-profiles into the codec's quantization floor
        # (see meso_relief's flow_slope note). rec4 is a 680 MB transient,
        # dropped before the field is built.
        rec4, d8s4 = kernels.d8_receivers(z, cell_m)
        z_pre = z
        z = z + meso_relief(
            z,
            cell_m,
            seed,
            origin_cells,
            amp15_m=consts.meso_amp15_m,
            amp11_m=consts.meso_amp11_m,
            slope_lo=consts.meso_slope_lo,
            slope_hi=consts.meso_slope_hi,
            flow_slope=np.asarray(d8s4, dtype=np.float32),
        )
        del d8s4
        # DESCENT ENFORCEMENT along the pre-meso D8 tree, and this is the
        # guarantee, not the gate. The gate keeps the band off gentle beds;
        # what it cannot do is bound the band's ALONG-FLOW gradient where the
        # gate itself varies, and a reach whose post-meso drop lands under the
        # codec's 100 mm LSB dams on RECONSTRUCTION even though no pit exists
        # at float precision (measured three times, at 10.4%, 5.0% and 0.9%
        # of the repro window, across three gate refinements -- the class
        # cannot be tuned away). So: every cell is raised until it keeps at
        # least min(its pre-meso drop, refill_eps_m) over its own pre-meso
        # receiver. Every interior cell then still has a strictly lower
        # neighbour (no pit, structurally) and every reach that was
        # codec-proof stays codec-proof. Pure-numpy fixed point: each pass
        # propagates one tree level; the raise is bounded by the band's own
        # amplitude, so convergence is tens of passes, not thousands.
        recf = np.asarray(rec4, dtype=np.int64).ravel()
        del rec4
        n_cells = recf.size
        self_idx = np.arange(n_cells, dtype=np.int64)
        tgt = np.where(recf >= 0, recf, self_idx)
        del recf
        zp = z_pre.ravel()
        drop = np.minimum(zp - zp[tgt], np.float32(consts.refill_eps_m)
                          ).astype(np.float32)
        del zp, z_pre, self_idx
        zf = np.ascontiguousarray(z, dtype=np.float32).ravel()
        for _ in range(256):
            need = zf[tgt] + drop
            if not (zf < need).any():
                break
            np.maximum(zf, need, out=zf)
        del tgt, drop
        z = zf.reshape(z.shape)
        del zf
    tick("B4.meso", c0)

    # -- B4b: micro-refill, belt and braces behind the descent enforcement
    # above. Producers of post-fill pits: thermal (its steepest-pair rule
    # bounds what a cell gives, not what it receives -- measured as one 41 mm
    # sink on the exemplar) and, before the enforcement existed, the meso
    # band. "The carrier drains" is a CONTRACT (0 sinks / 0.0% stranded on
    # steep ground), so any stray basin is resolved here, before the codec
    # sees the ground. The AUTO epsilon, deliberately: refill_eps_m used as a
    # flood epsilon domes every wide flat it refloods (lake floors became
    # 5-17 m cones -- flat-shaded scars in the hillshade); the auto epsilon
    # raises only true pits by their own depth.
    c0 = time.process_time()
    z = np.asarray(kernels.fill_depressions(z, **fill_kwargs), dtype=np.float32)
    tick("B4b.refill", c0)

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
        "wall_seconds": wall,
    }


#: The sub-stage surfaces ``bake_tile`` hands to a ``stage_sink``, in pipeline
#: order, with the ``bake_padded_domain`` output key each one is a view of.
#: These exist for OBSERVABILITY (tools/dump_stage_heightfields.py): the final
#: surface has repeatedly been measured as wrong without the measurement being
#: able to say WHICH stage made it wrong, because only the finished bake was
#: inspectable. A sink changes no baked byte and no identity hash.
STAGE_SINK_FIELDS: tuple[tuple[str, str], ...] = (
    ("B0B1.carrier_rough", "carrier_plus_roughness"),
    ("B2a.filled", "filled"),
    ("B2c.accumulation_m2", "acc"),
    ("B2d.incision_depth_m", "incision"),
    ("B2d.incised", "eroded"),
    ("B3.relaxed", "z"),
)


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
    stage_sink: "Callable[[str, np.ndarray], None] | None" = None,
) -> BakeResult:
    """Bake one coarse tile's fine tier. Interior only; the apron is discarded.

    ``coarse_fetch(x, y)`` returns that coarse tile's elevation in METRES as a
    (coarse_tile_px, coarse_tile_px) array, or None if it does not exist.
    ``inflow_source`` is the level-0 flow superblock covering this tile; None
    means "no cross-tile hydrology", which is only correct for a test.

    ``stage_sink(name, interior)`` is called once per `STAGE_SINK_FIELDS` entry
    with the INTERIOR (apron already cropped) of that sub-stage's grid, in
    pipeline order, before the result is assembled. The arrays are views into
    the bake's own grids: a sink that keeps one must copy it. Purely an
    observer -- it cannot change what is baked, and passing one does not touch
    the bake identity.
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
    if stage_sink is not None:
        for name, key in STAGE_SINK_FIELDS:
            stage_sink(name, out[key][sl, sl])
    z = np.ascontiguousarray(out["z"][sl, sl])
    acc = np.ascontiguousarray(out["acc"][sl, sl])
    incision = out["incision"][sl, sl]
    gain = out["thermal_gain"][sl, sl]
    # The DEPRESSION-FILLED surface, not the relaxed one: it is what
    # stream_power tapered against, so the flow plane's channel flag and the
    # incision it describes agree cell for cell.
    plane = flow_plane(acc, incision, gain, consts, elev_m=out["filled"][sl, sl])

    basin_depth = out["filled"] - out["carrier_plus_roughness"]
    padded_basin = basin_depth > 0.0
    # THE SOUND CONDITION (see APRON_BLIND_SPOT): a filled flat whose whole
    # extent is inside the padded domain has its spill point inside the padded
    # domain too, so priority-flood enters it through the terrain and the
    # epsilon staircase is a function of the terrain alone. Only a flat that
    # REACHES THE PADDED BORDER is entered from the border, and only then can
    # two neighbouring bakes cross it in different directions. Measured on the
    # perimeter, O(padded_fine_px) work, and it needs no connected-component
    # labelling -- which matters because pipeline.py may not import scipy.
    border_basin = int(
        padded_basin[0, :].sum()
        + padded_basin[-1, :].sum()
        + padded_basin[:, 0].sum()
        + padded_basin[:, -1].sum()
    )
    border_cells = 2 * padded_basin.shape[0] + 2 * padded_basin.shape[1]
    basin = padded_basin[sl, sl]
    del padded_basin
    # The honest cheap proxy for "this tile's fill is at risk of being decided
    # by the domain boundary": how deep the deepest filled depression is. On
    # (-5,2) it read 55.21 m at the 960 m apron and 88.79 m at 1920 m.
    max_basin_depth = float(basin_depth[sl, sl].max())
    del basin_depth
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
        # HYDROLOGY PROVENANCE, per tile (ORDER_DEPENDENCE). -1 means the tile
        # baked with NO superblock at all, which is a stronger statement than
        # "an incomplete one" and must not read as 0 missing tiles.
        "superblock_missing_tiles": (
            -1.0 if inflow_source is None else float(len(inflow_source.missing_tiles))
        ),
        "superblock_complete": (
            0.0 if inflow_source is None or not inflow_source.complete else 1.0
        ),
        # THE APRON'S BLIND SPOT, per tile. See APRON_BLIND_SPOT.
        #
        # ``max_basin_run_m``/``basin_exceeds_apron`` measure the LONGEST axis
        # run of the filled mask, which on real terrain is a valley floor's
        # LENGTH, not the width of anything. They are kept because they are
        # what the plan quotes, and demoted: the condition that actually
        # decides whether the interior is the infinite-domain answer is
        # ``basin_reaches_padded_border``.
        "basin_cells_frac": float(basin.mean()),
        "basin_max_depth_m": max_basin_depth,
        "max_basin_run_m": float(run_px) * geom.fine_pixel_m,
        "basin_exceeds_apron": float(run_px * geom.fine_pixel_m > geom.apron_m),
        "padded_border_basin_cells": float(border_basin),
        "padded_border_basin_frac": float(border_basin) / float(border_cells),
        "basin_reaches_padded_border": float(border_basin > 0),
        "peak_bytes_estimate": float(estimate_peak_bytes(geom)),
    }
    return BakeResult(
        tile_x=tile_x,
        tile_y=tile_y,
        elevation_m=z,
        accumulation_m2=acc,
        flow=plane,
        cpu_seconds=out["cpu_seconds"],
        wall_seconds=out["wall_seconds"],
        stats=stats,
        missing_coarse=tuple(missing),
        superblock_fingerprint=(
            "" if inflow_source is None else inflow_source.fingerprint_hex
        ),
    )
