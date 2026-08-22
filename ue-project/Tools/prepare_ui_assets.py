#!/usr/bin/env python3
"""Bring the front end's font and menu background art into ue-project/Content/UI.

WHY THIS IS A TOOL AND NOT A ONE-LINE COPY
------------------------------------------
The source art lives in the Mira-Thal (Godot) checkout at
assets/menu_backgrounds/ and is 30.6 MB across six JPEGs -- two of them
5524x3072. This repository's largest tracked binary today is 385 KB, and its
.gitignore records that the 38.8 MB T_SkyStarmap was excluded for exactly this
reason. Committing the originals unchanged would be a 79x jump in the largest
file the repo has ever carried, for pixels nobody can see: the menu draws them
at viewport resolution behind a 55% black wash.

So this script re-encodes them to fit 1920x1080 with Lanczos resampling at
quality 82, which lands the set at roughly 2 MB. That IS committed -- art the
front end cannot render without is source, not build output, and unlike the
sky textures there is no upstream to fetch from (the source is a sibling
checkout that CI does not have, so a fetch script would fail everywhere except
one machine). See docs/adr/0009-slate-front-end-and-committed-ui-art.md.

The credits file records the sha256 of both the original and the output for
every image, so a re-prepare is reproducible and a swapped source is visible.

USAGE
-----
    python ue-project/Tools/prepare_ui_assets.py --source /path/to/Test
    python ue-project/Tools/prepare_ui_assets.py --source ../Test --max-width 2560

Needs Pillow, which tools/imgdiff.py and tools/sky-albedo-check.py already
depend on, so it is not a new dev dependency.

NOTE: unlike every other script in this directory, this one does NOT run inside
the editor. It writes plain files that Slate loads at runtime by path -- there
is no .uasset, no import, and no UnrealEditor-Cmd invocation. That is the whole
point of the Slate front end (see Source/VoxelEarthUI/VoxelEarthUI.Build.cs).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
from datetime import datetime, timezone

try:
    from PIL import Image
except ImportError:
    sys.exit("prepare_ui_assets: Pillow is required (python -m pip install Pillow)")

# Source basename -> destination basename. The destination names drop the
# "_background1" suffix, which meant something in a folder of many candidates
# per scene and means nothing in a folder of exactly six.
BACKGROUNDS = {
    "battle_background1.jpg": "battle.jpg",
    "castle_feast_background1.jpg": "castle_feast.jpg",
    "cave_background1.jpg": "cave.jpg",
    "forest_fight_background1.jpg": "forest_fight.jpg",
    "fortress_battles_background1.jpg": "fortress_battles.jpg",
    "sailing_background1.jpg": "sailing.jpg",
}

FONT_SOURCE = os.path.join("assets", "fonts", "MacondoSwashCaps-Regular.ttf")
FONT_DEST = "MacondoSwashCaps-Regular.ttf"


def sha256_of(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def content_dir() -> str:
    # This file is ue-project/Tools/prepare_ui_assets.py.
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Content", "UI"))


def prepare_font(source_root: str, dest_root: str, report: list[str]) -> bool:
    src = os.path.join(source_root, FONT_SOURCE)
    if not os.path.isfile(src):
        print(f"  font: MISSING at {src}", file=sys.stderr)
        return False
    dest_dir = os.path.join(dest_root, "Fonts")
    os.makedirs(dest_dir, exist_ok=True)
    dest = os.path.join(dest_dir, FONT_DEST)
    # Copied byte-for-byte. A 31 KB font has nothing to optimise and re-encoding
    # a typeface is how hinting gets quietly destroyed.
    shutil.copyfile(src, dest)
    report.append(f"| {FONT_DEST} | copied verbatim | {os.path.getsize(dest):,} B | `{sha256_of(dest)}` |")
    print(f"  font: {dest} ({os.path.getsize(dest):,} B)")
    return True


def prepare_backgrounds(source_root: str, dest_root: str, max_width: int, max_height: int,
                        quality: int, report: list[str]) -> int:
    src_dir = os.path.join(source_root, "assets", "menu_backgrounds")
    dest_dir = os.path.join(dest_root, "Backgrounds")
    os.makedirs(dest_dir, exist_ok=True)

    written = 0
    total_in = 0
    total_out = 0
    for src_name, dest_name in sorted(BACKGROUNDS.items()):
        src = os.path.join(src_dir, src_name)
        if not os.path.isfile(src):
            print(f"  {dest_name}: MISSING source {src}", file=sys.stderr)
            continue
        src_sha = sha256_of(src)
        src_bytes = os.path.getsize(src)
        total_in += src_bytes

        with Image.open(src) as image:
            original_size = image.size
            image = image.convert("RGB")
            # thumbnail() preserves aspect ratio and never upscales, which is
            # the behaviour wanted here: the menu's own stretch mode is
            # KEEP_ASPECT_COVERED, so an image narrower than the cap is already
            # fine and enlarging it would only cost bytes.
            image.thumbnail((max_width, max_height), Image.LANCZOS)
            dest = os.path.join(dest_dir, dest_name)
            # progressive=True because the whole file is read before the first
            # decode anyway (FFileHelper::LoadFileToArray), so progressive costs
            # nothing here and shaves a few percent off the size.
            image.save(dest, "JPEG", quality=quality, optimize=True, progressive=True)
            out_size = image.size

        out_bytes = os.path.getsize(dest)
        total_out += out_bytes
        written += 1
        report.append(
            f"| {dest_name} | {src_name} {original_size[0]}x{original_size[1]} "
            f"({src_bytes / 1048576:.2f} MB) | {out_size[0]}x{out_size[1]} ({out_bytes / 1024:.0f} KB) | "
            f"`{src_sha}` |")
        print(f"  {dest_name}: {original_size[0]}x{original_size[1]} -> {out_size[0]}x{out_size[1]}, "
              f"{src_bytes / 1048576:.2f} MB -> {out_bytes / 1024:.0f} KB")

    if written:
        print(f"  backgrounds total: {total_in / 1048576:.2f} MB -> {total_out / 1048576:.2f} MB "
              f"({100.0 * total_out / total_in:.1f}%)")
    return written


def write_credits(dest_root: str, source_root: str, max_width: int, max_height: int, quality: int,
                  rows: list[str]) -> None:
    path = os.path.join(dest_root, "Backgrounds", "MENU_ART_CREDITS.md")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(f"""# Menu art and font: provenance

