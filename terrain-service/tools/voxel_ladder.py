#!/usr/bin/env python3
"""Where is the banding CREATED? Hillshade the same ground before and after
voxelisation, and at each LOD step, under identical light.

WHY. As of 2026-07-31 the evidence says the artifact is not in the height data:
the baked surface at 1.875 m/px reads as clean terrain to the owner, and the
client's continuous surface at 10 cm measures smooth (band_period.py: tread
follows 0.1/slope at ratio 1.25, i.e. only 25% more irregular than a perfectly
smooth ramp). Yet the rendered game shows hard parallel terraces. If both height
fields are clean, the artifact is created at or after VOXELISATION -- by the
lattice, by LOD decimation, or by the mesher -- and no generator term can be
responsible for it.

This renders the ladder so the step that introduces it can be seen directly:

    continuous            the surface as generated, no lattice
    voxel 10 cm           L0: floor to the shipped voxel pitch
    LOD 1 / 2 / 3         20 / 40 / 80 cm, the effective pitch at distance

Decimation matters as much as pitch. `--lod-mode` picks how a LOD cell is
chosen from its 2x2 children: `corner` (point-sample, what naive mip chains do),
`max` (what a conservative surface bound does), or `mean`. Point-sampling and
max-filtering produce visibly different terracing from averaging, and which one
the renderer actually does is the question this is meant to help answer -- so it
is a flag, not a constant.

Sun azimuth defaults to 292.5, halfway between two 45-degree directions, so that
a grid-locked artifact is neither flattered nor hidden.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

VOXEL_MM = 100.0


def hillshade(z: np.ndarray, cell_m: float, az_deg: float, alt_deg: float) -> np.ndarray:
    gy, gx = np.gradient(z, cell_m)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(gy, -gx)
    az = np.radians(360.0 - az_deg + 90.0)
    alt = np.radians(alt_deg)
    hs = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
    return np.clip(hs, 0.0, 1.0)


def decimate(z: np.ndarray, f: int, mode: str) -> np.ndarray:
    """Reduce by an integer factor, then hold each value over its block.

    The result stays on the ORIGINAL grid so every panel is the same size and
    the same ground -- the coarsening is what changes, not the framing.
    """
    if f == 1:
        return z
    n = (z.shape[0] // f) * f
    b = z[:n, :n].reshape(n // f, f, n // f, f)
    if mode == "corner":
        small = b[:, 0, :, 0]
    elif mode == "max":
        small = b.max(axis=(1, 3))
    else:
        small = b.mean(axis=(1, 3))
    return np.repeat(np.repeat(small, f, axis=0), f, axis=1)


def quantise(z_m: np.ndarray, pitch_mm: float) -> np.ndarray:
    return np.floor(z_m * 1000.0 / pitch_mm) * pitch_mm / 1000.0


def panel(z: np.ndarray, cell_m: float, text: str, az: float, alt: float) -> Image.Image:
    hs = hillshade(z, cell_m, az, alt)
    img = Image.fromarray((hs * 255).astype(np.uint8), mode="L")
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, img.width, 20], fill=0)
    d.text((5, 5), text, fill=255)
    return img


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="VXC_PROBE_DUMP int32 mm raster")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cell-m", type=float, default=0.1)
    ap.add_argument("--az", type=float, default=292.5)
    ap.add_argument("--alt", type=float, default=25.0)
    ap.add_argument("--lod-mode", default="corner", choices=("corner", "max", "mean"))
    a = ap.parse_args()

    raw = np.fromfile(a.bin, dtype="<i4")
    n = int(round(len(raw) ** 0.5))
    z = raw[: n * n].reshape(n, n).astype(np.float64) / 1000.0
    print(f"{a.bin}: {n}x{n} @ {a.cell_m} m = {n * a.cell_m:.1f} m, "
          f"relief {z.max() - z.min():.1f} m, lod-mode {a.lod_mode}")

    panels = [panel(z, a.cell_m, f"continuous ({n * a.cell_m:.0f} m across)", a.az, a.alt)]
    # L0: the shipped voxel pitch, no decimation.
    panels.append(panel(quantise(z, VOXEL_MM), a.cell_m, "voxel 10 cm (L0)", a.az, a.alt))
    # LOD: coarsen horizontally AND quantise to that level's pitch, which is what
    # a coarsened voxel actually is -- a bigger cube, not just a blurrier height.
    for lvl in (1, 2, 3):
        f = 1 << lvl
        pitch = VOXEL_MM * f
        zl = quantise(decimate(z, f, a.lod_mode), pitch)
        panels.append(panel(zl, a.cell_m, f"LOD {lvl}: {pitch / 10:.0f} cm voxel", a.az, a.alt))

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
