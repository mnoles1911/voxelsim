"""Measure the .vxtl v2 fine-tier compressed size on a REAL tile.

Open risk #2 in docs/terrain-amplification-plan.md is that the 8 MB/tile
planning number is MODELLED (entropy of a Laplacian at an assumed MED-error
sigma), not measured. This measures it.

Method:
  1. decode a real 30 m .vxtl tile (int16 metres),
  2. upsample 8x with the SAME uniform cubic B-spline the client carrier uses,
     giving the 4096x4096 / 3.75 m field with NO added detail,
  3. optionally add fractal roughness at a controlled amplitude, standing in for
     what the geomorphic bake will contribute,
  4. quantise to 10 cm about a per-tile datum,
  5. MED/LOCO-I predict, zigzag, compress per 256x256 block.

The zero-roughness row is a hard FLOOR on the fine tier: real eroded detail can
only add entropy. The roughness sweep says how size scales with it, which is the
part the model was guessing at.

zstd is not installed here; zlib and lzma bracket it (zstd -19 normally lands
between them, nearer lzma).
"""
import struct, sys, zlib, lzma
import numpy as np

TILE, SCALE = 512, 8
FINE = TILE * SCALE          # 4096
BLOCK = 256                  # 960 m at 3.75 m/px
PIXEL_M = 30.0 / SCALE       # 3.75


def decode_vxtl(path):
    b = open(path, "rb").read()
    magic, ver, seed, x, y, scale, size = struct.unpack_from("<4sHQiiBH", b, 0)
    assert magic == b"VXTL", magic
    off = struct.calcsize("<4sHQiiBH")
    elev = np.frombuffer(b, dtype="<i2", count=size * size, offset=off).reshape(size, size)
    return elev.astype(np.float64), size


def _weights(scale):
    T = 1024
    tq = (np.arange(scale) * T) // scale
    u = T - tq
    W = np.stack([u ** 3,
                  3 * tq ** 3 - 6 * tq ** 2 * T + 4 * T ** 3,
                  -3 * tq ** 3 + 3 * tq ** 2 * T + 3 * tq * T ** 2 + T ** 3,
                  tq ** 3], axis=1).astype(np.float64)
    return W / (6.0 * T ** 3)                     # (scale, 4), rows sum to 1


def _up_axis(a, scale, W):
    """Upsample the LAST axis by `scale` with a cubic B-spline."""
    rows, n = a.shape
    pad = np.pad(a, ((0, 0), (1, 2)), mode="edge")        # control points i-1..i+2
    out = np.zeros((rows, n, scale))
    for k in range(4):
        out += pad[:, k:k + n][:, :, None] * W[None, None, :, k]
    return out.reshape(rows, n * scale)


def bspline_upsample(a, scale):
    W = _weights(scale)
    b = _up_axis(a, scale, W)                # (512, 4096)
    c = _up_axis(np.ascontiguousarray(b.T), scale, W)   # (4096, 4096)
    return np.ascontiguousarray(c.T)


def med_predict(n):
    """LOCO-I / JPEG-LS median edge predictor error."""
    w = np.zeros_like(n);  w[:, 1:] = n[:, :-1]
    nn = np.zeros_like(n); nn[1:, :] = n[:-1, :]
    nw = np.zeros_like(n); nw[1:, 1:] = n[:-1, :-1]
    mx, mn = np.maximum(w, nn), np.minimum(w, nn)
    pred = np.where(nw >= mx, mn, np.where(nw <= mn, mx, w + nn - nw))
    pred[0, 0] = 0
    pred[0, 1:] = n[0, :-1]
    pred[1:, 0] = n[:-1, 0]
    return n - pred


def compressed_bytes(fine_cm):
    zl = xz = 0
    over = 0
    for by in range(0, FINE, BLOCK):
        for bx in range(0, FINE, BLOCK):
            blk = fine_cm[by:by + BLOCK, bx:bx + BLOCK]
            if blk.min() == blk.max():
                continue                                  # CONSTANT mode, 0 bytes
            err = med_predict(blk)
            over += int((np.abs(err) > 32767).sum())
            zz = ((err << 1) ^ (err >> 63)).astype("<u2")  # zigzag -> uint16
            payload = zz.tobytes()
            zl += len(zlib.compress(payload, 9))
            xz += len(lzma.compress(payload, preset=6))
    return zl, xz, over


def fractal(shape, rng):
    """Cheap fBm stand-in for bake roughness, normalised to unit RMS."""
    out = np.zeros(shape)
    amp, size = 1.0, 8
    while size <= shape[0]:
        g = rng.standard_normal((size, size))
        rep = shape[0] // size
        out += amp * np.kron(g, np.ones((rep, rep)))[:shape[0], :shape[1]]
        amp *= 0.5
        size *= 2
    return out / out.std()


def main(path):
    elev_m, size = decode_vxtl(path)
    assert size == TILE
    print(f"tile: {path}")
    print(f"  30 m elevation: min {elev_m.min():.0f} m  max {elev_m.max():.0f} m  "
          f"relief {elev_m.max()-elev_m.min():.0f} m")

    fine_m = bspline_upsample(elev_m, SCALE)
    print(f"  upsampled to {fine_m.shape[0]}x{fine_m.shape[1]} @ {PIXEL_M} m/px")

    km2 = (FINE * PIXEL_M / 1000.0) ** 2
    print(f"  raw int16 plane {FINE*FINE*2/1e6:.1f} MB over {km2:.0f} km2\n")
    # White noise, because it is the HIGH-frequency content that costs bits: a
    # low-frequency fBm barely moves the MED error and so barely moves the size.
    # Driving sigma directly is what tests the plan's bits/px model.
    rng = np.random.default_rng(20260719)
    white = rng.standard_normal(fine_m.shape)
    frac = fractal(fine_m.shape, np.random.default_rng(20260719))
    rows = ([("white", c) for c in (0.0, 2.0, 5.0, 15.0, 60.0)] +
            [("fBm", c) for c in (25.0, 50.0, 100.0, 200.0)])
    print(f"  {'kind':>6} {'RMS(cm)':>8} {'MEDsigma':>9} {'zlib(MB)':>9} {'lzma(MB)':>9} "
          f"{'bits/px':>8} {'model':>7} {'KB/km2':>8}")
    for kind, add_cm in rows:
        field = fine_m + (white if kind == "white" else frac) * (add_cm / 100.0)
        cm = np.rint((field - field.mean()) * 100.0).astype(np.int64)
        sigma = med_predict(cm).std()
        zl, xz, over = compressed_bytes(cm)
        # The plan's model: entropy of a discretised Laplacian, log2(2e*b),
        # b = sigma/sqrt(2), plus ~0.3 bit of framing.
        model = np.log2(2 * np.e * sigma / np.sqrt(2)) + 0.3
        flag = f"  (!{over} err>i16)" if over else ""
        print(f"  {kind:>6} {add_cm:>8.1f} {sigma:>9.1f} {zl/1e6:>9.2f} {xz/1e6:>9.2f} "
              f"{8*xz/(FINE*FINE):>8.2f} {model:>7.2f} {xz/1024/km2:>8.1f}{flag}")


if __name__ == "__main__":
    main(sys.argv[1])
