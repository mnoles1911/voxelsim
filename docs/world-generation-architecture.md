# World generation architecture: multiplayer first, offline-capable

**The game must support both online multiplayer and offline single-player.**
Multiplayer is built first. In multiplayer, work moves to the server wherever
that raises client frame rate. In single-player there is no server, so the
client must be *capable* of everything — which means the expensive paths are
written once, as a library, and invoked from two different places.

This document supersedes `fine-bake-production-architecture.md`, which framed
the question as "server or client" and got the emphasis wrong. That is not the
question. The client must be capable regardless; the server exists to
**accelerate and arbitrate**.

---

## 1. The units, and why there are four of them

| unit | size | what it is | cost |
|---|---|---|---|
| **coarse tile** | 15.36 km, 512² @ 30 m | diffusion-model output: elevation + 4 climate channels | 1.5 MB |
| **flow superblock** | 61 / 246 / 983 km | hydrology context — a **2048² raster at every level** | ~16 MB, one priority-flood |
| **fine tile** | 15.36 km, 8192² @ 1.875 m | the baked control-point heightfield players stand on | 190 MB, ~130 s CPU, ~5 GiB peak |
| **chunk** | client-side | voxels, generated from the fine tile by the amplifier | runtime |

Voxels are never stored or shipped. The fine tile is a **heightfield**; the
client's amplifier turns it into 10 cm voxels on the fly, on CPU or GPU,
bit-identically (`amplifier.cpp` ↔ `worldgen.ush`, guarded by
`kWorldGenVersion`).

## 2. Why the superblock exists, and why it is cheap

**A tile cannot compute its own terrain.** Stream-power incision — the process
that carves every valley — scales with *discharge*, and discharge is the total
catchment upstream of a cell. A river arriving at a tile edge may drain
hundreds of km that lie outside the tile.

Bake a tile in isolation and it sees only the drainage born inside itself. The
river entering from off-tile has zero upstream area, so it gets no incision.

**How much this actually damages the terrain was measured, and the answer is
"almost none" — which is not the argument this doc used to make.** An earlier
version claimed *"you do not get wrong water, you get wrong mountains"*: valleys
fading out at the tile boundary. Baking a tile with and without its injected
inflow moved **zero elevation cells** past the 100 mm wire LSB and changed
**5 of 67M flow cells**.

The superblock earns its place on **determinism**, not on visible relief. Those
5 cells are 5 cells where two clients that baked the same coordinates against
different neighbour sets disagree permanently about the ground — and in
multiplayer that is a correctness failure, not a quality one. The reasoning
above is why you would *expect* large damage; the measurement is what actually
happens, and where they conflict the measurement wins.

So the tile needs catchment context, and the superblock is it: route flow at
*coarse* resolution over a large span, and hand each fine bake the discharge
crossing its boundary.

**This is why it is efficient rather than expensive.** The pyramid keeps the
raster edge constant (`superblock_tiles=4`, edge = 4 × 512 = **2048 px at every
level**) while each level covers 4× the ground per axis:

    L0   4x4 =   16 coarse tiles     61 km span    2048^2 raster
    L1  16x16 =  256 coarse tiles   246 km span    2048^2 raster
    L2  64x64 = 4096 coarse tiles   983 km span    2048^2 raster

Each level is the same priority-flood over ~4 M cells. A fine tile bake is
9216² = **85 M cells** — so an entire pyramid level costs roughly **1/20th of
one fine tile**, and buys correct discharge for up to 4096 tiles.

The alternative — routing flow globally at fine resolution — is impossible in
an infinite world. The superblock is what makes catchment-correct terrain
tractable at all.

**Superblocks already exist and already run** (`pregen` pass 2,
`build_flow_superblock`). Nothing new is being proposed. What changes for
production is the *discipline*: today an incomplete superblock prints a
warning and bakes anyway —

> `flow superblock L1 (-1,-1) is INCOMPLETE (102 of 256 coarse tiles absent).
> Rivers entering from those tiles contribute nothing, permanently, to every
> tile baked against it.`

— and `pipeline.py` is explicit that this never self-heals: *"A tile baked
against an incomplete superblock stays baked that way."*

**In production that warning must become a publish gate.** A fine tile may not
be baked, cached or served until its superblock is complete. Because coarse
tiles are 127× smaller than fine tiles, completing a superblock is cheap:
256 × 1.5 MB = 384 MB of coarse data buys catchment-correct bakes for
246 km of world.

### The consequence: the coarse frontier must lead the fine frontier

