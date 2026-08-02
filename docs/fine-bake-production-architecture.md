# Fine-tier baking in production: where it runs, and why

**Decision: bake in the cloud, once per world, and serve the tiles as
content-addressed static assets. Do NOT bake on the player's machine.**

The rest of this document is the argument, because the reason is not the one
people usually reach for. Cloud baking is not chosen here to save the player's
CPU — that is the *third* reason and the weakest. It is chosen because **the
bake is not a per-tile function, and a client cannot know enough to compute a
tile correctly on its own.**

---

## 1. The decisive reason: a tile is not independent of its neighbours

Two measured facts about this bake:

* **The apron is 960 m, and the domain border's influence reaches much
  further.** `APRON_BLIND_SPOT` (`bake/pipeline.py`) measured **1.05% of the
  shipped interior moving past the 100 mm wire LSB, by up to 78.79 m**, with
  border influence reaching **3.8 km inward — four apron widths**. The cause is
  that depression fill is unbounded, so a truncated domain *invents an outlet*.
* **Hydrology is computed at superblock scale, not tile scale.** The flow
  superblock at L1 spans **256 coarse tiles**. Our own bakes logged
  `flow superblock L1 (-1,-1) is INCOMPLETE (102 of 256 coarse tiles absent)`.

Therefore: **the same tile coordinates, baked with different neighbours
present, produce different terrain.** That is not a rounding difference; it is
up to 78.79 m of elevation and a different river network.

Now put that in a multiplayer game where state is saved:

* Player A is the first to reach a frontier region. Their machine bakes tile T
  with most neighbours missing. They get terrain **X**. They build a structure
  on it.
* Player B arrives a month later, when the surrounding world has been baked.
  Their machine computes tile T with full context and gets terrain **Y**.
* A's building is now floating, or buried. The river A built a mill on does not
  exist in B's world.

You can patch this with "first bake wins, then freeze," but the price is that
world quality becomes a permanent function of *who wandered where first*, and
rivers stop joining up across the seams of exploration history. That is a
worse world, forever, in exchange for nothing.

**A server bakes with complete context because it can see the whole world. A
client cannot, by construction.**

## 2. Determinism is already an architectural commitment; local baking breaks it

The fine-tier residency gate is **fatal by design**: when a fine tile is absent
the client refuses rather than answering from the coarse tier, because
answering a fine-tier query with sea level would *fabricate terrain no other
client computes*. `DefaultGame.ini` says so explicitly, and it has already
killed capture runs rather than let them lie.

That is the multiplayer-correctness stance, already taken. Local baking
reintroduces exactly the divergence that gate exists to prevent, one layer
down, through three doors:

* **partial context** (§1),
* **`BAKE_VERSION` skew** — a player on an older client bakes with older
  constants and gets different ground,
* **constants skew** — the bake is parameterised by `CONSTANTS`, and a
  per-cell province field multiplies into it.

The bake is integer-only and deterministic, so *given identical inputs* two
clients agree. Guaranteeing identical inputs is the entire problem, and a
server gives it away for free.

## 3. Only third: cost and latency

Measured this session, on 6 physical cores:

    per fine tile      117-143 s CPU,  ~6.9 GiB peak commit,  180-192 MB output
    per tile footprint 15.36 x 15.36 km at 1.875 m/px (8192^2)
    289-tile world     ~21.7 h CPU,  ~58 GB

A player crossing open ground needs tiles ready *minutes* before they arrive.
Local baking would either stall the session or pop terrain in late, and it
would spend two minutes of six-core CPU per tile while the player is trying to
render the game.

Cloud-side, the same tile is baked **once per `(seed, BAKE_VERSION)` and served
to every player forever.** The marginal cost of the Nth player is a CDN hit.

---

## The architecture this implies

Not "bake on demand per player." **Bake on demand per world region, globally,
once.**

Most of the machinery already exists:

| piece | status |
|---|---|
| `fine_provider_id` = coarse id + `BAKE_VERSION` + bake fingerprint | **built** — tiles are already content-addressed, so the id is a cache key and identical ids guarantee identical bytes |
| client LRU cache keyed `<provider_id>/<seed:016x>/s16/<x>_<y>.vxtl` | **built** |
| client-side identity validation, counted as `identityMismatch` | **built** — foreign tiles are refused, never silently used |
| speculative prefetch ahead of the player (T4-1) | **built** at chunk scale; the same idea lifts to tile scale |
| edit log (`.vxlog`) composing player changes over baked terrain | **built** — player state and world content are already separate layers |

What is missing is the service around them:

1. **Bake service.** An idempotent job queue keyed `(provider_id, seed,
   tile_x, tile_y)`. Workers are the existing `terrain_service.pregen`. Output
   goes to object storage under the path the client already expects.
2. **Superblock-aligned batching.** Never bake a lone tile. §1 forces this:
   schedule work in units that carry complete apron and hydrology context, and
   refuse to publish a tile whose superblock is incomplete. The
   `INCOMPLETE (102 of 256 coarse tiles absent)` warning should become a
   **publish gate**, not a log line.
3. **CDN + a 202 path.** Client requests a tile; gets `200` from cache, or
   `202 Accepted` with a retry hint while a bake is queued. The client already
   knows how to wait and how to refuse.
4. **Frontier pre-baking.** Pre-bake the spawn region and a generous margin at
   world creation, then expand the frontier ahead of the *player population*,
   not per player. Exploration is heavily correlated between players; the
   frontier is a shared asset.
5. **Client keeps its local disk cache.** Already built. A returning player
   re-downloads nothing.

## Two things to fix before mass-baking anything

**1. Tiles are uncompressed, and this build cannot decode compressed ones.**
A baked tile inspected today: `VXTL` v2, **190.4 MB, CODEC_RAW**. Meanwhile the
UE build reports:

    VoxelEarth: no zstd module found ... Building without CODEC_ZSTD support:
    a CODEC_ZSTD tile will be refused with FineError::kNoDecompressor

`ue-project/Source/ThirdParty/zstd` does not exist and is not tracked in git.
So today the client can *only* consume raw tiles, at ~190 MB each. Serving that
is 58 GB for a 261 km world and far more for a real one. **Compression is not
an optimisation here, it is a precondition for serving**, and the host boundary
is not currently wired even though the task tracking says it is. Verify what
the compression ratio actually is before sizing anything.

**2. The L1 hydrology gap is being baked in.** `102 of 256 coarse tiles absent`
means rivers entering from those tiles contribute nothing — permanently, in
every tile baked so far. Fix before spending 21.7 hours and 58 GB baking around
a known defect.

Also worth settling first: **task #24** (an agent measured 69% stranded at the
steepest ladder step). If that is real, it is much cheaper to fix now than to
re-bake the world.

## What this does not decide

* **Whether the world is finite.** A 289-tile world can be fully pre-baked
  overnight and the queue never used in anger. An effectively unbounded world
  needs the frontier machinery for real. That is a game-design decision, not a
  technical one, and it changes how much of the above is worth building now.
* **Whether players can terraform below the fine tier.** The edit log composes
  over baked terrain today; large-scale terrain edits that would change
  hydrology are a different problem.
