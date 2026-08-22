# Fine-tile provider identity: who decides the namespace, and why it broke

**Status:** finding + partial fix shipped. One item needs an owner decision (§6).
**Date:** 2026-08-18. **Branch:** `claude/f6-interior-rim-injection`.

On the night of 2026-08-17/18 the fine-tier residency gate killed three capture
runs. Each was worked around by hand-passing `-VoxelFineTileProviderId=`. Nobody
could say why the default was wrong, which is the part that mattered: a
plausible-but-wrong fingerprint is the failure class this project keeps paying
for, and "we pinned it and it went away" does not distinguish a stale constant
from a broken hash.

It was neither. **Nothing was computing a wrong answer. Nothing was computing an
answer at all on one of the two sides.**

---

## 1. What the fingerprint is a function of, on each side

### Bake side — it is COMPUTED

`terrain_service/providers/diffusion.py`:

```
fine_provider_id() = fine_id_for(provider_id())
                   = provider_id() + "-b" + sha256(...)[:8]
```

Two independent halves, and the `-bXXXXXXXX` suffix is the one that moved:

| part | function of |
|---|---|
| `terrain-diffusion-<label>-<16 hex>` | inference identity only — checkpoint sha256, conditioning digest + file list, terrain-diffusion version, `world_shape` pipeline kwargs, climate calibration curves, scale, channel mapping, `_tile_format_fingerprint()`, identity schema |
| `-b<8 hex>` | `sha256({"bake": bake_identity_payload(), "product": product_identity_payload()})[:8]` — i.e. `BAKE_VERSION`, `TERRAIN_VERSION`, stage order, tile geometry, and **every whitelisted physical constant** in `bake/pipeline.py` |

The suffix is deliberately *suffixed* rather than re-hashed, so the coarse
namespace stays readable off the directory name (`fine_id_for`'s docstring).
That property is what makes the prefix matching in §5 sound.

**Consequence: the suffix moves every time a bake constant moves.** That is
correct and is the entire point — a tile baked with different constants must not
share a namespace with one that was not.

### Engine side — it is NOT computed. It is READ, verbatim

`VoxelWorldSubsystem.cpp` (~line 15194) and `VoxelFineTileStreamer`:

```
FineProviderId = -VoxelFineTileProviderId=<literal>          (command line)
              || DefaultFineTileProviderId in DefaultGame.ini (ini)
              || ""                                           (refuses every tile)
```

That string is passed straight into `FVoxelFineTileStreamer` and then into
`vxc::formatFineTileCacheKey`, becoming a **path segment**. There is no
derivation, no hash, and no C++ knowledge of `BAKE_VERSION` whatsoever.

**So the two sides are not two derivations that disagree. One side derives; the
other obeys. The only thing reconciling them is a human copying nine hex
characters into an ini after every bake change.** That human is the bug.

---

## 2. Which input diverged

Measured, not inferred — the fingerprint was recomputed from each commit's own
source tree:

| commit | `bake_ver` | fine suffix |
|---|---|---|
| `867a52f^` | 27 | **`-bdcab4bed`** |
| `867a52f` *(bake_ver 28: five placement channel planes, SECTION_PLACE_*)* | 28 | **`-b19d281fd`** |
| every commit since, through `77aef42` (HEAD) | 28 | `-b19d281fd` |

`DefaultGame.ini` still pins `-bdcab4bed`. **It was correct, and it went stale
the moment `bake_ver` 28 landed.** Nothing updates it, because nothing can:
no build step, test, or bake writes to that ini.

Two corrections to the working assumption:

* **worldgen v25 → v27 did NOT roll the fine namespace, and should not have.**
  Commit `1f61e8c` ("worldgen v27") touches only `voxel-core/` and `ue-project/`
  — the C++/shader biome classifier. It changes no baked byte, so the Python
  bake fingerprint is untouched. The sweep above confirms it: the suffix is
  identical across all 18 recent commits. This was checked specifically because
  a worldgen change that altered baked placement planes under an unchanged id
  would be a second, worse bug. It is not one.
* **The namespace rolled exactly ONCE tonight, not repeatedly.**

### The second, compounding cause: the dir and the id resolve independently

The failing run's path was
`D:/voxelsim/tile-cache/terrain-diffusion-...-bdcab4bed/` — a directory that has
never existed on any disk. It is a **cross-product of two settings that are only
valid as a pair**:

| setting | came from | value |
|---|---|---|
| `-VoxelFineTileDir` | command line | `D:/voxelsim/tile-cache` |
| provider id | ini fall-through | `...-bdcab4bed` |

> **UPDATE 2026-08-21 — the root split that made this trap possible is gone.**
> `D:/voxelsim/tile-cache` now holds **all three tiers**: the 289 coarse tiles
> (`s1`), the `bake_ver` 28 fine tiles (`s16`) and their flow superblocks. Before
> this, coarse lived only under `D:/vox-trunk-cache` and `D:/vox-wet-cache` while
> fine lived here, so the dir/id cross-product below had extra ways to be wrong
> and no single `--cache-dir` could drive a bake. `DefaultTileDir` in
> `DefaultGame.ini` was repointed to match (the coarse bytes are identical --
> all 289 sha256-equal, and the L1 superblock fingerprint recomputed from the new
> location still reads `066cf1d469ed`). The other roots are redundant copies now,
> not alternatives. The dir/id pairing rule below still stands, and still matters.

Both values are individually correct for *something*. Verified on disk:

* `D:/vox-wet-cache/...-bdcab4bed/` — **exists, holds tile (-3,-4)**, created
  2026-08-12. The ini's own pair (`DefaultFineTileDir=D:/vox-wet-cache` +
  `-bdcab4bed`) is self-consistent and would have worked, against the
  `bake_ver` 27 world.
