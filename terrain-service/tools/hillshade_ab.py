#!/usr/bin/env python3
"""Hillshade a baked fine tile, for looking at direction artifacts directly.

WHY A HILLSHADE AND NOT AN IN-ENGINE CAPTURE. A capture shows the bake through
the client's detail terms, voxelisation and LOD, any of which can mask or mimic
the thing being tested. This renders the baked surface itself, so what you see
is what the bake produced and nothing else.

WHY THE SUN AZIMUTH IS 292.5 AND NOT THE CONVENTIONAL 315. The artifact under
test is a lock to 8 compass directions, i.e. multiples of 45 degrees, and the
cartographic default of 315 is itself a multiple of 45 -- it would light one
diagonal family head-on and put the other in shadow, which either flatters or
exaggerates the very thing being measured. 292.5 is exactly halfway between two
lock directions, so no member of the family is privileged.

Rendered at NATIVE resolution with no downsampling: the stripes have a
wavelength of a few pixels, and any resampling on the way to the image can
alias them away entirely (or invent them).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from aspect_lock import load_vxtl  # noqa: E402

FINE_CELL_M = 1.875


def hillshade(z: np.ndarray, cell_m: float, az_deg: float, alt_deg: float,
              z_factor: float = 1.0) -> np.ndarray:
    """Standard Horn hillshade, 0..1."""
    gy, gx = np.gradient(z * z_factor, cell_m)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(gy, -gx)
    az = np.radians(360.0 - az_deg + 90.0)
    alt = np.radians(alt_deg)
    hs = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
    return np.clip(hs, 0.0, 1.0)


def crop(z: np.ndarray, n: int, ox: int, oy: int) -> np.ndarray:
    h = z.shape[0]
    cy = h // 2 + oy
    cx = h // 2 + ox
    y0 = max(0, min(h - n, cy - n // 2))
    x0 = max(0, min(h - n, cx - n // 2))
    return z[y0 : y0 + n, x0 : x0 + n]


def label(img: Image.Image, text: str) -> Image.Image:
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, img.width, 22], fill=0)
    d.text((6, 6), text, fill=255)
    return img


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pairs", nargs="+", help="label=path.vxtl")
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=1024, help="crop edge in px (native)")
    ap.add_argument("--ox", type=int, default=0)
    ap.add_argument("--oy", type=int, default=0)
    ap.add_argument("--az", type=float, default=292.5)
    ap.add_argument("--alt", type=float, default=25.0)
    ap.add_argument("--zf", type=float, default=1.0)
    a = ap.parse_args()

    tiles = []
    for p in a.pairs:
        name, _, path = p.partition("=")
        z, cell = load_vxtl(Path(path))
        c = crop(z, a.n, a.ox, a.oy)
        hs = hillshade(c, cell, a.az, a.alt, a.zf)
        img = Image.fromarray((hs * 255).astype(np.uint8), mode="L")
        span_km = a.n * cell / 1000.0
        tiles.append(label(img, f"{name}   {span_km:.2f} km   sun az {a.az:g} alt {a.alt:g}"))
        print(f"{name}: crop {a.n}x{a.n} @ {cell} m/px = {span_km:.2f} km, "
              f"relief {c.max() - c.min():.1f} m")

    w = sum(t.width for t in tiles) + 8 * (len(tiles) - 1)
    sheet = Image.new("L", (w, tiles[0].height), 40)
    x = 0
    for t in tiles:
        sheet.paste(t, (x, 0))
        x += t.width + 8
    sheet.save(a.out)
    print(f"wrote {a.out}  ({sheet.width}x{sheet.height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
