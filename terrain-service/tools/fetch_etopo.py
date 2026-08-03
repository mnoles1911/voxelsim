#!/usr/bin/env python3
"""Build ``data/global/etopo_10m.tif`` -- A NEW ONE. It will not be the shipping world's.

READ THIS BEFORE RUNNING IT
---------------------------
This script builds a conditioning raster. The world you already have was NOT
built by it, and cannot be rebuilt by it. That was measured on 2026-08-02 from
the canonical file's own TIFF tags (see
``terrain_service/conditioning_artifacts.py`` and
docs/measurements/etopo-build-not-reproducible-2026-08-02.txt):

  * the canonical ``etopo_10m.tif`` is UNCOMPRESSED -- 2160x1080 float32 =
    9,331,200 B plus 13,775 B of tags is exactly its 9,344,975 B length -- and
    this script has always written ``compress="deflate"``, which for those very
    pixels comes to 7,439,420 B at every zlib level;
  * the canonical file carries ETOPO's OWN georeferencing (``GeoAsciiParams``
    'WGS 84 + EGM2008 height', ``GDAL_NODATA`` '-99999', ModelPixelScale z=1.0),
    which a profile copied from ``wc2.1_10m_bio_1.tif`` -- what this script
    copies -- cannot produce;
  * the canonical file predates this script's first commit by 24 days.

So the honest description of this tool is: **it starts a new world.** That is a
legitimate thing to want, and it is why the tool still exists. It is not a
recovery path and it is not a fallback. To obtain the pinned bytes, use
``tools/fetch_conditioning.py``.

WHAT CHANGED, AND WHY IT STILL MATTERS
--------------------------------------
Even for a new world, a builder that lands on different bytes per box is
useless: two pods asked to build "the same" new world would get two. The
previous version had four sources of drift and now has none of them:

  1. FOUR CANDIDATE URLS, tried in order, including both a ``_bed`` and a
     ``_surface`` variant of ETOPO 2022. Whichever was reachable that day
     decided the bytes, and those two products differ by kilometres under ice
     -- the canonical file's pixels are ``_bed`` (Greenland centre -151 m, West
     Antarctica -1360 m; ``_surface`` reads about +3000 m and +1800 m there).
     NOW: one pinned URL for one pinned product. A 404 is a hard failure with a
     manual-download instruction, not a silent switch to a different planet.
  2. WHATEVER GDAL THE POD RESOLVED wrote the TIFF, with its own defaults for
     tiling, compression level, block size and metadata.
     NOW: every creation option is stated explicitly, and the rasterio/GDAL
     versions are printed and recorded so a future mismatch is diagnosable.
  3. GDAL's PAM sidecar (``.aux.xml``) and computed-statistics metadata could
     appear or not depending on version and on whether anything had opened the
     file.  NOW: PAM disabled, no statistics written.
  4. RESAMPLING was already pinned to ``average`` but nodata was not handled at
     all, so ETOPO's -99999 fill averaged into real cells wherever the source
     had gaps.  NOW: src/dst nodata are passed explicitly.

Usage (run from terrain-service/, i.e. the dir containing data/global):
    python3 tools/fetch_etopo.py --i-am-starting-a-new-world

Idempotent: exits early if data/global/etopo_10m.tif already exists.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import urllib.request

# ONE product, ONE URL. ETOPO 2022, 60 arc-second, BED elevation -- bed because
# that is what the existing world's raster contains, so a new world built here
# at least differs from it in as few dimensions as possible. If NOAA moves this
# path, FIX THE PIN in a commit; do not add a second candidate. A fallback list
# is how the drift got in.
SOURCE_URL = (
    "https://www.ngdc.noaa.gov/thredds/fileServer/global/ETOPO2022/60s/"
    "60s_bed_elev_gtif/ETOPO_2022_v1_60s_N90W180_bed.tif"
)

REFERENCE = "data/global/wc2.1_10m_bio_1.tif"
OUT = "data/global/etopo_10m.tif"
SRC = "data/global/_etopo_source.tif"

#: ETOPO 2022's own nodata value. Passed explicitly to reproject so gap cells
#: do not average into real elevations -- the old code passed neither src nor
#: dst nodata and silently blended -99999 into the histogram wherever the
#: source had holes.
SRC_NODATA = -99999.0
DST_NODATA = -99999.0

BANNER = """\
\033[33m\033[1m
  ============================================================================
  THIS BUILDS A NEW WORLD'S CONDITIONING RASTER.

  It does NOT reproduce data/global/etopo_10m.tif as pinned in
  data/conditioning-artifacts.json (sha256 9a45dd6d..., 9,344,975 B). That file
  was not produced by this script and cannot be -- it is uncompressed and
  carries ETOPO's own vertical datum, neither of which this code path can emit.

  Anything generated against the raster built here gets a DIFFERENT provider_id
  and lives in a DIFFERENT tile-cache namespace. That is correct and it is the
  point: two different planets must not share a namespace. Do not reach for
  --provider-id-override to make them look the same.

  To obtain the PINNED bytes instead:  python3 tools/fetch_conditioning.py
  ============================================================================
\033[0m"""


def _download(url: str, dest: str) -> bool:
    try:
        print(f"downloading {url}")
        req = urllib.request.Request(url, headers={"User-Agent": "voxelsim-bringup"})
        with urllib.request.urlopen(req, timeout=120) as r, open(dest, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            got = 0
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                if total:
                    print(f"\r  {got / 1e6:.0f} / {total / 1e6:.0f} MB", end="", flush=True)
            print()
        return True
    except Exception as e:  # noqa: BLE001
        print(f"  failed: {e}")
        if os.path.exists(dest):
            os.remove(dest)
        return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--i-am-starting-a-new-world",
        action="store_true",
        help=(
            "acknowledge that the raster this builds is NOT the pinned one and "
            "that anything generated from it is a separate world. Required."
        ),
    )
    args = ap.parse_args()

    print(BANNER)
    if not args.i_am_starting_a_new_world:
        print(
            "Refusing to build without --i-am-starting-a-new-world.\n"
            "\n"
            "This flag is not paperwork. Bootstrap used to call this script as a\n"
            "normal build step, which is how a pod silently produced a second\n"
            "planet and stamped it with the first one's seed. If you want the\n"
            "existing world's rasters, run:  python3 tools/fetch_conditioning.py\n",
            file=sys.stderr,
        )
        return 2

    if os.path.exists(OUT):
        print(f"{OUT} already exists -- nothing to do.")
        return 0

    if not os.path.exists(REFERENCE):
        print(f"ERROR: {REFERENCE} not found.")
        print("Run this from terrain-service/ AFTER the pipeline has fetched")
        print("WorldClim (it downloads on first run and prompts for consent).")
        return 2

    # PAM off BEFORE rasterio/GDAL is imported: a .aux.xml sidecar appearing (or
    # not) next to the output is one more thing that differs per box, and GDAL
    # reads this at driver-registration time.
    os.environ.setdefault("GDAL_PAM_ENABLED", "NO")

    import numpy as np
    import rasterio
    from rasterio.warp import Resampling, reproject

    # Recorded, not just printed: when a future box gets different bytes, the
    # first question is "which GDAL", and nobody will remember.
    print(f"rasterio {rasterio.__version__}  GDAL {rasterio.__gdal_version__}  numpy {np.__version__}")

    if not os.path.exists(SRC):
        if not _download(SOURCE_URL, SRC):
            print("\nERROR: the pinned ETOPO source URL failed.")
            print("This script deliberately has NO fallback URL: trying a second")
            print("product when the first is unreachable is exactly how two pods")
            print("ended up building two different planets.")
            print("\nDownload ETOPO 2022, 60 arc-second, BED elevation, GeoTIFF by hand")
            print("from https://www.ncei.noaa.gov/products/etopo-global-relief-model")
            print(f"save it as {SRC}, and re-run. If NOAA has moved the path for")
            print("good, update SOURCE_URL in this file in a commit.")
            return 1

    with rasterio.open(REFERENCE) as ref:
        profile = ref.profile.copy()
        dst = np.full((ref.height, ref.width), DST_NODATA, dtype="float32")
        print(f"reference grid: {ref.width}x{ref.height} crs={ref.crs}")

        with rasterio.open(SRC) as src:
            print(f"source grid:    {src.width}x{src.height} crs={src.crs}")
            # Average, not nearest: we are downsampling by ~6x per axis, and the
            # stats consume basin-scale elevation distribution. Nearest would
            # alias narrow trenches and ridges into the histogram.
            reproject(
                source=rasterio.band(src, 1),
                destination=dst,
                src_transform=src.transform,
                src_crs=src.crs,
                src_nodata=SRC_NODATA,
                dst_transform=ref.transform,
                dst_crs=ref.crs,
                dst_nodata=DST_NODATA,
                resampling=Resampling.average,
            )

    # EVERY creation option stated. Inheriting the reference's profile for
    # tiling/blocking/compression means the output depends on how WorldClim
    # happened to be written and on the writing GDAL's defaults for anything
    # the profile omits. Stated explicitly, two boxes with the same GDAL major
    # version produce the same container.
    profile.update(
        driver="GTiff",
        dtype="float32",
        count=1,
        nodata=DST_NODATA,
        compress="deflate",
        zlevel=6,
        predictor=1,
        tiled=False,
        blockysize=1,
        interleave="band",
        BIGTIFF="NO",
    )
    profile.pop("blockxsize", None)  # meaningless for a striped file; GDAL warns

    with rasterio.open(OUT, "w", **profile) as out:
        out.write(dst, 1)
        # Do not let GDAL attach computed STATISTICS_* metadata: it is written
        # by whoever opens the file next, with values that depend on the GDAL
        # version's approximation settings.
        out.update_tags(ns="IMAGE_STRUCTURE")

    size = os.path.getsize(OUT)
    sha = hashlib.sha256(open(OUT, "rb").read()).hexdigest()
    valid = dst[dst != DST_NODATA]
    print(f"wrote {OUT}  min={float(valid.min()):.1f} max={float(valid.max()):.1f}")
    print("(expect roughly -11000 .. 8800 m if the source covered full relief)")
    print(f"sha256 {sha}")
    print(f"size   {size:,} B")
    print(
        "\nRecord that sha256 in data/conditioning-artifacts.json IF you are"
        "\ndeliberately minting a new world, and rebuild synthetic_map_stats.json"
        "\nfrom it in the same breath -- it is derived from this file and every"
        "\nstale copy is a silent third planet."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