* `D:/voxelsim/tile-cache/...-b19d281fd/` — **exists, holds tile (-3,-4)**,
  created 2026-08-18T00:23:14Z. Tonight's bake.
* `D:/vox-wet-cache/` has **no** `bake_ver` 28 namespace at all.

`VoxelWorldSubsystem.cpp` resolves the id "INDEPENDENTLY of the dir above, so
that a command-line `-VoxelFineTileDir` pointed at a scratch bake still picks up
the project's provider id and does not have to restate it." That comment
describes the trap exactly: **pointing at a different cache root is precisely
the case where the id is most likely to differ too.** Overriding the dir alone
silently pairs a new root with an old id.

---

## 3. Is either side stale?

Neither derivation is wrong, and they are not answering the same question:

* the bake answers **"what namespace does THIS code write?"** — always current,
  by construction;
* the engine answers **"what namespace was I told to read?"** — always current
  with respect to whoever last edited the ini, which is a fact about human
  attention, not about the world.

The ini pin is a **cache of a computed value, with no invalidation.** It is the
same shape as every other "derived, not verified" bug in this repo (see
`derived-not-verified-detaches`): a join that is computed once and then trusted
forever instead of being checked.

---

## 4. Should the engine derive it? No.

Deriving the fingerprint client-side would require a second implementation, in
C++, of a sha256 over Python bake constants — `bake_identity_payload()` plus
`product_identity_payload()`, i.e. every whitelisted physical constant in
`bake/pipeline.py`. That is a **second answer to the question content addressing
exists to have exactly one answer to**, and it would drift the first time a
constant was added to one side only — failing *silently*, since a wrong id looks
exactly like an unbaked tile.

**The authority already exists and is already written.** `world_manifest.py`
drops `world-identity.json` into every world directory, carrying `namespace_id`
— which *is* the directory the bake wrote into. It was built after the
2026-08-03 "same seed, different world" incident for exactly this reason: a
world must carry a record of what made it.

So the rule is: **the namespace is READ off the record the bake wrote, never
recomputed by the reader.**

---

## 5. What shipped

### a. `find_fine_namespaces()` — the authority reader
`terrain-service/terrain_service/world_manifest.py`

Reads `<root>/<namespace>/<seed:016x>/world-identity.json` and reports every
baked fine namespace for a seed, with per-tile coverage, newest first. Derives
nothing, hashes nothing. Namespaces with no manifest are still reported — the
289-tile world has none, and a reader that hid what it cannot vouch for would
answer "nowhere" for the world that matters most.

Six tests in `terrain-service/tests/test_pregen.py`, all filesystem-only
(no GPU, no checkpoint, no rasters), including a reproduction of tonight's exact
failure.

### b. `tools/resolve_fine_namespace.py` — the harness entry point

```
python terrain-service/tools/resolve_fine_namespace.py \
    --cache-dir D:/voxelsim/tile-cache --seed 20260719 --tiles="-3,-4"
# -> terrain-diffusion-unlabeled-80b9ca451a23eae4-b19d281fd
```

Prints the id on stdout with no decoration, ready to interpolate into a flag.
Exit codes let a harness branch instead of guessing:

| rc | meaning |
|---|---|
| 0 | exactly one namespace holds every requested tile — id on stdout |
| 2 | **none** does. Do not fall back to a default: the gate is fatal, so a guess costs the whole run |
| 3 | several do, and `--newest` was not given. Two namespaces holding the same tile hold two different *bakes* of it; picking by luck is how a measurement ends up describing a world nobody meant to look at |

Verified against the real caches: returns `-b19d281fd` for `D:/voxelsim/tile-cache`
(rc 0, unambiguous), and rc 3 for `D:/vox-wet-cache`, which holds many bakes of
that corridor.

### c. The failure message now names the answer
`ue-project/Source/VoxelEarth/VoxelFineTileStreamer.cpp` — new
`DiagnoseNamespace()`, wired into **both** the fatal and the non-fatal gate-leak
reports.

On failure it lists the cache root (one directory listing, failure path only,
`std::error_code` throughout so a diagnosis cannot become a second fault) and
formats each candidate id through the *same* `vxc::formatFineTileCacheKey` the
loader uses, so it cannot drift from the real path grammar. Four distinct
outcomes, because the old message printed one path for all of them and the
operator had to guess which:

