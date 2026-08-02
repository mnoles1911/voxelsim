"""Bake ONE real 30 m diffusion tile end-to-end and measure what it actually cost.

Everything in `docs/terrain-amplification-plan.md` about fine-tier size, bake cost
and peak memory was modelled or measured on prototypes; until 2026-07-29 no fine
tile had ever been produced end-to-end, because there was no code path from a
`BakeResult` (SAMPLES) to `.vxtl` v2 bytes (a prefiltered CONTROL LATTICE). That
path is `tile_codec.encode_fine`; this tool is the thing that drives it on real
model output and writes down the numbers.

    bake_real_tile.py --tiles-dir <s1 dir> --tile X Y --out-dir DIR [--json f.json]

WHAT IS MEASURED, AND WHY IN THESE UNITS
----------------------------------------
* **CPU, not wall.** `time.process_time()`, exactly as `bake/pipeline.py`
  insists: five other sessions share this box and wall-clock here reads like a
  slow configuration. Wall is recorded too, but only so the ratio shows how much
  of the CPU total is the parallel kernels doing real work on 4 threads.
* **Peak memory is asked of the OS**, not counted from array shapes. Windows
  `GetProcessMemoryInfo` gives PeakWorkingSetSize (resident) and
  PeakPagefileUsage (private commit); both are process-lifetime maxima, so this
  tool bakes exactly ONE tile per process and the number is not contaminated by
  a previous tile's garbage. `pipeline.estimate_peak_bytes` is printed next to
  them -- it counts 9 named grids and nothing else, so the gap between them is
  the kernels' own transients.
* **Size is the encoded byte count**, twice: `CODEC_RAW`, which is what the
  files on disk are (voxel-core decodes only RAW -- it vendors no zstd), and the
  same block payloads re-compressed, which is the number the plan's ~8 MB/tile
  estimate should be read against. Re-compressing the payloads is exact rather
  than indicative: `codec` in v2 applies per block payload, so a CODEC_ZSTD file
  differs from the RAW one in precisely those bytes.

THE SHRINK SWITCH
-----------------
`--coarse-tile-px` bakes a smaller interior at the SAME 1.875 m/px and the SAME
960 m apron, by subdividing each 512 px source tile into a world-consistent grid
of sub-tiles. It exists so a box that cannot afford the production domain can
still produce real bytes -- and every report this tool prints names the geometry
it actually ran, because "measured at 1/16 the area and quoted as production" is
the failure mode the plan's own numbers already suffered from.

FIRST REAL RESULTS (2026-07-29, full 8192^2 + 960 m apron, CPU only)
--------------------------------------------------------------------
Three real diffusion tiles, each with a complete 3x3 coarse ring, seed
0x135276f, bake fingerprint 475fab2f588004c6, stream_K 0.15, a_crit 1e4 m^2.
zstd-19 over the actual block payloads; the `.vxtl` on disk is CODEC_RAW.

    tile              relief   bake cpu   peak WS   zstd MB   KB/km^2   elev-only
    (-5,2) alpine     1571 m    150.6 s   5.54 GiB    22.62      93.6      16.85
    (-5,3) mixed      2862 m    144.2 s   5.54 GiB    23.13      95.7      16.54
    (-7,4) OCEAN      2257 m    135.7 s   5.53 GiB    28.35     117.3      19.44

Against the estimates on record:

    quantity        plan says   vxtl-v2 spec says   MEASURED       verdict
    size/tile        ~8 MB        21-25 MB          22.6-28.4 MB   spec right
    KB/km^2          ~34          ~106              93.6-117.3     spec right
    bake cpu/tile    ~1.5 s       ~165 CPU-s        136-173 CPU-s  spec right
    peak memory      ~0.5 GB      ~3 GB (counted)   5.5-5.9 GB     BOTH LOW

The plan's 8 MB / 34 KB/km^2 was measured on the 4096^2 / 3.75 m tier
(`measure_fine_tier_size.py` is still hard-coded to SCALE=8), and the shipped
tier is 8192^2 / 1.875 m over the SAME ground -- 4x the samples. It is not a
missed estimate, it is a superseded one; `vxtl-v2-format.md` already replaced it
and the replacement is accurate.

Peak memory is the one nobody has right. `estimate_peak_bytes` counts 9 named
grids and says 3.16 GiB; the OS says 5.54 GiB resident / 5.70 GiB committed,
1.75x more, and the excess is the kernels' own transients (the priority flood's
queue, MFD's ordering, float64 accumulation). Size a bake pod on 8 GiB, not 4.

The bake is deterministic: tile (-5,2) baked twice on a contended box produced a
byte-identical control-point plane (sha256 bcf53bfd...). Its CPU total moved
150.6 -> 172.9 s between the two runs, which is worth knowing -- CPU-seconds are
immune to STOLEN wall time but not to memory-bandwidth contention, so +-15% is
the resolution of this measurement even in the contention-robust unit.

THE OCEAN CASE IS THE OPPOSITE OF WHAT THE FORMAT ASSUMES
---------------------------------------------------------
`vxtl-v2-format.md` §4 on CONSTANT blocks: "Zero data bytes. Common: ocean, flat
basin." On tile (-7,4), which is 100% ocean by every measure `rank_tiles.py`
has (0.0% land, -4654 m to -2405 m, p95 grade 0.0% at 30 m):

    CONSTANT blocks: 0 of 1024.
    flattest 256x256 block spans 14.3 m of relief; the median spans 56.7 m.
    the tile is the LARGEST of the three at 28.35 MB, not ~0.1 MB.

Two independent reasons, and neither is a bug in the codec:

1. **This "ocean" is bathymetry, not a water plane.** The diffusion model emits
   real seafloor with 2.26 km of relief in one tile; adjacent control points
   120 m apart differ by 7.4 m at the median. A CONSTANT block needs 65,536
   control points equal to within 100 mm across 480 m. Dropping to `quant`
   250 mm does not rescue it -- the flattest block would still span 57 LSB.
2. **The bake erodes below sea level.** Nothing in the pipeline gates on z < 0,
   so priority-flood, MFD, stream power and thermal relaxation all run on the
   seafloor. The result on (-7,4) is 26.6 M cells flagged as channel (39.7% of
   the tile, against 4.1% on the alpine tile) and 0.87 m of MEAN incision
   (against 0.13 m alpine) -- subaerial fluvial erosion at 3 km depth. That
   invented high-frequency detail is why the ocean tile costs MORE than the
   mountains: 19.44 MB of elevation against 16.85, and 8.88 MB of flow plane
   against 5.75.

The second is a bake-physics question, not a codec one: a sea-level gate would
roll `BakeConstants`, hence the fingerprint, hence provider_id and the world, so
it is deliberately NOT done here. But it is the single largest storage lever
found so far -- an ocean-majority world currently pays full price per tile for
drainage networks no player can reach.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec  # noqa: E402
from terrain_service.bake import pipeline as bp  # noqa: E402

SRC_TILE_PX = 512


# ---------------------------------------------------------------------------
# Memory, asked of the OS.
# ---------------------------------------------------------------------------

class _PROCESS_MEMORY_COUNTERS(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.c_uint32),
        ("PageFaultCount", ctypes.c_uint32),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


def numba_threads() -> str:
    """What the parallel kernels ACTUALLY got, recorded next to the timings.

    Nothing in this repo sets ``NUMBA_NUM_THREADS`` or calls
    ``numba.set_num_threads``, so this is whatever numba defaulted to -- on the
    measuring box, all 12 LOGICAL processors of a 6-core part. Every bake
    timing on record was taken at an unrecorded thread count; that is exactly
    the ambiguity this line closes. (``OMP_NUM_THREADS``, which
    ``gen_world_tiles2.py`` does set, does NOT cap numba: numba calls
    ``omp_set_num_threads`` itself. Verified.)
    """
    try:
        import numba

        return f"{numba.get_num_threads()} (max {numba.config.NUMBA_NUM_THREADS})"
    except Exception:  # pragma: no cover - numba is optional here
        return "n/a"


def memory_counters() -> dict:
    """Resident/committed bytes for THIS process, current and peak.

    psutil reports the current working set only; the peak is the quantity that
    decides whether a pod is big enough, and sampling for it from Python would
    miss a transient inside a numba kernel that never yields.
    """
    if os.name != "nt":  # pragma: no cover - this box is Windows
        import resource

        rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024
        return {"peak_working_set": rss, "working_set": rss,
                "peak_commit": rss, "commit": rss}
    c = _PROCESS_MEMORY_COUNTERS()
    c.cb = ctypes.sizeof(c)
    fn = ctypes.windll.psapi.GetProcessMemoryInfo
    fn.argtypes = [ctypes.c_void_p, ctypes.POINTER(_PROCESS_MEMORY_COUNTERS),
                   ctypes.c_uint32]
    fn.restype = ctypes.c_int
    # The current-process pseudo-handle is (HANDLE)-1. Spelling it out avoids
    # GetCurrentProcess()'s default c_int restype truncating a 64-bit handle,
    # which is what made the first version of this fail with a bare error code.
    if not fn(ctypes.c_void_p(-1), ctypes.byref(c), c.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return {
        "peak_working_set": int(c.PeakWorkingSetSize),
        "working_set": int(c.WorkingSetSize),
        "peak_commit": int(c.PeakPagefileUsage),
        "commit": int(c.PagefileUsage),
    }


# ---------------------------------------------------------------------------
# Coarse source.
# ---------------------------------------------------------------------------

def make_coarse_fetch(tiles_dir: Path, coarse_tile_px: int):
    """`coarse_fetch(x, y)` over a directory of v1 `.vxtl` tiles.

    With `coarse_tile_px == 512` this is a plain file lookup. Below it, each
    source tile is subdivided into `(512 / coarse_tile_px)^2` sub-tiles on a
    WORLD-anchored grid, so a shrunk run still bakes real adjacent data with a
    real apron -- the geometry changes, the terrain does not become synthetic.
    """
    if SRC_TILE_PX % coarse_tile_px:
        raise ValueError(
            f"--coarse-tile-px {coarse_tile_px} must divide {SRC_TILE_PX}"
        )
    per = SRC_TILE_PX // coarse_tile_px
    cache: dict[tuple[int, int], tuple[np.ndarray, np.ndarray] | None] = {}

    def load(fx: int, fy: int):
        if (fx, fy) not in cache:
            p = tiles_dir / f"{fx}_{fy}.vxtl"
            if p.exists():
                t = tile_codec.decode(p.read_bytes())
                # Climate is kept, not discarded. bake_ver 7's province
                # partition needs it; without it prov.temp_c is None, the
                # partition silently falls back to relief only, and every
                # tile comes out 100% FLUVIAL. That is not hypothetical --
                # it is what this tool reported for tile (-3,-3), a tile
                # that is 98.9% arid.
                cache[(fx, fy)] = (t.elevation.astype(np.float32), t.climate)
            else:
                cache[(fx, fy)] = None
        return cache[(fx, fy)]

    def _sub(x: int, y: int):
        fx, sx = divmod(x, per)
        fy, sy = divmod(y, per)
        return load(fx, fy), sx, sy

    def fetch(x: int, y: int):
        src, sx, sy = _sub(x, y)
        if src is None:
            return None
        n = coarse_tile_px
        return np.ascontiguousarray(src[0][sy * n:(sy + 1) * n, sx * n:(sx + 1) * n])

    def fetch_climate(x: int, y: int):
        """The ``(4, n, n)`` uint8 climate planes for the province partition.

        Subdivided on the same world-anchored grid as elevation -- the climate
        plane is (4, 512, 512), i.e. the same spatial grid with a leading
        channel axis, so the slice is identical with one extra leading colon.
        """
        src, sx, sy = _sub(x, y)
        if src is None:
            return None
        n = coarse_tile_px
        return np.ascontiguousarray(src[1][:, sy * n:(sy + 1) * n, sx * n:(sx + 1) * n])

    return fetch, fetch_climate


# ---------------------------------------------------------------------------
# Size accounting.
# ---------------------------------------------------------------------------

def _sections(data: bytes) -> dict[int, tuple[int, int]]:
    off = tile_codec._HEADER.size
    (_bl, _pred, _q, _codec, _bv, _flags, _base, _ps, _res,
     n_sections) = tile_codec._V2_EXT.unpack_from(data, off)
    off += tile_codec._V2_EXT.size
    out = {}
    for i in range(n_sections):
        sid, soff, slen = tile_codec._SECTION_ENTRY.unpack_from(
            data, off + i * tile_codec._SECTION_ENTRY.size
        )
        out[sid] = (soff, slen)
    return out


def plane_report(data: bytes, index_id: int, data_id: int, *, lzma_too: bool = False) -> dict:
    """Block-mode histogram plus what the payloads cost under a real entropy coder.

    Walks the block index with `struct` rather than calling `decode_v2`: the
    reference decoder's MED inverse is a per-pixel Python loop, which on a
    production tile is 67 M iterations per plane and would dominate every timing
    in this file. Nothing here needs the reconstructed pixels -- the C++ decoder
    is what proves those, in `verify_fine_tile.py`.
    """
    sec = _sections(data)
    if index_id not in sec:
        return {}
    ioff, ilen = sec[index_id]
    doff, _dlen = sec[data_id]
    n = ilen // tile_codec._BLOCK_ENTRY.size
    modes = {0: 0, 1: 0, 2: 0}
    resid32 = 0
    raw_payload = 0
    zl = xz = zs = 0
    zctx = None
    try:
        import zstandard

        zctx = zstandard.ZstdCompressor(level=19)
    except ImportError:
        pass
    import zlib

    if lzma_too:
        import lzma
    for i in range(n):
        off, comp_len, mode, _cp, rb, _pad = tile_codec._BLOCK_ENTRY.unpack_from(
            data, ioff + i * tile_codec._BLOCK_ENTRY.size
        )
        modes[mode] = modes.get(mode, 0) + 1
        if rb == 32:
            resid32 += 1
        if comp_len:
            payload = data[doff + off:doff + off + comp_len]
            raw_payload += comp_len
            zl += len(zlib.compress(payload, 9))
            if lzma_too:
                xz += len(lzma.compress(payload, preset=6))
            if zctx is not None:
                zs += len(zctx.compress(payload))
    return {
        "blocks": n,
        "constant": modes.get(0, 0),
        "coded": modes.get(1, 0),
        "raw": modes.get(2, 0),
        "resid_bits_32": resid32,
        "payload_bytes_raw": raw_payload,
        "payload_bytes_zlib": zl,
        "payload_bytes_lzma": xz if lzma_too else None,
        "payload_bytes_zstd": zs if zctx is not None else None,
    }


def size_report(encoded: bytes, km2: float, *, lzma_too: bool = False):
    """Print (and return) what the container costs, per plane and per km2.

    The `.vxtl` on disk is CODEC_RAW because that is the only codec voxel-core
    decodes, and RAW means "2 bytes per elevation pixel" -- a number nothing in
    the plan is comparable to. The re-compressed columns are the comparable ones:
    `codec` applies PER BLOCK PAYLOAD, so compressing exactly these payloads is
    what a CODEC_ZSTD file would contain, not an approximation of one.
    """
    elev = plane_report(encoded, tile_codec.SECTION_ELEV_INDEX,
                        tile_codec.SECTION_ELEV_DATA, lzma_too=lzma_too)
    flow = plane_report(encoded, tile_codec.SECTION_FLOW_INDEX,
                        tile_codec.SECTION_FLOW_DATA, lzma_too=lzma_too)
    for name, rep in (("elev", elev), ("flow", flow)):
        if not rep:
            continue
        print(f"    {name}: {rep['blocks']} blocks  "
              f"CONSTANT {rep['constant']}  CODED {rep['coded']}  RAW {rep['raw']}"
              f"  (resid32 {rep['resid_bits_32']})")
        cols = [f"raw {rep['payload_bytes_raw']/1e6:8.2f}",
                f"zlib {rep['payload_bytes_zlib']/1e6:6.2f}"]
        if rep["payload_bytes_lzma"] is not None:
            cols.append(f"lzma {rep['payload_bytes_lzma']/1e6:6.2f}")
        if rep["payload_bytes_zstd"] is not None:
            cols.append(f"zstd-19 {rep['payload_bytes_zstd']/1e6:6.2f}")
        print("      payloads " + " | ".join(cols) + "  MB")

    def total(key):
        return (elev.get(key) or 0) + (flow.get(key) or 0)

    overhead = len(encoded) - total("payload_bytes_raw")
    sizes = {"raw": len(encoded), "zlib": overhead + total("payload_bytes_zlib")}
    if elev.get("payload_bytes_lzma") is not None:
        sizes["lzma"] = overhead + total("payload_bytes_lzma")
    if elev.get("payload_bytes_zstd") is not None:
        sizes["zstd"] = overhead + total("payload_bytes_zstd")
    # Elevation alone, because the flow plane is optional (flags bit0) and the
    # plan's per-tile budget predates it: drop both flow sections and their two
    # section-table entries, and entropy-code what is left.
    if elev.get("payload_bytes_zstd") is not None and flow:
        sizes["zstd_elev_only"] = (
            len(encoded)
            - 2 * tile_codec._SECTION_ENTRY.size
            - flow["blocks"] * tile_codec._BLOCK_ENTRY.size
            - flow["payload_bytes_raw"]
            - elev["payload_bytes_raw"]
            + elev["payload_bytes_zstd"]
        )
    print(f"\n    container over {km2:.1f} km2:")
    for k, v in sizes.items():
        print(f"      {k:<14} {v/1e6:8.2f} MB   {v/1024/km2:8.1f} KB/km2")
    return elev, flow, sizes


def lattice_reconstruction_error_mm(cp: np.ndarray, base_offset_mm: int,
                                    quant_mm: int, samples_m: np.ndarray) -> dict:
    """How far the SHIPPED lattice reproduces the bake's own surface, in mm.

    At a lattice point the §8 weights are `(1, 4, 1, 0) / 6` on each axis, so
    evaluating the spline there is a separable [1,4,1]/6 stencil over the control
    points -- which is exactly the operator the prefilter inverts. This is
    therefore the end-to-end check of the one step that has no test anywhere else:
    samples -> prefilter -> quantise -> (client) spline -> samples again.

    The floor is quantisation. The stencil is a convex combination of three
    control points each rounded to +-quant/2, so the reconstruction error cannot
    beat that and any LARGE error means the prefilter is not the spline's inverse
    -- i.e. the client would render something the bake never computed.

    Edge columns are reported separately because a tile's outermost control point
    has no neighbour HERE; in the shipped world it has one, in the abutting tile.
    """
    cp_mm = base_offset_mm + cp.astype(np.float64) * quant_mm

    def smooth(a):
        p = np.pad(a, ((1, 1), (0, 0)), mode="edge")
        return (p[:-2] + 4.0 * p[1:-1] + p[2:]) / 6.0

    recon = smooth(smooth(cp_mm).T).T
    err = recon - samples_m.astype(np.float64) * 1000.0
    inner = err[1:-1, 1:-1]
    return {
        "quant_mm": quant_mm,
        "interior_mean_abs_mm": float(np.abs(inner).mean()),
        "interior_p99_abs_mm": float(np.percentile(np.abs(inner), 99)),
        "interior_max_abs_mm": float(np.abs(inner).max()),
        "border_max_abs_mm": float(
            max(np.abs(err[0]).max(), np.abs(err[-1]).max(),
                np.abs(err[:, 0]).max(), np.abs(err[:, -1]).max())
        ),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--report-existing",
                    help="skip the bake and only re-do the size accounting for an "
                         "already-written .vxtl v2 file")
    ap.add_argument("--lzma", action="store_true",
                    help="also measure LZMA. Off by default: at 8192^2 it is "
                         "minutes of CPU for a bracket zstd already brackets.")
    ap.add_argument("--tiles-dir")
    ap.add_argument("--tile", nargs=2, type=int, metavar=("X", "Y"))
    ap.add_argument("--out-dir")
    ap.add_argument("--json")
    ap.add_argument("--seed", type=int, default=0x135276F,
                    help="world seed; the cached tiles' own seed by default")
    ap.add_argument("--coarse-tile-px", type=int, default=512,
                    help="interior edge in COARSE px (512 = production). The apron "
                         "stays 32 coarse px = 960 m either way.")
    ap.add_argument("--no-flow", action="store_true",
                    help="omit the optional flow plane from the container")
    ap.add_argument("--no-superblock", action="store_true",
                    help="skip the level-0 hydrology superblock, i.e. bake with NO "
                         "cross-tile inflow (only correct for a test)")
    ap.add_argument("--npz", help="also dump the raw bake arrays here")
    ap.add_argument("--cp-bin",
                    help="dump the int16 control-point plane the encoder fed to "
                         "encode_v2, as VXCP + i32 size + i16[size^2] LE. This is "
                         "the ONLY honest reference for the C++ decode check: it is "
                         "the encoder's input, so agreeing with it proves the bytes "
                         "round-trip across languages rather than merely self-decode.")
    a = ap.parse_args()

    if a.report_existing:
        data = Path(a.report_existing).read_bytes()
        _m, _v, _s, tx, ty, _sc, size = tile_codec._HEADER.unpack_from(data, 0)
        km2 = (size * 30.0 / bp.PRODUCTION.scale / 1000.0) ** 2
        print(f"{a.report_existing}: tile ({tx},{ty}), {size}^2, {km2:.1f} km2")
        elev, flow, sizes = size_report(data, km2, lzma_too=a.lzma)
        if a.json:
            Path(a.json).write_text(json.dumps(
                {"tile": [tx, ty], "size": size, "km2": km2, "bytes": sizes,
                 "elev_plane": elev, "flow_plane": flow}, indent=2))
        return 0
    if not (a.tiles_dir and a.tile and a.out_dir):
        ap.error("--tiles-dir, --tile and --out-dir are required unless "
                 "--report-existing is given")

    tiles_dir = Path(a.tiles_dir)
    out_dir = Path(a.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    tx, ty = a.tile

    geom = bp.BakeGeometry(coarse_tile_px=a.coarse_tile_px)
    production = (a.coarse_tile_px == 512)
    consts = bp.CONSTANTS
    fetch, fetch_climate = make_coarse_fetch(tiles_dir, a.coarse_tile_px)

    print(f"tile ({tx},{ty})  seed 0x{a.seed:x}")
    print(f"  geometry: interior {geom.fine_tile_px}^2 @ {geom.fine_pixel_m} m/px "
          f"({geom.tile_span_m/1000:.2f} km), padded {geom.padded_fine_px}^2, "
          f"apron {geom.apron_m:.0f} m"
          + ("  [PRODUCTION]" if production else "  [SHRUNK -- not production]"))
    print(f"  bake fingerprint {bp.bake_fingerprint(geom, consts)[:16]}  "
          f"stream_K {consts.stream_K}  a_crit {consts.channel_init_area_m2:g} m2")
    print(f"  estimate_peak_bytes {bp.estimate_peak_bytes(geom)/2**30:.2f} GiB")

    kernels = bp.load_kernels()

    inflow_source = None
    cpu_sb = 0.0
    if not a.no_superblock:
        lvl = bp.FlowLevel(0, geom, consts)
        sx, sy = bp.superblock_index(tx, ty, lvl)
        c0, w0 = time.process_time(), time.perf_counter()
        inflow_source = bp.build_flow_superblock(fetch, sx, sy, lvl, kernels)
        cpu_sb = time.process_time() - c0
        print(f"  level-0 superblock ({sx},{sy}) {inflow_source.size_px}^2 @ "
              f"{inflow_source.cell_m:g} m, {len(inflow_source.missing_tiles)} "
              f"missing coarse tiles, {cpu_sb:.1f} s cpu / "
              f"{time.perf_counter()-w0:.1f} s wall")

    mem_pre = memory_counters()
    c0, w0 = time.process_time(), time.perf_counter()
    result = bp.bake_tile(
        world_seed=a.seed, tile_x=tx, tile_y=ty, coarse_fetch=fetch,
        # Without this the province partition has no climate, falls back
        # to relief alone, and reports 100% FLUVIAL on every tile --
        # including one that is 98.9% arid. See make_coarse_fetch.
        climate_fetch=fetch_climate,
        kernels=kernels, geom=geom, consts=consts, inflow_source=inflow_source,
    )
    cpu_bake = time.process_time() - c0
    wall_bake = time.perf_counter() - w0
    mem_bake = memory_counters()

    print(f"\n  BAKE  {cpu_bake:.1f} s cpu   {wall_bake:.1f} s wall "
          f"({cpu_bake/max(wall_bake,1e-9):.1f}x parallel)")
    # PER-STAGE cpu/wall. The bake-wide ratio above is an AMDAHL number -- only
    # B2b and B3 are numba parallel=True and they are ~58% of the CPU total, so
    # it reads ~2x even when those two are scaling at 7-9x. Reading it as the
    # kernels' scaling factor is a mistake this column makes impossible.
    print(f"    {'stage':<22} {'cpu s':>8} {'wall s':>8} {'cpu/wall':>9}")
    for k, v in result.cpu_seconds.items():
        w = result.wall_seconds.get(k)
        if w is None:
            print(f"    {k:<22} {v:8.2f}")
        else:
            print(f"    {k:<22} {v:8.2f} {w:8.2f} {v/max(w,1e-9):8.2f}x")
    print(f"    numba threads {numba_threads()}")
    print(f"  peak working set {mem_bake['peak_working_set']/2**30:.2f} GiB   "
          f"peak commit {mem_bake['peak_commit']/2**30:.2f} GiB   "
          f"(before bake: {mem_pre['peak_working_set']/2**30:.2f} GiB)")
    for k, v in sorted(result.stats.items()):
        print(f"    {k:<24} {v:,.4f}")
    if result.missing_coarse:
        print(f"    missing coarse ring tiles: {list(result.missing_coarse)}")

    if a.npz:
        np.savez_compressed(a.npz, elevation_m=result.elevation_m, flow=result.flow)

    # ---- encode ----------------------------------------------------------
    c0, w0 = time.process_time(), time.perf_counter()
    encoded = tile_codec.encode_fine(
        seed=a.seed, x=tx, y=ty,
        elevation_m=result.elevation_m,
        flow=None if a.no_flow else result.flow,
    )
    cpu_enc = time.process_time() - c0
    wall_enc = time.perf_counter() - w0
    mem_enc = memory_counters()

    # Outside the timed region on purpose: this repeats the prefilter, and the
    # encode cost being reported is `encode_fine`'s, not this tool's.
    cp, cp_base_mm, cp_quant = tile_codec.elevation_control_points(result.elevation_m)
    recon = lattice_reconstruction_error_mm(
        cp, cp_base_mm, tile_codec.QUANT_MM[cp_quant], result.elevation_m
    )
    if a.cp_bin:
        with open(a.cp_bin, "wb") as f:
            f.write(b"VXCP" + struct.pack("<i", int(cp.shape[0])))
            f.write(np.ascontiguousarray(cp, dtype="<i2").tobytes())
        print(f"    control points -> {a.cp_bin}")
    del cp

    (_bl, _pred, quant, codec, bake_ver, flags, base_mm, _ps, _res,
     _ns) = tile_codec._V2_EXT.unpack_from(encoded, tile_codec._HEADER.size)

    path = out_dir / f"{tx}_{ty}.vxtl"
    path.write_bytes(encoded)

    km2 = (geom.fine_tile_px * geom.fine_pixel_m / 1000.0) ** 2
    print(f"\n  ENCODE  {cpu_enc:.1f} s cpu   {wall_enc:.1f} s wall")
    print(f"    wrote {path}  ({len(encoded)/1e6:.2f} MB, CODEC_RAW)")
    print(f"    datum {base_mm/1000:.1f} m   quant {tile_codec.QUANT_MM[quant]} mm   "
          f"bake_ver {bake_ver}   flags {flags}   codec {codec}")
    print(f"    lattice -> spline -> samples, vs the bake's own surface: "
          f"mean |e| {recon['interior_mean_abs_mm']:.2f} mm, p99 "
          f"{recon['interior_p99_abs_mm']:.2f} mm, max "
          f"{recon['interior_max_abs_mm']:.2f} mm (quant {recon['quant_mm']} mm, "
          f"border max {recon['border_max_abs_mm']:.1f} mm)")
    elev, flow, sizes = size_report(encoded, km2, lzma_too=a.lzma)

    rec = {
        "tile": [tx, ty], "seed": a.seed,
        "production_geometry": production,
        "geometry": geom.as_payload(),
        "fine_tile_px": geom.fine_tile_px, "km2": km2,
        "fingerprint": bp.bake_fingerprint(geom, consts),
        "cpu_seconds_bake": cpu_bake, "wall_seconds_bake": wall_bake,
        "cpu_seconds_superblock": cpu_sb,
        "cpu_seconds_encode": cpu_enc, "wall_seconds_encode": wall_enc,
        "cpu_stages": result.cpu_seconds,
        "wall_stages": result.wall_seconds,
        "numba_threads": numba_threads(),
        "mem_before": mem_pre, "mem_after_bake": mem_bake, "mem_after_encode": mem_enc,
        "estimate_peak_bytes": bp.estimate_peak_bytes(geom),
        "stats": result.stats,
        "missing_coarse": [list(t) for t in result.missing_coarse],
        "bytes": sizes, "elev_plane": elev, "flow_plane": flow,
        "lattice_reconstruction_mm": recon,
        "base_offset_mm": base_mm, "quant_mm": tile_codec.QUANT_MM[quant],
        "path": str(path),
    }
    if a.json:
        Path(a.json).write_text(json.dumps(rec, indent=2))
        print(f"\n  wrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