Generated by `ue-project/Tools/prepare_ui_assets.py` on {stamp}.

These files are the front end's background art and typeface. They are ported,
along with the rest of the main menu, from the Mira-Thal / *Voxelmark* Godot
build -- see `docs/front-end-plan.md` for the 1:1 clone decision that brought
them here.

**Source checkout when this was generated:** `{source_root}`
**Re-encode settings:** fit {max_width}x{max_height}, Lanczos, JPEG quality {quality}, progressive.

The originals stay in the Mira-Thal checkout at `assets/menu_backgrounds/` and
are identified below by the sha256 of the ORIGINAL file, so a re-prepare is
reproducible and a swapped source is visible in the diff rather than only in
the pixels.

The font is **Macondo Swash Caps**, an Open Font Licence face; see `../Fonts/OFL.txt`.
It is copied byte-for-byte -- re-encoding a typeface is how hinting gets
quietly destroyed.

| File | Source | Output | sha256 (source) |
|---|---|---|---|
""")
        for row in rows:
            handle.write(row + "\n")
    print(f"  credits: {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", required=True,
                        help="Root of the Mira-Thal (Godot) checkout holding assets/menu_backgrounds/.")
    parser.add_argument("--max-width", type=int, default=1920)
    parser.add_argument("--max-height", type=int, default=1080)
    parser.add_argument("--quality", type=int, default=82)
    args = parser.parse_args()

    source_root = os.path.abspath(args.source)
    dest_root = content_dir()
    print(f"prepare_ui_assets: {source_root} -> {dest_root}")

    rows: list[str] = []
    font_ok = prepare_font(source_root, dest_root, rows)
    count = prepare_backgrounds(source_root, dest_root, args.max_width, args.max_height, args.quality, rows)
    write_credits(dest_root, source_root, args.max_width, args.max_height, args.quality, rows)

    if not font_ok or count != len(BACKGROUNDS):
        # Exit non-zero on a partial run. The front end degrades gracefully
        # when art is missing (that path is deliberate and screenshot-tested
        # via -VoxelUINoAssets), but a PREPARE that silently produced four of
        # six images would be discovered as a menu that looks wrong on someone
        # else's machine, which is much later and much worse.
        print("prepare_ui_assets: INCOMPLETE -- see the missing entries above.", file=sys.stderr)
        return 1
    print("prepare_ui_assets: done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
