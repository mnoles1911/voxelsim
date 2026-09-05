"""Generate the Asset Forge desktop icon: a voxel-stylized forge.

Owner ruling (2026-09-05): the desktop entry is named "Asset Forge" with "a
cool forge icon stylized with voxels". No external art assets -- the icon is
drawn here, deterministically, from a 16x16 voxel map, so it is reproducible
from the repo alone:

    python tools/make_icon.py            # writes tools/asset-forge.ico
    python tools/make_icon.py --preview out.png   # eyeball render at 256 px

The glyph is an anvil of steel voxels under a piece of white-hot stock, on a
dark rounded plate with a warm forge glow behind the hot metal. Every filled
cell is drawn as a chunky cube -- lit top face, shaded right face -- which is
what makes it read as VOXELS rather than flat pixel art at 48 px and up,
while at 16 px the cells collapse to single pixels and it stays legible.

Each icon size is rendered on its own from the same map (nearest-multiple
cell size, centred), never downsampled from one big render, so the 16 px and
32 px images stay crisp.
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
OUT = HERE / "asset-forge.ico"

# 16x16 voxel map.  legend:
#   .  transparent (dark plate shows through)
#   A  anvil steel        h  hot metal (orange)
#   w  white-hot core     e  ember spark (deep orange)
#   y  yellow spark
GRID = [
    "................",
    "......y.........",
    "...e.......e....",
    "..........y.....",
    "......ww........",
    ".....whhw.......",
    ".....hhhh.......",
    ".AAAAAAAAAAAAA..",
    "..AAAAAAAAAAA...",
    "......AAAA......",
    "......AAAA......",
    ".....AAAAAA.....",
    "...AAAAAAAAAA...",
    "..AAAAAAAAAAAA..",
    "................",
    "................",
]
N = 16

# base colour, lit top-face colour, shaded right-face colour
VOXEL = {
    "A": ((125, 132, 148), (168, 176, 194), (84, 90, 104)),
    "h": ((255, 138, 40), (255, 176, 84), (214, 96, 22)),
    "w": ((255, 236, 170), (255, 250, 220), (255, 190, 90)),
    "e": ((255, 94, 42), (255, 128, 70), (200, 60, 24)),
    "y": ((255, 210, 74), (255, 236, 140), (230, 168, 40)),
}

PLATE = (20, 22, 30, 255)          # near-black blue ground
PLATE_EDGE = (46, 50, 64, 255)
GLOW = (255, 140, 50)              # forge glow, alpha-faded radially
GLOW_CENTER = (7.5, 5.5)           # cell coords of the hot stock


def render(size: int) -> Image.Image:
    cell = max(1, size // N)
    pad = (size - cell * N) // 2
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # dark plate with a slightly rounded corner and a hairline edge
    r = max(1, size // 8)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=r, fill=PLATE,
                        outline=PLATE_EDGE, width=max(1, size // 32))

    # warm radial glow behind the hot metal, drawn per-pixel-band on an
    # overlay so it stays deterministic and dependency-free
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    cx = pad + GLOW_CENTER[0] * cell
    cy = pad + GLOW_CENTER[1] * cell
    rmax = size * 0.42
    steps = 24
    for i in range(steps, 0, -1):
        rad = rmax * i / steps
        alpha = int(70 * (1 - i / steps) ** 1.5)
        if alpha <= 0:
            continue
        gd.ellipse([cx - rad, cy - rad * 0.85, cx + rad, cy + rad * 0.85],
                   fill=GLOW + (alpha,))
    img = Image.alpha_composite(img, glow)
    d = ImageDraw.Draw(img)

    # the voxels: base square, lit top face, shaded right face
    bevel = max(0, cell // 4)
    for gy, row in enumerate(GRID):
        for gx, ch in enumerate(row):
            if ch not in VOXEL:
                continue
            base, top, side = VOXEL[ch]
            x0 = pad + gx * cell
            y0 = pad + gy * cell
            x1 = x0 + cell - 1
            y1 = y0 + cell - 1
            d.rectangle([x0, y0, x1, y1], fill=base + (255,))
            if bevel:
                above = gy > 0 and GRID[gy - 1][gx] == ch
                right = gx < N - 1 and row[gx + 1] == ch
                if not above:   # lit top face only on exposed tops
                    d.rectangle([x0, y0, x1, y0 + bevel - 1], fill=top + (255,))
                if not right:   # shaded right face only on exposed rights
                    d.rectangle([x1 - bevel + 1, y0, x1, y1], fill=side + (255,))
    return img


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preview", metavar="PNG",
                    help="also write a 256 px PNG for eyeballing")
    ap.add_argument("--out", default=str(OUT))
    args = ap.parse_args()

    sizes = [16, 32, 48, 64, 128, 256]
    frames = [render(s) for s in sizes]
    out = Path(args.out)
    # each size rendered on its own map (crisp at 16/32), not downsampled
    frames[-1].save(out, format="ICO",
                    append_images=frames[:-1],
                    sizes=[(s, s) for s in sizes])
    print(f"wrote {out}  ({', '.join(f'{s}px' for s in sizes)})")
    if args.preview:
        frames[-1].save(args.preview, format="PNG")
        print(f"wrote {args.preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
