"""Freeze the LAKE EXTENT RULE into a fixture both languages must reproduce.

WHY A FIXTURE AND NOT TWO TEST SUITES. The extent of a lake is defined once --
`bake/basins.py:lake_extent_mask` -- and implemented twice: there, and in
`voxelcore/lakes.h:lakeExtentFill`, which is what the client runs. The two
cannot be allowed to disagree, because a disagreement does not look like a
crash. It looks like a shoreline that stops short, or water lying on a hillside
across a ridge from its own lake, in ONE of the two processes -- and the
authority for "is this cell wet" is the client, while the authority for
"area_m2" on the wire is the bake.

So this writes an elevation raster whose answer is already interesting, plus
the basin rows to fill from, plus the mask Python produces. `test_lakes.cpp`
reads all three and asserts C++ reproduces the mask CELL FOR CELL. Regenerate
with:

    python tools/make_lake_extent_fixture.py

THE RASTER IS CHOSEN TO BREAK A LAZY IMPLEMENTATION, not to be typical:

  basin 0  two bowls under ONE bbox, connected by nothing. A threshold
           (`z <= surface`) passes both; only a seeded fill returns one. This
           is the single most important row and the reason the rule is a fill.
  basin 1  the OTHER bowl, seeded separately. Its mask must be disjoint from
           basin 0's, and the union must be the whole threshold set -- which
           checks that neither implementation leaks across the divide.
  basin 2  a bowl whose two lobes touch only DIAGONALLY. Four-connected code
           returns half of it; eight-connected returns all of it. The registry
           measured area on the 8-connected component, so half is a bug.
  basin 3  a bbox that CLIPS its own basin. The bbox is a bound, not a hint: a
           fill that ignores it walks out of the basin and into the next one.
  basin 4  a DRY basin (surface at the floor minus one mm). The answer is an
           empty mask, not an error -- the client asks about every registered
           row including the dry ones.

FORMAT (little-endian, and deliberately trivial: a fixture that needs a parser
is a second thing that can be wrong):

    magic   "VXLKEXT1"                       8 bytes
    w, h                                     2 x u32
    elevation_mm                             w*h x i32, row major, x fastest
    n_basins                                 u32
    per basin: seedX seedY x0 y0 x1 y1       6 x u16
               surfaceMm                     i32
               wetCells                      u32
    masks, concatenated in basin order       sum over basins of bboxW*bboxH x u8
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service.bake import basins as bs  # noqa: E402

OUT = Path(__file__).resolve().parents[2] / "voxel-core" / "tests" / "fixtures" / "lake_extent_v1.bin"

W = H = 128


def build() -> np.ndarray:
    """Elevation in mm. A plateau at 1000 m with hollows cut into it."""
    y, x = np.mgrid[0:H, 0:W].astype(np.float64)
    z = np.full((H, W), 1_000_000.0)          # 1000 m in mm

    def bowl(cx, cy, r, depth_mm):
        d = np.hypot(x - cx, y - cy)
        return np.maximum(0.0, depth_mm * (1.0 - (d / r) ** 2))

    # Two separate bowls, both below the same water level, no path between.
    z -= bowl(30, 30, 14, 8_000)
    z -= bowl(30, 80, 14, 8_000)
    # Two rectangular hollows that touch at EXACTLY ONE DIAGONAL CORNER --
    # (33,89) and (34,90) are 8-neighbours and not 4-neighbours. Rectangles
    # rather than bowls because the pinch has to be exact: a paraboloid's
    # contour at the water level is not a shape anyone can reason about, and
    # the whole point of this row is that one specific adjacency decides it.
    z[24:34, 80:90] -= 6_000.0
    z[34:44, 90:100] -= 6_000.0
    z[28, 84] -= 500.0          # a unique deepest cell, so the seed is defined
    # One deep hollow for the clipped-bbox and dry rows.
    z -= bowl(90, 100, 16, 12_000)
    return np.rint(z).astype(np.int32)


def main() -> int:
    zmm = build()
    z_m = zmm.astype(np.float32) / 1000.0

    def seed_of(x0, y0, x1, y1):
        sub = zmm[y0:y1 + 1, x0:x1 + 1]
        iy, ix = np.unravel_index(int(np.argmin(sub)), sub.shape)
        return x0 + int(ix), y0 + int(iy)

    rows = []
    # 0 and 1: the two bowls, ONE bbox covering both, seeded separately.
    both = (0, 10, 60, 100)
    rows.append((seed_of(20, 16, 44, 44), both, 995_000))
    rows.append((seed_of(20, 66, 44, 94), both, 995_000))
    # 2: the diagonal-pinch bowl, whole bbox.
    rows.append((seed_of(80, 24, 89, 33), (78, 22, 101, 45), 995_000))
    # 3: a deep hollow with a bbox that cuts it in half.
    rows.append((seed_of(80, 90, 100, 110), (74, 84, 90, 116), 992_000))
    # 4: the same hollow, DRY -- surface one mm below its own floor.
    sx, sy = seed_of(80, 90, 100, 110)
    rows.append(((sx, sy), (74, 84, 106, 116), int(zmm[sy, sx]) - 1))

    blob = [struct.pack("<8sII", b"VXLKEXT1", W, H), zmm.tobytes()]
    blob.append(struct.pack("<I", len(rows)))
    masks = []
    for (sx, sy), (x0, y0, x1, y1), surf_mm in rows:
        m = bs.lake_extent_mask(z_m, (sx, sy), surf_mm / 1000.0, (x0, y0, x1, y1))
        sub = np.ascontiguousarray(m[y0:y1 + 1, x0:x1 + 1]).astype(np.uint8)
        blob.append(struct.pack("<6hiI", sx, sy, x0, y0, x1, y1, surf_mm,
                                int(sub.sum())))
        masks.append(sub.tobytes())
        print(f"  seed ({sx},{sy}) bbox ({x0},{y0})-({x1},{y1}) "
              f"surface {surf_mm / 1000:.3f} m -> {int(sub.sum())} wet cells")
    blob.extend(masks)

    # The claims the docstring makes, asserted here so a raster edit that
    # quietly makes a row uninteresting fails at generation instead of leaving
    # a test that passes for the wrong reason.
    m0 = bs.lake_extent_mask(z_m, rows[0][0], rows[0][2] / 1000.0, rows[0][1])
    m1 = bs.lake_extent_mask(z_m, rows[1][0], rows[1][2] / 1000.0, rows[1][1])
    x0, y0, x1, y1 = rows[0][1]
    thresh = (z_m[y0:y1 + 1, x0:x1 + 1] <= rows[0][2] / 1000.0)
    assert m0.any() and m1.any(), "rows 0/1 must both be wet"
    assert not (m0 & m1).any(), "rows 0/1 must be disjoint"
    assert int(m0.sum()) < int(thresh.sum()), "row 0 must be SMALLER than the threshold"
    assert int(m0.sum()) + int(m1.sum()) == int(thresh.sum()), \
        "rows 0+1 must together be the whole threshold set"
    lab, _, _ = bs.depression_components(z_m)
    assert int(lab[rows[2][0][1], rows[2][0][0]]) != 0
    m2 = bs.lake_extent_mask(z_m, rows[2][0], rows[2][2] / 1000.0, rows[2][1])
    # The diagonal pinch: 8-connected reaches BOTH lobes, so the mask must
    # straddle the cut row.
    assert m2[24:34, 80:90].any() and m2[34:44, 90:100].any(), \
        "row 2 must cross the diagonal pinch into the second lobe"
    m3 = bs.lake_extent_mask(z_m, rows[3][0], rows[3][2] / 1000.0, rows[3][1])
    m3full = bs.lake_extent_mask(z_m, rows[3][0], rows[3][2] / 1000.0,
                                 (0, 0, W - 1, H - 1))
    assert int(m3.sum()) < int(m3full.sum()), "row 3's bbox must actually clip"
    m4 = bs.lake_extent_mask(z_m, rows[4][0], rows[4][2] / 1000.0, rows[4][1])
    assert not m4.any(), "row 4 must be dry"

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(b"".join(blob))
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
