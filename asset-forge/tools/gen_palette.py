"""Generate `forge/palette.py` from the engine's material palette header.

The palette is defined once, in `voxel-core/include/voxelcore/materialpalette.h`,
and every consumer reads a copy generated from it. That is not tidiness: the
material IDs were declared independently in the engine and in asset-forge for a
while, drifted, and because MaterialId is a byte the result rendered as
SOMETHING rather than failing. Nine of thirteen materials were out of range
before anyone noticed. Colour would go the same way and be harder to spot,
because a wrong colour still looks like a colour.

So the forge does not get to have opinions about what bark looks like. It reads
what the engine will render and shows you that.

Run after editing the header:

    python tools/gen_palette.py

Forgetting to is the failure this cannot prevent, so it is caught elsewhere:
`python -m forge.cli selftest` calls `source()` below and compares it with the
committed `forge/palette.py`. That is why the generation is a function that
returns text and `main()` only writes it.
"""
import re
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)

ROOT = Path(__file__).resolve().parents[1]
# Derived rather than hard-coded so the check that reads this can run from any
# checkout: voxel-core is asset-forge's sibling.
HEADER = ROOT.parent / "voxel-core" / "include" / "voxelcore" / "materialpalette.h"
OUT = ROOT / "forge" / "palette.py"

ENTRY = re.compile(
    r"/\*\s*(MAT_\w+)\s*\*/\s*"
    r"\{\{\{([^}]*)\},\s*\{([^}]*)\},\s*\{([^}]*)\}\},\s*"
    r"(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}",
    re.S,
)


def triple(text):
    return tuple(int(v) for v in text.replace(" ", "").split(","))


CORE = HEADER.parent / "core.h"
ENUM = re.compile(r"^\s*(MAT_\w+)\s*=\s*(\d+)\s*,", re.M)


def enum_order():
    """The material names in id order, read from the enum itself."""
    pairs = [(int(m.group(2)), m.group(1)) for m in ENUM.finditer(CORE.read_text(encoding="utf-8"))]
    pairs.sort()
    return [name for _, name in pairs]


def source() -> str:
    """The text `forge/palette.py` should contain, read out of the header.

    Everything the generator decides happens in here and nothing is written, so
    the same work can be redone in memory and compared against what is on disk.
    """
    for needed in (HEADER, CORE):
        if not needed.exists():
            raise SystemExit(f"cannot read the engine palette: no {needed}")
    src = HEADER.read_text(encoding="utf-8")
    rows = []
    for i, m in enumerate(ENTRY.finditer(src)):
        name, top, side, bottom = m.group(1), triple(m.group(2)), triple(m.group(3)), triple(m.group(4))
        jitter, hue, patch, scale, biome = (int(m.group(k)) for k in range(5, 10))
        rows.append((i, name, top, side, bottom, jitter, hue, patch, scale, biome))
    if not rows:
        raise SystemExit(f"no palette entries parsed from {HEADER}")

    # The palette table is POSITIONAL: entry N must be material N. Nothing in
    # C++ enforces that -- the static_assert on the table counts entries and is
    # blind to their order -- so it is checked here, against the enum itself.
    #
    # This is not hypothetical. The table was first written grouped by type,
    # which put MAT_BARK_PALE (id 23) up with the other woods at index 19 and
    # shifted every id above it, so every broadleaf tree in the library was
    # rendered in birch bark. A wrong colour still looks like a colour, which
    # is exactly why it needs a machine to notice.
    want = enum_order()
    got = [name for _, name, *_ in rows]
    if got != want[:len(got)] or len(got) != len(want):
        for i, (a, b) in enumerate(zip(got + [None] * len(want), want)):
            if a != b:
                raise SystemExit(
                    f"palette is out of step with the enum at index {i}: "
                    f"table has {a}, enum has {b}.\n"
                    f"  {HEADER.name} must list materials in id order.")
        raise SystemExit(f"palette has {len(got)} entries, enum has {len(want)}")

    lines = [
        '"""What each material looks like. GENERATED — do not edit.',
        "",
        f"Source: {HEADER.as_posix()}",
        "Regenerate: python tools/gen_palette.py",
        "",
        "One flat colour per voxel face, varied per voxel. See ADR-0008 for why",
        "this is not a texture and why the variation has two frequencies, and",
        "ADR-0009 for the third scale (biome_tint) and the metric patch unit.",
        "",
        "THE TABLE IS HERE, THE EVALUATION IS NOT. How these numbers turn into a",
        "colour is voxelcore/materialcolor.h, mirrored in forge/render.py.",
        '"""',
        "",
        "# id: (top, side, bottom, voxel_jitter, voxel_hue, patch_strength,",
        "#      patch_scale_dm, biome_tint)",
        "# Colours are sRGB 0-255. voxel_jitter, voxel_hue, patch_strength and",
        "# biome_tint are 1/255ths; patch_scale_dm is a WORLD wavelength in",
        "# decimetres (10 cm), which is one level-0 voxel and half a detail-grid",
        "# one -- it is not counted in whatever cells the grid at hand happens",
        "# to use.",
        "PALETTE = {",
    ]
    for i, name, top, side, bottom, jit, hue, patch, scale, biome in rows:
        lines.append(f"    {i:>2}: ({top}, {side}, {bottom}, {jit}, {hue}, {patch}, "
                     f"{scale}, {biome}),  # {name}")
    lines += [
        "}",
        "",
        "FACE_TOP, FACE_SIDE, FACE_BOTTOM = 0, 1, 2",
        "",
        "MATERIAL_COUNT = %d" % len(rows),
        "",
        "# Column indices into a PALETTE row, so a reader does not have to count.",
        "TOP, SIDE, BOTTOM = 0, 1, 2",
        "JITTER, HUE, PATCH, PATCH_SCALE_DM, BIOME_TINT = 3, 4, 5, 6, 7",
        "",
        "",
        "def entry(mat: int):",
        '    """Appearance for a material id, magenta if it has none."""',
        "    return PALETTE.get(int(mat), (((255, 0, 255),) * 3) + (0, 0, 0, 0, 0))",
        "",
    ]
    return "\n".join(lines)


def main():
    text = source()
    OUT.write_text(text, encoding="utf-8")
    count = re.search(r"^MATERIAL_COUNT = (\d+)$", text, re.M).group(1)
    print(f"{OUT}  <-  {count} materials")


if __name__ == "__main__":
    main()
