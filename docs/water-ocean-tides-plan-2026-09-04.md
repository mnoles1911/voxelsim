# Ocean, Tides, Waves, Rock Pools, Boats & Gliders

## Context

The owner wants voxelsim's water to mirror a reference micro-voxel game
(Reddit r/VoxelGameDev "Simulating ocean tides"): large ocean bodies inside the
existing water system; waves + tides as oscillating target-heights, depth-aware
so shores break; an ocean-connectivity flag so waves reach only ocean-connected
water and rock pools disconnect and hold their water as the tide falls; a
player-local ripple/wake surface sim; and boats + gliders with basic physics.

Owner decisions: **full datum tide** (the waterline really moves), **boats AND
gliders in scope**, **voxel-core freeze lifted** (lakes.h / basinledger.h /
core.h editable). Reddit is unfetchable (403 for bots); the pasted breakdown is
the authoritative reference text.

What exploration established (3 scouts, all claims anchored):
- **Already shipped, do not rebuild**: the ripple/wake sim (`VoxelRippleField`,
  512² @10 cm, GPU wave equation, shore-masked, auto-hooked to pawn/thrown
  items — VoxelRippleField.h, standing rule :16-20 it stays material-only);
  wind waves with McCowan depth-triggered shore break
  (`Tools/water_wave_graph.py`, 8 octaves, live wind via MPC, break at
  H/d=0.78 with depth from the BathyField).
- **Missing**: ocean as a *body* (today: a 2-triangle 65 km plane at Z=0 + an
  implicit predicate within ±25.6 m of camera); tides (zero code — the hook is
  pre-named at water_wave_graph.py:52-66: move `SurfaceZUU` through the datum
  seam, never lift the wave field's mean); ocean connectivity (nothing; basins
  have no adjacency; rivernet treats sea as a terminal sink); wave
  *displacement* on lake sheets (greedy rects = 4 verts spanning 100s of
  metres, WPO invisible); buoyancy/boats/gliders (nothing; swimming is a
  binary kinematic mode).
- **Constraints that shaped the design**: `kSeaLevelMm` is a constexpr
  geological datum with derived gates (caves/karst/biome/bake contract test) —
  tides must be a runtime offset, never a change to it. Any bake output change
  re-keys the fine namespace (~810-900 CPU-s × 15 tiles, no backup) — the
  whole plan is **zero-bake**. Extent masks don't follow the datum
  (lakes.h:262-282, the known v0 limit) — the tide work must fix this for
  tidal basins only. Draw over-covers / despawn under-covers deliberately
  (water-architecture.md:231-242) — preserved, never unified.

Verification doctrine throughout (house rules): every arm proves engagement
with a counter that can fail; image before timing on renderer changes; owner
judges screenshots (present captures + conditions, no verdicts); the OFF arm
of every feature must be byte-identical to baseline; `test_sea_level_contract.py`
passes untouched at every phase; headless legs via
`tools/voxel-run-flight-leg.ps1` + `tools/voxel-perf-gate.ps1`; material regen
only via `tools/voxel-sky-chain-regen.ps1` (sky → dome → water order).

