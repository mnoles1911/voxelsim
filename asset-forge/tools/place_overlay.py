"""Map overlays of asset-placement anchors over real topography.

The owner's instrument: "plot the positions of trees/rocks by asset type on a
map of a region" -- so "why is this slope bare" and "what stands in the lake"
are answerable by eye, per build, from the ENGINE-EQUIVALENT binding.

Inputs are what `vxc_assetprobe --overlay <base>` writes (the probe samples the
ground -- fine-tile elevation, the composed lake/river/sea datum placement
gates on, the baked shore-distance plane, the treeline -- and resolves the
instances through the same channel binding the engine composes with; this tool
only DRAWS. Never rebuild ground in Python):

    <base>.instances.csv   one row per placed instance, classed and audited
    <base>.ground.bin      VXOV raster: elevation / water datum / dist-to-water
                           / treeline, int32 planes at the fine tiles' 1.875 m

Outputs (into --out):

    <name>.map.png             hillshaded elevation + contours + water mask +
                               treeline line + one dot per anchor, colored by
                               class, riparian ringed
    <name>.density-<class>.png per-class anchor density heatmap over the same
                               hillshade

    python tools/place_overlay.py --base ../bake-out/overlay-alpine \
        --out ../bake-out/overlays --name alpine-lake
"""
import argparse
import csv
import struct
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

# Class = how the dots read, not the manifest's kind enum verbatim: the owner
# looks for "canopy vs small trees climbing the steeps vs shrubs vs rocks vs
# ground cover vs the riparian ring". Riparian is a FLAG on top of the class
# (a willow is both a canopy tree and riparian) and is drawn as a ring so both
# facts survive on one dot.
CLASS_COLORS = {
    "canopy":       (20, 110, 40),
    "small-tree":   (90, 200, 90),
    "shrub":        (170, 140, 40),
    "rock":         (105, 105, 115),
    "reed":         (0, 190, 190),
    "ground-cover": (200, 210, 120),
    "other":        (230, 120, 220),
}
RIPARIAN_RING = (255, 40, 200)
CLASS_DOT_R = {  # radius in image px at scale 2 (1 px ~ 0.94 m)
    "canopy": 3, "small-tree": 2, "shrub": 2, "rock": 2,
    "reed": 1, "ground-cover": 1, "other": 2,
}
# Draw order: dense carpet classes first so the sparse, tall classes stay
# visible on top of them.
DRAW_ORDER = ["ground-cover", "reed", "rock", "shrub", "small-tree", "canopy", "other"]

SMALL_TREE_MAX_MM = 8000  # a "small tree" is a tree under 8 m


def classify(kind: str, height_mm: int) -> str:
    if kind == "tree":
        return "small-tree" if height_mm < SMALL_TREE_MAX_MM else "canopy"
    if kind == "bush":
        return "shrub"
    if kind == "rock":
        return "rock"
    if kind == "reed":
        return "reed"
    if kind in ("grass", "flower"):
        return "ground-cover"
    return "other"


def read_ground(path: Path):
    raw = path.read_bytes()
    magic, ver = raw[:4], struct.unpack_from("<I", raw, 4)[0]
    if magic != b"VXOV" or ver != 1:
        raise SystemExit(f"{path}: not a VXOV v1 raster")
    x0, y0 = struct.unpack_from("<qq", raw, 8)
    step, w, h, planes = struct.unpack_from("<iIII", raw, 24)
    if planes != 4:
        raise SystemExit(f"{path}: expected 4 planes, got {planes}")
    data = np.frombuffer(raw, dtype="<i4", offset=36, count=4 * w * h)
    data = data.reshape(4, h, w)
    return {
        "x0_mm": x0, "y0_mm": y0, "step_mm": step, "w": w, "h": h,
        "elev": data[0].astype(np.float64),
        "water": data[1],
        "dist": data[2],
        "tree": data[3],
    }


