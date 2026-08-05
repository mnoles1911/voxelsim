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

v2 adds a **scale-16 slot** holding one *fine tier* per coarse tile coordinate: the same 15.36 km
footprint at **1.875 m/px, i.e. 8192×8192**. `tilePixelSizeMm` gains `16 → 1875`
(`tilestore.h:67-69`, mirrored in `tile_codec.py:43` and `tiles.h`). The addressing change rolls
`provider_id` through `_tile_format_fingerprint`, exactly as that mechanism is designed to.

The s1 tile stays **byte-identical** — no golden churn, no C++ parser risk.

**1.875 m was chosen over 3.75 m deliberately (Matt, 2026-07-29).** It buys the 3.75–7.5 m band —
small stream channels, gullies you can climb into, cut banks, terrace risers — which is the band a
1.8 m player physically occupies, and it is the difference between watercourses that *drain* and
watercourses that are decoration. Costs, measured rather than assumed, so nobody rediscovers them:
**~21–25 MB/tile** compressed (4× the pixels but ~1 bit/px cheaper, since halving post spacing
halves the per-step gradient) and **~165 CPU-s/tile** to bake. At ~106 KB/km² a 1 M km² explored
world is ~106 GB server-side, and an 8–16 GB client cache holds ~500 tiles.

**Three consequences that are easy to miss:**

1. **`kSurfaceBoundMaxCornersPerAxis` must rise 34 → 64.** v9 set 34 for a 3.75 m pixel. At
   1.875 m a level-5 footprint (108.8 m incl. apron) spans ~58 cells and needs ~61 control points,
   so the bound would silently **decline** from level 5 up — safe, but exactly the performance
   cliff v9 removed. 64² × int64 = 32 KB of stack, up from 9.2 KB.
2. **A two-tier residual ladder is now worth revisiting**, though this spec does NOT do it. Most of
   a 1.875 m tier is derivable from a 3.75 m one by the same B-spline, so residuals would be near
   zero — and a coarser fine tier would let the streaming layer feed the mid rings without pulling
   1.875 m everywhere. `parent_scale` is reserved in the header for exactly that; leave it 0.
3. **The bake stops being free.** At ~20–40 s/tile in production, advancing the frontier by one
   tile (3 bakes) becomes comparable to its 5 coarse tiles at 22.5 s. Prefetch ring sizing should
   be re-derived rather than inherited.

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
| `scale` | `u8` | 16 |
| `size` | `u16` | 8192 (fine grid edge) |
| — v2 extension — | | |
| `block_log2` | `u8` | 8 -> 256x256 fine px per block (480 m), 32x32 = 1024 blocks |
| `predictor` | `u8` | 1 = `PRED_MED` (§5) |
| `quant` | `u8` | 1 = 100 mm/LSB, 2 = 250 mm/LSB |
| `codec` | `u8` | 0 = `CODEC_RAW`, 1 = `CODEC_ZSTD` |
| `bake_ver` | `u16` | bake algorithm + constants version |
| `flags` | `u16` | bit0 = flow plane present, bit1 = basin table present (§6.1). Any other bit set is **rejected**. |
| `base_offset_mm` | `i32` | per-tile elevation datum |
| `parent_scale` | `u8` | 0 = absolute (this spec). Reserved for a future residual ladder. |
| `reserved` | `u8[3]` | must be 0 |
| `n_sections` | `u16` | |
| section table | `n_sections × {u32 id, u64 offset, u64 length}` | offsets are from file start |

Section ids: `ELEV_INDEX` = 1, `ELEV_DATA` = 2, `FLOW_INDEX` = 3, `FLOW_DATA` = 4,
`BASIN_TABLE` = 5 (§6.1).

**`codec` is a field, not a constant, on purpose.** `CODEC_RAW` lets the C++ decoder land and be
tested before any compression dependency exists; `CODEC_ZSTD` is added without touching the
format. Do not gate the decoder's correctness tests on having zstd.

**Where the zstd decompressor lives (decided 2026-07-29): NOT inside voxel-core.** It is injected
at the host boundary as an optional decompressor callback on the parse entry point, defaulting to
null (= `CODEC_RAW` only). Two reasons, and the second is the serious one: voxel-core currently has
**zero** third-party dependencies and that is deliberate; and voxel-core is linked into a UE 5.8
binary in which a zstd already exists, so a second static copy is an ODR/symbol-collision risk
rather than mere bloat. The UE module supplies the decompressor; the headless harness passes its
own. This keeps decode a pure function of the bytes (§7) either way, because zstd frame decode is
bit-exact by format definition.