At execution start this plan ships to `docs/water-ocean-tides-plan-2026-09-04.md`
(owner's standing deliverable preference), with a SCOREBOARD section recording
every gate verdict including nulls.

---

## Phase A — Tide core + the sea rises and falls (authority)

**A1. `voxel-core/include/voxelcore/tide.h`** (new, engine-free):
`TideSpec` = up to 4 oscillators {periodS, amplitudeMm, phaseMilliTurns};
`tideOffsetMm(spec, epochSeconds)` deterministic via integer phase + a 4096-entry
sine LUT (no libm — cross-machine agreement by clock, per authority doctrine);
`tideVelocityMmPerS`, `tideMaxMm`, `quantiseTideMm`. Unit tests in
`voxel-core/tests/test_tide.cpp` (each with a must-fail arm).

**A2. Tide state on `UVoxelWaterSubsystem`** (VoxelWaterSubsystem.h/.cpp):
`FVoxelTideState {OffsetUU, OffsetQuantisedUU/Mm, VelocityUUPerS, NormPhase,
bEnabled}`, driven from `FVoxelSkyState::EpochSeconds` (VoxelSkySubsystem.h:83 —
same clock as sun/moon; tide is pure f(epoch), nothing added to save games).
**Two cadences**: continuous offset feeds MPC/materials only (no meshing);
the **datum** steps only on a quantum crossing (`voxel.Water.Tide.QuantumMm`
25) rate-limited (`MinStepIntervalS` 2.0) — bounds waterline-through-voxel
churn; ocean re-meshes nothing anyway (deliberately absent from the near-field
sweep, lakes.h:1457-1460). CVars: `voxel.Water.Tide` (default 0, ships dark),
`.Spec` (string), `.QuantumMm`, `.MinStepIntervalS`, `.ForceOffsetMm`
(pins the tide for captures/tests). On each datum step: update
`Impl.SeaLevelNowMm`, invalidate the datum memo, bump `TideDatumSteps` counter.

**A3. MPC contract** (written into MPC_VoxelSky each tick, existence-checked
loudly per the VoxelRippleField.cpp:477-494 pattern; params added through
create_sky_material.py's authoritative list): `TideOffsetUU`, `SeaSurfaceZUU`,
`TideNormPhase`, `TideVelocityUUPerS`.

**A4. Ocean datum follows the tide.** Add 3-arg overloads
`oceanSurfaceMmAt(groundMm, seaLevelNowMm)` /
`implicitWaterDatumMm(baked, ground, seaLevelNowMm)` beside lakes.h:1391/:1411
(existing forms become forwarders — shipped tests bit-identical; groundMm stays
the worldgen amplified surface so dug pits stay dry). Thread through the
complete client list: ImplicitFn guard + composition
(VoxelWaterSubsystem.cpp:1040/:1060, guard recomputed per datum step, still
exact); `AVoxelOceanActor` plane Z (VoxelOceanActor.cpp:292 →
`SeaSurfaceZNowUU()`); new instance `IsOpenSeaNowAtWorld` (static keeps
geological meaning; convert the two live callers: VoxelCharacterMovement.cpp:161
swimming, VoxelOceanActor.cpp:351 underwater post); `SubmergedDepthUUAtWorld`.
Explicit non-changes: rivernet terminal sink, `implicitWaterCeilingMm`, cavern
flood.

**A5. The surface contract vehicles/render consume** (implemented here):
```cpp
enum class EWaterSurfaceKind : uint8 {None, Ocean, Lake, TidalLake, Cavern, CAOnly};
struct FWaterSurfaceSample { double SurfaceZUU, GroundZUU; bool bHasWater;
                             bool bOceanConnected; EWaterSurfaceKind Kind; };
bool WaterSurfaceZAtWorld(double X, double Y, FWaterSurfaceSample& Out) const;
```
Datum only (no wave displacement — waves are render-side); one implementation
shared with `SubmergedDepthUUAtWorld` so a boat and a swimmer never disagree;
CA-only water returns `CAOnly` (documented limit, same as today).

**Gates A**: voxel-core tide tests green; sea-level contract test untouched;
leg with Tide 1 logs `Tide ENABLED … steps=N`, N==0 fails; Tide 0 arm
byte-identical. Functional: coastal probe flips `IsUnderwaterAtWorld` between
`ForceOffsetMm=+1500` and `-1500` (same answer = FAIL). Owner screenshots:
high/low pin at the `-VoxelOceanSurvey` shoreline — waterline moved, no
plane/near-field gap.

---

## Phase B — Waves you can see (presentation)

**B1. Lake-sheet fine-band tessellation** (VoxelWaterSheetActor.cpp/.h):
new `AppendRectQuadTessellated(...)` beside `AppendRectQuad` (:25) — rects in
the finest band intersecting a camera disc (`WaveTessRadiusM` 80, snapped to
the existing `LodSnapPx` grid so it re-keys with `LodKey`) emit as ≤1.875 m
cells; ≤~23 k verts worst case, typically one basin, inside the existing
1-basin/tick round-robin. **No stitching**: cracks are prevented by a WPO
distance fade in the material (B2), zero displacement on both sides of every
seam. Diagnostic `GetTessellatedVertCount()`.

**B2. WPO distance fade** (create_water_voxel_material.py, at the one
wave+ripple summing site :2684-2790): multiply the WPO term (waves AND ripple
WPO; normals untouched) by
`1 - saturate((dist - WaveWpoFadeStartM) / (WaveWpoFadeEndM - WaveWpoFadeStartM))`,
defaults 55/72 m — strictly inside the fine band and tess disc. Also heals the
latent near-field-quads-vs-flat-sheet WPO seam (one material, one fade).

**B3. Tide × sheets**: per-basin components make tide a transform update —
`SetRelativeLocation(0,0, CurrentSurfaceZUU - Sheet.SurfaceZUU)` per tick
(free); full rebuild enqueued into the round-robin only when |drift| > 0.5 m
or on an extent-refresh signal (Phase C).

**B4. Shore-SDF sheet clip** — the standing black-band fix
(docs/lake-sheet-black-band-2026-08-29.md; three refuted mechanisms, do not
re-theorize): M_WaterVoxel → BLEND_MASKED, OpacityMask =
`lerp(1, step(-BathyShoreClipSlackM, shoreSdf), validity)` using the shore SDF
that today drives foam only (:1574-1626). `BathyDepthAuthority` stays 0.85
(engine depth half must stay alive for boats). Guard `-VoxelWaterShoreClip=0`
for byte-identical control.

**B5. Ocean surface upgrade** (VoxelOceanActor.cpp/.h +
create_ocean_material.py rewrite):
- Replace the 2-triangle plane with a concentric-ring grid PMC built once at
  BeginPlay (centre ±96 m @1.5 m cells, ~8 doubling rings out past the
  existing clipmap-derived reach; 2:1 index stitching on inner edges;
  recentre snap = finest cell; ~40-60 k verts, ~9 sections, never rebuilt;
  Z follows `SeaSurfaceZNowUU`).
- Rebuild M_Ocean on the shared modules — exactly what water_wave_graph.py:14-16
  names: SLW + masked, absorption from `water_optics.py` (never literals),
  `water_wave_graph.build_wave_field(...)` with the same wind inputs, ripple
  taps mirroring the M_WaterVoxel site. Shared *modules*, not one material
  asset (the vertex-colour contract differs).
- Ocean depth (no BathyField at sea): depth = `SeaSurfaceZUU − groundZ` from
  the terrain clipmap height texture (resident, 65 km) via new MPC params →
  McCowan shore break + absorption grading on ocean beaches for free; foam v1
  from depth/slope.
- create_ocean_material.py joins voxel-sky-chain-regen.ps1 after sky.

**Gates B**: owner screenshots — lakeshore crests moving on the sheet, no
cracks strafing, vista unchanged >96 m; black-band repro pose (band gone);
ocean horizon (no ring seams), beach break line, calm-wind flat control;
perf legs before/after each (masked SLW can shift early-Z — measure, and the
old translucent plane may get *cheaper*). Settings rows per policy:
"Water wave detail" (Off/Near/Default → tess radius 0/40/80) and "Ocean
detail" in VoxelGraphicsUserSettings + BuildSettingsPanel; owner verdicts set
defaults.

---

## Phase C — Connectivity, rock pools, extents follow the datum (authority)

**C1. `voxel-core/include/voxelcore/oceanconnect.h`** (new, engine-free):
`oceanConnectivityFill(groundMm, n, tideNowMm, seedLevelMm, outBits)` —
n×n column grid, wet = ground < tideNow, seeds = border cells wet AND below
`seaLevel − margin` (deep-margin guard so an inland below-sea playa touching
the window edge can't self-seed), 4-connected BFS, deterministic. Unit tests:
bay connected, ridge-locked pool NOT (must-fail arm), pool flips across a tide
step.

**C2. Connectivity window** (`FVoxelWaterImpl`): 128×128 @7.5 m (±480 m, same
footprint/recentre rules as BathyField), ground from the subsystem's own
Amplifier (shares ground truth with ImplicitFn); ground cached per recentre,
BFS rerun per tide quantum step (~sub-ms). APIs:
`IsOceanConnectedAtWorld(X, Y, bool* bKnown)` (outside window → unknown →
caller falls back to geological answer); `FLakeSheetBasin.bOceanConnected`
filled at gather by sampling the basin's baked v2 `worldOutlet`. **Transport
to materials**: BathyField's reserved A channel (VoxelBathyField.h:41) carries
the connectivity mask — the pixel-level "which water receives wave heights";
R/G stay lake-only. Answer stated once: wave forces/heights go to
ocean-connected water only — ocean, tidal reaches, flooded pools at high water.

**C3. Rock pools — `vxc::TidalDatumSource`**, a decorator on THE one datum
seam (`IBasinDatumSource`, lakes.h:283; wraps `BasinLedgerDatumSource`,
bound at VoxelWaterSubsystem.cpp:3438): non-tidal basins pass through
bit-exact; tidal + connected (tide ≥ sill) → surface rides the tide;
tidal + disconnected → held at `spillMm` (full to its sill — stateless and
deterministic: a tidal basin's sill is below astronomical high water by
qualification, so it re-floods every cycle); `max()` with the ledger so a
rain-credited pool still wins. Qualification oracle (cached per basin, per
recentre, BFS at high water): `spillMm ≤ sea + tideMax` AND `floor < spill`
AND connected-at-high-water. Ledger integrity: tide is a datum overlay, never
a credit — spills/refunds/save blob untouched. Every consumer (near-field
fill, sheet SurfaceZUU, SubmergedDepth, WaterSurfaceZAtWorld) moves at once by
construction of the seam.

**C4. Extents follow the datum** (scoped to tidal basins; ocean's extent is
implicit `ground < seaNow`, already done by A4):
- Near field: already correct (per-voxel ground bound, lakes.h:266-268) — pin
  with a test, change nothing.
- Far-field draw: `extentMaskAtDatum(tx, ty, id, datumMm)` in `LakeSampler` —
  per-pixel `ground < datum` over the basin bbox, memoised one entry per
  tidal basin keyed by quantised datum (dissolves the "expensive to keep"
  concern by scoping); sheet rect builders route through it for tidal basins;
  draw stays any-pixel-wet **over-cover**.
- Despawn: `BuildBasinExtentBits` uses the same current-datum mask for tidal
  basins, keeping centre-wet **under-cover** — the two roundings stay
  deliberately opposed (a test asserting them equal must FAIL — doctrine pin).
  Falling tide leaks a few particles into conserving sinks (the documented-safe
  direction).
- Dry tidal basins (blocker 2): `indexFor`/sheet gather admit a
  `holdsWater()==false` row iff oracle-tidal (the admit-on-demand hook
  lakes.h:279-282 prescribes; general playas stay out of scope).

**Gates C**: oceanconnect + TidalDatumSource truth-table tests; leg logs
`OceanConnect: seeds/wet/connected/basinsFlagged/recomputes` — zero seeds or
recomputes on a coastal leg FAILS; probe in a known pool: submerged at BOTH
pinned high and low (pool held) while an inland lake's datum is byte-identical
across pins (`InlandDatumDeltaMm != 0` fails); fluid conservation leg green
across a forced tide sweep. Owner screenshots: rock pool flooded at high /
standing proud at low; ebb/flood shoreline pair (no floating lip, no shore
gap).

---

## Phase D — Boats + wake (vehicles I)

**D0. Asset-forge assets (owner directive): canoe + glider on the entity
lattice.** Per ADR-0010 (docs/adr/0010-two-lattice-jurisdictions.md) vehicles
are ENTITIES: they carry their own per-entity pitch, never join the world
ladder, are excluded from world composition, and the `.vxa` format already
supports this. Author two new specs in asset-forge (`asset-forge/specs/`):
`canoe.json` and `glider.json`, pitch **2.5 cm** (entity jurisdiction permits
any pitch; 2.5 matches the craft floor visually — flag to owner if finer,
e.g. 2 cm like many animals, is wanted). Since asset-forge's ten kinds are all
organic/rock, add a small new kind (`artifact`) with a deliberately simple
generator: lofted hull sections for the canoe (thwarts, gunwale lip), wing +
spar + seat frame for the glider — respecting the shape rules on record
(no contour rings, no off-axis cuts, depth-vs-quantile, no per-voxel thinning;
asset-forge-voxel-shape-rules). Verify with the standing instruments (voxel
count + render, never colour impressions; a generation pass must prove it
changed voxels — the silent no-op trap).

**D1. Chassis `AVoxelBoat : APawn`** (new VoxelBoat.h/.cpp): the VoxelDebris
pattern (VoxelDebris.cpp:123 — Chaos body, ALL collision responses ECR_Ignore,
gravity + SimulatePhysics on, terrain via `RaycastVoxelWorld`); visuals =
new reusable `UVoxelAssetBodyComponent` — loads the unrigged `.vxa` grid at
its native entity pitch and renders it as one ISM of per-voxel cubes
(VoxelProxyBody discipline, but sourced from the asset grid; unrigged grids
pass the `hasParts()` exclusion untouched). This is the FIRST moving entity
rendered from an entity-lattice asset — animals are authored-but-unrendered,
so no shipped path exists; the component is deliberately minimal (static grid,
no animation) and becomes the seed for animal rendering later.

**D2. Buoyancy — flat surface (datum + tide), waves cosmetic** (v1 decision,
quantified: drawn WPO is ±~2 cm at reference wind — below perceptual threshold
from a boat camera; reference game is "fairly basic physics"; a CPU wave
mirror also phase-drifts vs MaterialExpressionTime — recorded stretch: have
water_wave_graph.py emit a C++ params header, the gen_material_palette_ush
precedent). 4 probes at hull corners against `WaterSurfaceZAtWorld`;
spring-damper up-force per probe via AddForceAtLocation (pitch/roll falls
out); keel-asymmetric linear drag, thrust, rudder torque; constants in a
`VoxelBoatTuning` block. Grounding: downward raycast → settle/friction when
shallower than draft; bow raycast kills velocity into cliffs. Unstreamed-tile
rule: datum-only queries; physics sleeps beyond ~100 m of camera.

**D3. Possession**: interact-key near boat → Possess (store fly pawn),
exit → teleport beside hull + re-possess; verify RippleField `AutoWatch`
follows the switch.

**D4. Wake**: `AddSweptDisturbance(Start, End, WidthM, StrengthM)` on
VoxelRippleField (sub-splats through `AddDisturbance` unchanged — same
counters/drop semantics; cosmetic-only rule intact). Boat injects 2 bow
splats ±half-beam + 1 transom splat per tick (≤ kSplatSlots=8); the
constant-c field turns a moving source into a clean wedge — reads as a Kelvin
wake with no new sim. Hull-slam splash on probe-velocity spike. Stated bound:
wake exists only in the ±25.6 m camera window — correct for the possessed
boat; distant boats float wake-less (documented).

**Gates D**: owner screenshots — at rest on its waterline, underway (wedge +
transom trail), beached, enter/exit. Ripple injection counters engage before
any timing; boat-drive leg + perf gate.

---

## Phase E — Glider (vehicles II)

`AVoxelGlider : APawn`, kinematic (no Chaos — the fly-pawn house pattern):
point-mass lift/drag (`Va = V − Wind` from VoxelWeatherSubsystem's
`WindVectorMS` — gliders ride the same wind that drives the waves;
`CL/CD` with stall clamp, ~1:8 glide), pitch/roll controls, coordinated yaw;
terrain by downward raycast → flare/settle → re-possess; water ditch →
ripple splash. Visual: the asset-forge `glider.json` grid via
`UVoxelAssetBodyComponent` (D0/D1). Launch v1: deploy while falling.

**Gates E**: owner screenshots (glide over water, ditch splash); perf gate
(expected zero-delta).

---

## Phase F — Water look & feel completion (owner directive, 2026-09-05)

Owner ask: "a shader that delivers wave simulation, dynamic foam, caustics, and
atmospheric rendering. Ocean and lake floors should have caustic reflections.
There should be underwater effects for player when submerged vs above water.
Include buoyancy physics in the plan. Add wake generation for objects/entities
moving across water surface such as player, boat, etc. add adjustable quality
settings to our Settings Panel in game to control key knobs for water quality.
Ensure multiplayer compliant determinism."

**Status map first — most of this exists; build only the delta:**

| Ask | Status | Where |
|---|---|---|
| Wave simulation | SHIPPED | wind-driven 8-octave field + McCowan shore break (water_wave_graph), WPO on tessellated sheets (B1/B2) + ocean grid (B5); interactive wave-equation sim (VoxelRippleField, 512² @10 cm GPU). No FFT sea — deliberate scope match to the reference game; an FFT upgrade is a separate future decision, not assumed here |
| Atmospheric rendering | SHIPPED 2026-09-05 | shared sky-reflection/sun-glint/moon/star chain (water_sky_reflection_graph) consumed by BOTH M_WaterVoxel and M_Ocean; absorption from water_optics; M_Underwater binds sun/moon |
| Dynamic foam | PARTIAL | shore foam from the SDF (lake), ocean foam v1 from depth/slope (B5) → **F2** adds wind/crest-driven foam |
| Caustics (incl. floors) | NEW | **F1** |
| Underwater vs above | SHIPPED | M_Underwater unbound post + depth grading + fog suppression, switched by the tided IsUnderwaterAtWorld (entered/exited transitions logged) → **F3** adds quality-gated polish only |
| Buoyancy physics | IN PLAN (Phase D, code landed) | 4-probe spring-damper on WaterSurfaceZAtWorld (datum+tide), keel drag, grounding; glider ditch + thrown-item splash already interact. Floating debris = future opt-in, not Phase F |
| Wake generation | SHIPPED (D4) + **F5** audit | AddSweptDisturbance is THE entity wake seam (boat bow/transom splats); pawn is auto-hooked (RippleField AutoWatch follows possession); thrown items splash. F5 pins player-swim wake with a counter gate and documents the seam for future entities |
| Quality settings | PARTIAL | Water Wave Detail + Ocean Mesh Detail rows shipped → **F6** adds Water Effects Quality |
| MP determinism | PARTIAL BY DESIGN | tide/connectivity already integer/LUT/fixed-order deterministic → **F7** audits, pins, and names the one prerequisite (sky-epoch replication) |

**F1. Caustics** (the one genuinely new renderer feature):
- New shared module `Tools/water_caustics_graph.py` — PROCEDURAL caustic field
  (2-3 panned, mutually-warped interference octaves; function of world XY +
  material time; sun direction/intensity + new `CausticIntensity` params via
  MPC_VoxelSky through create_sky_material.py's authoritative list — the regen
  chain's discovery scan will see the module; list it in $NOT_A_GENERATOR).
- **Floor consumers**: M_VoxelTerrain and M_ClipmapTerrain — both ALREADY bind
  the bathy field (create_voxel_material.py:68/:120,
  create_clipmap_material.py:56/:90 — the wet-shore plumbing), so submersion is
  known per-texel: lake depth from bathy R, ocean depth from `seaNow − ground`.
  Additive light term on submerged texels only: scaled by sun altitude,
  attenuated with depth via water_optics absorption (never literals), distance
  -faded (near field only, start ~40 m — measure). Night/moon: off below a sun
  -altitude floor (moon caustics are a non-goal).
- **Underwater half**: matching caustic term in M_Underwater (the post already
  binds the sky collection), quality-gated; optional light-shaft term at High.
- Controls: `voxel.Water.Caustics` (default per owner verdict; 0 = byte
  -identical off arm) + F6 row.
- Gates: shallow-shore image A/B (pattern on the floor at noon; gone at cvar 0;
  gone at night); off-arm byte-identical; perf leg before/after — the terrain
  base pass is the cost risk, and the quality row to Off is the escape hatch.

**F2. Dynamic foam v2**: crest/energy foam from the wave field's own octave
slopes (already computed in water_wave_graph — reuse, no second field);
whitecap coverage scales with wind speed; shore foam unchanged. Calm-wind arm
must show zero whitecaps (the existing calm control pose). Off arm byte
-identical behind `voxel.Water.FoamV2`.

**F3. Underwater polish** (quality-gated, all optional): caustic shafts (F1's
term), surface-underside tint via the existing reflection chain's Fresnel
(cheap Snell-window approximation). The submerged/above SWITCH itself is
shipped and already tided — F3 changes appearance only, never the predicate.

**F5. Wake coverage audit**: functional leg — possessed pawn swimming a line
logs rising `voxel.Water.Ripple.Stat` injected counters (engagement can fail);
document AddSweptDisturbance as the wake API for future entities; the ±25.6 m
camera-window bound stays documented, not fixed.

**F6. Settings Panel — "Water Effects Quality" row** (Off / Low / High) in
VoxelGraphicsUserSettings + BuildSettingsPanel per the settings-row policy:
Off = caustics 0, foamV2 0, shafts 0; Low = caustics on/no shafts; High = all.
Advanced individual cvars stay console-only (row count stays small; the
row-builder table is already in place). Mid-session toggle must move the
engagement counters (a parked-camera staleness trap was already fixed once —
test the toggle, not just the boot value). Defaults = owner image verdicts.

**F7. Multiplayer determinism** — audit, pin, and one prerequisite:
- **The authority layer is already deterministic by construction** and F7 pins
  it with tests: tide = integer phase + 4096-entry sine LUT, no libm
  (cross-machine agreement by clock was a Phase A design requirement);
  datum steps quantised (25 mm); oceanconnect BFS row-major seeds + FIFO +
  fixed neighbour order (byte-compared in test); TidalDatumSource is
  arithmetic on the ledger datum; buoyancy reads the same column contract as
  the swimmer. New pin: a cross-process determinism test (two runs, same seed
  + epoch, compare tideOffsetMm/oceanconnect bit output) in voxel-core tests.
- **Cosmetics are per-client BY DOCTRINE** and exempt: ripples (material-only
  standing rule), foam, caustics, WPO displacement. F7's deliverable includes
  the written boundary list so nobody "fixes" a cosmetic divergence into a
  synced system.
- **The one real prerequisite: sky-epoch replication**
  (VoxelSkySubsystem.h:35-61 TODO, already named in the risk register). Tide is
  pure f(epoch); two clients with skewed clocks get skewed seas. Scope: server
  -authoritative epoch replicated with smooth client correction. Full MP
  session gates deferred until an MP test harness exists — stated, not hidden.
- Vehicle physics authority (server vs client) is an MP-architecture decision
  OUT of this phase; documented as such.

**Phase F ordering**: F1 → F6 (row needs F1's cvars) → F2/F3 (share F1's
module regen) → F5 (pure leg) → F7 (voxel-core tests + epoch replication).
Each lands behind its cvar default-off until owner verdicts, per the standing
doctrine. Material changes ride the regen chain (sky first — new MPC params).

## What does NOT change

`kSeaLevelMm`/`kSeaLevelVoxelZ` + derived gates (caves.h:165, karst.h:116,
biome beach band); **the bake — zero output changes, no re-key,
TERRAIN_VERSION/bake_ver untouched, submarine-basin filter stays**;
test_sea_level_contract.py; rivernet terminal-sink rule; basin union rule;
ledger wire format + hydro save blob; over-cover/under-cover rounding
doctrine; ocean absent from the near-field sweep; RippleField stays
material-only; 1/2-arg lakes.h signatures (forwarders, bit-identical).

## Risk register (top items)

| Risk | Mitigation |
|---|---|
| Waterline-through-voxel remesh churn | Ocean not swept (no mesh); tidal-basin churn behind 25 mm quantum + 2 s rate limit + existing 192/tick budget; step counter exposes rate |
| Sheet tess verts / rebuild hitch (22.7 ms first-build history) | Disc-gated, 1-basin/tick unchanged, vert counter, settings row to 0 |
| Masked-SLW switch shifts early-Z / SLW cost | `-VoxelWaterShoreClip=0` control; image-before-timing; legs per change |
| MPC_VoxelSky recreation unbinding new params | Params via create_sky_material.py's list; existence-check-and-log pattern; regen chain order enforced |
| Cross-machine tide divergence | Integer phase + sine LUT, no libm; quantisation absorbs residue |
| Sky epoch not replicated (MP) | Documented prerequisite (VoxelSkySubsystem.h:35-61 TODO); single-player unaffected; optional sub-item |
| Buoyancy on unstreamed tiles | Datum-only queries; sleep beyond ~100 m |
| Boat vs ±25.6 m ripple / 960 m bathy windows | Camera rides possessed boat; distant boats float wake-less — documented bound |
| BFS admits inland below-sea depression at window edge | seedLevel deep-margin guard; residual is the already-documented IsOpenSeaAtWorld class, strictly narrowed |
| Tidal datum masks rain-credited pool | max() with ledger — ledger wins when higher |
| Asset-forge has no vehicle kind; entity assets have never been rendered on a moving actor | New minimal `artifact` kind + `UVoxelAssetBodyComponent` (static ISM from grid); per-voxel cube count at 2.5 cm on a 4 m canoe is ~low-thousands of instances — measure, coarsen pitch if heavy |

## Critical files

- voxel-core: `lakes.h` (datum seam :283, ocean composition :1391-1416, extent
  blockers :262-282), `basinledger.h` (:1804), new `tide.h`, `oceanconnect.h`,
  tests
- `Source/VoxelEarth/VoxelWaterSubsystem.cpp/.h` (ImplicitFn :1040, ledger
  stack :3438, sheet gather :7294, new tide state + surface contract)
- `Source/VoxelEarth/VoxelWaterSheetActor.cpp/.h` (tessellation, tide nudge)
- `Source/VoxelEarth/VoxelOceanActor.cpp/.h` (grid mesh, tide Z, depth params)
- `Source/VoxelEarth/VoxelRippleField.cpp/.h` (AddSweptDisturbance)
- `Source/VoxelEarth/VoxelBathyField.cpp/.h` (A-channel connectivity)
- `Tools/create_water_voxel_material.py` (WPO fade, shore clip),
  `Tools/create_ocean_material.py` (rewrite on shared modules),
  `Tools/water_wave_graph.py` (consumed, tide-note honoured)
- New: `VoxelBoat.h/.cpp`, `VoxelGlider.h/.cpp` (+ tuning blocks),
  `VoxelAssetBody.h/.cpp` (grid→ISM entity renderer)
- asset-forge: new `artifact` kind generator, `specs/canoe.json`,
  `specs/glider.json` (entity lattice, 2.5 cm)
- Settings: `VoxelGraphicsUserSettings`, `SVoxelMainMenu` (two new rows)

## Execution notes

Parallel agents per the standing preference (split by files: voxel-core+
subsystem lane, sheet/ocean render lane, material-python lane, vehicles lane;
one agent owns the build lane; legs and builds serialize; no .usf edits while
a leg runs). Each phase lands behind its cvar default-off, flips on only after
its gates; owner screenshot verdicts set the shipped defaults per the
settings-panel policy.

---

## SCOREBOARD

Every gate verdict lands here as it happens, including nulls. Format:
`date | phase | gate | verdict | evidence (leg/capture/test)`.

(empty — execution starting 2026-09-04)

## EXECUTION LOG

### Wave 1 (2026-09-04) — all five lanes landed, pre-build

- **Tide authority (Phase A)**: shipped as specced, with three deliberate deviations:
  velocity in µm/s (integer mm/s had ~7 distinct values per half-cycle), core epoch in
  MILLISECONDS (whole seconds pop the shader offset ~7 mm once a second), MPC
  SeaSurfaceZUU carries the CONTINUOUS surface while geometry/predicates read the
  quantised SeaSurfaceZNowUU (differ by ≤ one quantum). voxel-core tide tests compiled
  and run standalone: 9/9 PASS + a mutation arm that fails (gate can fail). The
  IsOpenSeaAtWorld static stays geological; the LIVE swim path converted inside
  IsUnderwaterAtWorld; the :161/:351 callers are null-subsystem fallbacks (both lanes
  independently converged on this). Known limit: WaterMobilizer noImplicit_ negative
  memo not dropped on datum step (breach freshness near the moving line only).
- **Render C++ (B1/B3/B5-mesh + bathy ocean)**: ocean grid = 13 sections, 40,017 verts,
  74,384 tris, reach ±135 km — ring count is 12 (derived from kReachDiameterUU; the
  plan's "~8" undershot: 8 doublings from 96 m reach only 24.6 km). Python model proved
  exact annulus coverage, all CCW, zero unmatched edges at all 12 seams. B3 tide nudge
  is Phase-B-scoped to basins at the sea datum ±5 UU (no public per-basin datum
  accessor; CurrentSurfaceZUUForBasin is the seam Phase C's TidalDatumSource replaces).
  BathyField ocean fill: R=depth, G=depth-proxy (clamped to ±100 m shore saturation),
  -VoxelBathyOcean=0 control, oceanTexels= engagement line; up to 262k elevation reads
  per refill — cost visible in fill=ms, coarse pre-pass named but not built.
- **Materials**: both generators executed end-to-end offline against a mock unreal
  module (M_Ocean 182 expr/198 links; M_WaterVoxel 320/372 with MP_OPACITY_MASK).
  OPEN: (1) M_Ocean has NO sky-reflection/sun-glint/moon/star chain (~700 lines in
  M_WaterVoxel) — sea reads flatter than lakes until that chain is promoted to a
  shared module; owner judges with first ocean captures. (2) OceanShoreClipSlackM 0.9
  means DEPTH not distance under the G-proxy (~20× on a 1:20 beach) — retune after
  first beach capture. (3) B4 off-arm is not byte-identical to PRE-B4 captures (masked
  in both arms; cancels within the A/B only). (4) No TideValid MPC flag — SeaSurfaceZUU
  is C++-to-C++ transport, materials must not read it as a datum.
- **Asset-forge (D0)**: canoe 16,998 vox / 3.90×0.90×0.53 m; glider 35,701 vox /
  5.6 m span; both 25 mm pitch, one piece, byte-reproducible, seed spread ≤0.13%.
  artifactprobe --arms: all three red arms fire. THE HASH TRAP: naive PARAMS addition
  re-seeds all 828 species + stales every bank; fixed with KIND_SCOPED_PARAMS +
  empty-container prune, 828/828 unchanged proven with two red arms; baseline at
  asset-forge/tools/spec_hashes.json — run artifactprobe --hashes after any PARAMS
  edit. Owner verdicts pending: canoe strake band, glider sail iso-height steps.
  Instance counts are 4-9× the plan's guess; 5 cm fallback measured (3,798/8,596).
- **Vehicles (D1-D4, E)**: consumed the column contract only (never SeaSurfaceZNowUU —
  boat and swimmer resolve identical columns). Possession bound on the CONTROLLER
  (E = board/leave boat, X = deploy/stow glider — the only unbound keys). AutoWatch
  needed no edit (reads GetPawn(), follows possession free). Cube ISM is one-per-
  material-id, MID-tinted from kMaterialPalette — PerInstanceCustomData exists nowhere
  in the tree, so per-voxel lightness jitter (ADR-0008) is NOT delivered by the cube
  path; flagged. Boat over CA-only water sinks (contract blind spot, documented).
  Rudder does nothing at rest (control surface — will be reported as broken; isn't).
  All tuning constants marked UNJUDGED in the tuning block.
- Weather header stale comment found (FVoxelWindSample says +X east; authority is
  X=North) — contradiction documented in VoxelGlider.h, header fix deferred (not that
  lane's file).


### Wave 2 (2026-09-04) -- Phase C landed (connectivity, rock pools, extents-follow-the-datum); GPU down, so code + voxel-core gates only

- **C1 oceanconnect.h**: shipped as specced -- `oceanConnectivityFill(groundMm, n, tideNowMm,
  seedLevelMm, outBits)`, 4-connected on purpose (lakeExtentFill stays 8-connected: it is
  pinned to the bake; this is runtime physics -- a diagonal pinch is not a channel, and a
  test pins the disagreement). Deterministic by construction (row-major seeds, FIFO + fixed
  neighbour order), byte-compared in test. `OceanConnectStats{seedCells,wetCells,
  connectedCells}`. Seed guard arithmetic: the engine passes seedLevel = kSeaLevelMm -
  tideMax - marginMm (`voxel.Water.OceanConnect.SeedMarginMm`, default 1000) so a seed stays
  wet at dead low water.
- **C3 TidalDatumSource**: new voxel-core/include/voxelcore/tidal.h, decorator on
  IBasinDatumSource exactly per plan; `ITidalBasinOracle` declared in lakes.h beside the
  datum seam (tidal.h includes lakes.h, so the interface could not live in tidal.h).
  Connected == tideNow >= spillMm -- arithmetic, not a grid read: the BFS decides WHO is
  tidal (once per recentre, through the oracle); the sill comparison decides the
  minute-by-minute surface, so nothing flickers with 7.5 m grid sampling. Non-tidal
  passthrough is bit-exact (returned untouched, never max()ed) -- the foundation of the
  inland gate. kNoWaterMm (INT32_MIN) can never win the max, which is what gives an admitted
  DRY pool a surface with no ledger row.
- **C2 engine window**: FOceanConnectivityWindow in FVoxelWaterImpl, 128x128 @7.5 m
  (+-480 m -- deliberately BathyField's exact footprint and recentre discipline, so a bathy
  texel asking the grid is almost never outside it). Ground = 16,384 calls of
  GetWorldgenSurfaceAndCavernFloodMm (the ImplicitFn's own accessor -- one ground truth),
  cached per recentre, cost logged as groundFillMs (UNMEASURED until the GPU returns; if it
  is tens of ms the fix is banding the fill across ticks, named here rather than built).
  TWO grids: NowBits at SeaLevelNowMm (rerun per quantum step -- the `recomputes` counter),
  HighBits at kSeaLevelMm + tideMax (per recentre / spec change; the oracle's clause 3).
  The qualification cache and the sampler's dry-row admissions always drop TOGETHER
  (ResetTidalQualification) -- dropping one leaves the bucket index disagreeing with the
  oracle about which rows exist.
- **The oracle** (engine side, FVoxelTidalBasinOracle): spill <= sea + tideMax AND
  floor < spill AND HighBits at the baked v2 worldOutlet (the spill saddle). v1 rows are
  never tidal (no floorMm / worldOutlet -- the question cannot be asked). Tide dark =>
  oracle refuses everything => passthrough bit-exact, no admissions, no routed extents: the
  OFF arm is the pre-C build by construction. Outside-window = UNKNOWN = not tidal and NOT
  counted as inland (the window did not rule, it was not looking).
- **C4 extents**: `extentMaskAtDatum` on IWaterSampler/LakeSampler, memo one entry per
  basin keyed by the (quantised) datum, stored INSIDE the TileIndex so tile eviction drops
  it with everything else borrowed from the tile. ONE DELIBERATE DEVIATION from the plan
  text: not a bare per-pixel ground<datum threshold but the SAME seeded fill as the baked
  mask (maskFor refactored into buildMaskAtDatum; a BasinEntry copy with surfaceMm=datumMm
  is the one changed number) -- two basins can share a bbox and a threshold floods the
  neighbour's side of the ridge, per lakes.h's own opening extent doctrine; for any datum
  >= baked the seeded component is a superset, so nothing the baked rule kept is lost
  (test pins superset + memo identity + shared-object-at-baked-datum). Routing:
  ResolveExtentMask in the subsystem covers BOTH rect builders AND BuildBasinExtentBits
  (despawn), only when oracle-tidal. Rounding doctrine untouched and now PINNED:
  draw_overcovers_and_despawn_undercovers_are_deliberately_unequal asserts the two
  decimations DISAGREE on a shore fixture -- asserting them equal fails. Near field pinned
  unchanged (near_field_fill_serves_a_tidal_datum_through_the_same_ground_bound). Dry tidal
  rows: LakeSampler::indexFor and the sheet gather admit holdsWater()==false iff
  oracle-tidal (the lakes.h:279-282 hook, narrowed to tidal; general playas stay out).
  Despawn masks refresh on the quantum step: VoxelFluidSubsystem polls DatumSteps and
  zeroes its 1 s sink cadence on a step -- no new event plumbing, the counter IS the step.
- **B3 seam replaced** exactly as wave 1 left it: CurrentSurfaceZUUForBasin now forwards to
  a new public UVoxelWaterSubsystem::GetBasinDatumNowZUU(tileX, tileY, basinId, &Z) -- the
  per-basin datum through the seam -- and the "at sea datum +-5 UU" placeholder (with its
  lagoon false positives) is deleted. Chose the subsystem-method form so the actor keeps
  consuming exactly one function.
- **BathyField**: the ocean fill grades against GetSeaLevelNowMm() (the Phase-B comment's
  reserved seam; the subsystem is reached via GetSubsystem inside FillWindow -- no ctor or
  type dependency added, the least-invasive route the plan allowed); a tide datum step
  FORCES a refill (polls DatumSteps -- the travel rule alone would leave a stationary
  camera graded against yesterday's tide). A channel = connectivity: armed window ->
  per-texel IsOceanConnectedAtWorld on WET texels only; unknown falls back BY KIND (an
  ocean-derived texel keeps 1, a lake texel keeps 0); unarmed / voxel.Water.OceanConnect=0
  -> 0 everywhere, the byte-identical control arm. -VoxelBathyOcean semantics untouched.
- **Surface contract**: WaterSurfaceZAtWorld fills bOceanConnected (window; unknown falls
  back by kind) and Kind=TidalLake via new IWaterSampler::basinAtVoxel -- the
  winner-reporting form of surfaceAtPixel, one candidate loop refactored rather than
  copied, so Kind and height cannot name two different basins.
- **Counters/log**: `OceanConnect: n=128 seeds= wet= connected= basinsFlagged= recomputes=
  bypassedInland= inlandDeltaMm= recentres= groundFillMs= highSeeds= highConnected=` at the
  1 Hz census, printed only while armed. InlandDatumDeltaMm = max |seam - ledger| over
  non-tidal gathered basins, MUST stay 0 (red gate). TidalBasinsBypassedInland = passed the
  sill/floor arithmetic, failed high-water connectivity -- the deep-margin guard made
  visible instead of silent.
- **Not mine, fixed to unblock the build lane**: assetfield.h:245 sign-conversion (another
  lane's uncommitted work, tripped clang -Werror in the vxc test build) -- one size_t cast;
  VoxelBoat.cpp:917 referenced non-static KeelOffsetUU from static SpawnAhead (wave-1
  vehicles lane, had never been compiled) -- the keel-depth correction moved post-spawn
  onto the instance, semantics preserved.
- **B4 shore-clip MID (coordinator add-on)**: the review pass's block in
  AVoxelWaterSheetActor::BeginPlay was found already landed and meets the spec -- ONE
  shared MID, WaterShoreClipEnabled=1, -VoxelWaterShoreClip=0 control, engagement log in
  both positions, placed BEFORE the -VoxelWaterMatScalar probe so the documented
  WaterShoreClipEnabled:0 spelling still wins, ribbons/near-field stay on the bare asset.
  Phase C's build-lane contribution: fixed the two comments the always-created MID
  falsified (BeginPlay's "only when the switch is passed / control arm byte-identical" and
  the header's "UNSET = NO instance created") -- control arms are now VALUE-identical, not
  instance-free, and the comments say to judge arms by logged parameter values. Rebuilt
  green.
- **Known limits, stated**: (1) WaterSurfaceZAtWorld / the near field inside a tidal pool
  answer through the BAKED extent mask -- the datum is tidal but fringe columns between the
  baked and at-datum outlines read dry to a probe (the sheet draws them; scoped per plan C4
  "near field: change nothing"). (2) A tidal basin whose sill leaves the +-480 m window
  de-qualifies until the camera returns -- window-bounded authority, the same bound as the
  ripple and bathy windows carry. (3) Ground absent from the fine set reads as sea level
  (the standing absence-reads-as-air class), so connectivity across unbaked ground at
  positive tide can over-connect; the deep-margin guard stops it SEEDING there. (4) A
  25 mm step nudges tidal sheets as a transform; only >50 UU of cumulative drift re-cuts
  the outline (kTideRebuildDriftUU's documented tolerance -- well inside the 1.875 m
  raster).

### SCOREBOARD (gates)

2026-09-04 | A | voxel-core tide tests | PASS 9/9 + mutation arm fails correctly | standalone clang run
2026-09-04 | D0 | artifactprobe --read --seeds --arms | PASS, 3/3 red arms fire | asset-forge tools
2026-09-04 | D0 | buildcheck --skip-heavy | PASS both craft (4 pre-existing coral warnings, untouched) | asset-forge tools
2026-09-04 | D0 | spec-hash isolation | PASS 828/828 unchanged, 2 red arms each move 828 | spec_hashes.json

### GPU OUTAGE (2026-09-04, unresolved — reboot required)

Every GPU-touching editor launch fails: D3D12 device creates, then CreateCommandQueue
returns DXGI_ERROR_DEVICE_REMOVED; under -dx11 the RX 7800 XT is not enumerable at all
(only Microsoft Basic Render Driver). No code/config delta vs the last good run (Sep 2
log byte-compared at RHI init). Tried: Win+Ctrl+Shift+B via keybd_event AND SendInput
(8/8 delivered, scancode variant too) — ineffective; pnputil /restart-device — access
denied (no elevation); elevated scheduled task — blocked; D3D11 fallback — no adapter.
Owner is remote for ~2 days and chose NOT to reboot (no auto-logon: a reboot strands
the box at the lock screen with no agent). ALL material regen + legs + captures are
PARKED. After reboot: run tools/voxel-sky-chain-regen.ps1 (sky+dome already done; it
reruns everything, fine), then the leg checklist in SCOREBOARD "pending GPU" rows.

### Tide-gate coastal sites (coarse-derived 30 m/px; RE-VERIFY with -VoxelOceanSurvey before captures)

Current 15-tile fine set has FIVE coastline tiles, all isolated (no baked neighbours):
(-15,-7) 53.7% ocean, (-11,-6) 13.2%, (-8,-11) 33.0%, (-7,-7) 56.8%, (-5,-7) 6.9%.
Sites >=4.1 km inside their own tile:
  PRIMARY  (-15,-7): world m (-224715,-100215)  water -1 m, land to +7 m  — flattest
           shore = max waterline movement per tide mm; rock-pool candidate terrain.
  SCENIC   (-7,-7):  world m (-97785,-99975)    water -1 m, land to +31 m backdrop.
  BACKUP   (-8,-11): world m (-117225,-160185)  water -4 m, land to +25 m.
(-11,-6) and (-5,-7) coasts hug tile edges — unusable. The old ocean-captures doc
sites are in UNBAKED tiles under namespace -b5e821e98 — invalid, do not use.
Fine-tile python decode measures RESIDUALS vs the coarse parent (quant=1 mm) — never
scan fine tiles for ground truth; coarse tier or engine survey only.

### Review pass over wave-1 code (2026-09-04, read-only agent; all fixes applied same day)

Ten findings, all verified against code, all fixed except one half deferred:
1. SEVERE: shore clip would delete every river/pool/near-field surface inside the
   960 m bathy window — bathy validity is NOT a wet test (lake-only plane, dry=-100 m
   with B=1; all three non-sheet consumers share the bare asset). FIX: asset default
   WaterShoreClipEnabled=0 (clip is opt-in). DEFERRED HALF: the lake sheet's MID with
   clip=1 (VoxelWaterSheetActor is Phase C's file; add after it lands — until then the
   black-band fix is authored but inert).
2. SEVERE: M_Ocean's clip made the sea's visibility depend on the -VoxelBathyOcean
   control arm + streamer existence. FIX: OceanShoreClipEnabled default 0 (the plane
   never needed it — terrain occludes it by depth test).
3. Beached-boat exit placed the pawn at waterline inside the bank. FIX: max(surface,
   boat resting Z) + lift.
4. Buoyancy-off arm left stale wet flags — drive/wake kept running underwater,
   corrupting the A arm. FIX: clear bProbeWasSubmerged with bProbeSeen.
5. Missing engine cube = silent invisible boat with nonzero instance count. FIX:
   Error log.
6. Glider placeholder at 25 mm = 89k cells → stride-3 sieve. FIX: fallback pitch 50 mm.
7. Ocean SetActorLocation dirtied a 40k-vert PMC every frame. FIX: gate on change.
8. Boat spawn-Z expression read as keel but wasn't. FIX: surface − draft − keelOffset.
9. EndPlay re-possessed a tearing-down pawn on quit/PIE-end. FIX: Destroyed-only.
10. NoseOffsetUU comment said span, is chord. FIX: comment. Plus the stale
   FVoxelWindSample "+X east" comment corrected to the X=north authority.
VERIFIED CORRECT by hand in the same pass: the 2:1 ring-seam index math (exact vertex
coincidence at (−9600,−9450); tri/vert counts reproduce the table), buoyancy constant
K (rest draft lands exactly at 12 cm; ζ=0.85 as claimed), wind axis use, aero signs,
ISM local-space instancing, WPO fade wiring, swept-splat subdivision.
NOTE: rebuild after Phase C lands covers these fixes (build lane is Phase C's until
it reports).
### SCOREBOARD (gates), continued -- wave 2 / Phase C

2026-09-04 | C | voxel-core full suite (incl. 7 oceanconnect + 5 TidalDatumSource + 4 C4 extent/doctrine tests) | PASS 825/825, 0 skips (golden fixture resolved; a wettable dry row was found) | build-main ninja clang, exit 0
2026-09-04 | C | oceanconnect + tidal standalone clang (scratchpad) | PASS 12/12 | llvm-mingw clang++ -std=c++20
2026-09-04 | C | red arm: BFS mutated to 8-connected | FAILS as required (3 checks, exit 1) | scratchpad mutation build
2026-09-04 | C | red arm: deep-margin seed guard removed | FAILS as required (3 checks, exit 1) | scratchpad mutation build
2026-09-04 | C | red arm: TidalDatumSource passthrough removed | FAILS as required (21 checks, exit 1) | scratchpad mutation build
2026-09-04 | C | doctrine pin: draw over-cover vs despawn under-cover asserted UNEQUAL | PASS (asserting them equal fails) | test_lakes.cpp
2026-09-04 | C | full UBT build | PASS 'Result: Succeeded', coherence proved, 0 warnings | tools/voxel-build.ps1 -Verify -AllowDirty
2026-09-04 | C | coastal leg: OceanConnect seeds>0, recomputes>0, Tide steps>0, inlandDeltaMm==0 | PENDING GPU (D3D12 device removed until reboot) | voxel-run-flight-leg.ps1 at a surveyed shoreline, Tide 1
2026-09-04 | C | pool probe submerged at BOTH ForceOffsetMm +1500 and -1500 (pool held) | PENDING GPU | pinned-tide leg pair at a known pool
2026-09-04 | C | fluid conservation leg across a forced tide sweep | PENDING GPU | fluid leg + ReconcileScalars line
2026-09-04 | C | groundFillMs per recentre (16,384 amplifier columns) | PENDING GPU -- unmeasured; if tens of ms, band the fill across ticks | census line
2026-09-04 | C | owner screenshots: rock pool flooded at high / proud at low; ebb+flood shoreline pair | PENDING GPU + owner verdict | capture legs, present conditions only

### Sheet shore-clip MID (2026-09-04, coordinator, closes review finding #1's deferred half)

AVoxelWaterSheetActor::BeginPlay now unconditionally creates the sheet MID and sets
WaterShoreClipEnabled=1 (asset default stays 0; ribbons/near-field stay on the bare
asset). `-VoxelWaterShoreClip=0` is the control arm; armed/disarmed logged in both
positions. The black-band fix is now LIVE on sheets pending the material regen +
owner capture at the documented grazing-angle repro pose (pending GPU).

### NullRHI functional-leg probe: REFUTED (2026-09-04)

Tried running the log-based engagement gates (tide/connectivity) under `-game
-nullrhi` to dodge the GPU wedge: the engine asserts at GlobalShader.h:201
Shader.IsValid() during boot — this project's GPU pipeline has global shaders with
no null-platform permutation, and the crash lands before the water subsystem arms.
VOID probe, not a failed gate. Do not re-try; every engagement gate stays behind
the reboot.

### Settings rows (2026-09-04): Water Wave Detail + Ocean Mesh Detail

Both rows shipped per the settings-panel policy, defaults ON (= today's behaviour;
owner image verdicts pending GPU set the shipped defaults). BuildSettingsPanel
refactored to a row-builder table at four rows (its own comment's threshold).
New cvars: voxel.Water.WaveTessRadiusM (float 80; -VoxelWaveTessM now sets it),
voxel.Ocean.HalfDetail (int 0; 1 = 3 m seed cell, one ring fewer, same asserted
reach; rebuild-on-change with a verts-before/after log). Fixed en route: sheet
staleness was LodKey-only, so a parked-camera toggle changed nothing forever —
FSheet::TessRadiusM added and carried across adoption. ApplyAll now WARNS on
cvars FindConsoleVariable cannot resolve (a missing cvar used to log identically
to a working apply). Build: Result: Succeeded + coherence. FrontEnd automation
tests skipped (need an RHI — GPU-parked).

2026-09-05 | D0 | owner verdict on canoe+glider via the 3D review artifact: "That
looks great" — strake band KEPT, sail steps ACCEPTED at 25 mm; both specs stand
as authored (curation still 'draft' until an in-engine capture confirms).

### GPU recovery + material chain landed (2026-09-05, post-reboot)

Reboot at 15:17 cleared the wedge: the RX 7800 XT enumerates, D3D12 RHI creates,
and the exact failing path (-AllowCommandletRendering ripple commandlet) runs
clean. Two new traps surfaced and were cleared on the way to a full chain:

1. **create_ripple_field_materials.py is no longer idempotent.** Its
   delete-then-recreate of the RT_VoxelRipple* render targets fails on EVERY
   run where the files exist -- delete_asset AND delete_loaded_asset both
   decline (the failing logs show M_VoxelRippleStep loaded and compiling in the
   commandlet before the Python delete runs; a loaded referencer pins the RT).
   The subsystem is correctly gated out of commandlet worlds, so the load comes
   from editor boot itself. WORKAROUND that unblocked today: hand-delete
   ue-project/Content/Voxel/RT_VoxelRipple{StateA,StateB,Field}.uasset (all
   git-tracked; the generator recreates them) BEFORE the chain. Every future
   full-chain run needs the same pre-delete until the generator deletes its
   referencing materials (M_VoxelRippleStep/Derive) before its render targets.
2. **The pinned verification pose now requires fine tile (-5,-3).** The
   chain's capture died on the fine-tier gate leak (fatal under -unattended):
   streaming's desired-set enumeration (RecomputeDesiredSet ->
   surfaceLowerBoundMm -> biomeTreelineMm -- NOT water code) queried ~8.2 km
   from camera into the unbaked neighbour. New since the clipmap-v3/cascade-8km
   merges (Sep 2-4); the chain capture never ran after them (GPU wedge same
   day). Fix in flight: bake (-5,-3) via terrain-service
   bake_tiles_from_cache.py (additive coverage, same namespace, no re-key).
   **Consequence for the leg checklist: the coastal tide sites are ALL in
   isolated tiles; any unattended coastal run within ~8 km of an unbaked
   neighbour dies the same way.** Engagement legs pass -VoxelFineTileGateFatal=0
   (log gates don't care that a distant unbaked neighbour reads sea level);
   owner-capture sites get their in-frame neighbours baked first.
3. voxel-capture.ps1 crashed under StrictMode on `$loaded.Count` when a log
   has no 'Voxel streaming: loaded=' lines (the gate-leak crash produced
   exactly that) -- masked the real error. Fixed: `@($loaded).Count`.

All eight generators then completed OK (ripple/water/ocean/underwater/voxel/
clipmap now rebuilt against the new MPC for the first time since the B2/B4/B5
material work), voxelcore.lib + module relinked, 'Result: Succeeded'. The
capture-and-diff verification is PENDING the (-5,-3) bake; chain -CaptureOnly
rerun after it.

### SCOREBOARD (gates), continued -- 2026-09-05 post-reboot legs

2026-09-05 | A | site re-verify: -VoxelOceanSurvey=80 at PRIMARY (-224715,-100215) | PASS -- ground crosses 0 in-grid (min -1.21 m NW sea floor, shoreline across the SE quadrant to +7.39 m); spawn column ground -0.6 m; site is the flat shore the notes promised | capture-tideA-survey.log, VoxelVerify00676.png
2026-09-05 | A | high pin engagement (Tide 1, ForceOffsetMm 1500) | PASS -- 'Tide ENABLED' (spec 720:800:0,360:250:250, maxMm 1050, sky-epoch, mpcParams=1); 'Tide: steps=1'; 'Ocean: sea surface moved 0.0 -> 150.0 UU' | capture-tideA-highprobe.log
2026-09-05 | A | probe underwater at high pin | PASS -- 'Ocean: camera entered water', camera z=1.31 m vs sea 1.5 m, depth 0.19 m, M_Underwater treatment live | same log, VoxelVerify00678.png (underwater frame)
2026-09-05 | C | coastal leg OceanConnect engagement | PASS -- seeds=203 wet=12450 connected=12344 recomputes=2 inlandDeltaMm=0 bypassedInland=1 recentres=1 (fired on the high-pin leg; tide-armed run at a surveyed shoreline) | capture-tideA-highprobe.log
2026-09-05 | C | groundFillMs per recentre (16,384 amplifier columns) | MEASURED 21.11 ms -- once per recentre, not per tick; banding decision now has its number (owner call whether 21 ms per recentre warrants it) | same log
2026-09-05 | leg-infra | coastal legs run with -VoxelFineTileGateFatal=0 -- PRIMARY's neighbours were unbaked (the (-15,-8) leak fired non-fatally as predicted); (-15,-8) and (-16,-7) baked this session for the owner shoreline captures; (-5,-3)+(-4,-3) baked for the pinned verify/lakeshore pose | bake_tiles_from_cache runs, all exit 0

2026-09-05 | A | probe dry at low pin (flip complete) | PASS -- 'sea surface moved 0.0 -> -150.0 UU', steps=1, ZERO 'entered water' lines at the same pose that submerged at +1500 | capture-tideA-lowprobe.log, VoxelVerify00680.png
2026-09-05 | A | off arm (Tide 0) vs no-cvar baseline, water log lines | PASS -- state-identical (every OceanConnect/BathyField state field equal, inlandDeltaMm=0 both); only two wall-clock timing fields differ (fill ms, groundFillMs), which no two runs can share | offarm-water.txt vs baseline-water.txt diff
2026-09-05 | C | OceanConnect armed independent of tide, identical in both dark arms (seeds=228 wet=8876 connected=8806 recomputes=1) | noted -- the byte-identical control for OceanConnect=0 remains unrun (separate row) | same diff

2026-09-05 | chain | load-time material compile check (fresh editor, all assets) | PASS -- zero 'Failed to compile Material' in the verification run; the 2026-08-10 failure class is excluded | capture-skychain-verify.log
2026-09-05 | chain | pinned-pose picture diff vs VoxelVerify00534 | 99.42% diffuse -- NOT the default-material failure (compile check green, change diffuse and water-concentrated). The reference froze the owner's LAST SIGNED-OFF water (2026-08-12) and predates wind waves + this wave's B1/B2/B4/B5 water redesign at a lake-dominated pose; the diff is the redesign's expected signature. Reference NOT re-pinned -- owner re-signs with the new captures (VoxelVerify00686 is the first candidate), then the chain re-pins | VoxelVerify00534 vs 00686

2026-09-05 | B4 | black-band repro pose A/B (80 m pitch -18, alpine (-54233,-68221)) | PASS -- control (-VoxelWaterShoreClip=0): 25,630 dark px (maxRGB<60/frame; doc's historical 27,003 -- band reproduces); armed (default): 225 (~the -VoxelLakeSheets=0 floor of 49). 'shore-SDF clip ARMED/DISARMED' logged in both arms. Owner visual verdict pending | VoxelVerify00688 (armed) vs 00690 (control)
2026-09-05 | B1 | wave tessellation engagement at the signed-off lake pose | PASS -- 'wave tessellation ENABLED', '1 of 648 basin(s) tessellated, 416 vertex(es) inside the 80 m disc', DRAINED build 10,448 tess verts. (The alpine B4 pose logged 0 tess verts -- pose artifact: no finest-band lake rect within 80 m XY of that camera, not an engagement failure) | capture-skychain-verify.log

### Owner rulings (2026-09-05, in-session)

- **groundFillMs banding: NOT NEEDED** ("I accept your recommendation") -- 21.1 ms
  once per recentre stands as-is; no banding machinery.
- **Ocean must not read flat**: "Sea should not be flat - it should have waves,
  reflection, etc to look realistic." The promoted water_sky_reflection_graph
  module is being wired into create_ocean_material.py; ocean-only regen
  (chain -Only) + first ocean captures follow.
- Canoe + glider: approved by the owner in-session ("Fine in the canoe and
  glider"); curation flips to approved on the asset-forge side. The D-gate
  in-engine capture still runs as planned.
- ADR-0006 / ADR-0007: owner requested a tradeoffs briefing + obsolescence check
  against the recent perf/tide work before deciding; research in flight.

2026-09-05 | A | OWNER VERDICT on the shoreline pair (VoxelVerify00692 high / 00694 low) | PASS -- "Yes there is water in A at high tide and no water in B at low tide" -- the waterline moves | owner, in session
2026-09-05 | B4 | OWNER VERDICT on the black-band pair (00688 armed / 00690 control) | PARTIAL -- "Black band is mostly gone but there is still black/dark shading and slight band around the pond." Follow-up queued: BathyShoreClipSlackM ladder at the repro pose + a wet-shore-darkening-off arm, so the intentional wet-sand ring can be told apart from residual band before any retune | owner, in session

2026-09-05 | B4 | slack ladder, on-pose (camera-drift run voided by SHUTTER-pose check and rerun) | dark px at maxRGB<60: +0.9 default=225, 0.0=73, -0.5=22, -2.0=0 -- monotonic; the residual band is ENTIRELY sheet overhang (wet-shore darkening exonerated: nothing dark remains at -2.0). Owner judgment on the tradeoff (band vs dry rim up close) pending | VoxelVerify00688/00710/00708/00706

### OWNER DIRECTIVE (2026-09-05): water is too transparent globally

"Water is too transparent and see through globally. Even very shallow water
bodies should clearly have a surface that looks like water from distance."
Opened as an appearance-tuning pass on the shared optics: the surface needs a
minimum presence (reflection/opacity floor) that does not vanish in shallow
water at distance. Knob inventory first, then a judged ladder at the 00698
tidal-flats pose (the frame that prompted the verdict) + the signed-off-lake
pose. Note the direction of travel reverses part of the 2026-08-12 "clarity
dialled back" tuning -- the reference-pin question stays owned by the owner.

2026-09-05 | transparency | OWNER VERDICT on SurfacePresence 1.0 pairs | REJECTED -- flats: "00698 honestly looked better with its green coloring. But however still look too transparent. 00714 looks very transparent at a distance"; lake: "00686 looks better than 00716". The sky sheen washes the body colour pale, which reads as MORE transparent. Default returns to 0.0 (lever kept for later fine-tuning). Redirect: slant-path (1/NoV) correction on the absorption/in-scatter path length, so shallow water saturates toward its own deep-water green at grazing distance -- the physically-true fix matching both statements. In flight | VoxelVerify00698/00714, 00686/00716

2026-09-05 | transparency | OWNER VERDICT on the slant-path flats pair | APPROVED -- "00720 looks much better". The secant slant-path default (BathyRefractInvN2=1.0) SHIPS; SurfacePresence stays 0.0. Lake pose: "00686 looks pretty much the same as 00722. No preference. Proceed" -- the approved close-up look is confirmed preserved, as designed | VoxelVerify00720, 00722

### Coast 2.5 fps regression: found and fixed (2026-09-05 evening)

Owner hit 2.5 fps / 420 ms in-editor near the coast. NOT water/materials: the
hitch census attributed 410-445 ms/frame to RecomputeDesiredSet's R7 entry
scan -- 82 footprints re-derived at ~5 ms each EVERY frame, because the
footprint z-range memo refuses to cache answers computed over non-resident
ground (correct for a tile mid-decode) and R7's 8 km reach crosses tiles
ABSENT from the baked set (permanently non-resident) at any coastal camera.
Introduced by clipmap-v3/cascade-8km's reach (Sep 2-4 merges); their legs ran
at the interior default spawn and never crossed the baked edge. FIX: the fine
streamer now publishes a ResidencyEpoch (bumped on tile arrival and
missing-memo reset); non-resident z-range answers are cached STAMPED with it
and die exactly when re-asking could differ. Verified at the identical
underwater pool pose: post-warmup p50 387.8 -> 8.52 ms, p95 400 -> 10.38 ms,
hitches 305 -> 21. uwperf-under/above/fixed legs on record.

2026-09-05 | C | pool probe submerged at high pin (+1500) | PASS -- 'entered water', depth 1.16 m, basinsFlagged=2, inlandDeltaMm=0 | capture-c-pool-probe-high.log
2026-09-05 | perf | coast frame time post-epoch-fix | p50 8.52 ms / p95 10.38 ms static underwater at the pool | uwperf-fixed.log

2026-09-05 | C | POOL HELD: probe submerged at BOTH pins (same pose, world (-117270,-160295) in tile (-8,-11)) | PASS -- high: 'entered water' depth 1.16 m; LOW: 'entered water' with the sea at -1.5 m, 1.5 m below the pool floor -- the tidal datum held the pool at its sill; basinsFlagged=2, inlandDeltaMm=0 in both | capture-c-pool-probe-{high,low}.log, VoxelVerify00728/00730

2026-09-05 | F1 | OWNER VERDICT on first caustics-era lake frame | "00734 looks good" -- the full-chain regen (caustics + WaveTimeScale collection), slant-path water, and the appearance-MPC push land owner-approved at the signed-off pose | VoxelVerify00734
2026-09-05 | lighting | voxel.Shadow.March default 0 -> 1 by owner ruling ("Ship voxel.shadow.March on so I can evaluate and judge"), with the 2026-08-23 ~13 ms cost figure re-surfaced at ruling time | VoxelShadowMarch.cpp; rides next build

### Craftable-items category (2026-09-05, owner request)

asset-forge gained an explicit taxonomy: environment / creature / CRAFTABLE
(forge/categories.py; canoe + glider are the members via the artifact kind).
Category defaults from the kind, per-spec override exists (unused, documented).
Hash-neutral PROVEN (0/830 moved with exclusion on; red arm 830/830 with it off);
geometry byte-identical across 826 builds; import-time asserts pin the taxonomy
to KINDS_ON_SCATTER/KINDS_ENTITY/KIND_ORDER. Query seam for the future crafting
system: library/categories.json + GET /api/categories + categories.members() —
all through one resolver. Web app rebuilt (grouped kind menu, craftable filter);
stale-dist debt closed. OWNER OPTION: rename the three categories (one tuple +
regenerate). Open: 'craftable' = makeable, not recipe-bearing.

### F7 determinism boundary (authored) + determinism pins (2026-09-05)

The written per-client/deterministic boundary list F7 requires lives at
[water-determinism-boundary.md](water-determinism-boundary.md) (own file so
plan-doc appends cannot collide with it): deterministic-by-construction (tide,
connectivity BFS, TidalDatumSource, WaterSurfaceZAtWorld inputs, buoyancy's
column contract) each with its mechanism; per-client-by-doctrine (ripples,
foam, caustics, WPO, wakes) each with the rule that protects it; and the
sky-epoch prerequisite's current state (implemented via AVoxelEditRelay, see
VoxelSkySubsystem.h:35-61).

2026-09-05 | F7 | whole-sweep determinism pins (tests/test_waterdeterminism.cpp): tideOffsetMm/quantiseTideMm/tideVelocityUmPerS over 4 specs x epochs to +-4e15 ms + oceanConnectivityFill bits+stats over 3 fixture families x 6 tide levels, double-evaluated in-process (independently-constructed state, byte-compared) and pinned to golden FNV-1a-64 literals (cross-process contract, same sense as test_tide.cpp's pinned table values) | PASS -- suite green; red arms: libm-sin tide rewrite and 8-connected fill both moved the goldens and failed the pins in scratch builds | vxc_tests
