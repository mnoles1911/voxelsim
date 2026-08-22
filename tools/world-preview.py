#!/usr/bin/env python3
"""Draw the world's ground colour, top-down, without the editor.

WHY THIS EXISTS
---------------
ADR-0009 makes a voxel face's colour its material, which replaces a continuous
climate gradient with a handful of per-material colours. Whether that is better
or worse than what ships today is a question about a LANDSCAPE, and nothing in
this repo could answer it without the editor box -- which one person can hold at
a time, and which the material graph has to be regenerated on before it shows
anything at all.

So this draws it. Real terrain from the amplifier, real surface materials from
the classifier, the real palette, the real `vxc::voxelTint`, and a hillshade
from the real gradient. Every number in the picture comes from the engine; the
only thing Python does is arrange pixels.

`--compare` is the reason to build it: today's biome-LUT colours beside the
material colours, over the same ground. Both sides are computable here -- the
LUT from `gen_terrain_textures.write_biome_lut`'s own arithmetic, the material
side from `materialpalette.h` -- so the A/B that decides the art direction can
be looked at now instead of after a session on the box.

WHAT IT IS NOT. It is not a render. There is no sun, no sky, no ambient
occlusion, no atmosphere, no water surface, and no vegetation -- and vegetation
is 828 species of placed cover that carries a great deal of the variety in the
finished world. Read it as "what colour is the ground", which is the question
ADR-0009 changes, and not as "what will the game look like".

AND THE LUT PANEL IS THE LUT, not the whole clipmap material. M_VoxelClipmap
also applies slope-rock, beach and snow over the LUT sample, and the sea is
covered by the water surface. So the vista panel will show ocean floor painted
as whatever the climate there says -- taiga soil, usually -- where the real
vista has water over it. What the panel is for is whether the two paths agree
about LAND, which is where their seam actually is.

INPUT is the binary dump from `vxc_matcensus --dump`, which carries per column:
surface elevation, the signed per-axis gradient, the surface material, the
biome, and the climate bytes. The header is text and self-describing, so this
tool reads the geometry out of the file rather than being told it twice.

    ./build/voxel-core/bench/vxc_matcensus <tiledir|--synthetic> <seed> \\
        --radius 40 --dump /tmp/world.bin --dump-radius 3000 --dump-stride 50
    python3 tools/world-preview.py /tmp/world.bin --compare -o /tmp/world.png

Needs numpy and Pillow, plus a C++ compiler (the tint comes from the engine).
"""
import argparse
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "voxel-core" / "include" / "voxelcore" / "materialpalette.h"
INCLUDE = ROOT / "voxel-core" / "include"
UE_TOOLS = ROOT / "ue-project" / "Tools"

ENTRY = re.compile(
    r"/\*\s*(MAT_\w+)\s*\*/\s*"
    r"\{\{\{([^}]*)\},\s*\{([^}]*)\},\s*\{([^}]*)\}\},\s*"
    r"(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}",
    re.S)

RECORD = struct.Struct("<iiiBBBB")  # surfaceMm, gradX, gradY, mat, biome, temp, precip

# The per-cell tint has to come from vxc::voxelTint, not from a copy of it here.
# tools/check-palette-parity.py exists because a copy drifts, and its first
# version was a copy that passed two of three deliberate breakages.
TINT_PROBE = r'''
#include "voxelcore/materialcolor.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
// stdin: n, then n triples of (mat, vx, vy). stdout: n pairs of Q16 tint.
int main() {
    long n = 0;
    if (std::scanf("%ld", &n) != 1) return 1;
    for (long i = 0; i < n; ++i) {
        int m = 0; long x = 0, y = 0;
        if (std::scanf("%d %ld %ld", &m, &x, &y) != 3) return 1;
        const vxc::VoxelTint t = vxc::voxelTint(
            vxc::MaterialId(m), int32_t(x), int32_t(y), 0, vxc::kVoxelSizeMm);
        std::printf("%d %d\n", t.lightQ16, t.hueQ16);
    }
    return 0;
}
'''


def read_palette():
    rows = {}
    for m in ENTRY.finditer(HEADER.read_text(encoding="utf-8")):
        rows[len(rows)] = {
            "name": m.group(1),
            "top": tuple(int(v) for v in m.group(2).replace(" ", "").split(",")),
        }
    if not rows:
        raise SystemExit(f"no palette rows parsed from {HEADER}")
    return rows


