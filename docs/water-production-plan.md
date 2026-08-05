# Production water: what exists, what is missing, and the order to build it

> ## SUPERSEDED — 2026-08-05
>
> **Do not plan from this document.** For how the water system works today, read
> **`docs/watershed-system-plan.md`**. For what is still wrong with it, read
> **`docs/water-deep-dive-brief-2026-08-05.md`**.
>
> This is kept because its reasoning is still worth reading — the
> deterministic-content vs mutable-state split (§4), the multiplayer argument
> (§6) and the "measure before you predict" performance discipline (§7) all
> still hold. What has gone stale:
>
> * **The four blocking facts are no longer blocking.** F1 (no lake basins) and
>   F4 (no water datum reaches the client) were both closed by the water plane
>   and the basin registry, at `bake_ver` 9 and 8. F5 (ocean handled
>   cosmetically) was closed when the sea became a real datum.
> * **Every version number here is wrong now.** It says `kWorldGenVersion` 10 →
>   11; it is **23**. `BAKE_VERSION` is **14**. `TERRAIN_VERSION` — which this
>   document predates entirely — is **8**.
> * **§9's work list and §10's branch triage are entirely spent.** The branches
>   it audits were merged or deleted; the work items landed by other routes.
> * **§3 is titled "the four blocking facts" and lists five** (F1–F5).
>
> One thing here that is still exactly true and worth carrying forward: *"State
> the worldgen version beside any water figure"* (§7). Every absolute number in
> this file expired when the versions moved, and it said so itself.

**Written 2026-08-02.** Answers the owner's question:

> "My assumption at the moment is that our game does not actually have the
> placement of water voxels at runtime to follow streams, fill lakes, and flow
> according to the carved terrain output by worldgen. What do we need to do
> next to realize that end state for production, game ready water for our
> players?"

**The assumption is CORRECT.** It is also correct for a deeper reason than the
question supposes, and that reason changes what "next" means. This document
gives the evidence, tests the deterministic-content/mutable-state split against
the code, and ends with an ordered work list.

Read `docs/world-generation-architecture.md` §4–§5 first; this document is the
promised follow-up to that section's two open items, and it **corrects one of
its claims** (§5, multiplayer water replication — see §6 below).

---

## 1. The answer, in one paragraph

Nothing in the shipping game places water to follow streams, fill lakes, or
flow along carved terrain. There are exactly **three** runtime water-placement
paths and none of them is hydrology: (a) worldgen **cavern** flood levels,
which are a hash of the seed and exist only underground; (b) an **ocean breach
hook** that adds water only after a player digs a hole below z=0 — nothing
pre-fills the sea; (c) **developer pours** (the `1` key, `voxel.SpawnWater`,
test fixtures). A fourth path, the river coupler, exists but is default-off,
non-persisted, and injects discharge onto unmodified hillside. Surface lakes
and ponds are not implemented anywhere. And underneath all of that sit two
facts that would defeat the feature even if it were written: **the baked
heightfield is guaranteed to contain no closed depressions**, so there is
nowhere for a lake to sit; and **the client's amplifier detail band re-strands
the ground at voxel scale**, measured at 87.9% of an alpine mountainside, so
water cannot flow anywhere regardless of what the bake hands over.

The water *simulation* is in good shape and is not the problem. The problem is
that nothing decides **where water belongs**, and the ground it would sit on
does not drain.

---

## 2. What exists today

Extensive, tested, and mostly merged. This is an inventory, not a complaint.

