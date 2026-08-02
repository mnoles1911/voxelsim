"""Task #39: does the province discriminant label REAL Earth correctly?

The bake classifies terrain into FLUVIAL / GLACIAL / ARID / LOWLAND and erodes
each with different constants. Nobody has checked those labels against ground
truth. The Earth reference corpus already carries 12 sites with a hand-assigned
`geomorphic_class` and cached elevation rasters, and WorldClim gives the real
climate at each -- so the discriminant can be run on real terrain with real
climate and its answers compared to what the site actually is.

The specific worry: province_cold_c is -2.0 C and temperature dominates
aridity, so an arid tile at -3 C classifies GLACIAL. If the Badlands come back
GLACIAL, the DISCRIMINANT is wrong rather than the multipliers -- a different
and prior problem to the one Wave 4 left open.
"""
import json
import pathlib

import numpy as np
import rasterio
from rasterio.warp import transform as warp_transform

from terrain_service.bake import pipeline as bp
from terrain_service.bake import province as _province

ROOT = pathlib.Path("D:/vox-int/terrain-service/data/earth_reference")
WC = "D:/terrain-diffusion/data/global"
G, C = bp.PRODUCTION, bp.CONSTANTS
N = 512                      # 512 px at 30 m = 15.4 km, one tile's worth
PAD = G.apron_coarse_px

# our uint8 encodings, from providers.diffusion EXPECTED_CHANNELS
RANGES = {"temperature": (-40.0, 40.0), "seasonality": (0.0, 3000.0),
          "precipitation": (0.0, 12000.0), "precip_variability": (0.0, 200.0)}
WCFILE = {"temperature": "wc2.1_10m_bio_1.tif", "seasonality": "wc2.1_10m_bio_4.tif",
          "precipitation": "wc2.1_10m_bio_12.tif", "precip_variability": "wc2.1_10m_bio_15.tif"}


def climate_at(lat, lon):
    out = {}
    for name, fn in WCFILE.items():
        with rasterio.open(f"{WC}/{fn}") as s:
            r, c = s.index(lon, lat)
            v = float(s.read(1, window=((r, r + 1), (c, c + 1)))[0, 0])
        out[name] = v
    return out


def to_u8(vals):
    planes = np.zeros((4, N + 2 * PAD, N + 2 * PAD), np.uint8)
    for i, name in enumerate(_province.CLIMATE_ORDER):
        lo, hi = RANGES[name]
        u = (vals[name] - lo) / (hi - lo)
        planes[i] = np.uint8(np.clip(round(u * 255.0), 0, 255))
    return planes


def elev_window(tif, lat, lon):
    with rasterio.open(tif) as s:
        xs, ys = warp_transform("EPSG:4326", s.crs, [lon], [lat])
        r, c = s.index(xs[0], ys[0])
        n = N + 2 * PAD
        # Copernicus is ~30 m at the equator but arc-second spaced; take n rows/cols
        r0, c0 = int(r - n // 2), int(c - n // 2)
        if r0 < 0 or c0 < 0 or r0 + n > s.height or c0 + n > s.width:
            return None
        a = s.read(1, window=((r0, r0 + n), (c0, c0 + n))).astype(np.float32)
    a[a < -1000] = np.nan
    if not np.isfinite(a).all():
        a = np.nan_to_num(a, nan=float(np.nanmedian(a)))
    return a


man = json.load(open(ROOT / "manifest.json"))
rows = []
for site in man["sites"]:
    cop = site.get("cop30") or {}
    tif = cop.get("cached_path") or cop.get("path") or cop.get("local")
    if tif:
        tif = pathlib.Path(tif)
        if not tif.is_absolute():
            tif = ROOT / "cache" / tif
    if not tif or not pathlib.Path(tif).exists():
        cands = list((ROOT / "cache" / "cop30").glob("*.tif"))
        tif = None
        for cand in cands:
            try:
                with rasterio.open(cand) as s:
                    xs, ys = warp_transform("EPSG:4326", s.crs, [site["lon"]], [site["lat"]])
                    r, c = s.index(xs[0], ys[0])
                    if 0 <= r < s.height and 0 <= c < s.width:
                        tif = cand
                        break
            except Exception:
                continue
    if tif is None:
        rows.append((site["id"], site["geomorphic_class"], "-", "NO RASTER", {}))
        continue
    e = elev_window(tif, site["lat"], site["lon"])
    if e is None:
        rows.append((site["id"], site["geomorphic_class"], "-", "WINDOW OFF EDGE", {}))
        continue
    cl = climate_at(site["lat"], site["lon"])
    pf = _province.province_fields(e, to_u8(cl), coarse_pixel_m=G.coarse_pixel_m,
                                   consts=C, max_half=PAD // 4)
    cs = slice(PAD, PAD + N)
    mix = {k: float(v[cs, cs].mean()) for k, v in pf.weights.items()}
    top = max(mix, key=mix.get)
    rows.append((site["id"], site["geomorphic_class"], top,
                 f"T {cl['temperature']:.1f}C P {cl['precipitation']:.0f}mm", mix))

print(f"{'site':<28} {'corpus class':<20} {'PROVINCE':<9} {'climate':<20} mix")
print("-" * 118)
for sid, klass, top, note, mix in rows:
    ms = "  ".join(f"{k[:4]} {v:.2f}" for k, v in sorted(mix.items())) if mix else ""
    print(f"{sid:<28} {klass:<20} {top:<9} {note:<20} {ms}")