def read_dump(path):
    """Header lines, then the binary body. Returns (meta, numpy record arrays).

    Read through the file rather than into memory. A dump is 16 bytes per
    column and the radius/stride that produce it are two independent flags, so
    it is easy to ask for one much larger than it looks: --dump-radius 3000
    --dump-stride 4 is a 15000x15000 grid and 3.6 GB. The first version of this
    function slurped the file and then sliced it, i.e. twice the file in RAM,
    and that dump got the process OOM-killed -- which surfaced as a zero exit
    status and no image, the failure mode this repo keeps paying for. np.fromfile
    with an offset holds one copy and nothing more.
    """
    import numpy as np

    # The header is short and ASCII; find its terminator without reading the body.
    CHUNK = 4096
    head_bytes = b""
    with open(path, "rb") as f:
        while b"# END\n" not in head_bytes:
            more = f.read(CHUNK)
            if not more:
                raise SystemExit(f"{path}: no '# END' header terminator -- is this a "
                                 "vxc_matcensus --dump file?")
            head_bytes += more
            if len(head_bytes) > 64 * 1024:
                raise SystemExit(f"{path}: no '# END' in the first 64 KiB -- is this a "
                                 "vxc_matcensus --dump file?")
    offset = head_bytes.index(b"# END\n") + len(b"# END\n")
    head = head_bytes[:offset].decode("ascii")

    # ONE KEY PER LINE: first token is the key, the rest are its values.
    meta = {}
    for line in head.splitlines():
        parts = line.lstrip("#").split()
        if len(parts) >= 2:
            meta[parts[0]] = parts[1:]
    n = int(meta["grid"][0])
    cell_m = float(meta["metres_per_cell"][0])

    want = n * n * RECORD.size
    have = Path(path).stat().st_size - offset
    if have != want:
        raise SystemExit(f"{path}: body is {have} bytes, header says "
                         f"{n}x{n} records = {want}")

    dt = np.dtype([("surface", "<i4"), ("gx", "<i4"), ("gy", "<i4"),
                   ("mat", "u1"), ("biome", "u1"), ("temp", "u1"), ("precip", "u1")])
    arr = np.fromfile(path, dtype=dt, count=n * n, offset=offset).reshape(n, n)
    return {"n": n, "cell_m": cell_m, "meta": meta}, arr


def srgb_to_linear(a):
    import numpy as np
    a = np.asarray(a, dtype=np.float64) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(a):
    import numpy as np
    a = np.clip(a, 0.0, 1.0)
    return np.where(a <= 0.0031308, a * 12.92, 1.055 * a ** (1 / 2.4) - 0.055)


def material_tints(mats, cell_m, stride_vox):
    """(h, w, 2) of (light, hue) from vxc::voxelTint, via the engine itself."""
    import numpy as np

    cxx = shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        raise SystemExit("no g++ or clang++ on PATH -- the tint comes from the "
                         "engine's own evaluation, not from a copy of it")
    h, w = mats.shape
    ys, xs = np.mgrid[0:h, 0:w]
    # The lattice coordinate each cell stands for, so the dither is the world's
    # and not the image's: at a 50-voxel stride, neighbouring pixels are 5 m
    # apart and must draw uncorrelated tints, which they do because the hash
    # sees their real separation.
    vx = (xs * stride_vox).ravel()
    vy = (ys * stride_vox).ravel()
    flat = mats.ravel()

    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "tint.cpp"
        src.write_text(TINT_PROBE)
        exe = Path(tmp) / "tint"
        build = subprocess.run([cxx, "-std=c++20", "-O2", "-I", str(INCLUDE),
                                str(src), "-o", str(exe)],
                               capture_output=True, text=True)
        if build.returncode != 0:
            raise SystemExit("the tint probe did not compile:\n" + build.stderr)
        payload = [str(flat.size)]
        payload += [f"{int(m)} {int(a)} {int(b)}" for m, a, b in zip(flat, vx, vy)]
        run = subprocess.run([str(exe)], input="\n".join(payload),
                             capture_output=True, text=True)
        if run.returncode != 0:
            raise SystemExit("the tint probe failed:\n" + run.stderr)

    out = np.fromstring(run.stdout.replace("\n", " "), sep=" ", dtype=np.int64)
    return (out.reshape(h, w, 2) / 65536.0)


