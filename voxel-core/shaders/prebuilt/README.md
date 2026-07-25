# Prebuilt SPIR-V (ADR-0001 M0 cross-vendor determinism gate)

These 7 `.spv` files are the compiled output of `voxel-core/shaders/worldgen.ush`
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

`worldgen.ColumnMain.spv` was recompiled after `worldgen.ush`'s `floorDiv`
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

A follow-up audit of `worldgen.ush` found five more constructs in the same
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

- Source: `voxel-core/shaders/worldgen.ush` at commit `8b834107701fd4a2a005b8dbd4a17352f44f26c1`
  ("M4: biome classification core + per-biome surface materials (round 1)")
- Compiler: Microsoft DXC, pinned version `v1.9.2602.24` (`tools/fetch-dxc.ps1`),
  `dxcompiler.dll: 1.9(5191-d355aa83)(1.9.2602.24) - 1.9.2602.24 (d355aa836)`
- Compile command: `tools/compile-shaders.ps1`'s SPIR-V invocation —
  `dxc -T cs_6_0 -E <Entry> -O3 -spirv -fspv-target-env=vulkan1.1
  -fvk-b-shift 0 0 -fvk-t-shift 1 0 -fvk-u-shift 3 0 worldgen.ush -Fo <out>.spv`
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

Current as of respin 5 (worldgen v6, see the bottom of this file):

```
2ab01165f70bfc2dc5e5d1e66a4ee0c5fe58ac34778c1c3245d1fbc9b31c77f9  worldgen.ColumnMain.spv
b3fdca92cefc9ee1967bb0d0b4bbef377d0183224e60223c32cfb41c2ee6981b  worldgen.MeshCountMain.spv
e6213729a69289bb1fced82714d67f041cbc34bb2b649a01797bd9ed9185f8b6  worldgen.MeshEmitMain.spv
72cb57ed63c531b0745f109e1b7c9a2054ed1e201b0ef59dbb062171ed207130  worldgen.ScanAddMain.spv
0168a302618b437e31b0b4e8bf69aa4ea203383dfd6d1e38b13acd0618d277e1  worldgen.ScanBlocksMain.spv
e2060888d9938627c0e5c899d4402c804aeee61aae04b86ffcf9c3c5ec4cdf8e  worldgen.ScanSumsMain.spv
cdcf478bcf5bac50e19f516543cb8fade86f79d514c5586873bf40cafeab7df0  worldgen.VoxelizeMain.spv
```

> **Stale-entry correction (found during respin 5).** The previous table
> listed `f5d1168c83935f7c35094f30f27012057aa8ea80d83130afeaa945a46e03bacd`
> for `worldgen.MeshEmitMain.spv`, but the file committed in this directory
> hashed to `e6213729a69289bb…` — and respin 5 did not touch MeshEmitMain
> (`git diff` shows only ColumnMain and VoxelizeMain changed). So that entry
> had been stale since an earlier respin: MeshEmitMain's bytes moved without
> the table being updated. Anyone following this file's own "verify with
> `sha256sum -c` before trusting a copy" instruction would have hit a
> spurious mismatch. The value above is the one the committed file actually
> has, verified against `git show HEAD:...` before the respin overwrote
> anything.

Verify with `sha256sum -c` (or `Get-FileHash -Algorithm SHA256` on Windows)
against this list before trusting a copy of these files.

## 2026-07-25 respin: worldgen v8, the climate recalibration

`ColumnMain`, `VoxelizeMain`, `MeshCountMain` and `MeshEmitMain` were
recompiled after `worldgen.ush`'s biome/stratigraphy mirror moved to
`kWorldGenVersion` 8. The three Scan kernels are **byte-identical** to the
previous respin — they touch neither columns nor materials, which is the
expected signature of a change confined to ColumnMain's classification and
VoxelizeMain's layer lookup.

What moved in the shader, mirroring `biome.h` / `amplifier.cpp` exactly:

* every Whittaker/gate threshold, now the CONVERTED value of a physical
  constant (`kBiomeTempColdU8 = 143` is `climateTempU8FromDegC(5)`, and so on).
  HLSL has no `climate.h`, so these are transcribed — **run
  `vxc_dump_biome_constants` and paste, never hand-compute**;
