"""Dump the SERVER half of the amplification pipeline as plain heightfields.

WHY THIS EXISTS
---------------
We amplify 30 m/px terrain-diffusion tiles down to 10 cm voxels and the result
does not look like Earth. The end state has been measured repeatedly, which
tells us *that* it is wrong and not *where*. Making the defect localisable means
being able to look at every stage on the same ground in the same units.

The five stages, and who writes them:

    S0  raw 30 m diffusion tile, int16 whole metres, as it       >>> HERE <<<
        arrives on the wire.
    S1  the BAKE output at 1.875 m/px (terrain_service.bake.     >>> HERE <<<
        pipeline), i.e. after carrier + roughness + flow +
        incision + thermal relaxation.
    S2  the client carrier only (C2 B-spline over the tile       vxc_stagedump
        raster, every detail octave and additive term OFF).
    S3  the client full continuous surface, Amplifier::          vxc_stagedump
        surfaceMm, before voxelisation.
    S4  the voxelised surface as Amplifier::stratigraphyAt       vxc_stagedump
        sees it.

``voxel-core/bench/stagedump.cpp`` writes S2-S4 with the SAME sidecar schema
(``vxc.stagedump.v1``) over the same world rectangle, so all five diff directly.

GEOMETRY, WHICH IS THE PART THAT IS EASY TO GET SILENTLY WRONG
--------------------------------------------------------------
Every tier in this pipeline uses the same NODE convention: the sample with
index ``i`` on an axis sits at world coordinate ``i * cell_size``, and world
(0, 0) is coarse pixel (0, 0) is voxel column (0, 0).

* coarse pixel  i -> world  i * 30 m
* fine cell     i -> world  i * 1.875 m       (``noise.carrier``'s phase
                                               convention: fine index p*16
                                               coincides with coarse index p)
* voxel column  i -> world  i * 0.1 m

Coarse tile (tx, ty) starts at coarse pixel (tx*512, ty*512) and fine cell
(tx*8192, ty*8192) -- the same world corner, 15.36 km on a side.

So a footprint origin that is a multiple of 30 m lands exactly on a node of
both the 30 m and the 1.875 m lattice, and every S0 node is also an S1 node.
It is NOT a node of nothing at 0.1 m -- see ``voxel_snap`` in the C++ tool's
sidecars: 1.875 m is 18.75 voxels, so no voxel column sits on a fine post and
the client's S2/S3/S4 lattice is snapped by up to 50 mm. That is a property of
the world, not of these tools.

WHERE S1 COMES FROM, AND WHY IT IS NOT READ OUT OF THE .vxtl
------------------------------------------------------------
A baked ``.vxtl`` v2 tile does NOT contain S1. It contains the PREFILTERED
CONTROL LATTICE (docs/vxtl-v2-format.md §2) -- the thing whose cubic B-spline
reproduces S1. Evaluating that spline is exactly what S2-fine already is, so
reading the ``.vxtl`` and calling the result S1 would collapse two stages into
one and destroy the only comparison that can tell the encoder's round trip
apart from the client's carrier.

S1 is therefore produced by RE-RUNNING THE BAKE (``bake_tile``, the same call
``bake_real_tile.py`` makes, with the same level-0 hydrology superblock). That
costs ~150 CPU-s and ~5.5 GiB peak RSS per tile, so the full 8192^2 interior is
cached to ``--bake-cache-dir`` as a float32 ``.npy`` and every later footprint
inside the same tile is a free slice. **Do not run two bakes at once on this
box.**

The bake is deterministic (``bake_real_tile.py``'s docstring records a
byte-identical control plane across two runs), so the cached array is the same
surface the shipped ``.vxtl`` was encoded from. ``--verify-vxtl`` checks that
claim rather than asserting it: it reconstructs the shipped lattice's own
samples with the [1,4,1]/6 stencil the spline reduces to at a lattice point and
reports the residual against S1. The floor is the 100 mm control-point
quantisation; the tile on record measured mean 12.2 mm / max 50.1 mm.

USAGE
-----
    python tools/dump_stage_heightfields.py \
        --out DIR --seed 20260719 \
        --tiles-dir <s1 dir> \
        --origin -67920 34800 --span 960 \
        --bake --bake-cache-dir DIR \
        [--verify-vxtl <s16 dir>] \
        [--cell 30000] [--cell 1875] [--s0-only]

Dumps are large; ``--out`` should be a gitignored directory.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec  # noqa: E402
from terrain_service.bake import pipeline as bp  # noqa: E402

SCHEMA = "vxc.stagedump.v1"


# ---------------------------------------------------------------------------
# Lattice geometry. Integer MILLIMETRES throughout, so the rectangle a sidecar
# reports is exact rather than a float that nearly is. Mirrors the identical
# arithmetic in voxel-core/bench/stagedump.cpp -- a sub-lattice is centred on a
# whole number of its own cells, so its nodes stay a subset of the full one's.
# ---------------------------------------------------------------------------

class Lattice:
    def __init__(self, cell_mm: int, origin_mm: tuple[int, int], span_mm: int,
                 own_span_mm: int | None = None):
        lat_span = span_mm if own_span_mm is None else own_span_mm
        if lat_span > span_mm:
            raise ValueError(f"sub-span {lat_span} mm exceeds footprint {span_mm} mm")
        self.cell_mm = int(cell_mm)
        self.n = lat_span // self.cell_mm + 1
        inset = ((span_mm - (self.n - 1) * self.cell_mm) // 2 // self.cell_mm) * self.cell_mm
        self.x0_mm = origin_mm[0] + inset
        self.y0_mm = origin_mm[1] + inset

    @property
    def span_mm(self) -> int:
        return (self.n - 1) * self.cell_mm

    def node_mm(self, axis0_mm: int) -> np.ndarray:
        return axis0_mm + np.arange(self.n, dtype=np.int64) * self.cell_mm

    def describe(self) -> dict:
        return {
            "shape": [self.n, self.n],
            "cell_size_mm": self.cell_mm,
            "cell_size_m": self.cell_mm / 1000.0,
            "world_origin_m": [self.x0_mm / 1000.0, self.y0_mm / 1000.0],
            "world_origin_mm": [self.x0_mm, self.y0_mm],
            "world_bounds_m": [
                self.x0_mm / 1000.0,
                self.y0_mm / 1000.0,
                (self.x0_mm + self.span_mm) / 1000.0,
                (self.y0_mm + self.span_mm) / 1000.0,
            ],
            "world_span_m": self.span_mm / 1000.0,
        }


def write_stage(out_dir: Path, base: str, values: np.ndarray, lat: Lattice, *,
                stage: str, name: str, tier: str, seed: int, source: str,
                provenance: dict, quantisation: str = "",
                extra_stats: dict | None = None) -> dict:
    """One float32 .npy + one JSON sidecar, in the shared schema."""
    a = np.ascontiguousarray(values, dtype=np.float32)
    if a.shape != (lat.n, lat.n):
        raise ValueError(f"{base}: array is {a.shape}, lattice is {(lat.n, lat.n)}")
    npy = out_dir / f"{base}.npy"
    np.save(npy, a)
    rec = {
        "schema": SCHEMA,
        "stage": stage,
        "name": name,
        "array": npy.name,
        "units": "metres",
        "dtype": "float32",
        "axis_order": ("row-major [y][x]; index 0 is the MINIMUM world coordinate "
                       "on that axis"),
        "sample_convention": ("node: value[j][i] is the surface at world "
                              "(origin_x + i*cell, origin_y + j*cell)"),
        **lat.describe(),
        "seed": seed,
        "tier": tier,
        "provenance": {
            "producer": "dump_stage_heightfields.py",
            "producer_source": "terrain-service/tools/dump_stage_heightfields.py",
            "stage_source": source,
            **provenance,
        },
        "stats": {
            "min_m": float(a.min()),
            "max_m": float(a.max()),
            "mean_m": float(a.mean()),
            **(extra_stats or {}),
        },
    }
    if quantisation:
        rec["quantisation"] = quantisation
    (out_dir / f"{base}.json").write_text(json.dumps(rec, indent=2))
    return {"base": base, "stage": stage, "tier": tier,
            "cell_size_mm": lat.cell_mm, "n": lat.n}


# ---------------------------------------------------------------------------
# S0 -- the wire tile.
# ---------------------------------------------------------------------------

def load_coarse(tiles_dir: Path, tx: int, ty: int, cache: dict) -> np.ndarray | None:
    if (tx, ty) not in cache:
        p = tiles_dir / f"{tx}_{ty}.vxtl"
        if p.exists():
            t = tile_codec.decode(p.read_bytes())
            cache[(tx, ty)] = (t.elevation.astype(np.int16), t.seed)
        else:
            cache[(tx, ty)] = None
    return cache[(tx, ty)]


def sample_s0(tiles_dir: Path, lat: Lattice, seed: int, cache: dict) -> tuple[np.ndarray, dict]:
    """S0 on the 30 m node lattice, in metres, straight off the int16 plane.

    No interpolation of any kind: every value is a stored sample. A node that
    is not covered by a tile on disk is an error, not a zero -- a plane of
    fabricated sea level in the middle of a stage dump is the single most
    misleading thing this tool could produce.
    """
    if lat.cell_mm % tile_codec.PIXEL_SIZE_MM[1] != 0:
        raise ValueError(
            f"S0 lattice cell {lat.cell_mm} mm is not a multiple of the wire pixel "
            f"{tile_codec.PIXEL_SIZE_MM[1]} mm; S0 is stored samples and this tool "
            "will not invent intermediate ones"
        )
    step = lat.cell_mm // tile_codec.PIXEL_SIZE_MM[1]
    px0 = lat.x0_mm // tile_codec.PIXEL_SIZE_MM[1]
    py0 = lat.y0_mm // tile_codec.PIXEL_SIZE_MM[1]
    if lat.x0_mm % tile_codec.PIXEL_SIZE_MM[1] or lat.y0_mm % tile_codec.PIXEL_SIZE_MM[1]:
        raise ValueError("S0 origin must be a whole number of 30 m pixels")

    out = np.zeros((lat.n, lat.n), dtype=np.float32)
    used: set[tuple[int, int]] = set()
    n_tile = tile_codec.TILE_SIZE
    for j in range(lat.n):
        py = py0 + j * step
        ty, ly = divmod(py, n_tile)
        for i in range(lat.n):
            px = px0 + i * step
            tx, lx = divmod(px, n_tile)
            got = load_coarse(tiles_dir, tx, ty, cache)
            if got is None:
                raise SystemExit(
                    f"S0: coarse tile ({tx},{ty}) is not in {tiles_dir}; the footprint "
                    "is not fully covered"
                )
            elev, tseed = got
            if tseed != seed:
                raise SystemExit(
                    f"S0: tile ({tx},{ty}) carries seed {tseed}, --seed is {seed}"
                )
            used.add((tx, ty))
            out[j, i] = elev[ly, lx]
    return out, {"coarse_tiles": sorted(f"{a}_{b}" for a, b in used)}


# ---------------------------------------------------------------------------
# S1 -- the bake output.
# ---------------------------------------------------------------------------

def bake_cache_path(cache_dir: Path, tx: int, ty: int, seed: int) -> Path:
    return cache_dir / f"S1_bake_{tx}_{ty}_seed{seed}.npy"


def run_bake(tiles_dir: Path, tx: int, ty: int, seed: int, cache_dir: Path) -> tuple[Path, dict]:
    """``bake_tile`` for one coarse tile, cached as the full 8192^2 interior.

    This is the same call ``bake_real_tile.py`` makes, superblock included --
    baking without the level-0 hydrology superblock changes the flow
    accumulation, hence the incision, hence the surface, so a no-superblock
    bake is a different S1 than the one the shipped tile came from.
    """
    cache_dir.mkdir(parents=True, exist_ok=True)
    npy = bake_cache_path(cache_dir, tx, ty, seed)
    meta = npy.with_suffix(".json")
    if npy.exists() and meta.exists():
        print(f"  S1: reusing cached bake {npy}")
        return npy, json.loads(meta.read_text())

    geom = bp.PRODUCTION
    geom.assert_production()
    consts = bp.CONSTANTS
    print(f"  S1: baking tile ({tx},{ty}) at {geom.fine_pixel_m} m/px, "
          f"{geom.fine_tile_px}^2 interior + {geom.apron_m:.0f} m apron")
    print(f"      estimate_peak_bytes {bp.estimate_peak_bytes(geom)/2**30:.2f} GiB "
          "-- do NOT run two of these at once")

    fetch = _coarse_fetch(tiles_dir)
    kernels = bp.load_kernels()
    lvl = bp.FlowLevel(0, geom, consts)
    sx, sy = bp.superblock_index(tx, ty, lvl)
    inflow = bp.build_flow_superblock(fetch, sx, sy, lvl, kernels)
    c0 = time.process_time()
    res = bp.bake_tile(world_seed=seed, tile_x=tx, tile_y=ty, coarse_fetch=fetch,
                       kernels=kernels, geom=geom, consts=consts, inflow_source=inflow)
    cpu = time.process_time() - c0
    print(f"      bake {cpu:.1f} s cpu, relief {res.stats['relief_m']:.1f} m")

    np.save(npy, np.ascontiguousarray(res.elevation_m, dtype=np.float32))
    rec = {
        "tile": [tx, ty],
        "seed": seed,
        "bake_version": bp.BAKE_VERSION,
        "bake_fingerprint": bp.bake_fingerprint(geom, consts),
        "geometry": geom.as_payload(),
        "fine_pixel_m": geom.fine_pixel_m,
        "fine_tile_px": geom.fine_tile_px,
        "superblock_fingerprint": res.superblock_fingerprint,
        "superblock_missing_tiles": len(inflow.missing_tiles),
        "missing_coarse": [list(t) for t in res.missing_coarse],
        "cpu_seconds_bake": cpu,
        "cpu_stages": res.cpu_seconds,
        "stats": res.stats,
    }
    meta.write_text(json.dumps(rec, indent=2))
    return npy, rec


def _coarse_fetch(tiles_dir: Path):
    cache: dict[tuple[int, int], np.ndarray | None] = {}

    def fetch(x: int, y: int):
        if (x, y) not in cache:
            p = tiles_dir / f"{x}_{y}.vxtl"
            cache[(x, y)] = (tile_codec.decode(p.read_bytes()).elevation.astype(np.float32)
                             if p.exists() else None)
        return cache[(x, y)]

    return fetch


def sample_s1(bake_npy: Path, tx: int, ty: int, lat: Lattice) -> np.ndarray:
    """Slice the cached 8192^2 bake interior onto the lattice, samples only.

    ``mmap_mode='r'`` because the array is 268 MB and only a window is wanted;
    the point of caching it was to stop paying for the bake, not to start
    paying for the read.
    """
    fine_mm = bp.PRODUCTION.fine_pixel_m * 1000.0
    if abs(fine_mm - round(fine_mm)) > 1e-9:
        raise SystemExit("fine pixel is not a whole number of mm")
    fine_mm = int(round(fine_mm))
    if lat.cell_mm % fine_mm or lat.x0_mm % fine_mm or lat.y0_mm % fine_mm:
        raise SystemExit(
            f"S1 lattice (cell {lat.cell_mm} mm, origin {lat.x0_mm},{lat.y0_mm}) is not "
            f"aligned to the {fine_mm} mm bake grid; S1 is stored samples and this tool "
            "will not invent intermediate ones"
        )
    step = lat.cell_mm // fine_mm
    size = bp.PRODUCTION.fine_tile_px
    i0 = lat.x0_mm // fine_mm - tx * size
    j0 = lat.y0_mm // fine_mm - ty * size
    i1 = i0 + (lat.n - 1) * step
    j1 = j0 + (lat.n - 1) * step
    if i0 < 0 or j0 < 0 or i1 >= size or j1 >= size:
        raise SystemExit(
            f"S1: the footprint reaches fine cells [{i0}..{i1}]x[{j0}..{j1}] of tile "
            f"({tx},{ty}), which is only {size}^2. Choose a footprint inside one tile."
        )
    a = np.load(bake_npy, mmap_mode="r")
    return np.ascontiguousarray(a[j0:j1 + 1:step, i0:i1 + 1:step], dtype=np.float32)


def verify_against_vxtl(fine_dir: Path, tx: int, ty: int, bake_npy: Path) -> dict:
    """Does the shipped .vxtl represent the S1 this run baked?

    At a lattice point the §8 spline weights are (1,4,1)/6 per axis, so
    evaluating the shipped control lattice AT its own nodes is a separable
    [1,4,1]/6 stencil -- which is the operator the encoder's prefilter inverts.
    Its residual against S1 is therefore the end-to-end check of
    ``samples -> prefilter -> quantise -> spline -> samples``, and its floor is
    the 100 mm control-point quantisation. A large residual means the shipped
    tile is not the surface this bake computed, i.e. S1 and S2-fine are not
    describing the same terrain and no comparison between them means anything.
    """
    p = fine_dir / f"{tx}_{ty}.vxtl"
    if not p.exists():
        return {"checked": False, "reason": f"{p} not found"}
    t = tile_codec.decode_v2(p.read_bytes())
    cp_mm = tile_codec.control_points_to_mm(
        t.elevation_cp, t.base_offset_mm, t.quant
    ).astype(np.float64)

    def smooth(a):
        q = np.pad(a, ((1, 1), (0, 0)), mode="edge")
        return (q[:-2] + 4.0 * q[1:-1] + q[2:]) / 6.0

    recon = smooth(smooth(cp_mm).T).T
    s1 = np.load(bake_npy, mmap_mode="r")
    err = recon[1:-1, 1:-1] - np.asarray(s1[1:-1, 1:-1], dtype=np.float64) * 1000.0
    return {
        "checked": True,
        "vxtl": str(p),
        "quant_mm": tile_codec.QUANT_MM[t.quant],
        "interior_mean_abs_mm": float(np.abs(err).mean()),
        "interior_p99_abs_mm": float(np.percentile(np.abs(err), 99)),
        "interior_max_abs_mm": float(np.abs(err).max()),
    }


# ---------------------------------------------------------------------------

def emit_vxtl(bake_npy: Path, tx: int, ty: int, seed: int, dest: Path) -> Path:
    """Encode THIS run's S1 as a .vxtl v2 tile, so S2-fine sits on this S1.

    WHY THIS IS NOT OPTIONAL WHEN THE FINGERPRINTS DISAGREE. S2/S3/S4 on the
    fine tier are evaluated by voxel-core over whatever ``.vxtl`` v2 tile it is
    pointed at. If that tile was baked by a different revision of the pipeline
    than the S1 in this dump, then S1 and S2-fine describe two different
    worlds and every difference between them is a mixture of "what the client's
    carrier does to the bake" and "what the bake did differently that day" --
    which is precisely the confound this whole exercise exists to remove.

    ``--verify-vxtl`` is what detects the disagreement; this is what fixes it.
    Point ``vxc_stagedump --fine-dir`` at the directory this writes.

    NO FLOW PLANE. Only ``elevation_m`` is cached (the flow plane is another
    67 MB and voxel-core reads elevation for every stage here), so the emitted
    tile has flags bit0 clear. That is a legal v2 tile; it is not a substitute
    for the shipped one in any context that wants hydrology.
    """
    dest.mkdir(parents=True, exist_ok=True)
    out = dest / f"{tx}_{ty}.vxtl"
    if out.exists():
        print(f"  S1: reusing emitted fine tile {out}")
        return out
    z = np.load(bake_npy)  # encode_fine needs the whole plane, not a window
    c0 = time.process_time()
    data = tile_codec.encode_fine(seed=seed, x=tx, y=ty, elevation_m=z, flow=None)
    out.write_bytes(data)
    print(f"  S1: emitted {out} ({len(data)/1e6:.1f} MB, CODEC_RAW, no flow plane) "
          f"in {time.process_time()-c0:.1f} s cpu")
    return out


def parse_cell(spec: str) -> tuple[int, int | None]:
    if ":" in spec:
        a, b = spec.split(":", 1)
        return int(a), int(b)
    return int(spec), None


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("USAGE")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--out", required=True, help="output directory (use a gitignored path)")
    ap.add_argument("--tiles-dir", required=True, help="directory of s1 .vxtl v1 tiles")
    ap.add_argument("--seed", type=int, required=True,
                    help="world seed; must match the tiles' own header seed")
    ap.add_argument("--origin", nargs=2, type=int, required=True, metavar=("X", "Y"),
                    help="world metres of the footprint's MINIMUM corner; make it a "
                         "multiple of 30 so the 30 m and 1.875 m lattices share nodes")
    ap.add_argument("--span", type=int, required=True, help="footprint edge, metres")
    ap.add_argument("--cell", action="append", default=None, metavar="MM[:SPAN_M]",
                    help="lattice cell in MILLIMETRES, repeatable, with an optional own "
                         "centred sub-span in metres. Default: 30000 for S0, 1875 for S1")
    ap.add_argument("--bake", action="store_true",
                    help="run the bake if it is not already cached (~150 CPU-s, ~5.5 GiB "
                         "peak RSS per tile -- do NOT run two at once)")
    ap.add_argument("--bake-cache-dir", default=None,
                    help="where the full 8192^2 bake interior is cached (default: --out)")
    ap.add_argument("--verify-vxtl", default=None, metavar="S16_DIR",
                    help="check the shipped .vxtl v2 lattice reproduces this bake's surface")
    ap.add_argument("--emit-vxtl", default=None, metavar="DIR",
                    help="encode THIS run's S1 as a .vxtl v2 tile in DIR and point "
                         "vxc_stagedump --fine-dir at it, so S2/S3/S4-fine sit on the "
                         "same bake as S1. Use this when --verify-vxtl reports a large "
                         "residual against the shipped tile.")
    ap.add_argument("--s0-only", action="store_true", help="skip S1 entirely")
    a = ap.parse_args()

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    tiles_dir = Path(a.tiles_dir)
    cache_dir = Path(a.bake_cache_dir) if a.bake_cache_dir else out

    origin_mm = (a.origin[0] * 1000, a.origin[1] * 1000)
    span_mm = a.span * 1000
    cells = [parse_cell(c) for c in (a.cell or [])]

    # Which coarse tile owns the footprint? S1 is baked per tile, so a footprint
    # that straddles two would need two bakes and a join; refuse rather than
    # produce half a stage.
    span_tile_m = bp.PRODUCTION.tile_span_m
    tx0 = int(np.floor(a.origin[0] / span_tile_m))
    ty0 = int(np.floor(a.origin[1] / span_tile_m))
    tx1 = int(np.floor((a.origin[0] + a.span) / span_tile_m))
    ty1 = int(np.floor((a.origin[1] + a.span) / span_tile_m))

    print(f"footprint: origin ({a.origin[0]}, {a.origin[1]}) m, span {a.span} m, "
          f"seed {a.seed}")
    print(f"  coarse tile(s): ({tx0},{ty0}) .. ({tx1},{ty1})")

    manifest = {"schema": "vxc.stagedump.manifest.v1",
                "producer": "dump_stage_heightfields.py",
                "seed": a.seed,
                "footprint_origin_m": list(a.origin),
                "footprint_span_m": a.span,
                "tiles_dir": str(tiles_dir),
                "files": []}

    # ---- S0 -------------------------------------------------------------
    # S0 and S1 are STORED SAMPLES. A requested cell that is not a whole number
    # of the stage's own pixels is dropped with a message rather than served by
    # interpolating -- an invented intermediate sample in a stage dump would be
    # indistinguishable from the stage having produced it.
    coarse_cache: dict = {}
    wire_mm = tile_codec.PIXEL_SIZE_MM[1]
    for cell_mm, _own in cells:
        if cell_mm % wire_mm and cell_mm % int(round(bp.PRODUCTION.fine_pixel_m * 1000)):
            print(f"  note: --cell {cell_mm} is a multiple of neither the {wire_mm} mm wire "
                  f"pixel nor the {int(round(bp.PRODUCTION.fine_pixel_m*1000))} mm bake cell; "
                  "no server-side stage can answer it and it is skipped")
    s0_cells = [c for c in cells if c[0] % wire_mm == 0] or [(wire_mm, None)]
    for cell_mm, own in s0_cells:
        lat = Lattice(cell_mm, origin_mm, span_mm, own if own is None else own * 1000)
        vals, prov = sample_s0(tiles_dir, lat, a.seed, coarse_cache)
        base = f"S0_wire_{cell_mm}mm"
        manifest["files"].append(write_stage(
            out, base, vals, lat,
            stage="S0", tier="wire",
            name="raw terrain-diffusion tile as it arrives on the wire",
            seed=a.seed,
            source="tile_codec.decode(.vxtl v1).elevation -- int16 WHOLE METRES, "
                   "no interpolation",
            provenance={"tiles_dir": str(tiles_dir), "wire_dtype": "int16",
                        "wire_lsb_m": 1.0, **prov},
            quantisation="int16 whole metres on the wire; widened to float32 here "
                         "without rescaling",
        ))
        print(f"  S0 {base}: {lat.n}^2 @ {cell_mm/1000:g} m, "
              f"{vals.min():.0f}..{vals.max():.0f} m")

    if a.s0_only:
        (out / "manifest_server.json").write_text(json.dumps(manifest, indent=2))
        print(f"\nwrote {len(manifest['files'])} heightfield(s) + sidecars to {out}")
        return 0

    # ---- S1 -------------------------------------------------------------
    if (tx0, ty0) != (tx1, ty1):
        raise SystemExit(
            f"the footprint straddles coarse tiles ({tx0},{ty0})..({tx1},{ty1}); S1 is "
            "baked one tile at a time, so choose a footprint inside a single tile"
        )
    npy = bake_cache_path(cache_dir, tx0, ty0, a.seed)
    if not (npy.exists() and npy.with_suffix(".json").exists()) and not a.bake:
        raise SystemExit(
            f"no cached bake at {npy}. Pass --bake to produce it (~150 CPU-s, "
            "~5.5 GiB peak RSS -- do NOT run two at once), or --s0-only."
        )
    npy, bake_meta = run_bake(tiles_dir, tx0, ty0, a.seed, cache_dir)

    verify = {"checked": False, "reason": "--verify-vxtl not given"}
    if a.verify_vxtl:
        verify = verify_against_vxtl(Path(a.verify_vxtl), tx0, ty0, npy)
        if verify.get("checked"):
            print(f"  S1 vs shipped .vxtl lattice: mean |e| "
                  f"{verify['interior_mean_abs_mm']:.2f} mm, max "
                  f"{verify['interior_max_abs_mm']:.2f} mm "
                  f"(quant {verify['quant_mm']} mm floor)")
        else:
            print(f"  S1 vs shipped .vxtl: NOT CHECKED ({verify['reason']})")

    emitted = None
    if a.emit_vxtl:
        emitted = emit_vxtl(npy, tx0, ty0, a.seed, Path(a.emit_vxtl))

    fine_mm = int(round(bp.PRODUCTION.fine_pixel_m * 1000))
    s1_cells = [c for c in cells if c[0] % fine_mm == 0] or [(fine_mm, None)]
    for cell_mm, own in s1_cells:
        lat = Lattice(cell_mm, origin_mm, span_mm, own if own is None else own * 1000)
        vals = sample_s1(npy, tx0, ty0, lat)
        base = f"S1_bake_{cell_mm}mm"
        manifest["files"].append(write_stage(
            out, base, vals, lat,
            stage="S1", tier="bake",
            name="bake output after carrier + roughness + flow + incision + thermal",
            seed=a.seed,
            source="terrain_service.bake.pipeline.bake_tile(...).elevation_m -- SAMPLES, "
                   "not the prefiltered control lattice the .vxtl ships",
            provenance={"bake_array": str(npy), "vxtl_reconstruction_check": verify,
                        "emitted_vxtl": str(emitted) if emitted else "",
                        **bake_meta},
        ))
        print(f"  S1 {base}: {lat.n}^2 @ {cell_mm/1000:g} m, "
              f"{vals.min():.1f}..{vals.max():.1f} m")

    (out / "manifest_server.json").write_text(json.dumps(manifest, indent=2))
    print(f"\nwrote {len(manifest['files'])} heightfield(s) + sidecars to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
