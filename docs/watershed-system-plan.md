# The watershed system: sources, rivers, lakes — from the bake to a player's boots

**Written 2026-08-03.** The owner's directive, verbatim:

> "i want realistic rivers that are sized based on the physical terrain output
> by worldgen - whatever that size may be. we [need] some system for our game
> worldgen to analyze the carved terrain, pick the origin/source points for
> streams to begin in a watershed, then populate water throughout the watershed
> to form larger and larger rivers naturally, and then follow the carved
> terrain out to the oceans. there may need to be some substantial new
> engineering work for this to determine many distributed origin/source points
> for water, also to procedurally determine where lakes and ponds should be
> placed at random (dry depressions vs lake)."

This plan builds directly on `docs/water-production-plan.md` (branch
`claude/water-production-plan`, the 2026-08-02 audit) and on
`docs/world-generation-architecture.md` §4–§5 including the 2026-08-03
corrections. Where this document disagrees with either, it says so in §9.
Every claim carrying a file:line was re-verified against the code for this
document; the handful that could not be verified are marked as such.

**The headline: almost everything the directive asks for is already computed
by the bake and then thrown away.** The genuinely new engineering is (a) lakes
— the bake currently guarantees no basin survives to the client, so there is
nowhere for a lake to sit — and (b) getting any water datum onto the wire at
all, since the fine tile today carries elevation plus a lossy flow plane and
no water surface of any kind. The plan is therefore not "build a hydrology
simulator"; it is "stop discarding the hydrology the bake already does,
give it a wire format, and hand it to the one runtime mechanism
(`WaterMobilizer`) that already turns a deterministic water datum into
touchable, replicated, persisted water."

---

## 1. What the directive maps to, term by term

| the owner asked for | what it is in this codebase | state |
|---|---|---|
| "analyze the carved terrain" | the bake's own MFD flow accumulation `acc` (m² upstream area per 1.875 m cell, `BakeResult.accumulation_m2`, `pipeline.py:3115`), routed over the depression-filled surface with cross-tile inflow from the flow-superblock pyramid | **computed on every tile today**; reduced to a lossy per-pixel byte on the wire, zero consumers in the client |
| "pick the origin/source points" | the channel-initiation criterion. The bake carves channels wherever `acc ≥ channel_init_area_m2 = 156 m²` (`pipeline.py:557`) — it already knows where every carved channel begins. **But 156 m² is a geomorphic criterion (where a swale exists), not a hydrologic one (where perennial water starts)** — see §3.1 | carving: shipped. Water heads: new, small, and climate-driven |
| "populate water throughout the watershed to form larger and larger rivers" | the accumulation field *is* this: `acc` grows monotonically downstream, and width/depth-from-discharge laws already exist twice (bake: width ∝ A^0.4 in `incise.py`; runtime: `channel.h:239-240`, width ∝ Q^0.4, depth ∝ Q^0.35, monotone by construction) | the growth is baked content, not a runtime propagation process — that is what makes it affordable (§7) |
| "follow the carved terrain out to the oceans" | the flow pyramid routes to sea level; the carve descends by contract ("'The carrier drains' is a CONTRACT (0 sinks)", `pipeline.py:2928`); `channel.h:100-144` documents why a graded bed reaches the sea with its mouth open below z=0 | shipped for terrain; no water follows it yet |
| "determine where lakes and ponds should be placed at random (dry depressions vs lake)" | **the one place this plan deliberately deviates from the letter of the directive**: not a random roll but a water balance — a basin is a lake when inflow exceeds evaporation, and the tiles already carry the climate inputs (bio_1/bio_4/bio_12/bio_15, `tiles.h:24-37`). A wet-climate depression is a lake; the same depression in a desert is a salt pan. This is the standing "caused variety, not placed variety" principle: a player can see *why* | new bake work, §4. The basins themselves are currently destroyed by the bake — the largest single work item in this plan |

---

## 2. Ground truth, re-verified

The load-bearing facts, each re-checked against the code on 2026-08-03.

**F1 — The bake destroys every lake basin, twice.** `fill_depressions` runs at
B2a on the carrier+roughness surface (`pipeline.py:2600-2602`) and everything
downstream (D8, MFD, incision, thermal, meso) descends from that *filled*
surface; B4b re-runs the fill as the last step before the codec
(`pipeline.py:2935`) to resolve stray pits the later stages introduced. So the
deep basins are gone **before incision ever runs**, and what B4b removes is
millimetre-scale residue (the comment cites a 41 mm sink). Depression filling
here is drainage *enforcement* — it levels basins into rock. This confirms the
audit's F1 and the corrected architecture §4, and refutes the original §4
claim that the fill "creates" basins.

**F2 — The discarded basin raster is real and now preserved.** `bake_tile`
computes `basin_depth = out["filled"] - out["carrier_plus_roughness"]`
(`pipeline.py:3035`), reduces it to four scalars, and deletes it
(`pipeline.py:3058`). The audit's "one deleted line away" is right in spirit
and slightly understated in both directions: the raster was *already*
reconstructible via the stage sink (both `B2a.filled` and `B0B1.carrier_rough`
are in `STAGE_SINK_FIELDS`), and it measures the **B2a fill of the
pre-erosion carrier**, not the final surface — the distinction matters for §4.
As of this branch the raster is a first-class stage-sink field
(`B2a.basin_depth`) with a test tying it bit-exactly to the shipped stats
(work item 0, done).

