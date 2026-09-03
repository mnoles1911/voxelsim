# The craft lattice — 25 mm sub-voxel building

Date: 2026-08-26, updated 2026-08-27. Design, and the record of what has landed.
Territory so far: **`voxel-core` only** — `craftlattice.h`, `craftvolume.h`,
`world.h`, `editlog.h`, `editcompact.h`, two test files. **No UE file has been
touched**, deliberately; see §7.

---

## 1. What this is

A third voxel lattice at **25 mm**, addressed only inside terrain bricks a
player has deliberately chiselled, produced through the *same*
`packChunkBricksCanonical` as terrain and ground cover, destined for the *same*
`FVoxelBrickPool`, and marched by the *same* traversal with a different ray
rescale. Not a new renderer, not a new format, not a second decode.

The reference point is Vintage Story's chiselling, and **two things commonly
said about it are wrong and change the design:**

1. **Vintage Story is 6.25 cm, not 1–2 cm.** Its blocks are 1 m and the chisel
   subdivides them 16³ = 4,096. Our terrain voxel is *already* 10 cm. At 25 mm we
   are **2.5× finer than Vintage Story per axis**, and a promoted 80 cm brick
   holds **32,768 craft cells against its 4,096**.
2. **We had no 2 cm lattice to build on.** The asset library is multi-lattice —
   every `.vxa` declares its own `voxel_mm` and 28 grids really are at 20 mm —
   but all seven of those species are **corals**, ocean-only, and every
   composition path **refuses them by name** (`assetCoverPitchRefusals()`,
   `assetmanifest.h:354`). The one fine volume that exists is ground cover at
   **50 mm, pool level 7**, fully built and byte-verified and **off by default**
   (`voxel.Cover.Produce` = 0). Nothing anywhere resamples: a 20 mm grid read
   into a 50 mm volume does not degrade, it comes out **2.5× too large**
   (`asset-forge/README.md:38-41`).

### Do not call it "fine"

`FVoxelFineTileStreamer`, `VoxelFineLockMeter.h`, `EStage::FineResidency`,
`-VoxelFineTileDir=` and `docs/fine-bake-production-architecture.md` already mean
the baked `.vxtl` v2 **2-D elevation** tier. Two systems called "fine" are
indistinguishable in every grep and every log line. This one is **craft**.

---

> **The jurisdiction rule is now ADR-0010** (`docs/adr/0010-two-lattice-jurisdictions.md`).
> The ladder below governs STATIC WORLD CONTENT only — terrain, rocks, trees,
> ground cover, chiselling. **Spawned entities (animals, NPCs, people, birds,
> fish) are not on it and are not required to be**: each carries its own pitch
> under its own transform. If you are here because you found a 1 cm asset and
> think it is a bug, read the ADR first — it almost certainly is not.

## 1b. PINNED DECISION (owner, 2026-08-27): three levels, and no more

**10 cm terrain -> 5 cm -> 2.5 cm, and 2.5 cm is the floor.** Settled; do not
re-open without the owner.

These are not three ad-hoc lattices. They are **L0, L-1 and L-2 of one binary
pyramid**, and reading them that way is what makes the rest of this document
consistent:

| level | pitch | pool level | what lives there |
|---|---|---|---|
| L0  | 10 cm  | 0-6 (ring cascade) | terrain |
| L-1 | 5 cm   | 7 (`kCoverLevel`)  | ground cover |
| L-2 | **2.5 cm** | 8 (`kCraftLevel`) | player chiselling |

### Why the divisor is 2 and can never be anything else

Four independent mechanisms each require it, so this is forced rather than
preferred:

1. **Exact ray rescale.** The marcher scales a ray into a lattice and the hit
   back out. At 2^+-k that round trip is bit-identical in float. `VoxelMarch.usf`
   records that two float ULP in exactly this arithmetic produced 20 wrong
   palette hashes per 1.35M rays -- visible as speckle, not as an error.
2. **Addressing is a shift, not a divide.** `VoxelMarchBeginLevel` uses
   `origin >> Level`, and says in-file that an arithmetic shift is the right
   floor for negative coordinates where a divide is not.
3. **Integral chunk nesting.** Chunk = 32 cells, brick = 8, so a divisor must
   divide 32. At 5 a fine chunk would span 6.4 terrain voxels: a lookup table
   instead of a key identity, and no home for the supersede bit.
4. **The projection reuses `mips.h`'s 2x fold.** 2^n is n applications of an
   existing worldgen-versioned rule; anything else needs a new aggregation rule,
   which `mips.h` warns is world-breaking to change.

**This is the answer to "why not 2 cm", stated once so it is not re-derived:**
20 mm divides 100 mm, but by 5, and it fails all four.

**There is no 2 cm tier and there is no exception for reefs** (owner, 2026-08-27).
An earlier draft of this document said the coral set "wants its own 2 cm volume
if it is ever rendered". That escape hatch is withdrawn: **everything in the game
divides down from 10 cm through 5 cm to 2.5 cm, and nothing else exists.** A
20 mm grid is an authoring error, not a special case to accommodate. See section
1c for the seven specs this makes stale and what to do with them.

### Why the depth stops at L-2

The cascade's own construction rule is `OuterUU(L) = R0 * 2^L` -- band radius
scales with cell size -- which is the condition that makes every level cost the
same, since cells over a surface go as area/pitch^2. It is measured, not
asserted: resident quads per ring came out 18.4 / 15.1 / 18.8 / 17.6 / 15.6 /
14.5 %. Nearly uniform.

Extending the same rule downward from R0 = 64 m:

