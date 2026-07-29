"""Emit B-spline probe points and their EXPECTED heights for the C++ decoder.

`docs/vxtl-v2-format.md` §9 item 3 asks for "a sample-for-sample check that the
C++ and Python B-spline evaluations agree on the same lattice". This is the
Python half: it reads the int16 control-point plane the encoder produced
(`bake_real_tile.py --cp-bin`), evaluates §8's spline at pseudo-random world
positions with the SAME integer arithmetic the C++ carrier uses, and writes

    <x_mm>\t<y_mm>\t<expected_height_mm>

for a C++ program to reproduce from the encoded `.vxtl` alone.

WHY THIS IS INTEGER ARITHMETIC AND NOT `numpy.float64`
------------------------------------------------------
§7 makes decode a pure integer function of the bytes, and §8's evaluation is
"two-stage separable with an intermediate division". A float mirror would agree
to a millimetre or so and would therefore prove only that the two are close --
which is exactly the class of agreement that lets a client and a server drift
apart. Every division here truncates toward zero, matching plain C++ `/`
(carrier.h says so explicitly, and it is NOT floor: the two differ on negative
numerators, and an ocean tile's control points are hundreds of thousands of
negative millimetres).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec  # noqa: E402

Q = 1024
VALUE_DEN = 6 * Q ** 3
PIXEL_MM = 1875  # scale 16


def trunc_div(a: int, b: int) -> int:
    """C++ `/`: truncate toward zero. `//` floors and would differ below sea level."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def value_w(tq: int) -> tuple[int, int, int, int]:
    t = Q
    return (
        (t - tq) ** 3,
        3 * tq ** 3 - 6 * tq ** 2 * t + 4 * t ** 3,
        -3 * tq ** 3 + 3 * tq ** 2 * t + 3 * tq * t ** 2 + t ** 3,
        tq ** 3,
    )


def eval_carrier_mm(stencil_mm, fx: int, fy: int, px_mm: int = PIXEL_MM) -> int:
    """§8's two-stage separable evaluation. `stencil_mm` is 4x4, row-major, j outer."""
    wx = value_w(fx * Q // px_mm)
    wy = value_w(fy * Q // px_mm)
    rows = []
    for j in range(4):
        v = sum(int(stencil_mm[j][i]) * wx[i] for i in range(4))
        rows.append(trunc_div(v, VALUE_DEN))
    h = sum(rows[j] * wy[j] for j in range(4))
    return trunc_div(h, VALUE_DEN)


def read_cp_bin(path: Path) -> np.ndarray:
    b = path.read_bytes()
    magic, size = struct.unpack_from("<4si", b, 0)
    if magic != b"VXCP":
        raise ValueError(f"{path} is not a VXCP control-point dump")
    n = size * size
    return np.frombuffer(b, dtype="<i2", count=n, offset=8).reshape(size, size)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tile", help="the encoded .vxtl v2 file (for header fields)")
    ap.add_argument("cp_bin", help="the matching VXCP control-point dump")
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=4096)
    ap.add_argument("--seed", type=int, default=20260729)
    a = ap.parse_args()

    data = Path(a.tile).read_bytes()
    _magic, ver, _seed, tx, ty, _scale, size = tile_codec._HEADER.unpack_from(data, 0)
    (_bl, _pred, quant, _codec, _bv, _flags, base_mm, _ps, _res,
     _ns) = tile_codec._V2_EXT.unpack_from(data, tile_codec._HEADER.size)
    if ver != tile_codec.VERSION_V2:
        raise SystemExit(f"{a.tile} is not v2")
    q_mm = tile_codec.QUANT_MM[quant]

    cp = read_cp_bin(Path(a.cp_bin))
    if cp.shape != (size, size):
        raise SystemExit(f"cp dump is {cp.shape}, header says {size}")
    cp_mm = base_mm + cp.astype(np.int64) * q_mm

    # Probes stay >= 1 px inside the tile: the 4x4 stencil reaches px-1..px+2,
    # and outside that margin it would read a neighbour tile the sampler has
    # not been given -- which would test the missing-tile policy, not the spline.
    rng = np.random.default_rng(a.seed)
    lx = rng.integers(1, size - 2, size=a.n)
    ly = rng.integers(1, size - 2, size=a.n)
    fxs = rng.integers(0, PIXEL_MM, size=a.n)
    fys = rng.integers(0, PIXEL_MM, size=a.n)
    # A quarter of the probes land exactly on the lattice (fx = fy = 0), where
    # the weights are (1,4,1,0)/6 and the prefilter's whole job is to make that
    # reproduce the bake's own sample. Random fractions alone would never hit it.
    fxs[: a.n // 4] = 0
    fys[: a.n // 4] = 0

    x0 = (tx * size) * PIXEL_MM
    y0 = (ty * size) * PIXEL_MM
    out = []
    for k in range(a.n):
        i, j = int(lx[k]), int(ly[k])
        st = cp_mm[j - 1:j + 3, i - 1:i + 3]
        hmm = eval_carrier_mm(st, int(fxs[k]), int(fys[k]))
        out.append(f"{x0 + i * PIXEL_MM + int(fxs[k])}\t"
                   f"{y0 + j * PIXEL_MM + int(fys[k])}\t{hmm}")
    Path(a.out).write_text("\n".join(out) + "\n")
    print(f"wrote {a.n} probes for tile ({tx},{ty}) -> {a.out}")
    print(f"  datum {base_mm} mm, quant {q_mm} mm, cp range "
          f"[{int(cp.min())}, {int(cp.max())}] -> "
          f"[{cp_mm.min()/1000:.1f}, {cp_mm.max()/1000:.1f}] m")
    return 0


if __name__ == "__main__":
    sys.exit(main())