K_NO_WATER = -2147483648  # kNoWaterMm / kNoWaterMarkerMm (INT32_MIN)
K_NO_TREE = -2147483648


def hillshade(elev_m: np.ndarray, step_m: float, az_deg=315.0, alt_deg=45.0):
    gy, gx = np.gradient(elev_m, step_m)
    az, alt = np.radians(az_deg), np.radians(alt_deg)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gx, gy)
    hs = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
    return np.clip(hs, 0.0, 1.0)


def build_base(g, scale: int) -> Image.Image:
    step_m = g["step_mm"] / 1000.0
    elev_m = g["elev"] / 1000.0
    hs = hillshade(elev_m, step_m)
    # Elevation tint under the hillshade so height reads as tone, slope as
    # texture -- both halves of "why is this slope bare".
    lo, hi = np.percentile(elev_m, 2), np.percentile(elev_m, 98)
    t = np.clip((elev_m - lo) / max(hi - lo, 1e-6), 0, 1)
    r = (60 + 150 * t) * (0.35 + 0.65 * hs)
    gch = (70 + 120 * t) * (0.35 + 0.65 * hs)
    b = (55 + 90 * t) * (0.35 + 0.65 * hs)
    img = np.stack([r, gch, b], axis=-1)

    # Contours every 25 m, faint.
    band = np.floor(elev_m / 25.0)
    edge = np.zeros_like(band, dtype=bool)
    edge[1:, :] |= band[1:, :] != band[:-1, :]
    edge[:, 1:] |= band[:, 1:] != band[:, :-1]
    img[edge] = img[edge] * 0.72

    # The water mask, from the SAME wet union placement gates on: the composed
    # lake/river datum (+ implicit sea) where it stands above ground.
    water = g["water"]
    wet = (water != K_NO_WATER) & (water > g["elev"])
    depth_m = np.where(wet, (water - g["elev"]) / 1000.0, 0.0)
    dt = np.clip(depth_m / 6.0, 0.15, 1.0)
    img[wet] = np.stack([30 * (1 - dt[wet]) + 15, 90 * (1 - dt[wet]) + 40, 170 * (1 - dt[wet]) + 85],
                        axis=-1)

    # The treeline, as a line: where ground crosses the temperature-adjusted
    # treeline the placement band reads.
    tl = g["tree"]
    known = tl != K_NO_TREE
    above = known & (g["elev"] > tl)
    tedge = np.zeros_like(above, dtype=bool)
    tedge[1:, :] |= above[1:, :] != above[:-1, :]
    tedge[:, 1:] |= above[:, 1:] != above[:, :-1]
    tedge &= known
    img[tedge] = (200, 40, 40)

    img8 = np.clip(img, 0, 255).astype(np.uint8)
    # World y grows up; image row 0 is the top.
    img8 = img8[::-1, :, :]
    im = Image.fromarray(img8, "RGB")
    return im.resize((g["w"] * scale, g["h"] * scale), Image.NEAREST)


