#!/usr/bin/env python3
"""Did the ground move between two baked fine namespaces? Answered from the
SHIPPED BYTES, with no re-bake and no npz dump.

    python tools/compare_fine_namespaces.py \
        --cache-dir D:/voxelsim/tile-cache \
        --a terrain-diffusion-unlabeled-80b9ca451a23eae4-b196f6020 \
        --b terrain-diffusion-unlabeled-80b9ca451a23eae4-b52995abb

WHY THIS EXISTS, AND WHY IT IS NOT ``verify_water_only_change.py``
-----------------------------------------------------------------
That tool is the guardrail for a change you are MAKING: it wants both bakes'
``--bake-npz-dir`` dumps, compares the unquantised float32 ``elevation_m``, and
is the strongest available evidence because it can see a perturbation below the
100 mm wire LSB. Use it when you have the dumps.

This tool answers the question you are left with when you DON'T: two namespaces
are sitting in the cache, one of them was baked weeks ago by a process that is
long gone, and someone needs to know whether the tiles in them describe the same
ground. The only evidence is the .vxtl files. That is a weaker question -- it can
only see differences at or above the wire LSB -- and it is the question that
actually gets asked, because a shipped namespace has no npz beside it.

WHAT IT REPORTS AND WHY EACH COLUMN IS THERE
--------------------------------------------
  cp differ     elevation control points, through ``tile_codec.decode_v2``.
                NEVER a hand-rolled quantiser: the section 2 prefilter is part
                of the chain that produces a control point, and leaving it out
                has already produced one false "terrain moved" alarm.
  max mm        the largest difference in MILLIMETRES, via ``QUANT_MM[quant]``.
                Note that ``quant == 1`` is the CODE for ``QUANT_100MM``, so one
                LSB is 100 mm -- one voxel edge -- and not 1 mm. Reading the
                code as a length has already produced one report that was wrong
                by 100x.
  datum         ``base_offset_mm`` and ``quant`` on each side. Two tiles can
                hold identical control-point integers on different datums and
                describe different ground; without this column "bit-identical
                cp" is not a statement about a surface.
  flow / water  the two other planes, exact. The flow plane carries the log2
                catchment area, i.e. the AREA field -- the thing a routing
                change moves if it has leaked out of the water pass.
  sections      per-section sha256 over the raw section bytes. This is what
                actually decides whether a client holding the old tile would
                have to re-fetch anything, and it is NOT implied by the columns
                above: a RAW tile and a ZSTD tile of the same coordinates decode
                to identical planes and share a ``fine_provider_id`` while
                sharing not one byte (``pregen._resolve_codec``). If the codecs
                differ this table will say so and the digests mean nothing
                about the ground -- read the plane columns instead.

Exits non-zero if any PLANE differs, so it can gate a claim. A differing
section digest alone is not a failure: that is a re-encoding, not moved ground.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402

SECTION_NAMES = {
    tc.SECTION_ELEV_INDEX: "ELEV_INDEX",
    tc.SECTION_ELEV_DATA: "ELEV_DATA",
    tc.SECTION_FLOW_INDEX: "FLOW_INDEX",
    tc.SECTION_FLOW_DATA: "FLOW_DATA",
    tc.SECTION_BASIN_TABLE: "BASIN_TABLE",
    tc.SECTION_WATER_INDEX: "WATER_INDEX",
    tc.SECTION_WATER_DATA: "WATER_DATA",
}


def _sections(blob: bytes) -> dict[int, tuple[int, int]]:
    """{section_id: (offset, length)}, walked with the codec's own structs."""
    off = tc._HEADER.size
    ext = tc._V2_EXT.unpack_from(blob, off)
    off += tc._V2_EXT.size
    out = {}
    for i in range(ext[-1]):
        sid, soff, slen = tc._SECTION_ENTRY.unpack_from(
            blob, off + i * tc._SECTION_ENTRY.size
        )
        out[sid] = (soff, slen)
    return out


def _codec_of(blob: bytes) -> int:
    # _V2_EXT is (block_log2, quant, predictor, codec, bake_ver, flags,
    # base_offset_mm, parent_scale, reserved, n_sections) -- see tile_codec.
    return tc._V2_EXT.unpack_from(blob, tc._HEADER.size)[3]


def _tiles_in(d: Path) -> list[tuple[int, int]]:
    out = []
    for p in d.glob("*.vxtl"):
        try:
            x, y = p.stem.split("_")
            out.append((int(x), int(y)))
        except ValueError:
            continue
    return sorted(out)


