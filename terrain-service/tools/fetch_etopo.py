#!/usr/bin/env python3
"""Build ``data/global/etopo_10m.tif`` for terrain-diffusion's synthetic-map stats.

terrain-diffusion's ``synthetic_map._compute_map_stats`` opens the RELATIVE path
``data/global/etopo_10m.tif``. Its README says to download the NOAA ETOPO
"30 Arc-Second Resolution GeoTIFF" and drop it in ``data/global`` -- but the
filename it actually reads is the 10 ARC-MINUTE downsample (``_10m``, matching
the WorldClim ``wc2.1_10m_*`` rasters beside it), which NOAA does not publish.
So the file has to be built, not just downloaded.

This script downloads a coarse ETOPO GeoTIFF and resamples it onto the EXACT
grid of the WorldClim raster already present, by reading that raster's
transform/CRS/shape and reprojecting into it. Matching the neighbour's grid
rather than computing our own means the stats code lines the two up correctly
by construction.

Usage (run from terrain-service/, i.e. the dir containing data/global):
    python tools/fetch_etopo.py

Idempotent: exits early if data/global/etopo_10m.tif already exists.
"""

import os
import sys
import urllib.request

# NOAA has reorganised these paths more than once, so try known locations in
# order rather than pinning one that may 404. Coarsest acceptable source first:
# we are downsampling to 10 arc-minutes regardless, so a 60 arc-second source
# is plenty and is ~16x smaller than the 15 arc-second one.
CANDIDATE_URLS = [
    "https://www.ngdc.noaa.gov/thredds/fileServer/global/ETOPO2022/60s/60s_bed_elev_gtif/ETOPO_2022_v1_60s_N90W180_bed.tif",
    "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/60s_bed_elev_gtif/ETOPO_2022_v1_60s_N90W180_bed.tif",
    "https://gis.ngdc.noaa.gov/arcgis/rest/directories/arcgisjobs/dem_mosaic/ETOPO_2022_v1_60s_N90W180_bed.tif",
    "https://www.ngdc.noaa.gov/thredds/fileServer/global/ETOPO2022/60s/60s_surface_elev_gtif/ETOPO_2022_v1_60s_N90W180_surface.tif",
]

REFERENCE = "data/global/wc2.1_10m_bio_1.tif"
OUT = "data/global/etopo_10m.tif"
SRC = "data/global/_etopo_source.tif"


def _download(url: str, dest: str) -> bool:
    try:
        print(f"trying {url}")
        req = urllib.request.Request(url, headers={"User-Agent": "voxelsim-bringup"})
        with urllib.request.urlopen(req, timeout=60) as r, open(dest, "wb") as f:
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
    except Exception as e:  # noqa: BLE001 - any failure means "try the next URL"
        print(f"  failed: {e}")
        if os.path.exists(dest):
            os.remove(dest)
        return False


def main() -> int:
    if os.path.exists(OUT):
        print(f"{OUT} already exists -- nothing to do.")
        return 0

    if not os.path.exists(REFERENCE):
        print(f"ERROR: {REFERENCE} not found.")
        print("Run this from terrain-service/ AFTER the pipeline has fetched")
        print("WorldClim (it downloads on first run and prompts for consent).")
        return 2

    import numpy as np
    import rasterio
    from rasterio.warp import Resampling, reproject

    if not os.path.exists(SRC):
        for url in CANDIDATE_URLS:
            if _download(url, SRC):
                break
        else:
            print("\nERROR: every candidate URL failed.")
            print("Download the ETOPO 2022 60 or 30 arc-second bed-elevation")
            print("GeoTIFF by hand from")
            print("  https://www.ncei.noaa.gov/products/etopo-global-relief-model")
            print(f"save it as {SRC}, and re-run this script (it will skip the")
            print("download and just resample).")
            return 1

    with rasterio.open(REFERENCE) as ref:
        profile = ref.profile.copy()
        dst = np.zeros((ref.height, ref.width), dtype="float32")
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
                dst_transform=ref.transform,
                dst_crs=ref.crs,
                resampling=Resampling.average,
            )

    profile.update(dtype="float32", count=1, compress="deflate")
    with rasterio.open(OUT, "w", **profile) as out:
        out.write(dst, 1)

    print(f"wrote {OUT}  min={float(dst.min()):.1f} max={float(dst.max()):.1f}")
    print("(expect roughly -11000 .. 8800 m if the source covered full relief)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
