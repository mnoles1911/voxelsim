# Water deprecation audit — 2026-08-09

**What this is.** A report, not a change. Nothing in `terrain-service/`,
`voxel-core/` or `ue-project/` was touched to produce this document — it lists
what `docs/water-rearchitecture-plan-2026-08-09.md` makes obsolete, with
per-item evidence (file:line, referencing files, pinning tests) and a
recommended action. **The integrator deletes; this document lists.**

**Method.** Every item below was verified against the current tree (grep for
every symbol, read every call site, read every pinning test), not inferred
from the plan's prose. Where the plan's own words already state an item's
disposition ("kept as documented fallback", "kept for reproduction"), that is
cited rather than re-argued.

**The conservative rule applied throughout:** anything with a live call site
outside its own test file, or a test that pins its behaviour, is
**deprecate-later**, never **delete-now** — regardless of how clearly the new
architecture supersedes its purpose. A recommendation of "delete now" appears
exactly zero times in this document, because every item in scope turned out to
have at least one of those two properties. See §9 for the one item that came
close.

---

## Summary table

| # | item | live refs? | pinning tests? | recommended action |
|---|---|---|---|---|
| 1 | bake water-shape stages (`water.py`) | yes — the default bake path | ~20 tests | **keep-as-data** — feeds faucets/ribbons/graph, not deprecated |
| 2 | SWE (`swe.h`, `swe.cpp`) | yes — `VoxelWaterSubsystem.cpp`, ~20 sites | golden-pinned suite | **keep-as-fallback** (per plan, explicit) |
| 3a | `rivernet.h` | yes — live default path + Phase 2 target | yes | **keep** — actively extended by the new plan |
| 3b | `rivercouple.h` | yes — ~10 sites, default-off cvar | 8 dedicated tests | **deprecate-behind-flag** (already is one; slate for removal after Phase 5) |
| 4 | `widen_to_channel_width` | yes — one opt-in config value | 5 tests | **keep-as-fallback** (reproduction of pre-bv13 bakes) |
| 5 | magenta water marker path | yes — active debug tool | 2+ tests | **keep** — not deprecated, still useful under the new plan |
| 6 | `farwater.h` + bench tools | none outside own tests/bench | 15 tests | **deprecate** — mark obsolete now, remove in a follow-up |
| 7 | `reservoir_v0` | none (already deleted); 3 guard/history tests remain | 3 tests | **keep-as-data** (regression guard against the bug recurring) |
| 8 | `density3` remnants | none (comment/string only) | none | **no action** — not a deprecation item, optional naming cleanup |

---

## 1. The bake water-shape stages in `terrain_service/bake/water.py`

`settle_to_adjacent_level` (line 770), `apply_discharge_budget` (975),
`equalize_lateral_levels` (1507), `smooth_level_field` (1191),
`enforce_upstream_monotone` (1120), `enforce_neighbour_consistency` (1339),
and the level-band logic (`LEVEL_BAND_MM = 2400.0` at 3152,
`LEVEL_BAND_DILATE_PX = 2` at 3176, `local_water_levels` at 3205).

**Pipeline wiring** (`terrain-service/terrain_service/bake/pipeline.py`,
~4976–5201) — this is the live, default-on stage order that produces the
shipped `water_surface` / `discharge` / `water_level` planes:

1. Extent (5005–5020): `water_extent_mode` — default `"lateral_fill"`; `"law"`
   reaches `widen_to_channel_width` instead (see §4).
2. `bridge_to_face_contact` (5038, `consts.water_face_contact_bridge`).
3. `settle_to_adjacent_level` (5088, default on) — internally calls
   `apply_discharge_budget` (default on) and `smooth_level_field`, but
   `smooth_level_field`'s own trigger (`water_level_smooth_iters > 0`)
   defaults to **0 iterations**, so it is wired and tested but **currently
   dormant in production baking**.
4. `enforce_neighbour_consistency` (5099, default on).
5. `equalize_lateral_levels` (5130, default on).
6. `enforce_upstream_monotone` (5163, default on) — a stray `locals()` guard
   once made this stage silently dead for a whole session; now fixed
   (`pipeline.py:5156-5161`).