Measured spread — and this is **two tiles, not a distribution;** work item 2
exists to fix that:

| tile | basin_cells_frac | basin_max_depth_m | max_accumulation_km2 |
|---|---|---|---|
| arid (-3,-3) | 1.70% | 42.4 m | 381.5 |
| wet (-2,-4) | 2.17% | 135.2 m | 84.3 |

**F3 — No water datum reaches the client.** The v2 fine tile carries exactly
four section ids — ELEV_INDEX/DATA, FLOW_INDEX/DATA (`tile_codec.py:144-149`,
mirrored `tilestore.h:125-130`) — one flag bit (`FLAG_FLOW_PRESENT`), and
rejects unknown flag bits (`tilestore.h:146-149`) and trailing bytes. The flow
plane itself is lossy: bits 0–4 are integer `log2(acc m²)` clamped to 31
(saturating at 2^31 m² ≈ **2,147 km²** — a hard ceiling on encodable
catchment), zeroed below `flow_mag_min_area_m2 = 1e4 m²` (`pipeline.py:757`);
the channel flag fires at `channel_area_m2 = 1e5 m²` or incision ≥ 0.25 m
(`pipeline.py:731-733,2081`). **So the 156 m² channel heads are carved into
the terrain but are *not* on the wire**, and `ITileSampler` (`tiles.h:41-60`)
cannot express flow at all — `pixelSizeMm/elevationMm/climate` only.

**F4 — `ChannelField` is built and wired to nothing.** Verified: references
are its own tests, bench, CMake, and docs; zero in `ue-project/`. It already
carries everything a river vessel needs: water-tight trapezoidal bed, bank
fill bounded at 4 m, graded strictly-descending bed, and
`waterLineMm = bed + depth·3/4` with banks constrained to contain it
(`channel.h:263-270,382-400`).

**F5 — The one shipping mechanism that turns a datum into water.**
`WaterMobilizer` (`waterca.h:951`): an `ImplicitFn(vx,vy,vz) → fill 0..255`
that must be a pure function of position and seed; unmobilized water is a
*wall* to the CA (`makeSolidFn`), mobilization is per-brick, one-way,
budgeted (64 bricks/tick), ledger-audited, persisted, and replicated
(authority mobilizes; clients `markMobilized` + fill-diffs). Production binds
it to cavern flood levels at `VoxelWaterSubsystem.cpp:177` (via
`Amp.columnCached`) and credits fill at `waterca.cpp:1727`. A baked lake or
river datum has **the same shape** as `floodZMm`. This plan builds no second
mechanism.

**F6 — Multiplayer ships server-authoritative fill-diffs.** `NM_Client` never
steps its own CA (`VoxelWaterSubsystem.h:18-23`, enforcement at `.cpp:3690`,
refusals at `:3190,:3821,:3846`); mirroring is
`MulticastWaterDiffs → ApplyReplicatedWaterDiffs/setReplicatedFill`
(`:3642,:3661`). ADR-0005 established water is not replayable in bounded
time, so late joiners need a snapshot regardless. This plan designs to that
shipped model, not to deterministic client replay.

**F7 — Perf reality.** The CA is a fixed 10 Hz step on the game thread;
ADR-0003 measured 6.5 ms/tick settled (127² lake) and ~30 ms on a large pour
— inside a 15.4 ms frame that is render-thread-bound at 2K (render 13.5–13.7
ms *is* the frame; game thread idle ~75%). Removed subsystem work does not
reliably become frame time. Conclusion: **maximise the baked share — content
does not tick and does not replicate** — and never grow the active CA set by
content.

**F8 — v23 is the precondition and it has landed.** Worldgen v23
(`claude/fine-micro-cap`, 0609be1; `kWorldGenVersion = 23`, `core.h:236`) caps
the summed detail pools so the client's detail band no longer re-strands the
ground (35.5% → 3.6% stranded on the worst clean site). Before it, water
puddled in micro-pits everywhere regardless of carved beds. This plan assumes
v23 (or later) is merged before any in-engine water milestone.