def world_to_px(g, scale, x_mm, y_mm):
    fx = (x_mm - g["x0_mm"]) / g["step_mm"]
    fy = (y_mm - g["y0_mm"]) / g["step_mm"]
    return fx * scale, (g["h"] - 1 - fy) * scale


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="probe --overlay basename (reads .instances.csv + .ground.bin)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default=None, help="output name stem (default: basename of --base)")
    ap.add_argument("--scale", type=int, default=2, help="image px per raster cell (default 2)")
    ap.add_argument("--classes", default=None, help="comma list to draw (default: all)")
    args = ap.parse_args()

    base = Path(args.base)
    name = args.name or base.name
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    g = read_ground(Path(str(base) + ".ground.bin"))
    scale = args.scale

    rows = []
    with open(str(base) + ".instances.csv", newline="") as f:
        for r in csv.DictReader(f):
            r["cls"] = classify(r["kind"], int(r["height_mm"]))
            rows.append(r)
    wanted = set(args.classes.split(",")) if args.classes else set(CLASS_COLORS)

    per_class = defaultdict(list)
    for r in rows:
        if r["cls"] in wanted:
            per_class[r["cls"]].append(r)

    # --- the map -----------------------------------------------------------
    im = build_base(g, scale)
    dr = ImageDraw.Draw(im)
    n_riparian = 0
    for cls in DRAW_ORDER:
        col = CLASS_COLORS[cls]
        rad = CLASS_DOT_R[cls]
        for r in per_class.get(cls, ()):
            x, y = world_to_px(g, scale, int(r["x_mm"]), int(r["y_mm"]))
            if int(r["riparian"]):
                n_riparian += 1
                dr.ellipse([x - rad - 1, y - rad - 1, x + rad + 1, y + rad + 1],
                           outline=RIPARIAN_RING)
            dr.ellipse([x - rad, y - rad, x + rad, y + rad], fill=col)

    # Legend + counts: the map must carry its own reading conditions.
    pad, lh = 8, 14
    lines = [f"{name}  ({len(rows)} instances, {n_riparian} riparian; "
             f"grid {g['w']}x{g['h']} @ {g['step_mm']/1000.0:.3f} m)"]
    for cls in DRAW_ORDER:
        if cls in per_class:
            lines.append(f"{cls}: {len(per_class[cls])}")
    lines.append("ring = riparian (water-gated)   red line = treeline   blue = water datum")
    box_h = pad * 2 + lh * len(lines)
    dr.rectangle([0, 0, 460, box_h], fill=(0, 0, 0))
    for i, ln in enumerate(lines):
        color = (255, 255, 255)
        if i > 0 and ":" in ln and ln.split(":")[0] in CLASS_COLORS:
            color = CLASS_COLORS[ln.split(":")[0]]
        dr.text((pad, pad + lh * i), ln, fill=color)
    map_path = out_dir / f"{name}.map.png"
    im.save(map_path)
    print(f"wrote {map_path}")

    # --- per-class density heatmaps ---------------------------------------
    bin_m = 8.0
    step_m = g["step_mm"] / 1000.0
    bw = max(1, int(round(g["w"] * step_m / bin_m)))
    bh = max(1, int(round(g["h"] * step_m / bin_m)))
    base_gray = build_base(g, scale).convert("L").convert("RGB")
    for cls, items in sorted(per_class.items()):
        hist = np.zeros((bh, bw), dtype=np.float64)
        for r in items:
            bx = int((int(r["x_mm"]) - g["x0_mm"]) / 1000.0 / bin_m)
            by = int((int(r["y_mm"]) - g["y0_mm"]) / 1000.0 / bin_m)
            if 0 <= bx < bw and 0 <= by < bh:
                hist[by, bx] += 1
        peak = hist.max()
        if peak <= 0:
            continue
        t = (hist / peak) ** 0.5  # sqrt: keep the low end visible
        heat = np.zeros((bh, bw, 4), dtype=np.uint8)
        col = CLASS_COLORS[cls]
        heat[..., 0], heat[..., 1], heat[..., 2] = col
        heat[..., 3] = (t * 235).astype(np.uint8)
        heat = heat[::-1, :, :]
        overlay = Image.fromarray(heat, "RGBA").resize(base_gray.size, Image.BILINEAR)
        out = base_gray.copy()
        out.paste(overlay, (0, 0), overlay)
        d2 = ImageDraw.Draw(out)
        d2.rectangle([0, 0, 460, pad * 2 + lh], fill=(0, 0, 0))
        d2.text((pad, pad), f"{name}  {cls} density ({len(items)} anchors, peak "
                            f"{int(peak)}/{int(bin_m)}m cell)", fill=col)
        p = out_dir / f"{name}.density-{cls}.png"
        out.save(p)
        print(f"wrote {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