The gate is not free at the edge of generated space, and this was measured on
the shipped world rather than reasoned about. The 20260719 world is 17×17 coarse
tiles (−16…0 on both axes). Superblocks are 4-aligned, so a tile can only bake
when its whole 4×4 block exists: **256 of the 289 tiles qualify and 33 do not,
and the 33 are exactly the index-0 row and column.** 19 of them are land. The
lake survey baked precisely those 256 — not by choosing a tidy 16×16, but
because the gate refused the rest.

This is not an artifact of one world's dimensions; it is what the frontier
always looks like. In an infinite world the newest region's outer tiles can
never bake, because the neighbours that would complete their block have not
been generated yet.

So **coarse generation must run at least one full superblock-block ahead of
where fine baking is allowed** — one block, not one tile, and aligned to the
block grid rather than to the player. Sizing it off the player's position
produces a ragged frontier that stalls whenever someone walks toward a corner.
This is cheap for exactly the reason above (coarse tiles are 127× smaller), and
it is the concrete scheduling rule the bake service needs.

A world that wants no permanently-unbakeable edge must also choose dimensions
that are a multiple of the superblock stride. 17 is not.

### The pyramid must be capped, and the cap is visible in-game

An infinite world cannot have an infinite superblock. Choosing a maximum level
chooses **the largest river basin the world can resolve** — cap at L2 and no
catchment larger than ~983 km exists. That is a world-design decision with a
visible consequence, forced by infinity. It is not yet made.

## 3. Multiplayer: push work to the server wherever it buys frame rate

The governing rule, from the owner: *in multiplayer, move work off the client
whenever doing so raises FPS.* Concretely:

**Server owns:**
* **Coarse generation** (the diffusion model — needs a GPU the player may not
  have).
* **Superblock construction**, and the completeness gate. This is the
  correctness-critical step and it must have one authority.
* **Fine baking.** ~130 s CPU and ~5 GiB peak per tile is the single largest
  non-render cost in the system. Baked once per `(seed, BAKE_VERSION)` and
  served to every player who ever visits, it is amortised across the whole
  population; done on the client it is re-paid by every visitor and competes
  with the frame budget.
* **Edit ordering.** The authority on *what happened when* (§5).

**Client owns:**
* **Voxelisation and meshing** from the heightfield — this is already GPU work
  the client must do anyway, and shipping voxels instead would be far more
  data.
* **Rendering, and the water CA tick** (§4).

Tiles are **content-addressed** (`fine_provider_id` = coarse provider id +
`BAKE_VERSION` + bake fingerprint), so identical ids guarantee identical
**decoded planes**.

Not identical *bytes*, and the difference now matters: the identity payload
(`bake_version`, `stage_order`, `geometry`, `constants`, `provinces`) carries
**no codec**, so the same tile stored `CODEC_RAW` and `CODEC_ZSTD` shares one
id and two different files. That is deliberate — it is what lets the codec
default change without orphaning a single baked tile — but anything treating
the id as an ETag or a byte-level checksum must key on `(id, codec)` instead.
The id addresses the world, not the file.
That makes them cacheable, CDN-able, and shareable across every player on the
seed — the marginal cost of the Nth visitor is a cache hit.

### Streaming: the tile is a storage unit, not a transfer unit

190 MB per tile sounds prohibitive and is not, because a tile is **236 km²**
and a player can see a few km. At ~0.8 MB/km² raw, a 2 km view radius is
**~10 MB** of terrain — and ~1.7 MB once compressed (below). The requirement is
not "ship 190 MB", it is "ship the couple of megabytes the player can actually
see", which means **fine tiles must be sliceable into independently addressable
sub-blocks**. That slicing does not exist yet and is the main streaming work
item.

**And compression is already measured — 6.0×.** `pregen._encode_fine` records
it on tile (-5,2): **201.4 MB RAW against 33.4 MB `CODEC_ZSTD`**, with
elevation and flow planes bit-identical on round trip. So the real numbers are:

    per fine tile        ~33 MB compressed  (not 190)
    per km^2             ~0.14 MB
    2 km view radius     ~1.7 MB
    289-tile world       ~9.7 GB            (not 58)

That materially changes the streaming story — a player's visible surroundings
are under two megabytes.

**Both ends can now use it** (task #43 — this paragraph previously said neither
could, and that is out of date):

* `pregen --codec {raw,zstd}` exists, defaulting to `raw`. There is
  deliberately **no `auto`**: a flag that quietly fell back to raw would fill a
  cache with uncompressed tiles its operator believed were compressed.
* The client decodes `CODEC_ZSTD` by binding a zstd at **runtime**, through the
  platform loader, from `Binaries/ThirdParty/zstd/Win64/libzstd.dll`
  (`TryRegisterRuntimeZstd`, fetched by `tools/fetch-zstd.ps1`). The original
  measurement still holds — UE 5.8's binary distribution ships no C/C++ zstd —
  and it is *why* the decoder is injected rather than linked: `ThirdParty/Blosc`
  already statically links a zstd into the same binary, and a second copy of
  those C symbols at a version nobody chose fails as **wrong terrain, not a link
  error**. With no DLL present, a `CODEC_ZSTD` tile is still refused whole with
  `kNoDecompressor` and never decoded as zeros.