| layer | file | state |
|---|---|---|
| Pressure CA (the authority) | `voxel-core/include/voxelcore/waterca.h` | Volume-conserving, bit-deterministic integer CA, own `kWaterCAVersion`. Live and ticking. |
| Implicit cavern water | `voxel-core/include/voxelcore/caverns.h:423-427,635` | `floodZMm` per site, hashed from seed, 40% dry, zero storage. |
| Mobilizer (implicit → CA handoff) | `waterca.h:951` `WaterMobilizer` | The ownership partition that makes zero-storage water become real on contact. Shipping. |
| Persistence | ADR-0005, `WaterState` | Save/load with byte-identical digest round trip. voxel-core half done; **UE-side hook still open**. |
| SWE momentum | `voxel-core/include/voxelcore/swe.h`, ADR-0004 | Armed behind `voxel.Water.SWE`, **default 0**, and refused on any net mode except `NM_Standalone` (`VoxelWaterSubsystem.cpp:2921-2936`). |
| River graph | `rivernet.h` / `rivercouple.h` | Graph routing + dam/divert diffs + coupling to the CA. Behind `voxel.Water.Rivers`, **default false**, graph **not persisted** (`VoxelWaterSubsystem.cpp:3385`). |
| Channel geometry | `voxel-core/include/voxelcore/channel.h` | Trapezoidal water-tight bed, `waterLineMm` at `channel.cpp:281`. **Consumed by nothing.** |
| Rendering | `VoxelWaterSubsystem.cpp`, `docs/gpu-water-pool-design.md` | Second ADR-0006 pool instance, stepped fill-fraction surfaces, bilinear smoothing, material motion, 4–8 sort buckets, depth tint. `voxel.Water.GPU` **default true**. |
| Ocean | `VoxelOceanActor.h/.cpp` | A 40 km `/Engine/BasicShapes/Plane` at z=0 following the camera. **No collision** (`VoxelOceanActor.cpp:21`), no voxels, no CA interaction. |

### 2.1 The three real placement paths, with evidence

**Cavern flood field.** `VoxelWaterSubsystem.cpp:177-178` binds
`cavernFloodedAt` as the mobilizer's `ImplicitFn`. Insertion happens at
`waterca.cpp:1727` (`credited += ca.addWaterAt(...)` inside `mobilizeBrick`),
driven from `VoxelWaterSubsystem.cpp:3971` (every authoritative terrain edit)
and `:3322` (`advanceFront`, every 10 Hz step). Ungated, shipping. It is
underground-only and never runs at sea level or in a surface depression.

**Ocean breach.** `VoxelWaterSubsystem.cpp:3838` `NotifyTerrainVoxelsCleared`
skips any cleared voxel with `Z >= 0` (`:3860-3863`), requires a non-solid
below-sea-level neighbour (`:3871-3881`), then registers a reservoir cell and
calls `CA.addWater` (`:3887-3896`). `StepFixed` tops those cells back to 255
every step (`:3307-3315`). Called from `TryDig`, `TryPlace`, `CarveSphere`,
`PromoteDetachedIslands` — all normal gameplay. **The sea is not water until a
player cuts into it.**

**Developer pours.** `SpawnWaterAt` (`:3813` → `CA.addWater` at `:3828`) is the
single funnel. Reached from `voxel.SpawnWater` (`:4695`), `voxel.Water.SpawnIn`
(`:4729`), and — note — the `1` key bound in ordinary play
(`VoxelEarthPlayerController.cpp:48,593-609`, default `voxel.Water.BucketFill`
= 200000). No `!UE_BUILD_SHIPPING` guard exists anywhere in the water code.

**And the river path, for completeness.** `voxel.Water.Rivers` (default false)
builds a 48×48-pixel (1.44 km) graph once around an arming anchor
(`VoxelWaterSubsystem.cpp:3129,3139`), routes at ~1 Hz (`:3344-3361`), and
injects at `rivercouple.cpp:162`. It refuses to write at or below sea level
(`rivercouple.cpp:102-105,134-137`). Run in engine it delivered 3.69M fill
units with an exact ledger and produced **disconnected puddles**, recorded in
`docs/backlog.md` ("W3 rivers — PARKED 2026-07-29").

---

## 3. The four blocking facts

These compose into a single causal chain. Fixing any one alone changes nothing
visible.

### F1 — The baked heightfield is pit-free by contract, so no lake basin exists

The bake runs `fill_depressions` (`bake/flow.py:382-411`, a Barnes/Lehman/Mulla
epsilon priority-flood) and then runs it **again** as a micro-refill at
`pipeline.py:2924-2936`, after `enforce_descent` (`:2920`) has already raised
every cell to keep a drop over its receiver. The comment is explicit: *"'The
carrier drains' is a CONTRACT (0 sinks / 0.0% stranded on steep ground), so any
stray basin is resolved here, before the codec sees the ground."*

