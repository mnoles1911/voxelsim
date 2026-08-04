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

**Correction, 2026-08-03 (work item 4).** Two claims in the R0 row above are
wrong, and both were found by trying to photograph a lake rather than by
reading:

1. "`RefreshImplicitWater` ... will pick up lake/river cells the moment the
   ImplicitFn includes them" — it does not consult the ImplicitFn at all; it
   re-derives the ceiling itself and skipped cavern-less columns. Fixed at
   `90686fa`.
2. "its 192-brick/tick budget already bounds it" — the budget bounds the
   brick COUNT, which was never the cost. A brick cost three full amplifier
   columns per padded cell (1,000 of them) because the pad was built
   z-outermost and every memo under it holds exactly one column; and 89% of
   the bricks offered were lake interior that emits no faces by design. One
   refresh of a 19.6 m lake was 38,025 candidates and 199 ticks, so every
   capture landed mid-rebuild and photographed a partial sheet. Fixed at
   `708f5a4`: 4,225 candidates, 23 ticks, 0 empty.

**And a third thing this section does not account for at all: a still water
surface has no SURFACE.** It renders — it always did — but as a flat tinted
film with no view-dependent term, which reads as haze over the ground rather
than as water.

**Correction to what this row said before (W6).** It previously claimed that
vertex colour A — the foam activity — "is what makes water visible in
`M_WaterVoxel`", so that an `Activity = 0` sheet drew nothing. That is not what
the graph does. Foam is a `LinearInterpolate` layered on top of an already
complete colour and opacity, so with foam at 0 the material still outputs
`lerp(0.55, 0.72, depth01)` opacity — it cannot reach zero. The A/B that
produced the claim compared `Activity = 0` against `Activity = 1` and **never
took a no-water control**, so "different from the foamed frame" was read as
"absent".

The control was taken at W6, same pose, same settle, same frozen sun, with
`M_WaterVoxel` reduced to `Opacity = 0`:

| frame | mean RGB | vs no-water control |
| --- | --- | --- |
| no-water control | (215.6, 207.0, 196.2) | — |
| `Activity = 0` (shipped) | (155.6, 152.5, 149.6) | mean abs diff **60.0**, 99.99% of pixels >25 |
| `Activity = 1` (foam) | (207.0, 200.5, 190.1) | mean abs diff 21.9 |

The still sheet darkens the whole basin by ~60 levels. It is the **`Activity = 1`
frame that is closer to bare terrain**, because the foam is near-white and so is
this terrain. Two further facts from the same wave: an unlit probe material
proved the sheet rasterises and fills the frame, and that probe read back
`VertexColor.A = 0.000`, `AO = 0.50`, `depth01 = 0.55` — so `Activity` does
arrive as 0, and nothing was ever gating visibility on it.

**The real fix, and what landed.** Still water needed a view-dependent term, not
a foam-independent one — it already was foam-independent. `create_water_voxel_material.py`
now gives it a Fresnel-weighted sky reflection and an analytic sun glint off
`MPC_VoxelSky.SunDirection`, with the same Fresnel folded into opacity so the
surface is transparent looking down and closes up at a grazing angle. Foam
remains a lerp on top and now also suppresses the reflection, since froth does
not mirror. Measured at the same site, the far-versus-near blueness contrast
that a surface produces goes from **+1.7 to +13.0**; the steep foreground is
essentially unchanged, which is the point of a Fresnel term.

**Still true, and still the reason this is wider than lakes:** every implicit
water surface passes `Activity = 0`, so cavern pools took the same flat-film
appearance and get the same surface now. The new terms carry no `Activity`
dependence at all. **Not verified in a capture**, though: `-VoxelFloodTest` found
no flooded cavern at the lake site, and the default cavern site's fine tiles are
absent from this box's cache (tile `(-6,3)` at `s16`, `absentOnDisk=1`).
Downgrading the fine-tier gate would have bought a frame at the cost of its
reproducibility. One authored limitation goes with it: the sky term is gated on
sun altitude, not on sky visibility, so an underground pool reflects a sky it
cannot see — Fresnel-weighted, so near zero when looked down into.

Work item 4's rendering half is now an owner judgement on the W6 captures, not
an open defect; work item 5 stays unstarted behind that judgement.

**Correction, 2026-08-03 (work item 5). The clipmap-band row above understated
the gap by two orders of magnitude, and the fix is wider than "the clipmap
bands".** The row reads as though R0 covers the near field and the clipmap
covers 1–16.4 km, leaving water missing only at range. It does not. R0's water
is `RefreshImplicitWater`'s **52 m disc**, and the voxel ring cascade runs to
4 km — so water was absent from **26 m outward**, over ground that is voxels,
not clipmap, for the first four kilometres of it. Basin 1 of tile (-12,-5) is
2.0 × 2.4 km; 99.9% of it could never be drawn at any pose. `AVoxelWaterSheetActor`
therefore draws sheets from the implicit disc's own edge outward, not from the
clipmap's inner hole.

**What was measured before building it, because the sheet would have been
pointless otherwise.** The clipmap samples the COARSE tier (`Impl->Tiles`,
30 m/px), which is the diffusion model's own output — the bake's B5 re-open
writes the hole into the FINE tile only. So the far terrain need not have
contained the basin at all, and a sheet at the datum would have been buried.
Measured over basin 1's 801,409-cell extent (which reproduces the registry's
281.7 ha exactly): coarse ground runs a median **5.1 m below** the datum, and
only **0.9%** of the lake's area sits above it; through the clipmap's actual
256 m vertex lattice, median 4.5 m below and **6.2%** above. The bowl survives
into the coarse tier. That was not a foregone conclusion and is the reason
work item 5 is buildable without a fine-tier far field.

**The handover is a rectangle subtraction, not a fade.** Sheet and voxel water
are coplanar at the datum, so an overlap is a z-fight *and* a doubled
translucent blend, and a gap is a ring of missing water. `vxc::subtractRect`
cuts the implicit disc's exact footprint out of the sheet, and the cut is
applied only when that disc is actually meshing water at that basin's datum —
the disc is bounded in z as well as xy, so a camera 30 m above the lake has no
near-field water to hand over to and must not have a hole cut for it.

**Measured frame cost, at the pose the 2 km capture was taken from**
(2560×1440, static perf leg, post-warmup, two runs per config):

| | p50 ms | p95 ms |
| --- | --- | --- |
| sheets ON | 7.819 / 7.860 | 8.780 / 8.767 |
| sheets OFF | 7.803 / 7.811 | 8.671 / 8.633 |
| **delta (means)** | **+0.032** | **+0.121** |

Same-config spread is 0.041/0.008 ms at p50 and 0.013/0.038 ms at p95, so the
p50 delta is inside the noise and the **p95 delta is not**: the sheet costs
about **0.12 ms at p95**, 0.7% of a 16.7 ms budget, in a frame where the lake
covers roughly 30% of the pixels. §7's "sheets add bounded overdraw" is now a
number rather than a claim. The water rebuild was re-measured in the same wave
and did not regress: 4,225 candidates, 23 ticks, 1,812 ms.

**Two things the captures show that are worth knowing before judging them.**
First, the sheet is ONE translucent surface where the near field is a meshed
shell whose faces blend more than once, so the same material reads lighter
across the seam — measured at the handover, blueness +44.5 on the sheet side
against +76.8 on the voxel side, with no gap between them. Second, the 10 km
frames contain a large blue wedge in the mid-ground that is **present in the
no-sheet control** and is therefore not this feature: it is the pre-existing
clipmap inner-hole seam, and it would have been reported as a sheet bug if the
control had not been taken.

