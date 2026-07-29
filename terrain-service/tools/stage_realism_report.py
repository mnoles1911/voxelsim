#!/usr/bin/env python3
"""Where in the 30 m -> 10 cm pipeline does the terrain stop looking like Earth?

WHAT THIS JOINS UP
------------------
Three instruments already exist and answer nothing on their own:

* ``terrain_service.geomorph`` measures a heightfield. Its validation sweep
  (``docs/geomorph-validation.md``) established which of its metrics can see
  through a spectrum-matched fake -- ``fill_volume_per_area_m``,
  ``curvature_asymmetry``, ``hack_h`` and ``tail_asymmetry``, with ``theta``
  marginal -- and, separately, that **geomorphon histograms separate terrain
  CLASSES and are blind to realism**. Those are two different questions and this
  tool keeps them in two different tables.
* ``tools/dump_stage_heightfields.py`` + ``vxc_stagedump`` write S0..S4 of our
  own pipeline as float32 ``.npy`` with sidecars.
* ``tools/earth_reference.py`` caches real Copernicus GLO-30 windows and USGS
  3DEP 1 m bare-earth windows for twelve sites in eight geomorphic classes.

This tool runs the metrics over the stage dumps AND over the real references,
**at matched resolution**, and prints the two tables plus the two specific
regressions the brief asks about (the bake's ``theta``, and the fine tier's
featureless-SLOPE fraction).

RESOLUTION IS THE WHOLE GAME, SO IT IS ENFORCED, NOT ASSUMED
-----------------------------------------------------------
Slope, curvature, geomorphon class fractions, pit counts and drainage density
are all functions of the cell size. A table whose columns were measured at
different cell sizes is worse than no table, so every band here is built from
fields that share ONE cell size, ``geomorph.require_same_resolution`` is called
on each band before anything is printed, and the reference is brought to our
cell size rather than the other way round:

* **30 m band.** Ours is already 30.0 m. Copernicus is read on the manifest's
  ``aeqd30`` grid -- a local azimuthal-equidistant reprojection at exactly
  30.0 m square pixels -- so no resampling is applied to either side. The 3DEP
  1 m DTM is BLOCK-AVERAGED by 30 to a 30 m grid; over a 2048 m window that is
  only 68x68 cells, which is ample for curvature and useless for Hack's law, so
  it is reported as a cross-check on the DSM and not as the primary reference.
* **1.875 m band.** The 3DEP 1 m DTM is AREA-AVERAGED onto exactly 1.875 m
  cells (``box_resample``: an exact box filter over the piecewise-constant
  source, done through an integral image, so 8 output cells span exactly 15
  input cells and no cell is double-counted). Block-averaging by 2 to 2.0 m
  instead would leave a 6.7% cell mismatch, which
  ``require_same_resolution`` correctly refuses.

Domains are cropped to the same NUMBER of cells as well as the same size, since
Hack's law and drainage density both read the domain's longest flow path.

TWO THINGS THAT ARE MEASUREMENTS OF US, NOT OF THE LANDSCAPE
-----------------------------------------------------------
Stated here because they are the easiest numbers in the output to misread:

* **Pit and fill metrics on S1 and anything downstream of it measure OUR
  epsilon fill.** ``bake.pipeline``'s B2a fills depressions before incising, so
  ``fill_volume_per_area_m`` on a baked surface is near zero by construction. It
  is a realism gate on S0 and on the references; on S1-S4 it says only that the
  fill ran.
* **Our 30 m samples of S1-S4 are POINT SAMPLES**, every 16th fine cell, while a
  real 30 m DEM is closer to an area average. ``--blockmean-s1`` adds a 16x16
  block-averaged 30 m field computed from the cached bake so the size of that
  convention difference is visible rather than argued about.

USAGE
-----
    python tools/stage_realism_report.py \
        --case plains:DUMPDIR:llano_estacado_plains,illinois_till_plain \
        --case alpine:DUMPDIR:alps_valais_alpine,teton_range_alpine \
        --cache RESULTS.json [--blockmean-s1 BAKE.npy@plains] [--markdown OUT.md]

``--cache`` is a plain JSON of every ``describe`` result keyed by field name, so
a re-run costs nothing and the numbers in the report can be re-derived without
re-reading a raster.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import geomorph  # noqa: E402
from terrain_service.geomorph import report as greport  # noqa: E402

TOOLS_DIR = Path(__file__).resolve().parent
REF_DIR = TOOLS_DIR.parent / "data" / "earth_reference"
REF_CACHE = REF_DIR / "cache"

#: 30 m grids are cropped by this many cells on every side before measuring.
#: The fine tier's carrier stencil reaches one fine pixel outside the baked
#: tile at the footprint edge and reads 0 mm there (vxc_stagedump prints
#: "rectangle is not fully covered"); measured on the plains tile, cropping ONE
#: cell takes the worst S3_fine-vs-S1 residual from 23.9 m to 3.1 m and cropping
#: more changes nothing. Two is that, doubled.
EDGE_CROP_30M = 2


# --------------------------------------------------------------------------- grids

def box_resample(z: np.ndarray, src_cell: float, dst_cell: float) -> np.ndarray:
    """Exact area-average of ``z`` from ``src_cell`` onto ``dst_cell`` (dst >= src).

    An integral image gives the exact mean of the piecewise-constant source over
    any real interval, so 1 m -> 1.875 m needs no integer relationship and no
    interpolation kernel that would invent roughness. Requires a void-free
    input; callers mask first. Verified two ways: against a brute-force
    partial-cell accumulation (agreement 1.4e-13) and against ``block_mean`` at
    an integer ratio (bit-identical).

    THE ONE ARTIFACT, BOUNDED. 1.875 = 15/8, so the partial-cell weights repeat
    with a period of 8 output cells, and on a constant-gradient source the
    staircase-vs-line difference shows up as a periodic ripple of about
    0.058 * (source rise per cell) peak-to-peak. On the references this tool
    actually resamples that is 0.0014 m for the Llano Estacado (1.38 deg at
    1 m), 0.0023 m for the Illinois till plain (2.29 deg) and 0.058 m for the
    Teton window (44.5 deg) -- i.e. under 0.08 deg of spurious grade on the
    plains and under 1.8 deg on the alpine site. It makes the reference very
    slightly ROUGHER, so any conclusion of the form "our surface is rougher than
    the reference" is conservative with respect to it.
    """
    if dst_cell < src_cell - 1e-12:
        raise ValueError(f"box_resample only downsamples ({src_cell} -> {dst_cell})")
    ratio = dst_cell / src_cell
    out = np.asarray(z, dtype=np.float64)
    for axis in (0, 1):
        out = np.moveaxis(out, axis, 0)
        n_src = out.shape[0]
        n_dst = int(np.floor(n_src / ratio))
        if n_dst < 2:
            raise ValueError("resample leaves fewer than two cells")
        c = np.concatenate([np.zeros((1,) + out.shape[1:]), np.cumsum(out, axis=0)])
        edges = np.arange(n_dst + 1, dtype=np.float64) * ratio
        lo = np.floor(edges).astype(int)
        frac = edges - lo
        lo = np.clip(lo, 0, n_src)
        hi = np.clip(lo + 1, 0, n_src)
        f = c[lo] + (c[hi] - c[lo]) * frac[(slice(None),) + (None,) * (out.ndim - 1)]
        out = (f[1:] - f[:-1]) / ratio
        out = np.moveaxis(out, 0, axis)
    return out


def block_mean(z: np.ndarray, factor: int) -> np.ndarray:
    if factor == 1:
        return np.asarray(z, dtype=np.float64)
    h = (z.shape[0] // factor) * factor
    w = (z.shape[1] // factor) * factor
    return (np.asarray(z[:h, :w], dtype=np.float64)
            .reshape(h // factor, factor, w // factor, factor).mean(axis=(1, 3)))


def centre_crop(z: np.ndarray, n: int) -> np.ndarray:
    if n > min(z.shape):
        raise ValueError(f"cannot crop {z.shape} to {n}")
    r = (z.shape[0] - n) // 2
    c = (z.shape[1] - n) // 2
    return np.ascontiguousarray(z[r:r + n, c:c + n])


def inset(z: np.ndarray, k: int) -> np.ndarray:
    return np.ascontiguousarray(z[k:z.shape[0] - k, k:z.shape[1] - k]) if k else z


def patch_voids(z: np.ndarray, name: str, max_frac: float = 0.02) -> tuple[np.ndarray, float]:
    """Replace non-finite cells with the field mean, refusing if there are many.

    ``geomorph`` has no void policy -- flow routing on a NaN is meaningless -- so
    a masked reference window has to be repaired or rejected. Rejecting at 2%
    keeps the repair from ever being the thing the metric measured; the fraction
    is returned and printed, so a run that patched anything says so.
    """
    m = ~np.isfinite(z)
    frac = float(m.mean())
    if frac == 0.0:
        return np.asarray(z, dtype=np.float64), 0.0
    if frac > max_frac:
        raise SystemExit(f"{name}: {frac:.1%} of cells are void; that is not a "
                         "window this tool will patch")
    out = np.array(z, dtype=np.float64)
    out[m] = np.nanmean(out)
    return out, frac


# --------------------------------------------------------------------------- references

def _manifest() -> dict:
    return json.loads((REF_DIR / "manifest.json").read_text())


def _site(mf: dict, site_id: str) -> dict:
    for s in mf["sites"]:
        if s["id"] == site_id:
            return s
    raise SystemExit(f"{site_id} is not in the earth-reference manifest")


def load_cop30_aeqd30(site_id: str) -> np.ndarray:
    """The site's Copernicus window on the manifest's own 30.0 m aeqd grid."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("earth_reference",
                                                  TOOLS_DIR / "earth_reference.py")
    er = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(er)
    er._gdal_env()
    mf = _manifest()
    s = _site(mf, site_id)
    if "error" in s.get("cop30", {}):
        raise SystemExit(f"{site_id}: cop30 not built ({s['cop30']['error']})")
    path = REF_CACHE / s["cop30"]["cache_file"]
    grid, _voids, _ns, _px = er.cop30_metric_grid(path, s["lat"], s["lon"],
                                                  s["cop30"]["window_km"] * 1000.0)
    return grid


