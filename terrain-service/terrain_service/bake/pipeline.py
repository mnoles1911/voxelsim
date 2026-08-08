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
A production padded domain is 9216^2 = 84.9 Mcell, so every full-domain
float32 or int32 grid is 340 MB and every float64 one is 680.
``estimate_peak_bytes`` puts the live set at ~3 GB, which is well above the
"~0.5 GB of bake grids" the plan's offline-appendix assumes. Size the pod
accordingly.

The peak stage is B2d, and the things that used to make it the peak were
mostly bookkeeping rather than data: the D8 receiver array was int64 (680 MB
for indices that cannot exceed 2^31), the implicit solve re-sorted the same
surface B2c had already sorted (a 680 MB int64 argsort plus its 340 MB int32
copy), and the regional-slope field was a 16x16-replicated 340 MB expansion of
a 1.3 MB one. None of those are gone because anything was approximated -- each
removal is byte-identical on the real tile and is asserted that way.

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

import dataclasses
import hashlib
import json
import struct
import time
from dataclasses import dataclass, field
from typing import Callable, ClassVar

import numpy as np

# Pure numpy, no kernels, no scipy -- safe to import eagerly here (the lazy
# imports elsewhere in this file exist for scipy/numba, not for module cycles).
from . import basins as _basins
from . import province as _province
from . import water as _water

