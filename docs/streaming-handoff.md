# Handoff: G2 closed and verified, G3 is next (2026-07-25)

Supersedes the earlier 2026-07-25 handoff. All work is on
**`claude/terrain-holes-wip`**. Build is clean; everything below is in the binary
and verified headless.

## State

| Item | Status |
|---|---|
| Concentric rings of holes | ✅ Root-caused and fixed (ring-seam admission, `749ad7b`) |
| ADR-0006 | ✅ Accepted, signed, diagnosis measured not asserted |
| **G1 — GPU greedy mesher** | ✅ Complete. Gate green, bit-exact with the CPU mesher |
| **G2a — kernels in-engine via RDG** | ✅ Complete. Cross-toolchain digest `f3c48a4df3e20e9a` |
| **G2 — GPU geometry pool + custom draw** | ✅ **Complete.** 256 chunks / 876k quads, ONE primitive, ONE draw |
| **G2 incremental upload** | ✅ **Verified.** Partial writes match a full rebuild exactly |
| **G3 — drive the pool from the cascade** | 🟨 **Wired and rendering, not yet correct.** 2893 chunks / 2.4M quads / ONE draw. Coarse rings render as flat slabs. |
| G4 parity | ⬜ Checklist done; two real blockers, see below |
| G5 — flip the default, retire per-chunk components | ⬜ |

## Start here

G3 is wired and the pooled cascade renders. **One bug is left, and it is
precisely located: the R0 ring stops populating.**

Same 30 s headless run, same spawn, only `voxel.Stream.GPU` differing:

| Ring | CPU path | GPU pool |
|---|---|---|
| **R0** | **1947** loaded, 0 pending | **255** loaded, 2 pending |
| R1 | 1319 | 1319 |
| R2 | 1602 | 1602 |
| R3 | 1853 | 1853 |
| R4 | 1675 | 1675 |
| R5 | 1426 | 1426 |

Every coarser ring is byte-identical. Only R0 differs, and `pending` is ~0, so
it is **not** a throughput stall -- the cascade simply stops *asking* for R0
chunks. That makes this a desired-set / admission bookkeeping difference, not a
rendering one, and it is why the picture still looks wrong: the near-field
detail never arrives, leaving the coarse rings' exposed interiors on screen as
large flat slabs. Chasing the slabs is chasing the symptom.

Already ruled out:

- **Coverage/retention** (`ReplacementCovered`) keys on `bMeshSettled`, not on
  the component, so it is already path-agnostic.
- **The three load-bearing `Component.IsValid()` sites** now ask
  `FChunkRecord::HoldsGeometry()` instead (retention gate, unload budget, and
  the record-drop check -- that last one would have leaked pool allocations).
- **Throughput.** The ~170 MB-per-update copy bug that did throttle streaming is
  fixed; it took the pool from 2893 to 8130 chunks and R0 pending from 66 to 2.
  R0's *loaded* count barely moved, which is what shows the remaining problem is
  admission, not speed.

Sharper still, from the ring-dispatch line (`total` = chunks that produced a
result at all, loaded + zero-quad):

```
CPU:  R0 total=3398 load=1947 zq=1451
GPU:  R0 total= 408 load= 255 zq= 152
```

So R0 is not failing to *draw* 3,000 chunks -- it never *meshes* them. `tracked`
is correspondingly lower (11,323 vs 16,417), and `bandCache` is 171 vs 1,288.
Records are going missing upstream of any geometry handoff.

And it is specifically R0-being-pooled that hurts R0: with
`voxel.Stream.GPUMaxLevel 0` (only R0 pooled, R1-R5 on components) R0 falls
further still, to 178. Whatever this is, it is triggered by a level-0 chunk
taking the pooled branch, not by the pool's size or by the coarse rings.

Things checked and NOT the cause: `DropFarthestOverCap`'s admission guard (a
never-meshed chunk has neither a component nor a pool slot, so `HoldsGeometry()`
is false there on both paths, exactly as before); `ReplacementCovered`; the
per-update copy throttle (fixed, and it moved `pending`, not `total`).

The next thing I would do is diff what the pooled branch of `ApplyMeshResult`
leaves on `FChunkRecord` against what the component branch leaves, field by
field, for a level-0 chunk -- the return value included, since `DrainResults`
uses it for its apply budget and hitch attribution. The component branch also
`MoveTemp`s `Quads` and the pooled branch does not, which is a real behavioural
difference in what the caller sees afterwards.

Two tools for narrowing it:

- `voxel.Stream.GPUMaxLevel N` pools only rings <= N and leaves the rest on the
  component path. Both renderers coexist per chunk, so R0 can be isolated
  directly.
- `voxel.Stream.GPUMaxChunks N` caps pool admission, so the streamed path can be
  run at the small scale `voxel.GPU.SpawnPool` is known good at. That is what
  proved the earlier blank screen was never a scale problem.

Two things G3 uses that already exist and are easy to miss:

- **`UpdateChunk(Handle, Quads)`** re-meshes in place when the new quad count
  fits the existing slot, and only reallocates when a chunk outgrows it. Use it
  for digs. Implementing a re-mesh as Remove+Add fragments the pool hardest on
  exactly the chunks that re-mesh most.
- **`RingPresets`** (`VoxelWorldSubsystem.h:86`) is `static constexpr` and must
  become a runtime accessor before R0 can move to 128 m. Fold that into G3;
  do not flip R0 before G3 lands.

## Verification recipes

Build (close the editor first — it locks the DLL, and a failed link DELETES it):

```
"D:/UE_5.8/Engine/Build/BatchFiles/Build.bat" VoxelEarthEditor Win64 Development \
  -Project="D:/voxelsim/ue-project/VoxelEarth.uproject" -WaitMutex -NoHotReloadFromIDE
```

