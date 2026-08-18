#!/usr/bin/env python3
"""DEM reference library helper -- companion to docs/dem-reference-library.md.

INERT BY DESIGN. Importing this module touches no network and no filesystem.
Nothing happens until a subcommand is explicitly invoked, and the only
subcommand that transfers bytes (--fetch) is guarded by --max-mb (default
200 MB) so it cannot be casually pointed at fine-resolution data.

Subcommands:
    --list                     print the region table
    --tiles REGION             print the GLO-30/GLO-90 tile URLs covering a
                               region's bbox (no download)
    --fetch REGION --dest DIR  download those tiles, sha256 each AS RECEIVED,
                               verify byte count against Content-Length, and
                               append entries to <dest>/manifest.json
    --inspect FILE             size + sha256 (+ raster stats if rasterio is
                               importable)

House rules honoured here (see conditioning_artifacts.py for why):
  * hashes are computed on the bytes that arrived, BEFORE any use;
  * transfers are verified by byte count, never by exit status alone;
  * the AWS key-naming assumption is HEAD-checked and failure is loud.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from dataclasses import dataclass

# ---------------------------------------------------------------------------
# The library (docs/dem-reference-library.md §3). Bboxes are (south, north,
# west, east) in degrees, sized to 2x2 coarse tiles (30.72 km) except hunza
# at 3x3. Keep this table in sync with the doc; the doc is the authority.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Region:
    key: str
    name: str
    grammar: str
    south: float
    north: float
    west: float
    east: float


REGIONS: tuple[Region, ...] = (
    Region("milford", "Milford Sound, NZ", "fjord", -44.758, -44.482, 167.706, 168.094),
    Region("geiranger", "Geirangerfjord, Norway", "fjord (alternate)", 61.962, 62.238, 6.705, 7.295),
    Region("lofoten", "Lofoten (Reine), Norway", "alpine archipelago", 67.812, 68.088, 12.633, 13.367),
    Region("faroes", "Northern Faroes (Enniberg)", "basalt ramp / sea cliff", 62.182, 62.458, -6.847, -6.253),
    Region("herdubreid", "Herdubreid, Iceland", "tuya + lava tableland", 65.042, 65.318, -16.679, -16.021),
    Region("skeidara", "Skeidararsandur-Oraefajokull, Iceland", "glacial outwash + escarpment", 63.852, 64.128, -17.165, -16.535),
    Region("hunza", "Hunza-Rakaposhi, Karakoram", "massive mountains", 36.073, 36.487, 74.293, 74.807),
    Region("wadirum", "Wadi Rum, Jordan", "monumental rock desert", 29.432, 29.708, 35.261, 35.579),
    Region("namib", "Sossusvlei erg, Namib", "dune sea", -24.868, -24.592, 15.188, 15.492),
    Region("canyonlands", "Island in the Sky, Canyonlands", "mesa/canyon", 38.262, 38.538, -110.076, -109.724),
    Region("guilin", "Guilin-Yangshuo, China", "tower karst", 24.642, 24.918, 110.338, 110.642),
    Region("wv", "Appalachian Plateau, WV", "temperate rolling forest", 38.362, 38.638, -80.676, -80.324),
    Region("rainier", "Mt. Rainier, WA (optional)", "stratovolcano", 46.712, 46.988, -121.962, -121.558),
)

#: AWS Open Data buckets (public HTTPS, no account). Key naming per the
#: registry; --fetch and --tiles HEAD-check before trusting it.
BUCKETS = {
    "glo30": ("copernicus-dem-30m", "Copernicus_DSM_COG_10_{lat}_00_{lon}_00_DEM"),
    "glo90": ("copernicus-dem-90m", "Copernicus_DSM_COG_30_{lat}_00_{lon}_00_DEM"),
}
AWS_REGION = "eu-central-1"


def find_region(key: str) -> Region:
    for r in REGIONS:
        if r.key == key:
            return r
    sys.exit(f"unknown region {key!r}; use --list (keys: {', '.join(r.key for r in REGIONS)})")


def tile_urls(region: Region, source: str) -> list[str]:
    """1x1-degree COG tiles covering the bbox, as public HTTPS URLs."""
    bucket, stem_fmt = BUCKETS[source]
    urls = []
    for lat in range(math.floor(region.south), math.ceil(region.north)):
        for lon in range(math.floor(region.west), math.ceil(region.east)):
            lat_s = f"N{lat:02d}" if lat >= 0 else f"S{-lat:02d}"
            lon_s = f"E{lon:03d}" if lon >= 0 else f"W{-lon:03d}"
            stem = stem_fmt.format(lat=lat_s, lon=lon_s)
            urls.append(f"https://{bucket}.s3.{AWS_REGION}.amazonaws.com/{stem}/{stem}.tif")
    return urls


def head(url: str) -> tuple[int, int]:
    """(status, content_length). Network happens ONLY here and in fetch()."""
    import urllib.request

    req = urllib.request.Request(url, method="HEAD")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, int(resp.headers.get("Content-Length", -1))
    except Exception as exc:  # noqa: BLE001 - report status, don't guess
        return getattr(exc, "code", -1), -1


def cmd_tiles(region: Region, source: str) -> None:
    urls = tile_urls(region, source)
    status, length = head(urls[0])
    if status != 200:
        sys.exit(
            f"HEAD {urls[0]} -> {status}: the AWS key-naming assumption is wrong "
            "or this tile is one of the withheld GLO-30 country tiles. "
            "Fix the naming (docs §7) rather than trusting the rest of this list."
        )
    print(f"# {region.name} ({region.grammar}) -- {len(urls)} tile(s), first verified 200")
    for u in urls:
        print(u)


def cmd_fetch(region: Region, source: str, dest: str, max_mb: int) -> None:
    import os
    import urllib.request

    urls = tile_urls(region, source)
    sizes = []
    for u in urls:
        status, length = head(u)
        if status != 200 or length <= 0:
            sys.exit(f"HEAD {u} -> status {status}, length {length}; refusing to fetch blind.")
        sizes.append(length)
    total_mb = sum(sizes) / 1e6
    if total_mb > max_mb:
        sys.exit(
            f"projected transfer {total_mb:.0f} MB exceeds --max-mb {max_mb}. "
            "This guard exists so the script cannot be casually pointed at big data; "
            "raise it EXPLICITLY if you mean it."
        )
    os.makedirs(dest, exist_ok=True)
    manifest_path = os.path.join(dest, "manifest.json")
    entries = []
    if os.path.exists(manifest_path):
        with open(manifest_path, "r", encoding="utf-8") as f:
            entries = json.load(f)
    for u, expect in zip(urls, sizes):
        name = u.rsplit("/", 1)[1]
        path = os.path.join(dest, name)
        with urllib.request.urlopen(u, timeout=120) as resp, open(path, "wb") as out:
            data = resp.read()
            out.write(data)
        if len(data) != expect:
            sys.exit(f"{name}: got {len(data)} bytes, Content-Length said {expect}; transfer is BAD, stopping.")
        digest = hashlib.sha256(data).hexdigest()
        entries.append({
            "region": region.key,
            "source": source,
            "url": u,
            "bytes": len(data),
            "sha256": digest,
        })
        print(f"{name}  {len(data):>12} B  sha256={digest}")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(entries, f, indent=2)
    print(f"manifest: {manifest_path} ({len(entries)} entries)")


def cmd_inspect(path: str) -> None:
    with open(path, "rb") as f:
        data = f.read()
    print(f"{path}: {len(data)} B  sha256={hashlib.sha256(data).hexdigest()}")
    try:
        import rasterio  # type: ignore
    except ImportError:
        print("(rasterio not importable; skipping raster stats)")
        return
    with rasterio.open(path) as ds:
        band = ds.read(1, masked=True)
        print(f"  size {ds.width}x{ds.height}  crs {ds.crs}  posting {ds.res}")
        print(f"  min {band.min():.1f}  max {band.max():.1f}  mean {band.mean():.1f}")


def main(argv: list[str] | None = None) -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--list", action="store_true")
    p.add_argument("--tiles", metavar="REGION")
    p.add_argument("--fetch", metavar="REGION")
    p.add_argument("--inspect", metavar="FILE")
    p.add_argument("--source", choices=sorted(BUCKETS), default="glo30")
    p.add_argument("--dest", metavar="DIR")
    p.add_argument("--max-mb", type=int, default=200)
    args = p.parse_args(argv)

    if args.list:
        for r in REGIONS:
            print(f"{r.key:12s} {r.name:40s} {r.grammar:32s} "
                  f"[{r.south:.3f}..{r.north:.3f}, {r.west:.3f}..{r.east:.3f}]")
    elif args.tiles:
        cmd_tiles(find_region(args.tiles), args.source)
    elif args.fetch:
        if not args.dest:
            sys.exit("--fetch requires --dest DIR (put it on the data drive, never in git)")
        cmd_fetch(find_region(args.fetch), args.source, args.dest, args.max_mb)
    elif args.inspect:
        cmd_inspect(args.inspect)
    else:
        p.print_help()


if __name__ == "__main__":
    main()