**Correction, 2026-07-29 — the original premise for that second reason was wrong, and the
conclusion survives anyway.** This paragraph used to say UE "already ships zstd in
`Engine/Source/ThirdParty`", so the module could simply link the engine's copy. It does not:
UE 5.8.0-55116800 (binary/launcher) ships **Oodle and LZ4**, with no `zstd.h`, no `zstd*.lib`, no
zstd module and no `.Build.cs` referencing one. Verified by scanning the engine tree.

The ODR hazard is nevertheless **real, and worse than assumed**: a scan of all 3,672 engine
binaries over 200 KB finds `ZSTD_decompress` *statically linked inside*
`ThirdParty/Blosc/.../libblosc.lib`. So there is a zstd in the process, it is simply not one we may
call — which is precisely the situation in which vendoring a second copy is dangerous rather than
merely redundant. The injection design is therefore right for a better reason than the one
originally given. The UE module probes for a zstd module and compiles with
`VOXELEARTH_WITH_ZSTD=0` plus a startup warning when there is none, so a fine tier encoded with
`CODEC_ZSTD` fails loudly at parse rather than silently yielding a lattice of zeros.

This is a *when*, not an *if*: ~21–25 MB/tile is the **compressed** figure, while the `CODEC_RAW`
form of an 8192² lattice is 134 MB (268 MB if blocks need 32-bit residuals). Production streaming
needs zstd; it just belongs at the boundary rather than inside the deterministic core.

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
is what buys per-block random access: the client decodes only the ~0.23 km² blocks it needs, not
134 MB. A decoded block is 128 KB. "Independent" also fixes the predictor's edge rules: they are
**block-local**, so the first row and column of every block use the §5 edge cases rather than
reaching into a neighbouring block.

The three modes, all of which a decoder must handle (§5's closing note explains why mode selection
is encoder policy):

- **`CONSTANT`** — the whole block is `const_cp`. Zero data bytes. Common: ocean, flat basin.
- **`CODED`** — §5 MED residuals, zigzagged, `resid_bits` wide.
- **`RAW`** — **literal control points, `int16` little-endian, row-major, no prediction and no
  zigzag.** Exactly `2 × (1 << block_log2)²` bytes.

**`RAW`'s payload definition is load-bearing and cannot be inferred.** Under `CODEC_RAW` a literal
`int16` plane and a `resid_bits = 16` MED-residual plane have **identical lengths**, so no length,
bounds or structural check can distinguish them — a decoder that guesses wrong produces plausible
geometry that is simply wrong terrain. (Measured on the golden fixture: reading its `RAW` block as
MED residuals reconstructs to ±4.19 M.) `resid_bits` is meaningless in this mode and is written 0.

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

**Interop details settled while building the two halves — decoders must honour these:**

- `resid_bits` is **written as 0** for `CONSTANT` and `RAW` blocks, where it has no meaning. A
  decoder must ignore it in those modes, **not** validate it against {16, 32}.
- `parent_scale != 0` must be **rejected**, not treated as absolute. This spec defines only the
  absolute reading; silently misinterpreting a future residual-ladder tile is worse than refusing
  it.
- `RAW` is never auto-selected by the reference encoder. Under `CODEC_RAW` it only beats `CODED`
  when `CODED` would need `resid_bits = 32`, so a size-minimising selector would make
  `resid_bits = 32` unreachable through the encoder and untestable. Mode selection is encoder
  policy, not wire format — decoders must handle all four modes regardless of how they were chosen.

## 6. Flow plane (optional, `flags` bit0)

One `uint8` per fine pixel, same block structure, same predictor:

- bits 0–4: `log2(flow accumulation in m²)` clamped to 0–31
- bit 5: channel, bit 6: bank, bit 7: deposition

Mostly zeros; ~5–10 KB compressed. Consumers: client alluvium/cut-bank materials, and later the
flow-conditioned rill synthesis and bank undercuts.

**Element width differs from the elevation plane, and the modes inherit that:**

- **`RAW`** on the flow plane is **one `uint8` per pixel**, not two. A block is exactly
  `(1 << block_log2)²` bytes.
- **`CONSTANT`** stores the flow byte in `const_cp` as an **unsigned 0–255** quantity, even though
  the field is `i16` on the wire. `0xFF` reads back as `255`, never sign-extended to `-1`. Valid
  flow bytes never reach the negative half of the field, so no sign-extension bug is reachable from
  *valid* data — but a **corrupt** file claiming an out-of-range `const_cp` must be **rejected**,
  not truncated into the target element type. That is the one place an all-or-nothing parser can
  otherwise let corruption through as plausible data.