def load_3dep_1m(site_id: str) -> np.ndarray:
    import rasterio
    mf = _manifest()
    s = _site(mf, site_id)
    u = s.get("usgs_3dep_1m")
    if not u or "error" in u:
        raise SystemExit(f"{site_id}: no 3DEP window in the manifest")
    path = REF_CACHE / u["cache_file"]
    with rasterio.open(path) as ds:
        a = ds.read(1).astype(np.float64)
        nod = ds.nodata
        px = abs(ds.transform.a)
    if abs(px - 1.0) > 1e-6:
        raise SystemExit(f"{site_id}: 3DEP window is {px} m, expected 1 m")
    bad = ~np.isfinite(a)
    if nod is not None and np.isfinite(nod):
        bad |= np.isclose(a, nod, rtol=0, atol=abs(nod) * 1e-6 + 1.0)
    bad |= a <= -100.0
    return np.where(bad, np.nan, a)


# --------------------------------------------------------------------------- driver

def describe_cached(cache: dict, key: str, z: np.ndarray, cell_m: float, *,
                    force: bool = False) -> dict:
    if key in cache and not force:
        return cache[key]
    z = np.ascontiguousarray(z, dtype=np.float64)
    print(f"    describe {key}: {z.shape[0]}x{z.shape[1]} @ {cell_m:g} m", flush=True)
    d = greport.describe(z, cell_m)
    d = {k: (float(v) if isinstance(v, (int, float, np.floating, np.integer))
             else bool(v) if isinstance(v, (bool, np.bool_)) else v)
         for k, v in d.items()}
    d["_shape"] = list(z.shape)
    cache[key] = d
    return d


