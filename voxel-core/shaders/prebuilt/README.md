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

## Provenance

- Source: `voxel-core/shaders/worldgen.hlsl` at commit `8b834107701fd4a2a005b8dbd4a17352f44f26c1`
  ("M4: biome classification core + per-biome surface materials (round 1)")
- Compiler: Microsoft DXC, pinned version `v1.9.2602.24` (`tools/fetch-dxc.ps1`),
  `dxcompiler.dll: 1.9(5191-d355aa83)(1.9.2602.24) - 1.9.2602.24 (d355aa836)`
- Compile command: `tools/compile-shaders.ps1`'s SPIR-V invocation —
  `dxc -T cs_6_0 -E <Entry> -O3 -spirv -fspv-target-env=vulkan1.1
  -fvk-b-shift 0 0 -fvk-t-shift 1 0 -fvk-u-shift 3 0 worldgen.hlsl -Fo <out>.spv`
- Compiled and committed from: worktree HEAD `a172baf54c8ebf45e99d281a539fdd63fcd5e7e6`
  (branch base: `main`, PR #34 "m6-pathfind" merge)
- Built on: this Windows dev box (MSVC/VS 2026 toolchain elsewhere in the
  repo; DXC itself needs no MSVC — it's a standalone compiler)

## SHA-256

```
1d5e9afca7bf4f4d590e97b70be6566f5feacc05e50158e5a10973baab0883d3  worldgen.ColumnMain.spv
d77c4d737507a61d9f9fa61bb2e6c0677d16d93c9ed74e20f2f3bfbc1725e6aa  worldgen.MeshCountMain.spv
c856bb46d94b8358797fe5c1275df54e48a3922ddb080db71cdfe8b559ea808e  worldgen.MeshEmitMain.spv
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

**What was actually verified** (this pass, on this box, fresh rebuild from
the pinned DXC + current `worldgen.hlsl`, using these exact committed
`.spv` files via `vxc_gpu --spv .../prebuilt/...`):

| Mode | Result | GPU output digest (columns+cells+quads) |
|---|---|---|
| default (2 regions, column+cell+quad compare) | **PASS**, 0 mismatches, 32768 columns / 786432 cells / 10739 quads | `1dbcabb01cfaf2bc` |
| `--radius 64` | **PASS**, 0 mismatches, 144/144 tiles (100%) verified, gate 0.129s (< 1s target) | `95a82ba20200f6f2` |
| `--radius 128` | **PASS**, 0 mismatches, 67/529 tiles (12.7%, every-8th sampled) verified, gate 0.259s (< 1s target) | `b4c8ec5d0966894b` |

Device: AMD Radeon RX 7800 XT.

These three digests — not the older status.md ones — are the values the
NVIDIA leg (`tools/run-nvidia-digest.sh`) must reproduce to close the M0
cross-vendor gate. `docs/status.md` should get its determinism row refreshed
against these current values as a separate follow-up (out of scope for this
pass, which only appends a new subsection — see this repo's `docs/status.md`
"Linux/NVIDIA cross-vendor determinism runner" entry).

## Regenerating

```powershell
tools/fetch-dxc.ps1              # once, pinned v1.9.2602.24
tools/compile-shaders.ps1        # writes build/shaders/*.spv (DXIL + SPIR-V)
# then copy the 7 worldgen.*.spv files here and update this README's hashes.
```

Bump the DXC pin (`tools/fetch-dxc.ps1`) and regenerate deliberately —
shader binaries are part of the determinism surface (ADR-0001).