**F9 — Catchment truncation (task #49 / `HYDROLOGY_RESIDUALS` #7).**
Verified on `claude/model-backed-flow-parent` (commit 5fa0851): the pyramid
hands a child only the area **crossing that child's boundary**; area added
inside the child's own footprint is invisible to it. The model-backed parent
(3,932 km reach) changed shipped terrain by **exactly nothing** — 0 of
67,108,864 cells — on both tiles A/B-baked, because its deposit was still in
transit inside the destination's own footprint when the pyramid stopped
carrying it. Also measured there: a tile whose L0 raster shows 2,238 km² of
upstream area was handed only **384.5 km²** at the bake boundary. What is
safely claimable today: catchments up to the L0-block scale (61 km span; the
measured per-tile maxima on the three exemplar stat files are 84–382 km²)
reach the bake; substantially larger ones do not, and the flow-plane encoding
itself saturates at 2,147 km². §8 states what each milestone can deliver
under that ceiling.

**F10 — Sea level.** 14 unnamed `0` literals across four languages (audit §3
F5; spot-verified `rivernet.h:271`, `rivercouple.h` config,
`VoxelOceanActor.cpp`); `world_map.py:325` draws maps at −3.0 m; underwater
is `CameraZ < 0` with no terrain test; the ocean plane is 40 km,
camera-following, collisionless.

---

## 3. The data: what the bake must emit, and what it costs

### 3.1 New bake products

**P1 — The basin registry (per-tile table, ~KB).** Connected components of
`basin_depth > d_min` over the padded domain, filtered by minimum depth and
area (thresholds to be *set from the work-item-2 survey*, not guessed here;
the intent is "a basin a player would notice", order 2 m deep / 50 m across).
Per basin:

    basin_id       u16   stable within tile (ordered by (min_y, min_x) of extent — deterministic)
    seed_px        2×u16 deepest cell (client flood-fill seed)
    bbox_px        4×u16
    spill_mm       i32   fill level of the basin on the FINAL surface (see §4.2)
    surface_mm     i32   equilibrium water surface from the water balance (§4.3); == spill_mm when overflowing
    kind           u8    0 dry-playa / 1 salt-flat / 2 seasonal / 3 lake-terminal / 4 lake-overflowing
    outlet_px      2×u16 spill cell (the head of the outlet channel), valid for kind 4
    reserved       u8[3] zero

~24–32 B per basin, expected tens of basins per tile → **≤ ~2 KB per tile**.

**P2 — The water-surface plane (per-pixel, phase 2).** `water_surface_mm` as
int16 control points in the elevation plane's own encoding (quant 100 mm,
`base_offset_mm`-relative), with an all-dry sentinel so dry blocks encode as
`MODE_CONSTANT`. Covers river reaches (graded, strictly descending along the
carve, from the same discharge→depth law as the carve itself) *and* lake
surfaces (constant per basin), so the client has one uniform query. Lakes
alone do **not** need this plane — extent is derivable client-side from
`ground < surface` flood-filled from `seed_px` — which is why P1 ships first
and P2 ships with rivers.

**P3 — Kept basins in the elevation plane itself** (§4.2): not a new section,
but the elevation bytes change — the bake stops levelling registered basins.

### 3.2 Wire format

New sections and flags in `.vxtl` v2, additive exactly as the format was
designed for (section table + flags word):

    SECTION_BASIN_TABLE = 5      FLAG_BASINS_PRESENT = 1 << 1     (phase 1)
    SECTION_WATER_INDEX = 6      FLAG_WATER_PRESENT  = 1 << 2     (phase 2)
    SECTION_WATER_DATA  = 7

Both ends must move in lockstep (`tile_codec.py` + `tilestore.h`/`.cpp` +
`docs/vxtl-v2-format.md` + a committed conformance fixture), because unknown
flag bits are **rejected** (`tilestore.h:146-149`) — an old client refuses a
new tile loudly rather than mis-reading it, and `fine_provider_id` covers
`BAKE_VERSION`, so mixed-version worlds are already caught by
content-addressing.

**Cost.** Measured baseline: 185.2 MB raw / 26.6 MB zstd (7.0×) on tile
(0,0); 201.4/33.4 (6.0×) on (-5,2) — ratio varies with terrain. P1 is noise
(≤2 KB). P2 adds 8192²×2 B = **+134 MB raw**, but compresses to almost
nothing where dry-constant: wet cells are ~2% (basins) plus channel cells
(3.62 M = 5.4% on tile (0,0)), and the wet values are smooth → estimate
**+0.5–3 MB zstd per tile**; measure before freezing. Two consequences:
P2 should not ship while production tiles are raw — which makes **task #43
(pregen `--codec` is done; the client-side zstd decode is not,
`VoxelTileCodec.h` records `kNoDecompressor`) a hard dependency of phase 2**
— and P1 has no such dependency.

**Every one of P1–P3 bumps `BAKE_VERSION` (currently 7, `pipeline.py:270`)
and invalidates baked tiles.** Right now that is nearly free — the cache
holds 17 tiles (commit abbde5f). Batch the bumps: P1+P3 together in one bump,
P2 in a second.

### 3.3 Client-side interface

`ITileSampler` stays what it is (three virtuals, many implementors, an HLSL
mirror discipline). Add a parallel narrow interface rather than widening it:

```cpp
// tiles.h — new, alongside ITileSampler
class IWaterSampler {
public:
    virtual ~IWaterSampler() = default;
    // Water-surface elevation at a tile pixel, or INT32_MIN for dry.
    virtual int32_t waterSurfaceMm(int64_t px, int64_t py) = 0;
    // Flow byte (vxtl §6 layout) — finally giving the shipped plane a consumer.
    virtual uint8_t flowByte(int64_t px, int64_t py) = 0;
};
```