**Pass the project path as an ABSOLUTE path when launching headless.** A relative
`VoxelEarth.uproject` fails with exit code 1, no log, and no crash report, which
reads exactly like a broken build and is not one.

Cross-toolchain gate, ~30 s:

```
UnrealEditor-Cmd.exe "D:/voxelsim/ue-project/VoxelEarth.uproject" -game -nosplash \
  -unattended -sm6 -ExecCmds="voxel.GPU.VerifyRegion, quit"
```
Both legs must print `f3c48a4df3e20e9a` (bench: `build/voxel-core-msvc/bench/vxc_gpu.exe --radius 64`).

Pool, three modes — the third is the one that matters:

```
UnrealEditor.exe "D:/voxelsim/ue-project/VoxelEarth.uproject" -game -windowed \
  -resx=1280 -resy=720 -nosplash -unattended -sm6 "-VoxelSpawnAt=-84480,53760" \
  -ExecCmds="voxel.GPU.SpawnPool 64 churnlive shot"
```
`shot` writes `Saved/Screenshots/WindowsEditor/ScreenShot00000.png` at 10 s.

- `voxel.GPU.SpawnPool 64 shot` — plain pool.
- `... 64 churn shot` — edits **before** the proxy exists. One full rebuild.
  Does **not** test the incremental path.
- `... 64 churnlive shot` — edits **after** the proxy is up. This is the path
  streaming uses. Pass = the log shows `Live churn`, the `upload:` line count
  does **not** increase after it, and the image matches the `churn` image.

## Four traps that cost real time here — do not rediscover

1. **These buffers must be `EBufferUsageFlags::Static`, never `Dynamic`.**
   Only the static lock path honours a lock offset: it stages exactly the locked
   size and `CopyBufferRegion`s it to that offset (`D3D12Buffer.cpp:750, :801,
   :818`). The dynamic path ignores the offset and returns the buffer's base
   address (`:659`), then renames the whole buffer on every lock after the first
   (`:667, :697`), leaving everything outside the dirty range uninitialised.
   "We write it every frame, so mark it Dynamic" is the intuitive and wrong call.
2. **Screenshot the pool at 10 s, not 3 s.** The pool spawns into the live
   streamed world. A shot taken before the CPU cascade fills shows coarse LOD
   slabs around the pool that read exactly like corrupted pool geometry. That
   cost an hour of chasing a bug that was not there.
3. **`SetRootComponent()` on a fresh `NewObject`ed component installs an
   identity transform**, redefining the actor's location as the world origin —
   8,448 km away, i.e. invisible. Call `SetWorldLocation()` after
   `RegisterComponent()`.
4. **The pool must be the ROOT component of its own actor.** Attached as a
   child of `ChunkRoot` the primitive never enters the visible set at all --
   `GetDynamicMeshElements` is never called, with a live proxy and valid
   bounds. Same trap as (3): `SetWorldLocation` after `RegisterComponent`.
5. **Bounds do not reach the renderer by themselves.** On the incremental path
   the proxy is created once and never rebuilt, and `UpdateBounds()` only
   updates the component -- the scene keeps the bounds it got on frame 1. That
   culled a 6.4 m box at the player's feet while 2.4M quads sat in the buffer.
   `MarkRenderTransformDirty()`, never `MarkRenderStateDirty()` (which rebuilds
   every buffer and defeats the incremental path).
6. **The chunk table is float32 and this world is 84 km wide.** At ~8.4M UU the
   ULP is 1.0 UU against a 10 UU voxel, so chunk origins are stored relative to
   a rebase origin the component carries in its double-precision transform.
7. **`VoxelEarthShaders` must stay `PostConfigInit`**, and `/VoxelCore` is a
   directory *mapping*, not a copy. Copying `worldgen.ush` into the project would
   void the cross-toolchain digest the moment the copies drift.

## G4 parity: the two genuine blockers

Everything else on the checklist is computable in-shader. These two are not:

- **Voxel GI light field** must become a GPU-readable volume. Today the CPU path
  bakes it into vertex colour G.
- **Per-chunk debug tints and ring fade** need a per-chunk buffer. Pooling
  collapses the per-primitive material instances they use today — the chunk
  table is the natural place, and it already carries origin + climate.

Water surface is a separate, near-identical task to the terrain pool.

## Open, not blocking G3

- **Deep-column waste:** 44.6% of R0 worker time meshes buried chunks emitting
  zero quads. Measured ceiling: **+34.8%** useful throughput. Safe path is moving
  the exact band skip from dispatch-time to admission-time; needs a downward
  escape hatch (`EditedFootprintMinZ` → widen `ChunkZMin`) first.
- **Residual perf cost of the seam fix:** p50 14.92 → 17.52 ms, 968 → 703
  chunks/s. Real extra chunks where holes used to be. Exactly what ADR-0006
  removes.
- **Bounded-admission straggler:** a few chunks never load until the player
  moves. Diagnosed to the refill path, not fixed.
- **Min-spec-proxy M1 gate re-run** still owed; current numbers are
  default-quality and cannot be read against the historical gate rows.
- **`status.md` determinism row is stale** — goldens predate worldgen v6.
- **NVIDIA CI leg** for the G1 gate.

## Two methodology rules earned the hard way

1. **Discard the first run after any build.** Cold PSO state costs ~20%
   throughput and reads exactly like a regression.
2. **Never A/B this renderer through scalability cvars.** `r.ShadowQuality 0`
   desyncs the BeginPlay PSO precache and measures precache invalidation instead.
   Change one primitive flag, or use `r.ScreenPercentage`.