__all__ = [
    "BAKE_VERSION",
    "PRODUCT_STAGE_ORDER",
    "TERRAIN_VERSION",
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
    "inject_edge_discharge",
    "load_kernels",
    "superblock_runoff_mm_yr",
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
#:
#: 6 -> 7 (2026-08-01, LANDFORM PROVINCES, Tier 1 -- docs/landform-provinces-plan.md):
#: the fine bake stopped applying one global constant set everywhere. Four bake
#: constants -- ``profile_K_dt``, ``channel_init_area_m2``, ``channel_init_q``,
#: ``stream_m`` -- plus the two meso amplitudes are now PER-CELL FIELDS blended
#: from a province partition (FLUVIAL / GLACIAL / ARID / LOWLAND) that is itself
#: derived from relief, elevation, temperature and precipitation. The counter
#: moves for two independent reasons and either alone would justify it:
#:
#:   * the domain now carries CLIMATE. ``bake_padded_domain`` used to receive
#:     elevation only; the coarse tile's ``(4, 512, 512)`` uint8 climate plane
#:     is now gathered over the same 3x3 ring and reaches the province
#:     discriminants. Nothing else in the bake reads it.
#:   * ``province_strength = 0`` reproduces the bake_ver-6 surface exactly (the
#:     partition collapses to pure FLUVIAL, whose multipliers are all 1.0, and
#:     a constant parameter field gathers to the same scalar the scalar path
#:     used) -- so the constant rolls the id on its own, and the counter moves
#:     because B2d and B4 grew the field path.
#:
#: WHAT THIS DOES NOT DO. It does not restore a seam guarantee: see
#: ``APRON_BLIND_SPOT``, which measured that guarantee already violated by
#: 1.05% of the shipped interior. Province fields add no influence radius (they
#: are pointwise at the point of use, and their one non-local step is a
#: landform smooth bounded well under the apron), so they neither worsen nor
#: repair it. It also does not touch ``mfd_p``, ``stream_n`` or
#: ``incision_cap_m``: the last two are scalars inside the numba Newton kernel,
#: and ``mfd_p`` is hashed as a SCALAR into ``superblock_inputs_fingerprint``
#: and drives the superblock MFD at 30 m and 120 m/px, so a per-cell value at
#: 1.875 m would have no counterpart at the parent levels and parent and child
#: would route differently -- worsening HYDROLOGY_RESIDUALS #3.
#: bake_ver 8 -- B5, the basin registry (docs/water-system-architecture.md item 3).
#: The bake stops levelling registered depressions into rock and ships a table
#: describing them. Every baked tile is invalidated; that is expected and
#: currently cheap.
#:
#: ===========================================================================
#: TWO COUNTERS, SPLIT AT bake_ver 9. READ THIS BEFORE BUMPING EITHER.
#: ===========================================================================
#: Until P2 there was ONE counter, and it did two unrelated jobs:
#:
#:   1. it seeded the world -- ``roughness_seed`` mixes it in, so bumping it
#:      RE-ROLLS THE B1 ROUGHNESS FIELD EVERYWHERE and the ground itself
#:      changes;
#:   2. it recorded which PRODUCTS a tile carries -- the sections in the file.
#:
#: Those are opposite in kind, and fusing them made every additive change to
#: the wire format cost a new world. Measured, when P2 was scoped: bumping the
#: single counter 8 -> 9 moved ``roughness_seed(20260719)`` from
#: 0x7e1ec856567c4fb5 to 0xc2b0bf0a8b32531f. Every basin in the 256-tile lake
#: survey, every channel the 25-tile bank probe walked, every vista site and
#: every spawn coordinate the owner holds describes ground that would simply
#: stop existing -- to add a section that changes no elevation byte.
#:
#: So:
#:
#:   TERRAIN_VERSION  decides the GROUND. Feeds ``roughness_seed`` and
#:                    ``bake_identity_payload``. Bump it when the surface
#:                    changes -- a new stage, a new constant in
#:                    ``BakeConstants.as_payload``, a kernel that moves a
#:                    height. Bumping it is a NEW WORLD and invalidates every
#:                    measurement, screenshot and site anyone holds.
#:
#:   BAKE_VERSION     decides the PRODUCTS. Stamped in the tile header as
#:                    ``bake_ver`` and hashed through
#:                    ``product_identity_payload``. Bump it when the bake emits
#:                    something new or differently -- a section, a table
#:                    layout, a threshold that decides written bytes. Tiles are
#:                    re-baked, but onto IDENTICAL ground.
#:
#: BOTH feed ``fine_provider_id`` (via ``providers/diffusion.py``), so content
#: addressing keeps meaning what it says: a tile whose water bytes differ never
#: shares an id with one whose do not. That is deliberate and is the reason the
#: product half is hashed rather than merely stamped -- the namespace holding
#: two mutually incompatible formats under one id is the failure
#: ``_tile_format_fingerprint`` was written to prevent.
#:
#: THE GATE THAT KEEPS THE SPLIT HONEST is
#: ``tests/test_bake_terrain_identity.py``: re-baking a resident fine tile must
#: reproduce its elevation plane BYTE FOR BYTE. If that ever fails, something
#: has leaked from the product half into the terrain half and the split has
#: stopped doing its job -- which is worth more than any green suite here,
#: because the whole argument for the split is that the ground does not move.
#:
#: And ``test_constants_partition_is_exhaustive`` asserts every
#: ``BakeConstants`` field lands in exactly one of ``as_payload`` (terrain) or
#: ``product_payload`` (product). A constant in NEITHER would decide baked
#: bytes while rolling no identity at all, which is the silent-drift failure
#: the whitelist form of ``as_payload`` otherwise invites.
#:
#: bake_ver 9 -- P2: runoff-weighted discharge, water heads and the graded
#: water plane (SECTION_WATER_INDEX/DATA). Additive: no elevation byte moves,
#: which is exactly what the terrain-identity gate proves.
#:
#: bake_ver 10 -- task #49: the pyramid CARRIES Q instead of B6 reconstructing
#: it from area times local runoff. See ``CARRIED_DISCHARGE``.
#:
#: WHY THIS IS A PRODUCT BUMP AND NOT A TERRAIN ONE, since that decision is
#: worth ~67 M control points per tile and was checked rather than assumed:
#: stream-power incision is ``K * A^m * S^n`` -- it reads the AREA field, which
#: this change does not touch. The new discharge raster is a SECOND quantity
#: beside it, consumed only by B6, whose whole output is the water plane. No
#: entry in ``STAGE_ORDER`` changes, no field in ``BakeConstants.as_payload``
#: changes, and ``tests/test_bake_terrain_identity.py`` is the gate that
#: proves it rather than the argument above.
#:
#: bake_ver 11 -- the drawable threshold drops about an octave
#: (``water_min_width_px`` 2.0 -> 1.5) and the water pass routes its discharge
#: single-receiver (``water_flow_single_receiver``). Both are PRODUCT changes by
#: the same argument bake_ver 10 made and both were checked the same way: the
#: area field, ``mfd_p``, ``STAGE_ORDER`` and every field of ``as_payload`` are
#: untouched, so ``A^m`` reads exactly what it read before. The two halves are
#: the two measured causes of a wet mask that came out as 2,014 pieces with a
#: longest reach of 1,113 m on a four-tile corridor -- see the constants
#: themselves for the apportionment and
#: ``docs/measurements/river-drawable-and-concentration-2026-08-04.txt``.
#:
#: bake_ver 12 -- the two halves bake_ver 11 left open, both PRODUCT changes on
#: the same argument and checked the same way (``as_payload``, ``mfd_p``,
#: ``STAGE_ORDER`` and the area field are untouched, so ``A^m`` reads what it
#: read before):
#:
#:   * ``water_pyramid_single_receiver`` -- the PYRAMID's discharge sweep goes
#:     single-receiver too, so the boundary condition a fine tile receives is
#:     concentrated the same way the field it computes from that boundary is.
#:     With the two disagreeing, a river arrived at a seam as a fan: measured on
#:     (-11,-5)/(-11,-6), the same 8.6e7 m^3/yr crossed either way (D8/MFD =
#:     0.99 on the total) but MFD's largest single crossing held 8.66e6 against
#:     D8's 2.02e7, and the downstream tile read 3.05% of the upstream trunk at
#:     the crossing column, under a cut it then failed. 35 of 36 raw components
#:     ended within one pixel of a tile edge before this.
#:   * ``water_width_from_law`` (retired at bake_ver 13, where it became
#:     ``water_extent_mode == "law"``) -- ``channel_width_m(Q)`` decides the drawn
#:     extent the way ``water_depth_m(Q)`` already decides depth, clamped to
#:     ground below the reach's own surface. bake_ver 11's plane drew a
#:     centreline: 99.21% of wet pixels were one 1.875 m pixel against a law
#:     width of 3.53 m at p50.
#:
#: bake_ver 13 -- ``water_extent_mode`` replaces the width law with a LATERAL
#: FILL: a cell is wet when it stands below the surface of the water it drains
#: into (``water.fill_to_local_surface``). A PRODUCT change on the same
#: argument as 10/11/12 and checked the same way -- ``as_payload``, ``mfd_p``,
#: ``STAGE_ORDER`` and the area field are untouched, so ``A^m`` reads what it
#: read before and ``tests/test_bake_terrain_identity.py`` is the gate that
#: proves the ground did not move rather than the sentence that asserts it.
#:
#: WHAT WAS WRONG, flown and reported by the owner: the river "cuts a straight
#: path across a valley floor ... to the left and right of the water channel,
#: there is empty air, not a river bank". bake_ver 12 painted a ribbon of
#: formula-decided width. It draws 3.5 m at p50 where the terrain at the drawn
#: level allows p50 11-28 m and p90 47-163 m, so the water stopped at a number
#: rather than at the ground.
#:
#: WHAT IS DIFFERENT ABOUT IT. Every previous extent rule read DISCHARGE. This
#: one reads the ground and the water's own surface and nothing else: no width,
#: no radius, no relief term. Extent therefore stops being a property of Q and
#: becomes a property of the valley, which is the point -- the same river is
#: wide on a floodplain and narrow in a gorge.
#:
#: WHAT IT COSTS AND WHAT IT MOVES, measured on the corridor before it shipped:
#: about 3 CPU-s per tile (one topological sweep over the D8 forest, no heap,
#: no search) and 7.15x the wet cells. The centreline itself is untouched, so
#: the long profile -- and the property that the surface never rises going
#: downstream -- is the same array bake_ver 12 produced.
#: bake_ver 14 -- THE WATER IS MADE TO TOUCH ITSELF DOWN A SLOPE.
#:
#: bake_ver 13's extent rule answers "which cells are wet". Nothing answered
#: whether the water in two adjacent cells actually TOUCHES, and the client
#: draws one flat slab per fine pixel (``lakes.h``: the water surface is
#: resolved by NEAREST pixel and the fill runs from ground to that surface), so
#: two neighbours only share a voxel face when the lower one's surface reaches
#: the higher one's bed. On a descending bed it does not, and a D8 path that
#: steps diagonally does not share a face at all. Measured on the shipped bv13
#: corridor: the same wet cells are 24-75 components in plan and 3,409-7,987 as
#: drawn, and on ground steeper than 0.15 up to 20.3% of wet cells touch
#: nothing. That is the owner's "several cubes of water placed in a general
#: direction but disconnected going down the slope".
#:
#: ``water.bridge_to_face_contact`` is the whole change. It runs after whichever
#: extent rule ran, adds the corner a diagonal step needs and raises each cell's
#: surface to reach its face neighbours' beds. It is EXACTLY A NO-OP on ponded
#: water -- every cell in a pool is already submerged below its neighbours'
#: level -- so the surface stays horizontal where the water stands and becomes
#: bed-parallel where it runs, with no slope threshold anywhere to seam the
#: river. TERRAIN_VERSION does not move: no ground byte changes.
TERRAIN_VERSION = 8
BAKE_VERSION = 20


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

    # -- B5 basin registry (bake_ver 8) ------------------------------------
    #
    # docs/water-system-architecture.md §4.2-§4.3. These DECIDE BAKED BYTES -- a
    # registered basin is re-opened in the shipped elevation plane -- so they
    # ride bake_identity_payload like every other constant here. Pinned from
    # tools/lake_survey.py's 12-tile sweep (docs/lake-survey/lake-survey.md),
    # not chosen: at 2 m / 2500 m2 the registry is a median of 65 rows per
    # tile and a worst case of 266, i.e. 8.5 KB against 26.6 MB of compressed
    # elevation; at 1 m / 0 m2 it is 4,082 rows and mostly puddles.
    #: Minimum depth of a registered basin's deepest cell, metres.
    basin_min_depth_m: float = 2.0
    #: Minimum footprint at the spill level, m^2. 2500 m^2 is 50 m across.
    basin_min_area_m2: float = 2500.0
    #: v1 registers INTERIOR basins only. A basin crossing the tile edge is
    #: seen by each neighbour from a different padded domain and the two need
    #: not agree. The excluded ones are COUNTED (survey: 57% of qualifying
    #: components, 4,609 ha over 12 tiles) so the cost is a number.
    basin_exclude_spanning: bool = True
    #: A depression spilling at or below sea level is sea floor, not a lake.
    basin_require_above_sea: bool = True
    #: Water-balance constants, mirroring bake/basins.WaterBalance. Restated
    #: here rather than referenced so that changing the balance rolls the bake
    #: identity -- the `kind` and `surface_mm` it produces are shipped bytes.
    basin_pet_a: float = 300.0
    basin_pet_b: float = 25.0
    basin_pet_c: float = 0.05
    basin_pet_floor_mm: float = 100.0
    basin_budyko_n: float = 2.0
    basin_min_lake_depth_m: float = 0.5
    basin_salt_aridity: float = 0.35
    basin_seasonal_cv_pct: float = 55.0

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

    # -- landform provinces (bake_ver 7) -----------------------------------
    #
    # See docs/landform-provinces-plan.md and bake/province.py. These are the
    # DISCRIMINANT thresholds only; the per-province multipliers live in
    # province.PROVINCE_MULTIPLIERS and ride the identity through
    # bake_identity_payload.
    #
    #: Master dial. 0 collapses the partition to pure FLUVIAL, whose
    #: multipliers are all 1.0, and therefore reproduces the bake_ver-6 surface
    #: exactly -- which is the property that makes every other constant here
    #: safe to retune. 1 is full strength.
    province_strength: float = 1.0
    #: Landform-scale smoothing applied to every discriminant, metres, on the
    #: PADDED coarse domain.
    #:
    #: THIS IS AN APRON OBLIGATION, not a taste knob. Climate arrives at 30 m/px
    #: and uint8-quantised (precipitation's LSB is 47 mm/yr), so an unsmoothed
    #: discriminant prints 30 m blocks into erosion intensity. But the smooth is
    #: also the ONLY non-pointwise step province adds, so its influence radius
    #: must stay well inside the 960 m apron or a tile's interior would read a
    #: cell its neighbour's padded domain does not have. 480 m is half the
    #: apron, i.e. 16 coarse cells of total radius against 32 available.
    #: ``province_fields`` refuses a value that reaches the apron.
    province_smooth_m: float = 480.0
    #: Relief discriminant: smoothed 30 m-scale slope (dimensionless, rise/run)
    #: at which terrain stops reading as "gentle" and starts reading as "high
    #: relief". 0.03 is a till plain (the plains exemplar's regional slope is
    #: ~0.03); 0.15 is a mountain flank. Deliberately the SAME quantity
    #: ``profile_regional_s_ref`` keys the regional-energy factor on, because
    #: it is the one piece of class-identity information the bake legitimately
    #: has and it is world-anchored by construction.
    province_relief_lo: float = 0.03
    province_relief_hi: float = 0.15
    #: Temperature discriminant, degrees C mean annual. Below cold_c the ground
    #: reads fully glacial-capable, above temperate_c not at all. -2 C is near
    #: the MAAT at which cirque glaciers persist; 6 C is comfortably temperate.
    province_cold_c: float = -2.0
    province_temperate_c: float = 6.0
    #: Precipitation discriminant, mm/yr. 300 is the conventional arid ceiling
    #: and 800 a humid floor. NOTE the shipped conditioning cannot currently
    #: produce an arid interior at all (plan, "What provinces do not solve":
    #: DESERT 1.84%, SAVANNA 0.00%), so ARID is wired but untuned and unjudged.
    province_arid_mm: float = 300.0
    province_humid_mm: float = 800.0
    #: Elevation band, metres, over which "lowland" fades out. Combined with
    #: LOW relief, not on its own: a high plateau is gentle but is not a
    #: floodplain.
    province_lowland_elev_lo_m: float = 150.0
    province_lowland_elev_hi_m: float = 600.0

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

    # -- P2 water plane (bake_ver 9) ---------------------------------------
    #
    # PRODUCT constants, not terrain ones: every field below decides bytes in
    # SECTION_WATER_*, and NONE of them can move a height. They are hashed
    # through ``product_payload`` rather than ``as_payload``, so retuning them
    # rolls ``fine_provider_id`` (a tile whose water differs must not share an
    # id) without re-rolling the roughness seed (the ground must not move).
    # See the TERRAIN_VERSION/BAKE_VERSION note at the top of this module.

    #: Discharge, m^3/yr, at which a watercourse is PERENNIAL -- wet in every
    #: month rather than after rain. 10 L/s. See ``bake.water`` for why this is
    #: a flow rate rather than the plan's open-ended "10^3-10^4 m^3/yr", and
    #: why it is deliberately NOT ``channel_init_area_m2``: 156 m^2 is where
    #: the bake carves a swale, which in most climates is a dry gully.
    water_q_perennial_m3_yr: float = 315576.0
    #: Minimum drawn channel width, in FINE PIXELS. The Q threshold the plane
    #: is actually written over is derived from this through the width law
    #: (``water.q_drawable_m3_yr``), so the two cannot drift: a channel at
    #: initiation is 1.5 m wide against a 1.875 m pixel, and phase 2 puts water
    #: only on reaches wide enough for the raster to hold a wet bed.
    #:
    #: 2.0 -> 1.5 at bake_ver 11, AND IT IS A WIDTH DECISION, NOT A KNOB TURN.
    #: The diagnosis measured what 2 px actually costs: against the corridor's
    #: median implied runoff (q/acc = 0.1883 m/yr) a cut at 3.1467e6 m^3/yr is a
    #: catchment-area cut at 2^23.99, a full octave above the 2^23 the design
    #: record assumed rivers start at, and 90.29% of the DRY cells on the area
    #: network sit within a factor of two of it. So the plane was not describing
    #: a sparse world, it was drawing a line an octave up a network that is
    #: densest right there.
    #:
    #: 1.5 px is derived, not picked at 2x: it is the width at which the ratio
    #: comes out at 1.042 octaves (0.486x), which is what "about an octave" is
    #: in this law's own units.
    #:
    #: WHAT THE NEW NUMBER SAYS ABOUT THE SMALLEST DRAWN RIVER. The threshold is
    #: a statement about ``channel_width_m(Q)``, so the marginal reach is one
    #: whose channel is 2.812 m across rather than 3.750 m -- at the client's
    #: 100 mm voxels, 28 voxels rather than 37. That is still a stream you wade
    #: rather than step over.
    #:
    #: 1.5 -> 1.0 at bake_ver 14, on the owner's call. The sentence that used to
    #: end this paragraph called 1.5 px "the floor" because at 1.0 px the
    #: marginal channel is 1.875 m, "a creek drawn as a raster artefact rather
    #: than a river" -- and that argument was measuring against a river this
    #: world does not contain. ``tools/survey_world_water.py trunk`` puts the
    #: LARGEST river here at 14.2 m^3/s draining 1,960 km^2 (6.6% of the world's
    #: 29,911 km^2 of land). Against a 27 m trunk a 1.9 m creek is not a raster
    #: artefact, it is a numerous and real member of the only network there is.
    #: The cut falls 1.5286e6 -> 5.5250e5 m^3/yr, a factor of 2.77, so about
    #: 2.8x more network is drawn.
    #:
    #: 0.75 px IS the floor, and unlike the old claim it is a property of the
    #: law rather than a judgement: ``channel_width_m`` is anchored at 1.5 m for
    #: ``water_q_perennial_m3_yr``, so ``q_drawable_m3_yr`` SATURATES there.
    #: 0.75 px and 0.50 px both return 3.1558e5 m^3/yr (10.0 l/s); below 0.75
    #: the constant stops meaning anything at all.
    #:
    #: What it is NOT is a promise about how wide the RASTER draws it. The plane
    #: is wet where a cell's own Q clears the cut, so the drawn ribbon is however
    #: many adjacent cells clear it -- an emergent property of the accumulation,
    #: not of this law. That distinction is worth the paragraph because the two
    #: were accidentally in agreement at bake_ver 10 (drawn p50 5.30 m against a
    #: law p50 of 4.87 m) and are not at bake_ver 11. See
    #: ``water_flow_single_receiver``, which is what changed it, and the
    #: measurements file, which reports both.
    #:
    #: In flow: 3.1467e6 -> 1.5286e6 -> 5.5250e5 m^3/yr, i.e. 0.100 -> 0.048 ->
    #: 0.0175 m^3/s (99.7 -> 48.4 -> 17.5 l/s). Still 1.75x
    #: ``water_q_perennial_m3_yr``, so §4.1's honesty clause survives -- there
    #: remain perennial reaches this raster declines to draw, and
    #: ``water_head_mask`` still reports both counts. At 0.75 px it would not:
    #: the cut would equal the perennial anchor exactly.
    water_min_width_px: float = 1.0
    #: Route the water pass's DISCHARGE single-receiver (D8) instead of MFD.
    #:
    #: THE TERRAIN'S ROUTING IS UNTOUCHED BY THIS. ``mfd_p`` still decides the
    #: area field, hence ``A^m``, hence every height; this flag reaches only the
    #: one extra sweep in B6 whose output is the water plane. That separation is
    #: why it is a PRODUCT field: it cannot move a metre of ground.
    #:
    #: WHY. On the measured corridor 25-33% of network cells have no strictly
    #: lower neighbour holding as much as they do -- their entire accumulation
    #: was split -- and a Q field consumed by a hard threshold turns each of
    #: those splits into a dry gap in a wet reach. Walking one channel found
    #: fifty wet-dry-wet excursions in 2,001 steps. Under any single-receiver
    #: rule that count is 0 by construction.
    #:
    #: WHY NOT A LARGER ``mfd_p`` CONFINED TO THIS PASS, which was the other
    #: candidate. Two reasons, and the second is decisive:
    #:
    #:   * ``mfd_p`` is hashed as a scalar into ``superblock_inputs_fingerprint``
    #:     and drives the superblock MFD at 30 m and 120 m/px as well as the
    #:     fine one. A second scalar that moved only the fine water sweep would
    #:     have the pyramid and the tile routing the same water two different
    #:     ways; threading it through the pyramid instead invalidates every
    #:     cached superblock through a fingerprint that does not currently see
    #:     it. Either way it is a routing change reaching a TERRAIN cache, which
    #:     is what this whole split exists to prevent.
    #:   * ``_accumulate_mfd`` evaluates its weights in the surface's own dtype,
    #:     and at float32 a large ``p`` UNDERFLOWS: after the epsilon fill a
    #:     flat's slope is ~2.6e-4, so every weight is 0 by ``p ~ 11``, the cell
    #:     reads as a pit and its whole accumulation is dropped -- on precisely
    #:     the flat near-coast ground the rivers have to cross. See
    #:     ``flow.accumulate_d8``.
    #:
    #: WHAT IT COSTS, MEASURED, because it is not free and the cost is visible.
    #: MFD's fan is what made the drawn ribbon wide: near a trunk the
    #: neighbouring cells carry a share large enough to clear the cut too, so
    #: the mask came out several pixels across. A single-receiver forest has
    #: one-cell-wide branches by construction, so the plane becomes a
    #: CENTRELINE: on the corridor's (-12,-5) the drawn ribbon goes from a
    #: median 5.30 m to 1.88 m (one pixel), against a law width that says
    #: 3.30 m. The network is right and the ribbon is now NARROWER than the
    #: channel it describes, where before it was slightly wider.
    #:
    #: That is a real trade and it is deliberately NOT papered over here. The
    #: fix, if the owner wants it, is to let ``channel_width_m(Q)`` decide
    #: EXTENT the way ``water_depth_m(Q)`` already decides depth -- which is
    #: more consistent than what either version does, not less -- and it is a
    #: decision about what the world looks like rather than a defect. Nothing in
    #: this constant should be read as having settled it.
    #:
    #: False reproduces the bake_ver 10 water pass exactly.
    water_flow_single_receiver: bool = True
    #: Route the PYRAMID's discharge sweep single-receiver too (bake_ver 12).
    #:
    #: THE SAME CHANGE AS ``water_flow_single_receiver``, ONE TIER UP, and it
    #: exists because leaving the two disagreeing is what broke the river at a
    #: tile seam. ``build_flow_superblock`` accumulates ``q`` at 30 m with MFD
    #: while B6 accumulates it at 1.875 m with D8, so the boundary condition a
    #: fine tile receives is dispersed and the field it computes from that
    #: boundary is concentrated. ``acc`` -- the AREA field, which decides every
    #: height -- stays MFD at ``mfd_p`` at every level. This reaches ``q`` and
    #: nothing else, which is why it is a PRODUCT field.
    #:
    #: WHAT THE DEFECT ACTUALLY WAS, measured on the (-11,-5)/(-11,-6) seam
    #: before this existed, because the obvious reading is wrong. The total
    #: water crossing into the downstream tile's padded domain was NOT short:
    #: 8.63e7 m^3/yr under MFD against 8.51e7 under D8, a ratio of 0.99. What
    #: differed was how it was spread. MFD delivered it through 832 crossings
    #: whose largest single cell held 8.66e6; D8 delivers the same total with a
    #: largest cell of 2.02e7. At the seam row the coarse trunk under MFD is
    #: split 9.50e6 / 7.08e6 / 2.29e6 across three adjacent columns, while D8
    #: puts 2.11e7 in one -- against the 2.35e7 the upstream FINE tile carries
    #: 180 m away. A threshold consumer cannot use a fan: the downstream tile
    #: read 7.18e5 at the crossing column, 3.05% of what arrived, against a
    #: 1.53e6 cut. That is the 6.66 km the longest reach stopped short by.
    #:
    #: WHY NOT A WIDER APRON OR A DIFFERENT ENTRY MODE. Neither addresses it.
    #: The apron already carries the water 960 m before the interior begins and
    #: the loss is not attenuation, it is division; ``ENTRY_FOOTPRINT`` vs
    #: ``ENTRY_CROSSING`` moves WHERE one crossing lands and both pick exactly
    #: one cell per crossing, so neither changes how much any single cell holds.
    #: And the entry mode is shared with the AREA currency, so touching it would
    #: move the ground.
    #:
    #: False reproduces the bake_ver 11 pyramid exactly.
    water_pyramid_single_receiver: bool = True
    #: HOW FAR THE DRAWN WATER REACHES SIDEWAYS (bake_ver 13). One of
    #: ``EXTENT_MODES``; the long note below walks all three in the order they
    #: shipped, because each one is best read as the answer to what the previous
    #: one got wrong.
    #:
    #: --- ``"law"``, bake_ver 12, RETIRED. Let ``channel_width_m(Q)`` decide
    #: the drawn EXTENT the way ``water_depth_m(Q)`` already decides depth.
    #:
    #: WHY IT WAS NEEDED. ``water_flow_single_receiver`` made the plane a
    #: CENTRELINE: a single-receiver forest has one-cell-wide branches by
    #: construction, so 99.21% of wet pixels were a single 1.875 m pixel against
    #: a law width of 3.53 m at p50. Under MFD the fan happened to widen the
    #: ribbon and the agreement with the width law was an accident of
    #: dispersion, not a property anything maintained. You cannot get width from
    #: the mask alone; this reads the law directly, which is more consistent
    #: than either previous version rather than less.
    #:
    #: WHAT BOUNDS IT, and both bounds are measured rather than assumed:
    #:
    #:   * the LAW. ``channel_width_m(Q)`` at the corridor's discharges is
    #:     2.8-8.8 m, i.e. a half-width of 0.78-2.23 fine pixels, so the drawn
    #:     ribbon is 1-5 px. There is no stamped band: the radius is a function
    #:     of Q and changes continuously down a reach.
    #:   * the TERRAIN. A widened cell is drawn only where the shipped ground
    #:     stands below the reach's own water surface. That is not decoration:
    #:     the far-field experiment measured 58.2% of a widened edge below drawn
    #:     ground at 20 km when width was added in the ground plane without one.
    #:
    #: WHICH ONE BINDS, measured on the bv11 corridor before this was built:
    #: the terrain allows p50 11-28 m and p90 47-163 m of lateral extent at the
    #: drawn water level -- 90-93% of wet cells have room for the whole law
    #: width. So the LAW does the shaping and the terrain clamp bites on the
    #: 7-10% where it must. That is the right division of labour and it is the
    #: opposite of what a bank-driven rule would give.
    #:
    #: That reading survived one flight. The measurement it rests on is sound
    #: and is quoted again below; what it got wrong was which side of the
    #: division of labour the interesting cases live on.
    #:
    #: --- THE THREE MODES.
    #:
    #:   ``"centreline"``    the plane as ``graded_water_surface`` leaves it,
    #:                       one cell per reach. bake_ver 11.
    #:   ``"law"``           a ribbon ``channel_width_m(Q)`` across, clamped to
    #:                       ground below the reach's own surface. bake_ver 12.
    #:   ``"lateral_fill"``  fill to the local water surface: a cell is wet when
    #:                       it stands below the surface of the water it drains
    #:                       into. bake_ver 13, and the shipped rule. See
    #:                       ``water.fill_to_local_surface``.
    #:
    #: WHY THE LAW HAD TO GO, in the owner's own observation: the river "cuts a
    #: straight path across a valley floor ... to the left and right of the
    #: water channel, there is empty air, not a river bank". Both previous modes
    #: decide extent from DISCHARGE -- one cell, or ``channel_width_m(Q)`` of
    #: them -- and neither can know that the ground beside a reach is already
    #: below that reach's own waterline. On the measured corridor it is, by a
    #: wide margin: the terrain allows p50 11-28 m and p90 47-163 m of lateral
    #: extent at the drawn level while the law draws 3.5 m at p50, which is the
    #: 99.21%-of-wet-pixels-are-one-pixel symptom seen from the other side. A
    #: formula cannot tell a floodplain from a gorge at equal Q. The ground can.
    #:
    #: THE VERSION THAT WAS TRIED AND MEASURED WRONG, recorded here because it
    #: is the reading the words invite and it is a continent-flooder: carrying
    #: each reach's surface outward to the NEAREST channel and stopping where
    #: the ground rises above it takes the corridor from 317,665 wet cells to
    #: 66,546,420 -- 27-40% of a tile, median added depth 7.8 m, median cell
    #: 550-630 m from any channel. Nothing is wrong with the flood; the level is
    #: simply never re-anchored as it descends a hillside, because the nearest
    #: channel does not change. ``water.fill_to_local_surface`` anchors on the
    #: FLOW PATH instead and the same corridor comes out at 7.15x.
    water_extent_mode: str = "lateral_fill"
    #: Write the water plane at all. False reproduces a bake_ver-8 tile's
    #: sections exactly (no SECTION_WATER_*, flag clear), which is what the
    #: terrain-identity gate bakes against.
    water_plane_enabled: bool = True

    #: Make the drawn water FACE-CONNECTED down a slope (bake_ver 14). See
    #: ``water.bridge_to_face_contact`` for the rule and for the measurement
    #: that forced it; the short version is that the extent rules above decide
    #: WHICH CELLS are wet and nothing before this decided whether the water in
    #: two adjacent cells actually TOUCHES, which is a different question and
    #: the one the owner was looking at.
    #:
    #: Runs AFTER whichever ``water_extent_mode`` ran, so the three extent rules
    #: keep meaning exactly what they meant. False reproduces bake_ver 13's
    #: plane byte for byte, which is the property a constant claiming to
    #: reproduce a previous bake has to have.
    #:
    #: NOT A MODE OF ``water_extent_mode``. Extent is "which cells are wet";
    #: this is mostly "how thick is the water in a cell that already is", and
    #: folding it into the mode enum would make the two impossible to A/B
    #: against each other -- which is how the corridor numbers in
    #: ``bridge_to_face_contact``'s docstring were taken.
    water_face_contact_bridge: bool = True

    #: Inject the DISCHARGE currency where the parent's flow crosses into the
    #: SHIPPED INTERIOR, instead of where it crosses into the padded domain.
    #: The fix for HYDROLOGY_RESIDUALS #7 -- see `_edge_entries`' own docstring
    #: for the geometry and the bounded double count it accepts.
    #:
    #: WATER ONLY, BY CONSTRUCTION. The AREA currency keeps the padded-rim
    #: crossings, so `acc` is unchanged, so `A^m` is unchanged, so the incision
    #: and every shipped elevation byte are unchanged. This constant may
    #: therefore roll `bake_ver` and must never roll `TERRAIN_VERSION`;
    #: tools/verify_water_only_change.py is what proves it did not.
    #:
    #: IT DELIBERATELY BREAKS THE "ONE SET OF CROSSINGS, TWO CURRENCIES"
    #: INVARIANT at the B2c call site, and that is the trade to watch. With this
    #: on, a cell can carry a discharge whose matching upstream AREA never
    #: arrived -- the water says "big river" where the ground was incised for a
    #: small one. That is the honest state of affairs (#7 loses area too) but it
    #: means channel geometry and channel water are no longer derived from the
    #: same boundary condition, which is exactly the class of drift
    #: `_edge_entries` was split out to prevent. Measure both.
    water_inject_at_interior_rim: bool = True

    #: F3: put SLOPE in the depth law. Off by default -- flipping it rolls
    #: ``bake_ver`` and invalidates every baked water plane, exactly as
    #: ``water_inject_at_interior_rim`` does, so the two should be decided
    #: together and rolled once rather than twice.
    #:
    #: WHAT IS WRONG WITHOUT IT. ``water_depth_m`` is Leopold & Maddock
    #: hydraulic geometry: depth depends on discharge and NOTHING ELSE. That is
    #: a fit to lowland rivers at roughly constant slope, and this world's long
    #: profile runs 173 -> 29 m/km on the wet block alone. Normal-depth flow
    #: says depth goes as ``(Q / sqrt(S)) ** (3/5)``, so a law with no S in it
    #: puts too much water on steep upper reaches -- where it then cannot stay
    #: connected -- and too little on flat lower ones.
    #:
    #: ONLY THE SLOPE HALF IS TAKEN. The Q exponent stays at
    #: ``CHANNEL_DEPTH_EXP``: architecture §4 records observed depth matching
    #: the Q law to three significant figures across three decades of
    #: discharge, and swapping 0.3516 for normal-depth's 0.6 would break an
    #: agreement that is currently exact. The slope enters as a ratio against
    #: ``SLOPE_REF_M_PER_M``, so at the reference gradient the law is
    #: BIT-IDENTICAL and every existing measurement survives.
    #:
    #: THE ACCEPTANCE TEST IS ALREADY WRITTEN, and it is falsifiable:
    #: ``bridge_to_face_contact`` is a hand-built correction for this missing
    #: term, so with slope in the law the bridge should become close to a no-op
    #: on steep reaches. If it is still doing heavy lifting, the depth model is
    #: still wrong and this goes back off rather than getting tuned.
    water_slope_in_depth: bool = True

    #: The same missing physics as ``water_slope_in_depth``, in the EXTENT rule
    #: instead of the depth law. Off by default; separate from the depth flag so
    #: the two can be measured apart, since they fix different halves of one
    #: complaint.
    #:
    #: ``fill_to_local_surface`` spreads water sideways to the local surface with
    #: no reference to gradient, so the same discharge makes the same sheet on a
    #: floodplain and on a mountainside. The owner, flying it: water "not being
    #: placed on a downslope where gravity would actually guide and push the
    #: water on a path of least resistance", descending as "a manmade magenta
    #: staircase, not a natural water flow falling down a gulley". That is water
    #: failing to CONCENTRATE -- fast water on a steep bed occupies less
    #: cross-section for the same Q.
    #:
    #: It scales the fill's DEPTH THRESHOLD by gradient rather than capping its
    #: reach. A radius cap is explicitly retired in that function's own
    #: docstring ("a fill that needs a radius to stop is a formula wearing a
    #: flood's clothes") after a measured 209x flood, and scaling the threshold
    #: keeps connectedness, levelness and downstream descent intact because it
    #: changes which cells clear the bar, never where a level came from.
    water_slope_in_extent: bool = True

    #: Forbid a cell's water from standing higher than adjacent water on HIGHER
    #: ground. Off by default until the owner has seen what it does to the long
    #: profile: it lowers the surface on ~75% of wet cells by a median 634 mm --
    #: a large intervention, even though every millimetre of it removes a state
    #: that cannot physically exist.
    #:
    #: Measured on (-4,-4) at bake_ver 15: 13.59% of downstream steps along
    #: traced reaches RISE, p90 627 mm against a 100 mm wire LSB, and 100% of
    #: them between TOUCHING pixels, so tracing explains none of it. See
    #: water.enforce_neighbour_consistency for why enforce_descent does not
    #: already cover this, and docs/measurements/uphill-water-2026-08-07.txt for
    #: the rule that was tried first and rejected for flattening the profile.
    water_level_neighbour_consistency: bool = True

    #: Let water spread sideways onto ground below it, after the flow-path fill
    #: has run. OFF until measured: the module's own history has a lateral rule
    #: that multiplied the wet set by 209x, so this ships dark and is turned on
    #: by a bake that has the before/after in hand.
    water_settle_to_level: bool = True
    #: Lateral reach of that settling, in cells. 8 = 15 m, which is a riverbed;
    #: see settle_to_adjacent_level for why the bound is the safety argument.
    water_settle_max_iter: int = 8
    #: Hold each channel cell's settled spread to the volume its OWN discharge
    #: supports -- width(Q) x depth(Q) x cell length. The budget therefore grows
    #: down the river as the watershed does, which is the only bound here that
    #: is conserved rather than chosen. See water.apply_discharge_budget.
    water_settle_discharge_budget: bool = True
    #: Relax the settled water surface with the drawn channel held fixed, so the
    #: floodplain is a smooth interpolation of the river rather than a mosaic of
    #: inherited levels. See water.smooth_level_field.
    water_level_smooth_iters: int = 12
    #: No drawn water may stand above its own upstream water. Checked LAST,
    #: after every stage that can break it. See enforce_upstream_monotone.
    water_enforce_upstream_monotone: bool = True

    #: The hydraulic-geometry exponents, Q8, mirroring channel.h's
    #: ``kChannelWidthExpQ8`` / ``kChannelDepthExpQ8``.
    #:
    #: THEY ARE HERE BECAUSE THEY DECIDE BAKED BYTES AND WERE NOT COVERED.
    #: ``water.py`` held them as module constants outside every payload, so
    #: editing them changed the written water plane under an UNCHANGED
    #: ``fine_provider_id`` -- the namespace would hold two mutually
    #: incompatible waters and no consumer could tell them apart. That is
    #: precisely the failure ``providers/diffusion.py``'s
    #: ``_tile_format_fingerprint`` docstring calls "especially nasty" about a
    #: codec bump, and ``_bake_fingerprint``'s own note says the product half is
    #: folded in "so a water constant that fed no identity would change written
    #: bytes under an unchanged id". These fed no identity. Now they do.
    #:
    #: Recorded, not read: the laws still take their exponents from
    #: ``water.py``'s module constants. These fields exist to make the identity
    #: honest about a value that decides bytes. If the two ever disagree the
    #: bake is lying about what it produced, which
    #: ``test_channel_exponents_match_the_mirror`` refuses.
    channel_width_exp_q8: int = 102
    channel_depth_exp_q8: int = 90

    #: The extent rules ``water_extent_mode`` may name, in the order they
    #: shipped. A ClassVar so it is not itself a constant, and a tuple so a
    #: typo in a config is a refusal at construction rather than a bake that
    #: silently drew a centreline.
    EXTENT_MODES: ClassVar[tuple[str, ...]] = ("centreline", "law", "lateral_fill")

    def __post_init__(self) -> None:
        if self.water_extent_mode not in self.EXTENT_MODES:
            raise ValueError(
                f"water_extent_mode={self.water_extent_mode!r} is not one of "
                f"{list(self.EXTENT_MODES)}"
            )
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
        if self.province_strength < 0.0:
            raise ValueError(
                f"province_strength must be >= 0 (0 disables), got "
                f"{self.province_strength}"
            )
        if self.province_smooth_m < 0.0:
            raise ValueError(
                f"province_smooth_m must be >= 0, got {self.province_smooth_m}")
        for lo, hi, nm in (
            (self.province_relief_lo, self.province_relief_hi, "relief"),
            (self.province_cold_c, self.province_temperate_c, "cold/temperate"),
            (self.province_arid_mm, self.province_humid_mm, "arid/humid"),
            (self.province_lowland_elev_lo_m, self.province_lowland_elev_hi_m,
             "lowland_elev"),
        ):
            if not lo < hi:
                raise ValueError(
                    f"province {nm} thresholds must satisfy lo < hi, got "
                    f"{lo}, {hi}"
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
            "basin_min_depth_m": self.basin_min_depth_m,
            "basin_min_area_m2": self.basin_min_area_m2,
            "basin_exclude_spanning": self.basin_exclude_spanning,
            "basin_require_above_sea": self.basin_require_above_sea,
            "basin_pet_a": self.basin_pet_a,
            "basin_pet_b": self.basin_pet_b,
            "basin_pet_c": self.basin_pet_c,
            "basin_pet_floor_mm": self.basin_pet_floor_mm,
            "basin_budyko_n": self.basin_budyko_n,
            "basin_min_lake_depth_m": self.basin_min_lake_depth_m,
            "basin_salt_aridity": self.basin_salt_aridity,
            "basin_seasonal_cv_pct": self.basin_seasonal_cv_pct,
            "province_strength": self.province_strength,
            "province_smooth_m": self.province_smooth_m,
            "province_relief_lo": self.province_relief_lo,
            "province_relief_hi": self.province_relief_hi,
            "province_cold_c": self.province_cold_c,
            "province_temperate_c": self.province_temperate_c,
            "province_arid_mm": self.province_arid_mm,
            "province_humid_mm": self.province_humid_mm,
            "province_lowland_elev_lo_m": self.province_lowland_elev_lo_m,
            "province_lowland_elev_hi_m": self.province_lowland_elev_hi_m,
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

    #: Fields that decide PRODUCT bytes but can never move a height. The
    #: complement of ``as_payload`` over the dataclass, and the two are
    #: asserted to partition the field set exactly
    #: (``test_constants_partition_is_exhaustive``) -- a field in NEITHER would
    #: change baked bytes while rolling no identity at all, which is precisely
    #: the drift a whitelist invites and the reason that test exists.
    #: ClassVar, NOT a bare annotation: on a dataclass an annotated class
    #: attribute becomes a FIELD, and this one would then appear in its own
    #: partition test as an unclassified constant.
    PRODUCT_FIELDS: ClassVar[tuple[str, ...]] = (
        "water_q_perennial_m3_yr",
        "water_min_width_px",
        "water_flow_single_receiver",
        "water_pyramid_single_receiver",
        "water_extent_mode",
        "water_plane_enabled",
        "water_face_contact_bridge",
        # PRODUCT, not payload: it moves the discharge currency only. The area
        # currency, and therefore every height, is untouched -- which is the
        # whole point of the constant and is what verify_water_only_change.py
        # checks.
        "water_inject_at_interior_rim",
        # PRODUCT, not payload: it changes the water plane's stored depths and
        # nothing else. No elevation byte moves, which is what
        # verify_water_only_change.py exists to prove.
        "water_slope_in_depth",
        "water_slope_in_extent",
        "water_level_neighbour_consistency",
        # PRODUCT: they decide the water plane's stored widths and depths and
        # nothing about the ground.
        "water_settle_to_level",
        "water_settle_max_iter",
        "water_settle_discharge_budget",
        "water_level_smooth_iters",
        "water_enforce_upstream_monotone",
        "channel_width_exp_q8",
        "channel_depth_exp_q8",
    )

    def product_payload(self) -> dict:
        """The product half of the identity. See ``PRODUCT_FIELDS``."""
        return {name: getattr(self, name) for name in self.PRODUCT_FIELDS}


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
    "B5.reopen_basins",
)

#: Stages that produce PRODUCTS rather than ground. Hashed into
#: ``product_identity_payload``, never into ``bake_identity_payload`` -- B6
#: reads the surface and writes a separate plane, and putting it in
#: ``STAGE_ORDER`` would roll the terrain identity of every world for a stage
#: that cannot move a height by construction.
#:
#: ``bake_tile`` reports timings for both lists in one ``cpu_seconds`` dict, so
#: the split is an identity boundary and not a reporting one.
PRODUCT_STAGE_ORDER: tuple[str, ...] = (
    "B6.discharge_water",
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
        # NAME KEPT at "bake_version" though it now carries TERRAIN_VERSION:
        # this key is hashed into every shipped fine_provider_id, and renaming
        # it would roll the terrain namespace of every existing world for a
        # cosmetic reason -- the precise thing the split exists to avoid.
        "bake_version": TERRAIN_VERSION,
        "stage_order": list(STAGE_ORDER),
        "geometry": geom.as_payload(),
        "constants": consts.as_payload(),
        # The per-province multiplier table is a tuning surface that decides
        # baked bytes but does not live on BakeConstants (it is a table, not a
        # scalar). Hashed here so retuning it rolls fine_provider_id like every
        # other bake constant, instead of silently reusing tiles baked under
        # different physics.
        "provinces": _province.identity_payload(),
    }


def bake_fingerprint(
    geom: BakeGeometry = PRODUCTION, consts: BakeConstants = CONSTANTS
) -> str:
    return hashlib.sha256(
        json.dumps(bake_identity_payload(geom, consts), sort_keys=True).encode("utf-8")
    ).hexdigest()


def product_identity_payload(consts: BakeConstants = CONSTANTS) -> dict:
    """Everything that decides which PRODUCTS a tile carries and what is in them.

    The other half of ``bake_identity_payload``. Folded into
    ``fine_provider_id`` alongside it, so a tile whose water plane differs never
    shares an id with one whose does not -- content addressing meaning what it
    says. What it deliberately does NOT do is reach ``roughness_seed``: bumping
    a product constant re-bakes tiles onto identical ground rather than
    creating a world.

    Note what is absent, because it is the reason this is affordable: water
    APPEARANCE -- translucency, colour, the depth cue, the foam channel -- is
    client-side material work and touches no baked byte, so the W6/W7-style
    retuning the owner does most often rolls nothing here.
    """
    return {
        "bake_version": BAKE_VERSION,
        "stage_order": list(PRODUCT_STAGE_ORDER),
        "constants": consts.product_payload(),
        # The section ids and flag bit the bake writes. A layout change with no
        # constant change would otherwise be invisible to the id -- which is
        # exactly the "namespace holding two mutually incompatible formats"
        # failure `providers/diffusion.py::_tile_format_fingerprint` records.
        "sections": {
            "water_index": _codec_const("SECTION_WATER_INDEX"),
            "water_data": _codec_const("SECTION_WATER_DATA"),
            "water_flag": _codec_const("FLAG_WATER_PRESENT"),
            "water_dry_sentinel": _codec_const("WATER_DRY_DEPTH"),
            "water_depth_lsb_mm": _codec_const("WATER_DEPTH_LSB_MM"),
        },
    }


def _codec_const(name: str) -> int:
    """Read one wire constant from ``tile_codec`` without importing it eagerly.

    ``tile_codec`` imports the bake lazily (for ``BAKE_VERSION``), so importing
    it at this module's top level would close a cycle.
    """
    from .. import tile_codec

    return int(getattr(tile_codec, name))


def product_fingerprint(consts: BakeConstants = CONSTANTS) -> str:
    return hashlib.sha256(
        json.dumps(product_identity_payload(consts), sort_keys=True).encode("utf-8")
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
    stage_tmp = 2 * grid32  # z_pre, d8s4 (B4) / kfac32, dist (B2d)
    wide = 4 * grid64       # acc64 + three int64 index grids
    # NOTE, and it is why this no longer matches the 6.33 GiB in the docstring:
    # item (1) above -- gy, gx, slope and delta leaking from B1 to the end of
    # the bake, 4 grids and 1.36 GB -- was FIXED in 8ed70a8, which freed all
    # four at their real last use. The count was written against the code
    # before that landed. The measured 6.90 GiB it was validated against is
    # likewise a pre-fix figure.
    #
    # The dtype work merged alongside it also narrowed `rec` to int32 and
    # removed the duplicate argsort and two of B4's index grids, so `wide = 4`
    # is now an over-count too. Both directions are conservative (the estimate
    # is HIGH), which is the safe way for a pod-sizing number to be wrong --
    # but it should be re-measured against the merged code rather than trusted.
    # Until then, 8 GiB per worker remains the right sizing.
    return held + stage_tmp + wide


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
    bake_version: int = TERRAIN_VERSION,
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
    #: ``flow.accumulate_d8``, the water pass's single-receiver sweep. Same
    #: None-default reason as ``profile_incision``; B6 refuses to bake with
    #: ``water_flow_single_receiver`` set and no kernel rather than silently
    #: falling back to MFD and shipping a plane that is not the one the
    #: constants describe.
    accumulate_d8: "Callable | None" = None


_MISSING_KERNELS = (
    "terrain_service.bake.{mod} is not importable ({err}). The bake numerics "
    "live in flow.py / noise.py / incise.py / thermal.py and need the "
    "terrain-diffusion venv (numba + scipy); pipeline.py itself deliberately "
    "imports none of it so the bake identity can be hashed on a bare box."
)


def load_kernels() -> BakeKernels:
    """Import the real numerics. Raises RuntimeError with a legible message."""
    try:
        from .flow import (
            accumulate_d8,
            accumulate_mfd,
            d8_receivers,
            fill_depressions,
        )
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
        accumulate_d8=accumulate_d8,
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

#: ``climate_fetch(x, y)`` -> that coarse tile's ``(4, coarse_tile_px,
#: coarse_tile_px)`` uint8 climate planes in ``province.CLIMATE_ORDER``, or
#: None.
#:
#: A SEPARATE callable rather than a widened ``CoarseFetch``, deliberately.
#: ``CoarseFetch``'s return value is hashed byte-for-byte by
#: ``superblock_inputs_fingerprint`` and fed to ``build_flow_superblock``; the
#: hydrology pyramid has no business knowing climate exists, and widening the
#: type would have put a climate plane inside every flow fingerprint and forced
#: every existing test double and ``tools/bake_real_tile.py``'s v1-tile path to
#: change. Climate is an input to the PROVINCE partition and to nothing else.
ClimateFetch = Callable[[int, int], "np.ndarray | None"]


def _ring_windows(tile_x: int, tile_y: int, geom: "BakeGeometry"):
    """Yield ``(dx, dy, win)`` for all nine ring tiles, in the original order.

    ``win`` is ``(dst_y, dst_x, src_y, src_x)`` slices, or None when this ring
    tile contributes nothing to the padded domain (only possible at
    ``apron_coarse_px == 0``). All nine are yielded, present or not, because
    ``assemble_padded_coarse``'s ``missing`` list records a ring tile's absence
    whether or not its window is empty.

    Factored out so the elevation gather and the climate gather cannot drift
    apart -- they must place the same tile at the same offset, or a province
    boundary would sit one apron away from the terrain it was derived from.
    """
    n = geom.coarse_tile_px
    a = geom.apron_coarse_px
    span = geom.padded_coarse_px
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            dy0, dx0 = a + dy * n, a + dx * n
            sy0, sx0 = max(0, -dy0), max(0, -dx0)
            sy1, sx1 = min(n, span - dy0), min(n, span - dx0)
            if sy0 >= sy1 or sx0 >= sx1:
                yield dx, dy, None
                continue
            yield dx, dy, (slice(dy0 + sy0, dy0 + sy1),
                           slice(dx0 + sx0, dx0 + sx1),
                           slice(sy0, sy1), slice(sx0, sx1))


def assemble_padded_climate(
    climate_fetch: "ClimateFetch | None",
    tile_x: int,
    tile_y: int,
    geom: BakeGeometry = PRODUCTION,
) -> "np.ndarray | None":
    """Gather the 3x3 ring's uint8 climate planes into the padded domain.

    Returns ``(4, padded_coarse_px, padded_coarse_px)`` uint8, or None when
    there is no fetcher or no tile in the ring carries climate. 1.3 MB at
    production -- three orders of magnitude under any fine-grid array, which is
    the whole reason province works at the coarse pitch and indexes at ``//16``.

    A MISSING ring tile leaves its window at zero, which de-quantises to the
    BOTTOM of each physical range (-40 C, 0 mm/yr). That is deliberately the
    same conservative posture ``MISSING_ELEVATION_M`` takes: a never-generated
    neighbour reads as cold and dry rather than as a plausible climate, so it
    biases toward GLACIAL/ARID in an apron the interior never reads, and never
    invents a temperate province out of absent data.
    """
    if climate_fetch is None:
        return None
    ch = len(_province.CLIMATE_ORDER)
    n = geom.coarse_tile_px
    dom = np.zeros((ch, geom.padded_coarse_px, geom.padded_coarse_px), np.uint8)
    any_found = False
    for dx, dy, win in _ring_windows(tile_x, tile_y, geom):
        if win is None:
            continue
        src = climate_fetch(tile_x + dx, tile_y + dy)
        if src is None:
            continue
        src = np.asarray(src)
        if src.shape != (ch, n, n):
            raise ValueError(
                f"climate tile ({tile_x + dx},{tile_y + dy}) has shape "
                f"{src.shape}, expected {(ch, n, n)}"
            )
        if src.dtype != np.uint8:
            raise ValueError(
                f"climate tile ({tile_x + dx},{tile_y + dy}) is {src.dtype}, "
                "expected uint8 -- these are the wire planes, not physical units"
            )
        dys, dxs, sys_, sxs = win
        dom[:, dys, dxs] = src[:, sys_, sxs]
        any_found = True
    return dom if any_found else None


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
    dom = np.full((geom.padded_coarse_px, geom.padded_coarse_px), MISSING_ELEVATION_M, np.float32)
    missing: list[tuple[int, int]] = []
    # The destination window of each ring tile inside the padded domain,
    # clipped to the domain (the corner/edge tiles contribute only their apron
    # strip). Shared with the climate gather so the two cannot drift.
    for dx, dy, win in _ring_windows(tile_x, tile_y, geom):
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
        if win is None:
            continue
        dys, dxs, sys_, sxs = win
        dom[dys, dxs] = src[sys_, sxs]
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
   (245 km at the default level 1) is truncated.

   WIRED 2026-08-03, as an opt-in: ``build_model_superblock`` builds a
   ``FlowSuperblock`` from a raster the CALLER supplies -- in production the
   diffusion model's own coarse map at 7.68 km/px -- and ``pregen --mode bake
   --bake-model-parent`` passes it as ``parent=`` to the top tile-backed
   level. A 512^2 window spans 3,932 km and needs zero coarse tiles, against
   4,096 of them (~6 GB) to buy one more EXACT level worth 983 km.

   This module still knows nothing about a diffusion model: the factory takes
   an array, exactly as the rest of the pyramid takes a ``CoarseFetch``. The
   provider lives in ``pregen``, which owns both halves. See
   ``docs/parent-hook-scope.md``.

   What it does NOT fix, all three MEASURED on seed 20260719 (289 coarse
   tiles, level-1 block (-1,-1), 512^2 window, 2026-08-03):

   a. The model window has an open top of its OWN, three coarse cells shy of
      4,000 km instead of 246. Truncation is pushed out 16x per axis, not
      removed.
   b. **Most of what arrives is not drainage, it is sea floor.** The priority
      flood fills the ocean basin and MFD routes it like any other surface, so
      at 7.68 km/px with a 61.7% ocean window the biggest crossing carried
      183,882 km^2 into the block through a parent cell 2,065 m BELOW sea
      level, only 41.4% of it land. Of the 222,888 km^2 injected in total,
      **2.9% landed on cells above sea level**; 611 of 1.9 M land cells moved
      at all. It is not harmful -- water entering at -2,000 m flows further
      down, out to sea, and never reaches a shipped hillslope -- but the
      headline number is not a river, and anyone quoting it should say so.
      Gating injection on land is a real option and is deliberately NOT taken
      here: it would need its own experiment, and a coastal plain below the
      coarse cell's mean is exactly where it would go wrong.
   c. The terrestrial gain is real but LOCAL to the boundary. Every tile that
      changed sits on the top level's own edge, because that is where
      injection happens; the best land tile went from 470.1 to 1,075.5 km^2 of
      catchment (2.29x). Whether a continental river appears at all depends on
      whether one drains into that particular 246 km block -- for this world,
      none does.
   d. **The reach is not symmetric, and where it points is arbitrary.** The
      window is world-anchored on a multiple of its own size, so a top-level
      block can sit anywhere in it -- and on seed 20260719 it sits in the
      extreme +x/+y CORNER (window cells [-512, 0), block cells [-32, 0)).
      Upstream area arriving from beyond the block's east or south edge is
      still truncated exactly as before; only west and north got the 3,932 km.

      Anchoring is what makes the pyramid cacheable (``superblock_index``:
      two tiles sharing an edge must read the same block), but a window
      CENTRED on its own top-level block would also be a deterministic
      function of that block, and would give every block the full radius. The
      cost is that two adjacent blocks would then see different windows and
      could disagree about the flow crossing their shared edge -- which they
      already do, since each runs its own fill and accumulation. Worth
      measuring; deliberately not changed here on argument alone.

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

7. **A level only ever hands its child what CROSSES the child's boundary.**
   Found while measuring #2's fix, and it is the reason that fix delivered so
   little. ``inject_edge_inflow`` counts parent cells that are OUTSIDE the
   child and drain INTO it. Area added to the parent anywhere INSIDE the
   child's own footprint is therefore invisible to that child, which rebuilds
   its accumulation from its own coarse tiles and never learns of it.

   Normally harmless -- a large catchment crosses many boundaries on its way
   down, so it is picked up at each hop. It bites when the new area is
   injected CLOSE to where it is wanted, which is exactly what a top-level
   parent does: it deposits on the top level's rim, and everything within one
   child block of that rim shares a block with the deposit.

   MEASURED, seed 20260719, 2026-08-03, and it goes all the way down:

     * the model parent moved level-1 accumulation on 0.49% of cells and took
       the best land tile from 470.1 to 1,075.5 km^2;
     * **1 of the 16 level-0 blocks under it changed at all** -- block
       (-4,-3), inflow 21,399.1 -> 21,854.7 km^2 (+455.6, +2.1%) -- delivering
       to exactly ONE tile, (-16,-9), 2,238.4 -> 2,683.9 km^2, 35,249 cells;
     * of those 35,249 changed level-0 cells, **35,232 (100.0%) lie INSIDE
       that tile's own padded domain**. The bake re-derives its interior from
       the coarse elevation and imports only a boundary condition, so a change
       inside the domain is discarded. The inflow handed to ``bake_tile`` was
       identical to four decimals in both arms (384.4912 km^2, 214 entry
       cells) and the tile baked BYTE-IDENTICAL in elevation, accumulation and
       flow -- 0 of 67,108,864 cells.

   So the model parent, correctly wired, deterministic, conservative and
   cheap, changed the shipped terrain of this world by exactly nothing. Not
   because the effect was small: because it was still in transit inside the
   destination tile when the pyramid stopped carrying it.

   TWO THINGS TO REMEMBER. **Rank candidate tiles by the change in the inflow
   their L0 block hands the bake, never by the change in level-1 accumulation
   over the tile** -- the first A/B here picked its tile the second way, spent
   13 minutes a side, and measured a tile the water provably could not reach.
   And **a boundary-crossing pyramid delivers nothing within one child block
   of where you inject**; buying reach at the top is worth little until that
   is addressed.

   SEPARATE FROM CARRIED DISCHARGE (task #49), AND NOW THE BINDING ONE. #7
   bounds HOW MUCH catchment reaches a tile; #49 fixed HOW MUCH WATER a given
   catchment is worth. They compose, and #49 shipped without touching this --
   but the corridor #49 was opened on is STILL DRY, and the measurement says
   this entry is why.

   MEASURED on (-14,-5), 2026-08-04, bake_ver 10, by baking the tile in memory
   and reading B6's own fields:

     * 1.49e7 m^3/yr of carried discharge arrives across 300 entry cells;
     * somewhere in the PADDED domain a cell carries 1.32e7 of it, so it does
       converge into a real stream;
     * the INTERIOR tops out at 2.22e6, against a 3.15e6 drawable threshold.

   Discharge only increases downstream, so a 6x drop from the padded maximum to
   the interior maximum is not dispersion -- it is proof that the whole
   downstream path of that 1.32e7 stream stays in the APRON. The pyramid's
   water enters the padded domain 960 m out and leaves again without ever
   crossing the tile that ships.

   Confirmed by a single-term control on the same tile (`q` set to None): the
   padded maximum falls 1.324e7 -> 3.574e6 and the padded drawable count 142 ->
   24, while the INTERIOR maximum is 2.219e6 in BOTH arms, identical to four
   figures. The boundary condition contributes nothing to the shipped bytes.

   So the tile is short of drawing a river by a factor of 1.4, where the
   discharge proxy had it short by 46. The remaining 1.4 is this entry, plus #3
   (the deposit lands where the 30 m parent's D8 says the flow crosses, and the
   1.875 m surface disagrees). A better NUMBER carried across the same seam
   cannot fix it; the seam has to deliver into the domain rather than onto its
   edge. See ``CARRIED_DISCHARGE``.

8. **Runoff disagreement between levels.** NEW with carried discharge. Each
   level computes its own Budyko runoff over its OWN raster at its own pitch
   and smooths it over ``province_smooth_m``, so the Q crossing a level-0
   boundary was integrated at 30 m while a level-1 crossing was integrated at
   120 m, and the fine tile re-derives its interior runoff on its own padded
   climate. The same class as #3, and bounded by the same argument: climate has
   no fine structure -- which is exactly why ``runoff_field_mm_yr`` keeps it
   coarse and gathers with ``y // scale`` rather than materialising it fine.
   The physical smoothing RADIUS is held constant across levels (the half-width
   is derived from the level's own ``cell_m``), so the disagreement is
   quantisation, not a change of filter.

9. **A model-backed parent carries area but no discharge.**
   ``build_model_superblock`` takes an elevation raster and nothing else, so
   ``q`` is None on it and the top tile-backed level receives area at its edges
   with no water attached. Deliberate: the model window is 3,932 km of ground
   the climate planes have never been generated for, and fabricating a runoff
   for it would be the very proxy task #49 deleted, at 64x the footprint. The
   consequence is that Q is UNDERSTATED at the top level's rim rather than
   invented, and #7 already says almost nothing arrives there anyway. Fixing it
   means giving the model window a climate stage, which is a provider question,
   not a pyramid one.
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

#: Where ``inject_edge_inflow`` puts a parent cell's through-flow inside the
#: child, and the reason there is a choice at all.
#:
#: ``ENTRY_FOOTPRINT`` -- the lowest child cell ANYWHERE in the receiving parent
#: cell's footprint. What the pyramid has always done, and correct enough at
#: L1 -> L0, where the footprint is 4x4 (<=120 m of lateral error, and
#: HYDROLOGY_RESIDUALS #3 records that it heals within a few hundred metres).
#:
#: ``ENTRY_CROSSING`` -- the lowest child cell on the FACE the parent's flow
#: actually crosses, i.e. the one-cell-thick strip of the footprint against the
#: shared edge. Identical to ENTRY_FOOTPRINT whenever the footprint is that
#: strip (a 1-cell-deep footprint), so it changes nothing at ratio 1.
#:
#: The choice only starts to matter as the ratio grows. A model-backed parent
#: at 7,680 m feeding a 120 m child is 64x64, so ENTRY_FOOTPRINT can deposit up
#: to 10.7 km from the crossing IN ANY DIRECTION -- including deep into the
#: domain, in a valley the water never entered, on the wrong side of a divide.
#: That looks broken rather than approximate.
#:
#: NOT hypothetical. MEASURED on seed 20260719, level-1 block (-1,-1), a 512^2
#: model window: the two rules disagreed on 9 of 15 crossings, by up to 9.61 km
#: (median 1.80 km of the ones that moved), and three of the crossings that
#: moved had a 100% LAND catchment behind them. ENTRY_CROSSING cannot do it:
#: the water starts on the face it came through, so its error is confined to
#: ONE axis, ALONG that face. The injected TOTAL was identical either way --
#: both rules pick exactly one cell per crossing, so conservation is untouched.
ENTRY_FOOTPRINT = "footprint"
ENTRY_CROSSING = "crossing"
ENTRY_MODES = (ENTRY_FOOTPRINT, ENTRY_CROSSING)

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
    entry_mode: str = ENTRY_FOOTPRINT,
    climate_fetch: "ClimateFetch | None" = None,
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

    CLIMATE IS A CONDITIONAL SECOND PASS (task #49), on the ``entry_mode``
    precedent immediately below. Since the block began carrying a DISCHARGE, the
    climate planes decide its bytes as surely as the elevation does, and a
    fingerprint that ignored them would say two blocks were built from the same
    world when one had a river in it and the other did not. But hashing them
    unconditionally would have rewritten the fingerprint of every block ever
    cached, for an input none of them read. So: no ``climate_fetch``, digest
    unchanged, byte for byte, exactly as before.
    """
    h = hashlib.sha256()
    h.update(_FP_DOMAIN)
    h.update(
        json.dumps(
            {
                "bake_version": TERRAIN_VERSION,
                "level": int(level.level),
                "sx": int(sx),
                "sy": int(sy),
                "tiles_per_side": int(level.tiles_per_side),
                "downsample": int(level.downsample),
                "cell_m": float(level.cell_m),
                "coarse_tile_px": int(level.geom.coarse_tile_px),
                "mfd_p": float(level.consts.mfd_p),
                "flat_eps": level.consts.flat_eps,
                # CONDITIONAL KEY, deliberately. The entry mode changes WHERE a
                # parent's through-flow lands, so it belongs in the provenance
                # digest -- but emitting it unconditionally would have rewritten
                # the fingerprint of every superblock already cached, for a
                # setting none of them used. Absent means ENTRY_FOOTPRINT, which
                # is what every block built before 2026-08-03 did.
                **({} if entry_mode == ENTRY_FOOTPRINT else {"entry_mode": entry_mode}),
                # CONDITIONAL, on the same precedent and for the same reason.
                # It decides the block's ``q`` raster, so it belongs in the
                # provenance digest; absent means the MFD sweep, which is what
                # every block built before bake_ver 12 did. A cached block whose
                # digest lacks it is therefore correctly read as built from a
                # different rule rather than silently reused with a dispersed
                # discharge in it.
                **({"q_single_receiver": True}
                   if level.consts.water_pyramid_single_receiver else {}),
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
    if climate_fetch is not None:
        h.update(b"climate")
        for j in range(n):
            for i in range(n):
                planes = climate_fetch(tx0 + i, ty0 + j)
                if planes is None:
                    h.update(b"\x00")
                    continue
                h.update(b"\x01")
                h.update(np.ascontiguousarray(planes, dtype=np.uint8).tobytes())
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
    #: RUNOFF-WEIGHTED DISCHARGE, m^3/yr, over the same cells as ``acc`` and by
    #: the same MFD sweep -- task #49. ``None`` means this block was built
    #: without climate and CANNOT answer "how much water", only "how much
    #: ground". That is a third state, distinct from zero, and every consumer
    #: must branch on it rather than reading a zero as a dry catchment.
    #:
    #: WHY IT IS A SECOND RASTER AND NOT A REPLACEMENT. ``acc`` is what
    #: stream-power incision reads (``A^m`` -- an AREA law), so it decides the
    #: ground and can never be swapped for a discharge without rolling
    #: TERRAIN_VERSION. ``q`` is what the water plane reads. Two currencies,
    #: two consumers, one pyramid; the alternative -- reconstructing Q from
    #: ``acc`` at the destination -- is exactly the proxy this field exists to
    #: retire.
    q: "np.ndarray | None" = None

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

    @property
    def carries_discharge(self) -> bool:
        """True when this block can answer "how much WATER crosses here"."""
        return self.q is not None


_FLOW_MAGIC = b"VXFL"
#: 1 -> 2: added the 16-byte inputs fingerprint. Old blobs are REFUSED rather
#: than read with a zero fingerprint, because a zero fingerprint would read as
#: "provenance unknown" everywhere and defeat the check it was added for.
#:
#: 2 -> 3 (task #49): added the optional DISCHARGE raster. Refused for the same
#: reason: a v2 block decoded as v3 would come back with ``q = None``, which is
#: the correct reading, but it would then silently feed the old local-runoff
#: proxy into a bake that has been told it carries Q. The bump makes "this
#: block predates carried discharge" a rebuild rather than a quiet downgrade.
_FLOW_VERSION = 3
#: magic, version, seed, bake_ver, level, has_q, sx, sy, tiles_per_side, size,
#: cell_m, origin_x_m, origin_y_m, inputs_fingerprint, n_missing
#:
#: ``has_q`` occupies what was a zero pad byte, so the header LENGTH is
#: unchanged and only the version distinguishes the two.
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
    # DISCHARGE IS float64 ON THE WIRE, unlike the two float32 planes beside it,
    # and the reason is range rather than precision: Q at a continental mouth is
    # order 1e10 m^3/yr while a headwater cell contributes ~1e2, and the width
    # law reads Q^0.4 over that whole span. float32 holds it, but the MFD sweep
    # that produced it accumulates in float64 and narrowing here would make the
    # cached block disagree with a freshly built one -- which is the difference
    # between a cache and a second implementation.
    q = None
    if sb.q is not None:
        q = np.ascontiguousarray(sb.q, dtype="<f8")
        if q.shape != acc.shape:
            raise ValueError("flow superblock discharge must match the acc raster")
    head = _FLOW_HEADER.pack(
        _FLOW_MAGIC,
        _FLOW_VERSION,
        _u64(seed),
        TERRAIN_VERSION,
        sb.level,
        1 if q is not None else 0,
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
    parts = [head, missing.tobytes(), acc.tobytes(), filled.tobytes()]
    if q is not None:
        parts.append(q.tobytes())
    return b"".join(parts)


def decode_flow_superblock(data: bytes) -> tuple[FlowSuperblock, int]:
    """Inverse of ``encode_flow_superblock``. Returns (superblock, seed)."""
    (
        magic,
        version,
        seed,
        bake_ver,
        level,
        has_q,
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
    if bake_ver != TERRAIN_VERSION:
        # TERRAIN_VERSION, not BAKE_VERSION: a superblock is a ROUTING product
        # over the coarse ground, and it stays valid across a products-only
        # bump. Pinning it to BAKE_VERSION would throw away every cached
        # superblock on a bump that cannot change a single one of their bytes.
        # Cannot happen through the cache (provider_id covers both), but a
        # stale file handed in directly must not be silently mixed in.
        raise ValueError(
            f"flow superblock was built by terrain_ver {bake_ver}, this is "
            f"{TERRAIN_VERSION}"
        )
    off = _FLOW_HEADER.size
    missing = np.frombuffer(data, dtype="<i4", count=2 * n_missing, offset=off)
    off += 8 * n_missing
    n = size * size
    acc = np.frombuffer(data, dtype="<f4", count=n, offset=off).reshape(size, size)
    off += 4 * n
    filled = np.frombuffer(data, dtype="<f4", count=n, offset=off).reshape(size, size)
    off += 4 * n
    q = None
    if has_q:
        q = np.array(
            np.frombuffer(data, dtype="<f8", count=n, offset=off).reshape(size, size)
        )
        off += 8 * n
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
        q=q,
    )
    return sb, seed


def _mean_downsample(a: np.ndarray, factor: int) -> np.ndarray:
    if factor == 1:
        return a
    h, w = a.shape
    return a.reshape(h // factor, factor, w // factor, factor).mean(axis=(1, 3))


CARRIED_DISCHARGE = """\
Why the pyramid carries Q, and what it cost to find out it had to (task #49).

THE DEFECT. ``build_flow_superblock`` accumulated one quantity -- catchment AREA
in m^2 -- and ``bake_tile``'s B6 turned the area arriving at its boundary into a
discharge by multiplying it by the runoff of the cell it arrived AT. That is
right only if the catchment upstream has the climate of its own mouth, and a
river that leaves its climate zone is precisely the case where it does not.

MEASURED, seed 20260719, the (-14,-4) -> (-14,-7) corridor: a river rising in
wet mountains and crossing an arid coastal plain. Local runoff along it falls
103.6 -> 217.5 -> 12.8 -> 3.6 -> 0.5 mm/yr. At the mouth the proxy read
352.9 km^2 x 3.6 mm/yr = 1.27e6 m^3/yr against a 3.15e6 drawable threshold, so
THREE OF THE FIVE TILES BAKED 0.000% WET, INCLUDING THE MOUTH, with water only
on the two upstream ones. The stitched coarse world, which has full
connectivity and no proxy, reads 58.7e6 m^3/yr at the same place. Backwards for
a river, and every river that leaves its own climate zone was broken this way.

THE FIX, in one sentence: run the same MFD sweep a second time at every level
of the pyramid with each cell seeded by its runoff VOLUME instead of its area,
and inject the result at the same boundary crossings the area uses.

FOUR PROPERTIES THAT MAKE IT CHEAP AND SAFE.

1. **It reuses the crossings, it does not recompute them.** ``_edge_entries``
   returns the (entry cell, child target cell) pairs once and both
   ``inject_edge_inflow`` and ``inject_edge_discharge`` scatter through them.
   Q and area therefore arrive at the SAME cell by construction rather than by
   two implementations agreeing -- which is how the three currencies in
   ``water.py``'s docstring happened in the first place.

2. **It cannot move a height.** Stream-power incision reads ``A^m`` -- an area
   law -- so ``acc`` still decides the ground, unchanged, and ``q`` is a second
   raster that only the water plane reads. This is a BAKE_VERSION change, not a
   TERRAIN_VERSION one, and ``tests/test_bake_terrain_identity.py`` is the gate
   that says so.

3. **It is one extra sweep per superblock, not per tile.** A level's raster is
   2048^2 whatever the level, so the whole pyramid costs a handful of seconds
   against the ~300 CPU-s a single fine tile takes.

4. **"No climate" is a third state, not zero.** ``q is None`` says the block
   cannot answer the question. B6 falls back to the old proxy in that case and
   SAYS SO in ``water_q_inflow_carried``, because a silent zero would read as a
   dry continent and would be the same class of failure as the proxy itself.

WHAT IT DELIVERED, MEASURED ON THE CORRIDOR IT WAS OPENED ON (2026-08-04, seed
20260719, bake_ver 10 against the shipped bake_ver 9 tiles):

    tile      local runoff   Q carried in at the boundary    wet px 9 -> 10
    (-14,-4)     103.6 mm/yr   7.21e6 @  108 mm/yr implied   1,630 -> 1,906
    (-13,-5)     217.5         6.39e6 @  156                 4,005 -> 4,003
    (-14,-5)      12.8         1.49e7 @   97                     0 ->     0
    (-14,-6)       3.6         2.50e7 @   71                     0 ->     0
    (-14,-7)       0.5         3.32e7 @   34                     0 ->     0

The middle column is the whole change: the water crossing into (-14,-6) is now
worth 71 mm/yr of catchment-mean runoff instead of the 3.6 mm/yr of the arid
ground it lands on -- 20x more water, and 2.50e7 m^3/yr where the proxy read
1.27e6. Against the stitched coarse world, which has full connectivity and no
proxy, the pyramid's own 30 m discharge near the mouth is 2.37e7 against
5.87e7: 40% of the true figure, where the proxy was 2.2% of it.

IT CORRECTS IN BOTH DIRECTIONS. (-13,-5) LOST two wet pixels, because its local
runoff (217.5) is HIGHER than its catchment mean (156) and the proxy had been
over-stating the water arriving there. A change that only ever adds water is
not measuring anything.

AND THE CORRIDOR IS STILL DRY, WHICH IS THE HONEST HEADLINE. Two things were
learned, and neither is the proxy:

  a. **(-14,-7) is 96.2% OCEAN** (median elevation -115 m). Every one of its
     395 drawable-Q cells at 30 m is BELOW SEA LEVEL. It was never a river
     mouth; it is the sea the river runs into, and 0.000% wet is the right
     answer there. The corridor's actual coastline is in (-14,-6).

  b. **The imported water does not reach the tile's own trunk stream.**
     Diagnosed on (-14,-5) by baking it in memory and reading B6's fields:

         Q injected at the padded boundary   1.49e7 m^3/yr, 300 entry cells
         max Q anywhere in the PADDED domain 1.32e7   (i.e. it does converge)
         max Q in the INTERIOR                2.22e6
         drawable threshold                   3.15e6
         interior cells at or over it              0

     Discharge only increases downstream, so a 6x drop from the padded maximum
     to the interior maximum cannot be dispersion: it PROVES that the entire
     downstream path of that 1.32e7 stream stays in the apron. The pyramid's
     water enters 960 m outside the tile and leaves without crossing it.

     THE SINGLE-TERM CONTROL, same tile, same superblock, `q` set to None:

                                     proxy      carried Q
         max Q, PADDED domain        3.574e6     1.324e7    3.7x
         drawable cells, PADDED           24         142    5.9x
         max Q, INTERIOR             2.219e6     2.219e6    1.00x
         interior wet cells                0           0

     The interior maximum is IDENTICAL to four figures. The boundary
     condition, in either currency, contributes NOTHING to the tile that
     ships; the whole gain is inside the apron.

That is HYDROLOGY_RESIDUALS #7 and #3 together -- the pyramid deposits what
crosses the boundary AT the boundary, using the 30 m parent's D8 idea of where
it crosses, and the 1.875 m surface then routes it back out. It is a DIFFERENT
defect from the one this task fixed, it is the one now standing between this
corridor and its river, and it is not addressable by carrying a better number
across the same seam. Task #49's premise -- "the discharge is faked" -- was
correct and is now false. The premise that fixing it would wet the mouth was
not, and the two must not be conflated in whatever is scoped next.

TWO NEW RESIDUALS, recorded honestly: HYDROLOGY_RESIDUALS #8 (each level
smooths its runoff at its own pitch) and #9 (a model-backed parent carries area
but no discharge).
"""


def superblock_runoff_mm_yr(
    climate_fetch: "ClimateFetch | None",
    sx: int,
    sy: int,
    level: "FlowLevel",
) -> "np.ndarray | None":
    """Budyko runoff over one superblock's raster, mm/yr, or None without climate.

    Assembled exactly as ``build_flow_superblock`` assembles elevation -- tile by
    tile, mean-downsampled to the level's own pitch -- so the two rasters are
    registered cell for cell and no resampling step can put the water somewhere
    the ground is not.

    THE SMOOTH IS OVER THE ASSEMBLED BLOCK, not per tile, and that is the whole
    reason this is a function rather than four lines inside the build. The
    uint8 climate wire LSBs are 0.31 C and 47 mm/yr; smoothing each tile alone
    would leave a discontinuity at every tile join, and a discontinuity in
    runoff is a step in Q, which the width law turns into a visible step in a
    river. ``province_smooth_m`` is converted to the level's pitch so the
    PHYSICAL smoothing radius is the same at every level.

    A MISSING TILE CONTRIBUTES ZERO RUNOFF, deliberately, matching what
    ``MISSING_ELEVATION_M`` already does to the routing: an ungenerated region
    delivers no water. It is the same residual (#1) in a second currency, not a
    new one -- and ``missing_tiles`` on the block is what records it.

    Returns None when NO tile in the block has climate, which is the honest
    answer to "how much water" and is propagated as ``FlowSuperblock.q = None``.
    """
    if climate_fetch is None:
        return None
    n_tiles = level.tiles_per_side
    tile_px = level.geom.coarse_tile_px
    ds = level.downsample
    sub = tile_px // ds
    tx0, ty0 = sx * n_tiles, sy * n_tiles

    temp = np.zeros((level.size_px, level.size_px), np.float32)
    precip = np.zeros((level.size_px, level.size_px), np.float32)
    have = np.zeros((level.size_px, level.size_px), bool)
    any_climate = False
    for j in range(n_tiles):
        for i in range(n_tiles):
            planes = climate_fetch(tx0 + i, ty0 + j)
            if planes is None:
                continue
            phys = _province.dequantize_climate(planes)
            ys = slice(j * sub, (j + 1) * sub)
            xs = slice(i * sub, (i + 1) * sub)
            temp[ys, xs] = _mean_downsample(phys["temperature"], ds)
            precip[ys, xs] = _mean_downsample(phys["precipitation"], ds)
            have[ys, xs] = True
            any_climate = True
    if not any_climate:
        return None

    half = max(int(round(float(level.consts.province_smooth_m)
                         / float(level.cell_m) / 2.0)), 1)
    # NORMALISED smooth: a missing tile is a HOLE, not a plateau at 0 C and
    # 0 mm/yr. Smoothing the raw arrays would drag every neighbouring cell
    # within the 480 m influence radius toward that fabricated cold desert and
    # invent a dry rim around every ungenerated region. Dividing by the smoothed
    # coverage mask is the nearest-valid extension and costs one more box pass.
    cover = _province.box_smooth(have.astype(np.float32), half)
    safe = np.maximum(cover, np.float32(1e-6))
    temp = _province.box_smooth(temp * have, half) / safe
    precip = _province.box_smooth(precip * have, half) / safe
    wb = basin_balance(level.consts)
    pet = _basins.pet_mm_yr(temp, wb)
    runoff = np.asarray(_basins.budyko_runoff_mm_yr(precip, pet, wb), np.float64)
    # A tile with no climate contributes no water at all -- the same statement
    # MISSING_ELEVATION_M already makes about the routing, in the second
    # currency. HYDROLOGY_RESIDUALS #1, not a new residual.
    runoff[~have] = 0.0
    np.clip(runoff, 0.0, None, out=runoff)
    return runoff


def build_flow_superblock(
    coarse_fetch: CoarseFetch,
    sx: int,
    sy: int,
    level: FlowLevel,
    kernels: BakeKernels,
    parent: "FlowSuperblock | None" = None,
    entry_mode: str = ENTRY_FOOTPRINT,
    climate_fetch: "ClimateFetch | None" = None,
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

    TWO CURRENCIES SINCE TASK #49. With ``climate_fetch`` the block also carries
    ``q``, a runoff-weighted DISCHARGE in m^3/yr from a second MFD sweep over
    the same filled surface and the same weights. Read ``CARRIED_DISCHARGE`` for
    why area alone could not deliver a river to the sea. Without it the block is
    exactly what it was and ``q`` is None -- which is a refusal to answer, not a
    dry world, and every consumer branches on it.

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
    entries = None
    inflow = None
    if parent is not None:
        # ONE geometry, computed ONCE, used by both currencies. Area and
        # discharge cross the boundary at the same cell because they are
        # scattered through the same (entry, target) pairs -- not because two
        # code paths were written to agree. See CARRIED_DISCHARGE property 1.
        entries = _edge_entries(
            child_z=filled,
            child_origin_m=origin_m,
            child_cell_m=level.cell_m,
            src=parent,
            d8_fn=kernels.d8_receivers,
            entry_mode=entry_mode,
        )
        inflow = _scatter_entries(entries, parent.acc, filled.shape)
    acc = np.asarray(
        kernels.accumulate_mfd(
            filled, level.cell_m, p=level.consts.mfd_p, inflow=inflow
        ),
        dtype=np.float32,
    )
    del inflow

    # -- THE SECOND SWEEP: DISCHARGE (task #49, CARRIED_DISCHARGE).
    q = None
    runoff = superblock_runoff_mm_yr(climate_fetch, sx, sy, level)
    if runoff is not None:
        # Per-cell runoff VOLUME, m^3/yr -- the seed accumulate_mfd(source=)
        # wants. Same construction as water.discharge_source at the fine tier,
        # at this level's own cell size. THE /1000 IS THE UNIT: runoff is
        # mm/yr and the cell is m^2, so without it every discharge in the
        # pyramid is 1000x too large (which is exactly what the climate-
        # boundary test read the first time it ran -- 1.46e6 mm/yr of implied
        # catchment runoff).
        q_src = (runoff / 1000.0) * (level.cell_m * level.cell_m)
        del runoff
        q_in = None
        if entries is not None and parent.q is not None:
            q_in = _scatter_entries(entries, parent.q, filled.shape)
        elif entries is not None:
            # A parent that carries area but not discharge -- today only the
            # model-backed top (build_model_superblock takes no climate). Its
            # area still lands, so incision still sees the catchment; its water
            # does not, and Q is UNDERSTATED at that boundary rather than
            # invented. Deliberate: fabricating runoff for ground the model has
            # never seen is the proxy this task exists to delete.
            pass
        # SINGLE-RECEIVER SINCE bake_ver 12, to match the fine tier's own water
        # pass. See ``BakeConstants.water_pyramid_single_receiver`` for the
        # measurement: MFD delivers the right TOTAL through the wrong number of
        # cells, and the consumer at the far end is a threshold. ``acc`` above
        # is untouched and stays MFD at ``mfd_p`` -- it is the area field and it
        # decides the ground.
        if level.consts.water_pyramid_single_receiver:
            if kernels.accumulate_d8 is None:
                raise RuntimeError(
                    "water_pyramid_single_receiver is set but this BakeKernels "
                    "has no accumulate_d8 kernel; inject flow.accumulate_d8 or "
                    "set the constant False (which reproduces the bake_ver 11 "
                    "pyramid)"
                )
            q = np.asarray(
                kernels.accumulate_d8(
                    filled, level.cell_m, source=q_src, inflow=q_in
                ),
                dtype=np.float64,
            )
        else:
            q = np.asarray(
                kernels.accumulate_mfd(
                    filled, level.cell_m, p=level.consts.mfd_p,
                    source=q_src, inflow=q_in,
                ),
                dtype=np.float64,
            )
        del q_src, q_in
    del entries

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
            entry_mode=entry_mode,
            climate_fetch=climate_fetch,
        ),
        q=q,
    )


#: ``FlowSuperblock.level`` for a MODEL-BACKED block. Deliberately far above
#: any level the tile-backed pyramid can reach (level 5 would already span
#: 62,914 km) so the two can never collide in the flow cache namespace, and so
#: a block read back from disk says which kind it is without a second field.
MODEL_FLOW_LEVEL = 200

#: ``FlowSuperblock.tiles_per_side`` for a model-backed block: it is not backed
#: by tiles at all. See ``build_model_superblock`` on why ``complete`` is
#: nonetheless True.
MODEL_TILES_PER_SIDE = 0

_MODEL_FP_DOMAIN = b"vxbake-flow-model-inputs:v1"


def model_superblock_fingerprint(
    elev_m: np.ndarray,
    origin_m: tuple[float, float],
    cell_m: float,
    consts: BakeConstants = CONSTANTS,
) -> bytes:
    """Provenance digest for a model-backed superblock.

    Same job as ``superblock_inputs_fingerprint`` and a DIFFERENT domain
    string, because the inputs are a different kind of thing: there are no
    tiles to enumerate present-or-absent, so the digest covers the raster
    itself plus the geometry and routing constants it was accumulated under.

    It matters for the same reason the tile-backed one does. The child hashes
    its parent's digest, so a top-level superblock built against a model window
    is distinguishable from the same block built without one, or against a
    different window -- which is precisely the "was this built in a smaller
    world?" question ``pregen`` asks before it reuses a cached block.
    """
    h = hashlib.sha256()
    h.update(_MODEL_FP_DOMAIN)
    h.update(
        json.dumps(
            {
                "bake_version": TERRAIN_VERSION,
                "cell_m": float(cell_m),
                "origin_x_m": float(origin_m[0]),
                "origin_y_m": float(origin_m[1]),
                "size_px": int(np.shape(elev_m)[0]),
                "mfd_p": float(consts.mfd_p),
                "flat_eps": consts.flat_eps,
            },
            sort_keys=True,
        ).encode("utf-8")
    )
    h.update(np.ascontiguousarray(elev_m, dtype=np.float32).tobytes())
    return h.digest()[:FLOW_FINGERPRINT_BYTES]


def build_model_superblock(
    elev_m: np.ndarray,
    origin_m: tuple[float, float],
    cell_m: float,
    kernels: BakeKernels,
    sx: int = 0,
    sy: int = 0,
    consts: BakeConstants = CONSTANTS,
) -> FlowSuperblock:
    """A ``FlowSuperblock`` over an elevation raster the CALLER supplies.

    This is the answer to HYDROLOGY_RESIDUALS #2. The tile-backed pyramid's top
    level gets no inflow at its own edges, so a river draining more than that
    level's span (246 km at the default) arrives with zero upstream area and
    carves nothing. Give the top level a parent and the truncation moves out to
    whatever the parent spans.

    WHY IT TAKES AN ARRAY. In production the raster is the diffusion model's
    own coarse map at 7.68 km/px, where a 512^2 window covers 3,932 km and
    needs ZERO coarse tiles -- against the 4,096 tiles (~6 GB) that buying one
    more EXACT pyramid level would require to exist before any fine tile in the
    region could publish. That asymmetry is the whole argument for doing it
    this way. But this module must not learn that a diffusion model exists: it
    receives a ``CoarseFetch`` callable and knows nothing about where coarse
    elevation comes from, and that is a property worth keeping. ``pregen``
    holds the provider AND calls this, so the seam costs no layering.

    ``inject_edge_inflow`` reads only ``cell_m``, ``origin_m``, ``acc`` and
    ``filled`` off a parent. Everything else on the dataclass is bookkeeping,
    which is what makes a synthetic parent constructible at all.

    COMPLETENESS, deliberately. ``missing_tiles`` is empty and ``complete`` is
    therefore True, because there are no tiles that could be missing -- the
    model is defined everywhere. That must NOT be read as "the child is
    complete": a child computes its own ``missing_tiles`` from its own coarse
    fetch and is unaffected by this, so ``pregen``'s publish gate still refuses
    a tile whose 30 m neighbourhood was never generated. Model backing fixes
    TRUNCATION; it says nothing about EXPLORATION ORDER, and conflating the two
    would silently reopen what the gate closed. See ORDER_DEPENDENCE.

    No downsample and no ``missing`` scan: unlike ``build_flow_superblock``
    there are no tiles to assemble. Everything after that is the same fill and
    the same MFD accumulation, at the same constants, so the child cannot tell
    which kind of parent it has.
    """
    z = np.ascontiguousarray(elev_m, dtype=np.float32)
    if z.ndim != 2 or z.shape[0] != z.shape[1]:
        raise ValueError(f"model superblock raster must be square 2-D, got {z.shape}")
    if not np.isfinite(z).all():
        # A non-finite cell would propagate through the fill into every
        # downstream accumulation and land in a shipped tile as a hole.
        raise ValueError("model superblock raster contains non-finite elevations")
    if cell_m <= 0:
        raise ValueError(f"model superblock cell_m must be positive, got {cell_m}")

    filled = np.asarray(kernels.fill_depressions(z), dtype=np.float32)
    acc = np.asarray(
        kernels.accumulate_mfd(filled, float(cell_m), p=consts.mfd_p),
        dtype=np.float32,
    )
    return FlowSuperblock(
        level=MODEL_FLOW_LEVEL,
        sx=int(sx),
        sy=int(sy),
        tiles_per_side=MODEL_TILES_PER_SIDE,
        cell_m=float(cell_m),
        origin_m=(float(origin_m[0]), float(origin_m[1])),
        acc=acc,
        filled=filled,
        missing_tiles=(),
        inputs_fingerprint=model_superblock_fingerprint(z, origin_m, cell_m, consts),
    )


def superblock_covers(parent: FlowSuperblock, child_origin_m, child_span_m) -> bool:
    """Does ``parent`` contain the child's whole extent?

    A parent that only half-covers its child is not an error -- it injects at
    the edges it does reach and the rest is the same truncation as no parent at
    all -- but it is never what the caller meant, and the failure is silent
    (fewer entry cells, smaller rivers, nothing logged). ``pregen`` checks this
    before wiring a model window so a mis-sized window is loud.
    """
    ox, oy = parent.origin_m
    n = parent.size_px * parent.cell_m
    cx, cy = child_origin_m
    return ox <= cx and oy <= cy and cx + child_span_m <= ox + n and cy + child_span_m <= oy + n


def _edge_entries(
    child_z: np.ndarray,
    child_origin_m: tuple[float, float],
    child_cell_m: float,
    src: FlowSuperblock,
    d8_fn: Callable,
    entry_mode: str = ENTRY_FOOTPRINT,
    target_slice: "slice | None" = None,
) -> tuple[np.ndarray, np.ndarray]:
    """WHERE the parent's through-flow crosses into the child. Geometry only.

    ``target_slice`` names the rectangle (same slice on both axes) the water
    must be delivered INTO, as indices of ``child_z``. None means the whole
    child array, which is the PADDED domain and is the historical behaviour.

    PASSING THE INTERIOR HERE IS THE FIX FOR HYDROLOGY_RESIDUALS #7. With the
    default, an entry cell is one outside the padded domain draining in -- so a
    stream can enter the padded domain, run its whole downstream path through
    the 960 m apron, and leave again without ever crossing the tile that ships.
    Measured on (-7,-5): 3.95e8 m^3/yr injected, 3.59e8 converging into one
    padded stream, and 1.30e6 in the interior. See
    docs/measurements/f6-pyramid-delivers-to-apron-2026-08-05.txt.

    THE DOUBLE COUNT THIS INTRODUCES, and why it is accepted rather than
    engineered around. With a target inside the child, an entry cell can lie in
    the APRON, and the parent's accumulation there already includes the apron's
    own local runoff -- which the child ALSO generates and routes inward. That
    water is counted twice. Bounded by the apron's local yield: 62.7 km^2 of
    apron at (-7,-5)'s 5.7 mm/yr is 3.6e5 m^3/yr against 3.95e8 injected, i.e.
    0.1%. Removing it means suppressing the child's own source in the apron,
    which would change the accumulation the incision reads and so could not stay
    water-only. Measured, small, and stated rather than hidden.

    Returns ``(src_flat, child_flat)`` -- for each boundary crossing, the flat
    index of the PARENT cell whose accumulation crosses, and the flat index of
    the CHILD cell it is deposited in. Both int64, same length, one entry per
    crossing.

    SPLIT OUT OF ``inject_edge_inflow`` FOR TASK #49, and the split is the point
    rather than tidying. Since the pyramid carries two currencies -- catchment
    area in m^2 and discharge in m^3/yr -- both must cross the boundary at
    exactly the same cell, or the width law reads a Q that belongs to a
    different channel than the area the incision cut. Computing the geometry
    once and scattering two rasters through it makes that true by construction.
    Two functions that each found their own crossings would be two
    implementations of one rule, which is precisely how ``water.py``'s "three
    incompatible currencies" came about.

    The construction, and why it is exactly conservative:

      * take the parent's SINGLE-receiver D8 field. Every flow path therefore
        crosses the child's boundary exactly once, at the unique parent cell
        that is outside the child and whose receiver is inside it;
      * that cell delivers its whole accumulated total. Nothing upstream of it
        is counted separately, because those cells' receivers are outside;
      * deposit in the LOWEST child cell of a CANDIDATE SET, i.e. the thalweg.
        Mass is preserved exactly and the water starts in the channel rather
        than smeared across 16 px of bank.

    ``entry_mode`` picks the candidate set -- the whole footprint, or just the
    face the flow crosses. See ``ENTRY_FOOTPRINT`` / ``ENTRY_CROSSING`` for
    which to use and why the answer depends on the parent-to-child ratio.
    Conservation is identical either way: both pick exactly one cell.

    See HYDROLOGY_RESIDUALS #3 and #4 for what this still gets wrong, and #7
    for the far larger thing it cannot address at all.
    """
    if entry_mode not in ENTRY_MODES:
        raise ValueError(f"entry_mode must be one of {ENTRY_MODES}, got {entry_mode!r}")
    child_z = np.asarray(child_z, dtype=np.float32)
    h, w = child_z.shape
    empty = (np.zeros(0, np.int64), np.zeros(0, np.int64))

    scell = float(src.cell_m)
    sox, soy = src.origin_m
    sh, sw = src.acc.shape
    ox, oy = child_origin_m
    # The TARGET rectangle in child indices. `ox`/`oy` stay the padded origin
    # below, because the deposit index still addresses the padded array; only
    # the "is this inside" test and the footprint clamp move.
    if target_slice is None:
        ti0, ti1, tj0, tj1 = 0, h, 0, w
    else:
        ti0, ti1 = int(target_slice.start), int(target_slice.stop)
        tj0, tj1 = ti0, ti1
    tx0 = ox + tj0 * child_cell_m
    ty0 = oy + ti0 * child_cell_m
    x1 = ox + tj1 * child_cell_m
    y1 = oy + ti1 * child_cell_m

    # Parent cells whose CENTRE lies inside the target extent.
    cx = sox + (np.arange(sw, dtype=np.float64) + 0.5) * scell
    cy = soy + (np.arange(sh, dtype=np.float64) + 0.5) * scell
    inside_x = (cx >= tx0) & (cx < x1)
    inside_y = (cy >= ty0) & (cy < y1)
    inside = inside_y[:, None] & inside_x[None, :]
    if not inside.any():
        return empty

    rec = np.asarray(d8_fn(src.filled, scell)[0]).reshape(-1)
    flat_inside = inside.reshape(-1)
    valid = rec >= 0
    # An entry cell: outside the child, draining to a cell inside it.
    entry = np.zeros(rec.shape, bool)
    entry[valid] = (~flat_inside[valid]) & flat_inside[rec[valid]]
    idx = np.flatnonzero(entry)
    if idx.size == 0:
        return empty

    tgt = rec[idx]
    src_out: list[int] = []
    child_out: list[int] = []
    # Perimeter-sized loop (a few thousand cells at most): the argmin over a
    # parent cell's child footprint does not vectorise cleanly and is not on
    # any hot path.
    for entry_cell, src_cell in zip(idx.tolist(), tgt.tolist()):
        si, sj = divmod(int(src_cell), sw)
        # Child index window covering this parent cell.
        j0 = int(np.floor((sox + sj * scell - ox) / child_cell_m))
        j1 = int(np.ceil((sox + (sj + 1) * scell - ox) / child_cell_m))
        i0 = int(np.floor((soy + si * scell - oy) / child_cell_m))
        i1 = int(np.ceil((soy + (si + 1) * scell - oy) / child_cell_m))
        # Clamp to the TARGET rectangle, not merely to the array: a parent cell
        # straddling the target's edge must deposit inside it, never in the
        # margin the target was chosen to exclude.
        i0, i1 = max(ti0, i0), min(ti1, i1)
        j0, j1 = max(tj0, j0), min(tj1, j1)
        if i0 >= i1 or j0 >= j1:
            continue
        window = child_z[i0:i1, j0:j1]
        if entry_mode == ENTRY_CROSSING:
            # Mask everything but the face(s) the parent's step crosses. The
            # step is one D8 move from the entry cell to this one, so its sign
            # names the face: moving east (dj=+1) enters through the WEST face,
            # i.e. the child column at j0. A diagonal step crosses a corner and
            # both faces are candidates.
            ei, ej = divmod(int(entry_cell), sw)
            di, dj = si - ei, sj - ej
            face = np.zeros(window.shape, bool)
            if di > 0:
                face[0, :] = True
            elif di < 0:
                face[-1, :] = True
            if dj > 0:
                face[:, 0] = True
            elif dj < 0:
                face[:, -1] = True
            if face.any():
                window = np.where(face, window, np.float32(np.inf))
        li, lj = np.unravel_index(int(np.argmin(window)), window.shape)
        # THE ENTRY CELL, not its receiver. The amount delivered is the
        # accumulation of the cell OUTSIDE the child that drains in -- the
        # receiver's own total already includes everything else inside the
        # child and would be counted a second time by the child's own sweep.
        # (Storing the receiver here inflated a 4-crossing fixture from 8,000
        # to 12,000 m^2 and the suite caught it immediately.)
        src_out.append(int(entry_cell))
        child_out.append((i0 + int(li)) * w + (j0 + int(lj)))
    return (
        np.asarray(src_out, np.int64),
        np.asarray(child_out, np.int64),
    )


def _scatter_entries(
    entries: tuple[np.ndarray, np.ndarray],
    src_field: np.ndarray,
    child_shape: tuple[int, int],
    dtype=np.float32,
) -> np.ndarray:
    """Deposit ``src_field`` at the crossings ``_edge_entries`` found.

    ``np.add.at`` rather than fancy-index assignment: two parent cells can
    legitimately choose the SAME child cell as their thalweg (the footprint's
    argmin is not injective), and plain assignment would drop one of them --
    silently losing a whole catchment's worth of water at a confluence.
    """
    src_flat, child_flat = entries
    out = np.zeros(child_shape, dtype)
    if src_flat.size:
        vals = np.asarray(src_field, np.float64).reshape(-1)[src_flat]
        np.add.at(out.reshape(-1), child_flat, vals.astype(dtype, copy=False))
    return out


def inject_edge_inflow(
    child_z: np.ndarray,
    child_origin_m: tuple[float, float],
    child_cell_m: float,
    src: FlowSuperblock,
    d8_fn: Callable,
    entry_mode: str = ENTRY_FOOTPRINT,
) -> np.ndarray:
    """Upstream AREA entering the child domain, as an m^2 field on the child.

    The geometry is ``_edge_entries``' -- read that for the construction and for
    why it is exactly conservative. This is the area currency; the discharge
    currency is ``inject_edge_discharge``, over the identical crossings.

    The result is meant for ``accumulate_mfd(..., inflow=...)``, which treats
    it as extra drainage area (m^2) present at that cell before routing.
    """
    entries = _edge_entries(
        child_z, child_origin_m, child_cell_m, src, d8_fn, entry_mode
    )
    return _scatter_entries(entries, src.acc, np.shape(child_z))


def inject_edge_discharge(
    child_z: np.ndarray,
    child_origin_m: tuple[float, float],
    child_cell_m: float,
    src: FlowSuperblock,
    d8_fn: Callable,
    entry_mode: str = ENTRY_FOOTPRINT,
) -> "np.ndarray | None":
    """Upstream DISCHARGE entering the child domain, m^3/yr, or None.

    Task #49. The sibling of ``inject_edge_inflow`` in the second currency, at
    the SAME crossings and the same deposit cells -- see ``CARRIED_DISCHARGE``.

    Returns None when ``src`` carries no discharge (``src.q is None``), which is
    a refusal to answer and NOT a dry boundary. The caller must branch: reading
    a zero here as "no water arrives" would reproduce, in a new place, exactly
    the failure this function exists to remove.

    float64 out, not float32: Q spans ~1e2 at a headwater cell to ~1e10 at a
    continental mouth and the accumulation it feeds is float64 throughout.
    """
    if src.q is None:
        return None
    entries = _edge_entries(
        child_z, child_origin_m, child_cell_m, src, d8_fn, entry_mode
    )
    return _scatter_entries(entries, src.q, np.shape(child_z), dtype=np.float64)


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
    #: The B5 basin registry (bake_ver 8), ordered by (min_y, min_x) of extent
    #: with ids 0..n-1. `elevation_m` above already has these depressions
    #: RE-OPENED; the table is what tells a client where they are and how deep
    #: the water in them stands. Empty tuple means "surveyed, holds nothing",
    #: which is a different statement from a tile that predates the registry --
    #: see tile_codec.FLAG_BASINS_PRESENT.
    basins: tuple["_basins.BasinRecord", ...] = ()
    #: Runoff-weighted discharge, m^3/yr, interior (bake_ver 9, plan P2). None
    #: when the tile baked without climate -- there is no honest Q without a
    #: precipitation field, and inventing one would put rivers in a world whose
    #: climate nobody supplied.
    discharge_m3_yr: "np.ndarray | None" = None
    #: The graded water surface, metres absolute, NaN where dry, interior.
    #: None for the same reason as ``discharge_m3_yr``. This is what becomes
    #: SECTION_WATER_*; see ``bake.water`` for the laws and for the overshoot
    #: gate that decides whether it is trustworthy.
    water_surface_m: "np.ndarray | None" = None


def basin_filter(consts: BakeConstants = CONSTANTS) -> "_basins.BasinFilter":
    """The registry filter, from the hashed bake constants.

    One function rather than a constructor call at each site, because a
    filter built from anything other than ``consts`` would decide baked bytes
    without rolling the bake identity.
    """
    return _basins.BasinFilter(
        min_depth_m=consts.basin_min_depth_m,
        min_area_m2=consts.basin_min_area_m2,
        exclude_spanning=consts.basin_exclude_spanning,
        require_above_sea=consts.basin_require_above_sea,
    )


def basin_balance(consts: BakeConstants = CONSTANTS) -> "_basins.WaterBalance":
    """The lake rule's constants, from the hashed bake constants."""
    return _basins.WaterBalance(
        pet_a=consts.basin_pet_a,
        pet_b=consts.basin_pet_b,
        pet_c=consts.basin_pet_c,
        pet_floor_mm=consts.basin_pet_floor_mm,
        budyko_n=consts.basin_budyko_n,
        min_lake_depth_m=consts.basin_min_lake_depth_m,
        salt_aridity=consts.basin_salt_aridity,
        seasonal_cv_pct=consts.basin_seasonal_cv_pct,
    )


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
    padded_climate: "np.ndarray | None" = None,
) -> dict:
    """Run B0-B3 over the padded domain and return every padded grid.

    Separated from ``bake_tile`` so a seam test can compare a per-tile bake's
    apron against a neighbour's interior directly, which is the check that
    caught the array-coordinate fBm in the first place.

    ``padded_climate`` is the ``(4, padded_coarse_px, padded_coarse_px)`` uint8
    climate stack from ``assemble_padded_climate``, or None. It is read by the
    LANDFORM PROVINCE partition (``bake.province``) and by nothing else; with
    None the partition degrades to relief and elevation alone, which is what
    every elevation-only caller gets.
    """
    if padded_coarse.shape != (geom.padded_coarse_px, geom.padded_coarse_px):
        raise ValueError(
            f"padded coarse domain is {padded_coarse.shape}, expected "
            f"{(geom.padded_coarse_px, geom.padded_coarse_px)}"
        )
    if padded_climate is not None:
        exp = (len(_province.CLIMATE_ORDER),
               geom.padded_coarse_px, geom.padded_coarse_px)
        if tuple(np.shape(padded_climate)) != exp:
            raise ValueError(
                f"padded climate is {np.shape(padded_climate)}, expected {exp}")

    # -- LANDFORM PROVINCES (bake_ver 7). Built once, up front, on the COARSE
    # domain: 576^2 at production, so the whole partition plus all six
    # parameter fields is ~10 MB against the 7 GB peak, and every consumer
    # gathers `field[y // geom.scale, x // geom.scale]` rather than materialising
    # 340 MB of np.repeat. Computed from `padded_coarse` -- the raster the model
    # produced -- rather than from independently hashed noise, which is the
    # plan's load-bearing principle and also the stronger seam story.
    #
    # NOT free the way "regional" is free, and worth being precise about: the
    # regional-energy factor is a pure function of the CARRIER, computed inside
    # B2d. This is a pure function of the coarse SOURCE, which is the carrier's
    # own input, so it is available before B0 and is world-anchored by the same
    # argument.
    #
    # THE APRON OBLIGATION is the landform smooth, and it is enforced by
    # geometry rather than trusted: `apron_coarse_px // 4` caps the smoothing
    # half-width so the influence radius (2 * half) is at most HALF the apron
    # on ANY geometry, production or test. Below that radius, two neighbouring
    # bakes read the same coarse cells throughout their overlap and compute
    # identical province values there.
    #
    # That is a statement about what province ADDS, and it is not a seam
    # guarantee -- see APRON_BLIND_SPOT, which measured the guarantee already
    # violated by 1.05% of the shipped interior for reasons (an unbounded
    # depression fill on a truncated domain) that predate this and are
    # untouched by it.
    prov = None
    if consts.province_strength > 0.0:
        prov = _province.province_fields(
            padded_coarse, padded_climate,
            coarse_pixel_m=geom.coarse_pixel_m, consts=consts,
            max_half=geom.apron_coarse_px // 4,
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
    # np.gradient already returns float32 for float32 input, and np.hypot
    # preserves it -- so .astype(np.float32) was allocating a redundant full
    # -domain copy (340 MB at 9216^2). copy=False keeps the cast as a no-op
    # assertion of the dtype rather than a second array.
    slope = np.hypot(gx, gy).astype(np.float32, copy=False)
    # gy and gx are dead the moment slope exists, and at 9216^2 they are
    # 340 MB each. Held to the end of the bake they inflate the B2d/B3 peak,
    # which is what caps how many bakes fit on one host.
    del gy, gx
    origin_cells = geom.padded_origin_cells(tile_x, tile_y)
    seed = roughness_seed(
        world_seed,
        origin_cells,
        bake_version=TERRAIN_VERSION,
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
    # In-place: `fine = fine + delta` allocates a third full-domain array while
    # both operands are still live. `delta` and `slope` are both dead after
    # this, and freeing them here rather than at function exit is worth ~680 MB
    # of the measured 7.76 GB peak.
    fine += delta
    del delta, slope
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
    inflow_q = None
    if inflow_source is not None:
        # ONE set of crossings, TWO currencies (task #49, CARRIED_DISCHARGE).
        # Area drives incision through A^m and decides the ground; discharge
        # drives the water plane. They must enter at the same cell, and here
        # they do because they are scattered through the same pairs.
        _entries = _edge_entries(
            child_z=filled,
            child_origin_m=geom.padded_origin_m(tile_x, tile_y),
            child_cell_m=cell_m,
            src=inflow_source,
            d8_fn=kernels.d8_receivers,
        )
        inflow = _scatter_entries(_entries, inflow_source.acc, filled.shape)
        if inflow_source.q is not None:
            if consts.water_inject_at_interior_rim:
                # HYDROLOGY_RESIDUALS #7. A SECOND set of crossings, taken
                # against the shipped interior rather than the padded domain,
                # so the water is delivered into the tile instead of onto the
                # apron it then drains out of. The area currency above keeps
                # the padded-rim crossings untouched, which is what keeps this
                # change water-only. See the constant's own note for the
                # invariant this trades away.
                _q_entries = _edge_entries(
                    child_z=filled,
                    child_origin_m=geom.padded_origin_m(tile_x, tile_y),
                    child_cell_m=cell_m,
                    src=inflow_source,
                    d8_fn=kernels.d8_receivers,
                    target_slice=geom.interior(),
                )
                inflow_q = _scatter_entries(
                    _q_entries, inflow_source.q, filled.shape, dtype=np.float64
                )
                del _q_entries
            else:
                inflow_q = _scatter_entries(
                    _entries, inflow_source.q, filled.shape, dtype=np.float64
                )
        del _entries
    # accumulate_mfd returns float64 m^2. Incision consumes it at full width;
    # the STORED field is narrowed to float32 (measured to agree to 2 ppm, and
    # 340 MB against 680 MB over the padded domain matters here) since its
    # consumers are a log2 flow plane and threshold tests.
    #
    # The sweep sorts `filled` by elevation, and B2d's implicit solve walks
    # that SAME order over that SAME array. Asking for it here rather than
    # sorting again there removes a full-domain argsort (13.6 s at 9216^2 with
    # `kind="stable"`) and a ~1.0 GB int64-plus-int32 transient from the bake's
    # PEAK stage, at the cost of keeping one 340 MB int32 array live across
    # B2c->B2d. Asked for only when B2d will use it, and a kernel double that
    # does not know the flag simply returns the area alone.
    order_kwargs = (
        {"return_order": True} if consts.incision_mode == "profile" else {}
    )
    acc_result = kernels.accumulate_mfd(
        filled, cell_m, p=consts.mfd_p, inflow=inflow, **order_kwargs
    )
    if isinstance(acc_result, tuple):
        acc64, elev_order = acc_result
    else:
        acc64, elev_order = acc_result, None
    acc64 = np.asarray(acc64)
    del acc_result
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
        regional_scale = 1
        if consts.profile_regional_s_ref > 0.0:
            # The carrier's 30 m-scale slope: a pure function of the
            # (world-anchored, apron-consistent) carrier, so it cannot
            # introduce a seam of its own. The scale factor 16 is the
            # coarse/fine ratio whatever the geometry.
            #
            # Kept COARSE. It used to be np.repeat-ed twice back up to the
            # fine grid, which at 9216^2 is 340 MB of 16x16-replicated float32
            # allocated inside the bake's peak stage; `profile_incision` now
            # takes `regional_scale` and gathers `coarse[y // f, x // f]` in
            # its row blocks instead. The gather copies values rather than
            # computing them, and its index clamp reproduces the old
            # edge-extension of a non-multiple domain, so the carve is
            # bit-identical.
            f = geom.scale
            h_f, w_f = fine.shape
            cb = fine[:h_f - h_f % f, :w_f - w_f % f].reshape(
                h_f // f, f, w_f // f, f).mean(axis=(1, 3))
            gyc, gxc = np.gradient(cb.astype(np.float64), cell_m * f)
            # float32 HERE, exactly where the assignment into the float32
            # full-resolution array used to round it.
            regional = np.hypot(gxc, gyc).astype(np.float32)
            regional_scale = f
            del cb, gyc, gxc
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
        # Forwarded only when B2c actually handed one back, for the same
        # test-double reason as the kwargs above.
        order_fwd = {} if elev_order is None else {"order": elev_order}
        # PROVINCE PARAMETER FIELDS (bake_ver 7). Forwarded only when the
        # partition is live, for the same test-double reason as the kwargs
        # above -- a kernel double written against the scalar form keeps
        # working, and province_strength = 0 takes this branch's `else` and
        # reproduces bake_ver 6 exactly.
        #
        # These are the four constants the `kfac` chain treats ELEMENTWISE
        # (K_dt * A^m * regional * erodibility * gate * taper), so per-cell is
        # arithmetic rather than a kernel change. stream_n and incision_cap_m
        # are NOT here: they are scalars inside the numba Newton kernel.
        province_kwargs = {}
        if prov is not None:
            province_kwargs = {
                "K_dt": prov.k_dt,
                "m": prov.stream_m,
                "a_crit_m2": prov.a_crit_m2,
                "gate_q": prov.gate_q,
                "field_scale": geom.scale,
            }
        else:
            province_kwargs = {
                "K_dt": consts.profile_K_dt,
                "m": consts.stream_m,
                "a_crit_m2": consts.channel_init_area_m2,
                "gate_q": consts.channel_init_q,
            }
        eroded = np.asarray(
            kernels.profile_incision(
                filled,
                rec,
                acc64,
                cell_m,
                n=consts.stream_n,
                cap_m=consts.incision_cap_m,
                **province_kwargs,
                regional_slope=regional,
                regional_s_ref=consts.profile_regional_s_ref,
                regional_scale=regional_scale,
                sea_taper_top_m=consts.sea_taper_top_m,
                sea_taper_bottom_m=consts.sea_taper_bottom_m,
                **regional_p_kwargs,
                **strength_kwargs,
                **order_fwd,
            ),
            dtype=np.float32,
        )
        del regional, strength_kwargs, order_fwd, province_kwargs
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
    del rec, elev_order
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
        from .flow import enforce_descent  # lazy, same reason as repose_field
        from .noise import meso_relief

        # Province amplitude fields, coarse and gathered at `amp_scale`. The
        # existing slope gate still applies on top: this varies HOW MUCH band a
        # steep face gets, not WHICH faces get one.
        meso_amp_kwargs = (
            {"amp15_m": prov.meso_amp15_m, "amp11_m": prov.meso_amp11_m,
             "amp_scale": geom.scale}
            if prov is not None else
            {"amp15_m": consts.meso_amp15_m, "amp11_m": consts.meso_amp11_m}
        )

        # The gate needs the DOWNSTREAM slope of this exact surface: a gully
        # bed between steep walls must read its own gentle profile, or the
        # band perturbs bed long-profiles into the codec's quantization floor
        # (see meso_relief's flow_slope note). rec4 is a 340 MB transient,
        # dropped before the field is built.
        rec4, d8s4 = kernels.d8_receivers(z, cell_m)
        z_pre = z
        z = z + meso_relief(
            z,
            cell_m,
            seed,
            origin_cells,
            **meso_amp_kwargs,
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
        # codec-proof stays codec-proof.
        #
        # ONE topological sweep, not a fixed point. This was a ~15-20 pass
        # numpy iteration of `np.maximum(zf, zf[tgt] + drop)` at 1.22 s a
        # pass, each pass propagating one tree level and each pass building a
        # 340 MB `need`. But the constraint lives on the pre-meso D8 forest,
        # whose receivers are strictly lower, so visiting each cell after its
        # own receiver computes the unique least fixed point directly -- and
        # in the same float32 arithmetic, which (addition being monotone) makes
        # it the same bits rather than merely the same value. It also drops
        # the int64 `tgt` and the full-domain `arange` that built it.
        z = enforce_descent(rec4, z_pre, z, consts.refill_eps_m)
        del rec4, z_pre
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
        # Depth of every depression B2a levelled into the carrier: the raster
        # bake_tile previously reduced to four scalars and deleted. Kept as a
        # first-class output because it is the LAKE-BED survey the watershed
        # plan (docs/water-system-architecture.md) sizes every decision on: where
        # a basin was, how deep, and what fill level would flood it. Same
        # bytes ship: this is observability, exposed via STAGE_SINK_FIELDS,
        # never encoded. filled >= fine cell-for-cell (the fill only raises),
        # so the field is non-negative by construction.
        "basin_depth": np.asarray(filled - fine, dtype=np.float32),
        "eroded": eroded,
        "acc": acc64.astype(np.float32),
        "incision": depth,
        "thermal_gain": (z - eroded).astype(np.float32),
        "d8_slope": d8_slope,
        "inflow": inflow,
        # CARRIED DISCHARGE at the domain edge, m^3/yr, or None when the
        # superblock cannot answer (no climate, or a pre-task-#49 block). None
        # is NOT zero and B6 branches on it -- see CARRIED_DISCHARGE.
        "inflow_q": inflow_q,
        "roughness_seed": seed,
        "interior_dead_ends": interior_dead_ends,
        # The COARSE province object (or None). Observability only -- nothing
        # downstream reads it, and it is ~10 MB rather than a fine grid. A
        # province the probe cannot see is a province nobody can tune.
        "province": prov,
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
    ("B2a.basin_depth", "basin_depth"),
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
    climate_fetch: "ClimateFetch | None" = None,
) -> BakeResult:
    """Bake one coarse tile's fine tier. Interior only; the apron is discarded.

    ``coarse_fetch(x, y)`` returns that coarse tile's elevation in METRES as a
    (coarse_tile_px, coarse_tile_px) array, or None if it does not exist.
    ``inflow_source`` is the level-0 flow superblock covering this tile; None
    means "no cross-tile hydrology", which is only correct for a test.

    ``climate_fetch(x, y)`` returns that coarse tile's ``(4, coarse_tile_px,
    coarse_tile_px)`` uint8 climate planes, or None. Optional: without it the
    landform-province partition sees relief and elevation but no climate, so
    GLACIAL and ARID -- which cannot be inferred from shape alone -- stay at
    zero weight everywhere.

    ``stage_sink(name, interior)`` is called once per `STAGE_SINK_FIELDS` entry
    with the INTERIOR (apron already cropped) of that sub-stage's grid, in
    pipeline order, before the result is assembled. The arrays are views into
    the bake's own grids: a sink that keeps one must copy it. Purely an
    observer -- it cannot change what is baked, and passing one does not touch
    the bake identity.
    """
    kernels = kernels or load_kernels()
    padded, missing = assemble_padded_coarse(coarse_fetch, tile_x, tile_y, geom)
    padded_climate = assemble_padded_climate(climate_fetch, tile_x, tile_y, geom)
    out = bake_padded_domain(
        padded,
        world_seed=world_seed,
        tile_x=tile_x,
        tile_y=tile_y,
        kernels=kernels,
        geom=geom,
        consts=consts,
        inflow_source=inflow_source,
        padded_climate=padded_climate,
    )
    sl = geom.interior()
    if stage_sink is not None:
        for name, key in STAGE_SINK_FIELDS:
            stage_sink(name, out[key][sl, sl])

    # -- B5: RE-OPEN THE REGISTERED BASINS (bake_ver 8, plan §4.2).
    #
    # Everything above ran on the depression-FILLED surface and stays exactly
    # as it was. That is hydrologically right, not a compromise: water flows
    # ACROSS a lake at its surface, so routing, the flow plane, and incision
    # (which cuts the outlet gorge through the rim and a thalweg across the
    # lake floor -- both correct, both desirable) all belong on the filled
    # surface, and the flow plane keeps agreeing with the carve cell for cell.
    # Only the LAST thing the bake does -- decide what elevation ships --
    # changes: registered holes are given back.
    #
    # Run on the PADDED domain, because the fill that recomputes each basin's
    # spill needs a boundary condition and the interior alone would invent
    # one, and because "does this basin leave the tile" is a question about
    # the padded grid.
    #
    # THE SPILL IS RECOMPUTED HERE, on the FINAL surface, and this is the part
    # no existing text anticipated: `basin_depth` is B2a's fill of the
    # PRE-EROSION carrier, and between B2a and here the bake has incised
    # (which can cut a rim metres lower), relaxed and added a meso band. A
    # table built from the B2a level would float lakes above their own
    # outlets. See bake/basins.py's header.
    c0 = time.process_time()
    climate_phys = (None if padded_climate is None
                    else _province.dequantize_climate(padded_climate))
    survey = _basins.survey_basins(
        z_final=out["z"],
        basin_depth=out["basin_depth"],
        accumulation_m2=out["acc"],
        climate=climate_phys,
        cell_m=geom.fine_pixel_m,
        interior=sl,
        filt=basin_filter(consts),
        wb=basin_balance(consts),
        keep_labels=True,
        keep_hypsometry=False,
    )
    # THE PRE-B5 SURFACE, kept for B6. It is the one the B4b refill guarantees
    # has ZERO SINKS, which is what makes the water plane's descent chain well
    # posed; after the re-opening below it has a hole per registered basin. A
    # full padded float32 copy (340 MB at production) taken only when the plane
    # will actually be built, and dropped inside B6.
    z_pre_reopen = None
    basin_keep_pad = None
    if consts.water_plane_enabled and padded_climate is not None:
        z_pre_reopen = out["z"].copy()
    if survey.basins:
        keep = survey.keep_mask()
        basin_keep_pad = keep
        # `basin_depth` is continuous and 0 at the rim by construction, so
        # subtracting it under the mask re-opens the hole with no seam at any
        # edge -- no feathering, no blend width, nothing to tune.
        #
        # IN PLACE, under a boolean index, not `np.where`: the registry covers
        # a couple of per cent of the domain, so this allocates on the order of
        # the basins' own area instead of two more full padded float32 grids
        # (340 MB each at production, on top of a bake that already peaks
        # around 5 GiB).
        out["z"][keep] -= out["basin_depth"][keep]
    survey.labels = None
    out["cpu_seconds"]["B5.reopen_basins"] = time.process_time() - c0

    z = np.ascontiguousarray(out["z"][sl, sl])
    acc = np.ascontiguousarray(out["acc"][sl, sl])
    incision = out["incision"][sl, sl]
    gain = out["thermal_gain"][sl, sl]
    # The DEPRESSION-FILLED surface, not the relaxed one: it is what
    # stream_power tapered against, so the flow plane's channel flag and the
    # incision it describes agree cell for cell.
    plane = flow_plane(acc, incision, gain, consts, elev_m=out["filled"][sl, sl])

    # -- B6: RUNOFF-WEIGHTED DISCHARGE AND THE GRADED WATER PLANE
    #    (bake_ver 9, plan P2 / §4.1).
    #
    # WHY IT CANNOT BE DONE ANYWHERE ELSE, measured before it was built. The
    # obvious cheap alternative is an additive pass over already-shipped tiles:
    # read accumulation back out of the flow plane's 5 log2 bits, multiply by
    # the local runoff, write the plane, skip the re-bake. Measured on the
    # shipped 289-tile coarse world over 174,886 drawable river cells, that
    # reconstruction writes 46.35% OF THE RIVER NETWORK AS DRY and agrees with
    # the true flow class on only 74.13% of the rest.
    #
    # And the reason is NOT the 5-bit quantisation, which is comparatively
    # benign (Q ratio p5/p95 = 0.732/1.366 at the bucket midpoint). It is that
    # AREA IS THE WRONG QUANTITY. A river is wet because of where its water
    # came from, not where it is: median local runoff at a river cell is
    # 247 mm/yr while the catchment-mean its discharge implies is 577 mm/yr,
    # and |mean - local| / mean is 0.571 at p50, 0.998 at p95. Exotic rivers
    # crossing dry ground have a local runoff of ~0 and vanish entirely. No
    # wider accumulation field on the wire would fix that; only a
    # runoff-weighted sweep over the network gives Q, and that sweep needs the
    # padded domain and the superblock inflow, which exist only here.
    #
    # ONE EXTRA MFD SWEEP, not two. `accumulate_mfd(source=)` replaces the
    # cell-area seed with each cell's runoff VOLUME, so the sweep returns
    # m^3/yr directly over the identical weights the area field used. (The
    # two-sweeps-and-subtract form in tools/worldmaps/water.py is exact only
    # because that tool has no `inflow`; with the pyramid's edge injection live
    # the difference of the two sweeps is A(I_q) - A(I_area), which is not the
    # discharge.)
    c0 = time.process_time()
    discharge = None
    water_surface = None
    water_stats: dict[str, float] = {}
    runoff_coarse = _water.runoff_field_mm_yr(
        padded_climate, basin_balance(consts), consts.province_smooth_m,
        geom.coarse_pixel_m,
    )
    if consts.water_plane_enabled and runoff_coarse is not None:
        # THE BOUNDARY CONDITION IS A DISCHARGE NOW (task #49). The pyramid
        # carries Q, so the water arriving at this tile's edge is the water that
        # actually fell on its catchment -- wherever that catchment is and
        # whatever the climate there. The area proxy below is the fallback for a
        # superblock that predates the change or was built without climate; it
        # is what made the showcase corridor's mouth bake dry. Which one ran is
        # reported per tile, because "the proxy is still in use here" must never
        # be something you have to infer from the shape of a river.
        carried_q = out["inflow_q"]
        src = _water.discharge_source(
            runoff_coarse, out["filled"].shape, geom.scale, geom.fine_pixel_m,
            inflow_area_m2=(None if carried_q is not None else out["inflow"]),
            inflow_q_m3_yr=carried_q,
        )
        # THE WATER PASS CONCENTRATES; THE TERRAIN PASS DOES NOT (bake_ver 11).
        # `acc` above is the area field and is still MFD at `consts.mfd_p`: it
        # feeds `A^m`, it decides every height, and pure D8 would put
        # dead-straight 45-degree channels in the ground (flow.py's first
        # lesson). Nothing here can reach it -- this is a separate sweep over
        # the same surface whose only consumer is the `q >= q_drawable`
        # threshold below.
        #
        # And a threshold is exactly what MFD is worst for. Splitting a reach's
        # discharge across every lower neighbour does not make a smoother
        # network, it makes one that crosses the cut and comes back: 25-33% of
        # network cells on the measured corridor have no strictly lower
        # neighbour holding as much as they do, and one walked channel showed
        # fifty wet-dry-wet excursions in 2,001 steps. Under a single-receiver
        # rule that count is 0 by construction.
        #
        # The forest is built on `filled`, THE SAME SURFACE THIS SWEEP RUNS
        # OVER, not on `z_route` below. `filled` is what the elevation argsort
        # orders and what `d8_receivers` guarantees a strictly-lower receiver
        # on; borrowing the water surface's forest would walk an order that is
        # not descending along it and the result would not be an accumulation.
        # It is also B2b's own forest, so the discharge follows the same
        # centrelines the incision's slope term was taken along.
        if consts.water_flow_single_receiver:
            if kernels.accumulate_d8 is None:
                raise RuntimeError(
                    "water_flow_single_receiver is set but this BakeKernels has "
                    "no accumulate_d8 kernel; inject flow.accumulate_d8 or set "
                    "the constant False (which reproduces the bake_ver 10 "
                    "water pass)"
                )
            rec_q, _ = kernels.d8_receivers(out["filled"], geom.fine_pixel_m)
            q_pad = kernels.accumulate_d8(
                out["filled"], geom.fine_pixel_m, source=src, receivers=rec_q
            )
            del rec_q
        else:
            q_pad = kernels.accumulate_mfd(
                out["filled"], geom.fine_pixel_m, p=consts.mfd_p, source=src
            )
        del src
        q_pad = np.asarray(q_pad, np.float64)

        # THE ROUTING BED IS THE PRE-B5 SURFACE. B4b's refill guarantees it has
        # zero sinks, which is what makes a descent chain over it well posed.
        # Post-B5 the registered basins are holes, and grading a river's water
        # surface down into a 500 m re-opened basin would drag the whole
        # upstream profile with it. Basin cells are written DRY here; their
        # surface is already on the wire in SECTION_BASIN_TABLE and the client
        # composes the two samplers.
        z_route = np.ascontiguousarray(z_pre_reopen, np.float32)
        rec_w, _ = kernels.d8_receivers(z_route, geom.fine_pixel_m)
        q_draw = _water.q_drawable_m3_yr(
            geom.fine_pixel_m, consts.water_min_width_px,
            consts.water_q_perennial_m3_yr,
        )
        wet_pad, heads_pad, head_stats = _water.water_head_mask(
            q_pad, rec_w, q_drawable=q_draw,
            q_perennial=consts.water_q_perennial_m3_yr,
        )
        w_pad = _water.graded_water_surface(
            z_route, q_pad, rec_w, wet_pad,
            eps_m=consts.refill_eps_m, exclude=basin_keep_pad,
            q_perennial=consts.water_q_perennial_m3_yr,
            slope_in_depth=consts.water_slope_in_depth,
            cell_m=geom.fine_pixel_m,
        )

        # -- THE EXTENT (bake_ver 13). The plane above is a CENTRELINE -- a
        # single-receiver forest has one-cell-wide branches by construction --
        # and this decides how far sideways the drawn water reaches. See
        # `BakeConstants.water_extent_mode` for the three rules and for the
        # owner observation that retired the middle one.
        #
        # `lateral_fill` is the shipped rule: a cell is wet when it stands below
        # the surface of the water it drains into, so the TERRAIN decides the
        # width and the same discharge comes out wide on a floodplain and narrow
        # in a gorge. The `law` branch is bake_ver 12's ribbon, kept because a
        # constant that claims to reproduce a previous bake has to be able to.
        #
        # ON THE PADDED DOMAIN, before the interior is cropped, so a reach that
        # runs along a tile edge is filled from both sides of it rather than
        # being cut in half by the crop.
        #
        # AGAINST `out["z"]`, THE SHIPPED SURFACE, not `z_route`. `z_route` is
        # the pre-B5 bed the descent chain needed; the wetness test is a
        # statement about the ground the client draws the waterline against,
        # which is the post-B5 one, and it is also the array
        # `water_depth_control_points` takes the stored depth against. Using the
        # routing bed here would admit cells that the encoder then reads as
        # water below its own ground. Outside the registered basins the two are
        # the same array cell for cell -- B5 subtracts `basin_depth` only under
        # `keep` -- which is what lets the fill's connectivity argument (every
        # wet cell has a wet descending path to the channel) hold on the surface
        # actually tested rather than on the one the forest was built on.
        centreline_pad = np.isfinite(w_pad)
        width_stats: dict[str, float] = {}
        if consts.water_extent_mode == "lateral_fill":
            w_pad, width_stats = _water.fill_to_local_surface(
                w_pad, out["z"], rec_w, cell_m=geom.fine_pixel_m,
                exclude=basin_keep_pad,
                # The slope field comes from the SAME receiver forest the fill
                # walks, so the extent and the flow direction cannot disagree
                # about which way the ground falls.
                slope=(_water.slope_to_receiver(z_route, rec_w, geom.fine_pixel_m)
                       if consts.water_slope_in_extent else None),
            )
        elif consts.water_extent_mode == "law":
            w_pad, width_stats = _water.widen_to_channel_width(
                w_pad, out["z"], q_pad, cell_m=geom.fine_pixel_m,
                exclude=basin_keep_pad,
                q_perennial=consts.water_q_perennial_m3_yr,
            )

        # -- CONTACT (bake_ver 14). Everything above decides WHICH CELLS are
        # wet. Nothing above decides whether the water in two adjacent cells
        # TOUCHES, and on a slope it does not: a flood-to-a-level rule on a
        # descending bed leaves each column standing in its own air, and a D8
        # path that steps diagonally leaves columns joined at a corner, which
        # is not a face. Measured on the shipped bv13 corridor the same wet
        # cells are 24-75 pieces in plan and 3,409-7,987 pieces as drawn.
        #
        # ON THE PADDED DOMAIN, for the reason the fill runs here: a reach
        # along a tile edge must be bridged from both sides of the seam, not
        # cut in half by the crop.
        #
        # AGAINST `out["z"]`, the shipped surface, because the contact test is
        # a statement about the ground the client draws the waterline against
        # and it is the array the codec takes the stored depth from.
        if consts.water_face_contact_bridge:
            w_pad, bridge_stats = _water.bridge_to_face_contact(
                w_pad, out["z"], cell_m=geom.fine_pixel_m,
                exclude=basin_keep_pad,
            )
            width_stats.update(bridge_stats)

        # LAST, AND THAT ORDERING WAS BOUGHT WITH A WASTED BAKE. This ran
        # BEFORE `bridge_to_face_contact` at bake_ver 16 and the bridge undid
        # it: the bridge RAISES a cell's surface to reach its neighbours' beds,
        # which is the exact inverse operation. Measured, the pass removed 100%
        # of the violations offline and only 3% survived to the shipped tile
        # (560,616 -> 544,544).
        #
        # Running it last means it can BREAK face contact the bridge just
        # established. That tension is real and is the point: the bridge exists
        # as a hand-built substitute for the slope term F3 now supplies, so how
        # much it is still doing is F3's own acceptance test. The per-tile log
        # prints both counters so the trade is visible rather than assumed.
        # WATER FINDS ITS OWN LEVEL. Measured on tile (-4,-4) at bake_ver 17,
        # 168,708 cells -- 59 hectares on ONE tile -- lay dry with adjacent
        # water standing p50 0.90 m above them, because `_fill_levels` only
        # carries a level UP a drawn cell's donors and a bed cell upstream of
        # nothing drawn gets NaN. Runs BEFORE the consistency pass so anything
        # it adds is held to the same no-water-above-its-upstream-neighbour
        # rule as everything else.
        if consts.water_settle_to_level:
            w_pad, settle_stats = _water.settle_to_adjacent_level(
                w_pad, out["z"], min_depth_m=_water.WIDEN_MIN_DEPTH_M,
                max_iter=int(consts.water_settle_max_iter),
                q_m3_yr=(q_pad if consts.water_settle_discharge_budget
                         else None),
                cell_m=geom.fine_pixel_m,
                level_smooth_iters=int(consts.water_level_smooth_iters))
            width_stats.update(settle_stats)

        if consts.water_level_neighbour_consistency:
            w_pad, lvl_stats = _water.enforce_neighbour_consistency(
                w_pad, out["z"])
            width_stats.update(lvl_stats)

        # THE HARD RULE, LAST, ASSUMING NOTHING. No water may stand above its
        # own upstream water. graded_water_surface established this and every
        # stage since -- widening, lateral fill, the bridge, settling, the
        # budget -- can break it. This is the only stage that re-checks, and it
        # runs after all of them on purpose.
        # NO locals() GUARD. It used to say `locals().get("rec_w") is not None`,
        # and `del rec_w` ran 70 lines above, so this stage NEVER EXECUTED in any
        # bake -- while the log printed mono=0>0 from the stats dict's default
        # and read as "zero violations, nothing to fix". A guard written to make
        # skipping safe made it invisible instead. Second time in one session a
        # counter read zero because the value was absent rather than small.
        if consts.water_enforce_upstream_monotone:
            w_pad, mono_stats = _water.enforce_upstream_monotone(
                w_pad, rec_w, out["z"],
                min_depth_m=_water.WIDEN_MIN_DEPTH_M)
            width_stats.update(mono_stats)

        discharge = np.ascontiguousarray(q_pad[sl, sl].astype(np.float32))
        water_surface = np.ascontiguousarray(w_pad[sl, sl])
        wet_int = np.isfinite(water_surface)
        # THE GATE READS THE CENTRELINE, not the widened ribbon, and that is not
        # a convenience. `overshoot_stats` asks "is the water the size its own Q
        # says", and a widened cell's own Q is the trickle that happens to fall
        # on it -- off-network by construction -- so including it would compare
        # a trunk river's depth against a hillside cell's law and read as metres
        # of overshoot that nothing overshot. The widened extent has its own
        # numbers, in `width_stats`.
        centre_int = np.ascontiguousarray(centreline_pad[sl, sl]) & wet_int
        del centreline_pad

        # THE OVERSHOOT GATE, on the interior only -- the bytes that ship.
        # Overshoot is the failure mode with NO GEOMETRIC BACKSTOP: the bank
        # probe found no containing ground within 120 m on 736 of 1,260 stream
        # bank sides at 10 m depth, so a head that comes out too high floods the
        # valley rather than leaking sideways and self-correcting.
        water_stats = {f"water_{k}": v for k, v in head_stats.items()}
        # WHICH BOUNDARY CONDITION RAN, and how much water it delivered. The
        # first is the honesty flag for task #49 -- 1.0 means the pyramid
        # carried Q, 0.0 means this tile is still on the local-runoff proxy that
        # baked a river mouth dry. The second and third are the magnitudes, so a
        # tile whose river is mostly imported (which is what "exotic" means) is
        # visible as a number rather than inferred from a picture.
        q_in_total = 0.0 if carried_q is None else float(carried_q.sum())
        water_stats.update({
            "water_q_inflow_carried": 0.0 if carried_q is None else 1.0,
            "water_q_inflow_m3_yr": q_in_total,
            "water_q_inflow_entry_cells": (
                0.0 if carried_q is None else float((carried_q > 0.0).sum())
            ),
            # The implied catchment-mean runoff behind the imported water:
            # Q_in / A_in, in mm/yr. Compare it against this tile's own local
            # runoff and the gap IS the defect, in one number. It is what
            # `water.py`'s docstring measures as 0.571 at p50 world-wide.
            "water_q_inflow_implied_runoff_mm_yr": (
                1000.0 * q_in_total / float(out["inflow"].sum())
                if (carried_q is not None and out["inflow"] is not None
                    and float(out["inflow"].sum()) > 0.0)
                else 0.0
            ),
        })
        # THE INTERIOR DISCHARGE MAXIMUM, which is the number HYDROLOGY_RESIDUALS
        # #7 turns on and which nothing reported until now. `water_q_max_m3_yr`
        # above is the PADDED maximum, and the gap between the two IS the
        # residual: on (-7,-5) it was 3.59e8 padded against 1.30e6 interior, a
        # stream that converged inside the apron and drained back out without
        # ever crossing the tile that ships. Discharge only increases
        # downstream, so padded >> interior cannot be dispersion.
        water_stats["water_q_interior_max_m3_yr"] = float(discharge.max())
        water_stats["water_q_apron_loss_ratio"] = (
            float(head_stats["q_max_m3_yr"]) / float(discharge.max())
            if float(discharge.max()) > 0.0 else 0.0
        )
        water_stats.update({f"water_{k}": v for k, v in width_stats.items()})
        if wet_int.any():
            # THE ACCEPTANCE STATISTIC FOR bake_ver 14, on the shipped interior.
            # "Is the river connected" was answered by labelling the wet mask
            # until it turned out that a mask cannot see whether two adjacent
            # columns of water touch, which on a slope they do not. This asks
            # the client's own question -- does the lower cell's surface reach
            # the upper cell's bed -- and a non-zero `contact_face_broken` after
            # the bridge is a real regression rather than a distribution moving.
            # Component labelling is left to `tools/river_column_contact.py`:
            # see `face_contact_stats`, it is a Python loop over millions of
            # edges and would cost more than the stage it grades.
            water_stats.update({
                f"water_{k}": v for k, v in _water.face_contact_stats(
                    water_surface, z, components=False,
                ).items()
            })
        if centre_int.any():
            water_stats.update({
                f"water_{k}": v for k, v in _water.overshoot_stats(
                    water_surface, z, discharge, centre_int,
                    cell_m=geom.fine_pixel_m,
                    z_route_m=np.ascontiguousarray(z_route[sl, sl]),
                    q_perennial=consts.water_q_perennial_m3_yr,
                ).items()
            })
        if wet_int.any():
            # The DRAWN ribbon against the law it is supposed to be following.
            # Reported on the shipped interior, at the same percentiles as the
            # law, so "wider than a centreline" is a number rather than a claim.
            # Its own receiver forest, on the INTERIOR: the transect has to run
            # perpendicular to the flow, and `rec_w` is indexed in padded flat
            # coordinates that do not survive the crop. One 8192^2 D8 pass
            # against a bake that already spends minutes on 9216^2.
            rec_int, _ = kernels.d8_receivers(z, geom.fine_pixel_m)
            drawn = _water.lateral_extent_stats(
                water_surface, z, discharge, wet_int, rec_int,
                cell_m=geom.fine_pixel_m,
                q_perennial=consts.water_q_perennial_m3_yr,
            )
            del rec_int
            water_stats.update({f"water_drawn_{k}": v for k, v in drawn.items()})
        del wet_pad, heads_pad, w_pad, q_pad, z_route
    del z_pre_reopen, basin_keep_pad
    out["cpu_seconds"]["B6.discharge_water"] = time.process_time() - c0


    basin_depth = out["basin_depth"]
    padded_basin = basin_depth > 0.0
    # THIS TEST CANNOT FIRE, AND NEVER COULD. It was meant to be the sound
    # condition of APRON_BLIND_SPOT: "only a flat that REACHES THE PADDED
    # BORDER is entered from the border". But ``fill_depressions`` never
    # raises a border cell (flow.py's own docstring says so -- that is what
    # makes a bake on tile+apron agree with a bake on a larger domain), so
    # ``filled - carrier`` is identically 0 along all four edges and no
    # depression can contain a border cell. Verified on a synthetic hollow
    # whose corner IS the domain corner: zero basin cells, because a hollow
    # that reaches the border DRAINS OUT and is not a depression at all.
    # ``basin_reaches_padded_border`` has therefore read 0.0 for every tile in
    # the record, and the watershed plan's §4.2.4 built a v1 exclusion on it.
    #
    # Kept, at zero, rather than deleted: the key is quoted in the plan and in
    # three stat files, and a key that silently vanished would be read as a
    # missing measurement rather than as a retired one. The exclusion that
    # actually ships is B5's TILE-SPANNING one, counted below, and the
    # near-the-edge diagnostic that replaces this is
    # ``basins_near_padded_edge``.
    border_basin = int(
        padded_basin[0, :].sum()
        + padded_basin[-1, :].sum()
        + padded_basin[:, 0].sum()
        + padded_basin[:, -1].sum()
    )
    # (The only place in this repo where it has ever been non-zero is
    # test_border_detector_separates_a_contained_flat_from_a_spilling_one,
    # which SUBSTITUTES a fake `fill_depressions` that adds 1.0 to every cell
    # -- including the border. That test demonstrates the statistic's
    # arithmetic; it is not evidence that the condition occurs.)
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
        # Structurally zero -- see the comment beside border_basin above.
        "padded_border_basin_cells": float(border_basin),
        "padded_border_basin_frac": float(border_basin) / float(border_cells),
        "basin_reaches_padded_border": float(border_basin > 0),
        # -- B5 registry (bake_ver 8). Counts, so a contended box cannot
        # distort them, and every exclusion is a NUMBER: the plan asks for the
        # cost of the tile-spanning refusal to be measured rather than assumed,
        # and on the 12-tile survey it was 57% of qualifying components.
        "basins_registered": float(len(survey.basins)),
        "basins_lake": float(sum(1 for b in survey.basins if b.is_lake)),
        "basin_components": float(survey.n_components),
        "basins_excluded_shallow": float(survey.excluded_shallow),
        "basins_excluded_small": float(survey.excluded_small),
        "basins_excluded_spanning": float(survey.excluded_spanning),
        "basins_excluded_spanning_area_m2": float(survey.excluded_spanning_area_m2),
        "basins_excluded_spanning_max_depth_m": float(
            survey.excluded_spanning_max_depth_m),
        "basins_excluded_submarine": float(survey.excluded_submarine),
        "basins_near_padded_edge": float(survey.kept_near_padded_border),
        "basin_water_volume_m3": float(sum(
            b.area_m2 * b.water_depth_m for b in survey.basins)),
        "peak_bytes_estimate": float(estimate_peak_bytes(geom)),
    }
    stats.update(water_stats)
    # LANDFORM PROVINCE MIX over the tile INTERIOR, as area fractions summing
    # to 1. Cheap (a mean over 512^2) and it is the only way to know whether a
    # tile actually encountered the province whose constants it was baked with
    # -- "ARID cannot be tuned or judged until there is a world containing it"
    # is a plan risk, and this is the instrument that answers it per tile.
    prov = out.get("province")
    if prov is not None:
        cs = slice(geom.apron_coarse_px,
                   geom.apron_coarse_px + geom.coarse_tile_px)
        for name, wgt in prov.weights.items():
            stats[f"province_{name}_frac"] = float(wgt[cs, cs].mean())
        stats["province_climate"] = 0.0 if prov.temp_c is None else 1.0
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
        basins=tuple(survey.basins),
        discharge_m3_yr=discharge,
        water_surface_m=water_surface,
        superblock_fingerprint=(
            "" if inflow_source is None else inflow_source.fingerprint_hex
        ),
    )