`FineTileSampler` implements it from SECTION_BASIN_TABLE (phase 1: flood-fill
extents once per tile, cached bitmap per basin bbox) and SECTION_WATER
(phase 2: direct). The missing-tile answer is **dry**, and the
`FVoxelFineTileStreamer` residency gate (block-until-ready,
`VoxelFineTileStreamer.h`) already guarantees no chunk generates over a
non-resident footprint, so "dry because not loaded" can never be voxelized
into a desync.

---

## 4. The algorithms

### 4.1 Source points and river growth (against fields that already exist)

The bake already computes the entire drainage cascade; the algorithm here is
selection, not simulation.

1. **Discharge, one currency.** Define `Q(cell) = Σ_upstream runoff(c) ·
   cell_area` in m³/yr, computed in the bake by running the existing MFD
   accumulation with a **runoff-weighted** source term instead of the
   constant unit area: `runoff(c) = budyko(P(c), PET(T(c)))` from the climate
   planes the bake already assembles (`assemble_padded_climate`). This
   reconciles the three currencies in play today (bake: m²; rivernet: summed
   mm/yr, threshold 23529 `rivernet.h:241`; channel.h: rivernet's units) —
   everything downstream reads Q.
2. **Water heads.** A source point is the most-upstream cell of a reach where
   `Q ≥ Q_perennial` (order 10³–10⁴ m³/yr·km²-scale; calibrate on the survey,
   work item 2). This is deliberately *not* 156 m²: that is where the bake
   carves a swale, which in most climates is a dry gully most of the year.
   The consequence is exactly the caused variety the owner wants: in a wet
   province the water starts high in the network and the map is dense with
   brooks; in an arid one only trunks carry water between dry carved washes.
   Distributed origin points fall out of the field — no explicit "pick N
   points" step exists or is needed.
3. **Width/depth/waterline.** Reuse `channel.h`'s laws verbatim (width ∝
   Q^0.4, depth ∝ Q^0.35, caps 400 m / 25 m, waterline = bed + ¾ depth,
   monotone in Q by construction) — re-anchored on `Q_perennial` instead of
   `kRiverAccumThresholdDefault`. The bake evaluates them per channel cell to
   produce P2's graded water surface, enforcing strict downstream descent
   along the D8 tree with the existing `enforce_descent` machinery, so a
   reach can never be a chain of puddles at tile scale (`channel.h:111-134`
   is the argument; the bake already owns the topological-order sweep).
4. **To the sea.** Nothing new: routing already terminates at sea level with
   the sea taper (`pipeline.py:2077-2079`), and the carve's mouth opens below
   z=0. The water surface plane simply follows the carve down and stops at
   `kSeaLevelMm` (§6.4).

**Sub-pixel headwaters, stated honestly.** A channel at initiation is ~1.5 m
wide (`channel.h:234`) — narrower than the 1.875 m pixel. Phase 2 puts water
only on reaches wide enough for the raster to hold a wet bed (~2 px, ≈4 m);
above that line the carved swales stay dry (they already exist as landform).
If headwater brooks matter later, they are a detail-band term (the
`detail_rill.h` precedent), and the bank-integrity probe (work item 7) gates
whether the idea is viable at 10 cm scale. Not in this plan's committed
scope.

### 4.2 Lakes: keep the hole

The pipeline change, minimal against what exists:

1. B2a fill, routing, incision, thermal, meso, B4b all run **unchanged** on
   the filled surface. This is hydrologically right — water flows *across* a
   lake at its surface, so the flow plane, incision (which cuts the outlet
   gorge through the rim and a thalweg across the lake floor — both correct
   and both desirable), and every downstream consumer stay consistent, and
   the flow plane keeps agreeing with the carve cell-for-cell.
2. **New final step (B5): re-open registered basins.** For basins passing the
   registry filter: `z_out = z − basin_depth` over the basin mask.
   `basin_depth` is continuous and 0 at the rim, so no seam. Then recompute
   the **final** spill level per basin with one more `fill_depressions`
   restricted to registered-basin cells, because incision may have lowered
   the rim since B2a — `spill_mm` must come from the surface that ships, not
   the carrier.
3. **The drainage contract is restated, not broken.** New contract: *the
   carrier drains once every registered basin is virtually filled to its
   recorded `spill_mm`* — i.e. `fill(z_out with basins clamped to spill)` has
   0 sinks, and every real sink lies inside a registered basin. Asserted in
   tests exactly as the old contract was. Dry basins (playas) violate "0
   sinks" **on purpose** — an endorheic depression not draining is the
   feature, and the registry is what distinguishes intent from bug.
4. **Tile-spanning basins are excluded in v1.** The bake already measures
   `basin_reaches_padded_border` (`pipeline.py:3096`); a basin touching the
   padded border has its fill level invented by the domain boundary
   (`HYDROLOGY_RESIDUALS` #6: measured 78.79 m of fill-level error from
   exactly this). v1 registers only interior basins and *counts* the
   excluded ones per tile so the cost of the exclusion is a number. The v2
   fix is #6's own: a fill boundary condition taken from the superblock's
   shared `filled` raster.

### 4.3 The lake rule: a water balance, not a roll