7. `local_water_levels` (5190, gated) — produces `water_level`, the sub-pixel
   waterline band shipped alongside `water_surface`.

**Tests** (`terrain-service/tests/test_bake_pipeline.py`, ~4088 lines): 2 for
neighbour consistency (3261, 3303), 9 for lateral equalisation (3355–3719), 5+
for the level band and `local_water_levels` (3760–3984), 2 for the discharge
budget (4022, 4070). `enforce_upstream_monotone` has no test by that exact
name but is exercised transitively by full-pipeline fixture tests.

**Why this is not a deprecation candidate at all, despite being exactly the
kind of code the re-architecture supersedes as *drawn shape*.** The
re-architecture plan is explicit: *"The bake's basin machinery, water plane,
flow plane, discharge and heads all remain load-bearing — as data (faucet
placement and rates, graph geometry, basin capacity, ribbon source), not as
drawn shape"* (`water-rearchitecture-plan-2026-08-09.md`, "What this means for
the existing work"). Every one of these six stages feeds directly into the
`water_surface`/`water_level`/`discharge` planes that `RiverNetwork::
buildFromBakedWater` (Phase 2), the ribbon actor, and the basin registry all
still read. Deleting any of them breaks the default bake path outright
(`ImportError`/`AttributeError` in `pipeline.py`) and fails ~20 pinned tests.

**Recommended action: keep-as-data.** No change. The one item worth flagging
to the integrator: `smooth_level_field` is dead-by-default (0 iterations) —
not a deletion candidate (it is tested and reachable), but worth a comment at
its call site noting it is currently inert so a future reader does not assume
it is shaping production output.

---

## 2. SWE (`voxel-core/include/voxelcore/swe.h`, `swe.cpp`)

**Wired, not orphaned.** `VoxelWaterSubsystem.cpp:29` `#include
"voxelcore/swe.h"`, with `Impl.SweSheet` (`std::unique_ptr<vxc::SweGrid>`,
line 1175) and `Impl.SweCoupler` (`std::unique_ptr<vxc::SweCaCoupler>`, line
1176) constructed in `MaybeArmSwe` (line 4165, called from the tick at 5097),
plus ~20 more read sites (velocity/depth/bed queries, conservation check,
flush-to-CA, save/load guard) between lines 3916–6437.

