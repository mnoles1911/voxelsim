# `.vxtl` v2 — the baked fine-tier wire format (FROZEN CONTRACT)

**Status:** frozen 2026-07-29. This is the shared contract between the Python encoder
(`terrain-service/terrain_service/tile_codec.py`) and the C++ decoder
(`voxel-core/src/tilestore.cpp`). **Change it only by editing this file first**, because the two
halves are built independently and a silent disagreement here is a corrupt world, not a build
error.

Context and rationale live in `docs/terrain-amplification-plan.md` (Phase 2). This file is the
normative bit-level spec.

---

## 1. What v2 is

v1 (unchanged, still shipped): one 512×512 tile at 30 m/px — `int16` elevation in **whole metres**
plus 4 × `uint8` climate planes.

v2 **redefines the scale-8 slot** to hold one *fine tier* per coarse tile coordinate: the same
15.36 km footprint at **3.75 m/px, i.e. 4096×4096**. Safe to redefine because nothing was ever
generated at scale 8 (`tile_codec.py:41`, `tilestore.h:66`); the addressing change rolls
`provider_id` through `_tile_format_fingerprint`, exactly as that mechanism is designed to.

The s1 tile stays **byte-identical** — no golden churn, no C++ parser risk.

## 2. The fine plane is a CONTROL LATTICE, not samples

The plane stores **uniform cubic B-spline control points**, already prefiltered server-side, in
**absolute** elevation.

- A B-spline *approximates* its control points. Feeding it raw samples low-passes the source —
  measured: detrended H degrades from 0.83 to 1.47 between 240 m and 30 m without the prefilter.
  The prefilter is a float IIR pass (pole √3−2), which is exactly why it lives in the bake and why
  the tier ships control points rather than samples.
- **The client never interpolates samples.** It evaluates the spline directly on this lattice.
  That is the whole contract: `fine plane == control lattice`.
- Chosen over residual-coding against the coarse tier deliberately: residuals compress slightly
  better but make fine decode depend on a resident 3×3 ring of s1 tiles and re-import the coarse
  tier's C¹ break into client arithmetic. Revisit **only** if a 1.875 m tier is adopted, where a
  two-tier residual ladder becomes clearly worth it.

**Units.** `int16`, LSB = **100 mm** (exactly one voxel), relative to a per-tile datum
`base_offset_mm`. Range ±3276.7 m about that datum. `quant` selects the LSB so a rare
higher-relief tile can fall back to 250 mm without a format change.

```
elevation_mm(i, j) = base_offset_mm + int32(cp[i][j]) * quant_mm
```

## 3. Header

Little-endian throughout. Extends the v1 `<4sHQiiBH` struct; the first 25 bytes are
positionally identical to v1 so a v1 parser fails on `version`, not on garbage.

| field | type | notes |
|---|---|---|
| `magic` | `char[4]` | `"VXTL"` |
| `version` | `u16` | **2** |
| `seed` | `u64` | |
| `x`, `y` | `i32` | **COARSE** tile coords; footprint = s1 tile (x,y) |
| `scale` | `u8` | 8 |
| `size` | `u16` | 4096 (fine grid edge) |
| — v2 extension — | | |
| `block_log2` | `u8` | 8 → 256×256 fine px per block, 16×16 = 256 blocks |
| `predictor` | `u8` | 1 = `PRED_MED` (§5) |
| `quant` | `u8` | 1 = 100 mm/LSB, 2 = 250 mm/LSB |
| `codec` | `u8` | 0 = `CODEC_RAW`, 1 = `CODEC_ZSTD` |
| `bake_ver` | `u16` | bake algorithm + constants version |
| `flags` | `u16` | bit0 = flow plane present |
| `base_offset_mm` | `i32` | per-tile elevation datum |
| `reserved` | `u32` | must be 0 |
| `n_sections` | `u16` | |
| section table | `n_sections × {u32 id, u64 offset, u64 length}` | offsets are from file start |

Section ids: `ELEV_INDEX` = 1, `ELEV_DATA` = 2, `FLOW_INDEX` = 3, `FLOW_DATA` = 4.

**`codec` is a field, not a constant, on purpose.** `CODEC_RAW` lets the C++ decoder land and be
tested before any compression dependency exists; `CODEC_ZSTD` is added without touching the
format. Do not gate the decoder's correctness tests on having zstd.