Inputs, all already on or beside the tile: basin hypsometry `A(h)` (from
`basin_depth`), catchment area / runoff-discharge `Q_in` at the spill cell
(from §4.1's accumulation — the basin's whole upstream network contributes,
which is "populate water throughout the watershed" applied to lakes), precip
`P` and temperature `T` from the climate planes (bio_12, bio_1), seasonality
from bio_4/bio_15.

    PET  = pet(T)                      # pinned monotone table (Langbein-class), deterministic
    h*   = the level where inflow balances lake-surface loss:
           Q_in + P·A(h) = PET·A(h)    # solve on the hypsometric curve; A(h) monotone ⇒ unique
    surface_mm = min(spill_mm, h*)

    kind: h* ≥ spill              → 4 lake-overflowing (outlet carries the residual; it is
                                      already in the flow plane because routing ran filled)
          h* < spill, A(h*) > 0   → 3 lake-terminal (endorheic; salty over geologic framing)
          A(h*) ≈ 0, PET >> P     → 1 salt-flat   |  bio_15 high & marginal → 2 seasonal
          otherwise               → 0 dry playa

The exact `pet()` and `budyko()` tables are tunables to be fitted once
against the survey (work item 2) and then pinned; the *structure* — same
depression, wet climate ⇒ lake, desert ⇒ playa, and the player can trace the
inflow that explains it — is the decision this plan commits to. Randomness
adds nothing here and would break the "see why" property; if the owner wants
extra variety, the causal knobs (climate field, hypsometry) already produce
it. **At overflow, the spillway is part of the drainage network by
construction**, not by a special case: the outlet channel below `outlet_px`
already carries the basin's full upstream area in `acc`, because
accumulation was computed on the filled surface.

### 4.4 Where the runtime simulation fits

Baked = equilibrium. Runtime = deviation from equilibrium, only where players
cause it. The CA/mobilizer own deviation (dig, dam, drain, pour); `rivernet`/
`rivercouple` become the *edit-response* layer (§6.3), not the water-placement
layer. Nothing populates watershed water at runtime — that is the load-bearing
difference from the parked W3 attempt, which injected discharge from a locally
re-derived 1.44 km graph onto unmodified hillside and produced disconnected
puddles.

---

## 5. The client path

### 5.1 Placement: one new ImplicitFn, zero new mechanisms

At the existing binding site (`VoxelWaterSubsystem.cpp:177`), compose the
mobilizer's implicit field:

```cpp
[this](int64_t vx, int64_t vy, int64_t vz) -> uint8_t {
    // Cavern water, exactly as today.
    if (vxc::cavernFloodedAt(Amp.columnCached(vx, vy).cavern, vz)) return 255;
    // Baked surface water: open air between the amplified ground surface and
    // the baked water surface. Ground bound excludes caves under the lakebed;
    // the mobilizer's own terrain half re-checks solidity per cell.
    const int32_t surfMm = WaterSampler->waterSurfaceMmAtVoxel(vx, vy); // dry ⇒ INT32_MIN
    const int32_t zMm    = int32_t(vz) * vxc::kVoxelSizeMm;
    if (zMm < surfMm && zMm >= Amp.columnCached(vx, vy).surfaceMm) return 255;
    return 0;
}
```

Everything else is inherited, tested, and shipping: unmobilized lake water is
a wall; digging the shore fires the existing edit hook
(`NotifyTerrainVoxelsCleared` → `mobilizeEditRegion`, the same funnel that
already handles caverns and the ocean breach at `VoxelWaterSubsystem.cpp:3838
ff.`); the front advances a brick-shell per tick, budgeted; fill replicates
as diffs; mobilized keys replicate; the ledger audits to zero. The lake
surface is **flat in tile space and indifferent to the detail band** — the
datum is authoritative, the amplified ground merely bounds it from below
(v23 keeps the shoreline band sane, F8).

Partial-fill nicety: the topmost water voxel should carry fractional fill
(`surfMm % 100`) so the surface sits at the datum rather than snapping to
10 cm; the CA's fill units express this natively.

### 5.2 Rendering, by LOD band

| band | ground today | water |
|---|---|---|
| R0 voxel ring (≤ ~1 km) | voxel chunks | CA water via the existing pool path where mobilized; implicit water via `RefreshImplicitWater` (already sweeps a 52×52×26 m disc around the camera each tick and will pick up lake/river cells the moment the ImplicitFn includes them — its 192-brick/tick budget already bounds it) |
| clipmap bands (~1–16.4 km) | heightfield (`VoxelClipmapActor`, 30 m/px) | **new: water sheet** — flat translucent mesh per visible basin (from the basin table: bbox + surface elevation; phase 2: same clipmap machinery sampling the water plane for rivers). No collision, no tick, no replication. This is the audit's §7.1 rule applied: surfaces at range, voxels only near |
| beyond | — | ocean plane (existing), see §6.4 |

The near/far handover mirrors the terrain's own: the sheet is clipped out of
R0 exactly as the clipmap's inner hole already lands on the ring edge.
Translucent overdraw is the render-thread risk (F7); the sheet adds draw
calls proportional to *visible basins*, not world size, and work item 5's
gate is a measured `VoxelPerfRun post-warmup` frame, not a component timing.