**Double-gated off.** `voxel.Water.SWE` (`VoxelDebug.cpp:145-152`,
`TAutoConsoleVariable<bool>`, **default `false`**), and `MaybeArmSwe` refuses
to arm on any net mode except `NM_Standalone` regardless of the cvar (ADR-0004
item 3's deferral). `SweCoupleConfig::enabled` also defaults false, making
`SweCaCoupler::step()` a total no-op even if constructed
(`swe_coupler_is_a_total_no_op_when_disabled`, cited in ADR-0004).

**Tests**: `voxel-core/tests/test_swe.cpp` — full numerics suite, golden-pinned
under an independent `kSweVersion` so an SWE change can never invalidate a
water golden (ADR-0004).

**Disposition, per the plan itself** (not re-argued here — cited):
*"SWE and the conveyor design are shelved with their analysis on record — if
PBF's Phase 0 numbers fail, that analysis is the documented fallback, not a
restart"* (`water-rearchitecture-plan-2026-08-09.md`, closing section). The
plan's §4 also gives the measured reason SWE is disqualified as the active
flowing-water solver: an integer dead zone below ~3% water-surface slope where
steady transport is exactly zero, against a world whose median river gradient
is 0.94%.

**Recommended action: keep-as-fallback, exactly as the plan directs. Do not
delete, do not enable.** This is real, wired, tested, doctrine-compliant code
that happens to default off — a kill switch, not dead weight — and the plan
explicitly wants it preserved as the documented fallback rather than deleted.
The only actionable follow-up: nothing currently prevents someone from
flipping `voxel.Water.SWE` on outside the standalone/single-player path this
ADR scoped it to; that gate is already correct (net-mode check in
`MaybeArmSwe`), so no code change is implied, just a note that reviving SWE
requires re-opening ADR-0004, not a cvar flip.

---

## 3. `rivercouple.h` vs `rivernet.h`

### 3a. `rivernet.h` — keep, and it grows

`RiverNetwork::buildFromFlowAccumulation` (declared `rivernet.h:328`, defined
`src/rivernet.cpp:46`) is called live today from `VoxelWaterSubsystem.cpp:4519`
— this is the currently-active river-graph builder, independent of
`voxel.Water.Rivers`. The plan names `rivernet.h` explicitly as a file the
re-architecture **extends**, not retires: *"routing graph rebuilt from baked
water+flow planes (`RiverNetwork::buildFromBakedWater`) and persisted"*
(Phase 2), and lists it under "Critical files" as
*"`voxel-core/include/voxelcore/rivernet.h` — `buildFromBakedWater`,
persistence (scalar authority fabric)"*.
**`buildFromBakedWater` does not exist in the tree yet** — it is a Phase 2
deliverable, not shipped code. No action; this is new-build scope, not
deprecation scope.

### 3b. `rivercouple.h` — deprecate-behind-flag (already is one; earmark for removal after Phase 5)

**~10 call sites**, all in `VoxelWaterSubsystem.cpp`: the `#include` (34), the
member `std::unique_ptr<vxc::RiverCaCoupler> RiverCoupler` (1244), the armed
check (4419), disarm ledger reads (4440-4441), `.reset()` (4442), config
construction (4548), construction (4554), the per-tick `->step()` call
(4664), outfall-write iteration (4671), `->takePendingDiffs()` (4681), and a
ledger-consistency check/log block (4702-4706).

**Gated off by default**: `voxel.Water.Rivers` (`CVarVoxelWaterRivers`,
`VoxelDebug.cpp` ~line 170), refused on `NM_Client`.

**Tests**: `voxel-core/tests/test_rivercouple.cpp` — 8 dedicated
`rivercouple_*` tests (no-op-when-disabled, exact conservation,
refusal/back-pressure, ocean-is-sink, never-writes-at-sea-level,
promotes-sustained-channel, rejects-puddle/flicker/dead-end,
promotion-moves-no-water) plus 3 `rivernet_*` tests in the same file exercising
`promoteChannel`/bifurcation/diff-replay that `rivercouple.h` depends on.

**Why this is a deprecation candidate at all.** `rivercouple.h`'s entire
purpose is injecting river discharge into the CA (`rivercouple.cpp:162`,
per `water-system-architecture.md` §2.1) — coupling the graph to the solver
that the re-architecture is retiring rivers *away from*. The plan's roadmap
(Phase 5) has rivers stop being CA/implicit-mesh content near the player at
all; once that lands, the CA-coupling role `rivercouple.h` exists for has no
remaining caller. The plan's own file list (§"Critical files") names
`rivernet.h` as touched going forward and does **not** name `rivercouple.h`,
which is consistent with it having no role past Phase 5.

**Recommended action: deprecate-behind-flag.** It already is one
(`voxel.Water.Rivers`, default off) — no code change needed today. Flag for
the integrator: re-evaluate after Phase 5 lands and rivers are migrated off
the CA; if nothing else has picked up a dependency on `RiverCaCoupler` by
then, it and its 8 tests are the actual delete-now candidate at that point,
not before. Do not delete now — it has live call sites and a pinned suite.

---

## 4. `widen_to_channel_width`

**Location**: `terrain-service/terrain_service/bake/water.py:627`.

**Reached only opt-in.** `pipeline.py:5016`, inside
`elif consts.water_extent_mode == "law":`. Default `water_extent_mode` is
`"lateral_fill"` (`pipeline.py:1187`) — `widen_to_channel_width` **does not
run in a default bake.** `EXTENT_MODES = ("centreline", "law",
"lateral_fill")` (`pipeline.py:1426`) is enforced at config construction
(`__post_init__`, 1428-1433), so `"law"` stays a legally selectable, tested
mode rather than dead code.

**The module's own framing matches "kept for reproduction" even though that
exact phrase isn't the docstring's wording.** `pipeline.py:376-382`:
*"`water_width_from_law` (retired at bake_ver 13, where it became
`water_extent_mode == "law"`)... bake_ver 11's plane drew a centreline: 99.21%
of wet pixels were one 1.875 m pixel against a law width of 3.53 m at p50"* —
i.e. it is the pre-bv13 behaviour, kept selectable rather than deleted when
`lateral_fill` became the default. `water.py:68-75` confirms the width **law**
functions it depends on (`channel_width_m`, `q_for_width_m`) stay live for a
different, load-bearing reason — `q_drawable_m3_yr` needs the inverse — so
those are not part of this item at all, only `widen_to_channel_width` itself.