## 4. Block index and data

`ELEV_INDEX` is `(size >> block_log2)²` entries, row-major, x fastest:

| field | type | notes |
|---|---|---|
| `offset` | `u64` | into `ELEV_DATA` |
| `comp_len` | `u32` | 0 when `mode == CONSTANT` |
| `mode` | `u8` | 0 = `CONSTANT`, 1 = `CODED`, 2 = `RAW` |
| `const_cp` | `i16` | the whole block's value when `CONSTANT` |
| `resid_bits` | `u8` | 16 or 32 — see §5 |
| `pad` | `u8[4]` | must be 0 |

Blocks are **independent** — one frame each, no shared dictionary, no cross-block prediction. That
is what buys per-block random access: the client decodes only the ~0.92 km² blocks it needs, not
33.5 MB.

`CONSTANT` blocks cost zero bytes and are common (ocean, flat basin).

## 5. Predictor and residual coding

Per block, in raster order, the LOCO-I / JPEG-LS **median edge predictor**:

```
W = cp[x-1][y], N = cp[x][y-1], NW = cp[x-1][y-1]
pred = min(W,N)           if NW >= max(W,N)
       max(W,N)           if NW <= min(W,N)
       W + N - NW         otherwise
first pixel: pred = 0 ; first row: pred = W ; first column: pred = N
resid = cp - pred
```

Residuals are zigzag-mapped (`(r << 1) ^ (r >> 31)`) and emitted little-endian.

**`resid_bits` is REQUIRED, not an optimisation.** Measured on real tiles: **73–159 residuals per
tile exceed int16** even at 100 mm quantisation — one 3.75 m post across a 30 m cliff does it. A
block sets `resid_bits = 32` if any residual leaves `[-32768, 32767]`, else 16. Encoders must not
assume 16.

## 6. Flow plane (optional, `flags` bit0)

One `uint8` per fine pixel, same block structure, same predictor:

- bits 0–4: `log2(flow accumulation in m²)` clamped to 0–31
- bit 5: channel, bit 6: bank, bit 7: deposition

Mostly zeros; ~5–10 KB compressed. Consumers: client alluvium/cut-bank materials, and later the
flow-conditioned rill synthesis and bank undercuts.

## 7. Decode is a pure integer function of the bytes

Non-negotiable, because these bytes are the multiplayer authority:

- zstd frame decode is bit-exact by format definition (any compliant decoder, any version).
- The MED inverse and the B-spline evaluation are exact `int64` with **truncating** division
  (C++ `/`, mirrored by `truncDiv` in HLSL) — *not* floor division.
- The **encoder** need not be deterministic. It runs once on a GPU pod and the bytes are shipped;
  that is the same licence the diffusion model already has, and it is what lets the bake use floats.

Never regenerate a shipped tile. A bake bug means a new `bake_ver` → new `provider_id` → new
world, with old caches untouched.

## 8. The B-spline the client will evaluate (normative)

Both halves must agree on this exactly. Cell fraction in q10; weights are integer numerators over
`6·1024³`; evaluation is **two-stage separable with an intermediate division** (the exact tensor
form overflows int64 by ~10 orders of magnitude).

```
tq = truncDiv(fx * 1024, pxMm)                    # fx in [0, pxMm)
w0 = (1024-tq)^3
w1 = 3*tq^3 - 6*tq^2*1024 + 4*1024^3
w2 = -3*tq^3 + 3*tq^2*1024 + 3*tq*1024^2 + 1024^3
w3 = tq^3                                          # sum == 6*1024^3, all >= 0
```

Stencil for cell `px`: control points `px-1 .. px+2` (`vxc::kCarrierStencilLo/Hi` in `tiles.h`).
This is the same carrier shipped in worldgen v9 — see `amplifier.cpp`'s carrier block for the
overflow and bound arguments.

## 9. Conformance

A codec change is not done until:

1. Python round-trips: encode → decode → identical array, over `CONSTANT`, `CODED`/16,
   `CODED`/32 and `RAW` blocks, and both `quant` values.
2. A **golden fine tile** committed as a fixture, encoded by Python, decoded by the C++ parser,
   digest-compared.
3. A sample-for-sample check that the C++ and Python B-spline evaluations agree on the same
   lattice.
4. Truncation/corruption is rejected cleanly, as `TileData::parse` already does for v1.
