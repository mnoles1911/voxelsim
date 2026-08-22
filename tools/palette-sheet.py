#!/usr/bin/env python3
"""Render the palette as a picture, so it can be judged instead of read.

WHY THIS EXISTS
---------------
The colour system is 47 rows of five numbers, and the question anyone actually
has about it -- "does this look like the game we want" -- cannot be answered by
reading them. Until now the only way to see the palette was to run the editor,
which one person at a time can do, on one box.

So this draws it. Every material, at its authored variation, through the SAME
evaluation the game uses (vxc::voxelTint, compiled and called out of a probe
here rather than reimplemented -- reimplementing it is how the parity checker
started, and it checked itself for a while).

WHAT EACH PANEL SHOWS, and why each one is on the sheet:

  NEAR      a block of voxel faces at 1 cube per 12 px, top face class. This is
            the cube-to-cube dither -- the thing the reference art is built on,
            and the thing terrain did not have at all until ADR-0009.
  FAR       the same material at 1 cube per pixel. The jitter averages back to
            its own mean here and what is left is the patch term. If a material
            looks identical in both panels its far-field half is doing nothing;
            if FAR is flat grey, the patch term is missing or too weak.
  FACES     top / side / bottom as three bands, so a material whose face split
            earns its keep (grass green over soil, bark over cut heartwood) can
            be checked at a glance against one that does not need it.
  CLIMATE   for terrain surfaces only: the material blended toward two climate
            colours at its authored biomeTint, so how much identity survives the
            blend is visible rather than inferred from a number.

The sheet is deliberately UNLIT -- no ambient occlusion, no sun. Those are the
renderer's and they would make every panel look better than the palette is
(ADR-0008 invariant 4). What is on this sheet is albedo.

Run:
    python3 tools/palette-sheet.py -o /tmp/palette.png
    python3 tools/palette-sheet.py --only GRASS,ROCK,BARK -o /tmp/some.png

Needs numpy and Pillow, plus a C++ compiler for the evaluation.
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "voxel-core" / "include" / "voxelcore" / "materialpalette.h"
INCLUDE = ROOT / "voxel-core" / "include"

ENTRY = re.compile(
    r"/\*\s*(MAT_\w+)\s*\*/\s*"
    r"\{\{\{([^}]*)\},\s*\{([^}]*)\},\s*\{([^}]*)\}\},\s*"
    r"(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}",
    re.S)

# Two climate answers to blend surface materials toward, as sRGB. Not sampled
# from T_VoxelBiomeLUT -- that is a .uasset and this tool refuses to need the
# editor -- but picked to bracket the range the LUT spans: a cold dry steppe and
# a wet tropical green. What the panel is for is how much MATERIAL survives the
# blend, and that is set by biomeTint rather than by which two colours are used.
CLIMATE_A = (150, 142, 108)   # cold, dry
CLIMATE_B = (58, 104, 44)     # warm, wet

PROBE = r'''
// Evaluates vxc::voxelTint over the grid tools/palette-sheet.py asks for.
#include "voxelcore/materialcolor.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    const int w = std::atoi(argv[1]);
    const int h = std::atoi(argv[2]);
    const int cubeMm = std::atoi(argv[3]);
    for (int m = 0; m < vxc::kMaterialCount; ++m) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                // A fixed z, and x/y offset per material so no two materials
                // show the same patch of noise -- side by side, an identical
                // pattern reads as a rendering artefact rather than as two
                // materials.
                const vxc::VoxelTint t = vxc::voxelTint(
                    vxc::MaterialId(m), x + m * 71, y + m * 37, 17, cubeMm);
                std::printf("%d %d %d\n", m, t.lightQ16, t.hueQ16);
            }
        }
    }
    return 0;
}
'''


def read_rows():
    rows = []
    for m in ENTRY.finditer(HEADER.read_text(encoding="utf-8")):
        faces = tuple(tuple(int(v) for v in m.group(i).replace(" ", "").split(","))
                      for i in (2, 3, 4))
        rows.append({
            "name": m.group(1), "faces": faces,
            "jitter": int(m.group(5)), "hue": int(m.group(6)),
            "patch": int(m.group(7)), "scale_dm": int(m.group(8)),
            "biome": int(m.group(9)),
        })
    if not rows:
        raise SystemExit(f"no palette rows parsed from {HEADER}")
    return rows


def evaluate(width, height, cube_mm, count):
    """(count, height, width, 2) of (light, hue), from vxc::voxelTint itself."""
    import numpy as np

    cxx = shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        raise SystemExit("no g++ or clang++ on PATH -- the sheet is drawn with the "
                         "engine's own evaluation, not a copy of it")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "probe.cpp"
        src.write_text(PROBE)
        exe = Path(tmp) / "probe"
        build = subprocess.run([cxx, "-std=c++20", "-O2", "-I", str(INCLUDE),
                                str(src), "-o", str(exe)], capture_output=True, text=True)
        if build.returncode != 0:
            raise SystemExit("the sheet's probe did not compile:\n" + build.stderr)
        run = subprocess.run([str(exe), str(width), str(height), str(cube_mm)],
                             capture_output=True, text=True)
    vals = np.fromstring(run.stdout.replace("\n", " "), sep=" ", dtype=np.int64)
    vals = vals.reshape(-1, 3)
    if vals.shape[0] != count * width * height:
        raise SystemExit(f"probe returned {vals.shape[0]} rows, expected "
                         f"{count * width * height}")
    return vals[:, 1:].reshape(count, height, width, 2) / 65536.0


def srgb_to_linear(a):
    import numpy as np
    a = np.asarray(a, dtype=np.float64) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(a):
    import numpy as np
    a = np.clip(a, 0.0, 1.0)
    return np.where(a <= 0.0031308, a * 12.92, 1.055 * a ** (1 / 2.4) - 0.055)


def tinted(base_srgb, tint, climate_srgb=None, biome=0):
    """One panel: base, optionally blended toward a climate, then varied.

    IN LINEAR, and in the composition's own order (materialcolor.h): the climate
    blend is stage 2 and the variation is stage 3, because applying the variation
    first would let the blend average it away -- which is the whole reason the
    renderer carries the two separately.
    """
    import numpy as np
    lin = srgb_to_linear(base_srgb)
    if climate_srgb is not None and biome:
        lin = lin + (srgb_to_linear(climate_srgb) - lin) * (biome / 255.0)
    out = np.empty(tint.shape[:2] + (3,), dtype=np.float64)
    light, hue = tint[..., 0], tint[..., 1]
    out[..., 0] = lin[0] * (1.0 + light) * (1.0 + hue)
    out[..., 1] = lin[1] * (1.0 + light)
    out[..., 2] = lin[2] * (1.0 + light) * (1.0 - hue)
    return (linear_to_srgb(np.maximum(out, 0.0)) * 255.0).astype("uint8")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--out", default="palette-sheet.png")
    ap.add_argument("--only", default="",
                    help="comma-separated name fragments, e.g. GRASS,ROCK")
    ap.add_argument("--cell", type=int, default=12,
                    help="pixels per voxel in the NEAR panel (default 12)")
    a = ap.parse_args()

    try:
        import numpy as np
        from PIL import Image, ImageDraw
    except ImportError as exc:
        raise SystemExit(f"{exc}: this needs numpy and Pillow "
                         "(pip install numpy pillow)")

    rows = read_rows()
    wanted = [w.strip().upper() for w in a.only.split(",") if w.strip()]
    shown = [i for i, r in enumerate(rows)
             if not wanted or any(w in r["name"] for w in wanted)]
    if not shown:
        raise SystemExit(f"nothing matched --only {a.only!r}")

    NEAR_VOX = 14
    cell = a.cell
    near_px = NEAR_VOX * cell
    band_px = near_px // 3

    # FAR IS DOWNSAMPLED, NOT DRAWN SMALLER, and the distinction is the whole
    # panel. What happens at range is that many voxels fall inside one pixel and
    # the jitter AVERAGES BACK TO ITS OWN MEAN -- which is the argument ADR-0008
    # invariant 3 rests on, and the reason the slow patch term is not optional.
    # Point-sampling a fine grid into a small panel would show the jitter intact
    # at a smaller size and prove nothing. So the field is evaluated at
    # FAR_SAMPLES per panel pixel and box-filtered down.
    FAR_SAMPLES = 8
    far_vox = NEAR_VOX * FAR_SAMPLES

    near = evaluate(NEAR_VOX, NEAR_VOX, 100, len(rows))
    far_fine = evaluate(far_vox, far_vox, 100, len(rows))
    far = far_fine.reshape(len(rows), NEAR_VOX, FAR_SAMPLES,
                           NEAR_VOX, FAR_SAMPLES, 2).mean(axis=(2, 4))
    # Each face class gets its OWN two-dimensional block of variation. The first
    # version of this sheet evaluated three ROWS and repeated each one down its
    # band, which drew vertical streaks -- an artefact of the sheet that looked
    # like an artefact of the palette.
    faces = [evaluate(NEAR_VOX, NEAR_VOX, 100, len(rows)) for _ in range(3)]
    clim = evaluate(NEAR_VOX, NEAR_VOX, 100, len(rows))

    LABEL_W, PAD, ROW_H = 190, 10, near_px
    panels = 5  # near, far, faces, climate A, climate B
    width = LABEL_W + panels * (near_px + PAD) + PAD
    height = PAD + len(shown) * (ROW_H + PAD) + 46
    img = Image.new("RGB", (width, height), (18, 19, 22))
    draw = ImageDraw.Draw(img)

    draw.text((PAD, 8),
              "voxel palette  --  albedo only, no lighting or AO (ADR-0008 "
              "invariant 4).  columns: NEAR one cube per cell  |  FAR the same "
              "field box-filtered 8x8, i.e. the jitter averaged out and only the "
              "patch term left  |  FACES top/side/bottom  |  CLIMATE cold-dry and "
              "warm-wet at each material's biomeTint",
              fill=(150, 155, 165))

    def paste(arr, x, y, w, h):
        im = Image.fromarray(arr, "RGB").resize((w, h), Image.NEAREST)
        img.paste(im, (x, y))

    y = 30 + PAD
    for i in shown:
        r = rows[i]
        top, side, bottom = r["faces"]
        draw.text((PAD, y + 2), r["name"][4:], fill=(225, 228, 233))
        draw.text((PAD, y + 18),
                  f"jit {r['jitter']}  hue {r['hue']}", fill=(130, 135, 145))
        draw.text((PAD, y + 32),
                  f"patch {r['patch']}/{r['scale_dm']}dm", fill=(130, 135, 145))
        draw.text((PAD, y + 46),
                  f"biome {r['biome']}", fill=(130, 135, 145))

        x = LABEL_W
        paste(tinted(top, near[i]), x, y, near_px, near_px)
        x += near_px + PAD
        paste(tinted(top, far[i]), x, y, near_px, near_px)
        x += near_px + PAD
        # Three bands, one per face class. A band that looks the same as its
        # neighbours is a material whose face split is not earning its keep --
        # true of most of them, and deliberately so.
        for b, face in enumerate((top, side, bottom)):
            band_rows = max(1, NEAR_VOX // 3)
            paste(tinted(face, faces[b][i][:band_rows]),
                  x, y + b * band_px, near_px, band_px)
        x += near_px + PAD
        for climate in (CLIMATE_A, CLIMATE_B):
            paste(tinted(top, clim[i], climate, r["biome"]), x, y, near_px, near_px)
            x += near_px + PAD
        y += ROW_H + PAD

    img.save(a.out)
    print(f"wrote {a.out}  ({len(shown)} materials, {img.size[0]}x{img.size[1]})")
    print("The two CLIMATE columns are identical to NEAR for every material with "
          "biomeTint 0 -- that is the point of the column, not a bug.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
