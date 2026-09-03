# ADR-0010: Two lattice jurisdictions — the world ladder, and per-entity pitch

- **Status:** accepted
- **Date:** 2026-08-27
- **Doctrine sections affected:** none directly. No voxel-core change, no new
  float in voxel-core, no on-disk format change — the `.vxa` format already
  carries the distinction this ADR names (see Context). One tooling gate added
  (`asset-forge/tools/export_banks.py`).
- **Human sign-off:** Matt, 2026-08-27. Settled the world ladder
  (10 cm → 5 cm → 2.5 cm, floor at 2.5, no 2 cm tier), that crafting carves at
  2.5 cm, that the seven off-ladder corals move onto it, that the ladder is
  enforced in tooling, and then drew the entity boundary this ADR records:
  *"all of our animals and npc entities like people, birds, fish, etc. are
  spawned in a separate system and can have their own individual lattice."*

## Context

Voxel content in this project had one implicit rule — "everything is on the
world lattice or a divisor of it" — and that rule had never been written down,
so it was applied to things it does not govern.

Concretely: 337 of 828 authored species sit at 1 cm or 2 cm, off any rung of the
world ladder. Read as a ladder violation, that is a 337-species cleanup with two
bad outcomes on offer (coarsen them into unrecognisability, or delete authored
work). Read correctly, **all 337 are animals, none of them bake, and no animal
reaches the world grid at all** — 382 animal species, 0 baked grids.

The distinction was already in the data, twice, and neither place had a name:

- **`.vxa` v3 stores joints in MILLIMETRES, not voxels**, explicitly because
  "a joint is the centroid of a contact patch and rounding it to the lattice
  would move a shoulder by half a voxel" (`assetgrid.h:76-77, 109-111`). A
  format that refuses to quantise a rig to the lattice is a format saying this
  content is not lattice-space.
- **`hasParts()` is a hard exclusion from world composition** — any rigged grid
  is refused by `assetfield.h:420` and again by `VoxelDetailAssetSubsystem.cpp:863`.
  The engine already refuses to compose rigged things into the world lattice.

So the boundary existed and was being enforced; it just had no name, which is
why it read as a backlog of stragglers.

## Decision

**There are two lattice jurisdictions, and they answer to different rules.**

### 1. The world ladder — 10 cm → 5 cm → 2.5 cm

Static world content: terrain, rocks, trees, ground cover, and player
chiselling. Exactly three rungs; **2.5 cm is the floor and there is no 2 cm
tier.**

| rung | pitch | pool level | content |
|---|---|---|---|
| L0 | 10 cm | 0–6 (ring cascade) | terrain |
| L−1 | 5 cm | 7 (`kCoverLevel`) | ground cover |
| L−2 | 2.5 cm | 8 (`kCraftLevel`) | player chiselling |

Each step halves, and that is **forced by four independent mechanisms**, not
chosen:

1. **Exact ray rescale.** The marcher scales a ray into a lattice and the hit
   back out; at 2^±k the round trip is bit-identical in float. `VoxelMarch.usf`
   records two float ULP in this arithmetic producing 20 wrong palette hashes
   per 1.35M rays — visible as speckle, not as an error.
2. **Addressing is a shift, not a divide.** `VoxelMarchBeginLevel` uses
   `origin >> Level`, and notes in-file that an arithmetic shift is the correct
   floor for negative coordinates where a divide is not.
3. **Integral chunk nesting.** Chunk = 32 cells, brick = 8, so the divisor must
   divide 32. At 5 (i.e. 20 mm) a fine chunk spans 6.4 terrain voxels: a lookup
   table instead of a key identity, and no home for the supersede bit.
4. **The projection reuses `mips.h`'s 2× fold.** 2^n is n applications of an
   existing worldgen-versioned rule; anything else needs a new aggregation rule,
   which `mips.h` warns is world-breaking to change.

Depth stops at three rungs by the cascade's own rule, `OuterUU(L) = R0 · 2^L` —
band radius scales with cell size, which is what makes every level cost about
the same (measured: quads per ring 18.4 / 15.1 / 18.8 / 17.6 / 15.6 / 14.5 %,
nearly uniform). A fourth rung at 1.25 cm is resolvable only within ~20 m and
would earn its index slot for objects a player holds, not for building.

### 2. Per-entity pitch — animals, NPCs, anything spawned

Spawned entities are **not on the ladder and are not required to be.** Each
asset declares its own `voxel_mm` and is drawn under its own transform.

