"""Measure what index-then-range fetching buys on real fine tiles (task #52).

    python tools/tile_slice_measure.py <dir-of-vxtl> [--verify] [--http]

`--http` re-runs one working set through the real Flask `/tile` endpoint
(in-process test client, no network) so the 206 path is exercised against a
50 MB tile and not only against a unit-test fixture. It asserts the bytes are
identical to the file-read path; if `/tile` ever stops honouring Range this
reports it instead of quietly downloading 50 MB per block.

Reports, per tile and per working set, the bytes and REQUESTS an
index-then-range fetch costs against re-downloading the whole 32-56 MB tile.

The working sets are the ones a client actually has:

  block-raw     one 480 m block of elevation, UNDILATED. The floor: what the
                bytes for "one block of ground" cost if nothing else is read.
                Not a shippable working set -- a generator over this block also
                reads the carrier stencil and its own cavern margin outside it,
                and undersizing that does not fault, it silently returns edge
                values (tilestreaming.h, dilateForCarrierStencil).
  ground-1blk   the same block, carrier- and cavern-dilated: the smallest
                CORRECT ground fetch. Elevation only -- a mesher that wants
                ground and nothing else can skip both other plane indices.
  full-1blk     the same block with flow and water as well, i.e. everything
                needed to draw ground AND put water on it.
  ring-3x3      a 1.44 km neighbourhood, three planes. What a player standing
                still has resident.
  water-only    every non-CONSTANT water block plus the water index and the
                basin table -- the shape of a water-only re-bake refresh.

`--verify` decodes the whole tile with tile_codec.decode_v2 and asserts every
sliced block is bit-identical to the corresponding slab. Slow (a full decode
per tile) and worth it: a range fetcher that is off by one block still returns
plausible terrain.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc          # noqa: E402
from terrain_service import tile_slice as ts          # noqa: E402

#: vxc::kCavernMaxReachMm = 36,394 mm, at 1,875 mm/px -> 20 px (rounded up).
#: The UE host's own generation reach, on top of the carrier stencil; see
#: tile_slice.dilated_block_coverage.
CAVERN_MARGIN_PX = 20


def human(n: int) -> str:
    for unit, div in (("MB", 1 << 20), ("KB", 1 << 10)):
        if n >= div:
            return f"{n / div:,.1f} {unit}"
    return f"{n} B"


def _plane_sets(pre: ts.TilePreamble, want: tuple[str, ...]):
    named = {"elev": pre.elevation, "flow": pre.flow, "water": pre.water}
    return [(k, named[k]) for k in want if named[k] is not None]


def measure(path: Path, *, verify: bool, coalesce_gap: int) -> dict:
    total = path.stat().st_size
    rows = []

    # ---- what one preamble costs, per plane appetite -----------------------
    with ts.FileRangeSource(path) as src:
        pre = ts.read_preamble(src)
        preamble_all = (src.bytes_fetched, src.requests)
    with ts.FileRangeSource(path) as src:
        ts.read_preamble(src, want_flow=False, want_water=False, want_basins=False)
        preamble_ground = (src.bytes_fetched, src.requests)

    nb = pre.blocks_per_axis
    bs = pre.block_dim_px
    mid = nb // 2

    def run(name: str, want: tuple[str, ...], blocks_for, *, ground_only=False):
        with ts.FileRangeSource(path) as src:
            p = ts.read_preamble(
                src,
                want_flow=not ground_only and "flow" in want,
                want_water=not ground_only and "water" in want,
                want_basins=not ground_only,
            )
            plans_all = []
            decoded = {}
            for key, plane in _plane_sets(p, want):
                blks = blocks_for(plane)
                got, plans = ts.fetch_blocks(
                    src, plane, blks, codec=p.codec, coalesce_gap=coalesce_gap
                )
                decoded[key] = (plane, got)
                plans_all += plans
            rows.append(dict(
                name=name, bytes=src.bytes_fetched, requests=src.requests,
                spans=len(plans_all),
                wasted=sum(pl.wasted_bytes for pl in plans_all),
                blocks=sum(len(g) for _, g in decoded.values()),
            ))
        return decoded

    # one 480 m block at tile centre, dilated by the carrier stencil + the
    # host's cavern reach -- what a chunk generator over that block may READ.
    def one_block(plane):
        return ts.dilated_block_coverage(
            mid * bs, mid * bs, (mid + 1) * bs - 1, (mid + 1) * bs - 1,
            block_dim_px=plane.block_dim_px, blocks_per_axis=plane.blocks_per_axis,
            extra_margin_px=CAVERN_MARGIN_PX,
        )

    def ring_3x3(plane):
        return ts.dilated_block_coverage(
            (mid - 1) * bs, (mid - 1) * bs, (mid + 2) * bs - 1, (mid + 2) * bs - 1,
            block_dim_px=plane.block_dim_px, blocks_per_axis=plane.blocks_per_axis,
            extra_margin_px=CAVERN_MARGIN_PX,
        )

    def all_wet(plane):
        return [(i % plane.blocks_per_axis, i // plane.blocks_per_axis)
                for i, e in enumerate(plane.entries) if not e.is_constant]

    def raw_block(plane):
        return [(mid, mid)]

    run("block-raw", ("elev",), raw_block, ground_only=True)
    run("ground-1blk", ("elev",), one_block, ground_only=True)
    run("full-1blk", ("elev", "flow", "water"), one_block)
    run("ring-3x3", ("elev", "flow", "water"), ring_3x3)
    run("water-only", ("water",), all_wet)

    verified = None
    if verify:
        full = tc.decode_v2(path.read_bytes())
        planes = {"elev": full.elevation_cp, "flow": full.flow, "water": full.water_cp}
        n = 0
        with ts.FileRangeSource(path) as src:
            p = ts.read_preamble(src)
            for key, plane in _plane_sets(p, ("elev", "flow", "water")):
                blks = ring_3x3(plane) + [(0, 0), (nb - 1, nb - 1), (nb - 1, 0)]
                got, _ = ts.fetch_blocks(src, plane, blks, codec=p.codec,
                                         coalesce_gap=coalesce_gap)
                for (bx, by), arr in got.items():
                    want = planes[key][by * bs:(by + 1) * bs, bx * bs:(bx + 1) * bs]
                    if not np.array_equal(arr, want):
                        raise AssertionError(f"{path.name} {key} block ({bx},{by}) differs")
                    n += 1
        verified = n

    return dict(
        path=path, total=total, preamble_all=preamble_all,
        preamble_ground=preamble_ground, preamble_content=pre.preamble_bytes,
        rows=rows, verified=verified,
        const_share={k: v.constant_share()
                     for k, v in _plane_sets(pre, ("elev", "flow", "water"))},
        nb=nb, bs=bs, water_data=pre.sections.get(tc.SECTION_WATER_DATA, (0, 0))[1],
    )


def over_http(root: Path, path: Path, coalesce_gap: int) -> dict:
    """The same ground fetch, but through Flask /tile with a Range header.

    Serves the SHIPPED cache read-only: the provider is a stub whose
    provider_id is the cache directory's own name, so nothing is generated and
    nothing is written. `root` is <cache>/<provider_id>/<seed>/s16.
    """
    from terrain_service.app import create_app
    from terrain_service.cache import TileCache

    seed_dir = root.parent
    provider_dir = seed_dir.parent
    cache_root = provider_dir.parent
    seed = int(seed_dir.name, 16)
    x, y = (int(v) for v in path.stem.split("_"))

    class _Stub:
        provider_id = provider_dir.name

        def generate(self, *a, **k):        # never called: every tile is cached
            raise AssertionError("measurement must not generate tiles")

    app = create_app(provider=_Stub(), cache=TileCache(cache_root))
    client = app.test_client()
    url = f"/tile?seed={seed}&x={x}&y={y}&scale=16"

    src = ts.HttpRangeSource(client.get, url)
    pre = ts.read_preamble(src, want_flow=False, want_water=False, want_basins=False)
    mid = pre.blocks_per_axis // 2
    bs = pre.block_dim_px
    blocks = ts.dilated_block_coverage(
        mid * bs, mid * bs, (mid + 1) * bs - 1, (mid + 1) * bs - 1,
        block_dim_px=bs, blocks_per_axis=pre.blocks_per_axis,
        extra_margin_px=CAVERN_MARGIN_PX,
    )
    got, _ = ts.fetch_blocks(src, pre.elevation, blocks, codec=pre.codec,
                             coalesce_gap=coalesce_gap)

    with ts.FileRangeSource(path) as fsrc:
        fpre = ts.read_preamble(fsrc, want_flow=False, want_water=False,
                                want_basins=False)
        fgot, _ = ts.fetch_blocks(fsrc, fpre.elevation, blocks, codec=fpre.codec,
                                  coalesce_gap=coalesce_gap)
    for k in got:
        if not np.array_equal(got[k], fgot[k]):
            raise AssertionError(f"{path.name}: HTTP block {k} differs from the file read")
    return dict(bytes=src.bytes_fetched, requests=src.requests, blocks=len(got))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--http", action="store_true")
    ap.add_argument("--coalesce-gap", type=int, default=ts.DEFAULT_COALESCE_GAP)
    args = ap.parse_args(argv)

    tiles = sorted(args.root.glob("*.vxtl"))
    if not tiles:
        print(f"no .vxtl under {args.root}", file=sys.stderr)
        return 2

    agg: dict[str, list[tuple[int, int, int]]] = {}
    grand_total = 0
    for path in tiles:
        m = measure(path, verify=args.verify, coalesce_gap=args.coalesce_gap)
        grand_total += m["total"]
        print(f"\n=== {path.name}  whole tile {human(m['total'])} "
              f"({m['total']:,} B), {m['nb']}x{m['nb']} blocks of {m['bs']} px "
              f"({m['bs'] * 1875 // 1000} m)")
        print(f"    CONSTANT share  elev {m['const_share']['elev']:.1%}  "
              f"flow {m['const_share'].get('flow', 0):.1%}  "
              f"water {m['const_share'].get('water', 0):.1%}"
              f"   WATER_DATA {human(m['water_data'])}")
        pa, pg = m["preamble_all"], m["preamble_ground"]
        print(f"    preamble  3 planes+basins {human(pa[0])} in {pa[1]} req"
              f"   |  elevation only {human(pg[0])} in {pg[1]} req"
              f"   (content {m['preamble_content']:,} B; the rest is the "
              f"{ts.HEAD_PROBE_BYTES // 1024} KB head probe over-reading, "
              f"which is under the BDP and so costs no wall time)")
        print(f"    {'working set':<14} {'blocks':>7} {'bytes':>12} {'req':>5} "
              f"{'spans':>6} {'coalesce waste':>15} {'vs whole tile':>14}")
        for r in m["rows"]:
            agg.setdefault(r["name"], []).append((r["bytes"], r["requests"], m["total"]))
            print(f"    {r['name']:<14} {r['blocks']:>7} {r['bytes']:>12,} "
                  f"{r['requests']:>5} {r['spans']:>6} {r['wasted']:>15,} "
                  f"{m['total'] / max(r['bytes'], 1):>13.0f}x")
        if m["verified"] is not None:
            print(f"    verified {m['verified']} blocks bit-identical to decode_v2")
        if args.http:
            h = over_http(args.root, path, args.coalesce_gap)
            print(f"    over HTTP /tile with Range: ground-1blk {h['blocks']} blocks, "
                  f"{h['bytes']:,} B in {h['requests']} req (206), bytes identical "
                  f"to the file read")

    print(f"\n=== totals over {len(tiles)} tiles "
          f"(whole tiles = {human(grand_total)}, {grand_total:,} B)")
    print(f"    {'working set':<14} {'bytes':>14} {'req':>6} {'saving':>10}")
    for name, xs in agg.items():
        b = sum(x[0] for x in xs)
        q = sum(x[1] for x in xs)
        print(f"    {name:<14} {b:>14,} {q:>6} {1 - b / grand_total:>9.3%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