### 5.3 Being in it

Underwater/swim tests must consult actual water (CA fill, implicit field via
`implicitFillAt`, or the datum) instead of `CameraZ < 0`
(`VoxelOceanActor.cpp:133-159`, `VoxelCharacterMovement.cpp:602`). This is
work item 1 and deliberately early: every visual judgement of every later
milestone is contaminated while a dry pit below z=0 renders as ocean.

---

## 6. Multiplayer and edits

Invariants kept, from F6 and doctrine §4-§5: one authority orders all world
changes; the CA replicates as fill-diffs to mirroring clients; baked content
replicates as **nothing**; the water datum joins tile identity via
`BAKE_VERSION` → `fine_provider_id`, so a client with a different lake table
is refused by content addressing instead of desyncing quietly.

| edit | mechanism | new work |
|---|---|---|
| **mine a lake shore / drain a lake** | existing: edit → `mobilizeEditRegion` → CA drains through the cut, diffs replicate, front self-limits | none for the session; **ADR-0005's UE-side hook** (open) for "stays drained across reloads" — without it the implicit field refills on load. Same blob doubles as the late-joiner snapshot |
| **mine a channel between basins** | same as above — water follows the cut because the CA routes it; when it settles in the lower basin it *stays CA water* (mobilization is one-way) | acceptable v1; a "re-settle to datum" compaction (CA → implicit demotion for settled bricks matching a datum) is a later optimisation, listed but not required |
| **dam a river** | v1: player builds a wall; nearby water is already mobilized (their edits touched it), CA pools behind the dam locally — real, visible, replicated | v2: a detector from sustained blockage to `kSetConveyance`, the diff **into the edit log** (they are world changes; `rivernet.h:127-149` designed them to replay byte-identically), and downstream drying driven by graph stage as a bounded per-reach *override* on the implicit datum. The graph must be built from the **baked flow plane** (`buildFromFlowPlane`, new), not `buildFromFlowAccumulation`'s local D8 — see §9.3 — and persisted (today it is per-client, 1.44 km, unpersisted) |
| **divert a river** | `kDivertChannel`/`promoteChannel` exist and replay; the flux detector exists in `rivercouple.h` | same edit-log routing; enable only after v2 graph work |
| **place water** | `SpawnWaterAt` funnel exists, authority-checked | unbind the `1` key from `voxel.Water.BucketFill`; make it an item with a budget |

SWE stays exactly where it is: standalone-only, non-authoritative, cosmetic
on top (`VoxelWaterSubsystem.cpp` refuses it off `NM_Standalone`).

### 6.4 Ocean

Item 1 makes sea level one symbol (`kSeaLevelMm = 0` in `core.h`, mirrored to
Python and HLSL by the existing constants-dump mechanism; `world_map.py:325`
fixed to draw at it). The ocean then becomes the third term of the same
ImplicitFn — `z < kSeaLevelMm` in open air — which unifies breach behaviour
with lakes (the bespoke reservoir top-up at `:3307-3315` can retire once
proven equivalent), gives the sea real voxels only where touched, and makes
"the plane shows through inland pits" fixable by testing the datum, not the
camera. The 40 km visual plane stays for the far field.

---

## 7. Performance case

* **Content is free at runtime.** An untouched lake: 0 bytes on disk beyond
  the tile, 0 bytes on the wire, 0 CA bricks, 0 ticks. Identical economics to
  the shipped cavern lakes. The active set stays bounded by player activity,
  never world size — the same property the edit log gives terrain.
* **The CA budget is the guard rail, and it already exists** (front budget 64
  bricks/tick; catch-up cap 4 steps). Work item 10 adds the missing half:
  budget or off-thread the step *before* any feature grows the mobilized set;
  ADR-0003's 30 ms pour inside a 15.4 ms frame is a hitch every 100 ms if
  ignored.
* **Water at range is surfaces, not voxels** (§5.2). Translucency is the
  frame's scarcest resource at 2K; sheets add bounded overdraw, voxelized
  lakes to the horizon would be the second full-world pass the owner is right
  to fear.
* **Measure the frame, not the component** — `VoxelPerfRun post-warmup`, with
  the worldgen version stated beside every number. Seven predictions were
  falsified in one day by skipping this.

---

## 8. Dependence on task #49, stated plainly

The algorithms in §4 are **independent of #49** — they consume `acc`
whatever its reach. What #49 caps is how *large* the largest river can be:

* **Unaffected before #49:** all of lakes (basins are local; their catchments
  are tile-scale and mostly resolved already — the wet exemplar's largest is
  84 km²), all headwater and mid-size rivers, the entire client path, edits,
  multiplayer. This is most of the visible value.
* **Capped before #49:** trunk rivers. Delivered catchment tops out around
  the L0-block scale (61 km span; measured tile maxima 84–382 km²; one
  instrumented case showed a 2,238 km² L0 catchment delivering only 384 km²
  to the bake). Through the §4.1 law that is a river of roughly 30–70 m
  width — a serious river, not a Mississippi. Rivers will also *stop growing*
  beyond that scale rather than taper wrongly: width is monotone in delivered
  Q, and delivered Q plateaus.
