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

## Phase F8 — Weather-water cohesion (owner directive, 2026-09-05 late)

Owner: "How if at all is the weather system wired into this session's water
work? We need to connect it cohesively in a way that makes sense." Ruled after
the racing-surface hunt landed on weather wind-direction drift.

**F8.1 (the found bug, first): wind-direction drift must not race the waves.**
Wave phase is wind-direction dotted with ABSOLUTE world position; slow synoptic
rotation sweeps phase at |pos| x dOmega -- tens of m/s of pattern motion at the
world's playable radii, present since live wind landed (~08-27), immune to
every time knob by construction. Fix is material-side in water_wave_graph
(direction quantisation with hysteresis + crossfade between direction bins, or
local phase anchoring -- designer's call, must kill the lever arm), possibly
plus an engine-side direction tau. Owner's interim state: the wind pins.

**F8.2 Gust channel -> foam/spray.** The unsmoothed gust channel was published
"for spray and foam, which SHOULD twitch, and nothing reads it yet". Foam v2's
coverage should read sustained+gust while the wave field keeps smoothed
sustained only -- the twitchy channel feeding the thing designed to twitch.

**F8.3 Rain (weather has NO rain model yet -- the gap is weather-side).** The
water stack is already rain-ready downstream: basin ledger rain credits exist
and the tidal datum max()es against them by design. When weather grows rain:
credit basins through the existing ledger rain path; splat raindrop rings into
the ripple field inside its window; a rain-streak surface/underwater material
term behind the quality row. Coordinate with the CA rework (backlog §12).

**F8.4 Wind felt at sea (small):** light lateral wind force on floating hulls
(boat drifts downwind at rest); glider already flies the shared wind.

**F8.5 Storm surge (optional, powerful):** a weather low-pressure term as a
SECOND datum overlay through the same TidalDatumSource seam -- architecture
already supports it ("tide is a datum overlay, never a credit"); surge is just
another overlay, inheriting determinism from the weather epoch. Decide scope
when weather grows storms.

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

2026-09-05 | F8.1 | OWNER VERDICT: wave speed at pinned wind | PASS -- "Step 1 passes": WaveTimeScale 1.0 reads correctly once direction drift is dead; the earlier "too fast" was the racing contamination. WaveTimeScale default STAYS 1.0 | live session
2026-09-05 | D | OWNER VERDICT on the boat, live session | FAIL x3 -- (1) no wake ripples at all; (2) no bobbing ("does not seem affected by buoyancy dynamics at all") -- overrules D2's flat-datum v1 decision, activating the recorded CPU-wave-mirror stretch; (3) water clips through the hull interior -- new work item: water exclusion masking (hull interior stencil -> all water materials discard). voxel.Boat.Stat output pending to split (1)/(2) between dry-probes bug vs designed-flat | live session; fixes in flight

2026-09-05 | E | OWNER VERDICT on the glider, live session | FAIL/RESPEC -- spawn produces an autonomous glider sailing down in a straight line; E cannot board it (v1 shipped X-deploy-while-falling as the only interaction, boarding never wired); on touchdown it despawns or falls through the ground. OWNER SPEC: walk-up E boarding of a parked/landed glider (the boat's interaction model), and landed gliders REST ON THE SURFACE persistently | live session; vehicles fixes in flight

### Late-session verdicts and closures (2026-09-06, owner live loop)

- **F8.1+speed-bin: racing CLOSED.** Owner: "Racing is not present. Only slow
  wave / surface effect that looks good." Both wind channels (direction AND
  wavelength/speed) proven piecewise-constant into the phase; PinMps
  discriminator confirmed the speed channel before the fix shipped.
- **Ripple deposit fix: PROVEN in-engine.** Debug-material frame shows wake
  wavefront arcs; 'field verified LIVE' in every session since. The 08-13
  shore-mask annihilation (splat added before the mask multiply, mask exactly
  0 at the waterline) is dead; MaskFloor absorbs instead of deleting.
- **Hull exclusion: owner-confirmed fixed** after two fits (box inside the
  taper; top dropped to gunwale+10 after the camera-dependent side-box
  report). Cockpit dry; bow/stern sliver deliberate; hull-shaped mesh = v2.
- **Wake visibility: root cause is MISSING ART, not physics.** All gains
  ineffective because the composite spends the field only on ~degrees of
  normal tilt + cm of WPO. Disturbance-foam term (whitewater from |ripple
  grad|+|height| into the existing foam stacks, both waters) in flight.
- **WaveBobGain default 6.0** (owner), WakeGain 3.0, exaggerated by design.
- **Leg doctrine correction (owner ruling): the canoe/wake/ripple loop is
  owner-tested in-editor from here** — no more scripted legs/screenshots for
  this feature family; the night's mistimed-shutter legs (wake half-life 5 s
  vs shutters ~40 s late, beachings) are the recorded reason.
- Boarding UX note for the vehicles backlog: the 4 m interact range measured
  in 3D from the pawn is tight at water level (failed by 11 cm with the pawn
  4 m underwater at a terrain-relative spawn); consider 2D distance or 6 m.

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

## Session 9 verdicts + THE FOAM CHANNEL FINDING (2026-09-06)

Owner, session 9 (owner-water-session9.log): bobbing GOOD, white-box artefact
GONE, but NO wake, NO player rings, and NEW clipping at the cockpit ENDS
("because of the more drastic bobbing").

**The foam finding.** His own session log shows the ripple field LIVE (max
field 0.1901, height 0.044 m, 6 injections, publish on) while he saw nothing —
so the break is in the material's last hop, and the 2026-08-30 note in
create_water_voxel_material.py already measured it: a BaseColor rewire on this
SLW material is a byte-identical NULL on the pond. EVERY foam signal lands
only on BaseColor. The art has been painted onto a dead channel all along.
Fix: disturbance foam now ALSO rides EMISSIVE (owner-verified visible channel
— he judges sky reflections there every session) in both waters, tinted like
the BaseColor foam, behind a new baked scalar DisturbanceFoamEmissive (0.6),
inheriting every upstream gate so all off arms hold. The BaseColor path stays
(harmless, and right if SLW ever honours it).

**Player rings.** Nothing ever injected for the player — VoxelRipple's own
tuning comment ("wading in still makes something rather than nothing") was
written for a caller that didn't exist. Wired: VoxelCharacterMovement sweeps a
disturbance (0.45 m, 0.020 m × the standard speed ramp) whenever the waterline
crosses the body (feet wet, head+0.5 m dry — deep divers excluded).

**Ends clipping.** WaveBobGain 6.0 moves the boat 6x the rendered surface, so
troughs push water past the waterline+10 lid, and the ends past 0.62L had no
box at all. Fix: both exclusion lids raised to adopted-gunwale-6 (geometric
proof in VoxelBoat.cpp that this cannot re-create the beside-hull box), plus a
second ends box 0.92L x 0.26B inside the taper.

NEXT: owner closes editor -> build + water-only regen -> relaunch -> owner
judges: wake wedge, wading rings, dry ends. Backlog §13 charge collapse is
with an Opus agent in parallel.

## Wake hunt state at 2026-09-06 17:30 (blocked on the box; Codex holds the editor)