def lut_colours(temp, precip):
    """Today's answer: the biome LUT, evaluated at each column's climate.

    Imported from ue-project/Tools/gen_terrain_textures.py rather than copied,
    so `--compare` cannot quietly compare against a LUT that no longer ships.
    """
    import numpy as np
    sys.path.insert(0, str(UE_TOOLS))
    try:
        import gen_terrain_textures as gtt
    except ImportError as exc:
        raise SystemExit(f"cannot import gen_terrain_textures ({exc}) -- "
                         "--compare needs it to paint today's colours")
    lut = gtt.write_biome_lut(Path(tempfile.mkdtemp()) / "lut.png")  # sRGB u8
    size = lut.shape[0]

    # The same axis remap the material graph applies: both channels arrive
    # already stretched to this world's p1..p99 by VoxelClimateProbe, and the
    # LUT is painted over that stretched range. Reproduced from the generator's
    # own constants so the two cannot drift.
    lo_t, hi_t = gtt.TEMP_U8_LO, gtt.TEMP_U8_HI
    lo_p, hi_p = gtt.PRECIP_U8_LO, gtt.PRECIP_U8_HI
    u = np.clip((precip.astype(np.float64) - lo_p) / max(1, hi_p - lo_p), 0, 1)
    v = np.clip((temp.astype(np.float64) - lo_t) / max(1, hi_t - lo_t), 0, 1)
    iu = np.clip((u * (size - 1)).astype(np.int32), 0, size - 1)
    iv = np.clip((v * (size - 1)).astype(np.int32), 0, size - 1)
    return lut[iv, iu]  # sRGB u8, (h, w, 3)


def hillshade(gx, gy, strength):
    """Lambert against a fixed sun, so landform is legible.

    NOT the game's lighting -- there is no sky, no shadow and no ambient
    occlusion here. It exists so a hillside reads as a hillside; without it a
    top-down albedo map of varied terrain is an unreadable field of noise.
    """
    import numpy as np
    if strength <= 0.0:
        return np.ones(gx.shape, dtype=np.float64)
    # mm per metre -> slope, then a normal.
    sx = gx.astype(np.float64) / 1000.0
    sy = gy.astype(np.float64) / 1000.0
    inv = 1.0 / np.sqrt(sx * sx + sy * sy + 1.0)
    nx, ny, nz = -sx * inv, -sy * inv, inv
    # Sun from the north-west and fairly high, the cartographic convention.
    lx, ly, lz = -0.5, 0.5, 0.7071
    ln = 1.0 / (lx * lx + ly * ly + lz * lz) ** 0.5
    lam = np.clip(nx * lx * ln + ny * ly * ln + nz * lz * ln, 0.0, 1.0)
    return 1.0 - strength + strength * (0.35 + 0.65 * lam)


def render(arr, palette, mode, cell_m, stride_vox, shade):
    import numpy as np
    mats = arr["mat"]

    if mode == "lut":
        base = srgb_to_linear(lut_colours(arr["temp"], arr["precip"]))
    else:
        top = np.zeros((256, 3), dtype=np.float64)
        for i, row in palette.items():
            top[i] = row["top"]
        base = srgb_to_linear(top[mats])

    if mode in ("albedo", "lit"):
        tint = material_tints(mats, cell_m, stride_vox)
        light, hue = tint[..., 0], tint[..., 1]
        base = base * (1.0 + light)[..., None]
        base[..., 0] *= 1.0 + hue
        base[..., 2] *= 1.0 - hue

    if mode in ("lit", "lut"):
        base = base * hillshade(arr["gx"], arr["gy"], shade)[..., None]

    return (linear_to_srgb(np.maximum(base, 0.0)) * 255.0).astype("uint8")


