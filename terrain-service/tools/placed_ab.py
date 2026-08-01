#!/usr/bin/env python3
"""Hillshade PLACED VOXELS -- the top solid voxel face -- not the height field.

WHY THIS EXISTS, AND WHY IT IS NOT band_period.py WITH A DIFFERENT INPUT.
Every other terrain instrument in this tree consumes VXC_PROBE_DUMP, which is
`amp.surfaceMm` -- the continuous height field. Since kWorldGenVersion 12 the
client does not render that: `stratigraphyAt` tests `centre <= surfaceMm + D`
with D = D(x,y,z) from density3.h, |D| <= 350 mm = 3.5 voxels. The solid set is
displaced OFF the surface the other tools measure, so an artifact living in D is
structurally invisible to all of them -- which on 2026-07-31 cost a full day of
measurements that came back clean while the game stayed banded.

The input here is VXC_PROBE_DUMP_VOXEL (terrainprobe.cpp), which scans the band
and writes the top solid voxel's TOP FACE in mm, plus a `.nod3` sibling computed
with `col.d3` zeroed. Zeroing the column is an exact ablation of the term --
density3ColumnDisplacementMm returns 0 identically when gateQ == 0 -- so the
difference between the two panels is the 3D density band and nothing else.

The values are already multiples of 100 mm, so nothing here quantises; that
would be quantising twice.

Sun azimuth defaults to 292.5, halfway between two 45-degree directions, so a
grid-locked or strike-locked artifact is neither flattered nor hidden.
Rendered at NATIVE resolution: the bands are a few pixels and any resampling
can alias them away entirely.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def hillshade(z: np.ndarray, cell_m: float, az_deg: float, alt_deg: float) -> np.ndarray:
    gy, gx = np.gradient(z, cell_m)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(gy, -gx)
    az = np.radians(360.0 - az_deg + 90.0)
    alt = np.radians(alt_deg)
    hs = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
    return np.clip(hs, 0.0, 1.0)


def load(path: Path) -> np.ndarray:
    raw = np.fromfile(path, dtype="<i4")
    n = int(round(len(raw) ** 0.5))
    return raw[: n * n].reshape(n, n).astype(np.float64) / 1000.0


def panel(z: np.ndarray, cell_m: float, text: str, az: float, alt: float) -> Image.Image:
    img = Image.fromarray((hillshade(z, cell_m, az, alt) * 255).astype(np.uint8), mode="L")
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, img.width, 20], fill=0)
    d.text((5, 5), text, fill=255)
    return img


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="VXC_PROBE_DUMP_VOXEL output (.nod3 sibling implied)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--surface", help="optional VXC_PROBE_DUMP height field, for a third panel")
    ap.add_argument("--cell-m", type=float, default=0.1)
    ap.add_argument("--az", type=float, default=292.5)
    ap.add_argument("--alt", type=float, default=25.0)
    a = ap.parse_args()

    with_d3 = load(Path(a.bin))
    no_d3 = load(Path(a.bin + ".nod3"))
    n = with_d3.shape[0]
    span = n * a.cell_m
    d = with_d3 - no_d3
    moved = int((d != 0).sum())
    print(f"{n}x{n} @ {a.cell_m} m = {span:.1f} m across")
    print(f"  placed top voxel moved on {moved}/{n*n} columns ({100.0*moved/(n*n):.2f}%)")
    if moved:
        print(f"  displacement: min {d.min()*1000:.0f} mm  max {d.max()*1000:.0f} mm  "
              f"mean|d| {np.abs(d[d != 0]).mean()*1000:.0f} mm")
    print(f"  relief with d3 {with_d3.max()-with_d3.min():.1f} m, "
          f"without {no_d3.max()-no_d3.min():.1f} m")

    panels = []
    if a.surface:
        panels.append(panel(load(Path(a.surface)), a.cell_m,
                            "HEIGHT FIELD (what every other tool measures)", a.az, a.alt))
    panels.append(panel(no_d3, a.cell_m, f"PLACED VOXELS, 3D band OFF ({span:.0f} m)", a.az, a.alt))
    panels.append(panel(with_d3, a.cell_m, "PLACED VOXELS, 3D band ON (what ships)", a.az, a.alt))

    w = sum(p.width for p in panels) + 6 * (len(panels) - 1)
    sheet = Image.new("L", (w, panels[0].height), 40)
    x = 0
    for p in panels:
        sheet.paste(p, (x, 0))
        x += p.width + 6
    sheet.save(a.out)
    print(f"wrote {a.out} ({sheet.width}x{sheet.height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