* the cliff gate 6000 → 21000 (a 70% grade, ~35°, the angle of repose);
* the treeline base 2 600 000 → 900 000 mm and its per-u8 rate 20 000 → 47 058;
* gate ORDER: sea level now precedes slope, so seafloor can no longer classify
  alpine, and the cliff gate returns the new `BIOME_BARE_ROCK = 9`;
* the topsoil formula: a retained slope FRACTION instead of an absolute
  subtraction, with the clamp applied after the jitter;
* `stratigraphyAt` carries rock through the subsoil band under a rock surface.

**Verified on this box** (fresh DXC 1.9.2602.24 build from current
`worldgen.ush`, these exact committed `.spv`):

| Mode | Result | GPU output digest (columns+cells+quads) |
|---|---|---|
| default (2 regions) | **PASS**, bit-exact, 8192 columns / 393216 cells / 6668 quads | `6e893ab3679a8c81` |
| `--radius 64` | **PASS**, bit-exact, gate 0.137 s (< 1 s target) | `8200c4b066ce3219` |
| `--radius 128` | **PASS**, bit-exact, gate 0.197 s (< 1 s target) | `8eed2f023bc57365` |

Device: AMD Radeon RX 7800 XT.

sha256 of the respun files:

```
1e72b08bf53f92945512cf57e7f0477bbc092ccdc568c9b786634a2be33ff5a1  worldgen.ColumnMain.spv
052f9e8a0cc5964081123020d33ff1c74dfadef2aa5ff5d0fac02246fe3a0c1d  worldgen.MeshCountMain.spv
f45164aff1118f7c41bc579299fcc9273a0d141328143d3cb958e09645be4c28  worldgen.MeshEmitMain.spv
72cb57ed63c531b0745f109e1b7c9a2054ed1e201b0ef59dbb062171ed207130  worldgen.ScanAddMain.spv
0168a302618b437e31b0b4e8bf69aa4ea203383dfd6d1e38b13acd0618d277e1  worldgen.ScanBlocksMain.spv
e2060888d9938627c0e5c899d4402c804aeee61aae04b86ffcf9c3c5ec4cdf8e  worldgen.ScanSumsMain.spv
5bcdadb2e0b316bc73a03bc25a7f54ee4736ad4f64516026158e53d01c74a81b  worldgen.VoxelizeMain.spv
```

**NVIDIA leg still owed**, as it was before this change (`docs/gpu-streaming-plan.md`:
"NVIDIA CI leg is still owed — the only unmet part of the original gate"). This
respin does not make that worse, but the cross-vendor claim rests on one vendor
until `tools/run-nvidia-digest.sh` is run against these `.spv`.

## IMPORTANT: these are NOT byte-identical to the digests previously recorded
## in docs/status.md

