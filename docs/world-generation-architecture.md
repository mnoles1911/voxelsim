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
river entering from off-tile has zero upstream area, so it gets no incision:
**you do not get wrong water, you get wrong mountains** — a valley that fades
out at the tile boundary, or never forms.

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
`BAKE_VERSION` + bake fingerprint), so identical ids guarantee identical bytes.
That makes them cacheable, CDN-able, and shareable across every player on the
seed — the marginal cost of the Nth visitor is a cache hit.

### Streaming: the tile is a storage unit, not a transfer unit

190 MB per tile sounds prohibitive and is not, because a tile is **236 km²**
and a player can see a few km. At ~0.8 MB/km², a 2 km view radius is **~10 MB**
of terrain. The requirement is not "ship 190 MB", it is "ship the ~10 MB the
player can actually see", which means **fine tiles must be sliceable into
independently addressable sub-blocks**. That slicing does not exist yet and is
the main streaming work item.

Compression compounds it: heightfields delta-compress well, but the client
**cannot decode compressed tiles today** — `ThirdParty/zstd` is absent, so
`CODEC_ZSTD` tiles are refused with `kNoDecompressor` (task #43). Measure the
real ratio before sizing storage or egress; do not extrapolate from 190 MB.

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
| **lake surfaces** | *should be* a baked datum | the bake's depression fill is what creates the basin; a lake at equilibrium is content, and making the CA rediscover it every session wastes work and invites divergence |
| **river bed + discharge** | baked | `channel.h` already carries `waterLineMm` with banks constrained to contain it |
| **flowing / disturbed water** | runtime CA | genuinely mutable state |

`waterca.h` is a volume-conserving **bit-deterministic integer** CA with its
own `kWaterCAVersion`, treated as world-breaking exactly like worldgen. That
determinism is what makes multiplayer water tractable (§5). `swe.h`,
`rivernet.h` and `rivercouple.h` complete the runtime side.

**Open:** ocean handling has not been verified end-to-end, and lake surfaces
are not currently baked datums. Both need checking before this section can be
called settled.

## 5. Edits: seed plus delta, for terrain *and* water

Terrain is deterministic content every client can derive; **edits are
server-authoritative state**. A player mines a block, dams a river, or places
water; the edit is recorded in the log (`.vxlog`) and broadcast. No terrain is
ever re-shipped — the delta is kilobytes against 190 MB.

This is the Minecraft / Space Engineers model and it is the right one.

Water edits work the same way *because the CA is bit-deterministic*: same
terrain, same initial state, same edits in the same order ⇒ the same water on
every client. Dam a river and every client's CA independently produces the same
flood. **The server's job is ordering, not simulation.**

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
3. **zstd** (#43) — and *measure* the ratio.
4. **Bake service + queue + CDN**, superblock-aligned.
5. **Edit ordering and broadcast** over the existing log.
6. **Bound the depression fill**, then re-evaluate a smaller bake unit for
   single-player.
7. **Cap the hydrology pyramid** — a world-design decision (§2).

Not on this list, and deliberately: mass-baking the world. Two known defects
would be baked in permanently — the L1 hydrology gap above, and task #24's
coarse-tier drainage regression (69% stranded at the steepest bin against a
17.5% baseline). Fix before spending the storage.