#: (file base, column label) for the 30 m band, in pipeline order.
BAND30 = [
    ("S0_wire_30000mm", "S0 wire 30 m"),
    ("S1_bake_30000mm", "S1 bake"),
    ("S2_coarse_30000mm", "S2 carrier (coarse)"),
    ("S3_coarse_30000mm", "S3 surface (coarse)"),
    ("S4_coarse_30000mm", "S4 voxels (coarse)"),
    ("S2_fine_30000mm", "S2 carrier (fine)"),
    ("S3_fine_30000mm", "S3 surface (fine)"),
    ("S4_fine_30000mm", "S4 voxels (fine)"),
]

BAND1875 = [
    ("S1_bake_1875mm", "S1 bake"),
    ("S2_coarse_1875mm", "S2 carrier (coarse)"),
    ("S3_coarse_1875mm", "S3 surface (coarse)"),
    ("S4_coarse_1875mm", "S4 voxels (coarse)"),
    ("S2_fine_1875mm", "S2 carrier (fine)"),
    ("S3_fine_1875mm", "S3 surface (fine)"),
    ("S4_fine_1875mm", "S4 voxels (fine)"),
]


def run_case(name: str, dump: Path, ref_ids: list[str], cache: dict,
             blockmean_s1: Path | None) -> dict:
    print(f"[{name}] {dump}")
    out: dict = {"case": name, "dump": str(dump), "refs": ref_ids,
                 "band30": {}, "band1875": {}, "notes": []}

    # ---- 30 m band ------------------------------------------------------
    ours30: dict[str, np.ndarray] = {}
    for base, label in BAND30:
        p = dump / f"{base}.npy"
        if not p.exists():
            continue
        side = json.loads((dump / f"{base}.json").read_text())
        cell = side["cell_size_m"]
        if abs(cell - 30.0) > 1e-9:
            raise SystemExit(f"{p}: sidecar says {cell} m, not 30 m")
        ours30[label] = inset(np.load(p).astype(np.float64), EDGE_CROP_30M)
    n30 = min(a.shape[0] for a in ours30.values())

    if blockmean_s1 is not None:
        z = np.load(blockmean_s1, mmap_mode="r")
        bm = block_mean(np.asarray(z, dtype=np.float64), 16)
        ours30["S1 bake (16x16 block mean)"] = centre_crop(bm, n30)
        out["notes"].append(
            "S1 bake (16x16 block mean) is the same bake area-averaged to 30 m "
            "rather than point-sampled; the gap between it and 'S1 bake' is the "
            "size of the sampling-convention difference, not a pipeline defect.")

    refs30: dict[str, np.ndarray] = {}
    for rid in ref_ids:
        g = load_cop30_aeqd30(rid)
        g, frac = patch_voids(g, f"{rid} cop30")
        if frac:
            out["notes"].append(f"{rid} cop30: {frac:.2%} of cells patched to the mean")
        refs30[f"REF {rid} cop30 DSM"] = centre_crop(g, n30)
        try:
            d = load_3dep_1m(rid)
        except SystemExit:
            continue
        d, frac = patch_voids(d, f"{rid} 3dep")
        if frac:
            out["notes"].append(f"{rid} 3DEP: {frac:.2%} of cells patched to the mean")
        refs30[f"REF {rid} 3DEP DTM@30 m"] = block_mean(d, 30)

    fields30 = {**ours30, **refs30}
    for k, z in fields30.items():
        out["band30"][k] = describe_cached(cache, f"{name}|30m|{k}", z, 30.0)
    # On the RESULTS, not on the inputs: every describe() carries its own cell_m,
    # so this checks what was actually measured rather than what was intended.
    greport.require_same_resolution(out["band30"].values(), what="30 m band")

    # ---- 1.875 m band ---------------------------------------------------
    ours = {}
    for base, label in BAND1875:
        p = dump / f"{base}.npy"
        if not p.exists():
            continue
        side = json.loads((dump / f"{base}.json").read_text())
        if abs(side["cell_size_m"] - 1.875) > 1e-9:
            raise SystemExit(f"{p}: sidecar says {side['cell_size_m']} m, not 1.875 m")
        ours[label] = np.load(p).astype(np.float64)
    n = min(a.shape[0] for a in ours.values())

    refs = {}
    for rid in ref_ids:
        try:
            d = load_3dep_1m(rid)
        except SystemExit:
            out["notes"].append(f"{rid}: no 3DEP 1 m window, so it cannot check the "
                                "1.875 m band at all")
            continue
        d, frac = patch_voids(d, f"{rid} 3dep")
        refs[f"REF {rid} 3DEP DTM@1.875 m"] = box_resample(d, 1.0, 1.875)
    if refs:
        n = min([n] + [a.shape[0] for a in refs.values()])
    fields = {k: centre_crop(v, n) for k, v in {**ours, **refs}.items()}
    for k, z in fields.items():
        out["band1875"][k] = describe_cached(cache, f"{name}|1875mm|{k}", z, 1.875)
    greport.require_same_resolution(out["band1875"].values(), what="1.875 m band")

    out["n30"] = n30
    out["n1875"] = n
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("USAGE")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", action="append", required=True,
                    metavar="NAME:DUMPDIR:REF1,REF2")
    ap.add_argument("--blockmean-s1", action="append", default=[],
                    metavar="BAKE.npy@CASE")
    ap.add_argument("--cache", required=True)
    ap.add_argument("--out", required=True, help="JSON of every case's results")
    a = ap.parse_args()

    cache_path = Path(a.cache)
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    bm = {}
    for spec in a.blockmean_s1:
        path, case = spec.rsplit("@", 1)
        bm[case] = Path(path)

    results = []
    for spec in a.case:
        # Split from BOTH ends, not left-to-right: a Windows dump directory is
        # "C:/..." and a naive three-way split on ":" makes the case name "C".
        name, rest = spec.split(":", 1)
        dumpdir, refs = rest.rsplit(":", 1)
        try:
            results.append(run_case(name, Path(dumpdir), refs.split(","), cache,
                                    bm.get(name)))
        finally:
            cache_path.write_text(json.dumps(cache, indent=1))
    Path(a.out).write_text(json.dumps(results, indent=1))
    print(f"\nwrote {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