**§5.3 is done and was verified in-engine rather than by reading.** The
underwater/swim predicate needed no new code — item 1 routed it through
`IsUnderwaterAtWorld` and item 4 put the lake datum into `implicitFillAt` — but
"needs no change" is a claim, so it was photographed: a camera at z = 356.3 m,
over worldgen ground at 345.5 m, with sea level at 0, logs `Ocean: camera
entered water`. Nothing but the baked datum can return true there; the old
camera test and `IsOpenSeaAtWorld` both say dry.

**What work item 5 does NOT do.** The scan radius is 10 km (the range this
section's own verification names), because a fine tile holds its whole
compressed `.vxtl` resident and the clipmap's 65.5 km half-extent would have
asked for ~81 of them, ~3 GB. Basins beyond 10 km draw no sheet: a bounded,
logged absence rather than an unbounded load. `-VoxelLakeSheetRangeM` moves it.

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
| **mine a lake shore / drain a lake** | existing: edit → `mobilizeEditRegion` → CA drains through the cut, diffs replicate, front **does *not* self-limit — see §6.3 M2** | none for the session; **ADR-0005's UE-side hook is DONE** (`VoxelWaterSubsystem.cpp:2481, 2513`, wired at `:847, :890`), so "stays drained across reloads" already holds. The blob is *not* yet the late-joiner snapshot: join-sync carries the compacted edit log only, and water arrives over subsequent `MulticastWaterDiffs` rounds (§6.3.7) |
| **mine a channel between basins** | same as above — water follows the cut because the CA routes it; when it settles in the lower basin it *stays CA water* (mobilization is one-way) | acceptable v1; a "re-settle to datum" compaction (CA → implicit demotion for settled bricks matching a datum) is a later optimisation, listed but not required |
| **dam a river** | v1: **nothing happens.** Measured, §6.3 M1: a wall across an undisturbed baked river settles in 1 tick and the upstream stage does not rise by one voxel, because a baked river is a *datum*, not a flow, and the untouched part of it is a wall to the CA. The row below used to claim "CA pools behind the dam locally"; that is true only of water already in motion | v2 = **§6.3**, which is bigger than this row assumed: it must supply throughflow at the deviation (M1) *and* bound mobilization (M2), not only dry the downstream. Detector → cut record **into the edit log** (`rivernet.h:127-149` designed the diffs to replay byte-identically; the log needs a record-kind byte, `editlog.h:26-31`), plus the two-component freeze/surface override. The graph is **not** the shared state and `buildFromFlowPlane` is not buildable from the shipped plane — no flow direction is on the wire (§6.3.4, §6.3.8). Today's graph is per-**authority-process** (refused on `NM_Client`, `VoxelWaterSubsystem.cpp:3652-3664`), 1.44 km, unpersisted, and its diffs are drained and discarded (`:3841-3850`) |
| **divert a river** | `kDivertChannel`/`promoteChannel` exist and replay; the flux detector exists in `rivercouple.h` | same edit-log routing; enable only after v2 graph work |
| **place water** | `SpawnWaterAt` funnel exists, authority-checked | unbind the `1` key from `voxel.Water.BucketFill`; make it an item with a budget |

SWE stays exactly where it is: standalone-only, non-authoritative, cosmetic
on top (`VoxelWaterSubsystem.cpp` refuses it off `NM_Standalone`).

### 6.3 The edit-response layer

This section answers four questions that were asked directly: what happens
when a player dams a multi-kilometre river; whether the upstream body rises
and floods around the dam; what happens when a player digs a canal and
re-routes the river; and whether the old downstream channel is permanently
rerouted and slowly dries after its water reaches the sea.

**Two of the four answers changed when they were measured.** Both
measurements are now pinned as tests in `voxel-core/tests/test_waterca.cpp`
(section C8b), because the drafts of this section that preceded them were
wrong in opposite directions.

#### 6.3.1 Ground truth, measured

**M1 — A dam on an undisturbed baked river impounds nothing at all.**
(`waterca_mobilize_a_dam_on_an_undisturbed_river_impounds_nothing`.) A wall
built across a synthetic baked river settles in **1 tick**, mobilizes **5
x-bricks of 32**, and the water standing immediately upstream is at *exactly*
the baked surface — it does not rise by one voxel, against a crest eight
voxels above it.

The reason is structural, not a tuning failure. A baked river is a *datum*,
not a flow: every part of it the player has not touched is a **wall** to the
CA (`waterca.h:887-894`), and the CA has no discharge to impound. The
intuition this section was first drafted on — "the CA is volume-conserving,
so water pools behind a wall and spills over the lowest lip" — is true of
water that is *moving*, and the shipped tests that support it
(`waterca_container_fills_bottom_up_never_escapes_walls`,
`waterca_wake_region_settled_pool_flows_through_a_carved_breach`) all pour or
breach. None of them dam a datum. **So the upstream half of "dam a river" has
no mechanism behind it either, and §6.3 cannot be only a downstream-drying
layer.**

**M2 — One edit that lets a baked river drain mobilizes the entire river.**
(`waterca_mobilize_front_consumes_an_entire_implicit_river_once_it_can_drain`.)
A single 4-voxel-wide shaft dug through the bed near the downstream end
converts **100 % of the reach** — every x-brick, every unit of implicit water
— and settles. `advanceFront` mobilizes the face-neighbours of every *active*
brick (`waterca.h:1021-1028`), a freshly mobilized brick is filled and woken,
so while the released water keeps moving the front keeps advancing. **There is
no length bound anywhere in that loop.** It terminates when the water stops,
which on a sloping river with an open drain means: at the source.

M1 and M2 together are the whole problem. It is *drainage*, not disturbance,
that makes mobilization run away — and the run-away is unbounded while the
thing everyone assumed worked does not happen at all.

**M3 — What an accumulated river costs.** Measured on the same synthetic
reach (25.6 m long, 0.4 × 0.4 m channel) by serializing the real
`WaterState` blob before and after:

| | savegame blob | mobilized bricks | stored CA bricks | active bricks |
|---|---|---|---|---|
| untouched | **44 B** (header only) | 0 | 0 | 0 |
| after ONE drain edit | **1 820 B** | 44 | 32 | **0** |

Normalised: **10.7 mobilized bricks per metre of river per m² of channel
cross-section.** The geometric floor for a perfectly packed body is ~2
bricks/m/m² (a `WaterBrick8` is 512 cells at 10 cm; `core.h:238`), so a
thin channel like this one is a ~5× worst case for packing. *The following is
arithmetic on that measurement, not a measurement:* a realistic 20 m × 2 m
river is 40 m² of cross-section, i.e. **80–430 permanent bricks per metre**,
i.e. **0.04–0.22 MB of resident `WaterBrick8` per metre**. A 10 km reach is
**0.4–2.2 GB**. That is the number that governs this design.

Note the last column. `active` is **0** — it has settled, so it costs no tick
time at all. The cost is not CPU. It is memory, savegame and the late-joiner
snapshot, and it is permanent. See §6.5.

#### 6.3.2 What this means for the four questions

* **Dam a river — does the upstream body rise and flood around?** *Today, no
  — nothing happens (M1).* With discharge supplied at the deviation, **yes,
  and correctly**: a probe that injects throughflow at the head of the same
  reach shows the stage rising monotonically behind the dam and spilling
  through a notch cut in one side — the **lowest lip**, not the crest.
  Path-of-least-resistance really is physics and not a special case
  (`waterca.h` Phase C, hydrostatic level equalization, `:145-293`), but only
  once something is flowing. *This probe is in the scratchpad, not committed:
  it is evidence that the proposed mechanism produces the wanted behaviour,
  not a pinned contract.*
* **Does the downstream channel dry up?** Today it dries *by accident and
  unaffordably* — the front eats it (M2) and it drains as CA water. The
  design below makes it dry *on purpose and for free*, by changing what the
  datum says rather than by converting the river to voxels.
* **Canal / re-route — permanently rerouted?** The **old** channel: yes,
  cheaply, by the same override. The **new** channel: it runs as ordinary CA
  water for its whole length, which is affordable only because a
  player-dug canal is tens of metres. It does **not** become a
  baked-quality river to the sea — see the honest limit in §6.3.8.
* **Does the old channel dry slowly, after its water reaches the sea?** Yes —
  §6.3.5 makes that a bounded, deterministic ramp.

#### 6.3.3 The mechanism: two overrides, applied at different speeds

The plan's own §4.4 rule stands: *baked = equilibrium, runtime = deviation
from equilibrium, only where players cause it.* An override does not populate
water and does not add a second mechanism (F5). It changes the **input** to
the one shipping mechanism — the `ImplicitFn` bound at
`VoxelWaterSubsystem.cpp:351-363` — for a bounded set of pixels a player's
edit named.

It has **two independent components, and separating them is the load-bearing
idea**:

1. **The mobilization freeze — instantaneous, invisible, and it is the
   bound.** From the tick the cut is logged, bricks along the affected reach
   are ineligible for *activity-driven* mobilization. This stops M2 dead.
   It changes nothing a player can see, because `implicitFillAt` still
   returns the full datum and the river still renders.
2. **The surface override — gradual, visible, and it is the look.** A
   per-pixel reduction of the water-surface datum, ramped over time, which is
   what makes the reach *dry*.

If the two were fused, the ramp would be the enemy of the bound: during the
minutes the surface takes to fall, the front would mobilize the reach anyway,
and those bricks — already CA-owned — would never dry (`implicitFillAt`
returns 0 for a mobilized brick, `waterca.h:984-987`). Freezing first and
draining slowly is the only order that works.

**The freeze must gate `advanceFront` only, never `mobilizeEditRegion`.**
`waterca.h:904-918` names the two seeds; the edit-driven one must always fire
or digging into a drying river would silently do nothing. This is the one
genuinely new hook in voxel-core: a caller-supplied
`bool(const BrickKey&)` predicate consulted by `advanceFront`, roughly ten
lines. It moves no water, so it is not a second water mechanism.

#### 6.3.4 What a reach is, and why the graph is not the shared state

**A reach is not identified by a graph node id.** Segment and node ids in
`rivernet.h` are vector indices assigned by a build over caller-supplied
`RegionBounds` (`rivernet.h:19-27`); two peers that swept different regions
get different ids for the same water. **That is the desync, and it is
avoidable by not shipping ids at all.**

Nor can a client re-derive the affected reach. The shipped flow plane carries
**magnitude only** — bits 0–4 are `log2(acc m²)`, plus three flag bits
(`tile_codec.py:194-199`). **There is no flow-direction raster on the wire**;
D8/MFD receivers exist only inside the bake (`flow.py:215,247`). A client
literally cannot walk downstream.

So the authority resolves the course and **ships the decision, not the
evidence** — exactly the argument `rivernet.h:136-140` already makes for
`kDivertChannel`: *"replay must not have to re-derive the course from live CA
state that no longer exists. The course IS the decision."* The same reasoning
applies verbatim, and it also disposes of tile residency and tile-version
skew in one move.

**The record.** One new edit-log record kind:

    cutPx, cutPy      i32 x2   the pixel the blockage sits on (absolute pixel space)
    startTick         u64      authority fixed-step counter at the cut
    rampTicks         u32      local drawdown duration
    releaseFactor     u8       0 for a cut; the factor at release for a release record
    n                 u16      course length in pixels
    then n x { dir u8, surfaceDeltaMm i16 }   3 B per pixel

`dir` is a D8 index reconstructing pixel *k* from pixel *k−1*, so the course
costs 3 B/pixel and needs no flow data on the client. `surfaceDeltaMm` is the
**final** dry-state drop at that pixel, computed once by the authority; the
client only interpolates in time and needs no hydrology at all. A 5 km reach
at 30 m/px is 167 pixels ≈ **500 B**; the hard cap below is ~1.5 KB per cut.

**Why the delta tapers on its own.** The authority computes it from the flow
plane it *does* have: the fraction of discharge lost at pixel *p* is
`acc_cut / acc_p`, which falls as tributaries rejoin below the dam. So the
override recovers toward zero downstream **causally and for free**, and the
reach ends where the loss stops mattering. The surface itself follows
`channel.h`'s own law (`waterLineMm = bed + depth·3/4`, `channel.h:263-270`)
rather than a new one — consistent with §9.3's "reuse `channel.h`'s geometry
and laws".
*Unknown:* the flow byte is `log2` in 5 bits, so `acc_cut/acc_p` is quantized
to powers of two and the taper may look stepped. Unmeasured. If it does, the
authority can read the un-quantized `acc` from its own `.vxfl` superblocks
(`pipeline.py:1793-1887`) instead — it is server-side and lossless.

**Bounds, three of them, independent:**

* `maxOverridePixels` per cut (~512 px ≈ 15 km) — a hard cap.
* the natural taper above, which usually stops it much sooner.
* `maxActiveCuts` on the authority. Beyond it a new cut is **refused and
  logged**, not queued: a dam that visibly does nothing is better than an
  unbounded world.

The bound that actually does the work is none of these — it is the freeze
(§6.3.3), which keeps the mobilized set from growing along the reach at all.
A frozen brick that the front skips is also memoized in `noImplicit_`
(`waterca.cpp:1760`), so re-asking is cheap.

#### 6.3.5 Drying in time — bounded, deterministic, and not an animation

The ramp is **a pure function of elapsed ticks, evaluated fresh every time —
never accumulated**. That single property is what makes reload, replay and
late join all land on the same answer with no catch-up:

    elapsed_k = currentTick - (startTick + k * pixelDelayTicks)
    factor_k  = clamp(elapsed_k / rampTicks, 0, 1)
    surface_k = bakedSurface_k - surfaceDeltaMm_k * factor_k

`pixelDelayTicks` staggers the start down the course, so drying travels as a
drawdown wave instead of the whole reach falling at once. Both it and
`rampTicks` are **gameplay calibration, not physics**, and are labelled as
such for the same reason `rivercouple.h:120-122` labels
`dischargePerFillUnit` one: a real drawdown wave moves at ~1 m/s, which would
take 14 hours to cross 50 km. That is not a knob to be honest about later.

The override floor is **not zero**, and should not be: below a dam a river
becomes a chain of pools fed by whatever tributaries survive, which is
exactly what the `acc_cut/acc_p` taper produces.

*Unknown, and it gates this:* the ramp needs an **authoritative, replicated
fixed-step tick counter**. The CA steps at a fixed 10 Hz (F7) and the edit log
has **no** tick or timestamp field at all (`editlog.h:26-31`), so this is new.
It is small, but it is a prerequisite and it is not free.

#### 6.3.6 The reverse transition — where this is easiest to get wrong

A release appends a **new** record carrying the factor at release, so an
interrupted ramp resumes from where it actually was rather than from a
predecessor that compaction might have dropped.

**The order is the whole answer, and the intuitive order is the expensive
one.** On release: **restore the datum first, lift the freeze second.** If the
freeze lifts while the bed is still dry, the impounded water surges downstream
as CA water and the front follows it to the sea — M2's runaway, now as a
burst, which is precisely when the world can least afford it. Restoring the
datum first means the returning river is *implicit*, and implicit water is
free.

**M4 (measured 2026-08-03, building 9a) — the hazard above only exists while
the water is still moving, and this was predicted wrong.** The 9a release test
was first written as "freeze the reach to a standstill, then lift the gate, and
expect M2's runaway to resume". **It does not resume.** On C8b's harness a
totally frozen reach settles at **tick 107** with **3** x-bricks converted (the
dig's own halo); lifting the gate then leaves it at 3 **forever**, against the
32 the ungated front ate. The reason is M1 restated: `advanceFront` is driven by
the *active* set, so once the freed water stops there is nothing to seed from
and the still-implicit river upstream is a wall again. Lifting the freeze
mid-drain *does* reproduce M2 exactly, ledger included — so the freeze is a true
deferral, not a loss, and the ordering rule above stands. It is simply narrower
than it was drafted: it binds during drawdown, not after it. Both halves are
pinned (`waterca_front_gate_release_mid_drain_reproduces_the_ungated_end_state`,
`waterca_front_gate_release_after_settling_never_restarts_the_front`).

The second hazard is that bricks mobilized during the dry spell — a player
walked the dry bed and dug — hold no water and will not refill, leaving
**scars exactly where players were**. This is the same defect as the
canal-refill case, and it has the same fix: §6.5.

#### 6.3.7 Determinism, replay, and the failure mode

**What must enter the edit log, and in what order.** The cut record is
appended in the *same batch* and *after* the terrain edit that caused it —
otherwise replay passes through a state with a dry river and no dam. The log
is a single ordered append-only sequence with contiguous `seq`
(`editlog.h:85-89,144`), so ordering is free once the append site is right;
the precedent is `PromoteDetachedIslands` running before `BroadcastNewEntries`
for exactly this reason (`VoxelWorldSubsystem.cpp:13265-13267`).

**Four shipped facts this has to be built around, all verified:**

1. **The edit log has no record-kind discriminator.** It has exactly one
   record shape — `(seq, BrickKey, vector<EditCell>)`, `editlog.h:26-31`. A
   new kind means `kFormatVersion` 2 → 3 plus a kind byte. Additive and safe
   (`kMinReadableFormatVersion` is 1, `editlog.h:47`), but it is a format
   change, not a field. The variable-length precedent to follow is
   `writePayload`/`readPayload` (`editlog.h:178-235`).
2. **`compactLog` silently drops what it does not know about.** It already
   drops `providerId` (`editcompact.h:43`), and saves prefer the compacted
   copy (`VoxelWorldSubsystem.cpp:13437-13439`). A cut record that compaction
   does not carry **will be silently lost on save and on join-sync**. This is
   a concrete shipping bug, not a risk; it must be an explicit work item.
3. **Late join is a compacted-log replay, chunked at 48 KB**
   (`VoxelEarthPlayerController.cpp:483-522`,
   `VoxelWorldSubsystem.cpp:14885-14898`). There is no voxel snapshot. Cut
   records ride this path for free *if* (2) is fixed, and because the ramp is
   evaluated at the current tick a late joiner arrives at the correct present
   state rather than replaying the animation.
4. **Water is not in the join-sync path at all.** It arrives over the
   subsequent ~5 Hz `MulticastWaterDiffs` rounds. So a late joiner sees the
   *implicit* world — including the override — immediately, and CA water fills
   in behind it. That ordering is favourable here and worth not breaking.

**The failure mode, stated plainly.** Two clients disagreeing about where a
river runs is a desync, not a cosmetic bug — and **the override is the first
piece of client-derived water state that is not a pure function of (seed,
tiles)**. That is the new risk and it should be named as such.

What bounds it is real and already shipped, not hoped for: **`NM_Client`
never steps its own CA** (`VoxelWaterSubsystem.h:18-23`, enforced at
`.cpp:3690`), and rivers are outright refused on clients
(`VoxelWaterSubsystem.cpp:3652-3664`). So a client that missed a cut record
cannot fork the simulation; the authority remains the only simulator. The
divergence is confined to **presentation and local prediction** — the wrong
river drawn, and swim/underwater tested against the wrong datum (work item 5
made those consult the datum). That is a bad bug and not a corrupt world.

The mitigation is to keep it in that class deliberately: cut records travel
the **existing** ordered edit-log path and the **existing**
`ApplyGroupedEdits` funnel (`world.h:43-47`,
`VoxelWorldSubsystem.cpp:13144`) — no side channel, no second ordering.

*Unknown, and it is not new:* the shipped UE build **never stamps
`providerId` into the edit log** (`VoxelWorldSubsystem.cpp:2618, 13505`), so
`EditLog::checkProvider`'s refusal (`editlog.h:70-81`) is exercised only by
tests today. Cut records do not make that worse, but they raise the cost of
leaving it unwired, because a cut record replayed against a different bake
names pixels that mean something else.

#### 6.3.8 What this does not do — the honest limits

* **The CA has no persistent throughflow.** It settles to flat pools; a river
  in this design is a static datum that *looks* like a river and carries
  nothing. Everything above supplies discharge only *at a player's deviation*,
  which is what §4.4's carve-out permits — not a world-wide river sim.
* **A canal does not become a river.** An override is a *reduction* of a
  datum; it cannot invent a graded water surface where the bake says dry.
  The canal runs as CA water, bounded by its own length. A canal long enough
  to matter is a canal expensive enough to hurt, at M3's rates.
* **A reservoir larger than 65 536 cells does not level.** Phase C leaves an
  over-cap component **completely unmodified** (`waterca.cpp:108`,
  `waterca.h:213-222`). At 10 cm voxels that cap is about an 8 m × 8 m × 1 m
  pond. Behind a real dam the surface will be the CA's local ±1 fixed point —
  a mound, not a plane (`waterca_pooling_spreads_flat_within_tolerance`
  states this outright for the pre-hydrostatic rule). This is a pre-existing
  limit, it is squarely in the way of "dam a river", and nothing in this
  section fixes it. `waterca.h:219-222` already names the intended fix (a
  persisted per-body union-find).
* **`buildFromFlowPlane` cannot be built from the shipped flow plane**, for
  the reason in §6.3.4: no direction on the wire. Work item 9 names it as
  though it were a build over existing data. It needs *either* a new
  flow-direction bake product (another `BAKE_VERSION` bump) *or* descent of
  the accumulation gradient as a heuristic. **This is a gap in the plan, not
  in the code.** The design above needs the graph far less than item 9
  assumes — the authority needs a downstream walk, not a routed graph — which
  is the cheaper way out.
* The dam **detector** is the least-designed part here and the biggest open
  risk. The cheap deterministic shape is: at a candidate pixel, the baked
  water column reads solid across the full channel width for N consecutive
  checks — terrain-only, no CA, no wall clock. Whether that fires reliably on
  real player dams is **unmeasured**.

#### 6.3.9 Corrections to this document, found by verifying it

* §6's table calls **ADR-0005's UE-side hook "(open)"** and work item 6 lists
  it as to-do. **It is done**: `SaveWaterStateToDisk` /
  `LoadWaterStateFromDisk` (`VoxelWaterSubsystem.cpp:2481, 2513`), wired at
  `:847` and `:890`, with the loud three-failure-mode fallback at `:2555-2575`.
* §3.2 says `BAKE_VERSION` is "currently 7". **It is 8**
  (`pipeline.py:275`); the P1 bump landed in `0081d0e`.
* §3.3's proposed `IWaterSampler` on `tiles.h` with `waterSurfaceMm(px,py)`
  and `flowByte(px,py)` **shipped in a different shape**: `lakes.h:151-189`,
  in **voxel** coordinates, with **no** `flowByte` accessor. The flow plane
  still has no runtime consumer.
* §6's table says the graph is "per-client". **It is refused on `NM_Client`**
  (`VoxelWaterSubsystem.cpp:3652-3664`); it is per-*authority-process*, built
  once at arm time over a 48 px / 1.44 km square (`:3592, 3695-3702`;
  30 m pixels, `tiles.h:159-162`) and never re-centred. Unpersisted is
  correct: the diffs are drained, logged and **discarded**
  (`:3841-3850`, whose own log line ends *"NOT PERSISTED -- this is lost on
  reload."*). `kSetConveyance` has **no producer anywhere outside tests**.

### 6.4 Ocean — DONE on this branch, except the captures

Item 1 makes sea level one symbol (`kSeaLevelMm = 0` in `core.h`, mirrored to
Python and HLSL by the existing constants-dump mechanism; `world_map.py:325`
fixed to draw at it). The ocean then becomes the third term of the same
ImplicitFn — `z < kSeaLevelMm` in open air — which unifies breach behaviour
with lakes (the bespoke reservoir top-up at `:3307-3315` can retire once
proven equivalent), gives the sea real voxels only where touched, and makes
"the plane shows through inland pits" fixable by testing the datum, not the
camera. The 40 km visual plane stays for the far field.

**What shipped, and the two corrections the work made to the paragraph above.**

The term is `vxc::oceanSurfaceMmAt(groundMm)` in `lakes.h`, composed with the
baked surface by `implicitWaterDatumMm`'s `max()` and consumed by the same
`implicitWaterFill` lakes use. The sea is therefore *a lake whose datum is
`kSeaLevelMm` and whose extent is every column whose **worldgen** ground lies
below it*. `max()` rather than a precedence rule is what makes a river mouth
one surface: at the mouth the reach's datum and the sea's **are the same
number**, so the composition cannot introduce a step.

*Correction 1: "`z < kSeaLevelMm` in open air" is not sufficient, and the
missing half is the whole inland-pit fix.* The gate is the **column's worldgen
ground**, not the voxel's z: a pit dug into land does not lower its column's
worldgen ground, so the ocean term keeps answering "no sea here" however deep
it is dug. Testing z alone would have flooded every inland pit with real
voxels — a worse bug than the plane showing through one.

*Correction 2: the reservoir was not "proven equivalent". It was proven
wrong, three ways, and deleted.* `voxel-core/tests/test_ocean.cpp` is a
**differential** test rather than the parity test this section asked for,
because the two paths agree nowhere:

| | Reservoir v0 | ocean term |
|---|---|---|
| cove cut through the coastal cliff | never settles: volume 358k → 610k fill units and **active bricks 536 → 758** between ticks 100 and 1500, still climbing; cove never reaches the datum | settles at tick 54, 197 bricks mobilized, surface 5 mm below the datum |
| inland pit, dug in three ordinary passes | seeds a boundary cell on pass 3 and pins it at 255 **forever, in dry rock 50 voxels inland** | 0 bricks mobilized, 0 fill units |
| deep puncture with headroom above it | stops at the puncture depth (its head is the pinned cell) | **also** stops at the puncture depth — see below |

The first row is the one that matters most and was not anticipated anywhere:
**under Reservoir v0 the sea is not water to the simulation at all.** Only the
pinned breach cells were; the ocean beyond them is open air to the CA. So a
breach did not *fill*, it *drained*, and the pinned cells fed it forever. The
growing active-brick count is a live CPU leak behind every below-sea-level dig
in the shipping build, hidden by the visual plane.

**What the ocean term does *not* fix, recorded so the next person finds a
measurement instead of a mystery.** A breach that requires water to **rise** —
a tunnel punched into the sea at depth with headroom above it inland — stops at
the puncture depth. The mobilized sea is already at hydrostatic equilibrium and
therefore *inactive*, and `waterca.h`'s Phase C explores previously-dry headroom
only through bricks in the **active** set. Diagnosis, not guess: the same dig
against a sea small enough that the tunnel's volume perturbs its level (64×65
voxels) *does* raise the shaft to the datum, settled at tick 8; against an
unbounded sea it does not, settled at tick 5; and raising the front budget from
64 to 4096 bricks/tick changes nothing, so it is not front starvation. **This
belongs to item 10**, the CA's activity/budget half, not to item 8.

**Rendering: the sea is deliberately kept out of the implicit candidate
sweep.** `RefreshImplicitWater` is *not* given the ocean as a third ceiling,
and that is this section's own "real voxels only where touched" rather than an
omission. The 40 km plane already draws the untouched sea on the same datum; a
voxel surface meshed under it would be a second coplanar translucent surface —
the exact defect measured this month between the near-field disc and the
far-field sheet, which agreed in geometry and disagreed in tone (+44.5 vs +76.8
blueness) because one is a single surface and the other a shell the view ray
crosses twice. It would also be tens of thousands of candidate bricks at any
shore. A **mobilized** sea brick is unaffected: the CA meshes it, so the sea
gets voxels exactly where a player has been.

**`voxel.Water.ImplicitOcean`** (default 1) is the control, and it refuses to
change under live water rather than silently reassigning cell ownership. Note
that 0 is *not* Reservoir v0 — that does not come back.

**The river mouth, and what item 7's rebase will and will not fix.** Item 7's
in-flight `CompositeWaterSampler` composes lakes and rivers by *the same
`max()`*, so the three terms are one max chain and the ocean stacks on top with
no merge conflict of substance. That settles the **geometry** at a mouth, and
settles it well: a reach's datum is bed + depth, so upstream of the mouth the
river wins and in the estuary the sea does, and because `max()` is continuous
there is no step where they cross — the mouth drowns flat at the datum, which
is what an estuary is.

It does **not** settle the tone, and item 7 will move the boundary rather than
remove it. Because the sea is deliberately not meshed and a river reach *is*,
a river mouth becomes precisely the place where near-field **voxel** water
meets the far-field **plane** — one translucent surface against a shell the
view ray crosses twice. That is the same pairing that measured +44.5 against
+76.8 blueness this month, and it will land in the exact frame the owner most
wants to look at. **Measure that frame before designing anything for it**; the
fix, if one is needed, is the same shape as the lake sheet's `subtractRect`
handover — cut the plane where the voxels draw — and should not be built on
a prediction.

**Captured 2026-08-04 — see `docs/water-map/ocean-captures.md`** for conditions,
settle evidence, the control's pixel-diff and the tone numbers. In one line
each: the inland pit stays dry through all three passes (0 bricks, 0 units,
digest unchanged) with its floor 3 m below the datum and inside the camera's
implicit-water box; the breach floods and its `ImplicitOcean 0` control is
clean (exposure moved by 0.2/255, the pair's difference is 10x the measured
in-session noise floor and sits on the water); and the plane/voxel tone
mismatch **is real and is not a constant** — over one join it runs −14.1 to
+9.0 blueness against a ±0.5 instrument floor, sign flipping with viewing
angle, on the same statistic that recorded +44.5 / +76.8 at the lake seam.
Two things the captures also established: the **river/sea** join cannot be
measured at all yet (no baked tile carries a reach — every fine tile in the
cache is `bake_ver 7`), and a breach into the open sea starts a mobilisation
front that **never stops**, because `setFrontGate`/`setMobilizedCeiling` are
never called from `UVoxelWaterSubsystem`.

**Was still outstanding, and it needed the editor:** every visual claim. The
inland-pit capture; the breach frame with its `voxel.Water.ImplicitOcean 0`
control; and the plane/voxel-water tone above.
### 6.5 The return path: what stops CA water accumulating without bound

§6.3 is about how the world *responds* to an edit. This section is about what
happens after a year of them. It is separated because it is a different
question with a different answer, and because it invalidates a claim in §7.

#### 6.5.1 The problem, verified

**`mobilized_` is insert-only.** Two insertions exist — `markMobilized`
(`waterca.h:1052`) and `mobilizeBrick` (`waterca.cpp:1710`) — and **zero
erasures anywhere in the tree**. `waterca.h:872` says so as doctrine:
*"Mobilization is per-BRICK and one-way."* It is persisted and replicated.

Two sets, and the distinction is the whole point:

| set | what it is | does it shrink? |
|---|---|---|
| `active_` | what the CA steps | **yes** — settles to empty |
| `mobilized_` | what is CA-owned rather than implicit | **never** |

So a settled reservoir stops costing tick time entirely (M3 measured
`active = 0`) and never stops costing memory, savegame bytes, and the
late-joiner snapshot. A mobilized brick is also a **permanent hole in the
implicit field**: `implicitFillAt` returns 0 there forever
(`waterca.h:984-987`), so §7's "content is free at runtime" is switched off
in that brick for the life of the world.

**§7's claim is wrong as written and is corrected below.** *"The active set
stays bounded by player activity, never world size"* is true of `active_` and
false of `mobilized_` — and the binding word is **cumulative**, not
**concurrent**. Concurrent activity is bounded by how many players are online.
Cumulative activity on a year-old server is not bounded by anything.

At M3's measured rate — 10.7 bricks/m/m², floor ~2 — one player who dams and
floods a single valley writes tens of thousands of permanent bricks. Nothing
in the system ever takes one back.

#### 6.5.2 Prior art: what actually scales

| game | approach | what it buys |
|---|---|---|
| **Minecraft** | bounds *propagation distance* (7 blocks), does not conserve volume | cost absolutely bounded; realism was never the goal |
| **Dwarf Fortress** | volume-conserving per-cell fluid, state accumulates | **the cautionary tale — this exact failure mode, shipped**; a well-known cause of late-fort slowdown |
| **Factorio** | fluids abstracted to a **graph** with a flow solver, not per-cell | scales to enormous factories |
| **Terraria** | global cap on liquid updates/tick, deliberate damping | sloshing terminates |
| **Oxygen Not Included** | full per-cell fluid | pays for it structurally in late game |

Two of these map directly. Dwarf Fortress is where this design goes if
nothing changes. Factorio is the shape that works, and it is *already* the
shape of this system: **a river is a graph of reaches, and per-cell detail is
only needed near a player.**

**The pattern that scales in a persistent world is transient solver plus
steady-state promotion:** simulate only what is changing, and hand the result
back to the static layer the moment it stops. The two-layer split here is
already exactly right — baked equilibrium plus CA deviation. **What is missing
is the arrow back.** Mobilization promotes implicit → CA; nothing demotes
CA → implicit. A persistent world without a return path is a memory leak with
extra steps.

**Recommendation: yes, build the return path.** The §6 table lists CA →
implicit demotion as *"a later optimisation, listed but not required."* For
the lake case that is fair. **For the edit-response layer it is required, and
not as an optimisation — as correctness**, because §6.3.6's refill scars and
§6.3's canal-refill case are both unfixable without it.

#### 6.5.3 Demotion, and why the predicate must be exact rather than tolerant

The obvious predicate — *inactive for N ticks AND fill matches the datum
within tolerance* — has a bug in it. Mobilization was made one-way for a
reason: the double-occupancy argument at `waterca.h:876-885`. A cell is one
byte and cannot carry both accountants' water, so a demotion that hands back
a cell whose fill merely *approximately* matches the datum creates or destroys
the difference, silently, forever.

**So demote only where the transfer is exactly volume-neutral**, and let the
tolerance be zero:

    demote brick k  iff
      (a) k is mobilized, and
      (b) k is not active, and no 6-face neighbour is active, and
      (c) every cell of k holds exactly what the current
          (override-adjusted) implicit field would give it.

Condition (c) sounds unachievable and is not, because it is satisfied by the
two cases that matter and by nothing else:

* **the fully dry brick** — CA fill absent, datum 0. `0 == 0`. This is the
  old-channel-dried-up case and the whole of the canal-refill fix.
* **the fully submerged brick** — every cell 255, datum 255. This is the
  drained-and-refilled-lake case.

It is *not* satisfied by the **surface** brick, where the datum carries a
partial top fill (`surfMm % 100`, §5.1) and the CA has settled to its own ±1
fixed point. That is fine and should be stated rather than engineered around:
one brick-shell of the water surface, 0.8 m thick, stays CA-owned. The scar
is a rim, not a river.

**Correction, found while building this (2026-08-03).** The surface brick is
not excluded by **rule**, it is excluded in **practice**, and the difference
matters if anyone ever tries to "fix" it. Nothing in the shipped predicate
treats a partial fill specially — a surface whose datum the CA happens to land
on exactly *does* demote, correctly and for free. What excludes the real case
is that the CA settles to its own fixed point and the datum was computed by a
different law, so they agree only by coincidence. Measured on a brick whose
datum has a ragged 128/100 top layer: Phase C levels it flat, the levelled
value equals neither datum value in any of the 64 cells, and the brick stays
CA-owned through 200 further ticks
(`waterca_demotion_never_reclaims_a_ragged_surface_brick`).

**As built.** `WaterMobilizer::canDemote` / `demoteBrick` / `demoteBudgeted` /
`takeRecentlyDemoted`, plus `WaterCA::clearBrickFill` as the only sink.
`demotedVolume()` is a separate counter: `debited_`/`credited_` stay **gross**
one-way flow totals, so their difference remains a pure audit of the wall
invariant even after a brick has round-tripped. No persistence code was needed —
`WaterState` already serializes `mobilized_` as a set, so a demoted brick is a
key that is no longer in it and its fill is gone because `clearBrickFill`
collapsed the brick out of the map. The budget counts **examinations**, not
demotions, because the 512-cell scan is the cost and a sweep that finds nothing
must still be bounded; a persistent key cursor wraps through the set so
successive calls cover all of it without re-scanning the front.

Condition (b) matters because demotion **restores the wall**
(`makeSolidFn`). A wall reappearing in contact with moving water is a
discontinuity; requiring the neighbourhood to be quiet removes the case
entirely, and no hysteresis is needed because (c) is exact.

**Determinism — the hard part, and the reason this is authority-only.**
A demotion that depends on wall-clock, or on which client noticed first,
desyncs. So:

* **Only the authority demotes**, on exactly the argument
  `waterca.h:931-941` already makes for mobilizing on the authority only: a
  client's CA is a replication mirror, not a simulation, so a client running
  its own predicate would drift the moment a packet was late.
* **Demotion replicates as an explicit key removal**, on the existing
  water-diff channel, alongside the fill-diffs it is consistent with. It must
  not be inferred client-side.
* **It does not enter the edit log.** It is not a world change a player made;
  it is a representation change that is *by construction* observationally
  equivalent. Putting it in the log would make replay order-dependent on a
  budget. It belongs in the water blob, which is already
  discardable-by-design (`waterca.h:1136-1141`) — discard it and the world
  degrades to fully-implicit, which is exactly what demotion is trying to
  reach anyway.
* **Budgeted**, like the front: N bricks/tick, and deferring is always safe
  because a deferred brick is simply still CA-owned.

The invariant to assert every tick is the one that already exists:
`implicitVolume + ca.totalVolume()` unchanged across a demotion, and
`shortfallVolume() == 0`. If demotion is written correctly those are
untouched by construction, which is the point of choosing an exact predicate.

#### 6.5.4 Periodic re-baking — scoped, not built

The owner's second proposal: re-derive equilibrium periodically and let the
new baked datum subsume the accumulated CA state.

This is the right long-run answer, and it is the *only* one that reclaims the
surface-brick rim, the partially-filled reservoir, and the canal that
demotion's exact predicate can never match. Requirements:

* **An edit-aware bake.** The bake generates from worldgen; a dam lives in the
  edit log. So the pipeline must apply the log, re-derive the water balance
  (§4.3) on the edited surface, and emit a new water plane. The edit log is
  already the complete, ordered, replayable record needed for this
  (`editlog.h:7`), so the input exists.
* **A discard rule**: which mobilized bricks the new datum subsumes, and proof
  that discarding them changes no visible water. This is demotion's predicate
  again, evaluated against the *new* datum instead of the old one — so
  §6.5.3 is a prerequisite, not an alternative.

**Correction to the premise, and it matters.** The proposal was framed on a
`TERRAIN_VERSION` / `BAKE_VERSION` split making a water-only re-bake a cheap
product change with terrain bytes provably unchanged. **`TERRAIN_VERSION` does
not exist** — not in code, not in docs, not on any branch in this checkout.
There is one `BAKE_VERSION` (`pipeline.py:275`), it is folded into
`_bake_fingerprint()` and thus into `fine_provider_id`
(`diffusion.py:785-830, 1049`), and the client keys every tile lookup by that
id (`VoxelFineTileStreamer.cpp:171, 278, 506`). **So today a water-only
re-bake re-keys the entire fine tile set and forces every client to
re-download every tile, elevation included.**

That does not kill the idea; it prices it. The split has to be *built* first —
a water-plane section versioned independently of the elevation bytes, so the
provider id changes for the water product only. That is a real, separable
work item and it is the actual prerequisite. It may already be in flight in
another worktree; it is not in this one.

#### 6.5.5 The worst case, bounded independently of the average

Demotion bounds the *average* — it reclaims what settles. It does nothing
about one player deliberately flooding a large valley in one session, because
that water is genuinely there and genuinely deviates from the datum. That
needs a separate, blunter bound:

* **A hard ceiling on `mobilized_.size()`, per world.** On reaching it, the
  front stops advancing — `advanceFront` returns 0. This is *safe by exactly
  the argument the front budget already relies on*: "a deferred brick is still
  a wall" (`waterca.h:896-901`). Water freezes at the boundary rather than
  duplicating or vanishing. Edit-driven mobilization must stay exempt, as in
  §6.3.3, so digging never silently fails.
* **Report it loudly.** A world at its ceiling is a world that needs a re-bake,
  and that should be an operator-visible signal, not a silent stall.
* **Demotion pressure first.** At high-water-mark, spend the demotion budget
  before refusing new mobilization.

The honest ordering: the ceiling is the cheapest of the three and is the only
one that is *guaranteed* to bound the worst case. Demotion is the one that
makes the average acceptable. Re-baking is the one that actually returns the
world to "content is free". They are complementary and should ship in that
order — ceiling, demotion, re-bake — because each is useful without the next.

#### 6.5.6 The adjacent landmine: a reservoir over 65,536 cells never levels

Located, confirmed and **deliberately not fixed** here, so it can be scheduled
rather than rediscovered.

* **Where it lives.** `kMaxHydrostaticComponentCells = 65536`,
  `voxel-core/src/waterca.cpp:108`. Tripped at `:1065` (`cells.size() >` cap
  sets `overflowed`), acted on at `:1180`: an over-cap component is
  `continue`d past and left **completely unmodified this tick** — deferred and
  correct, never partially levelled and wrong. Contract stated at
  `waterca.h:213-222`; `WaterCAProfile::hydroOverflowed` (`waterca.h:418`) is
  the only instrument and it needs `-DVXC_WATER_PROFILE=ON`.
* **The number.** 65,536 **cells**, and the cells are counted **including the
  air** the flood walks (ADR-0003 §"Verifying the two previously-filed
  blockers", item 1, confirmed). At 10 cm voxels that is 65.5 m³ — an 8 m × 8 m
  × 1 m pond is 64,000 cells, just under. So the cap is reached by a pond a
  player can throw a rock across, and reached **sooner** than that once the air
  shell above the surface is counted.
* **What it would take.** ADR-0003 is already the design pass and it is
  further along than "add a union-find": it re-derived the two previously filed
  blockers (both real, both survivable — the cell count *decomposes* into
  persistent water-component size plus this tick's air, and every air bridge
  lies inside the `touched` region we already pay to walk) and then filed a
  **third, sharper** one that is the actual work: **splits** (union-find merges
  but does not delete, and a cell draining to 0 can split a body, so the worst
  case is today's cost plus guard overhead) and **the visited obligation**
  (even a provably unchanged component must still have its cells marked
  visited, so the persistent structure must carry per-body, per-brick mask
  state, not just a count and a volume). That is a real work item with a real
  design in hand, not a constant to raise.
* **Does it make 9b or 9c untestable at realistic scale? No, and here is
  why.** The cap gates *levelling*, and neither bound reads a levelled surface.
  9b counts bricks. 9c's predicate is per-cell equality against the datum, and
  its two qualifying shapes are unaffected: a **fully dry** brick has nothing
  to level, and a **fully submerged** interior brick is 255 everywhere whatever
  the surface above it is doing. What the cap *does* guarantee is that behind a
  real dam the surface is a mound rather than a plane — so the **surface**
  bricks are even further from the datum than §6.5.3 already says, which makes
  demotion reclaim *less*, never *wrongly*. The tests here run below the cap,
  and that is a statement about their size, not a hidden dependence on it.
* **What it does block.** "Dam a river" as a *look*. It is squarely in the way
  of §6.3.2's upstream-rise answer and should be scheduled before 9f, not
  before 9a–9c.

---

## 7. Performance case

* **Content is free at runtime — until it is touched, and then permanently
  not.** An untouched lake: 0 bytes on disk beyond the tile, 0 bytes on the
  wire, 0 CA bricks, 0 ticks. Identical economics to the shipped cavern lakes,
  and measured: an untouched synthetic river serializes to **44 bytes**, the
  blob header alone (§6.3, M3).

  **Correction (2026-08-03).** This bullet used to end *"the active set stays
  bounded by player activity, never world size — the same property the edit
  log gives terrain."* That is **true of `active_` and false of
  `mobilized_`**, and the difference is the one that matters over a world's
  lifetime. `active_` settles to empty and costs no tick time. `mobilized_`
  is **insert-only** — two insert sites, zero erase sites anywhere in the tree
  (`waterca.h:1052`, `waterca.cpp:1710`) — so it is bounded by **cumulative**
  player activity, not **concurrent**, and on a long-lived server cumulative
  activity is not bounded by anything. It is not analogous to the edit log
  either: the edit log is compacted (`editcompact.h`), and nothing compacts
  the mobilized set. One drain edit was measured converting **100 %** of a
  reach permanently. See §6.5 for the return path this needs.
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

**5. Lake sheets in the clipmap bands + swim/underwater against the datum —
DONE on this branch.** `AVoxelWaterSheetActor` draws a flat translucent
rectangle set per baked basin at its own datum, from the same extent masks the
near field consumes, cutting the implicit disc's exact footprint out of itself.
The swim/underwater half needed no new code and was verified in-engine anyway.
Read §5.2's 2026-08-03 correction before touching it: the gap was from 26 m,
not from the clipmap's inner hole. *Verified: captures at 2 km and 10 km, each
with its no-sheet control, plus the near/far handover and a submerged frame;
`VoxelPerfRun post-warmup` p95 delta **+0.121 ms** at 2560×1440 against a
same-config noise floor of 0.013–0.038 ms.*

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

**8. Ocean unification (§6.4) — DONE on this branch except the captures.**
The sea is the third term of the water ImplicitFn (`oceanSurfaceMmAt` /
`implicitWaterDatumMm` in `lakes.h`); Reservoir v0 is deleted, taking the last
two bare sea-level literals in `ue-project` with it (item 1's "one symbol" had
never covered that tree — `test_sea_level_contract.py` now does).
*Verified: `voxel-core/tests/test_ocean.cpp` — eleven cases, each with its
ocean-term-off control. The asked-for parity test could not be written: the
two paths agree nowhere, so it is a differential test and §6.4 carries the
table. Outstanding, needs the editor: the inland-pit capture, a breach frame
with its control, and the plane/voxel-water tone at a breach.*

**9. The edit-response layer (§6.3).** Rewritten 2026-08-03 after M1/M2 were
measured; the old one-line item assumed a graph build that the shipped flow
plane cannot support (§6.3.8) and a dam behaviour that does not happen (M1).
Ordered so each step is useful without the next, and so the two cheap
*bounds* land before the expensive *behaviour*.

* **9a. The mobilization gate — DONE on `claude/water-return`.**
  `WaterMobilizer::setFrontGate`, a caller-supplied `bool(const BrickKey&)`
  consulted by `advanceFront` only, never `mobilizeEditRegion` (§6.3.3).
  Checked at both seed and drain time — seed time is what keeps `pending_`
  bounded beside a permanently frozen reach. No tick rule touched,
  `kWaterCAVersion` **not** bumped. *Measured: the drain edit that M2 says
  converts 32 x-bricks converts 4, leaving 913,920 of 1,044,480 implicit units
  with the datum. Verified: three tests in C8b's own harness — the pinned
  reach, the closed gate an edit still fires through, and both halves of
  release (see M4 in §6.3.5 — one prediction was wrong).*
* **9b. The `mobilized_` ceiling + demotion pressure (§6.5.5) — DONE on
  `claude/water-return`.** `setMobilizedCeiling` / `atMobilizedCeiling` /
  `ceilingRefusals` / `setCeilingRelief`. Safe by the front budget's own
  argument. `mobilizeEditRegion` and `markMobilized` are exempt, so the count
  may exceed the ceiling and the test is `>=`. *Measured: M2's runaway stops at
  exactly 12 of 44 bricks with a ceiling of 12; the stalled queue is 12 bricks
  at tick 200 and the same 12 at tick 4000, because refusal happens at seed
  time. Ledger and `shortfallVolume()` exact on all 4,000 stalled ticks.*
* **9c. CA → implicit demotion (§6.5.3) — DONE on `claude/water-return`.**
  Exact predicate (zero tolerance), authority-only, budgeted by examinations,
  replicated as key removal via `takeRecentlyDemoted`, in the water blob and
  not the edit log — and needing no new persistence code at all. *Verified:
  one unit in one cell of 512 flips demotable → not → demotable with no
  hysteresis; demote/re-mobilize round trip conserves volume as exact integer
  equality and returns a byte-identical digest to the never-touched world; a
  dried reach's four scar bricks refuse demotion until the override makes them
  exact, then hand back, and the river refills through the closed hole for
  free. `waterca.h`'s one-way doctrine comment is rewritten to say why the
  requirement was always exactness and never one-wayness.*
* **9d. The tick counter.** An authoritative, replicated fixed-step counter
  for the ramp (§6.3.5). Small, but a hard prerequisite for 9f.
* **9e. Edit-log record kind + compaction.** `kFormatVersion` 2 → 3 with a
  kind byte; **and `compactLog` must carry it** — today it silently drops what
  it does not know (§6.3.7, item 2). *Verified: round-trip; a compaction test
  that fails loudly on an unknown kind rather than dropping it; join-sync of a
  world with live cuts.*
* **9f. The override itself.** Authority-side course resolution + per-pixel
  delta from the flow plane, the record, the ramp, and the two-component
  freeze/surface split. *Verified: a two-client dam leg — upstream rise,
  downstream decay, byte-identical replay, and the mobilized-brick count
  bounded through the whole leg (the number M2 says is otherwise 100 %).*
* **9g. The dam detector.** Terrain-only, deterministic, no CA (§6.3.8).
  Last because it is the least certain and everything above is testable
  without it — a cut can be triggered by console command until it exists.

*Deliberately NOT in this item:* `buildFromFlowPlane` (§6.3.8 — the design
needs a downstream walk, not a routed graph, and the shipped plane carries no
direction), and re-enabling `rivercouple` promotion for production rivers.

**10. Perf gates: CA step budgeted/off-thread before the mobilized set can
grow; sheet overdraw measured.** *Verified: frame measurements under a
scripted mass-mobilization worst case.*

**11. The water-product version split, then edit-aware re-baking (§6.5.4).**
Prerequisite is real and currently missing: a water plane versioned
independently of the elevation bytes, so a water-only re-bake does not re-key
every fine tile and force a full client re-download. `TERRAIN_VERSION` does
not exist today. *Unblocks: the only mechanism that returns a worked-over
world to "content is free". Verified: a re-bake that provably leaves elevation
bytes and their provider id unchanged.*

Parallelism note: items 1, 2 are independent; 3 blocks 4; 6 is independent of
3–5 and can proceed anytime; 7+ are strictly after 4 proves the path.
**Exception, and it is the useful one: 9a–9c are pure `voxel-core` and depend
on none of the bake work.** The gate, the ceiling and demotion are testable
today against synthetic worlds — the C8b tests already are — so the
accumulation bound (§6.5) can land in parallel with items 3–7 rather than
waiting behind rivers. Given that the leak is live in the shipped build and
grows with every session, it should.

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