def _count_diff(a, b) -> int:
    if a is None and b is None:
        return 0
    if a is None or b is None:
        return -1  # present on one side only
    return int(np.count_nonzero(np.asarray(a) != np.asarray(b)))


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--cache-dir", default="D:/voxelsim/tile-cache")
    ap.add_argument("--a", required=True, help="first fine namespace directory name")
    ap.add_argument("--b", required=True, help="second fine namespace directory name")
    ap.add_argument("--seed-hex", default="000000000135276f")
    ap.add_argument("--tiles", default=None,
                    help='limit to "X,Y X,Y"; default is every tile in both')
    ap.add_argument("--sections", action="store_true",
                    help="also print the per-section digest table")
    args = ap.parse_args()

    root = Path(args.cache_dir)
    da = root / args.a / args.seed_hex / f"s{tc.FINE_SCALE}"
    db = root / args.b / args.seed_hex / f"s{tc.FINE_SCALE}"
    for d in (da, db):
        if not d.is_dir():
            print(f"error: {d} is not a directory", file=sys.stderr)
            return 2

    if args.tiles:
        want = []
        for part in args.tiles.replace(";", " ").split():
            x, y = part.split(",")
            want.append((int(x), int(y)))
    else:
        want = sorted(set(_tiles_in(da)) & set(_tiles_in(db)))
    if not want:
        print("error: no tile is present in both namespaces", file=sys.stderr)
        return 2

    print(f"A = {args.a}")
    print(f"B = {args.b}")
    print(f"{len(want)} tile(s) in both\n")
    hdr = (f"{'tile':>9} {'bake_ver':>9} {'codec':>10} {'datum A':>16} "
           f"{'datum B':>16} {'cp differ':>10} {'max mm':>8} "
           f"{'flow':>8} {'water':>9}")
    print(hdr)
    print("-" * len(hdr))

    tot_cp = tot_cp_d = 0
    moved = False
    per_tile_sections = []
    for (x, y) in want:
        pa, pb = da / f"{x}_{y}.vxtl", db / f"{x}_{y}.vxtl"
        ba, bb = pa.read_bytes(), pb.read_bytes()
        ta, tb = tc.decode_v2(ba), tc.decode_v2(bb)

        n_cp = _count_diff(ta.elevation_cp, tb.elevation_cp)
        # Compare ABSOLUTE height, not the raw integers: a shifted datum with
        # shifted integers is the same ground, and equal integers on different
        # datums are not.
        if ta.base_offset_mm != tb.base_offset_mm or ta.quant != tb.quant:
            mm_a = ta.elevation_cp.astype(np.int64) * tc.QUANT_MM[ta.quant] + ta.base_offset_mm
            mm_b = tb.elevation_cp.astype(np.int64) * tc.QUANT_MM[tb.quant] + tb.base_offset_mm
            delta = mm_a - mm_b
            n_cp = int(np.count_nonzero(delta))
            max_mm = int(np.abs(delta).max()) if n_cp else 0
        else:
            max_mm = (int(np.abs(ta.elevation_cp.astype(np.int64)
                                 - tb.elevation_cp.astype(np.int64)).max())
                      * tc.QUANT_MM[ta.quant]) if n_cp else 0

        n_flow = _count_diff(ta.flow, tb.flow)
        n_water = _count_diff(ta.water_cp, tb.water_cp)
        tot_cp += ta.elevation_cp.size
        tot_cp_d += max(n_cp, 0)
        if n_cp or n_flow > 0:
            moved = True

        ca, cb = _codec_of(ba), _codec_of(bb)
        codec = f"{'zstd' if ca else 'raw'}/{'zstd' if cb else 'raw'}"
        print(f"{f'{x},{y}':>9} {f'{ta.bake_ver}/{tb.bake_ver}':>9} {codec:>10} "
              f"{f'{ta.base_offset_mm}@{tc.QUANT_MM[ta.quant]}':>16} "
              f"{f'{tb.base_offset_mm}@{tc.QUANT_MM[tb.quant]}':>16} "
              f"{n_cp:>10} {max_mm:>8} {n_flow:>8} {n_water:>9}")

        if args.sections:
            sa, sb = _sections(ba), _sections(bb)
            rows = []
            for sid in sorted(set(sa) | set(sb)):
                def dig(blob, sec):
                    if sid not in sec:
                        return None, 0
                    o, n = sec[sid]
                    return hashlib.sha256(blob[o:o + n]).hexdigest()[:10], n
                ha, na = dig(ba, sa)
                hb, nb = dig(bb, sb)
                rows.append((SECTION_NAMES.get(sid, str(sid)), ha, na, hb, nb))
            per_tile_sections.append(((x, y), ca != cb, rows))

    print(f"\nTOTAL {tot_cp_d} of {tot_cp} elevation control points differ "
          f"({100.0 * tot_cp_d / max(tot_cp, 1):.7f}%)")

    if args.sections:
        for (x, y), codec_differs, rows in per_tile_sections:
            print(f"\n  sections, tile {x},{y}"
                  + ("   [CODECS DIFFER -- digests say nothing about the ground]"
                     if codec_differs else ""))
            same = sum(n for _, ha, n, hb, _ in rows if ha == hb and ha is not None)
            diff = sum(nb for _, ha, _, hb, nb in rows if ha != hb)
            for name, ha, na, hb, nb in rows:
                mark = "same" if (ha == hb and ha is not None) else "DIFFER"
                print(f"    {name:>12}  {str(ha):>10} {na/1e6:8.3f} MB   "
                      f"{str(hb):>10} {nb/1e6:8.3f} MB   {mark}")
            tot = same + diff
            if tot:
                print(f"    {'':>12}  changed bytes: {diff/1e6:.3f} MB of "
                      f"{tot/1e6:.3f} MB ({100.0 * diff / tot:.2f}%)")

    if moved:
        print("\nGROUND MOVED: a plane differs. This is NOT a water-only change.")
        return 1
    print("\nGround did not move: every elevation control point and flow cell "
          "agrees.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
