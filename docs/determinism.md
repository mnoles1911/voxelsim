# Determinism conventions (doctrine §2.3)

Everything below the 30m diffusion tiles MUST be bit-deterministic across
compilers, OSes, CPU vendors, and (once ported) GPU vendors. This document
defines the shared primitives. Any change here is a **world-breaking change**:
it invalidates every saved edit log and every golden test. Bump
`vxc::kWorldGenVersion` and regenerate goldens when (and only when) a change is
deliberate.

## Units and coordinate conventions

| Quantity | Unit | Type |
|---|---|---|
| Voxel edge | 100 mm (0.1 m) — `kVoxelSizeMm` | constant |
| World voxel coords | voxels, +z up, z=0 is sea level | `int64_t` |
| Elevation / depths | millimetres | `int32_t` / `int64_t` |
| Tile elevation | metres (signed, sea level = 0) | `int16_t` per pixel |
| Tile pixel size | mm (30000 at scale 1, 11250 at scale 8) | `int32_t` |
| Climate channels | temperature, seasonality, precipitation, precip variability | `uint8_t` × 4 |

Rounding rules:

- Integer division follows C++ semantics (truncation toward zero) **except**
  where flooring is required, which always goes through `vxc::floorDiv` /
  `vxc::floorMod`. Lattice/pixel index computation from world coordinates is
  always floored.
- No floating point anywhere in world derivation. `float`/`double` are banned
  in `voxel-core/src` and `voxel-core/include` (CI greps for them; rendering
  and bench timing live outside that boundary).
- Signed→unsigned casts rely on standard two's-complement wrap (well-defined
  in C++20).

## The hash (worldgen hash v1)

All procedural randomness derives from one primitive, the SplitMix64
finalizer:

```
splitmix64(z):
  z += 0x9E3779B97F4A7C15
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
  z = (z ^ (z >> 27)) * 0x94D049BB133111EB
  return z ^ (z >> 31)
```

Domain-separated lattice hash for 2D/3D integer coordinates plus a channel id
(channel separates uses: detail octaves, stratigraphy jitter, cave noise, …):

```
hash2(seed, x, y, channel)    = splitmix64(seed ^ splitmix64(ux ^ splitmix64(uy ^ splitmix64(channel))))
hash3(seed, x, y, z, channel) = splitmix64(seed ^ splitmix64(ux ^ splitmix64(uy ^ splitmix64(uz ^ splitmix64(channel)))))
```

where `ux,uy,uz` are the two's-complement bit patterns of the `int64_t`
coordinates. Rationale: SplitMix64's finalizer is a full-avalanche bijection,
the chained form is cheap on GPU (no 128-bit multiplies), and the whole thing
is expressible in pure 64-bit integer ops on every target (HLSL/GLSL via
uint64 or paired uint32 emulation — to be validated in the GPU port).

Derived value in `[-32768, 32767]`: take the top 16 bits,
`(int32)(hash >> 48) - 32768`.

## Value noise

`valueNoise2(seed, xMm, yMm, latticeMm, channel)`:

1. `x0 = floorDiv(xMm, latticeMm)`, `fx = xMm - x0*latticeMm` (same for y).
2. Corner values from `hash2(seed, x0.., y0.., channel)` mapped to
   `[-32768, 32767]`.
3. Exact integer bilinear:
   `((v00*(L-fx) + v10*fx)*(L-fy) + (v01*(L-fx) + v11*fx)*fy) / (L*L)`
   with 64-bit intermediates; the final division truncates toward zero.

Fractal detail is a fixed list of (latticeMm, amplitudeMm) octaves summed with
per-octave channels. Octave tables are versioned constants in
`amplifier.cpp`.

## Determinism digests

`vxc::WorldDigest` is FNV-1a 64 over (a) every generated brick's material
stream and (b) every mesher quad, in deterministic iteration order. The bench
prints it; tests pin golden digests. CI runs gcc and clang and both must match
the committed goldens — a cross-*compiler* proxy until we have NV/AMD GPU
runners for the real cross-*vendor* gate.
