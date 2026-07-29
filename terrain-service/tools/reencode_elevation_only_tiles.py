#!/usr/bin/env python3
"""Make the 52-tile UNPINNED world READABLE, without changing a single elevation.

WHY THIS EXISTS
---------------
The tiles in
``tile-cache/terrain-diffusion-unlabeled-UNPINNED-UNVERIFIEDDATA-27ac04bc8c6b7b7d/
000000000135276f/s1/`` are 524313 bytes each. A ``.vxtl`` v1 tile is
25 (header) + 512*512*2 (elevation) + 4*512*512 (climate) = 1572889 bytes, so
these are **512 KiB short: they carry no climate planes at all**. They were
written by ``tools/gen_world_tiles.py``, which hand-rolled the wire encode
instead of calling ``tile_codec.encode``; ``tools/gen_world_tiles2.py``'s
docstring records the same defect and the same diagnosis.

The consequence is not cosmetic. Nothing downstream can read them:

* ``tile_codec.decode`` raises ``ValueError: buffer is smaller than requested
  size`` -- so ``bake_tile``, ``bake_real_tile.py`` and
  ``dump_stage_heightfields.py`` all die on the first tile;
* ``voxel-core``'s v1 reader (``src/tilestore.cpp``, the four
  ``if (!r.u8(tile.climate[c][i])) return std::nullopt;`` lines) returns
  ``nullopt``, so ``TileGridSampler`` -- and therefore ``vxc_stagedump``,
  and therefore the game client -- sees no tile at all.

The right repair is to regenerate through the shipping provider
(``gen_world_tiles2.py``). That is a model run per tile and, at the time of
writing, it also PINS the checkpoint, which rolls ``provider_id`` -- so the
regenerated tiles are a different world under a different cache id, not the one
under study. When what you need is to MEASURE THE TILES THAT EXIST, this tool
makes them parseable and changes nothing that any height depends on.

WHAT IS AND IS NOT INVENTED
---------------------------
Elevation is copied byte-for-byte off the input. The climate planes cannot be
recovered -- they were never written -- so they are filled with a single
constant (default 128, mid-range on all four u8 channels) and that constant is
recorded in the sidecar this tool writes beside the output directory.

Every SURFACE HEIGHT in the pipeline is independent of that choice, which is
the only reason this is admissible:

* ``bake.pipeline.bake_tile`` is handed ``coarse_fetch(x, y) -> elevation``
  and never sees a climate plane (see ``dump_stage_heightfields._coarse_fetch``);
* ``Amplifier::evalSurface`` (voxel-core/src/amplifier.cpp) -- and hence
  ``surfaceMm``, i.e. S2 and S3 -- reads ``elevationMm`` only;
* ``Amplifier::column`` blends climate, but spends it on ``surfaceMat``,
  ``topsoilMm``, ``subsoilMm`` and the biome, none of which move the surface.

The ONE place a constant climate can reach a height is S4: ``col.d3 =
density3ColumnFor(seed_, x, y, slope, soilAboveRockMm(col))``, and
``soilAboveRockMm`` is topsoil + subsoil, which climate sets. Where the density3
slope gate is closed the displacement is identically zero and S4 is exactly the
quantised S3; where it is open, S4's top voxel can move by up to
``kDensity3MaxAbsMm``. ``vxc_stagedump``'s S4 sidecar reports ``displaced_frac``
for precisely this reason -- quote it whenever an S4 number is quoted off a
tile repaired here.

USAGE
-----
    python tools/reencode_elevation_only_tiles.py \
        --src  tile-cache/<provider>/<seed>/s1 \
        --dest <scratch dir> \
        [--tile -55,20]... [--ring] [--climate-u8 128]

``--ring`` expands every ``--tile`` to its 3x3 neighbourhood, which is what a
bake needs. With no ``--tile`` the whole source directory is converted.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec  # noqa: E402

#: header + elevation, with no climate planes: what gen_world_tiles.py wrote.
ELEV_ONLY_BYTES = tile_codec._HEADER.size + 2 * tile_codec.TILE_SIZE ** 2
#: the correct v1 length, for the error message.
FULL_BYTES = ELEV_ONLY_BYTES + tile_codec.CLIMATE_CHANNELS * tile_codec.TILE_SIZE ** 2


def read_elevation_only(path: Path) -> tile_codec.Tile:
    """Parse a header + elevation tile. Refuses anything that is not one."""
    data = path.read_bytes()
    if len(data) == FULL_BYTES:
        raise ValueError(f"{path.name} is a complete v1 tile; nothing to repair")
    if len(data) != ELEV_ONLY_BYTES:
        raise ValueError(
            f"{path.name} is {len(data)} bytes, which is neither the elevation-only "
            f"{ELEV_ONLY_BYTES} nor the complete {FULL_BYTES}"
        )
    magic, version, seed, x, y, scale, size = tile_codec._HEADER.unpack_from(data)
    if magic != tile_codec.MAGIC:
        raise ValueError(f"{path.name}: bad magic {magic!r}")
    if version != tile_codec.VERSION:
        raise ValueError(f"{path.name}: version {version}, expected 1")
    if size != tile_codec.TILE_SIZE:
        raise ValueError(f"{path.name}: size {size}, expected {tile_codec.TILE_SIZE}")
    elev = np.frombuffer(
        data, dtype="<i2", count=size * size, offset=tile_codec._HEADER.size
    ).reshape(size, size).astype(np.int16)
    return tile_codec.Tile(
        seed=seed, x=x, y=y, scale=scale, elevation=elev,
        climate=np.zeros((tile_codec.CLIMATE_CHANNELS, size, size), dtype=np.uint8),
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("USAGE")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True)
    ap.add_argument("--dest", required=True)
    ap.add_argument("--tile", action="append", default=None, metavar="X,Y")
    ap.add_argument("--ring", action="store_true",
                    help="expand each --tile to its 3x3 neighbourhood (what a bake needs)")
    ap.add_argument("--climate-u8", type=int, default=128,
                    help="constant written into all four climate channels (default 128)")
    a = ap.parse_args()

    src, dest = Path(a.src), Path(a.dest)
    dest.mkdir(parents=True, exist_ok=True)
    if not 0 <= a.climate_u8 <= 255:
        raise SystemExit("--climate-u8 must be 0..255")

    if a.tile:
        want = set()
        for spec in a.tile:
            tx, ty = (int(v) for v in spec.split(","))
            rng = (-1, 0, 1) if a.ring else (0,)
            for dy in rng:
                for dx in rng:
                    want.add((tx + dx, ty + dy))
        paths = [src / f"{x}_{y}.vxtl" for x, y in sorted(want)]
    else:
        paths = sorted(src.glob("*.vxtl"))

    done, skipped = [], []
    for p in paths:
        if not p.exists():
            raise SystemExit(f"{p} is not in the source directory")
        try:
            t = read_elevation_only(p)
        except ValueError as e:
            if "complete v1 tile" in str(e):
                (dest / p.name).write_bytes(p.read_bytes())
                skipped.append(p.name)
                continue
            raise SystemExit(str(e))
        t.climate[:] = np.uint8(a.climate_u8)
        (dest / p.name).write_bytes(tile_codec.encode(t))
        done.append({"tile": p.name, "seed": t.seed, "scale": t.scale,
                     "elev_min_m": int(t.elevation.min()),
                     "elev_max_m": int(t.elevation.max())})
        print(f"  {p.name}: elevation {t.elevation.min()}..{t.elevation.max()} m, "
              f"climate <- {a.climate_u8}")

    (dest / "reencode_manifest.json").write_text(json.dumps({
        "schema": "vxc.reencode_elevation_only.v1",
        "producer": "terrain-service/tools/reencode_elevation_only_tiles.py",
        "source_dir": str(src),
        "climate_u8_constant": a.climate_u8,
        "climate_is_synthetic": True,
        "elevation_is_verbatim": True,
        "why": ("source tiles are header+elevation only (gen_world_tiles.py wrote no "
                "climate planes), so neither tile_codec.decode nor voxel-core's v1 "
                "reader can parse them"),
        "heights_unaffected": ("bake_tile, Amplifier::evalSurface and therefore S1/S2/S3 "
                               "never read climate; S4 can move only through "
                               "soilAboveRockMm -> density3, bounded by the S4 sidecar's "
                               "displaced_frac"),
        "converted": done,
        "already_complete_copied": skipped,
    }, indent=2))
    print(f"\nwrote {len(done)} repaired + {len(skipped)} verbatim tile(s) to {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
