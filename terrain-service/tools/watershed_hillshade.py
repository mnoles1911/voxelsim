#!/usr/bin/env python3
"""Render a MOSAIC of fine tiles at fine pitch: hillshade + water depth.

    python tools/watershed_hillshade.py \
        --tiles "-5,-4 -4,-4 -3,-4 -5,-5 -4,-5 -3,-5" \
        --out D:/tmp/wet-block.png

    python tools/watershed_hillshade.py --bbox "-5,-5,-3,-4" --lake-outlines \
        --out D:/tmp/wet-block.png

WHY THIS EXISTS -- AND WHY NOTHING ELSE WOULD DO.

The water work is judged by LOOKING at it, and until this file there was no
renderer that could show more than one fine tile at a time:

  * everything under tools/worldmaps/ (heightmap.py, water.py, ...) stitches
    the COARSE plane at DS=4 x 30 m = 120 m/px. A river channel on this world
    has a p50 wetted width of about 2.65 m, i.e. 0.022 of one coarse pixel.
    Averaged into that pixel it moves the value by ~2% of the depth and then
    the colour ramp rounds it away. A 120 m/px map of this world is not a
    slightly blurry picture of the rivers; it is a picture with no rivers in
    it at all. Every "the water looks fine" and every "I see no water" read
    off those maps was reading the resampler, not the bake.
  * tools/hillshade_ab.py is the only renderer at fine pitch (FINE_CELL_M =
    1.875 m), and it is a single-tile A/B contact sheet -- crops of the same
    place from different bakes, laid side by side. It cannot answer "does this
    river leave the tile and keep going", which is the question a drainage
    network is judged on. Its hillshade maths and its .vxtl decode path are
    reused here verbatim rather than re-derived, so a mosaic and a contact
    sheet of the same ground shade identically.

RESOLUTION, AND THE ONE PLACE MEAN-DOWNSAMPLING IS A LIE.

A fine tile is 8192 px at 1.875 m = 15.36 km. The wet alpine block (3 x 2
tiles) is 46.08 x 30.72 km, which is 24576 x 16384 fine pixels -- 400 M cells,
not a picture anyone opens. So it is downsampled by an integer power of two,
and the two planes are downsampled by DIFFERENT reductions on purpose:

  ELEVATION: block MEAN. Relief is a smooth field; averaging it is what any
    map projection does anyway, and the mean is what keeps the hillshade from
    aliasing (hillshade_ab.py's docstring makes the opposite point about
    stripes, and it is the same point: resampling can invent or erase
    structure at the pixel scale, so choose the reduction that matches what
    the layer means).

  WATER DEPTH: block MAX, with alpha = "any cell wet". This is the whole
    reason this tool is trustworthy. At the default 7.5 m/px a 2.65 m channel
    occupies about 1.4 of the 16 fine cells inside one output pixel. A MEAN
    would report 1.4/16 = 9% of the true depth, the ramp would render that as
    nearly dry, and a perfectly good river would disappear -- the exact
    failure mode the coarse maps already have, reintroduced one level down.
    MAX keeps a one-pixel-wide river one pixel wide and at its real depth. The
    cost is honest and stated: a thin river is drawn WIDER than it is (one
    output pixel, 7.5 m, versus 2.65 m of real water). Judge CONNECTIVITY and
    PLACEMENT from this image; do not judge width from it.

Default --px-budget is 6144, not the ~4000 that was first asked for, because
those two are inconsistent over this block: 4000 px across 46.08 km is 11.5
m/px, and the useful pitch here is 7.5 m/px. 46.08 km / 7.5 m = 6144 px, which
is also exactly downsample factor 4 on an 8192 tile -- no resampling remainder
at the tile seam. --px-budget 4096 (factor 8, 15 m/px) is a fine choice for a
quick look; --px-budget 30000 gives native 1.875 m/px if you have the RAM.

MEMORY STRATEGY, AND THE MEASURED PEAK.

One decoded fine tile is 8192^2 int16 = 128 MiB for elevation and another 128
MiB for water. Six tiles decoded at once is 1.5 GiB before a single float is
formed, and the obvious next step -- float32 for the hillshade -- would double
it again. So nothing is ever held at full resolution across tiles:

  1. one tile is decoded at a time, and its file bytes are dropped as soon as
     decode_v2 returns;
  2. it is reduced into its slot in the OUTPUT-resolution mosaic in row bands
     of BAND_ROWS lines, so the float32 working copy is BAND_ROWS x 8192 x 4 B
     = 32 MiB rather than 256 MiB;
  3. the tile is deleted before the next one is opened;
  5. the shade and the RGB composite are ALSO banded, in float32, because the
     first version of this tool did them whole and that -- not the tile decode
     -- was where the memory went (see `composite`).

Peak resident set on the six-tile wet block, MEASURED, not estimated (Windows
peak working set, printed at the end of every run): 1.10 GB, in 465 s wall.
Roughly 0.2 GB of that is the output-resolution mosaic (6144 x 4096 x 4 B x 2
float planes, plus two bool masks) and scales with --px-budget; the remaining
~0.9 GB is ONE decoded tile -- elevation 128 MiB + water 128 MiB + flow 64 MiB
of planes, plus the compressed file bytes and decode_v2's own working set --
and is constant no matter how many tiles you ask for. Comfortably under the
2 GB ceiling; the earlier unbanded composite was 2.10 GB and was not.

THE FAILURE MODE THIS TOOL REFUSES TO RENDER SILENTLY.

A tile baked before bake_ver 9 carries no water plane at all. It decodes
perfectly and every pixel of it is dry -- indistinguishable, in a picture,
from a tile that was surveyed and found to have no rivers. That confusion has
cost this project real time. So every tile's bake_ver, water-plane presence
and wet fraction go to stderr; a tile with NO water plane gets a banner, is
hatched in the image instead of being drawn dry, and makes the process exit 2.
A missing tile file is likewise left blank (and marked) rather than aborting
the mosaic -- five good tiles and one hole is a useful picture, and a crash is
not.
"""