### 6.1 Basin table (optional, `flags` bit1) — added at `bake_ver` 8

The per-tile lake registry. `docs/watershed-system-plan.md` §4.2–§4.3 has the reasoning; this
section is the bytes.

A **flat table, never a plane**. The bake already knows where every depression is, how deep, and —
from a water balance against the tile's own climate — whether it holds water; the client needs
only enough to flood-fill each one. Tens of rows at 32 B is ~1 KB against 26.6 MB of compressed
elevation, so the cost is noise and there is no per-pixel water plane in v2. (A per-pixel
`water_surface` plane is the plan's P2 and is a *later* section id, not a change to this one.)

Payload:

| field | type | notes |
|---|---|---|
| `table_version` | `u16` | 1 |
| `entry_bytes` | `u16` | 32. Redundant with the section length **on purpose** — see below. |
| `count` | `u32` | rows |
| rows | `count × entry_bytes` | |

One row, 32 bytes:

| field | type | notes |
|---|---|---|
| `basin_id` | `u16` | **must equal the row index.** Ids are 0..n-1 in order. |
| `seed_px` | `2×u16` | deepest cell — the client's flood-fill seed. Tile-LOCAL pixels. |
| `bbox_px` | `4×u16` | `x0, y0, x1, y1`, inclusive. Bounds the flood fill. |
| `outlet_px` | `2×u16` | the spill cell: head of the outlet channel. |
| `spill_mm` | `i32` | fill level on the FINAL surface, **absolute** mm. |
| `surface_mm` | `i32` | equilibrium water surface, absolute mm. `<= spill_mm` always. |
| `kind` | `u8` | 0 dry playa, 1 salt flat, 2 seasonal, 3 lake (terminal), 4 lake (overflowing) |
| `reserved` | `u8[5]` | must be 0 |

Four decisions worth stating, because a decoder that gets any of them wrong produces **water on
terrain that has none** rather than an error:

- **Elevations are absolute millimetres, NOT relative to `base_offset_mm`.** A basin is read by
  gameplay code that never touches the control-point datum; sharing one would couple a water query
  to the elevation codec's internals.
- **`entry_bytes` is redundant with the section length and that is the point.** A decoder that
  disagrees with the encoder about the row size gets a mismatch it can refuse, instead of reading
  33-byte records out of a 32-byte stream and producing plausible garbage.
- **An EMPTY table with bit1 set is a statement**, not an absence: "this tile was surveyed and
  holds nothing". A tile baked before `bake_ver` 8 has bit1 clear. A client that conflated the two
  would put no water in a world that has some, and never know.
- **`surface_mm > spill_mm` is refused, not clamped.** Water standing above its own outlet is not a
  lake — the outlet would carry the excess away — so it is a bug in whatever wrote the file.

Both decoders also refuse: an id that is not the row index, an inside-out or out-of-tile bbox, a
seed outside its own bbox, a `kind` outside the enum, and non-zero reserved bytes.

The bake's registry filter (which depressions get a row at all) is a set of `BakeConstants`, so
changing it rolls `bake_ver` → `fine_provider_id` → a new world, exactly like any other bake
constant. It is **not** a decoder parameter.

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
This is the same carrier *stencil* shipped in worldgen v9 — see `amplifier.cpp`'s carrier block
for the overflow and bound arguments. The stencil and the weights above are unchanged; what the
carrier does around them is not. v13 added a prefilter and moved detail gating from gradient to
relief, and v16 added a horizontal warp to break straight contour terracing. Worldgen is at v23.

## 9. Conformance

A codec change is not done until:

1. Python round-trips: encode → decode → identical array, over `CONSTANT`, `CODED`/16,
   `CODED`/32 and `RAW` blocks, and both `quant` values.
2. A **golden fine tile** committed as a fixture, encoded by Python, decoded by the C++ parser,
   digest-compared.
3. A sample-for-sample check that the C++ and Python B-spline evaluations agree on the same
   lattice.
4. Truncation/corruption is rejected cleanly, as `TileData::parse` already does for v1.
5. For §6.1: a **basin fixture** committed and parsed by both halves
   (`voxel-core/tests/fixtures/vxtl_v2_golden_basins_512.vxtl`, regenerated by
   `terrain-service/tools/make_basin_fixture.py`). It must carry one row of **every** `kind` — a
   decoder that reads `kind` as a bool passes an all-lake table — a **negative** `spill_mm` and
   `surface_mm`, a one-pixel extent, a bbox touching the last pixel of the grid, and rows in
   `(min_y, min_x)` order. Each refusal above needs a corrupted-copy test on both sides.
