# Voxel Earth (voxelsim)

A multiplayer, Earth-realistic, procedurally generated world made of 10cm cubic
voxels, where everything is destructible. Terrain realism comes from
[terrain-diffusion](https://github.com/xandergos/terrain-diffusion) (learned
diffusion terrain at 30m/px) amplified down to 0.1m by bit-deterministic
integer math.

Read [docs/voxel-earth-implementation-plan.md](docs/voxel-earth-implementation-plan.md)
first — it is the project's source of truth (vision, architecture doctrine,
milestones). Current milestone status lives in [docs/status.md](docs/status.md).

## Repo layout

```
/terrain-service/   Python: terrain-diffusion worker, Flask tile API, disk cache
/voxel-core/        C++20, UE-header-free, CMake: bricks, palettes, amplifier, mesher, editlog
/voxel-core/bench/  headless benchmark + determinism harness
/ue-project/        UE5 project consuming voxel-core as a module (M1+, not yet created)
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