from __future__ import annotations

import argparse
import ctypes
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from terrain_service import tile_codec as tc  # noqa: E402

# The hillshade maths is imported, not copied: a mosaic that shaded differently
# from tools/hillshade_ab.py would make the two renderers non-comparable, and
# the contact sheet is what direction artifacts are judged on.
from hillshade_ab import FINE_CELL_M, hillshade  # noqa: E402

#: Rows of a tile converted to float at once. 1024 x 8192 x 4 B = 32 MiB, and
#: it must be a multiple of the downsample factor so a band reduces to a whole
#: number of output rows.
BAND_ROWS = 1024

#: Where the wet-alpine bake lives on this box. All three are overridable; they
#: are defaults so that the common invocation is just --tiles and --out.
DEFAULT_TILE_DIR = "D:/vox-wet-cache"
DEFAULT_PROVIDER = "terrain-diffusion-unlabeled-80b9ca451a23eae4-b10cf6d2c"
DEFAULT_SEED = "000000000135276f"

#: Basin kinds that hold standing water worth outlining. DRY_PLAYA and
#: SALT_FLAT are holes with no water in them; drawing a "lake" box around one
#: would be the same category error as rendering a water-less tile dry.
WET_BASIN_KINDS = (tc.BASIN_KIND_SEASONAL,
                   tc.BASIN_KIND_LAKE_TERMINAL,
                   tc.BASIN_KIND_LAKE_OVERFLOWING)
KIND_NAME = {0: "dry-playa", 1: "salt-flat", 2: "seasonal",
             3: "lake-terminal", 4: "lake-overflowing"}


# ---------------------------------------------------------------------------
# Loading. One tile, decoded once, reported honestly.
# ---------------------------------------------------------------------------


def load_tile(path: Path):
    """decode_v2 with the zstd fallback tools/aspect_lock.py established.

    decode_v2 consults an injected decompressor first and only falls back to
    the module-level `zstandard` import; on a box where that import failed but
    the package is importable from this process the first call raises, so the
    explicit decompressor is tried second rather than being assumed.
    """
    data = path.read_bytes()
    try:
        return tc.decode_v2(data)
    except RuntimeError:
        import zstandard as zstd

        dctx = zstd.ZstdDecompressor()
        return tc.decode_v2(
            data, decompressor=lambda b, n: dctx.decompress(b, max_output_size=n)
        )