**Tests**: `test_bake_pipeline.py` — `test_width_follows_the_discharge_and_stops_at_the_ground`
(2336), `test_width_never_leaves_a_trench_it_cannot_fill` (2398),
`test_width_does_not_spill_into_a_registered_basin` (2417),
`test_width_takes_the_nearest_reach_not_the_highest` (2446), and
`test_extent_mode_is_enumerated_and_a_typo_is_refused` (2743, iterates all
three modes including `"law"`).

**Recommended action: keep-as-fallback.** It is already gated behind an
explicit, non-default config value, so there is no flag to add. It exists to
reproduce old (pre-bv13) bakes exactly and is covered by 5 tests. The
re-architecture doesn't need this path removed to proceed — the shipped
default is `lateral_fill`, and `widen_to_channel_width` is inert unless someone
deliberately selects `"law"`. No action beyond noting its status.

---

## 5. The magenta water marker path

**Core symbol**: `MAT_WATERMARK = 15` (`voxel-core/include/voxelcore/core.h:333`).

**Production usage** (not test-only): `amplifier.cpp` `stratigraphyAt`
produces it (~lines 2597, 2622, 2625); `fluidoccupancy.h:120-133` exempts it
from "solid stops a particle"; emptiness gates in `amplifier.h:240,299` and
`VoxelFootprintBand.h:121`.

**Confirmed CPU-only, deliberately.** Zero references in
`voxel-core/shaders/worldgen.ush` or `ue-project/Shaders/VoxelWorldGen.usf`.
`VoxelWorldSubsystem.cpp:13758,13781-13786` (`InstallWaterMarker`) **refuses to
install the marker if the GPU mesh fork is on**, logging that worldgen.ush has
no `MAT_WATERMARK` branch and that a capture taken under the wrong fork
already happened once (14,848/44,873 chunks GPU-produced, had to be re-shot —
`water-session-handoff-2026-08-06.md`).

**Wiring**: `UVoxelWaterSubsystem::Initialize` (`VoxelWaterSubsystem.cpp:1363-1406`)
parses `-VoxelWaterMarker[=1]` and `-VoxelWaterMarkerOcean=<0|1>`, builds a
`LockedWaterSampler`, calls `UVoxelWorldSubsystem::InstallWaterMarker`
(`VoxelWorldSubsystem.cpp:13739-13812`), which also reads
`-VoxelWaterMarkerFillPx=<n>` (clamped 0-32). A separate flag,
`VoxelWaterMarkerOnly()` (`VoxelWaterSubsystem.cpp:167-181`, `-VoxelWaterMarkerOnly[=1]`),
suppresses the real water renderers and is independently checked in
`VoxelRiverRibbonActor.cpp:106-111` and `VoxelWaterSheetActor.cpp:99-105`.
Rendering-side opacity: `CVarVoxelWaterMarkerOpacity`
(`VoxelQuadVertexFactory.cpp:27-31,115`).

**Note on the flag names the task asked to check.** The real near-field flag
is `-VoxelWaterLateralFillPx=<n>` (`VoxelWaterSubsystem.cpp:746-799`, off by
default — *"adds up to 8n sampler calls to a dry miss"*), a different
mechanism from the marker's own `-VoxelWaterMarkerFillPx`. No literal
`lateral-fill-px` or bare `LateralFillPx` string exists; both real flags carry
the full `-VoxelWater...` prefix.

**Tests**: `test_amplifier.cpp:1160,1164,1221` pin `stratigraphyAt`/`materialAt`
returning `MAT_WATERMARK` at the right column; `test_mesher.cpp:129` notes the
mesher golden must not have it appended.