* **When #49 lands** (boundary-condition injection or interior hand-off so a
  parent's area survives inside a child's footprint — plus the wire ceiling:
  the flow plane's 5-bit log2 saturates at 2,147 km² and would need widening
  in the same bump): re-baking picks up continental rivers with **no change
  to anything in this plan**. Design decision taken now to stay compatible:
  everything reads Q, nothing assumes a maximum.

Honest unknown: the *exact* delivered-catchment ceiling today is not
determinable from the record (one instrumented tile, three stat files); work
item 2 reports the spread.

---

## 9. Where this plan disagrees with the existing documents

1. **Audit item 3 ("the drainage bar on the amplifier") is done** — v23, F8.
   This plan starts where the audit's pivotal item ends, which is why its
   ordering below differs from the audit's.
2. **"The raster is one deleted line away" (audit §3 F1 / item 1)** —
   directionally right, but the raster was already reconstructible from the
   stage sink, and more importantly it describes the **pre-erosion** B2a
   fill; the shipped lake needs the *final-surface* spill recomputation of
   §4.2.2, which no existing text mentions. Preserving the raster is done
   (item 0); the "one line" framing understated the remaining work.
3. **Audit item 5 ("wire `ChannelField` into worldgen") — partial
   disagreement, the substantive one.** `RiverNetwork::buildFromFlowAccumulation`
   re-derives accumulation with a local D8 over its own region bounds
   (`rivernet.h:30-63`) — no superblock inflow, no MFD, no sea taper. Wiring
   `ChannelField` on top of that would re-import catchment truncation at
   1.44 km scale (far worse than #49) and would disagree with the carve the
   bake shipped, cell for cell. **Reuse `channel.h`'s geometry and laws;
   do not reuse rivernet's build for production rivers.** The discharge
   authority is the baked flow field; rivernet gets a `buildFromFlowPlane`
   and keeps its routing/dam/divert role (§6.3). The graded-waterline
   computation itself moves into the bake (P2), where the non-local work is
   already paid — consistent with `channel.h`'s own header argument that
   non-local reasoning belongs in a build step, not in `evalSurface`.
4. **The brief's "source points: `channel_init_area_m2` (156 m²) — the bake
   already knows where every stream starts"** — true for the *carve*, but
   156 m² is not a perennial-water criterion, and those heads are not even on
   the wire (flow magnitude zeroed below 1e4 m², `pipeline.py:744-757`).
   Water heads need the small new climate-driven criterion of §4.1.2 — which
   is also what makes stream density *caused* rather than uniform.
5. **Architecture §5's original claim (clients re-simulate deterministically)**
   — already corrected in the working tree on 2026-08-03; this plan designs
   to the shipped fill-diff model and endorses the correction. Note the
   corrected text currently exists only as an uncommitted edit to
   `docs/world-generation-architecture.md` in the shared checkout; it should
   be committed.
6. **Numbers refreshed:** compression is 7.0× (185.2→26.6 MB, tile (0,0)) not
   just the 6.0× the architecture doc records; `kWorldGenVersion` is 23, not
   the 10→11 the audit cites; wet-exemplar basin stats now have an arid
   counterpart (F2 table) — still only two tiles, which is a gap, not a
   distribution.

---

## 10. The work, cheapest first

Each item: what it unblocks, and how it is verified. Items 0–2 spend nothing
on format or engine; 3–5 are the first playable; 6–10 complete the system.

**0. Preserve the basin raster — DONE on this branch.**
`basin_depth` is now a first-class `bake_padded_domain` output and stage-sink
field (`B2a.basin_depth`), bit-tied to the shipped stats by test
(`test_stage_sink_observes_every_sub_stage_and_changes_nothing`). No baked
byte changes. *Unblocks: item 2. Verified: extended test + full suite.*

**1. One sea-level symbol; honest underwater/swim tests; map waterline.**
(Audit items 0+2, merged.) `kSeaLevelMm` in `core.h`, mirrored via the
constants-dump; `world_map.py:325` → 0; underwater/swim consult water, not
`CameraZ`; fix the dead `$Rivers` switch in `tools/water-playtest.ps1`.
*Unblocks: every later visual judgement. Verified: unit tests + a capture of
a dry below-sea pit no longer tinting.*

**2. `tools/lake_survey.py` — the sizing instrument.**
Per-basin components, hypsometry, spill, climate water balance (§4.3) run as
a *report* against stage-sink dumps; overlay renders per the standard
world-generation deliverables (call the real classifiers). Bake ≥6 exemplars
(wet/arid/alpine/plains at minimum) and report **spread**: basins per tile,
lake vs playa fractions by province, depth/area distributions, spanning-basin
exclusion counts. Fit and pin `pet()`/`budyko()` here.
*Unblocks: every threshold in items 3 and 7; the format cost estimate.
Verified: synthetic-bowl unit test (balance flips lake→playa across a climate
sweep) + committed overlays.*

**3. Bake: keep the hole + basin table (P1+P3, one `BAKE_VERSION` bump).**
§4.2 re-open, final-surface spill, restated 0-sinks contract, registry
filter, SECTION_BASIN_TABLE in both codecs + fixture.
*Unblocks: items 4–6. Verified: new contract tests (fill-to-spill ⇒ 0 sinks;
excluded-spanning-basin counts); codec round-trip both languages; same-ground
A/B hillshade of a kept basin (visual bisection convention); suite green.*

**4. Client: decode + lake ImplicitFn — FIRST PLAYABLE (§11).**
`IWaterSampler`, extent fill from seed, composed ImplicitFn (§5.1), partial
top fill. *Unblocks: seeing it. Verified: §11's script — capture + dig-drain
leg + ledger/shortfall assertions.*

**5. Lake sheets in the clipmap bands + swim/underwater against the datum.**
*Unblocks: lakes at range; honest shorelines in vistas. Verified: capture at
2–10 km; `VoxelPerfRun post-warmup` frame delta measured, not predicted.*

**6. ADR-0005 UE-side hook.**
Write the water blob + mobilized set beside the edit log; load-back;
missing/stale policy. *Unblocks: "drained stays drained"; the late-joiner
snapshot. Verified: drain → save → reload leg; digest round-trip.*

**7. Rivers: runoff-weighted Q, water heads, graded water plane (P2), second
`BAKE_VERSION` bump; bank-integrity probe first.**
The probe (voxel-scale leak count along baked channels under v23 detail — the
`channel.h` bank-test precedent, 7763→0) decides whether the baked carve
holds its waterline or needs a detail-band bank term; run it *before*
building P2. Client: river water through the same ImplicitFn; sheets extend
to river reaches. Depends on #43 (client zstd) for shippable size.
*Unblocks: the directive's rivers. Verified: probe ≈0 leaks on exemplar
reaches; walk-the-river capture series source→mouth; spread of wet-reach
fraction by province.*

**8. Ocean unification (§6.4).** *Verified: breach parity test old-vs-new
path; inland-pit capture.*

**9. Hydrology graph v2: `buildFromFlowPlane`, persistence, dam detector,
graph diffs in the edit log; downstream-drying overrides.**
*Unblocks: dam/divert as ordered, replayable gameplay. Verified: rivernet
replay goldens against the new build; two-client dam leg — upstream rise,
downstream decay, byte-identical replay.*

**10. Perf gates: CA step budgeted/off-thread before the mobilized set can
grow; sheet overdraw measured.** *Verified: frame measurements under a
scripted mass-mobilization worst case.*

Parallelism note: items 1, 2 are independent; 3 blocks 4; 6 is independent of
3–5 and can proceed anytime; 7+ are strictly after 4 proves the path.

---

## 11. How we see it working: the first playable milestone

**What ships:** items 0–4. **What the owner does:** launch the editor game on
seed 20260719 with `-VoxelFineTileDir` at the re-baked cache, at a spawn site
chosen by `lake_survey` from the wet exemplar's basin registry (deepest
interior basin with `kind ≥ 3`; the survey prints the spawn command — site
coordinates are an output of item 2, not a guess in this document).