def legend_rows(arr, palette):
    import numpy as np
    mats, counts = np.unique(arr["mat"], return_counts=True)
    order = np.argsort(-counts)
    total = float(arr["mat"].size)
    out = []
    for i in order:
        m = int(mats[i])
        name = palette.get(m, {}).get("name", f"id {m}")
        out.append((name, palette.get(m, {}).get("top", (255, 0, 255)),
                    100.0 * counts[i] / total))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump", help="binary dump from vxc_matcensus --dump")
    ap.add_argument("-o", "--out", default="world-preview.png")
    ap.add_argument("--mode", default="lit",
                    choices=["material", "albedo", "lit", "lut"],
                    help="material: flat colour; albedo: + per-voxel variation; "
                         "lit: + hillshade (default); lut: today's biome LUT")
    ap.add_argument("--compare", action="store_true",
                    help="today's LUT colours beside the material colours")
    ap.add_argument("--shade", type=float, default=0.55,
                    help="hillshade strength 0..1 (default 0.55)")
    a = ap.parse_args()

    try:
        import numpy as np  # noqa: F401
        from PIL import Image, ImageDraw
    except ImportError as exc:
        raise SystemExit(f"{exc}: needs numpy and Pillow")

    palette = read_palette()
    info, arr = read_dump(a.dump)
    stride_vox = int(info["meta"]["stride_vox"][0])

    panels = []
    if a.compare:
        # THE LABELS CHANGED WITH ADR-0009 AND THE CHANGE MATTERS. This used to
        # read "today: biome LUT" against "ADR-0009: material", i.e. the old
        # authored LUT against the new material path. Now that the LUT is
        # generated from the palette too, the comparison is no longer old-vs-new
        # -- it is the 50 km VISTA against the NEAR FIELD, which is the seam the
        # two paths have to agree across. Mislabelling it would have someone read
        # a seam check as a before/after.
        panels = [("clipmap / 50 km vista: biome LUT", render(arr, palette, "lut",
                                                              info["cell_m"],
                                                              stride_vox, a.shade)),
                  ("near field: material", render(arr, palette, "lit",
                                                  info["cell_m"], stride_vox,
                                                  a.shade))]
    else:
        panels = [(a.mode, render(arr, palette, a.mode, info["cell_m"], stride_vox,
                                  a.shade))]

    n = info["n"]
    pad, header, legend_h = 12, 30, 18 * (len(legend_rows(arr, palette)) + 1)
    width = pad + len(panels) * (n + pad)
    height = header + n + 10 + legend_h + pad
    img = Image.new("RGB", (width, height), (18, 19, 22))
    draw = ImageDraw.Draw(img)

    # TRI-STATE, deliberately. A dump written before the header split has no
    # real_tiles key at all, and defaulting a missing key to "synthetic" prints
    # a confident claim about provenance from an absence of evidence -- the same
    # shape of error as the bug that made this tri-state. Unknown says unknown.
    real = info["meta"].get("real_tiles", [None])[0]
    real = None if real is None else real == "1"
    source = {True: "real tiles", False: "SyntheticTileSampler",
              None: "tile source UNKNOWN (dump predates the real_tiles header)"}[real]
    ox, oy = info["meta"]["origin_m"][0], info["meta"]["origin_m"][1]
    draw.text((pad, 6),
              f"ground colour, top-down  ·  {n}×{n} columns at {info['cell_m']:.1f} m/cell "
              f"({n * info['cell_m'] / 1000:.1f} km across)  ·  origin ({ox}, {oy}) m  ·  "
              f"{source}  ·  "
              f"no sun, no sky, no AO, no vegetation",
              fill=(150, 155, 165))

    for i, (label, pix) in enumerate(panels):
        x = pad + i * (n + pad)
        img.paste(Image.fromarray(pix, "RGB"), (x, header))
        draw.text((x, header - 14), label, fill=(225, 228, 233))

    y = header + n + 10
    draw.text((pad, y), "surface material, share of columns:", fill=(150, 155, 165))
    y += 18
    # MAT_AIR in a SURFACE census is not an error: it is a cave mouth breaking
    # daylight, where the cave pass has carved away the column's own top voxel.
    # It renders as the palette's black, which is right -- a hole is a hole --
    # but the legend has to say so or the next person reads 0.01% AIR as a bug.
    for name, rgb, pct in legend_rows(arr, palette):
        if name == "MAT_AIR":
            name = "AIR (cave mouth)"
        draw.rectangle([pad, y + 3, pad + 22, y + 13], fill=tuple(int(v) for v in rgb))
        draw.text((pad + 30, y), f"{name[4:]:<20s} {pct:5.2f}%", fill=(200, 204, 212))
        y += 18

    img.save(a.out)
    print(f"wrote {a.out}  ({img.size[0]}x{img.size[1]})")
    if real is False:
        print("NOTE: SyntheticTileSampler -- the biome mix is not the shipped "
              "world's. Point vxc_matcensus at a real tile cache for proportions.")
    elif real is None:
        print("NOTE: this dump has no real_tiles header, so the tile source is "
              "unknown. Re-dump with a current vxc_matcensus before quoting "
              "proportions.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