**Python-side documentation**: `ue-project/Tools/terrain_material_common.py:335`,
`terrain_palette.py:50-53` — *"DEBUG INSTRUMENT, not world content... Only ever
produced under `-VoxelWaterMarker=1`."*

**Recommended action: keep.** This is not a deprecation candidate — it is a
deliberately-scoped, safety-railed diagnostic tool (the GPU-fork refusal is
exactly the kind of guard the re-architecture's own verification section asks
for) and it remains useful for the new work: visualizing where the basin
registry and faucet placement think water is, at full clipmap range, is
exactly the kind of "solid marker voxels... an instrument, not a feature"
`water-system-architecture.md` §11b already argues for. No action, other than
noting it stays CPU-only unless someone deliberately mirrors `MAT_WATERMARK`
into `worldgen.ush` (previously filed as the top open task in
`water-session-handoff-2026-08-06.md`, itself now superseded in priority by
the re-architecture).

---

## 6. `farwater.h` and the far-water bench tools

**Locations**: `voxel-core/include/voxelcore/farwater.h` (294 lines,
header-only, `constexpr`/`inline`, no `.cpp`), `voxel-core/bench/farwaterprobe.cpp`,
`voxel-core/bench/farwaterschemes.cpp`, `voxel-core/tests/test_farwater.cpp`.
No "farwaterenum" symbol exists; the closest are `farWaterLodForDistance` (an
integer LOD-selection function, not an enum) and the two bench executables.

**Confirmed bench-only.** `voxel-core/bench/CMakeLists.txt:247-268` builds
`vxc_farwaterprobe` and `vxc_farwaterschemes`, both linked only against
`voxelcore`.

**Confirmed zero UE references** — grepped all of `ue-project` for
`farwater|FarWater` case-insensitively; no matches. `far-voxel-water-plan.md`
says so itself: *"The UE binding site is not wired. This is deliberate, and
there is a hard dependency behind it"* — `create_water_voxel_material.py:786`
hardcodes one voxel of WPO, which blocks the multi-LOD water material this
header's output would need, and that blocker was never cleared.

**Direction confirmed superseded, not merely unfinished.** Both
`far-voxel-water-plan.md` and `far-water-schemes-decision-2026-08-05.md` now
carry `[SUPERSEDED by water-rearchitecture-plan-2026-08-09.md]` banners: the
new plan does not extend voxel water outward at all (near field goes to PBF +
screen-space fluid; mid/far stays ribbons/sheets, unchanged).

**Tests**: `test_farwater.cpp` — 15 `VXC_TEST` cases (LOD ring boundaries,
cell/brick size doubling, coarse-fill rules, majority-rule aggregation, brick
range, the interior-proof-never-fires-on-shallow-water guarantee), wired into
`vxc_tests` via `tests/CMakeLists.txt:36`.

**Why this doesn't get "delete now" despite zero engine callers.** The
conservative rule: it has a pinned test suite (15 tests) and two bench targets
that would need their `CMakeLists.txt` entries removed too. That is exactly
the shape of "has references, just not in the engine" the rule exists to
catch — a test suite passing today is a maintenance cost and a regression
signal both, and deleting it in the same pass as writing an audit report is
the thing this task was told not to do.

**Recommended action: deprecate.** This is the closest thing in scope to a
clean delete, and the one place worth being explicit that the integrator
should feel free to act quickly: mark the header and both bench tools as
obsolete (a comment banner citing this audit and the two superseded planning
docs is enough), and remove header + bench targets + `test_farwater.cpp` in a
dedicated follow-up once confirmed nothing in an active branch depends on it.
Not bundled into this audit because that would be an edit, not a report.

---

## 7. `reservoir_v0`

**Not a live symbol — a historical label for an already-deleted mechanism**
("pin breach voxels to 255 every tick," the pre-datum ocean top-up). Found in
7 files, all either comments or tests documenting the old failure modes:

- `voxel-core/include/voxelcore/lakes.h:870-878` — comments explaining the old
  mechanism's defects, citing the test names below.
