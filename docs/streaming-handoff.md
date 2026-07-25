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
| **G3 — drive the pool from the cascade** | ⬜ **NEXT.** Plan in `docs/gpu-g3-integration-plan.md` |
| G4 parity | ⬜ Checklist done; two real blockers, see below |
| G5 — flip the default, retire per-chunk components | ⬜ |

## Start here

The pool is done and the incremental path under it is proven. G3 is now
ordinary integration work: wire `VoxelWorldSubsystem`'s `ApplyMeshResult` and
`DrainUnloads` to `AddChunk` / `UpdateChunk` / `RemoveChunk` behind a
`voxel.Stream.GPU` cvar defaulting off. File-level seams are in
`docs/gpu-g3-integration-plan.md`.

Two things G3 needs that already exist and are easy to miss:

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
4. **`VoxelEarthShaders` must stay `PostConfigInit`**, and `/VoxelCore` is a
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
