#!/usr/bin/env python
"""Re-encodes the 1920px menu backdrops to web weight for embedding.

READS THE GAME'S OWN FILES IN PLACE. The backdrops already live in the repo at
ue-project/Content/UI/Backgrounds; keeping a second copy under this package
would be 2.1 MB of duplicated binaries free to diverge from what the game
actually ships -- the same drift the token generator exists to prevent.
Provenance and rights for these files:
ue-project/Content/UI/Backgrounds/MENU_ART_CREDITS.md.

These encodes exist only because the design system delivers the art as data
URIs inside the JS bundle, where 2.1 MB of full-size JPEG would be paid by every
design built with it. 1024px at q72 is chosen against how the art is actually
seen: every screen puts it behind a 55-62% black wash, which suppresses exactly
the detail a higher quality would buy.
"""
import glob
import os

from PIL import Image

SRC = "../ue-project/Content/UI/Backgrounds"
OUT = "assets/backgrounds/web"
WIDTH = 1024
QUALITY = 72

if not os.path.isdir(SRC):
    raise SystemExit(
        "[BG] source art not found: " + SRC + os.linesep
        + "[BG] this package must live beside ue-project/ in the voxelsim checkout."
    )

os.makedirs(OUT, exist_ok=True)
total = 0
for path in sorted(glob.glob(os.path.join(SRC, "*.jpg"))):
    im = Image.open(path)
    w, h = im.size
    nh = round(h * WIDTH / w)
    im = im.convert("RGB").resize((WIDTH, nh), Image.LANCZOS)
    out = os.path.join(OUT, os.path.basename(path))
    im.save(out, "JPEG", quality=QUALITY, optimize=True, progressive=True)
    size = os.path.getsize(out)
    total += size
    print(f"{os.path.basename(path):24s} {w}x{h} -> {WIDTH}x{nh}  {size / 1024:6.1f} KB")
print(f"{'TOTAL':24s} {total / 1024:.1f} KB")
