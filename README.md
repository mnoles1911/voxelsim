# Voxel Earth (voxelsim)

A multiplayer, Earth-realistic, procedurally generated world made of 10cm cubic
voxels, where everything is destructible. Terrain realism comes from
[terrain-diffusion](https://github.com/xandergos/terrain-diffusion) (learned
diffusion terrain at 30m/px) amplified down to 0.1m by bit-deterministic
integer math.

Read [docs/voxel-earth-implementation-plan.md](docs/voxel-earth-implementation-plan.md)
first — it is the project's source of truth (vision, architecture doctrine,
milestones). Current milestone status lives in [docs/status.md](docs/status.md),
and everything known-and-not-done is in [docs/backlog.md](docs/backlog.md) —
including a "measured and CLOSED" section for options already ruled out.

## Repo layout

```
/terrain-service/   Python: terrain-diffusion worker, Flask tile API, disk cache
/voxel-core/        C++20, UE-header-free, CMake: bricks, palettes, amplifier, mesher, editlog
/voxel-core/bench/  headless benchmark + determinism harness
/ue-project/        UE 5.8 project (VoxelEarth) consuming voxel-core as a static lib
/docs/              implementation plan, ADRs, milestone status
```

## Building voxel-core

Requires CMake ≥ 3.20 and a C++20 compiler (gcc 13+ / clang 17+ / MSVC 19.36+).

```sh
cmake -S voxel-core -B build/voxel-core -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/voxel-core
ctest --test-dir build/voxel-core --output-on-failure
./build/voxel-core/bench/vxc_bench --radius 32   # benchmark + determinism digest
```

## Building ue-project (Windows, UE 5.8)

Build voxel-core first (its static lib is linked by the UE module), then:

```sh
"<UE_5.8>/Engine/Build/BatchFiles/Build.bat" VoxelEarthEditor Win64 Development -project="<repo>/ue-project/VoxelEarth.uproject"
```

Visual verification without the editor: run the game with
`-VoxelScreenshotAfter=<seconds>` (screenshots land in
`ue-project/Saved/Screenshots`); `-VoxelDefaultMaterial` isolates material
issues. The editor hosts the native Model Context Protocol server on
`http://127.0.0.1:8000/mcp` (auto-start; `.mcp.json` at repo root connects
Claude Code to it).

## Controls

The test pawn has a **fly** mode (default) and a **walk** mode; `G` toggles.
Fly clips through everything -- terrain, water, debris. Walk has gravity and
voxel collision, and holds position instead of falling when the ground under
it has not streamed in yet (the HUD says so).

| Key | Action |
| --- | --- |
| `W` `A` `S` `D` | Move |
| `Space` / `LeftCtrl` | Fly up / down (`Space` is jump in walk mode) |
| Mouse | Look |
| `LeftShift` (hold) | Boost -- 4x fly speed, sprint on foot |
| `LeftAlt` (hold) | Precision -- 0.15x fly speed, for 10 cm inspection |
| `]` / `[` | Fly speed step, 9 steps from 0.5 m/s to 2 km/s |
| `G` | Toggle walk / fly |
| `C` | Toggle first / third person |
| `F1` | Toggle the in-game debug overlay |
| `Up` `Down` | Overlay: move selection |
| `Left` `Right` / `Enter` | Overlay: change the selected setting |
| `F3` | Cycle `voxel.Debug` 0 -> 1 (perf HUD) -> 2 (+ 3D layers) |
| `LMB` / `RMB` | Dig / place |
| `1` `2` `3`, mouse wheel | Dig-and-place cube size |
| `T` | Cycle placement material |
| `F` (hold, release) | Charge and throw an explosive |

The overlay is OFF by default and lists these keybinds on itself. It shows
position/altitude, which diffusion tile you are in, per-ring loaded/pending
counts, the residency of the chunk you are standing in, and -- worth checking
first whenever the world looks wrong -- whether the run booted on **real
tiles** or fell back to the **synthetic sampler**. It toggles the same
`voxel.Debug.*` / `voxel.GI.Enabled` cvars the console does, plus wireframe.

Headless equivalents, for capturing any of the above without a keyboard:
`-VoxelWalkModeAfter=<s>`, `-VoxelFlySpeedStep=<n>`, `-VoxelOverlayShot=<s>`
(captures *with* UI, then quits), `-VoxelOverlayRow=<n>`, `-VoxelHudShotOnly`,
`-VoxelOverlayOn`.

## Running terrain-service

```sh
cd terrain-service
python3 -m pip install -r requirements.txt
python3 -m pytest                       # golden-tile regression tests
python3 -m flask --app terrain_service.app run   # GET /tile?seed=1&x=0&y=0&scale=1
```

The synthetic tile provider is the default in dev; the real terrain-diffusion
GPU worker is wired in on a CUDA machine (see terrain-service/README.md).

## Doctrine (short form — full version in the plan, §2)

1. The world is a deterministic function, not data; only diffs are stored/sent.
2. Never replicate voxels.
3. Everything below the 30m tiles is bit-deterministic integer math. This is
   the load-bearing wall of multiplayer — CI enforces it.
4. One authority path for world changes: the edit log.
5. Everything expensive is budgeted, never demand-driven.
6. Offload to server only what is cacheable and client-independent.

Deviations require an ADR in `docs/adr/` and human sign-off.