**It is not "an entity lattice".** There is no shared entity grid and none
should be built. It is per-asset pitch, applied under a per-entity transform.
Naming it as a lattice invites someone to construct a grid that has no reason to
exist.

## Why the ladder does not govern entities

Every one of the four mechanisms above is a consequence of **nesting inside the
world grid**, not of being made of voxels:

| constraint | what it is actually about |
|---|---|
| exact 2^±k rescale | the ray walks the *world* volume |
| shift, not divide | addressing *world* chunk coordinates |
| divisor divides 32 | nesting inside a *world* chunk |
| the 2× fold | projecting back into a *terrain* voxel |

An entity has a transform in continuous space, moves independently, is never
indexed by chunk coordinate, and never projects into a terrain voxel. **None of
the four binds it.** The ladder is a property of the world grid, not of voxel
content in general.

And a shared pitch is the *wrong* answer for entities on its own terms. The
smallest authored bird is 0.20 m long:

| pitch | voxels across the smallest bird |
|---|---|
| 1 cm (authored) | 20 |
| 2.5 cm | 8 |
| 5 cm | 4 |
| 10 cm | 2 |

Pitch should scale with the object. That is exactly what per-entity pitch does
and exactly what a shared ladder cannot.

## Consequences

**Immediate, and all already true:**

- Entities need their own collision — a capsule or AABB, not `IsSolidAtVoxel`.
  Normal for entities and already how the character controller treats itself.
- Entities need their own draw path rather than the terrain brick pool, since
  they cannot be keyed by world chunk coordinate. The instanced-mesh path they
  would use already exists.
- Residency is per species bank, process-lifetime — `AssetBankLibrary` already
  does exactly this, which is the right shape for spawned things.
- **The 337 off-ladder animal specs are correct as they stand.** They are not a
  cleanup backlog. Re-pitching them is part of whatever work first renders
  animals, and 2.5 cm is the only rung that could hold the small ones — a look
  judgement to be made against a render, not a mechanical conversion.

**The seam this creates, and it is the one thing to get right later:**

> **Entity → world conversion needs an explicit representation, never a
> resample.** The moment an entity becomes world content — a corpse that becomes
> harvestable, a dropped item placed as a block, a fish frozen into ice — it
> crosses jurisdictions. **Nothing anywhere resamples an `AssetGrid`**, so a
> 1 cm bird stamped into a 10 cm world does not degrade, it comes out ten times
> too big. The answer is a drop/corpse asset authored on the ladder, the same
> rule that made the corals a re-bake rather than a resize.

**Open and aesthetic:** entities will carry a finer visual grain than the terrain
around them. That is probably desirable — players look closely at creatures and
not at dirt, and both Minecraft and Vintage Story model mobs at finer effective
resolution than blocks — but it is a look call, and the first honest test is a
bird landing on a chiselled wall.

## Enforcement

`check_on_ladder()` in `asset-forge/tools/export_banks.py` refuses any bake whose
pitch is not in `LADDER_MM = (100, 50, 25)`, by name, on the same refusal path as
the existing layer check.

**At export and not at authoring, deliberately.** `resolution_cm` is an authoring
value: 226 specs sit at 1 cm and 111 at 2 cm while nothing baked has ever come
out below 20 mm, so authoring-time enforcement would fail hundreds of specs to
catch a handful of real ones. Export is where a spec becomes shipping content, so
it is where the ladder applies. It is the authoring-side twin of the engine's own
refusal (`assetCoverPitchRefusals`, `coverVolumeInit`), and its absence is exactly
how seven 2 cm corals shipped unnoticed.

**This gate does not and must not police entities.** It fires at bank export;
animals produce no banks. If animals ever gain an export path, it needs its own
rules, not this one.

## Alternatives rejected

- **Force everything onto the ladder.** Costs the animal library its
  recognisability (a 4-voxel bird) to satisfy a constraint that does not apply to
  it.
- **Delete off-ladder assets.** Discards 382 authored species — 127 birds, 131
  quadrupeds, 106 fish, 18 cetaceans — to fix a problem that does not reach the
  game.
- **Add a 2 cm rung for entities.** Fails all four mechanisms above the moment
  anything on it touches the world grid, and buys nothing: entities do not need a
  shared rung at all, and the reef assets it was once proposed for are now at
  5 cm.
- **A shared "entity lattice" grid.** Solves nothing per-entity pitch does not,
  and re-imposes a quantisation the `.vxa` joint table deliberately refuses.