So depression filling in this pipeline is **drainage enforcement, not lake
creation**. It does not carve a basin; it levels one into rock. `flow.py:69-74`
says so directly: *"genuinely tile-spanning water bodies want a real lake/outlet
model rather than an epsilon ramp."* No lake model exists.

> **This corrects the premise in the question and in
> `world-generation-architecture.md` §4.** That table's reasoning — "the bake's
> depression fill is what creates the basin" — is backwards. The fill destroys
> the basin. The conclusion (lake surfaces should be baked datums) is still
> right; the work required is larger than serializing something that already
> exists.

How much is being levelled is already measured and then thrown away:
`pipeline.py:3035` computes `basin_depth = out["filled"] -
out["carrier_plus_roughness"]`, reduces it to four scalars, and `del`etes it at
`:3053/3058`. On the wet exemplar tile (`bake-out/wet-2_-4.json`) that is
`basin_max_depth_m: 135.2` over `basin_cells_frac: 0.0217`. **2.2% of that tile
is a lake bed that was filled in with stone.**

### F2 — The amplifier's detail band re-strands the ground at voxel scale

Even where the bake hands over a connected network, the client's 10 cm detail
synthesis destroys it. Measured and committed on main (`109f959`, 2026-07-29),
worldgen v13, carrier vs amplified on the same domain:

| | alpine (-5,15) | plains (-55,20) |
|---|---|---|
| interior sinks | 0 → **1625** | 98 → **9525** |
| stranded area | 0.0% → **87.9%** | 75.2% → **97.4%** |
| mean flow path | 223.6 m → **28.9 m** | 39.3 m → **4.3 m** |

The mechanism is arithmetic, not a bug: each octave contributes ~amplitude ÷
lattice of gradient, the v13 ladder sums to ~2.3 on ground whose own gradient is
0.4, and a detail gradient exceeding the carrier's reverses the downhill
direction — which is a closed basin. The relief gate scales detail *up* with
relief, so it is loudest where physics says it must be quietest.

**That commit applied no fix.** It states the bar: *detail must not reverse the
carrier's downhill direction*, with a floor so flat ground keeps its decimetre
roughness.

This is the single most important item in this document. It explains the
observed river symptom better than the parked backlog entry does: water poured
on this terrain puddles in noise micro-pits *everywhere*, whether or not a
riverbed was carved.

### F3 — `ChannelField` is wired to nothing, so there is no river vessel

`channel.h` builds a water-tight trapezoidal channel and carries `waterLineMm`
(`channel.cpp:281`, `bed + depth*3/4`). Grep for `ChannelField` across the repo
returns `channel.h`, `channel.cpp`, `tests/test_channel.cpp`,
`bench/riverprobe.cpp`, two `CMakeLists.txt`, and docs. **Zero references in
`ue-project/`, `amplifier.cpp`, or `world.h`.** `kChannelVersion` is
deliberately independent of `kWorldGenVersion` because *"nothing here reaches
`evalSurface`"* (`channel.h:26-34`).

`docs/lessons.md` records why, and it is the sharpest lesson in the file:
*"'Avoided the cost' and 'the feature does not work' were the same fact… The
version bump was not an obstacle to the feature; paying it was the feature."*

### F4 — No water datum reaches the client

Fine tiles carry exactly two planes. `tile_codec.py:144-149`: four section ids
(`ELEV_INDEX`, `ELEV_DATA`, `FLOW_INDEX`, `FLOW_DATA`) and one flag bit
(`FLAG_FLOW_PRESENT`). C++ mirror at `tilestore.h:111-149`. `reserved u8[3]`
must be zero and unknown flag bits are rejected (`tilestore.h:146-149`);
trailing bytes are rejected too (`tile_codec.py:712-713`), so no sidecar can be
smuggled in. `BakeResult` (`pipeline.py:2368-2398`) has no water field.

The flow plane survives but is lossy and unread: bits 0-4 are
`log2(catchment m²)` clamped 0–31, zeroed below 1e4 m²
(`pipeline.py:2108-2115`). `docs/w3-channel-carving.md:211-215` — the flow plane
*"already exists and is shipped on every production tile … and today has zero
consumers."* And `ITileSampler` (`tiles.h:41-59`) cannot even express it: the
interface is `pixelSizeMm`, `elevationMm`, `climate`. There is no `flowAt`.