| level | pitch | equal-cost radius | resolvable to (1440p, 90 deg, 1 px) |
|---|---|---|---|
| L0  | 10 cm   | 64 m | 163 m |
| L-1 | 5 cm    | 32 m | 81 m |
| L-2 | 2.5 cm  | 16 m | 41 m |
| L-3 | 1.25 cm | 8 m  | 20 m |

L-3 would be resolvable only within 20 m and would earn its slot for objects a
player holds and inspects, not for building. It is not in scope.

**The caveat that keeps this rule from misleading:** equal-cost assumes a level
is FULLY POPULATED across its band. Terrain is; cover is not (only where plants
grow) and craft is emphatically not (only where someone built). Sparse levels are
priced by occupancy, not area, which is why cover at 64 m and craft at 32 m are
each about 2x the dense rule and still affordable. Do not shrink a sparse band by
quoting the dense rule at it.

### DECIDED 2026-08-27: crafting carves at 2.5 cm

**A note on how this was recorded, because I got it wrong once.** An earlier
version of this section stated "craft goes 10 -> 2.5 cm directly" as a settled
corollary when the owner had settled only the LADDER. That was my inference
written down as their decision. It was un-pinned, put to them as a question, and
is now genuinely decided. The reasoning below is what informed it.

**A carve is 2.5 cm, and the 5 cm behaviour comes from the BRUSH, not a second
lattice.** "5 cm normally, 2.5 cm when needed" needs one lattice at 2.5 cm and a
brush dial -- a 2-cell brush is 5 cm. Storage adapts on its own: carving on 5 cm
boundaries leaves 2x2x2 groups uniform, which the brick format collapses.

The trade that was weighed:

**2.5 cm has one structural property 5 cm does not have.** A craft chunk is 32
cells; at 25 mm that is 800 mm, which is EXACTLY one terrain brick, so
`craftChunkKey == terrain BrickKey`, promotion is per brick, and the supersede
flag is one bit per brick -- 64 per chunk record, which is the shape the existing
L1 mask already has. At 50 mm a craft chunk is 1.6 m = **eight** terrain bricks,
so either promotion becomes eight times coarser or the supersede plumbing stops
matching the record. **That identity is the reason the packer needed no changes,
and it exists only at 2.5 cm.**

**5 cm has one large advantage: it may need no new pool level at all.** Ground
cover already owns level 7 at exactly 50 mm with 1.6 m chunks. A 5 cm craft
lattice is geometrically identical to it, so it could compose into that volume
rather than claiming level 8 -- which would remove the level-field widening and
the ninth index slot (8 MiB, plus its share of the full-rebuild upload) from the
critical path entirely. **That is P0 disappearing.** It is not free: cover ADDS
material where terrain has none and craft REPLACES material terrain reports, and
cover is render-only while craft must be authoritative and persisted. Two
producers with opposite semantics in one volume is a real design problem, not a
merge.

Neither is written into voxel-core beyond `kCraftPitchMm`, `kCraftCellsPerVoxel`
and the two static_asserts that tie them together.

**Outcome: 2.5 cm. So P0 is confirmed necessary** -- the level-field widening and
the ninth index slot are on the critical path, and `kCraftLevel = 8` stands. What
is already built needs no change.

**What actually constrains the pyramid is index slots, not the level field.** The
field is 4 bits (16 levels) and we use 9. Each slot is 8 MiB of toroidal index,
against a full-rebuild upload already flagged in-file as a streaming regression
at 64 MiB per dirty frame. Slots are the budget; spend them on levels that earn
their band.

---

## 1c. What the no-2-cm decision makes stale — measured, 2026-08-27

