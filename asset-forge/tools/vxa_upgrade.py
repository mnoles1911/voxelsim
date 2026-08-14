"""Bring an old VXA file to the current version without rebuilding the asset.

WHY THIS EXISTS, AND IT IS NOT A CONVENIENCE.

Version 2 added the voxel size and version 3 added the rig -- part tags and
joints -- and the reader refuses both older versions rather than assuming
anything; see `forge/vxa.py`. That is right, but "re-bake it" is only a
valid answer while the asset is still reproducible, and a baked asset stops
being reproducible the moment anything is added to the parameter table.
Adding a parameter changes every spec's hash, which reseeds every species, so
`(spec, seed)` no longer names the individual it named yesterday.

That is not hypothetical. `hero-arch-colossal-0006` is a landmark the owner
chose off a contact sheet of six candidates. Re-baking it after two rounds of
parameter additions produced a different stone -- 49,336,892 voxels became
69,227,063 -- and the one he picked existed nowhere except in that v1 file.
Regenerating it is impossible by construction; converting it is trivial,
because a v1 file is not missing any voxels. It is missing one number about
them.

So the conversion is exactly that: parse the v1 header, write the v2 header
with a voxel size supplied by the caller, and copy the run table across
BYTE FOR BYTE. The body format did not change between versions and this
deliberately does not decode it -- there is no rounding, no re-run-lengthing
and no opportunity for a bug to alter a voxel.

The voxel size has to be supplied because the file cannot know it; that is the
entire defect being repaired. Get it from the entry's `meta.json`
(`stats.voxel_cm`), which is what `--from-meta` does.

    python tools/vxa_upgrade.py library/x/x-0001/tree.vxa --from-meta
    python tools/vxa_upgrade.py some.vxa --voxel-mm 100
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import vxa

V1_HEADER_BYTES = 36
V2_HEADER_BYTES = 40


def upgrade(blob: bytes, voxel_mm: int) -> bytes:
    """v1 or v2 bytes in, v3 bytes out. The run table is copied, never decoded."""
    if blob[:4] != vxa.MAGIC:
        raise ValueError("not a VXA file")
    version = struct.unpack("<I", blob[4:8])[0]
    if version == vxa.VERSION:
        raise ValueError(f"already version {vxa.VERSION}")
    if version not in (1, 2):
        raise ValueError(f"cannot upgrade version {version}")
    if voxel_mm <= 0:
        raise ValueError("voxel size must be positive")

    if version == 1:
        ox, oy, oz, nx, ny, nz, nruns = struct.unpack(
            "<iiiIIII", blob[8:V1_HEADER_BYTES])
        body = blob[V1_HEADER_BYTES:]
    else:
        ox, oy, oz, nx, ny, nz, had_mm, nruns = struct.unpack(
            "<iiiIIIII", blob[8:V2_HEADER_BYTES])
        body = blob[V2_HEADER_BYTES:]
        # A v2 file already knows its scale; believe it over the caller.
        voxel_mm = had_mm
    if len(body) != nruns * 5:
        raise ValueError(
            f"body is {len(body)} bytes, header promises {nruns * 5}; refusing "
            f"to upgrade a file that is already damaged")

    return (vxa.MAGIC
            + struct.pack("<I", vxa.VERSION)
            + struct.pack("<iii", ox, oy, oz)
            + struct.pack("<III", nx, ny, nz)
            + struct.pack("<I", int(voxel_mm))
            + struct.pack("<I", nruns)
            # NO PARTS AND NO JOINTS. That is the honest answer rather than a
            # limitation: an old file records voxels, and which voxels were a
            # wing is knowledge only the generator had. Anything that needs a
            # rig has to be re-baked; anything that does not -- every rock and
            # tree in the library -- is complete as it stands.
            + struct.pack("<I", 0)
            + struct.pack("<I", 0)
            + body)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--voxel-mm", type=int,
                    help="voxel edge in millimetres, if you know it")
    ap.add_argument("--from-meta", action="store_true",
                    help="read it from the sibling meta.json's stats.voxel_cm")
    args = ap.parse_args()

    path = Path(args.path)
    mm = args.voxel_mm
    if args.from_meta:
        meta = json.loads((path.parent / "meta.json").read_text(encoding="utf-8"))
        mm = int(round(float(meta["stats"]["voxel_cm"]) * 10.0))
    if not mm:
        print("need --voxel-mm or --from-meta: a v1 file cannot tell you its "
              "own scale, which is the whole reason version 2 exists",
              file=sys.stderr)
        return 2

    blob = path.read_bytes()
    was = struct.unpack("<I", blob[4:8])[0]
    out = upgrade(blob, mm)
    path.write_bytes(out)

    # Read it back through the real decoder rather than trusting the write.
    g = vxa.read(path)
    print(f"{path}: v{was} -> v{vxa.VERSION}, {int(g.voxel_m * 1000)} mm, "
          f"{g.shape[0]}x{g.shape[1]}x{g.shape[2]}, "
          f"{int((g.data != 0).sum()):,} solid voxels")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