def reduce_tile(tile, f: int, elev_slot: np.ndarray, water_slot: np.ndarray) -> dict:
    """Reduce one decoded tile into its output-resolution slots, in row bands.

    `elev_slot` and `water_slot` are VIEWS into the mosaic, so this writes
    straight into the final array and no per-tile output buffer exists either.

    Elevation is the CONTROL LATTICE scaled to metres, exactly as
    hillshade_ab.py/aspect_lock.py use it -- not the evaluated spline surface.
    The prefilter is separable and direction-symmetric, so at 7.5 m/px the
    difference between lattice and surface is far below one shading step, and
    evaluating a spline over 400 M cells to learn nothing is not worth the
    hour. Water is the DEPTH plane in metres above its own bed (10 mm LSB,
    -1 = dry), which needs no ground at all: the depth IS the quantity being
    coloured, so `water_surface_mm_from_depth` and its "never add depth to
    amplified ground" trap never come up here.
    """
    size = tile.size
    q_mm = tc.QUANT_MM[tile.quant]
    base_m = tile.base_offset_mm / 1000.0
    scale_m = q_mm / 1000.0
    n_out = size // f

    ecp = np.asarray(tile.elevation_cp)
    wcp = None if tile.water_cp is None else np.asarray(tile.water_cp)

    wet_cells = 0
    dmax = 0.0
    for r0 in range(0, size, BAND_ROWS):
        r1 = min(r0 + BAND_ROWS, size)
        rows = r1 - r0

        band = ecp[r0:r1].astype(np.float32)
        band *= scale_m
        band += base_m
        elev_slot[r0 // f : r1 // f, :] = band.reshape(
            rows // f, f, n_out, f).mean(axis=(1, 3))
        del band

        if wcp is not None:
            # 10 mm LSB, -1 == dry. Stay in the integer domain for the MAX so
            # "dry" survives the reduction as a single sentinel; only the
            # survivors become metres.
            wb = wcp[r0:r1].reshape(rows // f, f, n_out, f).max(axis=(1, 3))
            wet = wb >= 0
            out = np.where(wet, wb.astype(np.float32) * (tc.WATER_DEPTH_LSB_MM / 1000.0),
                           np.float32(np.nan))
            water_slot[r0 // f : r1 // f, :] = out
            wet_cells += int((wcp[r0:r1] >= 0).sum())
            if wet.any():
                dmax = max(dmax, float(out[wet].max()))
            del wb, wet, out

    return {"wet_cells": wet_cells,
            "wet_frac": wet_cells / float(size * size),
            "max_depth_m": dmax}


# ---------------------------------------------------------------------------
# Compositing.
# ---------------------------------------------------------------------------


def depth_ramp(depth_m: np.ndarray, lo_m: float, hi_m: float) -> np.ndarray:
    """Water depth -> RGB, float 0..1, on a LOG scale between lo_m and hi_m.

    Log, because depth on this world spans three decades: a 0.05 m headwater
    rill and a 20 m lake floor are both things the eye needs to find, and a
    linear ramp spends its whole dynamic range on the lake and paints every
    river the same near-black. The stops go pale cyan (shallow, high contrast
    against grey hillshade) -> mid blue -> deep navy.
    """
    t = np.log10(np.clip(depth_m, lo_m, hi_m) / lo_m) / np.log10(hi_m / lo_m)
    stops = np.array([
        [0.66, 0.93, 1.00],   # 0.0  shallow: pale cyan
        [0.30, 0.70, 0.95],
        [0.11, 0.42, 0.82],
        [0.04, 0.16, 0.50],   # 1.0  deep: navy
    ], dtype=np.float32)
    pos = np.linspace(0.0, 1.0, len(stops), dtype=np.float32)
    out = np.empty(t.shape + (3,), dtype=np.float32)
    for c in range(3):
        out[..., c] = np.interp(t, pos, stops[:, c]).astype(np.float32)
    return out


def hatch(shape: tuple[int, int], period: int = 12, row0: int = 0) -> np.ndarray:
    """Diagonal hatch mask -- the visual vocabulary for "NOT DATA".

    Flat colour would be read as terrain (that is precisely how a water-less
    tile passes for a dry one). A hatch is not a thing the bake can produce, so
    it cannot be mistaken for one.

    `row0` is the band's first row in MOSAIC coordinates. Without it the
    diagonals restart at every band boundary and the hatch grows a horizontal
    seam every BAND_ROWS lines -- a picture artifact that looks like a data
    artifact, which is the one thing a judging artifact may never do. Built by
    broadcasting rather than np.indices: np.indices would materialise two int64
    arrays the size of the band for a pattern that needs neither.
    """
    h, w = shape
    y = np.arange(row0, row0 + h, dtype=np.int32)[:, None]
    x = np.arange(w, dtype=np.int32)[None, :]
    return ((x + y) % period) < (period // 3)


def composite(elev: np.ndarray, water: np.ndarray, present: np.ndarray,
              nowater: np.ndarray, cell_m: float, a) -> np.ndarray:
    """Mosaic planes -> uint8 RGB. All layers are toggleable; order is fixed.

    BANDED, AND FOR A MEASURED REASON. The first version of this function did
    the whole mosaic at once and the run peaked at 2.10 GB -- over budget, and
    none of it in the tile decode the budget was written for. It was here:
    `hillshade` forms about six intermediates (two gradients, slope, aspect,
    the shade itself), and at 6144 x 4096 in float64 that is ~200 MB each, plus
    a float32 RGB working image at 302 MB. Banding the shade in float32 turns
    all of that into a few tens of MB and drops the measured peak to 1.10 GB,
    which is now the tile decode again -- where it belongs.

    The band carries a ONE-ROW HALO because np.gradient uses central
    differences: with the halo, every row this band keeps is bit-identical to
    what the unbanded version produced, so the shading has no seam at a band
    boundary. Only the mosaic's own first and last rows use one-sided
    differences, exactly as before.
    """
    h, w = elev.shape
    out = np.empty((h, w, 3), dtype=np.uint8)

    fill = np.float32(0.0)
    if not a.no_hillshade:
        fin = np.isfinite(elev)
        # Holes are filled with the mosaic median before shading: np.gradient
        # propagates NaN across a 3-cell neighbourhood, which would eat a
        # one-pixel black border around every hole. A flat fill has zero
        # gradient, so it shades to the ambient value and is masked out below.
        fill = np.float32(np.median(elev[fin])) if fin.any() else np.float32(0.0)
        del fin

    rows = max(BAND_ROWS, 1)
    for r0 in range(0, h, rows):
        r1 = min(r0 + rows, h)
        g0, g1 = max(0, r0 - 1), min(h, r1 + 1)          # halo
        sl = slice(r0 - g0, r0 - g0 + (r1 - r0))

        if a.no_hillshade:
            rgb = np.full((r1 - r0, w, 3), 0.55, dtype=np.float32)
        else:
            z = elev[g0:g1]
            holes = ~np.isfinite(z)
            if holes.any():
                z = np.where(holes, fill, z)
            hs = hillshade(z, cell_m, a.az, a.alt, a.zf)[sl]
            # Lift the black point: pure black shadow hides water, and water is
            # what this picture is for.
            rgb = np.repeat((0.18 + 0.82 * hs).astype(np.float32)[..., None], 3, axis=2)
            del z, holes, hs

        if not a.no_water:
            wb = water[r0:r1]
            wet = np.isfinite(wb)
            if wet.any():
                rgb[wet] = depth_ramp(wb[wet], a.depth_lo, a.depth_hi)
            del wb, wet

        # Not-data last, so it overwrites anything drawn under it.
        miss = ~present[r0:r1]
        if miss.any():
            hm = miss & hatch((r1 - r0, w), row0=r0)
            rgb[miss] = np.array([0.10, 0.09, 0.12], dtype=np.float32)
            rgb[hm] = np.array([0.24, 0.20, 0.28], dtype=np.float32)
        nw = nowater[r0:r1]
        if nw.any():
            # Water-plane-absent tiles keep their hillshade (the ground is real
            # and worth looking at) but carry an unmistakable warm hatch saying
            # that the ABSENCE of water here is not a measurement.
            hm = nw & hatch((r1 - r0, w), period=24, row0=r0)
            rgb[hm] = np.array([0.85, 0.42, 0.12], dtype=np.float32)

        np.clip(rgb, 0.0, 1.0, out=rgb)
        rgb *= 255.0
        out[r0:r1] = rgb.astype(np.uint8)
        del rgb

    return out


# ---------------------------------------------------------------------------
# Annotation. A judging artifact has to say what it is a picture OF.
# ---------------------------------------------------------------------------


def annotate(img: Image.Image, tiles, x0: int, y0: int, n: int, cell_m: float,
             a, notes: list[str]) -> None:
    d = ImageDraw.Draw(img, "RGBA")
    for (tx, ty) in tiles:
        px, py = (tx - x0) * n, (ty - y0) * n
        d.rectangle([px, py, px + n - 1, py + n - 1], outline=(255, 255, 255, 60))
        d.text((px + 6, py + 6), f"{tx},{ty}", fill=(255, 255, 255, 190))

    # Scale bar: the only way to check by eye that the pitch is what the header
    # claims. A 1-2-5 step near a tenth of the image width, so a sub-kilometre
    # mosaic gets a bar that fits inside it instead of one rounded up to 1 km
    # and drawn off the edge.
    target_km = img.width * cell_m / 1000.0 / 10.0
    km = min((1, 2, 5, 10, 20, 50, 100, 200, 500),
             key=lambda v: abs(np.log(v / max(target_km, 1e-6))))
    if km > target_km * 3:  # sub-kilometre image: label in metres instead
        km = target_km
    barpx = int(km * 1000.0 / cell_m)
    bx, by = 14, img.height - 34
    d.rectangle([bx - 6, by - 18, bx + barpx + 6, by + 12], fill=(0, 0, 0, 150))
    d.rectangle([bx, by, bx + barpx, by + 5], fill=(255, 255, 255, 230))
    lab = f"{km:g} km" if km >= 1 else f"{km * 1000:.0f} m"
    d.text((bx, by - 16), f"{lab}   {cell_m:.2f} m/px", fill=(255, 255, 255, 230))

    # Depth legend, drawn from the same ramp function the image uses so it
    # cannot drift out of step with it.
    lw, lh = 220, 12
    lx, ly = img.width - lw - 16, img.height - 34
    ramp = depth_ramp(
        a.depth_lo * (a.depth_hi / a.depth_lo) ** np.linspace(0, 1, lw, dtype=np.float32),
        a.depth_lo, a.depth_hi)
    d.rectangle([lx - 6, ly - 18, lx + lw + 6, ly + lh + 4], fill=(0, 0, 0, 150))
    img.paste(Image.fromarray((ramp[None, :, :] * 255).astype(np.uint8)
                              ).resize((lw, lh), Image.NEAREST), (lx, ly))
    d.text((lx, ly - 16), f"water depth  {a.depth_lo:g} m", fill=(255, 255, 255, 230))
    d.text((lx + lw - 40, ly - 16), f"{a.depth_hi:g} m", fill=(255, 255, 255, 230))

    y = 6
    for line in notes:
        d.rectangle([0, y - 4, 8 + 7 * len(line), y + 12], fill=(0, 0, 0, 140))
        d.text((6, y), line, fill=(255, 230, 120, 255))
        y += 16


def draw_basins(img: Image.Image, basin_boxes) -> int:
    """Outline registered wet basins.

    THE TABLE HAS NO SHORELINE IN IT. SECTION_BASIN_TABLE carries a bbox, a
    seed cell, an outlet and a surface level -- the client derives the actual
    lake outline by flood-filling to `surface_mm`, which needs the evaluated
    spline surface at full resolution and is therefore exactly the work this
    tool refuses to do. So what is drawn is the bbox: the bound the fill can
    never exceed. Read it as "a lake is registered somewhere in this box", not
    as a shoreline. The water layer underneath it is the real extent.
    """
    d = ImageDraw.Draw(img, "RGBA")
    for (x0, y0, x1, y1, kind, surf_m) in basin_boxes:
        col = (255, 220, 70, 220) if kind != tc.BASIN_KIND_SEASONAL else (255, 150, 60, 200)
        d.rectangle([x0, y0, x1, y1], outline=col)
        if (x1 - x0) > 40:
            d.text((x0 + 3, y0 + 3), f"{KIND_NAME.get(kind, kind)} {surf_m:.0f}m", fill=col)
    return len(basin_boxes)


# ---------------------------------------------------------------------------


def peak_rss_gb() -> float | None:
    """Windows peak working set, so a run can report what it actually cost.

    psutil is not installed on this box and this tool is not worth a dependency
    for one number, so it goes straight to psapi. Returns None off Windows.
    """
    if not sys.platform.startswith("win"):
        return None
    try:
        class PMC(ctypes.Structure):
            _fields_ = [("cb", ctypes.c_uint32), ("PageFaultCount", ctypes.c_uint32),
                        ("PeakWorkingSetSize", ctypes.c_size_t),
                        ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t),
                        ("PeakPagefileUsage", ctypes.c_size_t)]

        pmc = PMC()
        pmc.cb = ctypes.sizeof(PMC)
        # The argtypes/restype are NOT optional. Without them ctypes treats a
        # HANDLE as c_int, GetCurrentProcess's pseudo-handle is truncated on
        # win64, and the call fails silently -- which is how this returned None
        # on its first run and reported no memory figure at all.
        kernel32 = ctypes.windll.kernel32
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        fn = ctypes.windll.psapi.GetProcessMemoryInfo
        fn.argtypes = [ctypes.c_void_p, ctypes.POINTER(PMC), ctypes.c_uint32]
        fn.restype = ctypes.c_int
        ok = fn(kernel32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
        return pmc.PeakWorkingSetSize / 1e9 if ok else None
    except Exception:
        return None


def parse_tiles(a, ap) -> list[tuple[int, int]]:
    if a.bbox:
        v = [int(t) for t in a.bbox.replace(",", " ").split()]
        if len(v) != 4:
            ap.error("--bbox wants x0,y0,x1,y1")
        x0, y0, x1, y1 = v
        return [(x, y) for y in range(min(y0, y1), max(y0, y1) + 1)
                for x in range(min(x0, x1), max(x0, x1) + 1)]
    if not a.tiles:
        ap.error("give --tiles or --bbox")
    out = []
    for p in a.tiles.replace(";", " ").split():
        xs, _, ys = p.partition(",")
        out.append((int(xs), int(ys)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tiles", default=None,
                    help='space-separated tile coords, e.g. "-5,-4 -4,-4 -3,-4"')
    ap.add_argument("--bbox", default=None, help='inclusive "x0,y0,x1,y1"')
    ap.add_argument("--tile-dir", default=DEFAULT_TILE_DIR,
                    help="cache root holding <provider-id>/<seed>/s16")
    ap.add_argument("--provider-id", default=DEFAULT_PROVIDER)
    ap.add_argument("--seed", default=DEFAULT_SEED,
                    help="seed directory name (16 hex digits), or a decimal seed")
    ap.add_argument("--out", required=True)
    ap.add_argument("--px-budget", type=int, default=6144,
                    help="target long edge in px (default 6144 = 7.5 m/px over "
                         "a 3-tile-wide block); the factor is rounded UP to a "
                         "power of two so tiles land on whole output pixels")
    ap.add_argument("--no-hillshade", action="store_true")
    ap.add_argument("--no-water", action="store_true")
    ap.add_argument("--lake-outlines", action="store_true",
                    help="outline registered wet basins from SECTION_BASIN_TABLE")
    ap.add_argument("--min-basin-px", type=int, default=2,
                    help="skip basins whose bbox is smaller than this at OUTPUT "
                         "resolution. The wet block registers 552 wet basins "
                         "over six tiles and most are alpine tarns a few fine "
                         "cells across; at 7.5 m/px their boxes degenerate to "
                         "yellow specks that read as water rather than as "
                         "annotation. 0 draws all of them.")
    ap.add_argument("--no-annotate", action="store_true")
    ap.add_argument("--depth-lo", type=float, default=0.05, help="ramp floor, m")
    ap.add_argument("--depth-hi", type=float, default=20.0, help="ramp ceiling, m")
    # Sun angles default to hillshade_ab.py's, and for its reason: 292.5 is
    # halfway between two D8 lock directions, so a 45-degree routing artifact is
    # neither flattered nor exaggerated by the lighting.
    ap.add_argument("--az", type=float, default=292.5)
    ap.add_argument("--alt", type=float, default=25.0)
    ap.add_argument("--zf", type=float, default=1.0)
    a = ap.parse_args()

    t_start = time.time()
    tiles = parse_tiles(a, ap)
    seed_dir = a.seed if not a.seed.isdigit() or len(a.seed) == 16 else f"{int(a.seed):016x}"
    root = Path(a.tile_dir) / a.provider_id / seed_dir / "s16"

    xs = [t[0] for t in tiles]
    ys = [t[1] for t in tiles]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    nx, ny = x1 - x0 + 1, y1 - y0 + 1

    # Probe one tile for `size` rather than assuming 8192: the factor arithmetic
    # and every buffer below depend on it, and a wrong guess would silently
    # produce a mosaic with the tiles in the wrong places.
    size = None
    for (tx, ty) in tiles:
        p = root / f"{tx}_{ty}.vxtl"
        if p.exists():
            size = _peek_size(p)
            break
    if size is None:
        print(f"none of the {len(tiles)} requested tiles exist under {root}",
              file=sys.stderr)
        return 1

    f = 1
    while (max(nx, ny) * size) // f > a.px_budget and f < size // 16:
        f *= 2
    n = size // f
    W, H = nx * n, ny * n
    cell_m = FINE_CELL_M * f

    print(f"# root       {root}", file=sys.stderr)
    print(f"# tiles      {nx}x{ny} = {len(tiles)} requested, {size} px each "
          f"({size * FINE_CELL_M / 1000:.2f} km)", file=sys.stderr)
    print(f"# mosaic     {W}x{H} px, downsample x{f}, {cell_m:.3f} m/px, "
          f"{W * cell_m / 1000:.2f} x {H * cell_m / 1000:.2f} km", file=sys.stderr)
    print(f"# buffers    {2 * W * H * 4 / 1e9:.3f} GB of float32 mosaic",
          file=sys.stderr)

    elev = np.full((H, W), np.nan, dtype=np.float32)
    water = np.full((H, W), np.nan, dtype=np.float32)
    present = np.zeros((H, W), dtype=bool)
    nowater = np.zeros((H, W), dtype=bool)

    basin_boxes: list[tuple] = []
    n_missing = n_nowater = n_basin_small = 0
    total_wet = 0
    total_cells = 0

    for (tx, ty) in sorted(tiles, key=lambda t: (t[1], t[0])):
        sy, sx = (ty - y0) * n, (tx - x0) * n
        p = root / f"{tx}_{ty}.vxtl"
        if not p.exists():
            n_missing += 1
            print(f"({tx:3d},{ty:3d})  MISSING FILE {p.name} -- slot left blank",
                  file=sys.stderr)
            continue

        t_tile = time.time()
        tile = load_tile(p)
        if tile.size != size:
            print(f"({tx:3d},{ty:3d})  size {tile.size} != {size}; skipped",
                  file=sys.stderr)
            del tile
            n_missing += 1
            continue

        st = reduce_tile(tile, f, elev[sy:sy + n, sx:sx + n], water[sy:sy + n, sx:sx + n])
        present[sy:sy + n, sx:sx + n] = True
        has_water = tile.water_cp is not None
        nb = n_small = 0
        if a.lake_outlines and tile.basins:
            for b in tile.basins:
                if b.kind not in WET_BASIN_KINDS:
                    continue
                bx0, by0, bx1, by1 = b.bbox_px
                ox0, oy0 = sx + bx0 // f, sy + by0 // f
                ox1, oy1 = sx + bx1 // f, sy + by1 // f
                if max(ox1 - ox0, oy1 - oy0) < a.min_basin_px:
                    n_small += 1
                    continue
                basin_boxes.append((ox0, oy0, ox1, oy1, b.kind,
                                    b.surface_mm / 1000.0))
                nb += 1
            n_basin_small += n_small

        if has_water:
            total_wet += st["wet_cells"]
            total_cells += size * size
            print(f"({tx:3d},{ty:3d})  bake_ver {tile.bake_ver:2d}  water YES  "
                  f"wet {100.0 * st['wet_frac']:6.3f}%  "
                  f"({st['wet_cells']:,} cells)  max depth {st['max_depth_m']:6.2f} m  "
                  f"basins {len(tile.basins) if tile.basins else 0}"
                  f"{f' ({nb} wet outlined)' if a.lake_outlines else ''}  "
                  f"[{time.time() - t_tile:.1f}s]", file=sys.stderr)
        else:
            n_nowater += 1
            nowater[sy:sy + n, sx:sx + n] = True
            print(f"({tx:3d},{ty:3d})  bake_ver {tile.bake_ver:2d}  "
                  f"*** NO WATER PLANE *** FLAG_WATER_PRESENT is clear, so this "
                  f"tile carries no water data at all. It is NOT dry -- it is "
                  f"UNSURVEYED. Hatched orange in the image; do not read the "
                  f"absence of rivers here as a result. Re-bake at bake_ver "
                  f">= 9.  [{time.time() - t_tile:.1f}s]", file=sys.stderr)
        del tile

    img = Image.fromarray(composite(elev, water, present, nowater, cell_m, a), "RGB")
    del elev, water, present, nowater

    notes = []
    if n_nowater:
        notes.append(f"WARNING: {n_nowater} tile(s) have NO WATER PLANE "
                     f"(orange hatch) -- unsurveyed, not dry")
    if n_missing:
        notes.append(f"{n_missing} tile(s) missing from the cache (grey hatch)")
    if not a.no_annotate:
        annotate(img, tiles, x0, y0, n, cell_m, a, notes)
    if a.lake_outlines and basin_boxes:
        draw_basins(img, basin_boxes)
        print(f"# basins     {len(basin_boxes)} wet basins outlined (bbox, not "
              f"shoreline); {n_basin_small} smaller than --min-basin-px="
              f"{a.min_basin_px} skipped", file=sys.stderr)

    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    img.save(a.out)

    dt = time.time() - t_start
    rss = peak_rss_gb()
    if total_cells:
        print(f"# wet        {100.0 * total_wet / total_cells:.3f}% of "
              f"{total_cells:,} fine cells over {total_cells // (size * size)} "
              f"surveyed tile(s)", file=sys.stderr)
    print(f"# wrote      {a.out}  {img.width}x{img.height} @ {cell_m:.3f} m/px",
          file=sys.stderr)
    print(f"# cost       {dt:.1f}s wall"
          + (f", peak RSS {rss:.2f} GB" if rss else ""), file=sys.stderr)
    if n_nowater or n_missing:
        print(f"# EXIT 2: {n_nowater} tile(s) without a water plane, "
              f"{n_missing} missing. The PNG was still written -- read the "
              f"hatched areas as 'no data', never as 'no water'.", file=sys.stderr)
        return 2
    return 0


def _peek_size(path: Path) -> int:
    """Tile edge in px from the 25-byte v1-identical header, without decoding.

    Reading 25 bytes to learn the geometry avoids decoding a tile just to size
    the mosaic -- which would put a second 128 MiB plane alongside the first
    one at exactly the moment the buffers are being allocated.
    """
    with path.open("rb") as fh:
        head = fh.read(tc._HEADER.size)
    magic, version, seed, x, y, scale, size = tc._HEADER.unpack_from(head, 0)
    if magic != tc.MAGIC:
        raise ValueError(f"{path.name}: bad magic")
    return int(size)


if __name__ == "__main__":
    raise SystemExit(main())