The client's only way to learn a water surface elevation is
`FindFloodedCavernNear` (`VoxelWaterSubsystem.cpp:4267-4366`), which **scans the
runtime CA's own fill** — it discovers a surface rather than reading one.

### F5 — Ocean: handled cosmetically, not verified as gameplay water

The owner's note that this is unverified was right to be cautious. It works as
a picture and not as water:

- Sea level is **not a constant**. It is the literal `0` repeated across at
  least 14 sites in 4 languages, with no shared symbol: `core.h:238` (a
  comment), `rivernet.h:271` `kRiverSeaLevelMm`, `rivercouple.h:296` (a mutable
  per-instance config field), `caves.h:162`, `VoxelOceanActor.cpp:130` and
  `:159` (two independent `0.0` literals), `VoxelCharacterMovement.cpp:602`,
  `VoxelClipmapActor.cpp:792`, plus three copies of the sea-taper pair in
  Python (`incise.py:152-153`, `pipeline.py:572-573`, `pipeline.py:2031-2046`).
- The values all agree today. The one genuine disagreement is
  `tools/world_map.py:325`, whose default waterline is `beach_lower_m` =
  **−3.0 m**, so every published world map is drawn at a different sea level
  than the game's.
- Underwater state is `CameraZ < 0.0` with **no terrain test**
  (`VoxelOceanActor.cpp:133-159`). Standing in a dry cavern 200 m below sea
  level triggers underwater fog and tint. Swimming has the same shape
  (`VoxelCharacterMovement.cpp:602`).
- The plane is 40 km and follows the camera, so **any pit dug below z=0
  anywhere inland renders as full of ocean.**
- `SetCollisionEnabled(NoCollision)`. There is no buoyancy, no current, no
  force field — plan §3.7's force field is unbuilt.

---

## 4. Deterministic content vs mutable state — testing the split

The organising idea is right and it is already how terrain vs. player edits
works. Tested against the code, the owner's four rows survive with two
corrections and one addition.

| water | verdict | correction |
|---|---|---|
| **ocean = global constant** | **AGREE** | The model is right; the implementation is 14 unnamed literals and a cosmetic plane. Make it one symbol before anything else depends on it. |
| **lake surfaces = baked datum** | **AGREE, wrong reason** | The bake's fill does not create basins, it destroys them (F1). This is new bake work plus a tile-format section, not serialization of an existing product. The *raster* is one deleted line away (`pipeline.py:3058`); the *decision* (lake vs playa) and the *format* are not. |
| **river bed + discharge = baked** | **AGREE** | `channel.h` is ready and unwired. Costs `kWorldGenVersion` 10→11, a golden re-pin and an HLSL mirror change, in lockstep. There is no free route — "free" is exactly what not wiring it bought. |
| **flowing / disturbed water = runtime CA** | **AGREE** | Unchanged. This half is built and works. |

**The addition the split is missing: the handoff.** A baked datum has to become
mutable state when a player touches it, and go back when it settles. That
machinery **already exists, is proven, and is shipping** — it is
`WaterMobilizer` (`waterca.h:951`), built for cavern lakes. Its contract is
exactly what a baked lake needs: a zero-storage implicit field owns a cell until
a brick mobilizes, at which point the CA takes total ownership, and the
ownership partition makes double-counting structurally impossible
(`waterca.cpp:1195,1209`).

> **This is the leverage in the whole plan.** A baked lake datum has the same
> shape as `floodZMm`: a per-column water-surface elevation that is a pure
> function of world data. Feeding `WaterMobilizer` a *hydrologically derived*
> level instead of a hashed one reuses a tested, persisted, replicated,
> already-optimised path. Ocean is the same shape again with the level pinned
> to 0. **Do not build a second mechanism for lakes.**

Two consequences worth stating plainly, because they are the economic argument
for maximising the baked share:

1. **Content costs zero bytes on disk and zero bytes on the wire.** An untouched
   lake stores nothing and replicates nothing, exactly as an untouched cavern
   lake does today.
2. **Content costs zero ticks.** It is the difference between a world with a few
   thousand active bricks and a world with millions. See §7.