`docs/status.md`'s "Bit-identical amplifier output NVIDIA vs AMD" row records
GPU-output digests (`e1db29a9b6874012` at `--radius 64`, `583e91d62cefb8a9`
at `--radius 128`) from **before** commit `8b83410` ("M4: biome
classification core + per-biome surface materials, round 1") landed. That
commit added new materials/biome constants to `worldgen.ush` itself (see
`git diff 34c9de2..8b83410 -- voxel-core/shaders/worldgen.ush`), which
legitimately changes the GPU output — the old digests are stale relative to
current `HEAD`, not a bug in these shaders.

**What was actually verified** (2026-07 worldgen v3 pass, on this box, fresh
rebuild from the pinned DXC + current `worldgen.ush`, using these exact
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


## 2026-07 respin 3: M4 cave pass (kWorldGenVersion 3 -> 4)

`worldgen.ush` gained a real code change this time — the M4 cave pass, a
bit-exact HLSL mirror of `voxelcore/caves.h` (`caveEdgeExists`,
`caveEdgeRadiusMm`, `caveColumnFor`, `caveCarveAt`), plus the split of the
old `materialAt` into `stratigraphyAt` (layer model) and `materialAt`
(layers minus caves). All seven modules were recompiled from the pinned DXC
`v1.9.2602.24` / `dxc_2026_05_27.zip`; **ColumnMain and VoxelizeMain moved,
the five mesher/scan modules did not** (`MeshCountMain`, `MeshEmitMain`,
`ScanBlocksMain`, `ScanSumsMain`, `ScanAddMain` are byte-identical to
respin 2 — their source is untouched and DXC is deterministic, which is
itself a useful control on the rebuild). ColumnMain moved only because the
cave functions are declared ahead of it in the same translation unit and
DXC's module-level output shifts; its *behavior* is unchanged, which the
harness confirms (see the default-mode digest below).

`python tools/lint-shader-ub.py voxel-core/shaders --spv-dir
voxel-core/shaders/prebuilt` is clean on the respun bytecode: still ZERO
`OpSDiv`/`OpSRem`/`OpSMod`. Every coordinate -> lattice mapping the cave
pass adds (`floorDiv(xMm, kCaveLatticeMm)`, the closest-approach
`floorDiv(sdx * num, den)`) goes through the approved helpers, and the
backbone/sinkhole selectors use a power-of-two AND mask precisely so that no
signed `%` is needed for the "every 4th lattice row" test.

| Mode | Result | GPU output digest (columns+cells+quads) |
|---|---|---|
| default (2 regions) | **PASS**, 0 mismatches, 8192 columns / 360448 cells / 4997 quads | `e21e2767591496eb` (UNCHANGED) |
| `--radius 64` | **PASS**, 0 mismatches, 305/305 entries (100%) verified — 4,997,120 columns / 270,663,680 cells / 2,523,983 quads compared — gate 0.104s | `1e664cf6680a137c` (was `346b60c292a26b5a`) |
| `--radius 128` | **PASS**, 0 mismatches, 136/1089 entries (12.5%) verified, gate 0.180s | `7602afe508d2ee73` (was `75b737e961f65bf5`) |

Device: AMD Radeon RX 7800 XT.

**Why default mode's digest did NOT move, and why that is correct.** The two
default regions voxelize only the surface-shell brick range the amplifier
reports (`origin` is 6 bricks = 48 voxels tall, `far-negative` 5 bricks).
The cave pass never carves shallower than 6 m (60 voxels) below a column's
own surface, and the only construct that reaches the surface at all — the
sinkhole shaft — occurs about once per 205 m square, so a 6.4 m region
almost never contains one. Default mode therefore contains no cave voxels,
and an unchanged digest there is the expected result. It does mean default
mode does **not** exercise the cave path: the `--radius 64` run is the one
that verifies it, and it does so over all 270 million cells with zero
mismatches.

## Respin 4 — caverns (C4), crevices (C2) and the 180-220 m bedrock band

`worldgen.ush` now mirrors the C2 crevices, the C4 folded cavern pass and
the `kWorldGenVersion` 5 bedrock move (40-60 m -> a jittered band centred on
200 m). `VoxelizeMain` grew 20,816 -> 71,808 bytes; `ColumnMain` moved for
the usual module-layout reason. The other five modules are byte-identical to
respin 3 again, the same free control as before.

**`kMaxCavernSegs` must track the CPU.** The CPU shrank it 6 -> 4 (tight ==
`kCavernChildCount`, the provable max) after the mirror was first written, so
the shader was briefly one constant out of step. A wider GPU cap is not a
harmless over-allocation: the cap decides *which* segments survive into
`ColumnSample`, so a GPU that admitted a 5th segment the CPU dropped would
diverge. Matching it to 4 is what the final SPIR-V here is built from.

| Mode | Result | GPU output digest (columns+cells+quads) |
|---|---|---|
| default (2 regions) | **PASS**, 0 mismatches, 8192 columns / 360448 cells / 4997 quads | `71288ec0ac6dba0b` (was `e21e2767591496eb`) |
| `--radius 64` | **PASS**, 0 mismatches, 305/305 entries (100%) verified — 4,997,120 columns / 270,663,680 cells / 2,523,983 quads compared | `f102b490a42918c0` (was `1e664cf6680a137c`) |
| `--radius 128` | **PASS**, 0 mismatches, 136/1089 entries (12.5%) verified, gate 0.202s | `1f88f5e0d405321d` (was `7602afe508d2ee73`) |

Device: AMD Radeon RX 7800 XT.

**Why default mode's digest DID move this time** — and why the respin-3
reasoning above does not carry over. Respin 3 argued (correctly, for caves)
that default mode voxelizes only a ~48-voxel surface shell well above any
carved geometry, so its digest should not move. That argument is specific to
*voxel* changes. `bedrockDepthMm` is a per-**column** field and the digest
covers columns as well as cells and quads, so moving the bedrock band shifts
the default digest regardless of which voxels get meshed. For a change of
this shape, a default digest that did *not* move would be evidence the
shader had not picked the change up at all.

`python tools/lint-shader-ub.py` is clean on its own merits (5 rules,
fail-closed); every `allow` annotation in the file carries a justification.
`worldgen.ush` still contains zero `float`/`double`/`half`.

## Respin 5 — coarse-to-fine detail rework (kWorldGenVersion 5 -> 6)

`worldgen.ush` gained the GPU mirror of the amplifier's coarse-to-fine
detail rework — the first change to the surface term made against REAL 30 m
terrain-diffusion tiles rather than `SyntheticTileSampler`. Three pieces:

1. `fadeFractionMm` / `valueNoise2Fade` — bit-for-bit mirrors of the new
   `hash.h` functions. Perlin's quintic `6t^5 - 15t^4 + 10t^3` on the lattice
   fraction, carried as a 10-bit fixed-point `t` so the `t^5` term stays
   inside int64. Raw `valueNoise2` is still present and still used by the
   cavern roughness channel, unchanged.
2. `microScaleQ10` — mirror of the new microrelief band scale.
3. The octave table went to five entries with new amplitudes, and
   `evalSurface`'s loop split into two bands scaled independently.

**Every division in the new code goes through `truncDiv` even though every
operand is provably non-negative** (`fx` is in `[0, latticeMm)` by
`floorDiv`'s contract, and the quintic is non-negative on `[0, 1]`).
"This operand happens to be positive today" is precisely the reasoning that
produced the M0 NVIDIA-vs-AMD divergence, so it is not reasoning this file
gets to use.

`python tools/lint-shader-ub.py voxel-core/shaders --spv-dir
voxel-core/shaders/prebuilt` is clean on the respun bytecode: still ZERO
`OpSDiv`/`OpSRem`/`OpSMod` across all seven modules. That is the check that
matters here, because `fadeFractionMm` adds four new divisions to the
hottest function in the shader.

**ColumnMain and VoxelizeMain moved; the other five modules are
byte-identical to respin 4** — the same free control on the rebuild as
previous respins (their source is untouched and DXC is deterministic).
ColumnMain 37,468 -> 56,084 bytes, VoxelizeMain 71,808 -> 90,464 bytes.

| Mode | Result | GPU output digest (columns+cells+quads) |
|---|---|---|
| default (2 regions) | **PASS**, 0 mismatches, 8192 columns / 393216 cells / 6666 quads | `f3c48a4df3e20e9a` (was `71288ec0ac6dba0b`) |
| `--radius 64` | **PASS**, 0 mismatches, 319/319 tiles (100%) verified — 5,226,496 columns / 289,406,976 cells / 3,058,001 quads compared | `591c7602bb9b0e62` (was `f102b490a42918c0`) |
| `--radius 128` | **PASS**, 0 mismatches, 143/1138 tiles (12.6%) verified — 2,342,912 columns / 126,877,696 cells / 1,389,322 quads compared | `424bca33dbbb37bd` (was `1f88f5e0d405321d`) |

Device: AMD Radeon RX 7800 XT.

All three digests moved, and all three had to: this change moves `surfaceMm`
at essentially every column, so it moves columns, the cells voxelized from
them, and the quads meshed from those. A digest that had NOT moved would be
evidence the shader failed to pick the change up. The quad and tile counts
also grew relative to respin 4 (`--radius 64`: 305 -> 319 tiles,
2,523,983 -> 3,058,001 quads), which is the expected consequence of terrain
that is genuinely rougher at the voxel scale — more surface area, more
meshed faces. That is the cost side of this change and it is real.

`worldgen.ush` still contains zero `float`/`double`/`half`.

- Source: `voxel-core/shaders/worldgen.ush` at worktree commit
  `2d785e202dc60e3b055645a3e903494abe12c9cf` ("worldgen v6: mirror the detail
  rework to HLSL, bump version, re-pin goldens")
- Compiler: the same pinned Microsoft DXC `v1.9.2602.24`
  (`dxcompiler.dll: 1.9(5191-d355aa83)(1.9.2602.24) - 1.9.2602.24
  (d355aa836)`), via `tools/compile-shaders.ps1` with no flag changes.