**What has not happened is the migration.** Every fine tile in the cache today
is still `CODEC_RAW`, because the flag defaults to raw and nothing has been
re-baked with it. Sizing storage and egress from 33 MB is now a decision to
make, not a blocker to clear.

Client-side caching is already built and already bounded: a **12 GiB LRU**
keyed on `<provider_id>/<seed>/s16/<x>_<y>`, with identity validation that
refuses foreign tiles. At ~0.3 GiB resident per tile that is ~40 tiles ≈
9,400 km² held locally.

## 4. Water: baked vessel, runtime fluid

The bake emits no water voxels, and that is deliberate rather than a gap. The
split follows the same line as terrain vs. edits — **deterministic content vs.
mutable state**:

| | where it lives | why |
|---|---|---|
| **ocean** | global sea-level constant | trivially consistent everywhere |
| **lake surfaces** | *should be* a baked datum | a lake at equilibrium is content, and making the CA rediscover it every session wastes work and invites divergence |
| **river bed + discharge** | baked | `channel.h` already carries `waterLineMm` with banks constrained to contain it |
| **flowing / disturbed water** | runtime CA | genuinely mutable state |

`waterca.h` is a volume-conserving **bit-deterministic integer** CA with its
own `kWaterCAVersion`, treated as world-breaking exactly like worldgen. That
determinism is what makes multiplayer water tractable (§5). `swe.h`,
`rivernet.h` and `rivercouple.h` complete the runtime side.

### AUDITED 2026-08-03 — three corrections, see `water-production-plan.md`

**Nothing places water hydrologically at runtime.** There are three placement
paths and none is hydrology: cavern flood levels (a hash of the seed,
underground, 40% dry), an ocean breach that only fires when a player digs into
it (`NotifyTerrainVoxelsCleared` skips anything at `Z >= 0`, so the sea is not
water until then), and developer pours. Surface lakes are implemented nowhere.
`ChannelField` has **zero references** outside its own tests and bench.

**The depression fill DESTROYS basins, it does not create them.** The sentence
that used to sit in the lake row above was backwards. `fill_depressions` is
drainage *enforcement* — it levels basins into rock, and B4b re-runs it as the
last step before the codec, because *"'The carrier drains' is a CONTRACT
(0 sinks)"* (`pipeline.py:2931`). The bake ships a **pit-free heightfield by
contract**, so there is nowhere for a lake to sit. The conclusion (lakes should
be baked content) survives; the work is larger than serialising something that
already exists. The raster is one deleted line away (`pipeline.py:3058`,
`basin_depth` — 135 m deep over 2.2% of the wet exemplar); the decision and the
format are not.

**Sea level is not a constant anywhere.** It is 14 unnamed `0` literals across
four languages, and `world_map.py:325` draws published maps at −3.0 m instead.
The underwater test is `CameraZ < 0` with no terrain check, so a dry cavern
below sea level reads as underwater.

## 5. Edits: seed plus delta, for terrain *and* water

Terrain is deterministic content every client can derive; **edits are
server-authoritative state**. A player mines a block, dams a river, or places
water; the edit is recorded in the log (`.vxlog`) and broadcast. No terrain is
ever re-shipped — the delta is kilobytes against 190 MB.

This is the Minecraft / Space Engineers model and it is the right one.

**CORRECTED 2026-08-03.** This section used to claim water edits replicate the
same way *because* the CA is bit-deterministic — same terrain, same edits, same
order ⇒ the same water everywhere, with the server merely ordering. That is
true in principle and **is not what ships.** `NM_Client` never steps its own
CA; it mirrors server fill-diffs via `MulticastWaterDiffs` →
`setReplicatedFill`. The server simulates; the client displays.

Keep it that way. ADR-0005 proves water cannot be replayed in bounded time, so
a late joiner needs a snapshot regardless — which makes deterministic replay
insufficient on its own. The work is to *shrink what the diffs carry*, and that
is the strongest argument for maximising the baked-content share: content does
not tick and does not replicate.

The dependency worth naming: this holds only while every client has *identical
terrain underneath*. A client that baked against an incomplete superblock would
eventually desync its water too — which is why §2's completeness gate is a
multiplayer-correctness requirement, not a quality nicety.

## 6. Offline single-player: the same library, no server

No server means the client must generate coarse tiles, build superblocks, bake
fine tiles, and own its own edit log. The bake is already engine-free,
integer-only and deterministic, so this is a packaging problem rather than a
rewrite — but two costs land on the player's machine:

**Peak memory, measured today.** Peak is ~64 bytes per padded cell, so it
scales with the *area* of the bake unit — and the unit is a design parameter:

| coarse_px | tile | peak est. | apron % | CPU for same ground |
|---|---|---|---|---|
| **512** (production) | 15.36 km | **5.06 GiB** | 21% | 1.0× |
| **256** | 7.68 km | **1.56 GiB** | 36% | 4.8× |
| 128 | 3.84 km | 0.56 GiB | 56% | 28× |

5 GiB is not shippable to consoles or low-end PCs; ~1.6 GiB is. **But shrinking
the unit trades a memory problem for a correctness problem**: `APRON_BLIND_SPOT`
measured border influence reaching **3.8 km inward**, which at a 7.68 km tile
would contaminate half of every tile. The right fix is bounding the depression
fill so the apron works, *then* shrinking the unit — not shrinking first.

Two loose threads found while measuring: `estimate_peak_bytes` now reports
5.06 GiB against a docstring claiming 6.33 against a measured 6.90 — the Wave 0
dtype work moved it and **the real peak needs re-taking**. And there is a
documented, unfixed **1.27 GiB of ballast** (B1's `gy`/`gx`/`slope`/`delta`
never dropped), whose three-line fix the docstring says "cannot move a baked
byte." It was deferred because three sessions were editing the file; those
worktrees are now consolidated, so it is unblocked.

**CPU is schedulable.** ~130 s per tile sounds fatal beside a frame budget, but
a tile is 15.36 km — a player crossing at 20 m/s has ~13 minutes of lead time.
Background-thread the bake at low priority and it is a fraction of one core.
The constraint is memory, not CPU.

## 7. What is shared and what is mode-specific

**Shared:** the bake library, the amplifier, the water CA, the tile format and
codec, the edit-log format, the client cache and residency gate.

**Multiplayer-specific:** the bake service and job queue, the superblock
completeness gate as a *publish* gate, CDN and sub-block streaming, edit
ordering and broadcast, frontier pre-baking driven by the player population
rather than per player.

**Single-player-specific:** in-process scheduling of coarse generation,
superblock construction and baking; a local edit log with no ordering
authority; and a bake unit sized for consumer memory.

The risk to manage is **drift between the two paths**. The existing defence is
content addressing: if single-player and multiplayer produce different bytes
for the same `fine_provider_id`, that is a bug the identity check will catch
rather than a mystery. Keep it that way — never let the single-player path
"optimise" into a different result.

## 8. Build order

1. **Superblock completeness gate** — refuse to publish a fine tile whose
   superblock is incomplete. Correctness-critical, cheap, and it blocks
   everything else being trustworthy.
2. **Fine-tile sub-block slicing** — the actual streaming unit. Without it the
   190 MB tile is the transfer granularity and nothing else matters.
3. **zstd** (#43) — the ratio is already measured at 6.0×; what is missing is a
   `--codec` flag on `pregen` and a vendored zstd for the client.
4. **Bake service + queue + CDN**, superblock-aligned.
5. **Edit ordering and broadcast** over the existing log.
6. **Bound the depression fill**, then re-evaluate a smaller bake unit for
   single-player.
7. **Cap the hydrology pyramid** — a world-design decision (§2).

Not on this list, and deliberately: mass-baking the world. Defects baked in now
are permanent, because a shipped tile is never regenerated.

**Corrected 2026-08-03.** This previously blocked on "task #24's coarse-tier
drainage regression, 69% stranded at the steepest bin". That figure is **stale
and was misframed**, and re-measuring inverted it:

* The coarse row reads **24.2% at v22**, not 69%. v21's micro-cap reduction
  already fixed it; the 69% was real at v17/v18 only.
* The "4× regression" was a **labelling artifact** —
  `tools/drainage-ladder.ps1` labels rows by whole-tile p50 grade while probing
  an off-centre 384 m quadrant. Re-sorted by the grade actually probed, both
  ladders are monotone and 69% is not an outlier; v14's 17.5% was the anomaly.
* **The live defect is on the FINE tier, not the coarse one** (task #47).
  Median added stranding across paired windows: v14 0.3%, v18 0.0%,
  **v22 11.3%** (mean 16.0%). The step is at v19 — letting the micro pool
  exceed the carrier gradient — not at v17.

So the blockers before mass-baking are: the **fine-tier micro cap** (#47), the
**L1 hydrology gap**, and **world identity reproducibility** (#46), without
which a mass-baked world cannot be extended later anyway.

And never quote a single ladder row: on one tile at v22 the amplified stranded
figure ranges 1.3% to 93.2% across windows. Report the row, its carrier, and
the spread.
