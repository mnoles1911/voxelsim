# Milestone gate status

Updated whenever gate-relevant work lands. See the implementation plan §4 for
gate definitions.

## M0 — Core + proof of numbers (IN PROGRESS)

| Gate | Status | Notes |
|---|---|---|
| Amplify+mesh 128m radius < 1s on RTX 3060 | ⬜ open | CPU reference done; GPU compute port not started (needs GPU machine). Baseline (2026-07-19, single-threaded CPU ref, container hardware): 128m radius = **10.6s** with 8³ bricks, **14.9s** with 16³ (amplify 4.3s / mesh 5.9s dominate at 8³). GPU port + parallel columns must close ~10–15×, which is the expected shape of the win. |
| Bit-identical amplifier output NVIDIA vs AMD | ⬜ open | No GPU port yet. Interim proxy PASSING: gcc and clang builds produce bit-identical world+mesh digests (CI job `determinism-cross-compiler`), goldens pinned in tests. Real gate needs NV + AMD runners. |

### Early 8³ vs 16³ data (CPU ref, will re-decide after GPU port)

At 128m radius: 8³ = 129.7k surface-shell bricks, 36.5M solid voxels, 4.53M
quads, 10.6s total; 16³ = 32.5k bricks, 69.9M solid voxels (taller bricks
capture more buried volume), 4.35M quads, 14.9s (meshing 16³ slices costs
~2× despite 4× fewer bricks). CPU ref favors 8³ on generation cost; final
call needs GPU meshing + render/memory numbers (M1).

### M0 task checklist (plan §5)

- [x] Repo scaffold (monorepo layout, docs, CI)
- [x] Tile API: `GET /tile?seed&x&y&scale` → elevation int16 + climate uint8[4]; disk cache; golden-tile regression test (synthetic provider; real terrain-diffusion worker pending GPU machine)
- [x] Brick storage + palette + bitmask; property tests; 8³ vs 16³ benchmark
- [x] Amplifier v0: column stratigraphy + integer-hash fractal detail (fixed-point only, hash documented in docs/determinism.md). CPU reference first; GPU port pending
- [x] Greedy mesher (CPU ref); bricks/sec + 128m-radius wall-clock in bench
- [x] Edit overlay + append-only log format (versioned, RLE brick diffs) + replay test
- [ ] terrain-diffusion worker running (GPU machine)
- [ ] GPU compute port of amplifier + mesher (GPU machine)
- [ ] Cross-vendor (NV vs AMD) determinism CI

## M1+ — not started

## Water track — not started (W1 begins with M1)