**What he should see, in order:**

1. From the shore: a lake — a flat water surface at the baked datum, meeting
   terrain at a shoreline that follows the contour, in a basin whose inflow
   valley is visible upslope. Not placed: the survey overlay shows the same
   basin as a lake for the same climate reasons.
2. Walking in: water voxels in R0 (implicit → meshed by the existing
   `RefreshImplicitWater` path), surface at the datum, not snapped to 10 cm.
3. **The interaction that proves the system:** dig a trench at the rim. The
   wall mobilizes, CA water pours through the cut, the front advances shell
   by shell down the outlet slope, and the lake level visibly drops near the
   breach — on a listen server, the second client sees the same water via
   fill-diffs. (Level equalisation across the whole lake is CA-rate-limited
   and slow at first; the near-breach drawdown is the demo.)
4. The same basin type in the arid exemplar is a dry playa — the A/B that
   shows "caused, not placed".

**Evidence conventions:** `tools/voxel-capture.ps1` with its settle counters
and frozen sun (12:00, `-TimeScale 0`), before/after captures for the breach;
a scripted leg through the sanctioned harness for the dig-drain sequence with
the mobilizer ledger (`shortfallVolume() == 0`) and diff-byte counters
asserted; screenshots presented to the owner with conditions attached, no
verdicts. What is *not* in this milestone: rivers (item 7), persistence of
the drained state (item 6), swimming (item 5/8).

---

## 12. Summary

The bake already knows where every channel starts, how rivers grow, how wide
and deep they should be, and how they reach the sea; it discards the two
things the player needs — a surviving basin and a water datum on the wire.
The runtime already knows how to turn a deterministic water datum into
touchable, replicated, persisted water; it is fed a hash instead of
hydrology. This plan connects the two along the shortest path that respects
the constraints that killed the naive versions: server-authoritative
fill-diffs stay, content stays free, lakes are a water balance rather than a
roll, the CA never grows by content, and every step is individually
verifiable in UE5 before the next is funded.