- `voxel-core/tests/test_ocean.cpp:433` `reservoir_v0_pours_into_a_dry_seabed_and_never_settles`,
  and `:613` `reservoir_v0_floods_an_inland_pit_the_datum_test_leaves_dry` —
  real, currently-passing tests that demonstrate the **old** mechanism's
  failure modes under fixtures that still exist.
- `ue-project/Source/VoxelEarth/VoxelOceanCaptureFixture.cpp:31-35` — a
  comment citing the test name to justify a capture fixture's dig pattern.
- `terrain-service/tests/test_sea_level_contract.py:215-243` —
  **`test_reservoir_v0_stays_retired()`**, an active anti-regression guard:
  greps `VoxelWaterSubsystem.cpp` for `TSet<...> ReservoirCells` /
  `Impl->ReservoirCells` / `Impl.ReservoirCells` and **fails the build if
  found**. Docstring: *"§6.4 deleted the bespoke ocean top-up; it must not
  grow back... A second mechanism is the thing the whole plan forbids."* It
  explicitly permits one survivor — a `ReservoirCells` field on
  `FVoxelWaterPerfSnapshot` reported as a constant 0, kept only for HUD/log
  line shape compatibility.
- Narrative-only citations in `docs/water-architecture.md`,
  `docs/measurements/hydrostatic-cap-2026-08-04.txt`, `docs/water-map/ocean-captures.md`.

**Why this belongs in a deprecation audit at all, if the code is already
gone.** The task named it a "test straw-man," and that's accurate for
`test_ocean.cpp`'s two tests — but `test_sea_level_contract.py`'s guard test
is not a straw-man, it is a **live regression fence**, and it is exactly the
kind of thing worth flagging before the new scalar-hydrology basin ledger
lands: the new plan introduces per-basin volume ledgers and spillway-driven
faucets, which is a *different* mechanism from the old reservoir top-up but is
similar enough in shape (a per-cell/per-region "top up water here" pattern)
that it is worth the integrator double-checking this guard's grep pattern
doesn't false-positive against the new ledger's field names, and that the
ledger doesn't accidentally reintroduce the exact bug (unbounded growth into a
"dry" seabed; no distinction between an inland pit and the sea) the guard was
written to catch.

**Recommended action: keep-as-data.** All three tests stay — two document why
the old mechanism failed (useful regression coverage of the same pit/cove
fixtures under the *current*, correct CA-only path), one actively guards
against its return. Flag for the integrator: re-read
`test_sea_level_contract.py`'s grep pattern against the new `FBasinLedger`
design (Phase 2) before landing it, so the guard doesn't need a surprise
update mid-implementation.

---

## 8. "density3" remnants (removed at `kWorldGenVersion` 20)

**No dead code exists.** `density3.h` is not present in the tree
(`find voxel-core -iname "*density3*"` returns nothing). Every reference to
the string "density3" across `voxel-core/` (checked in `detail_bedding.h`,
`hash_channel_registry.h`, `worldgen.ush`, `amplifier.cpp`, `amplifier.h`,
`core.h`, `hash.h`, `bench/bench_main.cpp`, `bench/stagedump.cpp`,
`bench/terrainprobe.cpp`, `test_amplifier.cpp` — roughly 20 hits) is inside a
comment, a `static_assert` message string, a bench `printf` label, or one
bench JSON key (`"columns_displaced_by_density3"` in `stagedump.cpp`) — never
a live declaration, type, or call. `hash_channel_registry.h:78-83` states this
outright: *"density3.h allocated NOTHING and is deleted at kWorldGenVersion
20... removing the band frees no channel and leaves this table unchanged."*
The successor (`detail_bedding.h`'s `kBedding3*` family) correctly inherited
its budget. Zero references anywhere in `ue-project/`, including shaders.

**Recommended action: no action.** This is not a deprecation item — there is
nothing left to deprecate. Purely optional, low-priority naming hygiene for
whoever next touches `bench/stagedump.cpp` or `bench_main.cpp`: the printf
label and the JSON key still say "density3" when they now measure
`detail_bedding.h`'s displacement. Listed here only because the task asked
the audit to check for dead references, and the honest finding is that the
removal at v20 was already clean.