Census of `asset-forge/specs/*.json` against the baked banks in
`out/engine/banks`, joined by species name (439 of 439 names join, so a "no
offenders" result is a real answer and not a broken join):

| baked pitch | species | grids | on the ladder? |
|---|---:|---:|---|
| 100 mm | 209 | 833 | yes |
| 50 mm | 223 | 887 | yes |
| **20 mm** | **7** | **28** | **no** |

**The whole exposure is seven species and twenty-eight grids**, and they are the
ones already known: `black-coral-tree`, `branching-stony-coral`,
`carnation-soft-coral`, `cold-water-coral`, `elkhorn-coral`, `leather-coral`,
`staghorn-coral`.

**They are the odd ones out among their own kind.** Eleven other coral species
are already on the ladder -- `boulder-star-coral`, `brain-coral`,
`bubble-coral`, `coral-rubble-bank`, `fire-coral`, `lettuce-coral`,
`organ-pipe-coral`, `pillar-coral`, `plate-coral` at 10 cm and `coralline-turf`
at 5 cm. So this is not a reef-wide problem needing a reef-wide answer; it is
seven specs that were authored finer than their neighbours.

**A source-of-truth note that matters here.** `resolution_cm` in a spec is an
AUTHORING resolution and is not what ships: 226 specs say 1 cm and 118 say 2 cm,
yet nothing baked comes out below 20 mm. Only the 439 baked species matter, and
of those the authored pitches are 209 at 10 cm, 223 at 5 cm, 7 at 2 cm. **Do not
quote the spec census as if it were the shipping census** -- it overstates the
problem by roughly fifty times.

### DONE 2026-08-27 -- shipped, owner-judged

**Owner decision: re-author the seven at 5 cm.** `resolution_cm` "2" -> "5" on
all seven, one line each. **Baked and exported**; the whole library is now on the
ladder.

**The owner judged the render, not the description, and that is the process
working.** `asset-forge/tools/coral_ab.py` renders each species at 2 cm beside
5 cm at IDENTICAL metres-per-pixel (pixels-per-voxel proportional to pitch, then
one common downscale -- the correction `lattice_ab.py` earned when a per-variant
target size made its coarse column look better than it was). Verdict:
**"all of the assets actually look better at 5cm."**

That verdict mattered because the sheet showed something a description would
have missed. **The move does not merely coarsen them -- five of seven get
physically BIGGER**, `black-coral-tree` from 1.54 m to 2.45 m (+59%), while the
two soft corals shrink and lose most of their material (1,038 -> 92 voxels and
667 -> 83). Because the sheet holds metres-per-pixel identical, that is real
size, not a rendering artifact. The likely mechanism is generator parameters
counted in VOXELS rather than metres, so a coarser lattice multiplies the result.
Recorded rather than fixed: the owner judged the result good, so this is a
property of the generators, not a defect to chase.

| | before | after |
|---|---|---|
| species at 50 mm | 223 | **230** |
| species at 100 mm | 209 | 209 |
| species OFF the ladder | **7** | **0** |

**Verified from the bytes, not the log.** The bake reported 28 grids, 0 refused
-- so the new export gate accepted 50 mm rather than merely not running. Re-decoding every baked `.vxa` header confirms zero species off the
ladder. And the manifest re-export was checked by byte-diffing the old
`species.vxm` against the new: **exactly 7 bytes differ, each `0x14` -> `0x32`**
-- 20 mm to 50 mm, one byte per coral, nothing else in the file moved. That is
the whole change, proved rather than assumed.

**`species.vxm` and the banks are gitignored** (`.gitignore:123`,
`asset-forge/out/`), so the SPECS are the committed source of truth and anyone
reproducing this re-runs the bake:

```
python tools/export_banks.py --kind bush --only black-coral-tree \
    branching-stony-coral carnation-soft-coral cold-water-coral \
    elkhorn-coral leather-coral staghorn-coral
python tools/export_manifest.py
```

Note `--kind bush` is required as well as `--only`: the two are ANDed and
`--kind` defaults to the terrain kinds (tree, rock), so `--only` alone silently
bakes nothing.

**These seven now appear in the ocean for the first time.** They were refused at
load while their pitch was 20 mm; at 50 mm the cover path accepts them. That is a
visibility change and it is intended.

**A type gotcha worth knowing before touching these files.** `resolution_cm` is a
STRING in 821 of the 828 specs, not a number. My first edit wrote integers and
was caught by reading the diff; it is now `"5"`, matching the convention.

**And it broke one of my own checks, which is worth recording rather than
quietly fixing.** The census in the table above cross-referenced authored pitch
against baked pitch with `if authored in (1, 2)` -- an INT comparison against
STRING values, so it matched nothing and reported "no other offenders". A check
that could not fail. **The headline is unaffected and here is why it survives:
the seven were found by decoding the baked `.vxa` bytes, which carry `voxel_mm`
as a u32 and know nothing about spec types.** The broken half was the secondary
cross-check, not the measurement. Post-edit the spec census reads 226 at "1",
111 at "2", 10 at "2.5", 223 at "10", 258 at "5".

### Why the bake is not just a text edit

Changing those seven to 5 cm is `resolution_cm: 2 -> 5`, a re-bake, and a
manifest re-export. Three reasons it needs the owner's timing rather than a
quiet commit:

1. **Asset export is worldgen input.** Banks and `species.vxm` decide what the
   world contains; a re-bake moves the world and its digests.
2. **Nothing resamples.** A 2 cm grid is not scaled down to 5 cm -- it is
   re-authored at 5 cm. The forge's own note is that at 5 cm a blade is one voxel
   wide whatever you ask for, so the shapes will change and that is a judgement
   call about how the corals look, not a mechanical conversion.
3. **It is a visibility change, not a cleanup.** These seven are refused at load
   today, so they draw nothing. At 5 cm they would be accepted -- so this makes
   seven species APPEAR for the first time, in the ocean, which is a thing to
   look at rather than assume.

### The follow-on that makes the decision stick -- BUILT 2026-08-27

`check_on_ladder()` in `asset-forge/tools/export_banks.py`, refusing anything not
in `LADDER_MM = (100, 50, 25)` by name, on the same refusal path as the existing
layer check.

**At EXPORT, not at authoring, and the distinction is the whole design.**
`resolution_cm` is an authoring resolution: 226 specs sit at 1 cm and over a
hundred at 2 cm, while nothing baked has ever come out below 20 mm. Refusing at
authoring would fail hundreds of specs to catch a handful of real problems.
Export is the moment a spec becomes shipping content, so it is the moment the
ladder applies.

It is the authoring-side twin of the engine's own refusal
(`assetCoverPitchRefusals`, `coverVolumeInit`). Without it an off-ladder asset is
caught only at load, after a bake, by a different person -- which is exactly how
the seven corals survived as long as they did. Verified to accept 100/50/25 and
refuse 20/10/30; 25 mm passes, so a future craft-lattice asset is allowed.

---

## 2. The design, and why each number is forced

**25 mm, because it divides `kVoxelSizeMm` by a power of two.** The marcher
rescales a ray by 1/2^k per lattice (cover uses 1/2, craft uses 1/4); brick and
chunk nesting must stay aligned; and the projection is `mips.h`'s 2× fold. 20 mm
divides 100 mm as well — but by 5, and **none of those three properties survives
it.** That is the whole reason the target is 2.5 cm rather than the 2 cm the
asset library can author.

**The promotion unit is the 8³ terrain brick (80 cm), and the format chose it.**
A craft chunk is `kMarchChunkEdgeVoxels` = 32 cells; at 25 mm that is 800 mm,
which is exactly `8 × kVoxelSizeMm`. So

```
craftChunkKeyOfCell(c) == ChunkMap<8>::keyForVoxel(voxelOfCraftCell(c))
```

— one key space, no mapping table, an edit's dirty set maps 1:1, and the
supersede bit the marcher will need is **one bit per brick**, which fits the 64-bit
shape the chunk record already has room for. Per-*voxel* promotion would need
32,768 bits per chunk against 192 spare.

**The projection is `mips.h::downsampleBricks` applied twice, with no new rule.**
64 craft bricks → 8 bricks at 5 cm → one 8³ brick at 10 cm, which is exactly the
terrain brick. Everything that reads the coarse world — collision, pathfinding,
water, the ring meshes, the digest — keeps working and never learns a craft
lattice exists.

**Promotion is nearly free; carving is what costs.** Pinned in
`voxel-core/tests/test_craftcost.cpp`: a promoted-but-uncarved brick is 64
uniform-SOLID descriptors, **512 B**, with no occupancy or material payload at
all. The adversarial ceiling -- every brick mixed, 8 bpp, more materials than a
16-entry palette can hold -- is **37,376 B**. A 73x spread.

**A precision that cost me a wrong number twice:** `residentBytes()` counts
descriptors + occupancy + materials and does **not** include the 64 B chunk
record, which lives in the pool's separate ChunkTable. Figures of 576 B and
37,440 B are those totals *plus* the record. Both framings are fine; mixing them
is not.

---

## 3. Three rules that are not obvious, and each cost a bug to find

### 3.1 Promotion materialises all 64 craft bricks — to break a circularity

The tempting design is "a craft cell with no entry reads its parent terrain
voxel". But the parent voxel of a promoted brick holds **the projection**, which
is computed **from** the craft cells. Read one through the other and the
definition is circular: after a carve, the projection feeds back into the cells
it was derived from. So `promote()` expands the brick up front and inside a
promoted brick there is never a fallback. Uniform regions collapse to
homogeneous bricks, so it stays cheap and the definition is acyclic.

### 3.2 A hollow promoted brick must STILL be stored — this diverges from cover

Cover obeys requirement C1: a chunk with nothing in it stores nothing.
**Craft must not.** The marcher's terrain walk will skip a brick whose supersede
bit is set, and that bit means "a craft chunk is resident here". A player who
hollows a brick out completely produces an all-air craft chunk — and if the
producer drops it the way cover would, the bit clears, the terrain walk stops
skipping, and **the carved-away rock comes back**. The empty pack is the point of
the empty pack.

So the sparsity is in **which bricks are promoted**, not in whether a promoted
brick produces. C1 is satisfied by promotion being deliberate.

### 3.3 A terrain edit into a promoted brick is routed to the craft stream

**Found while writing the ordering test, not by design.** Two things go wrong
otherwise, and both are silent:

- **The invariant.** For a promoted brick the overlay holds `project(craft)`. A
  dig written straight to the overlay makes the two disagree until something
  reprojects, and **the next chisel anywhere in that brick silently reverts it.**
- **Chronology.** Two append-only streams have independent sequence numbers, so
  a replay cannot interleave them — it can only run one then the other. If a dig
  and a chisel touch the same voxel, whichever stream replays last wins, and that
  is not necessarily the one that happened last.

`applyEdit` therefore routes into the craft stream once a brick is promoted,
expanding each 10 cm cell into its 4³ craft cells. That makes the craft log the
**sole authority for promoted bricks**, which removes the interleaving question
entirely: terrain-then-craft replays the real order by construction.

And because a terrain log produced by this engine can then never name an
already-promoted brick, `replay()` **refuses** one that does — before applying
anything, so a refusal leaves the world untouched. That guard is proved to fire
by `the_out_of_order_guard_can_fire_and_leaves_the_world_untouched`.

---

## 4. What landed

### `voxel-core/include/voxelcore/craftlattice.h` (new)
The state and the projection. `CraftLattice<B>`: the promoted set, the 25 mm
overlay keyed in craft-brick coordinates, `promote`, `materialAt`, `setCell`,
`project`, `digest`, and a strict funnel of counters that exist to fail
(`bricksPromoted`, `cellsWritten`, `projectRefusedMissingBrick`, …).

**A missing craft brick is a refusal, never air.** This project has paid
repeatedly for "absence reads as air" — a missing fine tile returning sea level,
a missing tile counting as provably empty — and each time the result was deleted
terrain with every counter healthy.

### `voxel-core/include/voxelcore/craftvolume.h` (new)
`produceCraftChunk()` = `packChunkBricksCanonical` over a craft accessor. **The
packer needed no change at all**: `kCraftChunkEdgeCells == kMarchChunkEdgeVoxels`
and `100 % 25 == 0`, so a craft chunk is a canonical 32³ brick chunk in every
respect. Because the producer *is* the shipping packer, a GPU craft stamp is
checkable against it from day one by the gate that already exists
(`voxel.Cover.VerifyStore`'s shape).

### `editlog.h` — format v3
Adds `latticePitchMm` (u32) after `providerId`. **Brick edge is 8 on both
lattices, so it does not distinguish them**; without the pitch a craft log
replayed as terrain would land 25 mm diffs on the 10 cm lattice at a quarter of
their true coordinates and read as world corruption rather than a mixed-up file.
Pre-v3 logs read as `kVoxelSizeMm`, which is what every log written before this
was. `peekHeader` reports it, skipping `providerId` rather than materialising it
so it keeps its promise to allocate nothing.

**A log is stamped with the LOWEST version that can carry its content, not with
`kFormatVersion`.** A terrain log's pitch is exactly what a v2 reader already
assumes, so its v3 encoding would be byte-identical to v2 apart from the version
number and a redundant field — and stamping it v3 would make **every pre-craft
build refuse a save it could have read perfectly** (`parse` rejects
`fmt > kFormatVersion`). The world is fine, the reader is fine, and the version
number alone breaks them apart: silent data loss with nothing to diagnose.

So only a craft log — which genuinely needs the field — is written v3, and craft
logs live in a file no older build looks for. `kFormatVersion` is now "the
highest version this build can write"; `formatVersionForContent()` is what it
actually writes. Version and field presence move together, because reading one
without the other shifts every following byte.

*Added after the marcher lane flagged the v3 save risk as the one thing it was
carrying forward. It is cheaper to not create the incompatibility than to
remember the incompatibility exists.*

**The hazard was not hypothetical — it had already fired, and the record is on
disk.** `ue-project/Saved/VoxelWorlds/20260719.vxlog`, written 2026-08-26
17:23:30 by a real editor run, decodes as:

```
fmt=3  wgen=28  edge=8  provLen=0  pitch=100  entries=0    (35 bytes)
```

A **terrain** log — `pitch=100` is `kVoxelSizeMm` — stamped v3 for no reason,
because the editor binary compiled `editlog.h` and wrote `kFormatVersion`
unconditionally. Every earlier `.bak` beside it is 31 bytes and `fmt=2`. The
four-byte difference is the redundant pitch field, and that file is unreadable
to any pre-craft build. Under the rule above it is written as v2 and the
difference disappears.

Two notes for whoever cleans this up:

- **It carries zero entries**, so nothing is at risk. Every file in that
  directory is an empty log; there is no world content in any of them.
- **A copy is not a fallback here.** The `.presafe-*` backup taken before the
  PIE session is *also* v3, because it is a byte copy. It protects against
  content loss, which was never the exposure, and not against format refusal,
  which was. The newest genuinely v2 file is `.bak-20260826-040738`.

**The fix does not take effect until the editor is rebuilt.** The running
binary predates it, so the next autosave writes v3 again. This resolves itself
on the next UE build; no file surgery is needed, and file surgery while an
editor holds the save is how you lose the thing you were protecting.

### `editcompact.h`
`compactLog` now carries `providerId` **and** `latticePitchMm` through. It
previously dropped both; dropping the pitch would silently relabel a compacted
craft log as terrain.

### `world.h` — the second stream
`craftLattice()`, `craftLog()`, `craftDigest()`, `craftMaterialAt()`,
`isPromoted()`, `applyCraftEdit()`, `setCraftCell()`, `replayCraft()`, plus the
routing and out-of-order guard of §3.3. The projection is **written into the
overlay and never into the terrain log** — the craft log is the authority for
those cells, so writing both would double-count them on replay.

**`World<16>` still works.** It is instantiated in 19 places across the edit-log
and compaction tests and exists to prove the template works at a second brick
size. A craft chunk is one terrain brick only when `4 * B == 32`, so the craft
half is switched off there with `if constexpr` and every craft entry point
carries a `static_assert` — calling one on a `World<16>` is a clear compile error
naming `kCraftSupported`, not a silent no-op that appends log entries nothing
will apply.

---

## 5. Evidence

`ctest` / `vxc_tests`: **790 pass, 0 fail, exit 0** on the full suite (was 777
before this work; 13 new craft tests plus the persistence set).

The load-bearing test is **`promote_is_projection_identity`**: promoting a brick
expands each 10 cm voxel into 64 craft cells and folding straight back must
return the **byte-identical** terrain brick. If promotion alone changed the
world, every downstream claim would be built on a world that moved when nobody
edited it. It is run on a deliberately non-homogeneous brick and at negative
coordinates, plus both homogeneous sub-cases.

### Mutation results — the tests are shown able to fail

Per the standing rule (a gate proved only against a correct implementation is
proved against the easy case), the producer and the persistence layer were each
broken deliberately and the suite watched go red. Harness:
`scratchpad/mutate.py`, `mutate2.py`.

**Eight mutations, eight detections, every one by a named assertion.**

| mutation | caught by |
|---|---|
| missing craft brick silently reads as air | `producer_refuses_a_missing_craft_brick_rather_than_packing_air` |
| hollow promoted brick dropped instead of stored | `a_hollow_promoted_brick_is_still_PRODUCED_and_the_counters_say_which` |
| projection round 2 transposes y and z | `promote_is_projection_identity` |
| compaction drops the lattice pitch | `compaction_preserves_the_lattice_pitch_and_the_provider` |
| `replayCraft` accepts a terrain log | `a_craft_log_is_refused_by_the_terrain_replay_and_vice_versa` |
| terrain edit into a promoted brick is not routed | `a_dig_into_a_promoted_brick_keeps_overlay_equal_to_the_projection` |
| out-of-order replay guard removed | `replaying_craft_BEFORE_terrain_loses_the_carve`, `the_out_of_order_guard_can_fire_and_leaves_the_world_untouched` |
| parsed lattice pitch is discarded | `editlog_v3_round_trips_the_lattice_pitch`, `two_stream_replay_reproduces_the_world_exactly`, `replaying_craft_BEFORE_terrain_loses_the_carve` |

**One methodological note worth keeping.** The first mutation run reported
"SUITE RED" for the missing-brick case with **no named failing test** — the
mutation dereferenced a null and crashed the runner. A crash is a loud failure
and proves much less than the silent wrong answer the guard exists to stop, so
the mutation was rewritten to substitute air instead. It then failed cleanly by
assertion. **A mutation that crashes has not exercised the check.**

And a second: the harness restored sources but left the mutant `.obj` newer, so
MSBuild called the target up to date and the next run tested **the mutant binary
against pristine sources** — which presents exactly as a real regression. The
harness now touches every restored file.

---

## 5b. `voxelcore.lib` was never contaminated — and the reason generalises

Two sessions independently believed the UE-linked `voxelcore.lib` had been
rebuilt with this work mid-edit, and treated everything built against it as
suspect. **It had not, and it could not have been.**

Every file this work touches is **header-only** — `craftlattice.h`,
`craftvolume.h`, `editlog.h`, `editcompact.h`, `world.h` are templates and inline
classes. `voxelcore.lib` is built from the twelve `.cpp` files in
`voxel-core/src`, and a transitive include scan over all twelve reaches **none**
of those headers. They compile into whatever consumer includes them — the UE
module, the test binary — never into the lib.

The evidence was there and was misread twice:

- The lib's **mtime moved** on a build, which was read as content changing. A
  modification time is a proxy; it moved because the lib was relinked from
  unchanged objects.
- `amplifier.cpp` was believed mid-edit. It was clean the whole time —
  `git status` and `git diff` both empty, `kWorldGenVersion` still 28. **No
  worldgen drift ever existed**, so the goldens and every measurement taken
  against them stand.
- Rebuilding with `--target voxelcore` reported success and **did not move the
  timestamp**, which looks exactly like the silent-success failure this project
  keeps finding. It was correct behaviour: nothing compiled into the lib depends
  on those headers, so there was nothing to rebuild.

**The rule worth keeping: "the lib is stale" is a claim about a dependency
graph, not about a timestamp.** Before trusting or distrusting a prebuilt
artifact, ask which translation units actually reach the changed file. Here the
honest answer retired a risk two sessions were routing work around.

One real consequence remains and was fixed rather than noted: the Build.cs
staleness guard compares the lib's mtime against the newest `.cpp`/`.h` under
`voxel-core/`, so editing a header no TU includes still trips it. It is a
**warning, not an error**, so it blocks nothing — but it names a file and invites
a diagnosis that leads nowhere. The lib was force-rebuilt (`--clean-first`) so it
is genuinely newer than every source and the guard stays quiet.

## 5c. Two defects found by re-reading the routing path — both closed

Found by reading, not by a failing test, which is worth saying: the suite was
green and would have stayed green through both. **Both are now resolved
(2026-08-27).**

### 5c.1 One edit projects the same brick many times (performance, game thread)

`routeTerrainEditToCraft` groups a terrain edit by craft brick and calls
`applyCraftEdit` once per group — and `applyCraftEdit` ends with
`writeProjection(terrainBrick)`. Every group projects **the same terrain brick**.

A projection is two `downsampleBricks` rounds: 8 calls reading 4,096 child cells
each, then 1 more — **~36,864 child-cell reads and 8 brick constructions**. A
dig at `MaxCubeSizeVoxels = 4` spans 16 craft cells per axis, so up to 3 craft
bricks per axis: **up to 27 projections where 1 would do**, ~995k cell reads for
one dig, on the game thread.

It is **correct** — each projection is right for the state at that moment and the
last one wins — so no existing test caught it.

**FIXED.** Applying is now split from projecting: `applyCraftEditNoProject` does
the per-craft-brick work, and `routeTerrainEditToCraft` calls `writeProjection`
once after its loop (every group there belongs to the same terrain brick by
construction). `applyCraftEdit` still projects, because that is its public
contract.

**The gate is `a_routed_dig_projects_its_brick_exactly_once`**, and it was
written before the fix and watched fail — `projections == 1` against the 8 that
a 4x4x4 routed dig actually produced. A test that fails on the real defect is
worth more than a mutation of a correct implementation, because the defect chose
the test rather than the other way round. It also asserts the dig still produces
the right voxels: a cheaper wrong answer is not what was being asked for.

### 5c.2 `applyEdit`'s return value changes meaning when an edit is routed

`applyEdit` returns "the assigned sequence number". For an unpromoted brick that
is a **terrain**-log seq; for a promoted one the edit is routed and the return is
a **craft**-log seq, from a different stream with its own independent numbering.
Same type, same name, two incompatible meanings, decided by whether someone has
chiselled that brick.

**Latent, not live:** the only engine caller
(`VoxelWorldSubsystem.cpp:25112`) discards it, and `setVoxel` just forwards it.
So nothing is wrong today and this is a trap for the next caller rather than a
bug.

**DOCUMENTED AT THE DECLARATION rather than changed**, deliberately. Returning a
stream-tagged struct would ripple through `setVoxel` and every future caller to
fix a trap that has caught nobody; the proportionate move for a latent hazard
with zero live callers is to make it impossible to walk into unknowingly. The
comment names the specific wrong turn — indexing the terrain log by this for
replication catch-up — because a routed edit puts no entry in the terrain log at
all. Revisit if a caller ever genuinely needs the sequence number.

## 6. What is NOT done

Everything below `voxel-core`. In plan order:

- **P0** — the marcher's level field, the ninth index grid slot, the per-level
  dirty index rebuild that must land first.
- **P3** — `FVoxelBrickPool::kCraftLevel`, the band, the supersede mask, the
  craft march segment, `voxel.Craft.VerifyStore` / `VerifyProjection` / `Stats`.
- **P4** — `raycastVoxels` pitch parameter and its `maxSteps`, the tri-state
  `VoxelFineState`, `SweepAxis` escalation.
- **P5** — the chisel tool, brush dial, HUD.

**One engine-side consequence of §3.3 that P3 must not miss:** once a brick is
promoted, its edits stop appearing in the terrain log, so
`SerializeLogEntriesFrom(FromSeq)` and the `AVoxelEditRelay` multicast will not
carry them. Craft entries need their own relay path or the second client sees a
brick stop changing.

### 6.1 One correction to the plan

The plan's P0 section says five places in the repo assert that level 7 is the
format ceiling "because of the VisBuffer's three-bit level field". **All five are
wrong.** `VoxelMarch.usf:733-745`'s prose is stale; the code at `:796-840` leaves
`P.y[30:31]` unused, so widening the level to 4 bits is one bit at `:806` and its
mirror at `:836` with one spare left. The real ceiling is
`static_assert(FVoxelMarchChunkIndex::kGridSlots == 8)` in
`VoxelMarchRenderer.cpp` with six hand-written `[8]`s behind it.

### 6.2 `GVoxelMarchZCutRanZMin/Max` — corrected, and the real shape is worse

I first wrote that these two arrays "would silently write out of bounds".
**That was wrong**, and the correction came from the lane that owns the file.
There is a `static_assert(kGridSlots == 8)` in the *same function* as the writes,
so growing the slot count fails the build before anything can overflow. Verified:
arrays declared `[8]`; written in a loop bounded by `kGridSlots`; read in a loop
bounded by `kRingGrids` (7). Safe today.

**But the precise defect is nastier than "a literal 8", and it is a delayed
overflow rather than an absent one.** The assert's message names
`MarchLevelChunkZ` in `FVoxelMarchCSParameters` and `int4 MarchLevelChunkZ[8]` in
`VoxelBrickTraverse.ush` — it does **not** name `GVoxelMarchZCutRanZMin/Max`. So
whoever eventually satisfies that assert widens the two arrays it names, the
build goes green, and the write loop — now bounded by a `kGridSlots` of 9 —
stores index 8 into an array still declared `[8]`. **The tripwire names two of
the four things that have to move**, and it fires exactly once, on the person
least likely to know about the other two.

**UPDATED 2026-08-27: this is P0's job now.** The marcher lane is not building
ZCut narrowing (it only pays for rays that look up — 0.00% of decisions skipped
at the flight's pitch across 3.3e9 consultations, 21% at a sky pose), so the fix
went with it. Whoever widens the slot count owns these: derive both from
`kGridSlots`, and **put their names into the assert's message** so the tripwire
names all four things that must move rather than two.

### 6.3 A second tripwire P0 must not trip — verified

`FVoxelMarchDepthOnlyVS/PS` declare
`using FPermutationDomain = TShaderPermutationDomain<FVoxelMarchHalfResDim>` —
**no `FVoxelMarchRingsDim`**. So that pass compiles `VOXEL_MARCH_RINGS 0` and
therefore `VOXEL_MARCH_PACK_BIAS 0`, while the march packs its VisBuffer with
bias 4096.

Harmless **only** because the bias touches `V.LocalVoxel` alone and that pass
never reads it. **If P0 makes the depth-only pass read the level field — or
anything else out of the packed VisBuffer — it draws terrain 4,096 voxels away
with no error anywhere.** Verified directly in `VoxelMarchRenderer.cpp`; flagged
by the marcher lane, which found it while retiring half-res.

### 6.4 The census — RUN 2026-08-27

Built in two halves with deliberately different epistemic status, because
`--detail-cover` could walk real ground and ask the resolver what grows there and
**there is no procedural source for what a player builds**:

* **exact half** — `tests/test_craftcost.cpp`, per-pattern byte costs pinned
  against the format contract. Assumption-free; this is the half that can
  falsify something.
* **modelled half** — `vxc_volumeprobe --craft`, a settlement built from a
  recipe written into the tool. It says so on its own banner and must never be
  quoted as a measurement of the world.

#### The dominant term is what STRADDLES BRICKS — geometry *or* material

The first version of this section said "the dominant term is ALIGNMENT". That was
measured, correct, and **true only for single-material building**. Corrected below,
because a mutation exercise found the gap that produced it.

**Geometry alignment, measured.** A 16x32x16-cell hole whose faces land on the
8-cell brick grid leaves every brick uniform and costs the **512 B floor**. The
same hole moved **one cell** costs **3,072 B** — it now straddles 32 bricks.
A **6x** swing for a one-cell shift.

**Material changes do the same thing, and I had missed it entirely.** Every carve
pattern I first pinned used a single material, so the palette was never larger
than one and the 3-4 material rung of the bpp ladder was never exercised. Real
building is stone *and* plaster *and* timber *and* glazing. Four materials is
2 bpp plus a 16 B local palette — **+36% per mixed brick**, pinned as the
`4-material` case in `test_craftcost.cpp`.

Worse, a material change *makes a brick mixed* whether or not the geometry is
aligned. So at four materials the alignment advantage largely collapses:

| | 50 buildings, 1 material | 50 buildings, 4 materials |
|---|---:|---:|
| grid-aligned | 5.7 MB | **43.0 MB** |
| off by one cell | 39.9 MB | **57.9 MB** |
| ratio | **7.0x** | **1.35x** |

> **The real rule: cost tracks how much of the fabric straddles a brick boundary
> — from geometry OR from a material change.** Aligning the geometry only pays
> off if the material courses are aligned too.

*(The 4-material figure is somewhat pessimistic: this model bands materials on a
5- and 11-cell period, which aligns with nothing. A builder laying courses on
20 cm boundaries would land nearer the aligned column. The honest statement is
that the two columns bracket real building, not that either is it.)*

**Both discoveries came from the same place**, and neither from reasoning
further: my first draft used an *aligned* window as the "typical" shape, and my
first mutation run showed that a bounds-only assertion could not catch a change
to the bpp ladder. The pins are now exact and include a multi-material case.

#### One 8x6x3 m building, 20 cm walls

| align | carve | bricks | mixed | pack KiB | floor share |
|---|---|---:|---:|---:|---:|
| grid | shell | 224 | 0 | 112.0 | 100% |
| grid | detailed | 224 | 630 | 161.2 | 56% |
| off-1 | shell | 256 | 8,348 | 780.2 | 10% |
| off-1 | detailed | 256 | 8,148 | 764.6 | 10% |

Fifty buildings, four materials: **43.0 MB aligned, 57.9 MB misaligned**
(linear -- buildings share no bricks). At a hundred buildings the misaligned
column reaches **115.8 MB**, which is the first configuration to come within
sight of the 150 MB falsifier.

Note the rows get slightly *cheaper* with more carving: heavy detailing removes
enough material to collapse some bricks back to uniform air.

#### Both falsifiers, as written

**Falsifier 1** (registered 2026-08-26, 150 MB for a settlement-scale volume):
**STANDS** at 60.7 MB for fifty four-material buildings. It stood at 39.9 MB on
the single-material model too, but that model was under-pricing by ~1.5x, so the
margin is 2.5x rather than the 3.8x first reported.

**Falsifier 2** (registered 2026-08-27, more than the terrain control on the same
footprint): **FIRES on every row** -- 3.7x to 25.8x. The control is the published
real-fine-tile grassland number, 126.7 MiB over the 256 m ring = 645 B/m^2, so
1.55 MB over the settlement's 2,400 m^2 footprint.

**Both are reported, and the ratio is not the reading that should decide.** It
fires because a 3 m building is more geometry than the ground it stands on, which
is expected rather than a defect. The absolute column is what matters: 39.9 MB
against a pool sized in hundreds of MB, and against cover's own 131.6 MiB at its
shipping ring. On that basis craft is a decoration on the world budget rather
than a doubling of it -- **but the gate is recorded as having fired**, because a
pre-registered falsifier is not reinterpreted after the fact.

#### What it means for P3

The band is affordable at realistic building densities, so **±32 m stands**. The
eviction question that falsifier 2 was registered to answer is *not* settled by
it -- the absolute numbers say eviction is not urgent, but §6.2's finding that
`FocusDistSqOf` would rank craft chunks catastrophically wrong is a correctness
bug independent of budget and still must be fixed.

### 6.5 `VoxelMarchBeginLevel(V.Level)` — verified latent

Two independent coincidences hold this up, and both are load-bearing:

`VoxelMarchBeginLevel(Level)` sets `GVoxelMarchIndexGrid = Level` and
`GVoxelMarchOriginVoxelL = MarchBrickOriginVoxel >> Level`.
`VoxelMarchBeginCover()` sets the grid to `MarchCoverIndexGrid` and the origin to
`MarchBrickOriginVoxel * 2`.

So calling `BeginLevel` with a **cover** hit (`V.Level == 7`):

1. **The grid slot is right only by coincidence.** `Level` 7 equals
   `kCoverGridSlot` only because `kCoverGridSlot == kRingGrids == kLevels == 7`.
   Change the ring count and cover's level and its slot stop being the same
   number, and this line starts reading another lattice's sub-grid.
2. **The origin is already wrong, by a factor of 256** — `>> 7` where cover needs
   `* 2`.

It survives today because this call site's only consumer,
`VoxelMarchLookupChunk`, is keyed on **absolute** chunk coordinates and never
calls `VoxelMarchSourceOriginVoxel()` (the sole reader of that origin). Nothing
reads the wrong value, so nothing breaks.

**P0 must route this through a shader mirror of `GridSlotForLevel` rather than
adding a third coincidence to the pile.** A craft level 8 mapped to slot 8 would
coincide again by luck, which is precisely how this stays invisible.

### 6.6 The marcher now carries TAA jitter (2026-08-26)

`voxel.March.TAAJitter` shipped today, **default 1**. The marcher's rays had
never carried the engine's TAA jitter at any resolution — the ray is built from
the projection diagonal and UE puts jitter in row 2. Any P3 image comparison
against a capture taken before today is comparing two different renderers.

---

## 7. Why no UE file was touched

The working tree carries **~1,320 lines of uncommitted work from another
session** on branch `lane/nearest-first-admission-2026-08-24`, in exactly the
files P0 and P3 need:

```
ue-project/Shaders/VoxelBrickTraverse.ush                        110 +
ue-project/Shaders/VoxelMarch.usf                                124 +
ue-project/Source/VoxelEarth/VoxelWorldSubsystem.cpp             297 +
ue-project/Source/VoxelEarthShaders/Private/VoxelMarchRenderer.cpp 520 +
ue-project/Source/VoxelEarthShaders/Public/VoxelMarchRenderer.h  108 +
```

Editing those would stomp work in flight, and building UE against them is the
serialisation hazard the build wrapper exists to prevent. **The engine half needs
that work committed or parked first.**
