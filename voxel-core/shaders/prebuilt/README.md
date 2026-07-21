# Prebuilt SPIR-V (ADR-0001 M0 cross-vendor determinism gate)

These 7 `.spv` files are the compiled output of `voxel-core/shaders/worldgen.hlsl`
(ColumnMain, VoxelizeMain, MeshCountMain, MeshEmitMain, ScanBlocksMain,
ScanSumsMain, ScanAddMain), committed here — rather than left in the
gitignored `build/shaders/` directory `tools/compile-shaders.ps1` normally
writes to — so a Linux/NVIDIA box can run `vxc_gpu` without needing DXC
(Windows-only tool) at all. SPIR-V is portable, vendor- and OS-neutral
bytecode: the SAME bytes dispatched on an NVIDIA GPU must byte-match the CPU
reference the same way they already do on AMD, closing the NVIDIA≡AMD leg of
the M0 determinism gate transitively (both legs agree with the CPU reference
⇒ both legs agree with each other).

## 2026-07 respin: signed `%` vendor-divergence fix (ColumnMain only)

`worldgen.ColumnMain.spv` was recompiled after `worldgen.hlsl`'s `floorDiv`
was rewritten to stop depending on signed `/` and `%` with mixed-sign
operands (HLSL leaves `%` undefined unless both operands share a sign). That
dependency is what failed the NVIDIA leg of this gate: on an RTX 4090 the
flooring correction never fired, so `floorDiv` silently truncated for every
NEGATIVE world coordinate. The other six `.spv` files are byte-identical to
the previous respin — the change only touches ColumnMain's call graph.

The **CPU reference did not change**, so `vxc::kWorldGenVersion` stays at 2
and no goldens move. Confirmed empirically: all three AMD-leg digests below
are bit-identical before and after the fix.

## 2026-07 respin 2: cross-vendor UB hardening (ColumnMain, MeshCount, MeshEmit)

A follow-up audit of `worldgen.hlsl` found five more constructs in the same
undefined-behavior family as the signed-`%` bug above — latent rather than
live, each currently masked by a host-side contract that nothing in the
shader enforced. All five are now guarded IN THE SHADER:

1. `decodeMask`'s interior-brick dims (`bricksX - 2u`) wrapped to
   `0xFFFFFFFF` for a region with fewer than 3 bricks on an axis, which the
   `maskIndex >= maskCount` range check then passed — early-out added.
2. The greedy run-length scan read `mask[64]`, one past `uint mask[64]`, on
   the last row's terminating iteration; safe only if `&&` short-circuits,
   which HLSL does not guarantee — hoisted into an explicit `if`/`break`.
3. `OutQuads[baseOffset + quadCount]` was an unclamped write driven by
   scanned offsets — now bounded by `OutQuads.GetDimensions` (the buffer's
   own length, so no new host/shader contract), and `MeshEmitMain` refuses
   to emit above `ScanCount`.
4. `PixelSizeMm` is a host-controlled divisor; zero would be an
   OpUDiv-by-zero — guarded in `ColumnMain` and asserted host-side in
   `voxel-core/bench/gpu_harness.cpp`.
5. `clamp64(..., 0, RasterSize.x - 1)` inverted its range to `[0, -1]` when
   the raster window was empty, returning -1 that the `(uint)` cast turned
   into a ~4-billion index — zero-extent early-out added.

These are GUARDS, not behavior changes: on every valid input each guarded
branch is unreachable, and all three AMD digests below reproduce bit-identically
(re-verified on this box after the change). `ColumnMain`, `MeshCountMain` and
`MeshEmitMain` respun; the other four kernels are byte-identical to respin 1.
The CPU reference is untouched — `vxc::kWorldGenVersion` stays at 2, no
goldens move.

`tools/lint-shader-ub.py` (CI job `shader-ub-lint`) now enforces this whole
class going forward, including an exact OpSDiv/OpSRem/OpSMod opcode audit of
the `.spv` files in this directory.

## Provenance

- Source: `voxel-core/shaders/worldgen.hlsl` at commit `8b834107701fd4a2a005b8dbd4a17352f44f26c1`
  ("M4: biome classification core + per-biome surface materials (round 1)")
- Compiler: Microsoft DXC, pinned version `v1.9.2602.24` (`tools/fetch-dxc.ps1`),
  `dxcompiler.dll: 1.9(5191-d355aa83)(1.9.2602.24) - 1.9.2602.24 (d355aa836)`