1. **Namespace mismatch — the tile is baked, under a different id.** Names the
   configured id, says whether it has a directory under this root *at all*,
   counts the namespaces that do hold the tile, explains that the id is a bake
   fingerprint and that `-VoxelFineTileDir` does **not** carry the id with it,
   and prints a copyable `-VoxelFineTileProviderId=<id>` line per candidate.
2. **The configured namespace does not exist under this root** — root or id is
   wrong for this run; they resolve independently.
3. **Namespace right, tile not baked** — a coverage gap, not an identity
   mismatch. Names the bake command with the coordinates filled in.
4. **The cache root itself is not a directory** — check `-VoxelFineTileDir`.

Before: `needs tile (-3,-4), which is not resident and could not be loaded from
D:/voxelsim/tile-cache/...-bdcab4bed/...`

After, for the same failure: *"NAMESPACE MISMATCH — THIS TILE IS BAKED, UNDER A
DIFFERENT PROVIDER ID. This run is configured for fine provider id
`...-bdcab4bed` (which has NO directory under this cache root at all), but
D:/voxelsim/tile-cache holds tile (-3,-4) for seed 000000000135276f under 1
OTHER namespace(s). … Re-run with exactly one of:
`-VoxelFineTileProviderId=terrain-diffusion-unlabeled-80b9ca451a23eae4-b19d281fd`"*

`VoxelFineTileStreamer.cpp` compiles clean. **Note:** the module build currently
fails in `VoxelGpuVerify.cpp` on a pre-existing `static_assert`
(`kWorldGenVersion` moved without `kExpectedCpuDigest` being re-measured, from
the worldgen v27 commit). That is unrelated to this change and outside this
work's file scope — confirmed by stashing, and by `VoxelFineTileStreamer.cpp`
compiling as step 14/19 with no diagnostics.

---

## 6. The owner decision: what happens to the ini pin

`ue-project/Config/DefaultGame.ini` is outside this work's write scope, and the
choice below is a policy call, not a bug fix. Three options:

**A. Update the pin, keep the mechanism.** Set
`DefaultFineTileProviderId=terrain-diffusion-unlabeled-80b9ca451a23eae4-b19d281fd`
and `DefaultFineTileDir=D:/voxelsim/tile-cache` **together, as a pair**.
*Cost:* it goes stale again at `bake_ver` 29 — this is the fourth time. *Benefit:*
zero new machinery; the ini stays the single deliberate pin for shipping, which
is what it is for.

**B. Make the pair atomic.** Treat `DefaultFineTileDir` +
`DefaultFineTileProviderId` as one setting: if either is given on the command
line, require both, and refuse at startup rather than silently cross-producing
them. *Cost:* breaks the "scratch bake inherits the project id" convenience the
current comment defends, and needs a `VoxelWorldSubsystem.cpp` change (another
agent's file). *Benefit:* kills tonight's compounding cause outright — the
cross-product becomes unrepresentable.

**C. Let the engine read the record.** On startup, if no id is given, scan
`<FineTileDir>/*/<seed:016x>/world-identity.json` and adopt the newest — the
authority, read not recomputed. *Cost:* a startup directory scan; ambiguity
needs a tie-break policy; a shipping build silently adopting whatever was baked
last is arguably worse than a deliberate pin, and would have hidden rather than
surfaced this bug. *Benefit:* no hand-copied constant anywhere.

**Recommendation: A now, B next.** C is the wrong default for a shipping client
for the same reason the ini pin exists at all — a product should render the
terrain it was *shipped* with, not whatever a dev baked last. B removes the sharp
edge without giving that up. The `DefaultGame.ini` comment block should also drop
its stale "17 of 289 tiles are baked" tile list, which describes 2026-08-02.

---

## 7. Interim rule for capture harnesses — apply now

**Never let the provider id fall through to the ini when you override the cache
dir.** They are a pair. Resolve the id from the cache you are actually pointing
at, every run:

```powershell
$Tiles = "-3,-4 -4,-4"          # the tiles the camera will touch
$Id = python D:/voxelsim/terrain-service/tools/resolve_fine_namespace.py `
          --cache-dir $CacheDir --seed $Seed --tiles="$Tiles" --newest
if ($LASTEXITCODE -ne 0) { throw "no baked fine namespace for $Tiles - bake first" }
... -VoxelFineTileDir=$CacheDir -VoxelFineTileProviderId=$Id
```

Three rules, each paid for:

1. **Pass both flags or neither.** Overriding only the dir is what produced a
   path that had never existed.
2. **Do not `catch` your way past a non-zero exit.** The gate is fatal in
   unattended runs; a wrong id is indistinguishable from an unbaked tile and
   kills the run at the first elevation query.
3. **Pass `--tiles` with the tiles the camera will actually touch** — including
   neighbours for an off-centre camera (a tile-edge column needs all four
   quadrants). That filter is the only thing that tells a stale pin apart from a
   coverage gap.

`tools/voxel-capture.ps1` currently defaults `-FineProviderId` to `''`, which is
exactly the fall-through above. It is outside this work's file scope; wiring it
to the resolver is the single highest-value follow-up.