---

## 5. What the end state looks like

For a player standing anywhere in the world:

- The sea is at z=0, swimmable, with buoyancy and a shoreline that agrees with
  the terrain instead of clipping through it.
- Valleys hold rivers that are continuous from headwater to coast, sitting in a
  carved bed at `waterLineMm`, with width and depth set by discharge.
- Depressions that climate says should hold water hold it, at a surface
  elevation the bake decided, identical on every client, at zero streaming cost.
- Depressions that climate says should not are salt flats and playas.
- Digging a channel out of a lake drains it, and it stays drained across
  reloads and for every other player.
- Damming a river raises water behind the dam and dries the bed below it.
- All of the above is the same on a dedicated server, a listen server, and
  offline.

---

## 6. Multiplayer

**Read this first: the architecture doc and the shipped code disagree, and the
code is currently the more conservative of the two.**

`docs/world-generation-architecture.md` §5 says water edits replicate like
terrain edits — same seed, same edits, same order, every client's CA
independently produces the same flood, *"the server's job is ordering, not
simulation."* That is not what ships. `VoxelWaterSubsystem.h:18-23` and
`:321-333`: the authority ticks the CA and **`NM_Client` never steps its own
CA**, mirroring server state through `AVoxelEditRelay::MulticastWaterDiffs`
(`VoxelEditRelay.cpp:97-111`) applied via `WaterCA::setReplicatedFill`
(`VoxelWaterSubsystem.cpp:4523`). That matches plan §3.7 Layer B ("replicate
compressed fill-diffs for active regions"), not architecture §5.

This needs a decision, not a doc edit. The evidence favours **keeping fill-diff
replication and shrinking what it has to carry**:

- **Bit-determinism makes client-side derivation possible but not sufficient.**
  The CA's solidity comes from the amplifier, so any client whose terrain
  differs diverges. `VoxelFineTileStreamer.h:31-32` already flags the concrete
  hazard: a non-resident tile answers elevation 0, and 0 is sea level. The
  superblock completeness gate (now enforced, `60413cd`) is a necessary
  condition for this, not a sufficient one.
- **Join-in-progress cannot be solved by replay.** ADR-0005 established that CA
  water is irreducible state: reproducing it needs every tick since world
  creation, unbounded, before the first frame. A late joiner therefore needs a
  snapshot regardless of how edits replicate. The ADR-0005 blob is that
  snapshot — which is a second reason to finish its UE-side hook.
- **SWE is not deterministic and knows it.** It is float, and it is refused on
  any net mode except `NM_Standalone` (`VoxelWaterSubsystem.cpp:2921-2936`).
  Momentum water is single-player-only today. If it is to ship in multiplayer it
  must stay non-authoritative and client-reconstructible, exactly as plan §3.7
  Layer C specifies.

**So the multiplayer requirement is: maximise the deterministic-content share.**
Ocean, baked lakes and baked riverbeds replicate as *nothing at all* — every
client derives them from tiles it already has. Only the mobilized set — water a
player has actually disturbed — needs the wire. That keeps the replicated set
bounded by player activity rather than by world size, which is the same property
that makes the edit log work for terrain.

**What must additionally be true:**

- Hydrology graph diffs (`kSetConveyance`, `kDivertChannel`) must go **in the
  edit log**, not a side channel. `rivernet.h:130-145` already designed them to
  replay byte-identically against a freshly built network, and doctrine §4 says
  one authority path for all world changes. Today the graph is built per-client
  around an arming anchor and is not persisted at all
  (`VoxelWaterSubsystem.cpp:3385`).
- The baked lake datum must be part of the tile identity
  (`fine_provider_id`), so a client with a different lake table is caught by the
  existing content-addressing check rather than desyncing quietly.
- Offline single-player needs no new work here: authority == standalone
  already, and the whole stack is engine-free and integer-only below the SWE
  layer.

---

## 7. Performance

Read `docs/backlog.md` §0 and the frame anatomy before planning any of this.
The two facts that decide the design:

**The render thread and GPU are the bottleneck; the game thread is idle 75% of
the frame.** At 2560×1440: render thread 13.5–13.7 ms *is* the frame, GPU
16.8–18.4 ms, game thread 3.0–3.6 ms working and 10.1–10.7 ms waiting. Water is
a *visual* addition, so it lands on the full side.

**Consequences, in order of how much they should shape the design:**

1. **Water surfaces must not be voxels at range.** `RefreshImplicitWater`
   (`VoxelWaterSubsystem.cpp:2314`) meshes the implicit field directly through
   `meshBrick<8>` at a 32-brick (~25 m) radius with a 192-brick/tick budget, and
   it runs **every Tick, unconditionally, including on clients** (`:3707`). That
   is right for a cavern lake you are standing in. Scaling it to every lake and
   the ocean to the horizon is the wrong shape and would be the second full pass
   over the world the owner is right to fear. Baked water surfaces should render
   as a **heightfield in bands 2–4** (the clipmap already draws the ground
   there) and voxelise only inside R0, exactly as terrain does. Budget it like
   terrain: capped per frame, priority by screen-space error.
2. **Translucency is the expensive part, and it is already paid.** The water
   pool is translucent, two-sided, with 4–8 sort buckets and depth-varying
   opacity. More water surface = more overdraw on the thread that is already
   full. This is the argument for surfaces over voxels stated in GPU terms.
3. **The CA step is game-thread and lands in one frame.** Fixed 10 Hz
   accumulator, `FixedStepSeconds = 0.1f` (`VoxelWaterSubsystem.cpp:236`), up to
   4 catch-up steps per frame (`:3678-3683`). The game thread has ~10 ms of
   headroom *per frame*, but a tick does not spread: ADR-0003 measured a settled
   127² lake at 6.5 ms/tick with the solid memo on, and the large pour at
   ~30 ms/tick after the 4.7× optimisation. A 30 ms tick inside a 15.4 ms frame
   is a guaranteed hitch every 100 ms. **Before increasing the active-brick
   count — which lakes-as-CA would do by orders of magnitude — either budget the
   step across frames or move it off-thread.** The baked-datum split is the
   cheaper answer: content does not tick.
4. **Measure the frame, not the component.** Seven predictions were falsified in
   one day by exactly this mistake. Work removed from a subsystem does not
   reliably become frame time. No predicted saving goes on the plan until an
   experiment removes the work and measures the frame. Use the
   `VoxelPerfRun post-warmup` line, not `Hitch frame:`.
5. **State the worldgen version beside any water figure.** Absolute numbers
   expire when `kWorldGenVersion` moves, and several items below move it.

---

## 8. Player edits

Who is authoritative, and what happens downstream.

| edit | today | needed |
|---|---|---|
| **Mine below sea level** | Works. `NotifyTerrainVoxelsCleared` breaches the implicit ocean and the reservoir tops it up every step. Authority-only, shipping. | Nothing structural. Extend the same hook to baked lake datums — the mobilizer already does this for caverns. |
| **Mine above sea level** | Nothing happens, because there is no water to inrush. | Falls out of the lake/river datums for free. |
| **Place water** | `SpawnWaterAt`, funnelled and authority-checked. But it is bound to the `1` key in normal play at 200000 fill units. | Make it a real item with a real budget; keep the funnel. |
| **Dam a river** | `setConveyance(0)` already gives upstream stage rise and downstream decay with no special-casing (`rivernet.h:88-111`). **Nothing connects a player's voxel dam to a segment's conveyance**, and the graph is per-client, 1.44 km, and unpersisted. | A detector from CA/terrain state to `kSetConveyance`, the diff into the edit log, and a persisted world-scale graph. |
| **Re-route a river** | `kDivertChannel` → `promoteChannel` is real and replays byte-identically; the diff carries the course, not a re-derivation. Reached only with `voxel.Water.Rivers` on. | Wire the sustained-flux detector, put the diff in the edit log, default the cvar on once F2/F3 land. |
| **Drain a cavern lake** | Works in-session. **Lost on reload** — the ADR-0005 UE-side hook is the open item, and without it the implicit field refills the cavern. | Finish the hook. Decide the missing/stale/refused blob policy. |

The invariant to hold: **every world change is an edit-log entry, ordered by one
authority** (doctrine §4). Water diffs are a separate multicast today, which is
defensible because they are solver state rather than world changes; hydrology
graph diffs are *not* — they are world changes and belong in the log.

---

## 9. The work, cheapest first

Each item says what it unblocks. Items 0–2 are cheap and mostly measurement;
they exist so that the expensive items are spent well.

### 0. One sea-level symbol — hours, no behaviour change
Replace the 14 literal `0`s with a single shared constant, mirrored into HLSL by
the existing `dump_biome_constants` mechanism rather than hand-pasted. Fix
`world_map.py:325` so published maps are drawn at the game's waterline, not
−3.0 m. Fix the dead `$Rivers` switch in `tools/water-playtest.ps1` (referenced
at `:113`, never declared in the `param()` block at `:31-35`, so `-Rivers` has
never worked).
**Unblocks:** every later item that reasons about sea level; makes the ocean's
datum reviewable at all.

### 1. Keep `basin_depth` — a day, no format change
Stop deleting the raster at `pipeline.py:3058`. Emit the basin mask and fill
level to the debug `.npz` and richer per-tile stats. Render a lakes overlay
alongside the existing standard world-generation deliverables.
**Unblocks:** every sizing decision below — how much of the world is lake, where,
how deep, and therefore what a tile section costs. Currently unknown beyond one
exemplar's 2.2%.

### 2. Fix the underwater and swim tests — a day
Test against actual water (CA fill, implicit field, or a baked datum), not
`CameraZ < 0`. Stop the ocean plane showing through inland pits below z=0.
**Unblocks:** honest playtesting of everything else. Today a dry cavern below sea
level reads as underwater, which will contaminate every visual judgement made
about water.

### 3. **The drainage bar on the amplifier detail band — the pivotal item**
Constrain detail so it cannot reverse the carrier's downhill direction, with a
floor that preserves decimetre roughness on flat ground. The bar is already
stated and the measurement already exists (`109f959`); no fix was applied.
Costs `kWorldGenVersion` 10→11, an HLSL mirror change in lockstep, and a golden
re-pin. **Coordinate before entering `amplifier.cpp` / `worldgen.ush`** — the
backlog records a permanently unmergeable branch (`origin/claude/erosion-v7`)
created by exactly this collision, including binary `.spv` conflicts.
**Unblocks:** literally everything else. Until 87.9% of a mountainside stops
being closed basins at voxel scale, no river flows, no poured water runs
downhill, and no lake has a defined shoreline. **Verify with the routing metric
(interior sinks / stranded area / mean flow path), not with area statistics —
mean slope and geomorphon fractions both passed v13 and cannot see connectivity.**

### 4. Ship the lake datum
Three parts, in order:
1. **Bake:** decide which filled depressions are lakes (the precip-vs-evaporation
   gate from plan §3.7 — the climate channels are already on the tile; dry
   basins become playas/salt flats) and **stop levelling those basins into
   rock**. Routing continues to run internally on the filled surface, which is
   what `stream_power` tapered against and what the flow plane agrees with cell
   for cell (`pipeline.py:3035`); only the *output* surface keeps the hole. Costs
   a `BAKE_VERSION` bump.
2. **Format:** a new fine-tile section id + flag bit for the lake surface plane.
   The format is designed for this — section ids plus a flags word, with unknown
   flags rejected — so it is a clean additive change. Include it in
   `fine_provider_id`.
3. **Client:** extend `ITileSampler` (it has no `flowAt` and no water accessor
   today), and bind the lake level as a **second `WaterMobilizer::ImplicitFn`**.
   Do not write a new mechanism.
**Unblocks:** lakes; the zero-storage/zero-bandwidth economy; the whole
"disturb it and it becomes real, settle it and it goes back" loop, reusing a
path that is already persisted and optimised.

### 5. Wire `ChannelField` into worldgen
Carve the bed and carry `waterLineMm` into the amplifier's output. Costs
`kWorldGenVersion` again (combine with item 3 if the schedule allows — one bump,
one re-pin), plus the HLSL mirror. Then `voxel.Water.Rivers` produces continuous
rivers instead of puddles, and the ocean-as-sink path in `rivercouple` becomes
meaningful.
**Unblocks:** rivers; dam and divert gameplay having something to act on.
**Do this after item 3**, not before: a carved channel on ground whose detail
band strands its own tributaries still will not flow.

### 6. Finish ADR-0005's UE-side hook
Write the water blob alongside the edit log; feed it back on load; decide the
missing/stale/refused policy. **Unblocks:** drained caverns and drained lakes
surviving a reload — the single most satisfying thing a player can do to water is
currently the only thing the save refuses to remember. Also becomes the
join-in-progress snapshot in §6.

### 7. Hydrology graph into the edit log, and to world scale
Persist the graph, build it at world scale rather than per-client around an
arming anchor, wire player voxel dams to `setConveyance`, and put both diff
kinds in the edit log.
**Unblocks:** damming and re-routing as real, ordered, replicated, replayable
gameplay; multiplayer river behaviour.

### 8. Water as a gameplay force
Buoyancy and currents from plan §3.7's force field: an authoritative coarse grid
derived from CA/SWE, replicated ~5 Hz, with character controllers and debris
consuming only that. Ocean collision. **Unblocks:** swimming that means
something, debris, and boats later.

### 9. Perf gates, taken as their own item
Budget or off-thread the CA step before the active-brick count grows; render
baked water as heightfield in bands 2–4 and voxels only in R0. **Measure the
frame.**

### 10. SWE beyond standalone, and W5 polish
Only after the breach test the design doc names as its own prerequisite:
*"Tuning damping/absorption toward pooling before running that test would bury a
possible bed-seating bug rather than fix it."* Then foam, caustics, particles.

---

## 10. The unmerged water branches

Checked because a lot of work looked like it might be stranded. **It is not, and
nothing here should be merged.**

There are 14 `claude/water-*` branches. **Ten are 0 commits ahead of
`origin/main`** — already merged: `water-ca-perf`, `water-ca-perf-int`,
`water-p5-sortkey-tint`, `water-pool-integrate`, `water-rendering-phases`,
`water-session-lessons`, `water-spawnz-wip`, `water-surface-pool`,
`water-swe-breach`, `water-w5-integration`.

The four with unmerged commits are **one branch, not four**:
`water-p2-smooth-surface`, `water-p3-material-motion` and `water-p4-swe-momentum`
are each **fully contained in** `water-system-physics-review-cyn8jz`
(verified with `git merge-base --is-ancestor`). Their four "ahead" commits are
the same shared prefix.

And `cyn8jz` is **stale and superseded**, not unfinished:

- It is **214 commits behind** `origin/main` and only 20 ahead.
- `git diff origin/main cyn8jz --stat` = **1,895 insertions against 79,249
  deletions**. Relative to main it *deletes* `rivercouple.cpp`, `channel.cpp`,
  `test_channel.cpp`, `test_rivercouple.cpp` and much else.
- Its `VoxelWaterSubsystem.cpp` is 3,657 lines; main's is **4,795**.
- Its copy of `docs/gpu-water-pool-design.md` still describes constant 0.55
  opacity and a single sort key; main's already carries the W5 correction
  (0.18–0.95 by depth/foam, 4–8 sort buckets).
- Every file it once uniquely held — `VoxelWalkTestSubsystem`,
  `tools/voxel-measure-guard.ps1`, `tools/water-playtest.ps1` — is on main, and
  main's `water-playtest.ps1` is the richer version.
- A dry-run merge conflicts in seven files including a **binary** conflict in
  `M_WaterVoxel.uasset`, because main's material moved on afterwards.

**Verdict: the work landed by other routes. Merging `cyn8jz` would be a
regression.** Its lasting value is documentary and already extracted — the
playtest verdict (CA pooling reads as more correct than SWE spreading, recorded
as a preference and explicitly not yet tuned in) and the `-VoxelWaterParityTest`
anchor are both on main. The branches can be deleted after a final read.

---

## 11. Summary

The water simulation is not the gap. **Where water belongs** is the gap, and
under it sits a terrain problem: the bake guarantees the ground has no holes and
the client's detail band then fills the ground with holes too small to drain.
Fix the drainage bar first (item 3), because it is the precondition for every
water feature looking right; then make lakes and riverbeds baked content and let
the existing, tested, persisted `WaterMobilizer` turn them into simulation only
where a player actually touches them.