- Compile command: `tools/compile-shaders.ps1`'s SPIR-V invocation —
  `dxc -T cs_6_0 -E <Entry> -O3 -spirv -fspv-target-env=vulkan1.1
  -fvk-b-shift 0 0 -fvk-t-shift 1 0 -fvk-u-shift 3 0 worldgen.hlsl -Fo <out>.spv`
- Compiled and committed from: worktree HEAD `4b54e9f1118de70cec46b837e7e237547ef2fce8`
  (branch base: `main`, PR #40 "worldgen-vendor-ub-fix" merge) — ColumnMain,
  MeshCountMain and MeshEmitMain respun for the UB-hardening pass above.
  ScanAddMain/ScanBlocksMain/ScanSumsMain/VoxelizeMain are unchanged since
  worktree HEAD `a172baf54c8ebf45e99d281a539fdd63fcd5e7e6` (branch base:
  `main`, PR #34 "m6-pathfind" merge); the intervening respin
  (`354ea238a66caa1b5aa966632cd81c3f5070ee06`) touched ColumnMain only, for
  the signed-`%` fix.
- Verified: recompiling from the pinned DXC reproduces these bytes exactly,
  and `python tools/lint-shader-ub.py --spv-dir voxel-core/shaders/prebuilt`
  confirms ZERO `OpSDiv`/`OpSRem`/`OpSMod` across all seven modules. Every
  division that remains lowers to `OpUDiv`/`OpUMod` (24 `OpUDiv` in
  ColumnMain alone), which has exactly one legal result on any
  implementation.
- Built on: this Windows dev box (MSVC/VS 2026 toolchain elsewhere in the
  repo; DXC itself needs no MSVC — it's a standalone compiler)

## SHA-256

```
5f16bc95373ed6f585b068cf4ba93666eda2d8081e463e053a8b371e34768ce6  worldgen.ColumnMain.spv
b3fdca92cefc9ee1967bb0d0b4bbef377d0183224e60223c32cfb41c2ee6981b  worldgen.MeshCountMain.spv
f5d1168c83935f7c35094f30f27012057aa8ea80d83130afeaa945a46e03bacd  worldgen.MeshEmitMain.spv
72cb57ed63c531b0745f109e1b7c9a2054ed1e201b0ef59dbb062171ed207130  worldgen.ScanAddMain.spv
0168a302618b437e31b0b4e8bf69aa4ea203383dfd6d1e38b13acd0618d277e1  worldgen.ScanBlocksMain.spv
e2060888d9938627c0e5c899d4402c804aeee61aae04b86ffcf9c3c5ec4cdf8e  worldgen.ScanSumsMain.spv
729fc4f21e7fcc323545f9977512827f2acc53b15a83c82bc78b8ccfade6b0c1  worldgen.VoxelizeMain.spv
```

Verify with `sha256sum -c` (or `Get-FileHash -Algorithm SHA256` on Windows)
against this list before trusting a copy of these files.

## IMPORTANT: these are NOT byte-identical to the digests previously recorded
## in docs/status.md

`docs/status.md`'s "Bit-identical amplifier output NVIDIA vs AMD" row records
GPU-output digests (`e1db29a9b6874012` at `--radius 64`, `583e91d62cefb8a9`
at `--radius 128`) from **before** commit `8b83410` ("M4: biome
classification core + per-biome surface materials, round 1") landed. That
commit added new materials/biome constants to `worldgen.hlsl` itself (see
`git diff 34c9de2..8b83410 -- voxel-core/shaders/worldgen.hlsl`), which
legitimately changes the GPU output — the old digests are stale relative to
current `HEAD`, not a bug in these shaders.

**What was actually verified** (2026-07 worldgen v3 pass, on this box, fresh
rebuild from the pinned DXC + current `worldgen.hlsl`, using these exact
committed `.spv` files via `vxc_gpu --spv .../prebuilt/...`):

| Mode | Result | GPU output digest (columns+cells+quads) |
|---|---|---|
| default (2 regions, column+cell+quad compare) | **PASS**, 0 mismatches, 8192 columns / 360448 cells / 4997 quads | `e21e2767591496eb` |
| `--radius 64` | **PASS**, 0 mismatches, 305/305 dispatch entries (144 tiles + 161 z-slabs, 100%) verified, gate 0.093-0.096s (< 1s target) | `346b60c292a26b5a` |
| `--radius 128` | **PASS**, 0 mismatches, 136/1089 entries (12.5%, every-8th sampled) verified, gate 0.182-0.215s (< 1s target) | `75b737e961f65bf5` |

Device: AMD Radeon RX 7800 XT.

At kWorldGenVersion 2 the same three modes measured: default (128x128
regions) `1dbcabb01cfaf2bc`, `--radius 64` `95a82ba20200f6f2` (144 tiles),
`--radius 128` `b4c8ec5d0966894b` (529 tiles) — superseded by the v3 table
above; see the next section for why every digest legitimately moved.

Re-verified UNCHANGED after the 2026-07 signed-`%` fix above (same box, same
three modes, freshly rebuilt `vxc_gpu` against these committed `.spv`): all
three digests reproduced exactly — `1dbcabb01cfaf2bc`, `95a82ba20200f6f2`,
`b4c8ec5d0966894b`. That the AMD digests are byte-identical across the fix is
the proof that the fix is a no-op on the vendor that was already correct, and
therefore cannot move any golden.

Re-verified UNCHANGED AGAIN after "respin 2: cross-vendor UB hardening" above
(same box, same three modes, `vxc_gpu` rebuilt against the respun `.spv`):
`1dbcabb01cfaf2bc`, `95a82ba20200f6f2`, `b4c8ec5d0966894b`. Since every added
guard is unreachable on valid input, identical digests are the expected
result and the evidence that the guards changed no behavior.

## 2026-07 re-verify 3: worldgen v3 spectral-gap terrain (kWorldGenVersion 2 -> 3)

The **SPIR-V did not change** in this pass — all seven `.spv` files and the
SHA-256 table above are byte-identical to respin 2. What changed is the CPU
reference itself: `SyntheticTileSampler` gained four elevation octaves at
480/240/120/60 m wavelength (the terrain-realism audit's spectral-gap fix,
`vxc::kWorldGenVersion` 2 -> 3). The tile raster is host-generated and
uploaded as a buffer, so the shaders consume the new terrain with zero
shader edits; every digest legitimately moved because the WORLD moved, and
the gate re-passed bit-exact on the same committed bytecode.

Two `gpu_harness.cpp` changes rode along, both forced by the rougher v3
relief and affecting dispatch shapes (never per-quad output):

1. **Gate mode splits tall tiles into z-slabs** (`ZWindow`,
   `kMaxSlabInteriorLayers = 6`). v3 terrain routinely produces gate tiles
   taller than 6 interior brick layers (125/144 tiles at `--radius 64`),
   i.e. maskCounts past ScanSumsMain's single-workgroup 65,536-mask
   capacity. The gate path previously had NO guard for that (unlike
   `runMeshChain()`'s FATAL): masks past the capacity silently missed their
   scanned block base, MeshEmitMain wrote their quads at bogus offsets on
   top of the early stream, and the corruption was nondeterministic across
   runs. This was a live, silent correctness bug in the harness, latent at
   v2 only because v2's flat terrain never exceeded 6 interior layers.
   Slabs partition a tall tile's interior layers exactly (1-brick shared
   halo, ascending z, inline in the work order), so every brick is still
   meshed exactly once and the digest/compare order stays deterministic.
   prepTileCpu now FATALs if a dispatch could still exceed the capacity.
2. **Default regions shrank from 128x128 to 64x64 columns** — the origin
   fixture at v3 needs 10 brick layers (75,264 masks), which tripped
   `runMeshChain()`'s FATAL. At 64x64 (6x6-brick interior, 1,728 masks per
   layer) the capacity needs >37 interior layers to overflow: unreachable
   for surface terrain. The gate mode still exercises the full 128x128
   dispatch shape.

These three digests — not the older status.md ones — are the values the
NVIDIA leg (`tools/run-nvidia-digest.sh`) must reproduce to close the M0
cross-vendor gate (run it from a checkout at or after worldgen v3).

## Regenerating

```powershell
tools/fetch-dxc.ps1              # once, pinned v1.9.2602.24
tools/compile-shaders.ps1        # writes build/shaders/*.spv (DXIL + SPIR-V)
# then copy the 7 worldgen.*.spv files here and update this README's hashes.
```

Bump the DXC pin (`tools/fetch-dxc.ps1`) and regenerate deliberately —
shader binaries are part of the determinism surface (ADR-0001).