WHAT IS PROVEN, and none of it depends on framing or timing:
- Injection: 20,778 with zero drops (owner's own Ripple.Stat).
- Deposit: voxel.Water.Ripple.Dump reads the whole 512x512 back and finds the
  ring at the EXACT world position dropped (within 1 cm), 6.72% of texels
  non-zero, and PER CHANNEL maxR=1.666 maxG=1.666 (gradient, what the material
  consumes) maxB=1.872 (height). The simulation is healthy end to end.
- Binding: TestFill writes a constant into the same UObject the subsystem draws
  into; the material renders it saturated. Same texture, and the gain/edge-fade
  chain amplifies correctly.
- Emissive pin: a constant renders blazing red. SLW shows our emissive.
- UV is LIVE: differencing the uv arm against the same-pose gradient arm gives
  R +0.471, G +0.318, B +0.035 -- exactly the shape of emissive=(u,v,0). Units
  check out too (kWindowUU = 5120 UU, published InvSize = 1/5120).
- Shore mask: CLEARED by a two-arm control (mask on/off both deposit 0.4998).
- Altitude/mip: cleared (blank at 2 m as well as 35 m).

WHAT IS NOT PROVEN: that a correctly framed ripple is invisible. Several
"blank" captures were MY framing errors (DropHere puts the splat under the
camera, which sits in the blind spot below a pitched-down view), and two of my
own debug arms turned out miswired -- fixeduv rendered nothing even against a
guaranteed uniform fill, and the first marker was 51 cm wide and unfindable.
Discard conclusions built on those.

NEXT, when the box frees, in order:
1. Marker arm at the WIDENED 0.08 uv falloff -- does this material's uv reach
   0.583 where the ring is? An offset shows as a displaced patch and is then
   measured rather than inferred.
2. If uv lands right: the sample reads the right texel and the ring must
   render, so re-run the gradient arm with a 120-step burst (the only drop form
   proven to leave 1.67 in the gradient) at a pose with the drop 6 m ahead.
3. Owner's near-water complaint is a SEPARATE, well-understood lever:
   water_optics.ABSORPTION_DISTANCE_M = 5.5 and SCATTERING_PER_M. Ladder
   -VoxelWaterMatScalar=AbsorptionDistanceM:5.5/3.0/2.0 at a low near-shore
   pose. No rebuild, no regen.

## 2026-09-07 morning: persistence regression found + fixed in the harness; uv ladder state

PERSISTENCE REGRESSION (affects the owner's launches, not just captures). The
session-persistence merge writes `<seed>.vxlog.checkpoints/` + detached
sidecars beside the .vxlog. The capture harness cleared only .vxlog/.vxwater,
so every launch after the first post-merge session found committed checkpoints
with no log -> loader: "No complete supported checkpoint generation could be
loaded" -> VoxelSessionCheckpoint::Fail -> BeginPlayerSession returns early ->
no player session, fine tiles stall at 1 (vs 4), and at low altitude the frame
is a VOID (camera in unloaded space). Fixed in tools/voxel-capture.ps1
(e90f7f7): the clear now includes the checkpoints dir and sidecars. Proven:
captures at 05:17/05:20 show refused=0, tiles=4 again. The loader's refusal is
left alone -- for a real player a vanished log under committed checkpoints IS
suspicious; that semantic belongs to the persistence lane. Note for the owner:
the fixture seed 20260719 is also his play seed.

INVALIDATED BY THE ABOVE (do not cite): shore-foam ladder frames 00820/00822
(different shoreline than baseline = stalled world), uvstep 0.55 frame 00824
and the T=0 "control" 00826 (both void). Withdrawn: "uv never reaches 0.55".

STILL STANDING, on healthy sessions: uvstep T=0.51 (00828) and T=0.52 (00830)
painted NO red/green anywhere across the lake. That is a real null only if the
T=0 control paints the whole lake; the control has not yet run on a healthy
session. NEXT (box permitting): T=0 control -> then re-run shore-foam ladder
(ship / ShelfHi 2.0 / ShelfHi 100 single-pair -- the two-pair "off" arm did not
echo its second scalar and is not trusted).

Also: Codex is running automation in a scratch checkout
(.scratch/environment-verification) that occupies the box; two of my captures
overlapped it. Visual validity holds (fixed pose, frozen sun); timing does not.

## 2026-09-07 02:00-02:40: params arm POSITIVE, shore ladder NULL, arm-B hang

**Wake -- the collection IS read.** The uvstep T=0 control did run on a healthy
session and painted nothing, which sent the search to "the material never sees
the published RippleFieldOrigin/InvSize/Gain". The `params` arm (emissive
R=origin.x/-1e7, G=invSize*5120, B=gain/2.5, frame VoxelVerify00840, pose
+2 m / -25 deg) measures near-water R/G = 0.72, mid 0.45, far 0.00 -- the far
red loss is the volume's absorption, the near ratio is the published
-6.5e6 UU origin arriving (0 would be pure cyan). Origin, inverse size and gain
all reach the pixel. WITHDRAWN: "unpublished collection". The ripple graph
(`ripple_field_graph.py:161-197`) builds uv = (AbsoluteWorldPosition.xy -
origin.xy) * invSize with WPT_EXCLUDE_ALL_SHADER_OFFSETS and samples
/Game/Voxel/RT_VoxelRippleField, the same asset the C++ derives into
(`VoxelRippleField.cpp:32,498`). Both remaining "uv is wrong" observations
(marker at 0.583 unfindable, uvstep T=0 blank) are my own instruments, three of
which have already been miswired, so the next arm is the plainest one: `uv`
(R=u, G=v) with a pixel read-back at the camera column, expected ~(0.5,0.5).
Running as this is written.

**Shore foam -- a genuine null, overrides confirmed.** Same lake, +6 m / -18 deg,
yaw 45, four arms, every override echoed by the sheet's own
"Lake sheets: material scalar '<name>' set to <v>" line:

| arm | override | frame |
|---|---|---|
| A ship | none | 00834 |
| B shelf2 | BathyFoamShelfHi 2.0 | 00836 |
| C gateoff | BathyFoamShelfLo 50, ShelfHi 100 | 00838 |
| D width6 | BathyFoamWidthM 6.0 | 00842 |

The four frames are indistinguishable. The white streaks in all of them are
SSR of the snow slope (they were there with SSR on and absent with it off on
2026-09-06), not foam. shore_foam = shore_band * shelf_gate * gain(0.55, SHORE
FX ON per the regen log) * bathy validity, and the bathy field was live
(window #1, holes 0.0%). A 6 m band at 0.55 through an open gate is not
invisible if it exists, so a FACTOR is zero along this shore -- most likely
shore_band (shore_m not measuring distance to THIS lake's edge) or validity.
WITHDRAWN: "the shelf gate structurally cannot pass on a 10 cm beach" -- the
gate was not the limiter. New `shorefoam` debug arm (R=shore_band,
G=shelf_gate, B=validity) queued behind the uv arm; frame will be shore-E-viz.

**Arm B hung once (frame counter stuck at 710 for 90 s), then re-ran clean.**
Not the harness: a silent 27-voxel edit at the pawn's column mobilised cavern
water into the CA and the level>=4 distant-edit mip re-mesh ran synchronously
on the game thread (L4 10 s, L5 79 s, L6 never). Written up as backlog §14 with
the timing table; it is the other half of §13.

**Harness note.** The origin has no fine tiles: any loading/in-game shot that
starts at (0,0) trips the FINE TIER GATE LEAK fatal. `-Shot Loading` needs
`-ExtraArgs '-VoxelSpawnAt=-61440,-61440'` (the 2026-08-25 runs had it).

### 03:10 addendum: the shore-foam factor paint (VoxelVerify00844)

`shorefoam` arm at the +6 m / -18 deg pose, R=shore_band, G=shelf_gate,
B=validity: the lake is flat blue. Measured: validity 1 everywhere on the sheet;
shelf_gate open (cyan) ONLY in one far patch at x>1518, y 300-610 (the gentle
bed at the far end); shore_band ZERO on every water pixel below the horizon at
the shipping 1.6 m width (0 magenta pixels, exhaustive scan). Reading: the
sheet's own shore-SDF clip hides the sheet where shore_m<0 and the opaque bank
stands above the first metres of water, so the 1.6 m band is entirely behind
terrain at this shore; the 6 m band (arm D) would have emerged but the shelf
gate was CLOSED there (bed steeper than 1:4 -- it is a stepped snow bank), and
the gate-open arm (C) still had the 1.6 m band. The two gates were never opened
together. Arm F (ShelfLo 50 / ShelfHi 100 / WidthM 6.0 in one override) is
queued behind the uv re-run; if it foams, the shore-foam term is alive and the
question becomes the owner's taste on width/shelf, not plumbing.

Interleaving lesson: two background chains that each idle-wait can interleave
inside each other's regen->capture->restore window. 02:42 uv regen was
overwritten by the 02:45 shorefoam regen; frame 00846 ("wake-uv") is SHIPPING
water, not a uv paint, and is void. Debug chains must be ONE serialized script.

## The cockpit-ends clipping: the mask now rides the WATER (2026-09-07)

Owner, 2026-09-06: *"water is clipping through the front and back ends of the
cockpit - this seems to be happening now because of the more drastic bobbing
and buoyancy."* The bobbing is approved and was not touched.

**The diagnosis the 09-06 fix missed.** v1's exclusion boxes were bolted to the
hull, with the lid at `keel + RestDraft + 10 UU` -- the hull's OWN rest
waterline. `voxel.Boat.WaveBobGain` is 6.0, so the probes ride a surface six
times the drawn one and the hull heaves and pitches several times further than
the water it floats in: at shipped defaults the drawn surface moves ~6 UU and
the hull ~36 UU, against 33 UU of freeboard. A lid measured from the hull is
therefore off by tens of UU at every trough, worst at the ends, where pitch
adds the most travel. Adding a second box (09-06) bought coverage in PLAN and
left the height wrong; raising the lid to gunwale-6 (also 09-06) was reverted
because it carved water outside the hull.

**Also corrected: the 09-06 explanation of that beside-hull artefact.** It
blamed the gunwale (a ray entering above the near rim, crossing the slab,
exiting past the far one). The gunwale does not enter it. A ray that reaches
water beyond the hull is above the water surface for its whole crossing, so it
can only pick the mask up where the mask rises ABOVE THE WATER, and the ring of
open water it then kills is `lid / tan(view depression)` wide. The lid's height
over the WATER is the whole artefact; where the boat is has nothing to do with
it. That is why gunwale-6 carved and why the fix is to hold a few centimetres
over the surface rather than anywhere over the boat.

**The fix (the plan's "hull-shaped mesh = v2", as a staircase).**
`AVoxelBoat::UpdateWaterExclusion`, solved every tick:

* SEVEN stations along the keel line out to 0.96 L, each taking the half-beam
  an elliptic canoe plan carries at its OUTER edge x0.85 -- so every station
  keeps planking between itself and the water, and the ends are covered instead
  of being left to a 0.26-beam rectangle.
* Each station is placed in ABSOLUTE world space with YAW ONLY, so its lid
  stays horizontal (a lid that inherits pitch is a ramp: clearing the water at
  its low end means standing tens of UU over it at the high end), and its
  footprint is scaled by the XY projection of the hull's axes, which is the
  foreshortening a pitched or rolled hull actually has.
* The lid sits `voxel.Boat.HullMaskLidUU` (6) above the DRAWN surface at that
  station -- the mirror at gain 1 WITH the WPO camera fade, i.e. the surface
  the pixels show, not the exaggerated one the probes ride. Same mirror, same
  clock, same published wind, same fingerprint guard as the buoyancy, so a
  stale mirror drops both to the flat datum together.
* A station HIDES (no stencil writer, water pixel-identical to no boat) when
  the drawn surface is below the hull bottom there -- a bow thrown clear by the
  bob has no water in it to cull and an absolute-placed mask left at the
  surface would carve the water the hull is flying over -- and when the column
  is CA-only/datum-less (the buoyancy's own documented blind spot), and when
  the boat is asleep past SleepRadiusUU.

Arms, per the house rule: `voxel.Boat.HullMask` 0 hides every station (the
contract's own off arm) and `voxel.Boat.HullMaskLidUU` dials the one number the
artefact scales with. `voxel.Boat.DebugDraw` now also draws the stations.

**Frames** (pond pose `-65102,-51084`, +8 m, pitch -20, yaw 0, wind pinned
8 m/s from 240, boat spawned 5 m ahead, 120 s settle, same build):

| arm | frame |
|---|---|
| `HullMask 0` (control: no writer) | `VoxelVerify00852.png` |
| `HullMask 1`, lid 10 UU | `VoxelVerify00854.png` |
| `HullMask 1`, lid 6 UU (shipped) | `VoxelVerify00860.png` |

Read: in the control the bow interior is a flat violet-grey sheet and the stern
is hazed over -- the lake drawn inside the hull at both ends, the owner's
report. In both mask arms the planking, ribs and floor are crisp from stem to
stern and no water is drawn inside the hull; no ring of missing water appears
outside it at either lid. (First reading of 00854 called the bright voxels off
the stern quarter "carved bed"; the zoom shows they are inboard of the sheer --
the boat's own sunlit interior, which the water film had been hiding.)

Stated limits: the frames are shot at ONE camera depression (~21 deg) and the
ring the lid can cause grows as the camera goes grazing, which is exactly the
owner's own screenshot angle -- the lid knob exists for that. Two runs at the
same settle do not reproduce the bob phase exactly (dt varies), so the arms are
the same pose, not the same instant. And these are scripted frames for a
feature family the owner ruled is owner-tested in-editor from 09-06: they are
evidence the mechanism engages, not a substitute for his judgement.

### 04:30 addendum: uv arm landed (VoxelVerify00866) -- not discriminating

Third attempt (the first two were overwritten/refused by other launches; frames
00846 and 00856 are SHIPPING water and void). Regen log marker for this arm is
"RIPPLE UV TEST: ON" (not "UV ARM"). Read-back at +2 m / -25 deg: bottom band
R 134/153/163/165/163/159/150/127 across eight x-bins, G 201-215 -- symmetric
left-right, falling only with distance; centre column R 0 -> 164 top to bottom.
That is a constant emissive under the volume's absorption, indistinguishable
from a correct uv (0.5,0.5) because the near field spans ~3 m of a 51.2 m window
(delta-u ~0.06 = 8 levels, under the absorption gradient). Consistent with the
params arm; does NOT locate the fault. Handed to the material-owning agent with
the remaining hypotheses (texture-parameter binding on the sheet MID, V
orientation between derive and sample, WaveTimeScale gate at runtime), each to
get one discriminating instrument with framing that provably contains the
disturbance. Shipping material restored 04:24:31, proven by the regen log.
Combined shore arm F (00864) is identical to shipping: shore foam is also with
that agent.

### 05:20 CORRECTION: the "+2 m / -25 deg" lake pose is UNDER WATER

The R6/R7 agent read it off the log: "Ocean: camera entered water ...
submerged depth 3.32 m, treatment=M_Underwater". The harness's altitude is
measured from ground top (the lakebed, 1644.2 m); the lake surface stands
5.32 m above that, so +2 m is 3.3 m below the surface and +6 m is 0.7 m above
it. Every ripple debug frame taken at +2 m -- params (00840), uv (00866), and
the earlier uvstep / marker / height / gradient arms -- was drawn by the
underwater post-process and never contained M_WaterVoxel's emissive.
WITHDRAWN: "the material receives the published collection values" (the pale
cyan was underwater fog) and "the uv arm is non-discriminating". The read side
of the wake is UNTESTED, not exonerated. Rule from here: a water-surface arm
must be shot from above the surface (+6 m / -18 deg, or a pose computed against
the surface height) and the capture log must carry no "camera entered water"
line before the shutter. Handed to the material-owning agent with the arm
order const -> params -> texture-sample-with-disturbance.

## 05:35 SHORE FOAM: SOLVED AT THE MECHANISM LEVEL. The band is not too narrow — the baked shoreline is 6-40 m from the drawn water's edge

Material-owning agent, 2026-09-07. **New instrument, new arm:
`VOXEL_WATER_RIPPLE_DEBUG=shoredist`** in `create_water_voxel_material.py`
(log marker `SHOREDIST ARM: ON` — add it to the contamination list).

**Why a new arm was needed.** The 03:10 factor paint established
`shore_band == 0` and the search stopped there, but `shore_band` is a product of
two terms that fail in *opposite* directions and both paint black:

    shore_band = (1 - ramp(shore_m + noise, 0, width))   <- 0 when shore_m is BEYOND the band
               * saturate(8 * shore_m)                   <- 0 when shore_m <= 0 (sign test)

"No red" was therefore consistent with the visible water being far from the
shoreline *and* with the bake calling every visible water pixel dry land. Those
have opposite fixes. The new arm paints `shore_m` itself, in **binary** (the
lesson recorded on the `uvstep` arm: the tonemapper defeats reading a ramp off a
PNG), so the measurement is where an EDGE is, not what shade a pixel is:

* R = 1 where `shore_m > 0`
* G = 1 where `0 < shore_m < BathyFoamWidthM` — the live scalar, so the green
  ribbon is a **runtime ladder on one baked material**
* B = 1 where `shore_m > 6 m`

**Frames** (all `+6 m / -18 deg`, above the surface, `camera entered water` count
**0** in all three logs): `VoxelVerify00896` (width 1.6, shipping),
`VoxelVerify00898` (width 6), `VoxelVerify00900` (width 40). Compared per band
against each other — 00900 minus 00896 isolates the green channel exactly,
because that is the only term the width changes.

| band | mean G, width 1.6 | width 6 | width 40 |
|---|---|---|---|
| strip at the visible waterline | 177.3 | 177.8 | 206.5 |
| far water | 174.8 | 174.9 | 217.4 |
| mid | 98.4 | 98.4 | 201.8 |
| near | 98.0 | 98.0 | 201.4 |

**The result, and it is unambiguous:**

1. **R is on over 100% of the water. `shore_m > 0` everywhere — the sign test is
   NOT the failure**, and the bake is not calling the lake dry land.
2. **B is on over 100% of the water. `shore_m > 6 m` everywhere — including the
   strip immediately below the visibly drawn waterline.**
3. **Green does not move at all between width 1.6 and width 6** (177.3 -> 177.8,
   98.4 -> 98.4: noise). It lights up everywhere only at **width 40**.

So on every rendered water pixel, **6 m < `shore_m` < 40 m**, and the field never
approaches zero anywhere in the drawn sheet — *not even at the drawn water's
edge*. The 1.6 m foam band is therefore evaluated on a set that contains no
rendered pixels. That is why all five runtime ladders were pixel-identical and
why `BathyFoamWidthM:6` in particular changed nothing: **6 m is still short of
the actual distance.** No width, shelf or gain value can fix this.

It also explains the shelf gate independently, which is a consistency check the
factor paint could not do: `bed_slope = depth_m / max(shore_m, 0.5)`, and with
`shore_m` in 6-40 m against a ~5 m depth the ratio sits right on `shelf_hi`
(0.25), which is exactly the "closed almost everywhere, open in one far patch"
the 03:10 frame showed.

**The defect is therefore upstream of the material: the baked lake-shore SDF and
the rendered water sheet disagree about where this lake ends, by at least 6 m
and less than 40 m.** The material is behaving correctly given its input. The
next step belongs in `bathy_field_graph.py` / `VoxelBathyField.cpp` /
`basins.py` — compare the baked lake polygon's edge against the sheet's drawn
extent at this shore (candidates in the coordinator's own list: the bake's
polygon inset, the sheet's water level vs the basin datum, or the shore-clip
tolerance trimming the sheet inward). **NOT YET DONE** — this session establishes
the mechanism and hands over a bounded number.

**Refinement available cheaply:** the width scalar is a runtime override, so one
more `shoredist` regen plus a 10/20/30 m ladder pins the offset to a few metres
rather than the 6-40 m bracket. Not run here (the material was restored to
shipping so the R3 arm could be shot on a clean asset).

### 06:00 `-VoxelWaterMatScalar` SILENTLY APPLIED ONLY ITS FIRST PAIR. Fixed. Two ladder arms are void.

Found while running R3, fixed in `VoxelWaterSheetActor.cpp:325`, built clean.

`FParse::Value`'s fourth parameter, `bShouldStopOnSeparator`, **defaults to
true**, and a separator includes the comma. So the documented multi-pair form
`-VoxelWaterMatScalar=Name:Value,Name:Value` parsed as **only the first pair**
for as long as the switch has existed. Everything after the first comma was
dropped with no warning — and because the loop logs each assignment it *did*
make, the log looked healthy: one correct line, and no mention of the rest.

Proof, before and after, same command line both times:

* before — `...FarGain:1,FadeStartM:0,FadeEndM:1,Far:0.30` reached the command
  line verbatim (it is in `capture-r3-all030.log`) and produced exactly **one**
  `material scalar ... set` line, for `WaterRoughnessFarGain`. The frame was
  consequently identical to the plain `FarGain:1` arm.
* after — the same argument produces **four** lines (`FadeEndM 1.0000`,
  `FadeStartM 0.0000`, `Far 0.3000`, `FarGain 1.0000`), and the frame moves by a
  whole-frame mean |diff| of 20.2 where it previously moved 1.0.

**Consequences for this document's own ladders — please re-read the affected
rows.** Any arm that passed more than one pair set only its first:

* shore-foam **arm C** (`BathyFoamShelfLo:50 + BathyFoamShelfHi:100`) actually
  set **ShelfLo only**; `ShelfHi` stayed at the shipping 0.25.
* shore-foam **arm F** ("all three together") actually set **one** of the three.

Neither is evidence for what its label claims. **Single-pair arms are
unaffected**, which covers arms A, B and D, and covers the whole `shoredist`
ladder above — so the shore-foam conclusion stands on its own. But the null of
arms C and F was never a test of the thing they were named for.

## 2026-09-07 (material-owning agent): the "bake vs sheet disagree by 6-40 m" claim is REFUTED with numbers; shore foam has TWO defects, both measured; the wake's read side is narrowed to ONE fetch

Two new instruments were built first, because both hunts had been running on
inference off tonemapped PNGs and both had already produced a withdrawn
conclusion.

### Instrument 1 -- `voxel.Water.Shore.Audit [ReachM=120] [Rays=8] [StepM=0.25]`

`VoxelBathyField.cpp`, plus `UVoxelBathyFieldSubsystem::SampleWindowAtWorld`.
Walks rays out from the pawn and prints, per ray and in METRES:

* where the DRAWN water ends -- `UVoxelWaterSubsystem::WaterSurfaceZAtWorld`,
  the same datum+extent authority `AVoxelWaterSheetActor` gathers its rects
  from, so "is water drawn here" is answered by the thing that draws it;
* where the BAKED signed distance crosses zero -- read out of `Pixels_`, i.e.
  the exact texel the material samples, post-hole-fill and post-ocean-branch;
* a CPU mirror of the material's own `shore_band * shelf_gate * gain * validity`
  at the shipping constants, so "is there a foam ribbon at this shore and how
  wide is it in metres" stops being a question a photograph has to answer.

It also prints the origin column's surface and ground Z, because every water
capture at this lake for two days was framed from an altitude measured off the
LAKEBED and half of them were shot from under the water.

### Instrument 2 -- `-VoxelRippleWakeAfter=<sec>` (+ `Steps` / `RadiusM` / `StrengthM` / `Yaw` / `Freeze`)

`VoxelRippleField.cpp`. A headless capture has no swimmer and no boat under way,
and a ripple halves every 5 s, so EVERY "no wake" frame this project has shot
photographed an undisturbed field -- including the ones that concluded the wake
was invisible. This injects five rings on the camera's own axis at 4/6/8/10/12 m
ahead, runs N steps, then FREEZES (read back, not asserted), and logs each ring's
world XY with the baked depth and shore distance under it.

**It must use the SHUTTER's yaw, not the live camera's.** `-VoxelScreenshotAfter`
re-poses the camera inside the shutter callback (VoxelEarthGameMode.cpp:1992-2016
-- pitch from `-VoxelSpawnPitch`, yaw from `-VoxelSpawnYaw` DEFAULTING TO 45,
which is not the spawn path's 0). `VoxelVerify00914` is the frame that cost:
"yaw 0.0" at the drop, "yaw 45.0" at the shutter, rings 47 deg off a 45-deg
half-frustum.

### THE POSE, CORRECTED AND MEASURED

`-VoxelSpawnAltM` is measured from ground top (the lakebed, 1644.2 m). The audit
reads the surface at **1650.235 m**, i.e. **6.04 m** above the bed -- not the
5.32 m the 05:20 correction assumed. So `+6 m` puts the PAWN 1.5 cm UNDER the
surface and the camera (eye +0.72 m) 0.7 m over it: grazing, with every shore in
frame 21-40 m away. Captures from there can only ever see the shoreline as a few
screen rows. **The pose used for everything below is `+12 m / -35 deg / yaw 45`**
-- camera 6.6 m over the water, shoreline resolved, and no "camera entered water"
line in any of the logs.

### TASK 2, PART 1: the bake and the sheet AGREE. `gap = 0.00 m`.

`voxel.Water.Shore.Audit` at the pond column (-65102,-51084), 8 rays, 0.25 m
steps (`Saved/capture-wake-ship-drop2.log`, 11:50):

| yaw | drawn edge | baked zero | gap | shore at the drawn edge |
|---|---|---|---|---|
| 0   | 28.38 m | 28.12 m | **-0.25 m** | -1.90 m |
| 45  | 31.38 m | 31.38 m | **0.00 m** | -1.90 m |
| 90  | no shore inside 120 m | | | |
| 135 | 39.62 m | 39.62 m | **0.00 m** | -1.90 m |
| 180 | 35.62 m | 35.62 m | **0.00 m** | -1.90 m |
| 225 | 21.62 m | 21.62 m | **0.00 m** | -1.90 m |
| 270 | 22.88 m | 22.88 m | **0.00 m** | -1.90 m |
| 315 | 29.62 m | 29.62 m | **0.00 m** | -1.90 m |

Seven of eight rays agree EXACTLY; the eighth by one sample step. **WITHDRAWN:
"the baked lake polygon and the drawn water sheet disagree about where this lake
ends, by at least 6 m and less than 40 m"** (05:35). There is no bake-side
defect, no tile/seed misalignment, no window-origin offset, and NOTHING TO
RE-BAKE. The 05:35 measurement was a screen strip at a shoreline 22-40 m away
seen from 0.7 m above the water, where the last six metres of water are under ten
pixels tall: it measured open water and called it the waterline.

### TASK 2, PART 2: `BathyFoamWidthM` 1.6 m was BELOW THE FIELD'S OWN QUANTUM

`shore@drawnEdge = -1.90 m` on every single ray is the finding. The baked plane
is an exact Euclidean distance transform on the 1.875 m fine raster
(`basins.bathymetry_planes`), and a transform measures to the nearest cell of the
OPPOSITE class -- so on texel centres its positive values are 1.90, 3.80,
5.70 m ..., and **(0, 1.6) is a range the plane cannot take**. `tilestore.h:597-601`
says exactly this in words; nobody had read it against `BathyFoamWidthM`. The
material samples bilinearly, so the band is not literally empty -- it lives in
the ~0.42 of a texel where the interpolant climbs from -1.90 to +1.90 through
(0, 1.6): a ribbon under a metre wide, pinned to the outermost half-texel of
drawn water. The CPU mirror agrees: `bandMax = 0.000` on all 8 rays at width 1.6.

**Proven in one frame with its own positive control in the same pixels**
(`VoxelVerify00934`, the `shoredist` arm at the corrected pose): the B channel
(`shore_m > 6 m`) leaves a red-only ribbon along the waterline measured at
**4.0-4.8 m of world width**, while the G channel -- the LIVE `BathyFoamWidthM`,
i.e. exactly the set `shore_band` is nonzero on -- paints **ZERO pixels in the
whole frame**. Same fetch, same instant, same shore. The bathy sample is
per-pixel and correct; the threshold is the defect.

Settled by the same run: the shelf gate is NOT the limiter here. The audit
reports the bed slope at the outermost water texel per ray -- 0.026 (yaw 45,
dead ahead), 0.000, 0.089, 0.100, 0.153, 0.310, 0.342 -- so `shelf_gate` is 1.0
on the shore the camera faces.

### TASK 2, PART 3: with the band open, the foam draws BLACK, not white

`-VoxelWaterMatScalar=BathyFoamWidthM:6,BathyFoamGain:5` (BOTH echoed by the
sheet -- a post-06:00 multi-pair run, not the silently-dropped kind) moves
**1.17%** of the frame against shipping (`VoxelVerify00936` vs `VoxelVerify00922`),
and what it draws is a **dark band at the waterline**. That is the composite's
two halves disagreeing: `MP_Opacity = saturate(foam)` responds -- the volume is
removed, which is what makes the band dark -- while the BaseColor half, the SAME
`foam` lerped from black to the foam tint and multiplied by a vertex AO that is
255 on the sheet (`AppendRectQuad` writes `FColor(255,255,255,0)`), does not
arrive. It is the 2026-08-30 "a BaseColor rewire on this SLW material is a
byte-identical NULL" measurement seen from the other side, and it is the shipped
`docs/lake-sheet-black-band-2026-08-29.md` band with the foam turned up.

Width 6 at the SHIPPED gain (0.55) is consequently a null too
(`VoxelVerify00932` vs `00922`: mean |diff| 0.51, 0.15% of pixels over 10) --
foam ~0.42 through a dead channel is nothing.

### THE FIX (both halves, one regen, `create_water_voxel_material.py`)

1. `BathyFoamWidthM` **1.6 -> 6.0** (3.2 source texels), with the raster
   derivation written at the parameter.
2. **Shore foam now rides EMISSIVE as well**, exactly as the disturbance foam
   was routed on 09-06 and for the same reason: new baked scalar
   `ShoreFoamEmissive` (0.6), same tint, additive, 0 restores the previous
   emissive bit for bit, and it inherits every upstream gate (SHORE FX arm,
   `BathyFoamGain`, the shelf gate, bathy validity, the top-face mask) so every
   existing off arm stays an off arm. The BaseColor path is kept, harmless, and
   right the day SLW honours it.

Both are LOOK changes and the owner judges looks; the width is derived from the
raster rather than chosen.

### TASK 1: the wake's read side, narrowed to ONE step

Everything upstream is now proven ON THE SHIPPING MATERIAL, AT AN ABOVE-WATER
POSE, IN FRAMES THAT PROVABLY CONTAIN THE DISTURBANCE:

* **The emissive pin reaches these pixels.** The `wakeprobe` arm's B channel is a
  0.2 constant and it is on over the whole lake (`VoxelVerify00926`).
* **The per-pixel ripple UV is CORRECT to 0.2 m at 6.5 million UU from the world
  origin.** Same frame, G = `ripple u > 0.5`, a hard edge: projected back through
  the camera it lands at world **X = -65101.8 m** against a predicted
  **X = -65102.00 m**. Independently `voxel.Water.Ripple.TestFill 0.5`
  (`VoxelVerify00918`) paints the window's own edge fade, and its boundary
  measures to **camera +23.7 m in X and +23.6 m in Y** -- an axis-aligned square
  of the right size in the right place (fade reaches zero at 24.8 m).
* **The field really holds localised data at the shutter.** Five 8 m discs;
  `voxel.Water.Ripple.Dump` at t=168 reports height max **2.0 m over 12.6% of the
  512x512**, peak at uv (0.5811, 0.5342) = 4 m in front of the camera, inside the
  frustum by projection. Freeze read back as 1.
* **The instrument can fire.** `wakeprobe` R = `|sampled ripple height| > 0.05`
  goes to **100% of the water** under `TestFill 0.5` (`VoxelVerify00930`) -- the
  must-fail control this hunt has repeatedly lacked.

And with all of that true, `wakeprobe` R is **0% of the water** over the discs,
frozen (`VoxelVerify00926`) and with the simulation still running
(`VoxelVerify00928`, `-VoxelRippleWakeFreeze=0`); the two frames are identical in
the R channel (water-region R max 102 vs 101, `R>100` on 0.0000 of pixels in
both). The shipping arm agrees: the `height` arm over the same discs is
pixel-identical to shipping (mean |diff| 0.45 over the whole frame).

**So the broken step is the FETCH of `RT_VoxelRippleField` in M_WaterVoxel: it
returns the render target's clear value for content written by the derive's
`DrawMaterialToRenderTarget`, while returning content written by
`ClearRenderTarget2D` (TestFill) through the same parameter, the same UV and the
same frame.** Uniform-renders / localised-does-not is not a coincidence of
magnitudes; it is the difference between the two WRITE paths. Note that the
simulation's own step material samples the STATE targets, also canvas-drawn, and
those propagate correctly -- the difference there is that the step binds its
texture through `SetTextureParameterValue` on a MID, while the water material
reads the asset through a baked default parameter (the deliberate "WHY AN ASSET"
choice, ripple_field_graph.py:40-46).

NOT YET FIXED. The next instrument is the existing `fixeduv` arm at this pose
(sample the field at a CONSTANT uv inside the disc and paint it everywhere): it
separates "this material cannot read derived content from this render target at
all" from "it can, and something in the per-pixel path is still wrong", and the
two have completely different fixes. Everything else about the wake -- injection,
deposit, freeze, framing, the collection values, the uv, the emissive pin, the
foam art's route -- is exonerated by the frames above.

### Frames (all `+12 m / -35 deg / yaw 45` at `-65102,-51084` unless noted)

| frame | arm | what it shows |
|---|---|---|
| `VoxelVerify00914` | shipping, +6 m / -18 deg | the yaw trap: rings dropped on yaw 0, shutter at yaw 45 |
| `VoxelVerify00916` | shipping, rings frozen 20 s | no wake |
| `VoxelVerify00918` | shipping, `TestFill 0.5` | the ripple window; boundary at camera +23.7 m |
| `VoxelVerify00920` | shipping, rings frozen 4 s | no wake |
| `VoxelVerify00922` | shipping, five 8 m discs | no wake -- and the shore-foam baseline |
| `VoxelVerify00924` | `height` arm, same discs | pixel-identical to 00922 |
| `VoxelVerify00926` | `wakeprobe`, same discs | pin ON, uv edge correct, sample EMPTY |
| `VoxelVerify00928` | `wakeprobe`, `WakeFreeze=0` | identical to 00926 |
| `VoxelVerify00930` | `wakeprobe` + `TestFill 0.5` | R fires on 100% of the water (must-fail control) |
| `VoxelVerify00932` | shipping + `BathyFoamWidthM:6` | null (0.15% of pixels over 10) |
| `VoxelVerify00934` | `shoredist` arm | 6 m ribbon 4.0-4.8 m wide; the 1.6 m band is ZERO pixels |
| `VoxelVerify00936` | shipping + `WidthM:6,Gain:5` | the band is BLACK, not white |

Every debug arm ran as ONE serialized regen -> capture -> restore script with the
restore in a `finally` block, and every regen is proven from
`Saved/sky-chain/regen-create_water_voxel_material.log` (a debug arm prints its
own `ARM: ON` marker; the restores print `SHORE FX ARM: ON` and none of the debug
markers).

### New debug arm on record: `VOXEL_WATER_RIPPLE_DEBUG=wakeprobe`

Log marker `WAKEPROBE ARM: ON` -- add it to the contamination list. R =
`|sampled ripple height| > 0.05`, G = `ripple u > 0.5`, B = 0.2 constant. Three
questions on the same pixels, binary so the tonemapper cannot eat them, and B
guarantees the frame cannot be confused with a dead pin.

### Addendum: the fix is LANDED and the band is now non-empty, but at this shore it still does not read as surf

The fixed material is on disk and proven from the regen log (`SHORE FX ARM: ON`,
no debug markers, 08:40); `ShoreFoamEmissive` is present in the saved
`M_WaterVoxel.uasset` by a byte scan, so the regen took.

| frame | arm | result vs the frame before it |
|---|---|---|
| `VoxelVerify00938` | **shipping default, FIXED** (width 6, shore foam on emissive) | vs old shipping `00922`: mean 0.46, 0.039% of pixels over 10 |
| `VoxelVerify00940` | fixed + `-VoxelWaterMatScalar=BathyFoamGain:2.0` (echoed) | vs `00938`: mean 0.57, 0.41% over 10, max 85 |

So the term is now ALIVE on a live channel -- 6,328 pixels move by more than 15
when the gain goes 0.55 -> 2 -- and it is still not surf you would notice: the
ribbon that carries it is essentially the OUTERMOST TEXEL of drawn water
(`shore_band` is 0.76 at shore_m = 1.90 m, 0.35 at 3.80 m, 0.007 at 5.70 m), and
at this pond that texel is already the brightest water in the frame because the
shallow-depth grading has taken it nearly white. The remaining lever is the
band's SHAPE rather than its width or gain -- `shore_band` decays from the
waterline outward and the shelf gate closes as depth grows, so the two multiply
to a one-texel ribbon on any shore steeper than about 1:20.

That is a look question and the owner judges looks. What is settled is that the
band is no longer evaluated on an empty set and no longer painted onto a dead
channel; the ladder from here is `BathyFoamGain` / `ShoreFoamEmissive` at a pose
with a genuinely shallow shelf, not another hunt for plumbing.

## 2026-09-07 (material-owning agent, afternoon): the derive's WRITE is exonerated; the wake's read is now one reproducible sentence that should not be possible

Eight frames, every one carrying a must-fire control in the same frame, all at
`+12 m / -35 deg / yaw 45` at `(-65102,-51084)` with `-VoxelRippleWakeAfter=166`
(five 8 m rings, `StrengthM 0.9`, frozen) and `voxel.Water.Ripple.Dump` at
t=168. No `Ocean: camera entered water` line in any of the logs.

### WITHDRAWN: "the broken step is the FETCH ... for content written by the derive"

The 11:50 conclusion -- *"the broken step is the FETCH of
`RT_VoxelRippleField` in M_WaterVoxel: it returns the render target's clear
value for content written by the derive's `DrawMaterialToRenderTarget`, while
returning content written by `ClearRenderTarget2D` through the same parameter,
the same UV and the same frame"* -- **is refuted.**

New arm **`VOXEL_WATER_RIPPLE_DEBUG=fieldpic`** (marker `FIELDPIC ARM: ON`; add
it to the contamination list) samples the field at the SCREEN's own uv --
`MaterialExpressionScreenPosition` -- so the frame IS the render target,
stretched over whatever water is on screen, with the world, the collection and
the mapping removed from the question. `VoxelVerify00942` draws the five
injected discs as a hard-edged blob spanning texture uv **0.44-0.81 in u,
0.42-0.79 in v, centred (0.62, 0.60)**. That is where the injector's own logged
ring world positions put them (the ring line runs (0.564,0.555) to
(0.674,0.666)), where `Ripple.Dump` puts them, and it carries the **1.85:1**
on-screen aspect a round uv shape must take when stretched over a 2560x1398
frame (measured 960x518 px; the frame's own aspect is 1.83). Its must-fire
control `VoxelVerify00944` (`TestFill 0.5`) fills the frame.

**So M_WaterVoxel reads canvas / `DrawMaterialToRenderTarget`-written content
perfectly well.** There is no write-path defect, nothing to re-plumb in
`RunDerive`, and no reason to move the derive to RDG or to a compute pass. The
diagnostic built for that branch -- **`voxel.Water.Ripple.StateFill <h>`**
(`VoxelRippleField.cpp`: clears both STATE targets to a constant immediately
before the derive, so the canvas draw deposits a UNIFORM field -- the write-path
half of `TestFill`) -- is on record, costs nothing at its 0 default, and was
never needed: the branch closed before it was fired.

### The per-pixel uv is CORRECT -- offset, scale, sign AND orientation

Every "the uv is correct" measurement before today was a HALF-PLANE (the 09-06
`u > 0.5` edge) or the TestFill window boundary. **Both are blind to the two
error families that reproduce this bug exactly:** a flip about the window centre
leaves the `u = 0.5` isoline exactly where it was and leaves the Chebyshev edge
fade exactly square, and a wrong SCALE with a compensating origin leaves a
straight boundary straight. A uniform TestFill is invariant under *any* wrong
coordinate at all, which is why every positive uv frame in this hunt so far is a
uniform-field frame.

* **`uvpin`** (`UVPIN ARM: ON`, `VoxelVerify00946`) -- R = `u > 0.615`,
  G = `v > 0.604`, B = the MIRRORED read `|field.B at (1-u, 1-v)| > 0.05`. Four
  clean quadrants meeting at screen **(0.4875, 0.600)**; predicted for the
  shipped mapping **(0.473, 0.603)**, i.e. the world point **7.5 m dead ahead**
  of the camera on the shutter's yaw-45 axis -- inside the disc union. The `u`
  isoline runs along +/-Y (up-right on screen) and the `v` isoline along +/-X
  (up-left), which is the correct orientation for `(X-Ox, Y-Oy)*s` at yaw 45.
  **The mirrored read painted ZERO pixels: it is not a flip.**
* **`bandprobe`** (`BANDPROBE ARM: ON`, `VoxelVerify00952`) -- R =
  `0.600 < u < 0.630`, G = `0.590 < v < 0.620`: BANDS, not edges, so their
  on-screen width is a ruler. They render as two stripes crossing at
  (0.615, 0.605) and measuring **~1.5 m of world width** against
  `0.03 x 51.2 m = 1.54 m`. **The scale is right.**

### And the fetch at that uv is still empty

* **`bothtap`** (`BOTHTAP ARM: ON`, `VoxelVerify00948`) -- the three questions on
  ONE set of pixels: R = `|field.B at SCREEN uv| > 0.05` draws the blob;
  G = `|field.B at the PER-PIXEL WORLD uv| > 0.05`, **RAW** (no gain, no edge
  fade, same texture parameter, same threshold, one node apart) is **BLACK**;
  B = the weighted `height_m` the shipping foam reads is **BLACK**.
* **`mipprobe`** (`MIPPROBE ARM: ON`, `VoxelVerify00950`) -- the world uv with
  **LOD forced to mip 0**: black. The world uv rebuilt from a **plain
  `WorldPosition`**, dropping `WPT_EXCLUDE_ALL_SHADER_OFFSETS` (the only
  structural difference from `bathy_field_graph`'s proven-good tap): black.
  Screen-uv control: fires. *(The mip pin is called `Level`, not `MipLevel`:
  `UMaterialGraphNode::GetShortenPinName` shortens it, which is also why `UVs`
  reaches the input the engine calls `Coordinates`. The arm refuses to build if
  it cannot connect that pin, so mip 0 provably took.)*
* **`constprobe`** (`CONSTPROBE ARM: ON`, `VoxelVerify00954`) -- the SAME texture
  parameter at CONSTANT uv **(0.615, 0.605)** and **(0.583, 0.583)** lights the
  **entire lake**. The render target really does hold the discs at those
  coordinates at the shutter.
* **`fixprobe`** (`FIXPROBE ARM: ON`, `VoxelVerify00956`) -- a fresh absolute-uv
  chain at mip 0: black. The LWC-safe rewrite `uv = ((W - C) - (O - C)) * s`,
  with `W - C` the engine's own translated world position (small floats, no LWC)
  and `O - C` a difference of two UNIFORMS that folds on the CPU and never
  appears per pixel: black. Const-uv control: fires.

### THE FINDING, IN ONE SENTENCE

**A fetch of `/Game/Voxel/RT_VoxelRippleField` from M_WaterVoxel returns the
injected data when its UV is a CONSTANT or the SCREEN's uv, and returns nothing
when its UV is derived from `WorldPosition` -- in the same frame, on the same
pixels, through the same texture parameter, at a UV whose value is pinned to
(0.615, 0.605) by two 1.5 m bands.** The arithmetically identical world-uv
construction against `bathy_field_graph`'s plain `UTexture2D` is per-pixel
correct in these very frames (the shoredist ribbon). A sampler cannot return two
answers for one coordinate, so the value the ARITHMETIC produces and the value
the FETCH receives are not the same value.

### What it is NOT, each with the frame that killed it

| suspect | killed by |
|---|---|
| the derive's `DrawMaterialToRenderTarget` write | 00942 -- the discs render through the screen tap |
| the object binding / a second render target | 00954 -- a const uv on the same parameter reads the discs |
| the field being empty or decayed at the shutter | 00954, and `Ripple.Dump` at t=168 |
| a uv flip about the window centre | 00946 -- the mirrored read is zero pixels |
| a wrong uv offset | 00946 -- the quadrant corner lands 7.5 m dead ahead |
| a wrong uv scale | 00952 -- the bands measure 1.5 m against 1.54 m predicted |
| mip / LOD selection | 00950, 00956 -- mip 0 forced, still black |
| `WPT_EXCLUDE_ALL_SHADER_OFFSETS` | 00950 -- a plain `WorldPosition` is black too |
| large-world (LWC) precision in the subtraction | 00956 -- the camera-relative rewrite is black too |
| `RippleFieldGain` / the edge fade | 00948 -- the RAW, unweighted tap is black as well |

### Not settled, and how to settle it next

`crosstex` (`CROSSTEX ARM: ON`, `VoxelVerify00958`) crossed the two remaining
variables -- the BATHY `UTexture2D` sampled at the RIPPLE's uv -- and **is
non-discriminating as built**: it gated on bathy *validity*, which is 1 across
the whole baked window AND at its clamped border, so the frame comes back white
either way. Re-run that arm against `depth_m` or `shore_m`, which have
structure, and it becomes the fork it was meant to be:

* bathy's image squeezed into a **51.2 m square around the camera** -> the
  ripple uv chain hands correct in-range coordinates to a sampler, and the defect
  is this render target sampled through a world-derived coordinate;
* bathy's normal full-window image -> the ripple uv chain does not reach a
  sampler as the value its arithmetic provably produces, whatever texture is on
  the other end.

**NOT FIXED.** The wake, the player ripples and the boat wake all read through
this one tap -- `VoxelCharacterMovement.cpp:951` and `VoxelBoat.cpp:1116-1124`
both call `UVoxelRippleFieldSubsystem::AddSweptDisturbanceAt`, so they share the
pending queue, the step, the derive, the field and `sample_ripple_field` -- so
all three are dark for the same reason and all three come back together. Worth
recording for when they do: the player's walking ripple is injected at
**StrengthM 0.020** against this capture's 0.9, so it will be far subtler than
these frames even once the read works.

### New debug arms on record (add all to the contamination list)

`FIELDPIC ARM: ON`, `UVPIN ARM: ON`, `BOTHTAP ARM: ON`, `MIPPROBE ARM: ON`,
`BANDPROBE ARM: ON`, `CONSTPROBE ARM: ON`, `FIXPROBE ARM: ON`,
`CROSSTEX ARM: ON`. `ripple_field_graph.sample_ripple_field` now also returns
`height_raw` and `weight` -- the two halves of `height_m`, so an instrument can
ask which of them is zero without building a second sampler that would not be
the same fetch. Nothing shipping reads them and not one node of the graph
changed.

Every arm ran as ONE serialized regen -> capture -> restore script with the
restore in a `finally` block, and every restore is proven from
`Saved/sky-chain/regen-create_water_voxel_material.log` (`SHORE FX ARM: ON`,
`STAR REFLECTION ARM: ON`, and none of the debug markers above).

## 2026-09-07 (material-owning agent, evening): THE WAKE'S READ IS FIXED. The mechanism is the compiled TextureSample, not the coordinate -- proven in ONE frame with the defect and the fix in the same pixels

### The frame: `VoxelVerify00960`, arm `customtap`

Same pose, same injection, same shutter as the whole afternoon
(`+12 m / -35 deg / yaw 45` at `(-65102,-51084)`, `-VoxelRippleWakeAfter=166`,
five 8 m rings at `StrengthM 0.9`, `Steps=1`, frozen -- freeze read back as 1,
`Ripple.Dump` at t=168 reports peak 2.0 m at uv (0.5811,0.5342) over 12.61% of
the field, 22573 non-zero gradient texels). **No `Ocean: camera entered water`
line in the log** (grep count = 0), and no `Failed to compile Material`.

Three questions on ONE set of pixels, binary so the tonemapper cannot eat them:

| channel | what it is | measured |
|---|---|---|
| **R** | `Texture2DSampleLevel(RippleFieldTex, RippleFieldTexSampler, UV, 0).b` in a **Custom HLSL node**, `UV` = the SHIPPING per-pixel world uv | **51.98% of the frame** -- the injected disc union, in world space, in the right place |
| **G** | the **ordinary** `TextureSampleParameter2D` at **that same uv expression** -- same parameter name, same asset, one node apart | **0.0000%** (max green-over-others across the whole frame = **4/255**, i.e. noise) |
| **B** | the ordinary sampler at the **CONSTANT** uv (0.615, 0.605) -- the must-fire control | fires over **all** the water |

The picture is unmistakable: a blue lake (B), with a large magenta (R+B) lobe
across the near field where the rings were dropped, and **no green anywhere**.

### THE MECHANISM, as far as the evidence names it

**The material compiler's emitted texture fetch for `RippleFieldTex` does not
receive the coordinate the graph computes.** The coordinate itself is fine -- it
was measured four separate ways (`uvpin` orientation and offset, `bandprobe`
scale to 1.5 m against 1.54 m predicted, and both again here, since R and G read
the SAME expression object). A hand-written `Texture2DSampleLevel` fed that
expression returns the data; the compiler-emitted `TextureSample` fed the same
expression returns the clear value. That is not a property of the coordinate,
the texture, the binding, the mip, LWC, the gain or the fade -- each of those has
its own frame that killed it (the table in the afternoon entry above) -- and it
is the only remaining place the two values can diverge.

**What is NOT yet named** is which compiler behaviour does it: the candidates
are (a) the derivative autogen / coordinate re-derivation UE5 runs for LOD on a
sample whose UV chain reaches `WorldPosition`, and (b) something Single Layer
Water-specific about a sample shared between the base pass and the water
shading. Distinguishing them needs the generated HLSL, not another photograph --
see "What I would run next" below. It does not block the fix: the fix bypasses
the emitter entirely, so both candidates are repaired by it.

**WITHDRAWN by this frame:** nothing. Every earlier withdrawal stands, and the
afternoon's one-sentence finding is now *explained* rather than contradicted.

### THE FIX (`ripple_field_graph.py`, `sample_ripple_field`)

The `MaterialExpressionTextureSampleParameter2D` is replaced by a
`MaterialExpressionTextureObjectParameter` (SAME parameter name
`RippleFieldTex`, same asset, same `SAMPLERTYPE_LINEAR_COLOR`) feeding a
`MaterialExpressionCustom` whose whole body is

```hlsl
return Texture2DSampleLevel(RippleFieldTex, RippleFieldTexSampler, UV, 0);
```

with `output_type = CMOT_FLOAT4`, so `.rg` is still the gradient and `.b` still
the height. **The uv chain is not touched** -- it was never the defect.

Why this shape and not another:

* **ONE read path is preserved.** `grad_xy` (normal + disturbance foam) and
  `height_m` (World Position Offset) still come off the same single fetch, which
  is the module's founding argument. The **explicit mip 0** is what makes that
  legal: the same expression now has to compile in the VERTEX shader for WPO,
  and an explicit level removes the derivative requirement. The render target
  has exactly one mip (`create_ripple_field_materials.py` never asks for more),
  so nothing is lost.
* **The binding is unchanged.** A texture OBJECT parameter under the same name
  is still a baked asset default, so the far-field sheet -- which is assigned the
  shared `M_WaterVoxel` with no MID on purpose (`VoxelWaterSheetActor.h:47-52`)
  -- still gets the field. The "WHY AN ASSET" contract survives intact.
* **The precedent is already shipping in this project.**
  `create_sunshadow_lf_material.py:106-131` uses exactly the
  object -> Custom -> `<InputName>Sampler` convention.
* `AbsorptionDistanceM 3.5`, `WaveQuantPerVoxel 0` and the shore-foam block are
  untouched. So is `RippleFieldGain`, the edge fade, the `WaveTimeScale` gate
  and every existing off arm: `RippleFieldGain 0` still zeroes the tap.

`sample_ripple_field` is shared, so **M_Ocean gets the same fix on its own
regeneration** (`-Only create_ocean_material.py`) -- the source is fixed for
both; only M_WaterVoxel's asset has been rebuilt against it so far.

### New debug arm on record: `VOXEL_WATER_RIPPLE_DEBUG=customtap`

Marker `CUSTOMTAP ARM: ON` -- add it to the contamination list. R = the Custom
fetch at the world uv, G = the ordinary sampler at the same uv, B = the ordinary
sampler at a constant uv. It is the arm that both diagnosed and validated the
fix, so it is worth keeping: it will fail loudly the day an engine upgrade makes
the ordinary sampler work again (G would light) or breaks the Custom one.

### `crosstex` re-gated, and `VoxelVerify00958` is VOID

The 10:38 firing of `crosstex` gated R and G on bathy **validity**, which is 1
across the whole baked window *and* at its clamped border -- `VoxelVerify00958`
is a white frame that says nothing, exactly as the afternoon entry predicted.
The arm is now gated on `|shore_m| < 6 m`, a BAND whose ON set is a curve, so
the "bathy image squeezed into a 51.2 m square" signature is legible. **It was
not needed** -- `customtap` answered the same fork more directly and in the
direction of a fix -- and it is left re-gated and ready.

### New, non-arm switch: `VOXEL_WATER_DUMP_HLSL=1`

Sets `r.DumpShaderDebugInfo 1` before the first compile, so the translated
shader source lands under `ue-project/Saved/ShaderDebugInfo`. **It is not a
debug arm**: it sets console variables and nothing else, the asset it produces
is byte-identical to the same environment without it, and it therefore needs no
restore. Built for the mechanism question above; not yet fired.

### NOT YET PROVEN, and it is the next thing to run

The shipping-default capture. Everything above is measured on the `customtap`
arm; the shipping material has NOT been photographed with a disturbance in
frame since the fix. The restore after `customtap` is proven from
`Saved/sky-chain/regen-create_water_voxel_material.log` (`SHORE FX ARM: ON`,
zero debug markers) -- but that restore predates the fix landing in
`ripple_field_graph.py`, so **M_WaterVoxel on disk is still the OLD (broken)
tap** until the next regeneration. The source is fixed; the asset is not.

### What I would run next (paused: the owner is taking the box)

1. `tools\voxel-sky-chain-regen.ps1 -Only create_water_voxel_material.py -SkipCapture`
   with **no** `VOXEL_WATER_RIPPLE_DEBUG` -- lands the fix in the shipping asset;
   proven from `SHORE FX ARM: ON` + zero debug markers.
2. `tools\voxel-capture.ps1 -Name wake-ship-fixed -SpawnAt '-65102,-51084'
   -SettleSec 170 -SpawnAltM 6 -SpawnPitch -18` plus
   `-ExtraArgs '-VoxelRippleWakeAfter=166' '-VoxelRippleWakeRadiusM=8'
   '-VoxelRippleWakeStrengthM=0.9' '-VoxelRippleWakeSteps=1'` -- the owner's
   requested pose (camera 0.7 m over the surface there; check the log for
   `camera entered water` before believing it). Then the same at
   `-SpawnAltM 12 -SpawnPitch -35`, which is the pose the shoreline is actually
   resolved at, as the legible frame.
3. `-Only create_ocean_material.py -SkipCapture`, so the sea gets the same tap.
4. The mechanism, for the record and for the day someone tries to revert this:
   one regen with `VOXEL_WATER_RIPPLE_DEBUG=customtap` **and**
   `VOXEL_WATER_DUMP_HLSL=1`, no capture, then read
   `ue-project/Saved/ShaderDebugInfo` for the two fetches side by side -- the
   Custom one and the compiler-emitted one are in the same shader, so the diff
   is the mechanism. Restore afterwards.
5. The boat. The unattended recipe exists and is cheap:
   `-VoxelExecAfter=165 -VoxelExecCmds="voxel.Boat.Spawn 8,voxel.Boat.Enter,voxel.Boat.Throttle 1 12"`
   with `-VoxelScreenshotAfter=173`, so the shutter opens with the hull under
   way (`voxel.Boat.Spawn/Enter/Throttle`, VoxelBoat.cpp:1444-1490 -- the
   "Spawn -> Enter -> Throttle is the whole unattended underway-wake leg"
   comment is the author's own).

### The player ripple and the boat wake: arithmetic, NOT yet a measurement

Both read through the tap just fixed (`VoxelCharacterMovement.cpp:951` and
`VoxelBoat.cpp:1116-1124` both call `AddSweptDisturbanceAt`), so both come back
with it. At the shipped `DisturbanceFoamGain 8.0` /
`DisturbanceFoamHeightWeight 4.0`, `foam = saturate((|grad| + 4|h|) * 8)`:

| source | strength | radius | h | rough grad | foam at deposit |
|---|---|---|---|---|---|
| player walking, 4 m/s (`Frac = 4/6`) | 0.0133 m | 0.45 m | 0.0133 | ~0.030 | ~0.66 |
| player at/above 6 m/s (`Frac` 1.0) | 0.020 m | 0.45 m | 0.020 | ~0.044 | 1.0 (saturated) |
| boat bow (`BowWakeStrengthM` 0.030) | 0.030 x Frac | 0.55 m | | | 1.0 at Frac >= 0.6 |
| boat transom (0.022) | 0.022 x Frac | 0.95 m | | | 1.0 at Frac >= 0.9 |

So on this arithmetic **no gain change is needed** -- 0.020 m is not too small
once the read works; it was invisible because the read returned zero, not
because the number was small. The ripple halves every 5 s and spreads, so the
trail behind a walker will fade over a few metres rather than paint a permanent
stripe, which is the correct look. **This is a calculation, not a frame**: it
needs run 2 and run 5 above before it may be quoted as measured.

### Frames

| frame | arm | what it shows |
|---|---|---|
| `VoxelVerify00958` | `crosstex` v1 | VOID -- gated on bathy validity, which is 1 everywhere; a white frame |
| `VoxelVerify00960` | `customtap` | R (Custom HLSL @ world uv) 51.98%, G (ordinary sampler @ the SAME uv) 0.0000%, B (const uv) all water |

Both ran as ONE serialized regen -> capture -> restore script with the restore in
a `finally` block; the restore is proven from
`Saved/sky-chain/regen-create_water_voxel_material.log` (`SHORE FX ARM: ON`,
zero debug markers).

## 2026-09-07 afternoon: wake fix shipped into the assets

The fix is now IN BOTH SHIPPING ASSETS, proven at the byte level, **and
photographed**. Committed as `fe51c15`.

The pass was interrupted partway through: the capture harness rebuilt
`voxelcore.lib` from another agent's in-flight edits and left the module DLLs
incoherent, so steps 1.2-1.4 were blocked until a build landed. After the build
they all ran and all passed -- frames `VoxelVerify00964` (legible pose),
`00966` (owner pose), `00974` (boat under way), `00978` (glider parked). The
scoreboard below is the record; where an earlier draft of this section said the
frames did not exist, it was written during the block and is superseded.

What remains open is not the mechanism but the LOOK: the wake injection is a
2.0 m debug-magnitude displacement, roughly 100x the shipped player ripple of
0.020 m, so at the owner's +6 m pose it swamps the frame. That is an owner call
on injection strength for future captures, not a defect in the tap.

### Scoreboard

| date | phase | what | verdict | evidence |
|---|---|---|---|---|
| 2026-09-07 | 0.2 | Cold-cache measured launch at the lake pose | **DONE, but the premise failed** | `Saved/capture-coldlaunch-lake.log`; frame `VoxelVerify00962.png`. Standby cache was 41,526 MB and an untouched-namespace 468 MB tile read at 4015 MB/s, so the file cache was warm. Full table in `docs/tile-loading-async-2026-09-07.md` |
| 2026-09-07 | 0.2 | Tile I/O is the hitch cause? | **REFUTED on this leg** | 4 decodes, 261-326 ms each; `fineMs=0.02`-`0.03` on every `Hitch frame recompute`; largest stall `renderWaitMs=50626.63` has no tile load near it; raster-atlas fills peak at 1478.4 ms GT per 5 s window |
| 2026-09-07 | 1.1 | Regen `create_water_voxel_material.py`, no env vars | **PASS** | `Saved/sky-chain/regen-create_water_voxel_material.log`: `SHORE FX ARM: ON` x1, `NOT A SHIPPING MATERIAL` x0, `Python script executed successfully` x1, `LogPython: Error` x0, `Failed to compile Material` x0 |
| 2026-09-07 | 1.1 | Regen `create_ocean_material.py`, no env vars | **PASS** | `Saved/sky-chain/regen-create_ocean_material.log`: `M_Ocean RIPPLE FIELD ARM: PRESENT`, `NOT A SHIPPING MATERIAL` x0, `Python script executed successfully` x1, `LogPython: Error` x0, `Failed to compile Material` x0. `SHORE FX ARM` is x0 because the string does not exist in `create_ocean_material.py` (grep count 0) -- the gate is inapplicable to the ocean, not failed |
| 2026-09-07 | 1.1 | The fix is actually in the shipped bytes | **PASS (stronger than mtime)** | Byte search of both `.uasset` files finds `Texture2DSampleLevel` AND `RippleFieldTex` in each. `M_WaterVoxel.uasset` 209893 -> 211037 B at 14:41:41, `M_Ocean.uasset` 176995 -> 178139 B at 14:42:15, both newer than `ripple_field_graph.py` (11:13:35) |
| 2026-09-07 | 1.1 | Neither shipped asset carries a debug arm | **PASS** | Byte search of both `.uasset` files for `CUSTOMTAP`, `NOT A SHIPPING MATERIAL`, `crosstex`, `uvpin`, `bandprobe`, `RIPPLE_DEBUG`: zero hits in either. `Get-ChildItem env:VOXEL_*` was empty before both regens |
| 2026-09-07 | 1.2 | Wake capture, shipping default, legible pose | **PASS** | `VoxelVerify00964.png` (+12 m, pitch -35, yaw 45). Engagement: `CAPTURE WAKE FIRED ... injected=5 dropped(outside=0 full=0 unarmed=0 inert=0) steps=9703 fieldMaxAbs=2.0000 stateMaxAbs=2.0000. FROZEN.` and `field verified LIVE -- centre patch max field value 2.0000`. All 5 rings on valid water (`baked depth=5.10-5.95 m valid=1`). `Ocean: camera entered water` x0, `Failed to compile Material` x0, gate leaks 0. A large concentric disturbance fills the near field |
| 2026-09-07 | 1.2 | Wake capture, owner pose | **PASS (engagement); frame swamped** | `VoxelVerify00966.png` (+6 m, pitch -18). Same counters (`injected=5 ... fieldMaxAbs=2.0000`), `camera entered water` x0. The 2.0 m displacement seen almost edge-on from 6 m fills nearly the whole frame; the lake is not legible in it |
| 2026-09-07 | 1.3 | Boat under way | **PASS** | `VoxelVerify00974.png` (+6 m, pitch -12). `VoxelBoat: BOARDED at (-6509900,-5108400,165038). ... The ripple window now follows the boat.` then `voxel.Boat.Throttle 1 12`; camera travelled ~22 m from spawn by the shutter, `unloaded=280`. Hull and wake arcs visible. Gate leaks 0 |
| 2026-09-07 | 1.2 | Wake IMAGE, owner verdict | **FAIL (owner)** | Owner on the boat frame: "The boat wake looks like a hard grey blob on the surface and it makes no sense what so ever. I see no shore foam at all in the screenshot." On `VoxelVerify00966`: "a grey plane covering almost the entire world map - that's really weird and not expected." **The PASS rows above are ENGAGEMENT passes and were wrongly presented as settling the image.** Code retained, defect assigned; not a rollback |
| 2026-09-07 | 1.3 | Boat wake IMAGE, owner verdict | **FAIL (owner)** | Same verdict, same frame set. Diagnosis to be tested, not assumed: `build_disturbance_foam` computes `saturate((|grad| + |height_m| * 4.0) * 8.0)`, so any texel above raw 0.125 pins to full white and the wake becomes a binary mask of "where the field has been touched" rather than a wake. The hard edge is the ripple window boundary, where a ~3.3 m edge fade cuts saturated foam against undisturbed water |
| 2026-09-07 | 1.2/1.3 | Shore foam | **NOT ANSWERABLE from these frames** | Saturated disturbance foam sits on top of the shoreline, so "I see no shore foam" is confounded and is not evidence about shore foam. The two features must be separated before either gets a verdict |
| 2026-09-07 | 1.4 | Glider parked spawn | **PASS** | `VoxelVerify00978.png` at column -65059,-51042 (+3 m, pitch -12). `voxel.Glider.Spawn: PARKED glider at (-6505100,-5104200,165161), resting on the surface.` Both required strings present, gate leaks 0 |
| 2026-09-07 | 1.5 | `voxel.Water.Caustics` help text says the real default | **DONE** | `VoxelWaterSubsystem.cpp:262` help string `Default 1.` -> `Default 0.5.`; the cvar's value `0.5f` is unchanged. String-only edit, not built |
| 2026-09-07 | 1.6 | UI divergences vs the 2026-09-07 mocks | **DONE, 14 screens** | 8 pre-existing captures (Inventory, Journal, Map, Player, Codex, Death, Dialogue, HUD v2) plus 6 taken this pass (`VoxelMenu00015`/`00016`/`00018`, `VoxelLoading00004`, `VoxelPause00005`/`00006`). List below, with 7 cross-cutting causes called out |
| 2026-09-07 | 1.6 | Capture set is real (silent-failure check) | **CAUGHT ONE** | `VoxelMenu00017.png` was byte-identical to the main menu (md5 `e9d094f9…` on both) -- `-Shot Panel -Panel save` opened nothing. Re-taken as `-Shot Pause -Panel save -DemoSaves` -> `VoxelPause00006.png`, log `6 save(s) listed`; all three pause captures now hash differently |
| 2026-09-07 | 1.7 | Commit the water files | **DONE** | `fe51c15`, staged by explicit path list (no `git add -A`; 388 unrelated asset-forge files left alone). Gated on 1.2 passing, which it did |

### How the box blocked itself, and why no build was run

`tools/voxel-capture.ps1` guards against a stale `voxelcore.lib` by asking cmake
whether the lib is really behind (lines 200-256). On the wake capture it found
`tilestore.cpp` newer than the lib, called `cmake --build ... --target voxelcore`,
and cmake **did** relink -- because the parallel loader agent has uncommitted
edits in `voxel-core/src/tilestore.cpp`, `include/voxelcore/tilestore.h` and
`tests/test_tilestore.cpp`. The lib then became newer than
`UnrealEditor-VoxelEarth.dll`, which trips the second half of the guard, and
there is no bypass switch.

**The capture harness invalidated the box as a side effect of being run.** That is
worth recording as its own hazard: a capture is not read-only with respect to the
build state when another agent has voxel-core edits in flight.

No build was run to clear it, for two reasons:

1. This pass was explicitly told not to run `tools/voxel-build.ps1`.
2. More importantly, a build now would link **two** agents' in-flight work into
   the binary that photographs the water. `git status` shows uncommitted edits in
   `VoxelOceanActor.cpp`, `VoxelSkySubsystem.cpp`, `VoxelClipmapActor.cpp`,
   `VoxelWeatherSubsystem.cpp`, `VoxelGI.cpp` and `VoxelShadowMarch.cpp` -- all of
   them in the water/sky tick path. A wake frame taken against that binary could
   not be attributed to the wake fix either way, so it would not settle the gate
   it exists to settle.

The DLL on disk (15:59:54 UTC) predates both agents' edits and is therefore the
**correct** binary for the water image gate. The block is purely the lib mtime.

**To unblock:** serialize -- let the loader and menu agents reach a committed,
compiling state, run one `tools/voxel-build.ps1 -Verify`, then re-run Phase 1
steps 2-4. There is no shortcut that keeps the water verdict meaningful.

## UI divergences vs the 2026-09-07 mocks

The list the plan says must exist before any owner verdict can be asked for.

**Captures.** Eight were already on disk from this morning's session
(`docs/ui-mocks/2026-09-07/captures/`: codex, death, dialogue, hud, inventory,
journal, map, player, taken 06:38-07:37). Six were taken this pass at 14:45-14:48
via `tools/voxel-ui-capture.ps1` at 2560x1440:

| screen | shot | frame |
|---|---|---|
| Main menu | `-Shot Menu` | `VoxelMenu00015.png` |
| Loading screen | `-Shot Loading` | `VoxelLoading00004.png` |
| Pause menu | `-Shot Pause` | `VoxelPause00005.png` |
| Settings panel | `-Shot Panel -Panel settings` | `VoxelMenu00016.png` |
| Save dialog | `-Shot Panel -Panel save` | `VoxelMenu00017.png` |
| Load dialog | `-Shot Panel -Panel load` | `VoxelMenu00018.png` |

All six ran against the **committed** binary (`UnrealEditor-VoxelEarth.dll` of
15:59:54 UTC), which predates the loader and menu agents' uncommitted edits, so
this list describes shipped behaviour and not either agent's work in progress.
The save/load dialogs were captured with `0 save(s) listed`, so an empty list is
expected; only the styling and wording of the empty state are judged.

Mocks are authored at 1920x1080; captures are 2560x1440, so absolute pixel
figures below are converted at the mocks' 1.33333 authoring zoom where quoted.

**Status of the six new captures.** All six render their intended screen (none is
blank or black); that was checked directly. Two first-hand notes on them, pending
the full per-screen diff:

- **Settings (`VoxelMenu00016.png`).** Renders AUDIO (Master, Music), DISPLAY
  (Fullscreen + "Takes effect on APPLY."), GRAPHICS (Fine Detail Smoothing,
  Faster Terrain Drawing, Water Wave Detail) with `ESC back` / `APPLY` /
  `SAVE & LEAVE`. **The fourth graphics row, Ocean Mesh Detail, is below the fold**
  — a scrollbar is visible and the row is cut off. Both water rows exist in
  `SVoxelSettingsPanel.cpp:72-81`'s `kGraphicsRows`, so this is a panel-height
  problem, not a missing row. Worth fixing before the owner is asked to judge the
  water settings, since one of the two water rows is invisible at 2560x1440.
- **Pause (`VoxelPause00005.png`).** Renders `— PAUSED —` with RESUME / SAVE /
  LOAD GAME / SETTINGS / EXIT TO MENU / QUIT and the footer `Day 11`. The HUD
  compass tape and the ten-slot hotbar remain fully lit behind the panel; the
  mock dims the paused world behind its `.se-panel`. Note the footer's `Day 11`
  is the same sky-derived value discussed under Journal, which confirms the
  Journal screen's internal `Day 12` is the odd one out.

### Four patterns that account for most of the list

Before the per-screen detail, four causes explain the majority of the bullets
below. Fixing any one of them closes many rows at once, so they are worth
deciding on before anyone works the list screen by screen.

1. **CSS `clip-path` glyphs are all flat rectangles.** Every mock icon built as a
   clip-path silhouette — attribute glyphs, equipment-slot glyphs, skill glyphs,
   the heraldic sub-tab shields, the craft arrow, the rotated option diamond, the
   compass rose — renders in Slate as a plain coloured block. The *hues* are
   almost always correct, so the port is reading the palette and not the shape.
   This is one missing capability (vector/clip brushes in `VoxelUITheme.h`), not
   ~20 separate bugs.
2. **Press Start 2P is not shipped.** `docs/ui-mocks/2026-09-07/README.md` already
   records `--pixel` as "not shipped (unused by the ported screens)". Every
   `var(--pixel)` element therefore falls back to a serif/mono face. Either ship
   the font or stop specifying it; today the mock and the port cannot agree.
3. **Gradients render as flat fills.** HP and hunger bars, button fills and panel
   grounds specified as `linear-gradient(...)` come out as a single colour. This
   is what makes the hunger bar read as the wrong colour rather than merely
   flatter — it picks one stop of a three-stop ramp.
4. **The `Q/E Switch screen` / `ESC Exit` hint bar is a shell feature, not five
   divergences.** It comes from one shared string
   (`VoxelUIStrings.cpp:249 ScreenActionPage`) rendered by the common
   `SVoxelScreenShell`, so it appears on Inventory, Journal, Map, Player and Codex
   even though none of those mocks has a footer. The per-screen hint bars the
   mocks *do* specify (`E Spend point`, `F Compare`, the Map's seven) were
   replaced by it. That is a design decision to confirm, not a rendering fault —
   though on Map it actively conflicts, because the mock binds `Q/E` to rotating
   the map.

The same goes for tab key-letter prefixes (`M MAP`, `J JOURNAL`, ...): the shared
`.menu-tab .key` rule exists in `menus_shared.css`, so the port looks sanctioned
by the framework even where individual mock markup omits it.

### Inventory

Matches the mock closely — the pack/side-panel split, 8x8 pack + 8-slot hotbar
with keys 1-8, the filter row, the CRAFT / CHARACTER toggle, the 3x3 craft grid
and output, and the "Drag from the pack into the grid." note are all present and
correctly ordered. Real deltas:

- **Search placeholder.** Capture `Search the pack`; mock (`#invSearch`) `Search…`.
- **Weight readout.** Capture `80 kg`; mock (`.weight`) `112 / 180` preceded by an
  11px hexagon swatch (`.weight i`, `background:var(--bronze)` `#b07a3a`). The
  hexagon is absent.
- **Stack-count prefix missing.** Capture shows `16`; the mock builds `'×' + it.qty`, i.e. `×16`.
- **Craft arrow is a dash, not a triangle.** Mock `.craft-arrow` is a downward
  triangle (`border-top:11px solid var(--bronze-deep)`); capture draws a short
  horizontal bronze bar.
- **CRAFT button colour.** Capture renders the label dim grey; mock `.craft-act`
  uses `color:var(--parchment)` `#e8d9b0` on the oak gradient. If this is a
  disabled state, the mock has no such state.
- **Search field too wide.** Capture ~537 device px; mock caps at
  `max-width:280px` (~373 px at authoring zoom), which is what pushes the weight
  readout right in the mock.
- **In the capture, not the mock:** per-tab key prefixes on the top tab bar
  (`M MAP`, `J JOURNAL`, ...) — `menus_shared.css` does define `.menu-tab .key`,
  so this looks framework-sanctioned even though the mock markup omits it; and a
  bottom hint bar `Q/E Switch screen` / `ESC Exit`, which this mock lacks entirely.
- **Not verifiable here:** the pack holds one item, so rarity rings, `×qty` badges
  and durability bars could not be checked against the mock's 31-item seed.

### Journal

Structurally faithful: the `PLAYER'S ENTRIES` / `GOALS` switch, the entry list,
the parchment page with kind / title / date-stamp / body, and the selected-card
gold underline are all correct. Deltas:

- **`Entries 3` sub-tab is unstyled.** Mock `.subtab.on` is a bordered chip
  (`color:var(--gold)` `#f0c14b`, `background:var(--panel-oak-2)` `#2e1b0d`,
  `border:1px solid #000`); capture renders plain grey text.
- **Drop cap missing.** Mock splits the body's first character into
  `.p-body .first` (48px `var(--serif)`, `#5a2a14`, floated left); capture renders
  uniform 18px body text.
- **New-entry snippet disagrees with the rest of its own screen.** The capture
  shows `Day 11` on the new-entry card while the card directly below it reads
  `DAY 12 · Summer` and the page stamp reads `Day 12`. Two different day numbers
  in one screen is the real finding here — **not** the difference from the mock's
  `Day 12`, which is a hard-coded mock seed. The shipped number is live world
  state: `VoxelPauseUISubsystem.cpp:56 CurrentDayNumber` returns
  `1 + floor(EpochSeconds / DayLength)` off the sky subsystem, so under the
  capture harness's frozen 12:00 / 03-20 sky it is legitimately whatever it is.
- **Stamp format truncated.** Mock `stampOf(WORLD)` renders the full
  `Day 12 · Summer, 18th Year of the Second Age`; the capture's new-entry card
  gives the day alone.
- **Ornament glyphs dropped.** Mock prefixes `✦` on every list stamp and on the
  page kind line; none appear.
- **Season case.** Capture `DAY 12 · Summer`; mock `${e.season.toUpperCase()}` →
  `DAY 12 · SUMMER`.
- **Stamp font.** `.p-kind` and card stamps are specified `var(--pixel)`
  (Press Start 2P); the capture uses a rounded serif/mono face. The README already
  records Press Start 2P as **not shipped**, so this is expected fallout of that.
- **In the capture, not the mock:** the `Q/E Switch screen` / `ESC Exit` hint bar.

Page copy, date stamp, body text, paragraph break, snippet truncation and the
300px / 1fr column split all match.

### Map

This is the one screen where the shipped UI is a **different design**, not a
divergent rendering of the mock. The mock is a full-bleed, drag-pannable
parchment map; the capture is a static world-overview raster with a data sidebar.
It should be treated as a design decision to confirm or reverse, not a list of
bugs.

- **Pannable parchment sheet replaced by a static hillshade.** Mock:
  `.parchment-frame` fills `menu-body` with a 4200x2800 `.sheet` (`cursor:grab`,
  Q/E rotate via `.rotor`) on a parchment gradient `#e8d3a0 → #c8a868`. Capture:
  a fixed hypsometric hillshade with no parchment ground and no pan/rotate.
- **Right-hand sidebar is not in the mock at all.** Capture adds `POSITION`,
  `LATITUDE / LONGITUDE`, `HEADING`, `CHUNK`, `SURFACE`, `SEED`; the mock's map
  occupies the full body width.
- **`SAVED PLACES` moved and restyled.** Mock: a collapsible drawer at the
  top-right *inside* the map frame (`.pl-drawer{position:absolute;right:0;top:0}`)
  with a `▾` caret and a `Filter places…` box. Capture: a permanently-open panel
  in the sidebar, no caret, no filter.
- **Empty-state copy truncated.** Capture `No places saved yet.`; mock `.pl-none`
  adds `Right-click the map to mark one.`
- **"You are here" marker missing.** Mock `.you` is a 16px `--hp-bright` `#e84a3a`
  dot with a pulsing gold ring at sheet centre.
- **Compass card.** Mock `.compass-card` is a 72px circular graduated limb with
  eight split spearheads; capture shows an opaque square tile with a 4-point rose.
- **In the capture, not the mock:** a `50 km` scale bar and a metadata strip
  (`seed 20260719 | 289 coarse tiles | 261 × 261 km @ 120 m/px | ...`).
- **Action bar reduced from 7 hints to 3**, and `Q/E` is bound to *different
  actions* in each (mock: rotate the map; capture: switch screen).
- **No mark-placing UI.** The mock's `#mkDialog` (`MARK THIS PLACE`, name input,
  symbol picker, DELETE / CANCEL / SAVE) has no counterpart.
- **Shell smaller than spec.** Measured ~1321x902 device px against
  `.menu-shell.compact` `min(1060px,…) x min(760px,…)` = 1413x1013 at authoring
  zoom: ~7% narrower, ~11% shorter. With the added sidebar the map viewport is
  roughly half the mock's area.

### Player

The strongest data match: `ROLAND` / `WAYWARD HEIR · ARMOUR 42`, the 4+4 gear
columns, `MAIN LEVEL 6`, `XP 2,140 / 5,080` at ~42% fill, and every value in all
four right-hand stat blocks is correct. Rarity ring colours are right too.
Deltas are presentational:

- **COMBAT block flattened from a matrix to a list.** Mock `.stat-mtx` is a
  4-column grid with `SLASH` / `STAB` / `BLUNT` headers over `Armour` (21/34/35)
  and `Weapon` (28/30/12). Capture drops the headers and emits six rows with
  ellipsised labels (`Armour · sl… 21`, `Armour · s… 34`, ...), so the damage-type
  names are unreadable. **This is the one Player delta that costs information.**
- **COMBAT swatch colours wrong.** Capture uses purple for Armour and red for
  Weapon; mock uses `.ico-shieldsk` (bronze `#b07a3a`) and `.ico-blade`
  (steel `#d8d4c8`). Purple is `--rare-epic #a04ac8`, which the mock reserves for
  the Stealth & Presence glyphs (where the capture does use it correctly).
- **Sub-tabs lost the heraldic shields.** Mock `.sub-tab` stacks a 36x42
  clip-path shield with an icon above the label; capture renders plain text tabs.
- **Section headers dropped the `◆` diamond** on all four titles.
- **Attribute and equipment glyphs are plain blocks.** Mock uses clip-path shapes
  (`.g-charisma` diamond, `.g-speed` bolt, `.g-vitality` heart, `.gl-helm` etc.);
  hues are correct, shapes are lost.
- **Character render is a placeholder**: black panel reading `No equipment system
  yet — these sockets are chrome.` against the mock's rotating `.voxel-char`.
  Worth confirming this is intended to ship.
- **Action bar hints replaced.** Mock `E Spend point` / `F Compare`; capture
  `Q/E Switch screen` / `ESC Exit` — despite `2 unspent points` being displayed.
- **Not covered:** only the STATS sub-page was captured; SKILLS, PERKS and
  REPUTATION were not compared.

### Codex

- **Recipe detail pane uses the wrong panel style.** Mock `.rc-detail` is a dark
  iron panel (`background:#0a0805`, `.rc-title` in `var(--gold)` #f0c14b at 4px
  tracking, body in `var(--mono)` VT323 18px). The capture renders it in the
  parchment `.entry` treatment (tan ground, `--parchment-ink` #3a2a14, `--hand`
  body) that the mock reserves for lore entries only.
- **The 3x3 crafting grid is missing.** Mock `.craft-area` has `.craft-3` (nine
  64px slots showing the pattern), a bronze `.craft-arrow`, then `.out-wrap`.
  The capture has only the yield slot, and places it inline left of "Made at
  Crafting grid" instead of in its own right-hand column.
- **Placeholder text in the ingredient row.** Mock `48 in pack` (green
  `--rare-uncommon` #5fa84a, red when short); capture `"not an item yet"`. Also
  `1 X Oak Log` uses ASCII `X` where the mock uses `×` (U+00D7).
- **Category tallies differ.** Recipes `23`→`15`, People `4/12`→`4/7`, Lore
  `5/14`→`3/3`; Factions `2/4` and Places `0` match. (The mock's own labels are
  hard-coded and do not match its `LORE` arrays either.)
- **Seven recipes missing.** Building lacks Ladder, Crafting Table, Glass Pane;
  Tools lacks Iron Bucket. Building order also differs.
- **Group headers.** Mock `.grp-head` is `▸ BUILDING <count>` — serif, uppercase,
  3px tracking, caret, right-aligned count; capture shows lowercase `Building`
  with neither caret nor count.
- **Search placeholder.** Mock `"Search recipes or ingredients…"`; capture
  `"Search recipes"`. The `.clear` `×` affordance is absent.
- **In capture, not mock:** a `BUILDING` category eyebrow above the recipe title;
  tab key-letter prefixes; the `Q/E Switch screen` / `ESC Exit` footer.
- Station line: mock emphasises the station name in `var(--gold)`; capture gives
  it no emphasis.

### Death screen

- **Date stamp truncated and mis-cased.** Mock `#stamp` `"DAY 12 · SUMMER, 18TH
  YEAR OF THE SECOND AGE"`; capture `"Day 11"` — the season, year and age are all
  dropped, and it is mixed case where the mock is uppercase `var(--serif)` at 5px
  tracking. **The number itself is not a defect** (live sky-derived day, see the
  Journal note); the missing season/year/age and the casing are.
- **`.version-stamp` missing.** Mock has `"DEV BUILD v0.18.2"` at
  `left:48px; bottom:32px`; nothing renders there.
- **Button row far too high.** Mock `.actions{bottom:120px}` ≈ 89% down the frame;
  capture puts RESPAWN/QUIT at ~69%.
- **RESPAWN and QUIT have different fills.** Mock gives both the identical
  `linear-gradient(180deg,#6a3a14,#3a1e08)` and brightens only on `:hover`;
  capture paints RESPAWN bright orange (reads as `--warm-primary` #E8873A).
- **`.fade` vignette absent** — mock takes the corners to pure black; the
  capture's corners still show lit terrain.
- **`.world` desaturation not applied** — mock ends at `filter:grayscale(1)
  brightness(0.28)`; the capture's terrain is still tan and much brighter.
- `.rule` is the right width (~520px) but has hard ends; the mock fades to
  transparent at both.
- The quip is drawn from the `GENERIC` pool while the mock hard-codes
  `CAUSE='goblins'`. Random by design — flagged only in case the cause plumbing
  is meant to be live.
- "YOU DIED" position, size, tracking and `--blood-bright` all read correct.

### Dialogue

Layout matches well: options top-right, skills strip bottom-right, and every
disabled/enabled state is correct against the stat values shown. Deltas:

- **Selected-option cartouche missing.** With `selected = 0`, row 1 should carry
  `.dlg-option.selected` (gold gradient band, 1px `--gold-rule` top and bottom,
  `box-shadow:0 0 24px var(--gold-soft)`, bronze/gold key box). The capture gives
  row 1 no highlight at all — nothing distinguishes it from row 2. **This is the
  one Dialogue delta that costs function**, since it is the selection indicator.
- **Option text is italic; the mock is upright.** `.dlg-option` sets no
  `font-style`; only `.dlg-speaker__line` and `.role` are italic.
- **NPC portrait frame is empty.** `.dlg-portrait` (160x200, bronze triple-inset)
  renders, but the Captain Vossant artwork inside it does not.
- **`.dlg-option__diamond` is not rotated** — mock is a 6x6 gold square at
  `rotate(45deg)` with a glow; capture draws it axis-aligned with none.
- **Skill glyphs are flat rectangles** rather than the `clip-path` pixel-art
  silhouettes, in both the option rows and the bottom strip.
- **`.dlg-stage` scrim missing** (plus `.scene-tint`), so the top-right options
  sit on bright unshaded terrain.
- The opening quote renders as a right double quote (`”The fine is a hundred…`);
  the mock source uses a straight ASCII `"`.
- **One place where the port is probably right and the mock is wrong:**
  `.dlg-speaker` is the only in-flow child of a `flex-direction:column` stage with
  no `justify-content`, so in a browser it lands top-left at y=48px. The capture
  puts it bottom-left, which matches the CSS comment ("bottom-left portrait +
  name + spoken line") and `.dlg-companion{bottom:48px}`. Do not "fix" the port
  to match the mock here without deciding which is intended.

### HUD v2

Compass, dock geometry and bar layout line up well: compass centred at
`top:12px` with the tick tape in the correct order, dock centred at `bottom:12px`,
bars spanning the 694px hotbar width with the 10px mid gap on the slot 5/6 seam,
the 10-division tick overlay present, and slot 1 carrying the gold triple-inset
ring. Deltas:

- **Hunger bar is the wrong colour.** Mock `.bar.stam .fill` is
  `#bfe6f5 → var(--stam) #c8a04a → var(--stam-deep)`, i.e. amber/gold with a pale
  blue highlight only on the top row. The capture paints it solid steel blue for
  its full height.
- **Bars are flat fills with no vertical gradient.** HP should run
  `#d44a3a → --hp #b8302a → --hp-deep #5a1410` with an inset highlight; the
  capture is one uniform dark red.
- **Nine of ten hotbar slots are empty.** The mock fills all ten with glyphs and
  quantity badges; the capture has content only in slot 1. This is live inventory
  state, so it is a divergence only if the HUD is meant to show the mock's seed.
- **Item icon is a flat colour swatch, not a glyph,** and it is not centred:
  `.slot .glyph{inset:15px}` should leave equal margins, but the swatch sits
  toward the top-left of the 64px cell.
- **Interact prompt at screen centre.** Mock `.interact{left:62%; top:54%}`;
  capture puts `E Examine the cairn` at ~50%/50%. Probably deliberate (it should
  track the reticle), but it is a literal delta.
- `.slot.active .num` should be `var(--gold)`; the "1" reads parchment-white like
  the inactive numbers.
- The `.grain` scanline overlay is not present. Low-HP vignette correctly off at
  full HP.

### One capture silently failed, and md5 is how it was caught

`VoxelMenu00017.png`, taken as `-Shot Panel -Panel save`, is **byte-identical to
the main menu** `VoxelMenu00015.png` (both md5 `e9d094f9601ecb784000303b6048e4b1`).
It rendered the title screen with no overlay: SAVE exists only on the pause menu,
so nothing opened, and the harness reported a successful capture anyway. The
script's own usage line at `tools/voxel-ui-capture.ps1:33` gives the right route,
`-Shot Pause -Panel save`.

Re-captured 19:01-19:02 with `-DemoSaves` (log: `6 save(s) listed`, so the arm
engaged and could have failed):

| screen | shot | frame | md5 |
|---|---|---|---|
| Save dialog | `-Shot Pause -Panel save -DemoSaves` | `VoxelPause00006.png` | `3aff1de9…` |
| Load dialog, populated | `-Shot Pause -Panel load -DemoSaves` | `VoxelPause00007.png` | `f2e66879…` |

All three pause-route captures hash differently, which is the check that the
first failure would have passed. **Hash every capture set that is supposed to
differ** — a UI capture that photographs the wrong screen looks exactly like a
successful one in the log.

### Main menu

All seven `.title-menu__item` rows present in mock order, right-aligned, with the
`.quit` extra top gap (~65 px measured vs 69 px specified). CONTINUE and LOAD GAME
correctly dimmed for a no-saves state. Callout copy is character-for-character
identical.

- **Active-item cartouche missing.** `.title-menu__item.active` specifies a gold
  cartouche (gradient fill, `border-top/bottom:1px solid var(--gold-rule)`,
  `box-shadow:0 0 32px var(--gold-soft)`, `min-width:380px`). NEW GAME has none of
  it — only the `font-size:33px` part landed. **Same class of defect as the
  Dialogue screen's missing selection band**: the selected-item indicator is the
  part that did not port.
- **Logo ~72 px too far right.** Measured right margin 61 px; `.title-logo` is
  `right:100px` → 133 px at 2560. Top edge correct.
- **Callout body too large and loose.** `.callout-news__copy` is `16px` /
  `line-height:1.45` (≈31 px pitch at 2560); capture measures ≈54 px and wraps to
  6 lines.
- Backdrop art cannot be compared: `assets/menu_backgrounds/cave_background1.jpg`
  is not present under `docs/ui-mocks/2026-09-07/`.

### Loading screen

All five mock elements present and correctly ordered; title width 373 px against
~355 px predicted; TIP row centred to within 1 px and using the mock's gold-serif
`TIP` + italic-hand structure.

- **Hourglass is ~1.9x too wide.** Measured 118x94 px; `.hg-flip` is `46x69` →
  61x92 at 2560. Height matches, width does not, so the glass reads squat
  (aspect 1.26) where the mock is tall (0.67).
- **Hourglass detail layers missing:** curved bezier bulb walls, the `url(#rim)`
  glass stroke, two specular streaks, neck collar rects, post highlights,
  `.hg-shadow`, and the `rotateX(-10deg) rotateY(-14deg)` tilt. The capture is a
  flat straight-line X. Sand top/mound/falling grains are correct.
- **Engine debug text over the screen:** `Preparing Shaders (1)` and
  `'DisableAllScreenMessages' to suppress`, top-left, in default Roboto. Not in
  the mock, and it would ship to a player on a cold shader cache.
- **TIP line ~52 px too low** (23 px from frame bottom vs `bottom:56px` → 75 px),
  while the version stamp is exact — so this is one row's offset, not a global
  bottom-anchor error.
- **Centre stack ~54 px high**, and inter-element gaps run 40/31 px against
  `gap:18px` → 24 px.
- **Message pool differs in flavour.** Capture `"Lighting the ash-throne's
  braziers..."`; none of the mock's 18 `TIPS` entries match, and the mock's are
  worldgen-flavoured (`"Reticulating cave splines"`). Single samples from rotating
  pools, so only the flavour mismatch is checkable.
- **Unattributed:** a ~12 px checkerboard lattice at ~13% brightness modulation
  across flat sky (sampled 66 vs 76 grey). The mock's `.grain` is 1 px scanlines
  at 1.8% alpha, nothing like this. Source not determined — UI grain layer or the
  backdrop art itself.

### Pause menu

Structure is right: title, `.pa-rule`, six `.pa-btn` rows in mock order, `›`
chevrons, per-row hairlines, `.pa-btn.danger` salmon on EXIT TO MENU and QUIT,
centred italic footer. Panel centred to within 8 px vertically.

- **Button label wrong.** Row 3 reads `LOAD GAME`; the mock's third `.pa-btn` is
  `LOAD`.
- **Footer truncated.** Capture `Day 11`; mock `.pa-foot`
  `Day 12, the 18th Summer of the Second Age`. Font, style and position correct —
  only the string is reduced to the bare day counter. (The number is live state;
  see the Journal note.)
- **Panel ~1.34x too wide.** Outer frame ≈550 px; `.pa-panel` is `width:380px` +
  `padding:0 28px` + 2 px border then `scale(0.7)` → ≈411 px at 2560. The capture
  matches the *unscaled* box, i.e. the mock's own 0.7 factor did not port.

### Settings panel

Good match on chrome: panel ≈820 px against 800 px specified, centred, title with
`.se-rule`, `AUDIO` / `DISPLAY` headers in `--warm-primary` with the trailing
gradient hairline, slider track/fill/knob/value, and the footer in mock order with
the orange primary on `SAVE & LEAVE`.

- **SFX slider missing.** Mock AUDIO has MASTER, MUSIC, SFX; the capture goes
  MASTER → MUSIC → `DISPLAY`. SFX would be above the scroll fold, so this is a
  definite absence, not a scroll artefact.
- **In the capture, not the mock:** the whole `GRAPHICS` section (Fine Detail
  Smoothing, Faster Terrain Drawing, Water Wave Detail, each with a two-line
  italic hint) and the `Takes effect on APPLY.` hint under FULLSCREEN. These are
  real shipped rows (`SVoxelSettingsPanel.cpp:72-81`), so the **mock is behind the
  product** here, not the other way round.
- **Hints wrap to their own line.** `.ck-row` is `display:flex; align-items:center`
  with `.ck-hint` a sibling at `margin-left:6px`, i.e. inline to the right of the
  label. Every hint in the capture is on a second line.
- **Label casing inconsistent within one panel.** `.ck-label` is all-caps
  (`FULLSCREEN`); the three GRAPHICS labels are Title Case in the same face.
- **Slider value font wrong.** `.sl-val` is `"Courier New",monospace 14px`; the
  `100` readouts render in the Macondo swash serif.
- **Panel scrolls where the mock does not**, hiding ~90 px (~14%). **Ocean Mesh
  Detail — one of only two player-facing water rows — is below the fold**, and the
  mock's `GAMEPLAY` section and `MINING` cycler can be neither confirmed nor
  denied from this capture. Fix the panel height before asking the owner to judge
  water settings.

### Save dialog

`VoxelPause00006.png` renders the dialog correctly and closely: `— SAVE GAME —`
title, `.sv-rule`, the context row, `NAME THIS SAVE` prompt, a parchment
`.sv-input` carrying `VOXELMARK Day 1` with a `15 / 40` counter, the overwrite
`.sv-warn` (`A save named VOXELMARK Day 1 already exists. Confirm to overwrite
it.`), the `ENTER to confirm` / `ESC to cancel` key row, and a `CANCEL` /
`CONFIRM` footer with the orange primary on CONFIRM.

- **Context row content differs.** Mock `.sv-context` is `✦` + `ROLAND · DAY 12`
  (character and day); the capture shows a solid diamond + `VOXELMARK` (the world
  name). Both the glyph substitution and the field choice differ.
- Background not blurred/dimmed, and the HUD compass and hotbar stay drawn behind
  the panel — same as Pause.

### Load dialog

`VoxelMenu00018.png` (title-screen route, zero saves) is a strong match: panel
≈1062x873 against 1040x853 specified, centred; `LOAD GAME` title, `.ld-rule`, the
full `.ld-header` with `ALL` / `MANUAL` / `AUTO` and `ALL` carrying the
`.ld-filter.on` orange gradient, a parchment `.ld-search` with `Search…`, and
`.ld-count` reading `0 / 0`. Footer `CANCEL` right-aligned above a top hairline.

- **Empty-state glyph substituted.** `.ld-empty .glyph` is `∅`; the capture draws
  a solid filled diamond. Sampled colour `#5A3818` = `--leather-edge` is exact, so
  this is glyph coverage in the serif face, same as the main menu's `✦`.
- **Empty-state text differs.** Capture `"No saves yet. Start a New Game to
  begin."`; mock `"No saves match."` The mock's string is written for a
  filter-returns-nothing state, so the capture's wording is arguably the better
  fit for genuine zero saves — a delta, not necessarily a defect.
- **Empty block is top-anchored, not centred.** `.ld-empty` is `flex:1;
  justify-content:center`; list area centre is ≈730, the block's centre ≈570.
- **Empty text wraps at ~490 px inside a ~1040 px panel**, breaking as
  `"No saves yet. Start a New Game to"` / `"begin."` `.ld-empty` has no max-width
  in the mock.
- Row styling, `.ld-thumb` day badges, `LATEST` / `AUTO` tags and the
  `.ld-btn.primary` / `.ld-btn.del` pair were not checkable at zero saves.
  **`VoxelPause00007.png` (6 demo saves) exists for exactly that and has not yet
  been diffed.**

### Three more cross-cutting items

5. **Version stamp string.** All four screens that carry it print
   `Milestone 5-3D — dev build` where the mock says `DEV BUILD v0.18.2`.
   Placement is pixel-correct, so this is one string.
6. **Missing glyphs in the serif face.** `✦` (main menu callout, save context)
   and `∅` (load empty state) both fall back to a solid diamond, with exact
   colours. Font glyph coverage, not styling.
7. **No blur/dim behind overlay panels.** Pause, Settings, Save and Load all
   specify `.bg { filter: blur(8px) brightness(0.32-0.35) saturate(0.6) }` plus a
   vignette; every capture shows a sharp, full-brightness backdrop with the HUD
   still lit behind it.

**Measurement caveat:** every capture is **2560x1398**, not the 2560x1440
requested — 42 px short, presumably window chrome. Bottom-anchored elements above
were judged against the 1398 frame, so bottom-offset figures carry that
uncertainty.

### UI polish pass, 2026-09-07 (VoxelEarthUI only)

Worked from the list above. No build, no editor, no capture -- every claim below
is a source-level or file-level measurement, and every visual claim needs the
box owner's re-capture at the new 1.0 DPI step to confirm.

#### THE FONT: what the owner has to supply, and where

**Press Start 2P is not in the repository and this pass did not add it.** A
listing of `ue-project/Content/UI/Fonts/` on 2026-09-07 holds exactly four
faces -- `MacondoSwashCaps-Regular.ttf`, `VT323-Regular.ttf`, `IMFeENrm28P.ttf`
and `IMFeENit28P.ttf` -- and `docs/ui-mocks/2026-09-07/README.md` already
records `--pixel` as "not shipped". What shipped instead is the wiring:
`FVoxelUIStyle::Pixel(int32)` loads

> **`ue-project/Content/UI/Fonts/PressStart2P-Regular.ttf`**

by path if it is there -- the same `FStandaloneCompositeFont` route the other
four use, so **dropping that one file at that one path is the entire change: no
.uasset, no import, no editor, no code edit and no rebuild of this module.** The
file is the OFL "Press Start 2P" regular from Google Fonts (the same source as
the four already committed; its OFL text belongs beside them in
`Content/UI/Fonts/OFL.txt`). Until it arrives, `Pixel()` returns **VT323**, not
the engine default and not Macondo: VT323 is the only fixed-pitch,
bitmap-derived face in the shipped set, so the `--pixel` sites read as small
caps rather than as the serif swash the 2026-09-07 journal capture shows.
`FVoxelUIStyle::IsPixelFontAvailable()` reports which face is in use, and the
missing-file warning is printed once at startup by the same helper that reports
the other three.

The `--pixel` sites now routed through it: the journal card stamps, the journal
NEW ENTRY card, and the journal page's `.p-kind` line.

#### Four items in the list above that are wrong, with the receipts

1. **The hunger bar's colour is correct as shipped.** The list reads
   `.bar.stam .fill` against `menus_shared.css`'s `--stam:#c8a04a` and concludes
   the port picked the pale-blue stop of an amber ramp. **"Voxelmark HUD v2.html"
   carries its own `:root` and overrides `--stam:#6fb8d8; --stam-deep:#2a5a78`**
   (line 20 of that file), and the port follows the screen it is cloning --
   which `VoxelUITheme.h` says out loud at the `HudHungerFill` declaration. The
   bar is meant to be blue. Do not "fix" it to gold.
2. **The Codex ingredient line already uses U+00D7, not an ASCII X.**
   `VoxelUIStrings.cpp`'s `CodexIngredientLine` is `"{0} x {1}"` with a real
   U+00D7, stored as UTF-8 (`c3 97`), and UnrealBuildTool passes `/utf-8`
   unconditionally on MSVC (`VCToolChain.cs:708`), so a BOM-less UTF-8 source is
   read correctly. There is no encoding bug and no substitution to make.
3. **Every absolute pixel figure in the list above is a DEVICE-pixel figure.**
   They were measured on 2560-wide captures at a 1.333 scale. ADR-0011 keeps
   that scale, so the numbers are still what the screen shows -- but they are
   not comparable to the mocks' authored px or to anything in `VoxelUITheme.h`
   without dividing by 1.333 first. Several entries in the list above compare
   the two spaces directly; `LogoRight` is the one that was acted on, and it is
   re-derived below.
4. **The `.title-menu__item.active` cartouche was built and is not missing** --
   `SVoxelMenuButton`'s Cartouche variant draws the gold fill and both 1 px
   rules. What was wrong is *when*: it keyed only off `IsLit()`, i.e. hover or
   keyboard focus on the inner `SButton`. See the next section.

#### The selection indicator, on two screens, was one bug

The title screen and the dialogue overlay both lost their only "which item am I
on" affordance in the captures, and it is the same cause: the Cartouche
variant's band was drawn only when the inner `SButton` held keyboard focus, so
anything that takes focus elsewhere -- a viewport click, the capture harness, a
panel focusing its own default -- erases it. `SVoxelMenuButton::IsCartoucheOn()`
now ORs the existing `Active` attribute into that predicate (kept out of
`IsLit()` so a selected Leather filter chip does not also take the hover plate).
The dialogue binds it to `Selected == I`; the title screen binds it to
"no title item holds focus", so NEW GAME carries the resting `.active` band and
yields the instant any item is actually focused.

#### One mechanism worth keeping: CSS line-height does not port

`STextBlock::LineHeightPercentage(N)` is **N x the face's own line height**
(`TextLayout.cpp:451`, `LineSize.Y = UnscaleLineHeight * LineHeightPercentage`),
where CSS `line-height:N` is **N x the font size**. IM Fell English -- the
`--hand` face every wrapped body block here uses -- has a natural line height of
**1.269 em** (hhea 1638 / -961 / 0 over 2048 upm, read from both .ttf files), so
the nine sites that passed the mock's CSS number straight through were laying
out at 1.27x the intended leading. That is most of the list's "callout body too
large and loose" (mock 16px / 1.45; capture measured a ~54 device px pitch
against ~31 predicted). `VoxelUITheme::HandLineHeight(Css)` now does the
conversion and the call sites still write the mock's own number.

#### Left deliberately, and why

* **Weight readout `112 / 180`.** The mock shows carried-over-capacity; the
  product has no carry capacity at all (`FVoxelInventoryScreenData` carries
  `CarriedKg` and nothing else). Printing a denominator would invent a game
  rule. The 11 px `.weight i` swatch WAS added; it is the rotated square this
  front end already uses for missing ornaments, because a cmap dump of all four
  shipped faces found **none** of U+2B22, U+2B21, U+2726 or U+2205.
* **The right-pointing caret on the Codex group header and the four-pointed star
  on the journal card stamps.** Same cmap dump: U+25B8, U+25BE, U+25BC and
  U+2726 are in none of the four faces. The group header took the half that does
  not need a glyph -- upper case, tracking, and the right-aligned tally. Drawing
  the carets needs either the pixel font above or hand-built geometry, and
  neither is worth doing blind.
* **RESPAWN's warm plate on the death screen.** The mock gives RESPAWN and QUIT
  the identical leather gradient; the port makes RESPAWN primary. That is an
  affordance the mock does without, not a porting error -- an owner call.
* **`TitleFontSize = 84` against the mock's 108, and `PausePanelWidth = 380`
  unscaled against the mock's own `scale(0.7)`.** Both are recorded deviations
  with reasons in `VoxelUITheme.h`. At the new 1:1 step they now land at 84 and
  380 *physical* pixels on a 2560-wide screen, which is materially smaller than
  they looked at 1.333. Worth an owner look on the re-capture; not changed
  blind.
* **The Map screen.** The list itself says it is a different design, not a
  divergent rendering. Out of scope for a polish pass.
* **The Journal drop cap, the Player COMBAT matrix, the Codex 3x3 grid.** Real
  divergences, all structural rather than cosmetic, and none of them cheap. Left
  for a pass that can capture what it changes.

#### The five suspect constants, re-derived (ADR-0011 consequence 5)

| constant | verdict | why |
|---|---|---|
| `LogoRight` | **120 -> 100** | The mock states the number: `.stage .title-logo{right:100px}`. The +20 was added to "restore the ink margin" after a capture measured 34 px against an expected ~60 -- 34 device px is 25 units, and the ~60 was authored px, so the two were never in the same space. The Macondo swash-K overhang is real but a browser right-aligns on the box too, so the mock carries the same overhang and the port should not be compensating for it. If a capture shows the K crowding the frame, the fix is right padding on the logo text block, not a bigger inset on everything. |
| `TitleFontSize` (84 vs mock 108) | **left, and it is moot** | **The constant is dead.** A grep of every .cpp in the module on 2026-09-07 found no reader: the 2026-09-07 title screen draws its wordmark from `LogoFontSize = 132` (`.title-logo__name`), and the only surviving references to `TitleFontSize` are its own ini registration. It cannot be re-derived because it does not reach a pixel. |
| `TitleBoxWidth` | **left, and it is moot** | Same: dead, only its ini registration remains. It was also never a mock figure -- it is a Slate text-clipping workaround measured off a render, which is why its own comment forbids recomputing it from font metrics. |
| `DlgDimAlpha` (0.55) | **left** | The mock states no single number for this layer: it stacks `rgba(0,0,0,0.25)` on a radial `.dlg-stage` wash reaching 0.6, and the port flattens both into one flat layer because Slate has no radial brush. There is no mock px value to re-derive from. Alpha, so scale plays no part either way. |
| `PausePanelWidth` (380) | **left, it IS the mock value** | `.pa-panel{width:380px}`. The deviation is that the port drops the mock's own `scale(0.7)`, which the existing note argues is the author shrinking one card to sit beside two sibling states in one file. That is an owner judgement about the mock, not a constant to re-derive. |

`SettingsPanelHeight = 660` was a sixth suspect and is **deleted** -- see below.

#### Every 1 px border promoted to 2 (ADR-0011 decision 3)

`VoxelUITheme::RulePx = 2.f` is the new floor, as a `constexpr` in the theme
rather than an `FVoxelMenuLayout` field: it is a rendering constraint, not a
design choice, and nothing should be able to tune it back to 1 from an ini.
After the pass a scan of every `SOverlay` ring stack in the module finds **no
band narrower than two units**, and no `HeightOverride(1.f)` /
`WidthOverride(1.f)` / `FMargin(1.f)` anywhere in `SVoxel*.cpp`.

Two shapes needed fixing, and the second is the one that is easy to miss:

* a **hairline** drawn as a 1-unit `SBox`, and
* a **band inside a ring stack**, whose width is the *difference* between two
  consecutive `SOverlay` insets. `2` then `3` looks like two healthy numbers and
  is a one-unit band. Six stacks were of that shape.

| screen / helper | what it was | now |
|---|---|---|
| `VoxelOverlayChrome::Rule` (pause, settings, save, load titles + footers) | 1 u | 2 u |
| `VoxelOverlayChrome::Panel` ring stack (all four overlay dialogs) | border 2 / edge 1 / leather 2 | 2 / 2 / 2 — `OverlayEdgeRingPx` 1→2, `OverlayInnerRingPx` 3→4 |
| `VoxelOverlayChrome::KeyCap` (every `<kbd>` in the dialogs) | 1 u rule | 2 u |
| `VoxelScreenChrome::CardRule` (hairlines inside every oak card) | 1 u | 2 u |
| `VoxelScreenChrome` `kRingPx` — OakCard, IronWell, **ItemSlot's rarity ring** | 1 u | 2 u |
| `VoxelScreenChrome::ParchmentPanel` | already asked for `kRingPx * 2` | now plain `kRingPx`, so it stays 2 rather than becoming 4 |
| `VoxelScreenChrome::Track` ×2 (level bar, reputation bar, **HUD vitals**) | 1 u surround | 2 u |
| Main menu — callout left rule, callout right rule, `.callout-news__glyph` plate | 1 u | 2 u |
| Menu button — Cartouche top + bottom gold rules; `.pa-btn` bottom hairline | 1 u | 2 u |
| Settings — slider track well, checkbox well (and the value fill's inset, which had to move with the ring or the warm bar paints over its own border) | black 1 / edge 1 | 2 / 2 |
| Settings — `CheckboxMarkSize` | 14 u | **10 u** — the well's interior fell from 18 to 14, so the old mark exactly filled it |
| Journal — page rule, goal rule, step marker dot | 1 u | 2 u |
| Codex — page rule | 1 u | 2 u |
| Player — perk plate edge (2/3), reputation bar (edge 1) | 1 u | 2 u |
| Map — parchment frame's bronze ring (2/3) | 1 u | 2 u |
| Dialogue — skill-cell divider; option key plate; portrait ring (2/3/5); skill strip ring (2/3/3) | 1 u | 2 u throughout (portrait now 2/2/2) |
| Screen shell — tab plate (1/2) | 1 u | 2 u |
| Save dialog — context band, context icon, `.sv-input` plate | 1 u | 2 u |
| Load dialog — search plate, row plate, `.ld-tag` chip, `.ld-thumb` badge | 1 u | 2 u |
| HUD — compass frame bronze band (2/3), interact key ring, **bar tick rules** | 1 u | 2 u |

**Three to judge on a capture, flagged rather than exempted.** Each is a place
where two units is a large fraction of a small control, and none of them can be
settled without a picture:

1. **The HUD bar ticks.** Nine 2-unit black rules across an 11-unit bar, where
   the CSS asks for 1 px. This is the promotion most likely to read as too much
   black. If it does, the honest fix is a lower tick alpha, not a 1-unit rule.
2. **The HUD vitals bars.** `Track`'s black surround went 1→2 per side, so the
   coloured fill inside an 11-unit bar drops from 9 units to 7.
3. **The settings checkbox.** The well's chrome doubled and the mark had to
   shrink 14→10 to keep a gap around it, so a ticked box reads smaller.

Everything else lands on a surface at least 40 units across, where one extra
unit of chrome per side is not a proportion anyone will notice.

#### Icon and ornament audit (ADR-0011 decision 3, third bullet)

**One violation, and it is art rather than code.**

* **The six menu backdrops are authored at 1920x1071 and drawn full-bleed.**
  `battle/castle_feast/cave/forest_fight/fortress_battles/sailing.jpg` are all
  1920 wide (measured from their JPEG SOF markers). On the owner's 2560-wide
  screen that is a **1.33x upscale**; on a 4K screen it is 2x. ADR-0011 asks for
  at least 2x the largest displayed size, so these are short by a factor of
  ~2.7 at 1440p. They are photographic rather than line art, so the failure mode
  is softness and not aliasing -- which is why nobody has filed it -- but a
  full-screen 1.33x upscale is exactly the class the ADR names. **Recommendation:
  re-source at 3840 wide** (2x of 1920, and 1x at 4K); `SVoxelCoverImage` needs
  no change, it computes its cover scale from the brush at paint time.
* **The map hillshade is fine.** `world-maps/seed*/01-heightmap-hillshade.png`
  is 1950x2085 and is drawn into a frame of roughly 800x930 device px, i.e.
  **downscaled ~2.2x** -- comfortably inside the rule.
* **Everything else in the front end is geometry, not bitmap.** Every rule,
  ring, plate, well, track, item glyph and ornament is `FVoxelUIStyle::SolidWhite()`
  -- a 1x1 white brush stretched under a tint -- so it is resolution-independent
  by construction. `VoxelOverlayChrome::Diamond` is an `SBox` under a render
  transform; `VoxelScreenChrome::ItemGlyph` and `VerticalRamp` are stacked boxes.
  There is no icon texture anywhere in `Content/UI` and `FVoxelItemDef` has no
  image field, so there is no icon art to audit.
* **Text is already compliant.** All four shipped faces are TrueType outline
  fonts (`unitsPerEm` 1000-2048, real `glyf` outlines), rasterised per size by
  FreeType. The fifth, Press Start 2P, is a pixel-styled face but is still a
  TrueType outline; when it is supplied it will scale like the rest.

#### Whole-pixel audit (kept, but no longer load-bearing)

ADR-0011 makes the scale continuous, so nothing lands on a device pixel at any
setting and rounding buys no alignment. It is kept because a fractional
*authored* value still buys nothing either, and because one of the five buys
something real that survives the doctrine change: rounding the compass tape's
offset gives the strip a STABLE subpixel phase, which is what stops its letters
shimmering as the player turns. Five fractional edges found and rounded:

| where | was | now |
|---|---|---|
| `SVoxelScreenChrome.cpp` `ItemSlot` glyph inset | `Size * (6/56)` = 6.214 at the 58 px pack cell | rounded to a whole unit |
| `SVoxelGameHud.cpp` interact key ring | `FMargin(1.5f)` | `2.f`, matching the compass frame's band in the same file |
| `SVoxelDialogueOverlay.cpp` option key ring | `FMargin(1.5f)` | `2.f` |
| `SVoxelInventoryScreen.cpp` craft grid | `InvCraftSlotGap(3) * 0.5` = 1.5 | asymmetric 2/1 halves -- the gap is still exactly 3, every edge whole |
| `SVoxelGameHud.cpp` compass tape offset | continuous, from the heading | rounded; the tape steps 1 unit per half a degree, below what a turning player sees |

Every other `Gap * 0.5f` in the module divides an even token. The remaining
fractional literals are alphas and line-height ratios, which are not pixel
edges.

#### The Settings panel: the fold, and the new row

* **The fixed panel height is gone.** `SettingsPanelHeight = 660` hid OCEAN MESH
  DETAIL below a scrollbar -- one of only two player-facing water rows. The
  panel is now content-sized (`VoxelOverlayChrome::Panel`'s documented
  `Height <= 0`) and capped by `MaxDesiredHeight` bound to the panel widget's
  own geometry, which is the whole overlay area in both hosts and therefore
  cannot feed back on the panel's desired size. That is the mock's own
  `max-height:calc(100vh - 24px)`. The `SScrollBox` stays as a backstop for a
  window shorter than the content, and its bar is now 12 units thick with 6 of
  padding so that if it ever does engage it is not mistaken for a border.
* **New row: INTERFACE / INTERFACE SIZE.** ADR-0011 decision 5, and the
  settings-panel policy: a manual multiplier on top of the engine's own
  ShortestSide curve, so the owner can settle framing without a rebuild:
  `VoxelGraphicsUserSettings::Get/SetUIScale`, persisted at
  `[VoxelGraphics] UIScale` in `GGameUserSettingsIni`, applied live through
  `FSlateApplication::SetApplicationScale`, **default 1.00**, 0.75-1.50 in 0.05
  steps, snapped and clamped in one place on both the read and the write path.
  `ApplyAll()` applies it at boot alongside the cvar rows and names it in the
  engagement log. `BuildSliderRow` grew a readout and a step parameter so the
  row could map a non-0..1 range without a second copy of the track. Its hint
  no longer claims pixel-exactness at 100% -- under continuous scaling nothing
  is pixel-exact at any setting, and the earlier wording promised something the
  policy does not deliver.

#### The unified screen shell: one size, and a resize dial (owner, 2026-09-07)

Two directives, verbatim, both recorded at the top of `SVoxelScreenShell.h`:
*"for the unified inventory/journal/map/player/codex UI menu, make it resizable
if a player wants to make the entire thing larger or smaller in run time"* and
*"it currently appears that the inventory UI menu size is slightly larger than
the default sizing for map, journal, player, and codex sections. unify this."*

**The size discrepancy had one named cause, and it was not a min-size leak.**
`SVoxelScreenShell` carried a `ShrinkWrap` argument whose entire effect was
`if (!ShrinkWrap) { SetWidthOverride; SetHeightOverride; }`.
`VoxelScreensUISubsystem` passed `ShrinkWrap(Tab == Inventory)` at two call
sites, so the inventory alone had **no size override at all** and its frame grew
to whatever the 8x8 pack needed -- the 1348x944 against 1326x906 measured on the
captures, i.e. 16 units wider and 28 taller in authored space. It was a
deliberate port of the inventory mock's `.menu-shell{width:max-content}`, which
the other four mocks do not carry.

**The frame side is what changed.** The shell is now unconditionally
`ScreenShellWidth x ScreenShellHeight` (the authored 1060x760) and `ShrinkWrap`
is a documented no-op, kept declared only so its two callers in
`VoxelScreensUISubsystem.cpp` -- another agent's file -- still compile. Whoever
holds that file can delete both call sites and then the argument.

**The content side is what makes that safe.** The comment the old code left
behind is a real objection: a fixed height that also fits the other four screens
would clip a row off the pack, and clipping an inventory row is worse than a
frame that is the wrong size. So the body now sits in an `SScaleBox` at
`ScaleToFit` + `DownOnly`. A body that fits is untouched -- `DownOnly` cannot
return a factor above 1, so nothing about map, journal, player or codex changes
-- and a body that does not fit is drawn uniformly smaller rather than losing
its bottom row. No reflow: the inventory keeps its authored layout and is scaled,
so it still looks like itself.

**The resize dial.** `VoxelScreenShellSettings` (new pair) holds it:
`[VoxelGraphics] ScreenShellScale` in `GGameUserSettingsIni`, default **1.00**,
0.75-1.50 in 0.05 steps, snapped and clamped in one place on both the read and
the write path, flushed immediately, and logged as
`VoxelScreenShell: scale %.2f` **only when the value actually changes** -- a
wheel notch that lands past a clamp must not produce the same line as one that
moved something. It carries an `OnScaleChanged` multicast, which the Settings row
deliberately does not subscribe to: both halves of that row are `TAttribute`s
that re-read the getter every paint, so a Ctrl+wheel made elsewhere is already
reflected next frame. The delegate is there for a future consumer that must
*act* on a change rather than display it.

It is applied to the shell frame as a **Slate render transform about the frame's
centre**, not as a layout scale -- so nothing inside re-measures or re-wraps at
any setting, and the five screens cannot look different from each other at 1.25
than they do at 1.00. At exactly 1.00 the transform is returned **unset** rather
than as an identity, so the shipped default takes the same paint path the widget
took before the dial existed.

| binding | where | note |
|---|---|---|
| `Ctrl` + mouse wheel | `SVoxelScreenShell::OnMouseWheel` | Only the SIGN of `GetWheelDelta` is used -- one notch is one 0.05 stop whatever magnitude the platform reports, or the size would differ per mouse. A bare wheel is passed straight through so the codex, journal and perk lists still scroll. **Caveat:** over one of those scroll boxes the list consumes the wheel first, so Ctrl+wheel resizes over the tab bar, the action bar, the panel margins and the inventory grid, but not over a scrolling list. The key bindings always work. |
| `Ctrl` + `+` / `=` / numpad `+` | `OnKeyDown` | Three spellings because a keyboard has more than one: Slate reports the physical key, so the main row's `+` arrives as `EKeys::Equals` with shift held. |
| `Ctrl` + `-` / `_` / numpad `-` | `OnKeyDown` | Same, via `Hyphen` / `Underscore` / `Subtract`. |
| `Ctrl` + `0` / numpad `0` | `OnKeyDown` | Back to 1.00. Any dial that can be moved by accident needs a way back that does not require remembering what it was. |

The Ctrl chords are tested **before** the tab keys, because the unmodified
letters below them are tab switches and a chord that fell through would page the
screen instead of resizing it. The screens run under `FInputModeUIOnly` with
`SetWidgetToFocus(Shell)` and an unlocked, visible cursor
(`VoxelScreensUISubsystem.cpp:720-723`), so both routes reach the shell.

**Settings row: INTERFACE / MENU SIZE**, immediately after the two Hide rows,
built with the panel's own slider-row builder exactly as INTERFACE SIZE is. Its
hint names the shortcut, because a dial buried in a panel is not how anyone
finds the size they want -- they find it by looking at the screen they are
resizing.

**Two more ADR-0011 misses, caught here.** Both were one-unit bands expressed as
NAMED CONSTANTS summed into an inset, which is why the first sweep's grep for
`FMargin(1.f)` did not see them:

* `SVoxelScreenShell.cpp` `kBodyRing1Px` / `kBodyRing2Px` -- `.menu-body`'s
  first two rings, 1 unit each. Both now `VoxelUITheme::RulePx`; the body's
  chrome grows two units per side.
* `SVoxelMenuButton.cpp` `kLeatherBorderPx` -- the width of a band in **two**
  stacks: the Leather plate's border (every dialog action button) and the Tab
  plate's edge ring. Now `RulePx`; both grow one unit per side.

#### Needs a file outside this pass's scope

**The journal shows two different day numbers at once**, and the fix needs one
line in `VoxelScreensUISubsystem.cpp`, which this pass did not own. The cause is
not the day derivation -- `VoxelPauseUISubsystem.cpp:56` is correct -- it is that
`VoxelScreenData::SeedJournal`'s newest placeholder entry is hard-coded to
**day 12** while the NEW ENTRY card prints the live `TodayStamp` (day 11 in the
capture). The seed entry is therefore dated *tomorrow*. The smallest honest fix
is to date the seeds relative to the live day:

    --- a/ue-project/Source/VoxelEarthUI/VoxelScreenData.h
    +++ b/ue-project/Source/VoxelEarthUI/VoxelScreenData.h
    -VOXELEARTHUI_API FVoxelJournalData SeedJournal(const FText& TodayStamp);
    +// TodayDay: the live day number the NEW ENTRY card's stamp is showing. The
    +// placeholder entries are dated relative to it so the screen cannot print
    +// two different "todays" at once. 0 keeps the mock's own 12/9/1 seed.
    +VOXELEARTHUI_API FVoxelJournalData SeedJournal(const FText& TodayStamp, int32 TodayDay = 0);

    --- a/ue-project/Source/VoxelEarthUI/VoxelScreensUISubsystem.cpp
    +++ b/ue-project/Source/VoxelEarthUI/VoxelScreensUISubsystem.cpp
    @@ around line 398
    -    Body = SNew(SVoxelJournalScreen).Data(VoxelScreenData::SeedJournal(DayStamp()));
    +    Body = SNew(SVoxelJournalScreen).Data(VoxelScreenData::SeedJournal(DayStamp(), CurrentDayNumber()));

with `SeedJournal` mapping its three seed days `(12, 9, 1)` onto
`(TodayDay, TodayDay - 3, 1)` when `TodayDay > 0`. Whoever owns
`VoxelScreensUISubsystem.cpp` should decide whether the day number is reachable
there directly or has to come through the same route `DayStamp()` does.

Two smaller ones, both outside VoxelEarthUI's polish surface:

* **The death screen's stamp is `"Day 11"`, not the mock's full
  `"DAY 12 . SUMMER, 18TH YEAR OF THE SECOND AGE"`.** The screen prints
  `FVoxelDeathScreenData::Stamp` verbatim; the missing season/year/age and the
  casing belong to whatever produces that string, not to the widget.
* **Pause, Settings, Save and Load are drawn with the HUD still lit behind
  them.** `SVoxelPauseMenu` does draw `VoxelOverlayChrome::Scrim()`, so this is
  a z-order question in whatever adds the HUD and the pause overlay to the
  viewport, not a missing scrim.

### Is the white terrain in VoxelVerify00974 real, or a colour-authority fault?

Asked because a fully white world is also what a broken colour authority looks
like, and the water is being judged against that terrain.

**On the evidence available it is the genuine surface, not a colour fault.** Two
independent reasons:

1. The capture log carries the healthy authority line, not the broken one:
   `Clipmap colour authority: PALETTE (one authority, default)
   (-VoxelClipmapVertexAlbedo=1.00, bytes as authored sRGB)`. PALETTE is the
   single-authority mode -- "the palette colour of each vertex's REAL surface
   material". The known fault (`voxelsim-one-colour-authority`) is the clipmap
   being told every land vertex is one material; that is not this line.
2. **Non-white materials do render in the same frames.** Both `VoxelVerify00962`
   and `VoxelVerify00974` show distinct tan/orange voxels scattered along the
   shoreline against the white. A broken colour authority paints everything one
   colour; two colours in one frame falsifies it directly.

Plausibility: the spawn column's ground top is 1644.2 m, and the frames show peaks
well above it, so alpine snow cover is expected at this site.

**What this does NOT establish**: the log does not name the biome or the material
ids, so this is an argument from the authority line plus a falsifier, not a
positive identification. A definitive answer needs the real classifier at column
-65102,-51084 (`vxc_terrainprobe`), which was not run -- the box was serialised
behind other legs. If the water work needs certainty about the ground it is being
judged against, that probe is the cheap next step, and per the standing rule it
must be the real classifier rather than a reimplementation.

### Open owner verdicts, unchanged by this pass

Still open, and none of them can be asked yet because no shipping-default wake
frame exists: sky-light pair (`voxel.Sky.SkyLightAtGroundZ`, `voxel.Sky.FogInSkyCapture`),
caustics default (the help text is now honest; the 0.5 value itself is still the
owner's call), shore-foam look, hull mask (commit c038f99), glider parked spawn.
Refraction and boat bobbing remain CLOSED.

## 2026-09-07 (water-look agent, night): the grey rings diagnosed and fixed in the generator; Single Layer Water research; shore/surface foam re-keyed on depth with a procedural breakup

Owner verdicts this section answers: *"The boat wake looks like a hard grey
blob on the surface and it makes no sense what so ever. I see no shore foam at
all"*; *"a grey plane covering almost the entire world map"*; *"Keep the ripple
and wake related work and code but fix this issue with the grey rings"*; and
*"tuning the Single Layer Water in UE5 to look better and have good surface and
shore foam effects ... do research for how other UE5 games with realistic water
have configured and setup their single layer water systems."*

### 1. What the three frames actually photograph (read before the diagnosis)

| frame | pose | what was injected | what it is a picture of |
|---|---|---|---|
| `VoxelVerify00964` | +12 m, pitch -35, yaw 45 | `-VoxelRippleWakeRadiusM=8 -VoxelRippleWakeStrengthM=0.9`, five discs at 4/6/8/10/12 m ahead, one step, frozen (`CAPTURE WAKE ARMED ... r=8.00 m s=0.900 m`) | Five 8 m discs of 0.9 m each, overlapping, clamped at the sim's 2.0 m ceiling (`fieldMaxAbs=2.0000 stateMaxAbs=2.0000`). A 2 m mound 16 m across. **Not a wake; a stress pattern.** The rounded-square grey region with a soft edge and blue water outside it is the ripple WINDOW (51.2 m, Chebyshev fade 0.42-0.485) seen in perspective; the faint darker disc inside it is the mound's WPO. |
| `VoxelVerify00966` | +6 m, pitch -18 | same injection | The same 2 m mound seen almost edge-on from 6 m: it occludes the lake. The "grey plane covering the world" is that mound wearing full foam. |
| `VoxelVerify00974` | +6 m, pitch -12, boat under way (`voxel.Boat.Throttle 1 12`, ~22 m travelled) | nothing injected; the boat's own bow/transom swept splats plus the boarding slam | The only frame that photographs a WAKE. Log at t+0.25 s: `field verified LIVE -- centre patch max field value 0.1919, max state height 0.0436 m`. |

Pixel measurements on `00974` (PIL, 4-px stride box means, original 2560x1398):
the grey region reads (148-182, 155-187, 158-187) everywhere in the window --
far ahead of the boat (182,186,187), mid-left (158,164,167), at the wake rings
(151,158,162), near-bottom (148,155,158). A vertical transect at x=2432 goes
snow (237,238,238) -> water OUTSIDE the window (62,142,181) at y=760-784 ->
grey (150,156,158) from y=808 down. So the grey is the whole window, it is
uniform to +/-15, and it stops at the window's fade start (21.5 m from the
boat; from +6 m at pitch -12 that lands ~2.8 deg below frame centre, which is
where it lands). The blue water beyond it is the control: same lake, same
light, no ripple window.

### 2. The diagnosis, and the two mechanisms it has to choose between

The shipped response was `saturate((|grad| + |h| * 4.0) * 8.0)`. Its knee is at
raw 0.125: 3 cm of ripple, or a slope of 1:8, is FULL foam. The hull in `00974`
is at raw ~0.36 (grad 0.19 + 0.044 * 4), 2.9x past the knee before the gain
does anything. That much is arithmetic and is the coordinator's diagnosis.

What the arithmetic does NOT explain on its own is the window being grey 20 m
AHEAD of the bow: the boat covered ~22 m in ~11 s (about 1.9-2.5 m/s) and the
sim's wave speed is 1.60 m/s (`RippleField: armed ... speed 1.60 m/s`), so
physically the water ahead of the bow is undisturbed. Two mechanisms fit the
frame:

* **(A) saturation of a faint, everywhere-non-zero field** -- the sim's
  spread-out remainder (millimetres; slopes ~0.01) after eight seconds under
  way, plus grid-speed numerical leakage (the stencil carries information at
  dx/dt = 6 m/s regardless of c), is above the 0.125 knee across the whole
  window; or
* **(B) a DC value in undisturbed texels** -- a bias-handling or fresh-texel
  error putting a constant non-zero into the field wherever it has never been
  stepped, which the gain then paints as foam.

They are told apart by a GAIN ladder on the shipping asset, no regen: under
(A) the grey resolves into arcs near the boat as the gain drops (a 4 mm
remainder is 0.13 foam at gain 8 and 0.016 at gain 1); under (B) a constant
raw >= 0.5 survives gain 1 and gain 0.25 as a uniform tone. Both ladders are
preceded by `DisturbanceFoamEnabled:0`, which is simultaneously the reach test
the coordinator asked for (the lake at the boat is the SHEET --
`voxel.Water.MeshImplicitLakes` defaults to 0, "the far-field sheet owns lake
basins at every range", and the 00974 command line did not pass it -- and the
sheet is where `-VoxelWaterMatScalar` builds its MID) and the answer to "is the
grey the foam at all". Verdict rows are in section 6.

### 3. The fix (authored; regen pending the box)

`ue-project/Tools/ripple_field_graph.py`, `build_disturbance_foam`:

    x    = |grad| + |height_m| * DisturbanceFoamHeightWeight     (was a baked 4.0; now a scalar, 1.0)
    foam = saturate((x - DisturbanceFoamThreshold) * DisturbanceFoamGain * DisturbanceFoamEnabled)

A dead band below `Threshold` (provisional 0.05: a 1:20 slope or 5 cm) and a
linear knee above it -- the same family the wind whitecaps already use
(`build_whitecap_foam`: `saturate((|gradient| - SlopeThresh) / (SlopeFull -
SlopeThresh))`). Texels the field has merely touched draw NOTHING, so the
window edge is invisible on undisturbed water by construction and the 3.3 m
fade is left alone (widening it would also soften the ripple NORMAL for no
reason). Gradient leads (HeightWeight 1.0: the hull's 0.044 m counts as 0.044
against a gradient of 0.19), because a wake is steep before it is tall. All
four numbers are ScalarParameters, so the entire ladder runs on
`-VoxelWaterMatScalar` against one regenerated asset; `Enabled:0` remains a
pixel-identical off. The ocean consumes the same helper and takes the baked
defaults (no MID path there).

### 4. Single Layer Water: what is documented, what is source, what is folklore, and what applies here

**Documented by Epic** ([Single Layer Water Shading Model](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine)):
the SLW output node takes *Scattering Coefficients* ("the rate at which light
scatters on particles within a medium"), *Absorption Coefficients* ("how easily
light penetrates the volume of water"; per-channel, the doc's example is R
0.0033 G 0.0016 B 0.0011), *PhaseG* (positive = forward toward the sun,
negative = back, 0 isotropic) and *Color Scale Behind Water* ("multiplies the
luminance of the surfaces below the water", above-water only). **Opacity on
the main node "controls the ratio between the volume's BSDF and the surface's
BRDF"** -- i.e. it is COVERAGE of the surface layer over the volume, which is
exactly what this project's `MP_Opacity = saturate(foam)` wiring uses it for.
Reflections: "generated screen tiles are used for an indirect draw SSR pass"
and then "a full screen pass to composite reflection captures, sky, and newly
computed screen space reflections". Blend mode Opaque/Masked; the pass runs
after the base pass and deferred lighting, before translucency.

**From the 5.8 engine source on this box** (`D:\UE_5.8\Engine`), which
outranks any forum:

* `Renderer/Private/SingleLayerWaterRendering.cpp:69-71`:
  `r.Water.SingleLayer.Reflection` -- "0: Disabled, 1: Enabled (same as rest
  of scene), 2: Force Reflection Captures and Sky, 3: Force SSR". Companions:
  `.Reflection.DownsampleFactor` (noise; pair with `.Reflection.Denoising`),
  `.Reflection.ScreenSpaceReconstruction` ("usually not needed, as water has
  mostly mirror reflections"), `.SSRTAA` (default 1), `.TiledComposite`,
  `.DistanceFieldShadow` (default 1), `.VSMFiltering` /
  `.ShadersSupportVSMFiltering` (default 0), `.DepthPrepass` (default 1,
  "necessary for proper Virtual Shadow Maps support"),
  `.RefractionDownsampleFactor`, and the `.Refraction.*Culling` family.
  **This project already ships `r.Water.SingleLayer.Reflection=1`**
  (`DefaultEngine.ini:226`, switched from 2 on 2026-09-06 with the reasoning
  recorded there; `LogConfig: Set CVar [[r.Water.SingleLayer.Reflection:1]]`
  in the 00974 log). Lumen on water is moot here: no mesh distance fields
  exist for voxel terrain, so Lumen has nothing to trace -- the ini says so
  and it is right.
* `Shaders/Private/BasePassPixelShader.usf:1140-1141, 1379-1384`:
  `BaseMaterialCoverageOverWater = Opacity; WaterVisibility = 1 - coverage;
  GBuffer.DiffuseColor *= coverage; DiffuseColor *= coverage`. The surface
  layer's diffuse IS lit -- forward, in the water pass, by
  `GetForwardDirectLightingSplit` (`:1480-1492`) plus the sky/indirect term
  gathered just above it. So a BaseColor foam at coverage 1 should be a lit
  albedo. The record on this water says it renders BLACK
  (`VoxelVerify00936`, the "black band"), which is why every foam here rides
  EMISSIVE. That contradiction is measured in this pass with
  `DisturbanceFoamEmissive:0` at full coverage (section 6): if the window goes
  black with emissive off, the forward-lit surface layer is dead on this
  water and foam can only ever be emissive paint until that is found; if it
  goes lit-white, the grey was simply the 0.6 emissive and the fix for "flat
  grey" is to let BaseColor carry the foam and reduce the emissive.
* `Shaders/Private/SingleLayerWaterShading.ush:234-238`: the volume's
  scattered luminance is multiplied by `(1 - EnvBrdf)` above water and the
  whole output by `WaterVisibility` -- the mechanism
  `docs/water-realism-analysis-2026-09-06.md` section 0 already documents for
  "far water goes dark".
* `Shaders/Private/Common.ush:1797-1800`: `MaterialExpressionNoise(Position,
  Scale, ...)` does `Position *= Scale` with an unconnected Position = world
  UU. **Consequence for this project: the shipped shore-band noise
  (`scale 0.35`) had ~3 cm features, not the "~3 m and ~1.4 m" its comment
  claims; the +/-0.9 m waterline perturbation was per-pixel speckle.** Fixed
  to 0.0035 in this regen (section 5).

**Epic's own Water plugin material, read from the asset itself**
(`Engine/Plugins/Experimental/Water/Content/Materials/WaterSurface/Water_Material.uasset`,
parameter and function names): foam is a TEXTURE product, not a scalar --
`T_WaterFlow_01_Foam_Tiled` + `_N`, with "Foam Texture Blend Min/Width", "Foam
MacroScale", "FoamContrast", "Foam powr", "Front Foam Scale"; it is gated by
DEPTH ("Depth for DF Foam", "Foam Dpeth Min" [sic], "Foam Distance", function
`WaterOpacityMaskFromDepth`) and, for oceans, by wave attenuation in shallows
("Beach Foam", "Enable Ocean Foam", "Wave Foam", functions `WaveDepthFalloff`,
`ComputeWavesAttenuationFactor`); and foam changes MORE than colour -- "Foam
Opacity", "Foam Roughness", "Foam Scattering", "Foam Scatter Bias", "Foam
Emissive" are all parameters, so Epic pushes coverage, roughness, the volume's
scattering coefficient AND an emissive term together. Normals flatten with
distance ("Default Near/Distant Normal Strength", "Far Normal Fresnel Power");
refraction is distance-faded ("Refraction Far", "Refraction Bottom Amount");
"Wetness" is a function. Rivers add velocity/flowmap foam; lakes are the same
material with switches.

**Named titles and community practice** (secondary sources; treated as
practice, not specification): Still Wakes the Deep used SLW with a Niagara
shallow-water simulation and Gerstner waves, foam "driven by the wave
Jacobian -- where the surface folds, foam increases", persisting seconds and
dissipating ([80.lv summary of the Epic spotlight](https://80.lv/articles/learn-how-still-wakes-the-deep-used-unreal-engine-5-to-create-water-mechanics);
the unrealengine.com spotlight itself returned 403 to the fetch). Depth-based
shore foam is universally SceneDepth - PixelDepth or DistanceToNearestSurface
([80.lv Nimue breakdown](https://80.lv/articles/how-to-build-stylized-water-shader-design-implementation-for-nimue);
[StraySpark guide](https://www.strayspark.studio/blog/ocean-water-simulation-ue5-guide)),
layered at multiple texture scales with an advance/retreat animation; SSR for
near water with a capture/sky fallback far, planar reflections only for hero
lakes; "one Gerstner layer looks like a swimming pool, use 4-6". A typical
tutorial SLW setup ([worldofleveldesign pool](https://www.worldofleveldesign.com/categories/ue5/single-layer-water-pool-still.php))
runs BaseColor 0, Roughness 0, Opacity 0.5, Refraction 1.05 with PixelNormal
Offset, absorption = colour * intensity / 1000, scattering = colour / 1000.
The "r.Water.SingleLayer.SSR" / ".LumenReflections" names that circulate in
game-mod ini files do NOT exist in the 5.8 source (the list above is
exhaustive for `r.Water.SingleLayer*` in `SingleLayerWaterRendering.cpp`) --
folklore.

**What applies here, and what does not.**

| practice | here |
|---|---|
| Opacity = surface coverage; BaseColor = foam albedo; volume from coefficients | already the design (`create_water_voxel_material.py` W5). The open question is why the lit surface layer reads black on this water -- measured this pass. |
| SSR on water (mode 1) | already shipped 2026-09-06. Lumen: not applicable (no distance fields). Planar: rejected -- a full scene re-render on a render-thread-bound frame for a lake. |
| Depth-keyed shore foam | ADOPTED: new `ShallowFoam*` term on the baked depth field (section 5). DistanceToNearestSurface is dead here (no distance fields); SceneDepth - PixelDepth is available but the baked depth is the same quantity without the marcher's late-depth caveats. |
| Foam as a texture product with contrast/macro scale | ADOPTED procedurally: `FoamBreakup*` (section 5). A real foam texture + normal is a follow-up if the owner likes the shape. |
| Foam moves roughness, opacity, scattering and emissive together | roughness/opacity/emissive already ride `foam`; scattering does not -- follow-up (backlog). |
| Jacobian whitecaps | not applicable: the wind whitecap term (`FoamV2`) is the steepness-times-coverage equivalent for a spectral field, and the lake is calm by design. |
| Distance-flattened normals / far roughness | `WaterRoughnessFarGain` exists (default 0.0, R3 of the realism doc) -- unchanged here, owner ladder pending. |
| Refraction distance fade | not touched. |

### 5. Objective-2 authoring in the lake generator (`create_water_voxel_material.py`), all dark by default

1. **Shore-noise scale 0.35 -> 0.0035** (see Common.ush above). A shipped-image
   change with its own zero arm (`BathyFoamNoiseM:0`).
2. **`FoamBreakupGain` (0.0), `FoamBreakupScaleM` (0.6), `FoamBreakupDriftMPS`
   (0.15), `FoamBreakupContrast` (2.2)**: a 3-level turbulent gradient noise
   on world XY drifting on the one Time node, shaped and lerped from 1.0 by
   the gain, multiplied into shore, shallow and disturbance foam. Gain 0 is a
   multiply by exactly 1.0.
3. **`ShallowFoamDepthM` (0.6), `ShallowFoamGain` (0.0)**: `(1 - ramp(depth_m,
   0, DepthM)) * sign(shore_m) * validity * gain * breakup`, max()ed into
   `shore_foam` so it inherits the emissive route, `ShoreFoamEmissive`, the
   SHORE FX arm and every existing off arm. Gain 0 is bit-identical.

The ocean generator is untouched this pass (its disturbance foam takes the new
helper's baked defaults; no breakup, no shallow term) -- backlog.

### 6. Arms and verdicts

(filled in below as each leg lands; every row names its engagement echo)

Every arm below is a headless leg through `tools\voxel-capture.ps1` at the
boat pose used for `VoxelVerify00974`: `-SpawnAt '-65102,-51084' -SpawnAltM 6
-SpawnPitch -12 -SettleSec 173 -ExtraArgs @('-VoxelExecAfter=165',
'-VoxelExecCmds="voxel.Boat.Spawn 3,voxel.Boat.Enter,voxel.Boat.Throttle 1
12"')` plus the arm's own switch. Sun frozen 12:00 03-20, 2560x1440
(`-ResX/-ResY`), frame 2560x1398. The "regions" are the box means in
`wake_regions.py` (session scratch; the boxes are listed in section 1 above):
`ahead` = (1280,820)-(1790,895), `wake` = (1408,1024)-(1920,1150), `outside`
= (2240,717)-(2496,780) (water beyond the window, the control), `snow` =
(768,512)-(1280,576) (exposure control). Every region number is stated as
measured; no reading of how the frame looks is offered here -- the owner and
the designer judge the frames.

**The one thing to know before reading the table: the boat leg is NOT
pose-reproducible across a material regen.** The five legs on the committed
asset all boarded at exactly (-6509900,-5108400) and shot from
(-6507979,-5108666..668, 165316) at pitch -11.8 / yaw 44.8. The first leg on
the regenerated asset boarded at (-6510301,-5108682), the boat then headed a
different way, and the harness flagged `FRAMING NOT AS REQUESTED`; the next
leg boarded at (-6509984,-5108191). Region numbers are therefore comparable
WITHIN the committed-asset group (A0-A4) and not across it.

| arm | log | frame | asset | switch | engagement echo (from the log) | regions: ahead / wake / outside / snow |
|---|---|---|---|---|---|---|
| baseline (box owner, earlier today) | `Saved/capture-boat-underway-a6.log` | `VoxelVerify00974.png` | committed `fe51c15` (211037 B, 14:41:41) | none | `BOARDED at (-6509900,-5108400,165038)`; `field verified LIVE -- centre patch max field value 0.1919, max state height 0.0436 m, after 6 injection(s)` | 182,186,187 / 151,158,162 / 170,195,206 / 215,215,214 |
| **A0** foam off | `Saved/capture-wl-a0-foamoff.log` | `VoxelVerify00980.png` (shutter cam (-6507979,-5108668,165316) pitch -11.8 yaw 44.8) | committed | `-VoxelWaterMatScalar=DisturbanceFoamEnabled:0` | `Lake sheets: material scalar 'DisturbanceFoamEnabled' set to 0.0000`; BOARDED same; LIVE `0.1910, 0.0438 m` | 171,199,206 / 83,162,185 / 170,195,205 / 215,215,214 |
| **A2** gain 1 | `Saved/capture-wl-a2-gain1.log` | `VoxelVerify00982.png` (cam (-6507978,-5108665,165316)) | committed | `-VoxelWaterMatScalar=DisturbanceFoamGain:1` | `material scalar 'DisturbanceFoamGain' set to 1.0000`; LIVE `0.1918, 0.0436 m` | 182,186,187 / 151,158,162 / 172,196,206 / 215,215,214 |
| **A3** gain 0.25 | `Saved/capture-wl-a3-gain025.log` | `VoxelVerify00984.png` (cam (-6507979,-5108667,165316)) | committed | `-VoxelWaterMatScalar=DisturbanceFoamGain:0.25` | `material scalar 'DisturbanceFoamGain' set to 0.2500`; LIVE `0.1913, 0.0437 m` | 182,187,188 / 151,158,162 / 170,195,206 / 215,215,214 |
| **A4** field readback attempt | `Saved/capture-wl-a4-fieldread.log` | `VoxelVerify00986.png` (cam (-6507979,-5108666,165316)) | committed | `-VoxelRippleWakeAfter=172 -VoxelRippleWakeStrengthM=0.001 -VoxelRippleWakeRadiusM=0.1 -VoxelRippleWakeSteps=1` | `CAPTURE WAKE FIRED ... injected=1649 dropped(outside=0 full=0 unarmed=0 inert=0) steps=10084 ... fieldMaxAbs=0.0959 stateMaxAbs=0.0218. FROZEN.`; ring depths at 4/6/8/10/12 m ahead: `1.85 / 1.00 / 0.84 / 0.23 / 0.00 m`, shore `5.60 / 3.80 / 3.80 / 1.90 / -1.90 m` | not measured (the injection freezes the sim 1 s before the shutter) |
| **B0** new asset, defaults | `Saved/capture-wl-b0-default.log` | `VoxelVerify00988.png` (cam (-6511125,-5110629,165324) pitch -13.6 yaw 44.7 -- `FRAMING NOT AS REQUESTED`) | regenerated 17:46:42 (229742 B) | none | `BOARDED at (-6510301,-5108682,165038)`; LIVE `0.0950, 0.0220 m, after 3 injection(s)` | 147,155,157 / 152,160,164 / 107,127,137 / 166,173,177 -- pose moved; the `snow` box is no longer snow, so these are NOT comparable with A0-A4 |
| **C1** new asset, boat wake gain 1 | `Saved/capture-wl-c1-wakegain1.log` | `VoxelVerify00990.png` (cam (-6508521,-5106788,165322) pitch -12.1 yaw 45.2) | regenerated | `-Cvars 'voxel.Boat.WakeGain 1'` | `voxel.Boat.WakeGain = "1"`; `BOARDED at (-6509984,-5108191,165038)`; LIVE `0.0625, 0.0148 m, after 6 injection(s)`; the shutter lies 15 m E / 14 m N of the boarding point, i.e. along yaw 45 | not measured (pose differs from every other leg) |

Arms authored but NOT run (owner paused live tuning at this point):
**A1** `-VoxelWaterMatScalar=DisturbanceFoamEmissive:0` on the committed asset
(the "is the SLW surface layer lit" test at full coverage); the threshold /
height-weight ladder on the new asset; the ocean regen; and any leg with the
C++ change in section 7(c).

### 6.1 What the arms establish, stated as measurements

1. **A0 vs baseline**: with the one scalar `DisturbanceFoamEnabled` at 0 the
   `wake` box moves from (151,158,162) to (83,162,185) and the `ahead` box
   from (182,186,187) to (171,199,206), while `outside` and `snow` are
   unchanged to +/-1. So (i) `-VoxelWaterMatScalar` reaches the surface the
   wake is drawn on -- the lake at the boat is the SHEET, `voxel.Water.
   MeshImplicitLakes` default 0 -- and (ii) the entire grey window is the
   disturbance-foam channel and nothing else. This arm could have failed
   either way (an unchanged window would have meant reach failure or a
   different mechanism).
2. **A2 and A3 vs baseline**: `DisturbanceFoamGain` 8 -> 1 -> 0.25 leaves
   every region unchanged to +/-1. `foam = saturate(raw * gain)` is
   therefore saturated at gain 0.25, i.e. **raw >= 4 across the whole
   window at the shutter**, `ahead` included. The "field verified LIVE"
   figure (0.19 / 0.044 m) cannot be the field the shutter saw: health
   sampling runs until the first verification, 0.25 s after boarding, and
   stops. **There is no instrument that reads the field at the shutter**;
   A4 shows the injector's `fieldMaxAbs` is the same stale number
   (`LastFieldMaxAbs_`).
3. **Arithmetic behind raw >= 4.** The material multiplies the field by
   `RippleFieldGain`, which `voxel.Water.Ripple.Gain` publishes at **2.5**
   (`VoxelRippleField.cpp:114`; its help text says "1 is shipped strength";
   set to 2.5 in `c031b03`, 2026-08-13). With the committed response
   `(|grad| + 4|h|) * 2.5`, raw >= 4 needs |h| >= 0.4 m or |grad| >= 1.6 on
   every texel. The World Position Offset is the same `2.5 * h`, so a window
   at |h| ~0.4-0.6 m is drawn 1.0-1.5 m proud of the lake.
4. **Where that amplitude comes from (source, not a frame).** The boat calls
   `AddSweptDisturbance` once per tick per station with that tick's sweep.
   `VoxelRippleField.cpp:895` sets `NumSplats = 1 + floor(Length / Spacing)`
   with spacing = max(width/2, 0.3 m) = 0.30 m, so a 3.3 cm per-tick sweep at
   2 m/s still injects one FULL `StrengthM` ring per station per tick --
   ~9 rings per spacing of travel instead of one, and `injected=1649` by
   t=172 s in A4 (7 s under way). `voxel.Boat.WakeGain` **3.0**
   (`VoxelBoat.cpp:68`, owner directive 2026-09-06, chosen against the 1.5 cm
   LIVE readback which is taken at boarding, before the throttle) multiplies
   each ring. The authored strengths are `BowWakeStrengthM 0.030`,
   `TransomWakeStrengthM 0.022` (`VoxelMovementTuning.h:388-391`).
5. **B0 vs C1** (different poses; read the frames, not the boxes): B0 is the
   new asset at `WakeGain` 3.0, C1 the same asset at `WakeGain` 1. In B0 the
   near-surface silhouette against the far water reaches display y~460 of
   1092 with the frame centre at 546, i.e. ~9 deg above the camera's axis
   (pitch -13.6), and the hull is covered up to the bow tip; in C1 the near
   surface meets the far water at the window edge (display y~600-650) with
   no silhouette above the far water line. The `voxel.Boat.WakeGain` echo is
   the only difference in the two command lines.

### 7. Classification of every edit in the tree, for the commit decision

**(a) Proven by an arm that could have failed -- the DIAGNOSIS, not a fix:**
the grey window is the disturbance-foam channel (A0); the field the material
samples is >= 4 raw everywhere at the shutter (A2/A3), so the committed
response's saturation is the visible symptom and the amplitude is the cause;
the amplitude tracks `voxel.Boat.WakeGain` (B0 vs C1, two poses). **No edit
below has been proven to fix the look.** B0 -- the regenerated asset at its
new defaults -- still carries a window-wide foam at `WakeGain` 3.

**(b) Generator edits now on disk, and their arms:**

| file | change | default state | zero arm |
|---|---|---|---|
| `ue-project/Tools/ripple_field_graph.py` | `build_disturbance_foam`: `saturate((x - Threshold) * Gain * Enabled)`, `x = |grad| + |h| * HeightWeight`; three new ScalarParameters | **LIVE change of the shipped response**: HeightWeight 4.0 -> 1.0, Threshold 0 -> 0.05, Gain 8 -> 6 | `DisturbanceFoamEnabled:0` (pixel-identical off, unchanged); `Threshold:0,HeightWeight:4,Gain:8` reproduces the committed response exactly |
| `ue-project/Tools/create_water_voxel_material.py` | shore-noise `scale` 0.35 -> 0.0035 (Common.ush: Position is in cm) | **LIVE change of the shipped shore band** (+/-0.9 m perturbation at ~3 m features instead of ~3 cm) | `BathyFoamNoiseM:0` |
| same | `FoamBreakupGain/ScaleM/DriftMPS/Contrast` -- drifting turbulent noise multiplied into shore, shallow and disturbance foam | **dark** (gain 0.0 = multiply by exactly 1.0) | it is the default |
| same | `ShallowFoamDepthM/ShallowFoamGain` -- depth-keyed shore foam max()ed into `shore_foam` | **dark** (gain 0.0 = bit-identical) | it is the default |

Both `.py` files are unmodified in HEAD as of `fa86d97`; `git diff --stat`
below. The three LIVE changes are the ones a commit has to decide on; the
two dark ones ride along at no image cost.

**(c) Experimental -- decide before any commit or any leg:**

* `ue-project/Source/VoxelEarth/VoxelRippleField.cpp` (+30/-1 at
  `AddSweptDisturbance`): a sweep shorter than one spacing now deposits
  `StrengthM * Length / Spacing` instead of a full ring. **AUTHORED, NOT
  BUILT, NOT TESTED** -- the owner paused live tuning before the build. It is
  the mechanism fix for item 6.1(4) and is four lines of arithmetic, but an
  unbuilt C++ edit is live input to the next `Build.bat`. Either build it
  (`tools\voxel-build.ps1`) and re-run C1's command line without the `-Cvars`
  (the observable is the same near-surface silhouette test as B0 vs C1), or
  `git checkout -- ue-project/Source/VoxelEarth/VoxelRippleField.cpp` before
  anyone runs a leg. The stationary-hull consequence (no deposit at zero
  sweep, so `DroppedInert` counts those calls) is in the comment.
* **`ue-project/Content/Voxel/M_WaterVoxel.uasset` is a regenerated shipping
  asset**: mtime 17:46:42, 229742 B, against `fe51c15`'s 211037 B; `git
  status` shows it modified. It carries every (b) change including the three
  LIVE ones. `M_Ocean.uasset` was NOT regenerated (178139 B, 14:42:15,
  committed bytes) although `create_ocean_material.py` imports the changed
  helper, so the two water materials are now inconsistent on disk until
  either the ocean is regenerated (`tools\voxel-sky-chain-regen.ps1 -Only
  create_ocean_material.py -SkipCapture`) or the lake asset is restored
  (`git checkout fe51c15 -- ue-project/Content/Voxel/M_WaterVoxel.uasset`).
  Regen log for the lake: `Saved/sky-chain/regen-create_water_voxel_material.
  log` -- `Python script executed successfully` x1, `LogPython: Error` x0,
  `NOT A SHIPPING MATERIAL` x0, `Failed to compile Material` x0, 23 s; the
  asset's bytes contain `DisturbanceFoamThreshold`,
  `DisturbanceFoamHeightWeight`, `ShallowFoamGain`, `FoamBreakupGain` and no
  `RIPPLE_DEBUG` / `NOT A SHIPPING` marker. `Get-ChildItem env:VOXEL_*` was
  empty.

### 8. How to iterate live (for the designer)

**Live in a running session (console, no relaunch):**

| cvar | default | what it does |
|---|---|---|
| `voxel.Boat.WakeGain` | 3.0 | multiplier on every bow/transom/slam splat the boat injects (the amplitude lever of section 6.1) |
| `voxel.Water.Ripple.Gain` | 2.5 | multiplier on the ripple field AS DRAWN (normal, WPO and foam); does not touch the sim |
| `voxel.Water.WaveTimeScale` | 1 | wind-wave clock; also gates the ripple's amplitude in the material (0 = provably still surface AND no disturbance foam) |
| `voxel.Water.Ripple.Drop <XUU> <YUU> [RadiusM] [StrengthM] [Steps]`, `voxel.Water.Ripple.DropHere ...` | -- | inject one ring |
| `voxel.Water.Ripple.Freeze` | 0 | freeze the sim to study a frame |
| `voxel.Water.Ripple.PlayerStrengthM / PlayerRadiusM / ObjectStrengthM` | 0.22 / 0.9 / 0.18 | the player's and objects' splash sizes |
| `voxel.Water.Ripple.TestFill` | 0 | DIAGNOSTIC: overwrite the field with a constant (proves the material samples this RT; everything else in the run is void) |
| `voxel.Water.MeshImplicitLakes` | 0 | 1 = the old two-path lake draw (near-field voxels + sheet) |
| `voxel.Boat.WaveBobGain`, `voxel.Boat.WaveBob` | 6.0 / 1 | hull bobbing on the wind waves (unrelated to the ripple field) |

**Material scalars on `M_WaterVoxel`** (all ScalarParameters; live in the
editor through a Material Instance of `/Game/Voxel/M_WaterVoxel`, or at
launch through the switch below; the generator default is the source of
truth and must be edited + regenerated for anything kept):
`DisturbanceFoamGain` 6, `DisturbanceFoamThreshold` 0.05,
`DisturbanceFoamHeightWeight` 1, `DisturbanceFoamEnabled` 1,
`DisturbanceFoamEmissive` 0.6, `ShoreFoamEmissive` 0.6, `BathyFoamWidthM` 6,
`BathyFoamNoiseM` 0.9, `BathyFoamShelfLo` 0.05, `BathyFoamShelfHi` 0.25,
`BathyFoamGain` 0.55, `ShallowFoamDepthM` 0.6, `ShallowFoamGain` 0,
`FoamBreakupGain` 0, `FoamBreakupScaleM` 0.6, `FoamBreakupDriftMPS` 0.15,
`FoamBreakupContrast` 2.2, `WaterRoughnessFarGain` 0 (`WaterRoughnessFar`
0.30, `WaterRoughnessFadeStartM/EndM`), `SurfacePresence` 0.
`M_Ocean` shares the four `DisturbanceFoam*` scalars only.

**Launch-only switches (headless legs; each echoes, and a missing echo is a
void arm, not a null):**
`-VoxelWaterMatScalar=Name:Value[,Name:Value...]` -- sets scalars on the
**lake SHEET's MID only** (`VoxelWaterSheetActor.cpp:350`; the ocean and the
near-field voxel water have no such path); echo `Lake sheets: material scalar
'<Name>' set to <v>`, one line per pair (multi-pair works on the current
build; any multi-pair arm run before 2026-09-07 06:00 applied only its first
pair). `-Cvars 'voxel.Boat.WakeGain 1'` on `voxel-capture.ps1` (`-ExecCmds`
underneath; echo `voxel.Boat.WakeGain = "1"`). `-VoxelRippleWakeAfter=<s>
-VoxelRippleWakeSteps/RadiusM/StrengthM/Freeze` -- the capture injector; its
`fieldMaxAbs` in the FIRED line is stale. `-VoxelLakeSheets=0` removes the
sheet. The boat leg itself: `-VoxelExecAfter=165 -VoxelExecCmds="voxel.Boat.
Spawn 3,voxel.Boat.Enter,voxel.Boat.Throttle 1 12"` with `-SettleSec 173`.

**Regen (23 s for the lake, measured; pass = `Python script executed
successfully` x1, `LogPython: Error` x0, `NOT A SHIPPING MATERIAL` x0,
`Failed to compile Material` x0, and `Get-ChildItem env:VOXEL_*` empty
first):**
`tools\voxel-sky-chain-regen.ps1 -Only create_water_voxel_material.py -SkipCapture`
(log `Saved\sky-chain\regen-create_water_voxel_material.log`);
`tools\voxel-sky-chain-regen.ps1 -Only create_ocean_material.py -SkipCapture`.
`ripple_field_graph.py` has no asset of its own: regenerate BOTH consumers
after editing it.

**The two questions a live session can answer in minutes that this pass
could not:** (1) with `voxel.Water.Ripple.Freeze 1` after a run, what the
field actually holds (there is no readback; `voxel.Water.Ripple.TestFill`
proves the binding, the frozen frame shows the content); (2) A1 -- at full
coverage with `DisturbanceFoamEmissive 0`, whether the SLW surface layer is
lit on this water (`BasePassPixelShader.usf:1480-1492` says it is
forward-lit; the record says it renders black).

### 9. Settled elsewhere this session, recorded here so nobody re-opens it

The white terrain at this lake is genuine alpine ground, not a colour-authority
fault: the 00974 log carries `Clipmap colour authority: PALETTE (one authority,
default)`, tan/orange voxels render along the waterline in 00962/00974/00980,
and the ground top is 1644.2 m with higher peaks in frame (box owner, from the
log). The biome name at the column is not yet on record (`vxc_terrainprobe`).

## 2026-09-07 night, coordinator: water look PAUSED by owner; the night's authoring preserved as a patch, tree restored to fe51c15

Owner, on the six ladder frames (00980-00990): "All of the capture screenshots look bad
for different reasons. Boat is moving around. There is water inside boat in second.
Massive grey coloration is 1, 3rd, 4th, 5th, and 6th. Wakes and ripples still look
very bad." And earlier: pause wake/ripple tuning until a human designer iterates live;
agent image interpretation is not good enough. Both stand.

Those six frames were the DIAGNOSTIC ladder that found the mechanism (section 6 above),
not candidates for the look; the coordinator should not have presented them as such.

**State of the tree after this note:** the four water files the night pass edited
(`ripple_field_graph.py`, `create_water_voxel_material.py`, `VoxelRippleField.cpp`,
`M_WaterVoxel.uasset`) are restored to `fe51c15`, so the shipping asset pair is
consistent again and no unbuilt C++ can be swept into a build. Everything authored is
in **`docs/patches/water-look-2026-09-07-night.patch`** (`git apply` it, then regen
BOTH water generators, then build for the C++ deposit fix). Its contents, per the
agent's own classification: (a) the foam response re-keyed on a dead band + linear
ramp with three new scalars -- LIVE default change, UNPROVEN (B0 still showed
window-wide foam); (b) shore-noise scale 0.35 -> 0.0035 (Position is in cm) -- live,
unproven; (c) `FoamBreakup*` and `ShallowFoam*` -- dark by default; (d) the C++
rate-independent wake deposit -- UNBUILT, UNTESTED, and the only change aimed at the
actual mechanism. Its gate is a counter, not an image: `injected` per metre of boat
travel must fall ~9x from the ~1649-in-7-s the B0 log shows.

**Recommended first step for the live session:** apply the patch, build, and read the
`injected=` count on one boat run BEFORE looking at any picture. Until the deposition
rate is fixed the surface is a half-metre dome and no material setting can make it
read as a wake.

2026-09-07 | UI | OWNER VERDICT on the 2 px promotions, HUD pair + crops (bar ticks, vitals bars) | **ACCEPTED** -- "All of the after screenshots for HUD look better than the before." HUD bar ticks and HUD vitals bars ship as promoted; the Settings checkbox crop was not judged and stays open | commit 423e25c, docs/ui-mocks/2026-09-07/captures/after/

2026-09-07 | UI | OWNER DIRECTIVE, live at 1440p: main-menu corner insets too wide | **AUTHORED VALUES TIGHTENED** (LogoTop 80->40, LogoRight 100->48, TitleMenuRight 160->64, CalloutTop 80->40, CalloutLeft 60->32, VersionInsetLeft 36->20, VersionInsetBottom 24->14) -- per ADR-0011 the scale is untouched; owner judges on the next launch | VoxelUITheme.h, pending build
2026-09-07 | UI | OWNER BUG REPORT, live: NEW GAME always highlighted + expanded | **FIXED IN SOURCE** -- the 2026-09-07 UI pass bound NEW GAME's Active to HasNoColumnFocus (a resting selection from the mock's `.active`); removed, so it lights/grows only on hover or keyboard focus like its siblings | SVoxelMainMenu.cpp, pending build
2026-09-07 | UI | OWNER VERDICT + DIRECTIVE, live: corner insets "spacing looks good" (ACCEPTED); title 15% smaller, patch-notes callout 25% smaller and compact | **AUTHORED** LogoFontSize 132->112; callout 520/14/16/64/22 + faces 16/22/16 -> 390/10/12/48/16 + 12/16/12 | VoxelUITheme.h, pending build
2026-09-07 | audio | OWNER: "no music playing on home or load screen" -> "confirmed music is present" once the game window had focus | **NOT A BUG** -- the engine mutes all audio when the game window is unfocused (BaseEngine.ini [Audio] UnfocusedVolumeMultiplier=0.0, not overridden); the log showed the track playing throughout | live session
2026-09-07 | audio/HUD | OWNER DIRECTIVE: in-game HUD music controls, top-right: back / play-pause / next, design-system styling; music from the soundtrack library plays BY DEFAULT in game; pause stops, next skips, back goes to the previous track | **ASSIGNED** (Opus agent; today the front end fades music OUT at hand-off, so this also changes that) | pending
2026-09-08 | water, live | OWNER VERDICTS on the deposit-fix build (patch applied, both materials regenerated, curtain thread off): "no more water in the hull" **FIXED**; "i think the half metre dome is gone" **FIXED**; "i dont see any shore foam in the game" **OPEN**; "the disturbed areas from both the boat and from player in the water still paints as grey. i would expect this wake and water surface ripples to paint as effects on the water in a correct color - not this flat grey color" **OPEN**; "loading screen looks good" **ACCEPTED**; "title screen looks good as well" **ACCEPTED** | Ripple.Stat: injected=13932 over the run (~2 per tick = bow+transom, count unchanged by design; the deposit fix scales STRENGTH per deposit, so my injected-per-metre gate was ill-defined -- the field peak from voxel.Water.Ripple.Dump is the number) | live session
2026-09-08 | water, live | NEXT ARM for the two open verdicts (flat grey disturbance foam; no shore foam): relaunch with `-VoxelWaterMatScalar=FoamBreakupGain:1,ShallowFoamGain:1,ShallowFoamDepthM:0.6,DisturbanceFoamEmissive:1.0` -- switches on the night pass's dark-by-default procedural breakup (textured whitewater instead of a flat tint lerp of (0.82,0.90,0.94)) and the depth-keyed shallow shore foam, and brightens the disturbance emissive | owner judges live; no regen, no build needed for these four (all sheet MID scalars) | pending the window close
2026-09-08 | audio/HUD | HUD music controls AUTHORED (Opus agent): FVoxelUIMusic playlist (session shuffle, Next/Previous/TogglePause/NowPlaying, end-of-track advance marshalled to the game thread, PcmGuard), music continues into gameplay (VoxelAudioUserSettings MusicInGame default true; unattended runs stay silent), HUD cluster top-right [prev][play-pause][next] as Slate geometry + now-playing label; hotkeys , . / and NumPad 4/5/6; clicks whenever an in-game screen has the cursor up (HUD z 18->111 above the screen stack, below pause); hidden over death/dialogue | **BUILDING**, owner judges live; two unverified: triangle glyph rendering, long-name clipping | VoxelUIMusic.*, SVoxelGameHud.*, VoxelScreensUISubsystem.*, VoxelAudioUserSettings.*, VoxelFrontEndSubsystem.cpp, VoxelUITheme.h, VoxelUIStrings.*
2026-09-08 | UI | OWNER, live: title + callout "are smaller" (size ACCEPTED); "patch notes can be moved up and to the left more"; "the voxelmark title needs to move further to the left because the end of the K letter ... is currently clipping off screen" | **AUTHORED** LogoRight 48->96 (swash-K overhang ~47 px at 112 px), CalloutTop 40->24, CalloutLeft 32->16 | VoxelUITheme.h, pending build
2026-09-08 | UI | OWNER VERDICT: "confirmed new game grows and lights up as expected now" | **ACCEPTED** (resting-active binding removed, commit b9cdb75) | live session
2026-09-08 | audio | Playlist observed live on the title screen: built 30 tracks shuffled; end-of-track advance fired 1->2->3->4 ("Defeat _ Game Over", "Boss Battle" among them) | **WORKS**; follow-up: curate menu vs combat vs ambient sets (stingers like Defeat/Boss Battle should not open the title screen) -- playlist tagging by filename prefix or a manifest | backlog
2026-09-08 | UI/HUD | OWNER DIRECTIVE, live: "move the song/title name to the left of the back, pause, and next buttons" | **ASSIGNED** (music agent; one right-aligned row [label][prev][play-pause][next]) | pending build
2026-09-08 | controls | OWNER DIRECTIVE, live: "holding tab should cause the mouse cursor to pop up on screen and be able to be controlled. when holding tab and moving the mouse, player can use the cursor to click anything on screen." | **ASSIGNED** (music agent: hold-to-point mode, game-and-UI input while held, look suppressed, release on focus loss; transport becomes clickable via the existing bCursorVisible gate) | pending build
2026-09-08 | loading | OWNER, live: "lots of hitches and lags on the loading screen ... the hourglass freezes and sand stops falling" | **OPEN**; log profile of that load: renderWaitMs 6452/2002/1276/924 ms (render-thread waits, the 6.4 s inside the first GPU worklist burst ten seconds after the 952 MiB brick-pool commit), plus one 550 ms recompute and one 2016 ms raster-atlas fill window on the game thread. Threaded curtain is OFF (deadlock). Assigned: pre-warm GPU pools under the menu + cap atlas fills during the theatre (Opus agent) | pending
2026-09-08 | audio/HUD | OWNER VERDICT: "confirmed pause, back, and next for music is working as expected" | **ACCEPTED** (transport + hotkeys; label placement change pending) | live session
2026-09-08 | settings | OWNER DIRECTIVE: Settings rows "Hide Music UI" (hides all music HUD items) and "Hide Compass UI" (hides just the compass at the top of the HUD) | **ASSIGNED** (music agent; persisted like the other rows per the settings-panel policy) | pending build
2026-09-08 | UI | OWNER DIRECTIVE: the unified inventory/journal/map/player/codex shell must be resizable at runtime (whole thing larger or smaller); and the inventory shell renders larger than the other four (measured 1348x944 vs 1326x906 on the after captures) -- unify | **ASSIGNED** (UI polish agent: shell scale multiplier, live Ctrl+wheel / keys, persisted; all five on one fixed shell size; the Settings row for it follows once the music agent's rows land) | pending build
2026-09-08 | HUD/controls/settings | AUTHORED (music agent): now-playing label moved left of the plates (HudMusicLabelGap 4 now horizontal); hold-Tab point mode (Tab was unbound; SetIgnoreLookInput + FInputModeGameAndUI with SetHideCursorDuringCapture(false); ends on key-up, focus loss, or any overlay claiming input; logs `VoxelInput: point mode ON/OFF`); Settings rows Hide Music UI / Hide Compass UI ([VoxelGraphics] HideMusicUI/HideCompassUI, default false, live via bound visibility, hidden wins over hold-Tab, hotkeys untouched) | **PENDING BUILD**; unverified: triangle glyph rendering, long-name clipping, label gap 4 vs plate gap 6 | SVoxelGameHud.cpp, VoxelScreensUISubsystem.*, VoxelGraphicsUserSettings.*, SVoxelSettingsPanel.cpp, VoxelUIStrings.*, VoxelUITheme.h
2026-09-08 | UI/map | OWNER DIRECTIVE: map screen -- live player marker; zoom in/out; right-click-drag pans; right-click dropdown to place, name and SAVE a marker for future reference; default view centred on the player at 10 km x 10 km, not the whole world | **ASSIGNED** (new Opus agent; SVoxelMapScreen.*, map data feed, a persisted marker store) | pending
2026-09-08 | controls | OWNER: "spacebar ... appears to be pausing, starting or skipping music" -> "i was wrong about spacebar" | **BUG FOUND + FIXED IN SOURCE**: a clicked transport SButton kept Slate focus and SButton::OnKeyDown fires on SpaceBar (the pawn's ascend key), log burst `PREV ->` 27->22 at 04:09:47-04:10:06; fix: `.IsFocusable(false)` on the three plates, focus term dropped from IsMusicButtonLit, `SetAllUserFocusToGameViewport()` at EndPointMode. Space stays ascend; music keys stay , . / | pending build
2026-09-08 | audio | OWNER VERDICT: "music is confirmed playing in world" | **ACCEPTED** (music continues through hand-off; MusicInGame default true) | live session
2026-09-08 | UI | AUTHORED (shell agent): inventory discrepancy CAUSED by ShrinkWrap (only the inventory had no size override; faithful port of its mock's width:max-content) -> shell now unconditionally 1060x760 with the body in an SScaleBox ScaleToFit DownOnly (cannot enlarge the other four); runtime resize as a render transform about the frame centre, [VoxelGraphics] ScreenShellScale 0.75-1.50 @0.05 default 1.00, Ctrl+wheel / Ctrl+plus/minus / Ctrl+0, MENU SIZE slider row under INTERFACE; two more 1-unit rings promoted (kBodyRing1/2Px, kLeatherBorderPx) | **PENDING BUILD**; caveat: Ctrl+wheel over an inner scroll list is eaten by the list, keys always work | SVoxelScreenShell.*, VoxelScreenShellSettings.* (new), SVoxelSettingsPanel.*, VoxelUIStrings.*, SVoxelMenuButton.cpp
2026-09-08 | water, live | OWNER VERDICT on the foam arm (FoamBreakupGain 1, ShallowFoamGain 1 @0.6 m, DisturbanceFoamEmissive 1.0): "i can see the shore foam effects and it looks pretty good" | **ACCEPTED** (first shore foam ever accepted). Owner's new diagnosis: "our water in general at the SLW level is too transparent (especially when it is only 1-4 voxels deep) such that its hard to even tell there is water ... i think i am in the carribean"; the foam reads light grey only because the surface under it is clear. Owner asked for PROPOSALS. Screenshot: C:/Users/Matt Noles/Pictures/Screenshots/Screenshot 2026-09-08 001606.png | live session
2026-09-08 | loading | CORRECTION (load agent, from the engine's own timer semantics): `renderWaitMs` on the Hitch line is GRenderThreadWaitTime = the render thread's IDLE time, and on all five big hitches it equals the whole frame -- the render thread had nothing to do because the GAME THREAD was busy. My earlier 'the big waits are render-thread' reading was wrong. Frame 851 (6.47 s): two synchronous fine-tile loads (~1.0 s, `read 190/288 ms` + decode 270/274) plus ~5.1 s UNLOGGED; leading suspect the fine-tile RW lock: `req calls=23870 lockFree=0 shared=0 excl=23870` in 10.7 s with the wait meter off. Settles with `-VoxelFineLockMeter=2` on one leg | **the next relaunch carries -VoxelFineTileAsync=1 -VoxelFineTileRingRadius=1 -VoxelFineLockMeter=2** | authored: BrickPool arenas timed + pre-warmed under the menu (quad pool cannot be, ChunkOwner is null while held -- instrumented instead); FVoxelQuadVertexFactory PSO precache added (never precached before); voxel.Stream.AtlasFillMs 2.0->0.5 for the theatre (the sweep deadline, not the 256 demand cap, which was 9 ms of 2016); the R5/R7 entry scan has NO bound and none was invented
2026-09-08 | water, live | OWNER VERDICT, boat under way on the foam arm: "good progress ... no longer terrible grey looking sheet. i can clearly see wake waves forming around the boat. the surface foam is also present. however, both of these look bad and need significant tuning": (1) wake waves "should be much smaller and finer. fine ripples in high quantity"; (2) surface foam "way too prevalent and spreads out in a circle everywhere from the boat", should be "only near the wake and pretty small/not very noticable" | **OPEN, tuning**; live cvars for (1): voxel.Boat.WakeGain 3.0, voxel.Water.Ripple.Gain 2.5, voxel.Water.Ripple.HalfLifeSec 5.0; launch scalars for (2): DisturbanceFoamThreshold 0.05, DisturbanceFoamGain 6, DisturbanceFoamHeightWeight 1 | screenshot 2026-09-08 (boat, blotchy foam ring, large smooth rings to the horizon)
2026-09-08 | water | OWNER DIRECTIVE on the transparency proposals: "add the turbidity floor asap ... i accept darkening the voxel bed colors under any water. no more surface reflectance for now. execute 4 as well. so do 1, 2, and 4" -> (1) turbidity/scattering floor in the shallows with a minimum body colour keyed on bathy depth; (2) submerged-bed darkening on the terrain material keyed on water depth; (4) per-channel extinction, red first. NOT (3) reflectance | **ASSIGNED** (Fable water agent) | pending regen + build

## 2026-09-08 (water-look agent): turbidity floor, submerged-bed darkening, per-channel extinction, and the smaller wake -- owner-directed, authored and regenerated

Owner, live session 2026-09-07/08, verbatim: *"our water in general at the SLW
level is too transparent (especially when it is only 1-4 voxels deep) such
that its hard to even tell there is water ... i think i am in the carribean"*;
*"add the turbidity floor asap ... i accept darkening the voxel bed colors
under any water. no more surface reflectance for now. execute 4 as well. so do
1, 2, and 4."* And on the wake: *"wake waves should be much smaller and finer.
fine ripples in high quantity"*; *"surface foam ... way too prevalent and
spreads out in a circle everywhere from the boat ... should only be near the
wake and pretty small"*. Also accepted tonight: the first shore foam ever
(ShallowFoam gain 1 / depth 0.6 + FoamBreakup gain 1 + DisturbanceFoamEmissive
1.0, *"i can see the shore foam effects and it looks pretty good"*), and the
swept-splat deposit fix (built and in: *"no more water in the hull"*, *"the
half metre dome is gone"*).

The mechanism behind "Caribbean", in numbers: a white bed (albedo ~0.9) under
the shipped optics at 20 cm depth keeps 80% of its RED and 91% of its GREEN
(absorption 1.118 / 0.458 / 0.291 per metre), and the SLW volume term
integrates ~nothing over 20 cm of a 0.067/m scatterer. The pixel is the bed,
faintly tinted, and blue survives best, so the tint is sky-blue. Three levers,
all authored below; NOT touched: reflectance (owner: "no more surface
reflectance for now").

### (1) The turbidity floor -- both water generators

    turb    = ShallowTurbidityFloor * (1 - ramp(depth_m, 0, ShallowTurbidityDepthM)) * validity
    scatter = ScatteringPerMetre * (1 + ShallowScatterBoost * turb)          -> SLW ScatteringCoefficients
    body    = (ShallowBodyR, G, B) * turb * ShallowBodyEmissive * (1 - foam)  -> added to Emissive (lake: x top-face mask)

The scattering half is the physics (silt and plankton: shallow lake water is
a strong scatterer); at the defaults a 20 cm column in-scatters 0.20 of the
green light instead of 0.013. The body half is the "minimum colour even at
zero depth" and rides EMISSIVE because that is the channel proven to reach
the pixel on this water (the foam rides it by the same argument). `(1 - foam)`
is what keeps foam ON TOP: where foam is 1 the body tint is gone and the foam
white is what shows. In the ocean `depth_eff_m` is 60 m wherever the bathy is
invalid, so open sea is untouched by construction.

| scalar | default | 0 / off | where |
|---|---|---|---|
| `ShallowTurbidityFloor` | 0.7 | 0 = pre-2026-09-08 optics bit for bit | lake + ocean |
| `ShallowTurbidityDepthM` | 1.5 m | -- | lake + ocean |
| `ShallowScatterBoost` | 15 | 0 = no scatter boost (body floor still on) | lake + ocean |
| `ShallowBodyEmissive` | 0.30 | 0 = previous emissive bit for bit | lake + ocean |
| `ShallowBodyR` / `G` / `B` | 0.05 / 0.22 / 0.20 | -- | lake + ocean |

### (2) Submerged-bed darkening -- terrain materials AND the marcher

`terrain_material_common.build_terrain_base_color` (shared by
`M_VoxelTerrain` and `M_VoxelClipmap`), right after the wet-shore band and
OUTSIDE the `VOXEL_SHORE_FX` arm:

    submerged = saturate(depth_m / SubmergedRampM) * validity     dry -> dark within 0.30 m
    base     *= lerp(1, SubmergedDarken, submerged)               then the floor

| scalar | default | off |
|---|---|---|
| `SubmergedDarken` | 0.45 | 1.0 = previous terrain bit for bit |
| `SubmergedRampM` | 0.30 m | -- |

**The near-field bed is drawn by the ray marcher, not by those materials**
(`voxel.Terrain.RetireQuads` 1; clipmap inner hole 8 km), so the same factor
is applied in `ue-project/Shaders/VoxelMarch.usf` at the caustic block's bathy
sample, to BOTH `BaseColor` (goes to GBufferC, lit by the sun in the deferred
pass) and `Emissive` (the marcher's ambient term), BEFORE the caustic light is
added -- constants `VOXEL_MARCH_SUBMERGED_DARKEN 0.45f` /
`VOXEL_MARCH_SUBMERGED_RAMP_M 0.30f`. **This is a shader edit, compiled at the
next launch; it needs no `Build.bat`.** Two limits of the no-C++ version,
both deliberate: (i) the marcher's numbers are `#define`s, not a live dial --
binding two floats is a C++ change in `VoxelMarchRenderer`'s caustic
bindings (backlog); (ii) the term rides the caustic bathy sample and is gated
with it (`VOXEL_MARCH_CAUSTICS` permutation, `MarchCausticField.w > 0`, i.e.
`voxel.Water.Caustics` > 0, and inside the caustic fade distance) -- caustics
OFF turns the marcher's bed darkening off with it. ONE colour authority is
respected: nothing here is a colour source; it is one multiplicative factor
of the baked depth, with the same two numbers on every path. Change both or
neither.

### (4) Per-channel extinction -- both water generators + M_Underwater

`water_optics.ABSORPTION_CHANNEL_SCALE = (2.0, 1.0, 1.5)`, applied inside
`absorption_per_m()` (so `M_Underwater` stays on the same water) and baked as
three scalars multiplied into the per-cm absorption in both water materials:

| channel | absorption/m before | scale | after | 20 cm transmittance before -> after |
|---|---|---|---|---|
| R | 1.118 | `WaterAbsorbScaleR` 2.0 | 2.236 | 0.80 -> 0.64 |
| G | 0.458 | `WaterAbsorbScaleG` 1.0 | 0.458 | 0.91 -> 0.91 |
| B | 0.291 | `WaterAbsorbScaleB` 1.5 | 0.437 | 0.94 -> 0.92 |

Red goes first; green and blue are now near-equal absorbers, so the survivor
is cyan rather than sky-blue, and the shallows drift green-cyan with the
scattering (G 0.067 vs B 0.120) setting the body hue. 1/1/1 is the previous
water bit for bit. `water_caustics_graph` derives its own attenuation from
`ABSORPTION_COLOR` and does not see the scale -- a minor, known divergence
(backlog).

### The wake's foam, new defaults (`ripple_field_graph.DISTURBANCE_FOAM_DEFAULTS`)

| scalar | was | now |
|---|---|---|
| `DisturbanceFoamThreshold` | 0.05 | **0.2** |
| `DisturbanceFoamGain` | 6 | **3** |
| `DisturbanceFoamHeightWeight` | 1 | **0** (gradient only) |

So foam needs `|grad| * RippleFieldGain > 0.2` to start and 0.53 to be full
white -- crests only. The wave-size levers are live cvars with the owner
(`voxel.Boat.WakeGain`, `voxel.Water.Ripple.Gain`, `HalfLifeSec`,
`voxel.Boat.WakeWidthScale`).

### The ladder (launch-time, lake sheet only, every pair echoes `Lake sheets: material scalar '<Name>' set to`)

Baseline = the regenerated defaults (no switch). Move ONE thing per arm:

    -VoxelWaterMatScalar=ShallowTurbidityFloor:0            (1) off  -- the control for the whole floor
    -VoxelWaterMatScalar=ShallowTurbidityFloor:1
    -VoxelWaterMatScalar=ShallowScatterBoost:5
    -VoxelWaterMatScalar=ShallowScatterBoost:30
    -VoxelWaterMatScalar=ShallowTurbidityDepthM:0.6
    -VoxelWaterMatScalar=ShallowTurbidityDepthM:3
    -VoxelWaterMatScalar=ShallowBodyEmissive:0               body floor off (scatter boost stays)
    -VoxelWaterMatScalar=ShallowBodyEmissive:0.6
    -VoxelWaterMatScalar=ShallowBodyR:0.02,ShallowBodyG:0.18,ShallowBodyB:0.26   bluer body
    -VoxelWaterMatScalar=ShallowBodyR:0.08,ShallowBodyG:0.26,ShallowBodyB:0.14   greener body
    -VoxelWaterMatScalar=WaterAbsorbScaleR:1,WaterAbsorbScaleG:1,WaterAbsorbScaleB:1   (4) off
    -VoxelWaterMatScalar=WaterAbsorbScaleR:3
    -VoxelWaterMatScalar=WaterAbsorbScaleB:2                 pushes the survivor toward green
    -VoxelWaterMatScalar=DisturbanceFoamThreshold:0.35       wake foam: crests only, tighter
    -VoxelWaterMatScalar=DisturbanceFoamThreshold:0.1,DisturbanceFoamGain:4   wake foam: wider

(2) has no launch-time switch: `SubmergedDarken` / `SubmergedRampM` are
scalars on `M_VoxelTerrain` / `M_VoxelClipmap` (Material Instance in the
editor, or edit the default in `terrain_material_common.py` and regenerate
both), and the marcher's copy is the two `#define`s in `VoxelMarch.usf`. The
marcher's is the one the owner will see at the lake; keep the three in step.

### Regen list (separate invocations; verified by byte-searching each .uasset for a new scalar name)

| generator | asset | proves |
|---|---|---|
| `create_water_voxel_material.py` | `M_WaterVoxel.uasset` | `ShallowTurbidityFloor`, `WaterAbsorbScaleR`, `ShallowBodyEmissive` |
| `create_ocean_material.py` | `M_Ocean.uasset` | same three |
| `create_voxel_material.py` | `M_VoxelTerrain.uasset` | `SubmergedDarken` |
| `create_clipmap_material.py` | `M_VoxelClipmap.uasset` | `SubmergedDarken` |
| `create_underwater_material.py` | `M_Underwater.uasset` | (constants only: `absorption_per_m()` changed; no new name -- verified by the regen log and mtime) |
| -- | `VoxelMarch.usf` | compiles at the next launch |

Results of the regens are appended below as they land.
2026-09-08 | UI/map | AUTHORED (map agent): SVoxelMapSheet with one transform (ViewCentreUU, UUPerPixel); default view player-centred at 10 km on the short side; wheel zoom about cursor (+/- keys; Ctrl+wheel left to the shell dial); right-drag pan with a 4 px threshold; right-click menu Mark/Rename/Remove with inline name field; marks persisted to Saved/VoxelWorlds/<seed>.vxmarks.json (write-through, tolerant loader, 4096/64 caps); geometry player arrow that leaves the frame rather than clamping. TWO DEFECTS FOUND: (a) the map has drawn a DIFFERENT PLANET since 2026-08-01 -- RasterPathForSeed resolved world-maps/seed<N> (provider 71e2b362) while DefaultGame.ini points the engine at 80b9ca451a23eae4-seed<N>; now resolved by provider from the coarse tile dir, no seed fallback; (b) the hillshade raster is SOUTH-UP (heightmap.py origin=upper, no Y negation; r=0.82 vs -0.29 against decoded tile-row elevations) -- the sheet now flips V. Extent measured from the .vxtl set (-245.76..+15.36 km). Build.cs gains Json | **BUILDING**; open: +/- need focus (first click), zoom floor 500 m is ~4 source px of a 120 m/px raster, CoarseTileDir() duplicates the subsystem's resolution | SVoxelMapScreen.*, VoxelMapMarks.* (new), VoxelScreenData.h, VoxelScreensUISubsystem.*, VoxelUITheme.h, VoxelUIStrings.*, VoxelEarthUI.Build.cs
2026-09-08 | water | OWNER DIRECTIVE: "i want our river and lake water + surface effects and ripples to all look as close to possible as Elder Scrolls Oblivion IV Remastered. Do open source internet research and see if there is anything worth learning or calibrating our own config values after" | **ASSIGNED** (Fable research agent, read-only; deliverable docs/water-oblivion-remastered-reference-2026-09-08.md with a calibration table onto our scalars) | pending
2026-09-08 | audio | OWNER DIRECTIVE: research Oblivion/Skyrim exploration music -- 20-30 ambient/environment tracks (no epic, no vocals, no hero moments), and the distinction between the released OST and the larger in-game explore pool; map onto our 30-track library with a tagging scheme | **ASSIGNED** (Opus research agent, read-only; deliverable docs/music-exploration-reference-2026-09-08.md) | pending

### Regen results (2026-09-08, after build-done.flag 01:07:08; each a separate `-Only` invocation, Get-Process clear before each)

| generator | log gates (ok / pyerr / notship / compilefail) | asset | names verified in bytes |
|---|---|---|---|
| `create_water_voxel_material.py` | 1 / 0 / 0 / 0, 35 s | `M_WaterVoxel.uasset` 01:15:04, 244352 B | ShallowTurbidityFloor, ShallowScatterBoost, WaterAbsorbScaleR, ShallowBodyEmissive, ShallowBodyG, DisturbanceFoamThreshold; no NOT A SHIPPING / RIPPLE_DEBUG |
| `create_ocean_material.py` | 1 / 0 / 0 / 0, 36 s | `M_Ocean.uasset` 01:19:12, 193307 B | ShallowTurbidityFloor, ShallowScatterBoost, WaterAbsorbScaleR, ShallowBodyEmissive, DisturbanceFoamThreshold; no debug markers |
| `create_voxel_material.py` | 1 / 0 / 0 / 0, 13 s | `M_VoxelTerrain.uasset` 01:19:48, 111106 B | SubmergedDarken, SubmergedRampM (WetShoreDarken still present) |
| `create_clipmap_material.py` | 1 / 0 / 0 / 0, 13 s | `M_VoxelClipmap.uasset` 01:21:17, 111650 B | SubmergedDarken, SubmergedRampM |
| `create_underwater_material.py` | see the line below | `M_Underwater.uasset` | constants only (absorption_per_m); proof is the log's optics summary |
| `VoxelMarch.usf` | compiles at the next launch | -- | VOXEL_MARCH_SUBMERGED_DARKEN 0.45 / RAMP_M 0.30 |

Both water logs print `absorb channel scale  R 2.00  G 1.00  B 1.50 (2026-09-08 item 4)`.
2026-09-08 | UI | OWNER VERDICT: "title screen passes" (pass-two insets, title 112, callout x0.75, K no longer clipping) | **ACCEPTED** | live session, build of 01:1x
2026-09-08 | WATER | OWNER VERDICT on the turbidity floor + bed darkening + channel extinction set (live, lake over snow shore): "water looks better/good enough for now" | **ACCEPTED** | committed this row
2026-09-08 | UI | OWNER VERDICT: map (marker, zoom, right-drag pan, marks) and hold-Tab cursor "work as expected" | **ACCEPTED** | live
2026-09-08 | UI | OWNER: journal elements inside the unified pane now too small; no hover/drag handle to resize the unified panel | **OPEN** | UI agent
2026-09-08 | WAKE | OWNER: "much better but still much too large, spread too far, last too long; smaller, finer, closer to the canoe, radiate slower, persist as long or longer" | **OPEN** | live dials then defaults
2026-09-08 | WAKE | Defaults set from owner feedback without a live dial pass (owner: "make the best changes you can"): Ripple.SpeedMPS 1.6->0.5, Ripple.HalfLifeSec 5->8, Boat.WakeGain 3.0->1.5, Boat.WakeWidthScale 1.0->0.3. Spread = speed x lifetime; footprint sets wavelength. | **BUILT, awaiting owner** | next launch
2026-09-08 | WAKE | Defaults set from owner feedback without a live dial pass (owner: "make the best changes you can"): Ripple.SpeedMPS 1.6->0.5, Ripple.HalfLifeSec 5->8, Boat.WakeGain 3.0->1.5, Boat.WakeWidthScale 1.0->0.3. Spread = speed x lifetime; footprint sets wavelength. | **BUILT, awaiting owner** | next launch
2026-09-08 | WAKE | OWNER: "water and wake, surface effects inside the canoe. the inside of the boat should be dry" | **OPEN** | hull mask investigation
2026-09-08 | WAKE | Cockpit water diagnosed: the CustomDepth lid sits 6 UU over the AMBIENT surface; the ripple WPO adds the wake crest on top, so pixels inside the hull rise above the lid, fail `behind > 0`, escape the stencil cull, and the disturbance foam paints them. No lid height fixes both directions. | DIAGNOSED | evidence: water_hull_mask_graph.py contract + create_water_voxel_material.py ripple WPO site
2026-09-08 | WAKE | Hull ripple mask built (working tree, regen + build PENDING): MPC_VoxelSky vectors HullEllipseA=(cx,cy,cos,sin) HullEllipseB=(halfLen,halfBeam,enabled,edge) pushed per tick by AVoxelBoat::PushHullRippleMask; water_hull_mask_graph.build_hull_ripple_mask multiplies the ripple WPO height and the disturbance foam by a world-XY ellipse keep-mask in M_WaterVoxel and M_Ocean; gradient/normal untouched. Off arm voxel.Boat.HullRippleMask 0 (enabled 0 = pixel-identical). Regen order: create_sky_material.py (MPC) -> full sky chain -> create_water_voxel_material.py -> create_ocean_material.py. Engagement line: `VoxelBoat: hull ripple mask ENGAGED`. | **BUILT, awaiting regen + owner** | fork agent
2026-09-08 | UI | Journal "too small" root cause: SScaleBox(DownOnly) around the body scaled by DESIRED width; auto-wrap text with no wrap width asks for its longest line (1221 units at 18 px) -> factor 0.62 (codex 0.58). Fixed by bounding the page columns (988 exactly); shell now drag-resizes from the right/bottom edge and corner grip, committed to ScreenShellScale on release. | **BUILT, awaiting owner** | next launch
2026-09-08 | HARNESS | Unattended boat capture died on frame 1: AVoxelOceanActor::UpdateUnderwaterState read PlayerCameraManager before its first update (origin), worldgen asked for tile (-1,-1), fatal gate leak by design. Guard: use the pawn until GetCameraCacheTime() > 0. Log: Saved/capture-chainproof-boat.log | **FIXED, building** | rerun capture
2026-09-08 | WAKE | Headless boat leg after the full chain regen: no fatal, 0 "Failed to compile", `hull ripple mask ENGAGED halfLen=187 halfBeam=45 edge=10`, RippleField armed at 0.50 m/s / half-life 8 s. Frame VoxelVerify00992.png (pitch -35, +12 m, boat under way at the lake). Conditions only; the owner judges wake size and the dry cockpit. | **FRAME READY** | owner |
2026-09-08 | UI | Backlog set landed (unbuilt at time of row): dead title constants removed; seeded journal anchored to the live day (no card after today, test); HUD hidden AND dropped to z 109 under every overlay incl. pause (RefreshHudForOverlays, logs "HUD hidden/restored for overlay"); death stamp now "Day N · Season, Nth Year of the Second Age" via one WorldStamp composer; VoxelSky::SeasonIndexFromDayOfYear exported. Two visual growths for the owner: death card stamp, journal card date line (ellipsis). | **BUILD PENDING** | owner capture |
2026-09-08 | LOADER | -VoxelFineTileRingRadius now has ONE reader (FVoxelFineTileStreamer::RingRadiusFromCommandLine); default unchanged (0); a future flip is the one constant kDefaultFineRingRadiusTiles. | **BUILD PENDING** | after the quiet-box measurement |
2026-09-08 | MUSIC | Headless -Shot Menu leg on the current binary: `pools built -- 29 cue(s): Explore/Day=11, Explore/Night=1, Dawn=0, Dusk=0, Rain=0, Cave=3, Town=7, Water=1, Combat=2, Menu=1, Stingers=1, ...`, then `pool=Menu slot=- cue=02 Main Title gap=24.2`; 0 fatals, 0 UI errors; frame VoxelMenu00032.png. Pool selection and the gap draw engage; in-world pools (Explore/Water/Cave, crossfade) still unseen because unattended runs stay silent in game by design. | **ENGAGED (menu)** | owner launch for in-world |
2026-09-08 | UI | 1080p shell trim (ADR-0011 d.4): premise refuted by measurement -- ShortestSide gives the shell 1080 units tall at EVERY landscape resolution (1080p/1440p pair 53.3% of width, same layout). Only Menu Size >= 1.40 or INTERFACE SIZE 1.50 overflow; ShellChromeForViewport trims chrome first (PadTop 14->8, BodyPadY 18->10, ActionBarGap 10->6 = 26 units), then clamps and scrolls. Capture tool -ResX/-ResY were INERT (borderless); fixed with measured-size print. BEFORE pair in Saved/ui-before/. | **BUILD PENDING** | owner: 1080p vs 1440p pair, Menu Size 1.50 shot |
2026-09-08 | UI | OWNER VERDICT on the 1080p / Menu Size captures: "Title screen looks good. 1.50 reads ok but 1.00 is much better looking for the menu size; giving the player the option is nice." Default stays 1.00; the 0.75-1.50 dial and the drag grip stay. | **ACCEPTED** | docs/ui-mocks/2026-09-08/1080p |
2026-09-09 | TESTS | Headless automation on the quiet box: VoxelEarth.FrontEnd.* 10 pass (Chrome, Layout, ResizeDrag, ResizeZone, Darkened, JournalSeed, LoadProgress, Slugify, SwitchPolicy, WorldStamp), VoxelEarth.Music.* 6 pass (Gaps, PoolPriority, RecentsAndBanks, Shuffle, SlotBorrowing, SlotFromHour). 2 FAIL in VoxelEarth.GpuPool.BrickBasic/BrickChunkRecord: expected resident bytes 21696, got 21824 = 4 records x 32 B extra; the Codex session's UNCOMMITTED VoxelBrickPool diff adds `Appearance` to FResidentChunk, so the format census in VoxelGpuGeometryPoolTests.cpp:447 is theirs to update. | **MINE PASS; 2 CODEX** | tell Codex |
2026-09-09 | LOADING | Quiet-box legs: the hourglass freeze was the lake-sheet first gather loading fine tiles SYNCHRONOUSLY on the game thread (13.6 s: 11.3 s for a tile the streamer was mid-read on, ~0.4 s each cache-warm). Fix: lake tier async arm (worker read+decode, adoptWarmTile, gated on the streamer ring) -- leg 3: every tile "loaded ASYNC ... adopt 0.00 ms", gather 0.0-0.1 ms per tile, READY 13.7 s. Remaining under the curtain: 5.7 s / 4.8 s unlogged frames and two 1.4-1.8 s world-subsystem ticks; loader switches flipped on for interactive launches. | **LANDED** | next: attribute the rest |
2026-09-09 | LOADING | Insights traces (legs 4-7): the remaining curtain freezes are the first ring recomputes (1.7 + 5.4 s, admission loop) and single DispatchJobs passes of 6.2 / 8.8 / 9.1 s (~1300 iterations at 5-9 ms, unbounded loop during cold fill). The Hitch frame line was one tick late on its own attribution (fixed: prints PrevTickMs). Fix landed: voxel.Stream.DispatchBudgetMs (0 = unbounded), theatre caps it at 8 ms. | **BUILD/LEG PENDING** | leg 9 |
2026-09-09 | LOADING | Leg 9 with the dispatch budget: `capped voxel.Stream.DispatchBudgetMs 0.0 -> 8.0`, `wall-clock budget ENGAGED`, exitBudget=168 of 762 passes in the fill window; seg=LOADING n=3789 hitches=15 p50 5.2 ms p99 20.6 ms max 5328 ms (was n~1060 hitches 33-40 p99 8.5-9.1 s max 9.1 s); READY 14.1 s (unchanged). The one remaining freeze is the first-recompute admission loop (5.3 s) at spawn. | **LANDED** | backlog: split the first recompute across ticks |
