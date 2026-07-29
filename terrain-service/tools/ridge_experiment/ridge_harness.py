"""Ridge-deficit experiment harness.

Modes:
  bake  --config NAME --tile plains|alpine|rolling [--set k=v ...] [--noise STYLE_JSON]
        Full production bake_tile with overridden BakeConstants and/or a variant
        B1 roughness kernel; measures the metric panel on the matched windows.
  s1a   --config NAME --tile T [--noise STYLE_JSON]
        Fast B0+B1-only window (carrier subgrid + variant roughness), no erosion.
        For sweeping noise styles cheaply.

Windows (identical to docs/terrain-validation-2026-07.md):
  1.875 m band: fine cells [3544, 4633) of the tile interior -> 1089^2, 2040 m.
  30 m band   : every 16th fine cell of the whole interior, edge-cropped 2 -> 508^2.

Outputs under OUT/:
  metrics/{config}_{tile}.json      the panel
  windows/{config}_{tile}_S1.npy    1089^2 S1 window (bake mode)
"""
from __future__ import annotations

import argparse
import os
import dataclasses
import json
import sys
import time
from pathlib import Path

import numpy as np

WT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(WT))

from terrain_service import tile_codec  # noqa: E402
from terrain_service.bake import pipeline as bp  # noqa: E402
from terrain_service.bake import noise as bnoise  # noqa: E402
from terrain_service.geomorph import report as greport  # noqa: E402
from terrain_service.geomorph.geomorphon import geomorphon_histogram  # noqa: E402
from terrain_service.geomorph.slope import slope_statistics  # noqa: E402

TILES_DIR = Path(os.environ.get(
    "RIDGE_TILES_DIR",
    r"D:\voxelsim\tile-cache"
    r"\terrain-diffusion-unlabeled-060b0c927ccc807e\000000000135276f\s1"))
OUT = Path(os.environ.get("RIDGE_OUT",
                          Path(__file__).resolve().parent / "ridge_out"))
SEED = 20260719

TILE_XY = {"plains": (-55, 20), "alpine": (-5, 15), "rolling": (15, 55)}
WOFF, WN = 3544, 1089  # 1.875 m band window inside the 8192^2 interior
CELL = 1.875


# --------------------------------------------------------------------------- noise variants

_ABS_MEAN = float(np.sqrt(2.0 / np.pi))            # E|X|, X ~ N(0,1)
_ABS_STD = float(np.sqrt(1.0 - 2.0 / np.pi))       # std |X|


def make_roughness(style: dict):
    """A drop-in for noise.roughness with a style dict.

    style keys (all optional):
      fold: "none" (default) | "up"    fold octaves to crests-up ridged noise
      amp:  amplitude multiplier on _REF_AMPLITUDE_M (default 1.0)
      gain_hi / gain_lo: slope-gain clip overrides
    Defaults reproduce noise.roughness bit-for-bit.
    """
    fold = style.get("fold", "none")
    amp_mul = float(style.get("amp", 1.0))
    gain_lo = float(style.get("gain_lo", bnoise._SLOPE_GAIN_LO))
    gain_hi = float(style.get("gain_hi", bnoise._SLOPE_GAIN_HI))
    cons_amp = float(style.get("cons_amp", 0.0))
    cons_lo = float(style.get("cons_lo", 0.10))
    cons_hi = float(style.get("cons_hi", 0.30))
    cons_max_wl = float(style.get("cons_max_wl", 1e9))
    cons_min_wl = float(style.get("cons_min_wl", 0.0))

    def roughness(carrier_z, cell_m, slope, seed, src_nyquist_m=30.0,
                  origin_cells=(0, 0)):
        z = np.asarray(carrier_z)
        cell_m = float(cell_m)
        oy, ox = int(origin_cells[0]), int(origin_cells[1])
        n0, n1 = z.shape
        out = np.zeros((n0, n1), dtype=np.float32)
        for octave, wavelength in enumerate(
                bnoise.octave_wavelengths(cell_m, float(src_nyquist_m))):
            p = int(round(wavelength / cell_m))
            amp = np.float32(amp_mul * bnoise._REF_AMPLITUDE_M
                             * (wavelength / src_nyquist_m) ** bnoise._ROUGHNESS_H)
            f = bnoise._octave_field((n0, n1), p, seed, octave, oy, ox)
            if fold == "up":
                f = (np.float32(_ABS_MEAN) - np.abs(f)) / np.float32(_ABS_STD)
            out += amp * f
        s = np.asarray(slope, dtype=np.float32)
        gain = np.clip(s / np.float32(bnoise._SLOPE_REF),
                       np.float32(gain_lo), np.float32(gain_hi))
        out *= gain
        if cons_amp > 0.0:
            c = np.zeros((n0, n1), dtype=np.float32)
            for octave, wavelength in enumerate(
                    bnoise.octave_wavelengths(cell_m, float(src_nyquist_m))):
                if not cons_min_wl <= wavelength <= cons_max_wl:
                    continue
                p = int(round(wavelength / cell_m))
                amp = np.float32(cons_amp * bnoise._REF_AMPLITUDE_M
                                 * (wavelength / src_nyquist_m) ** bnoise._ROUGHNESS_H)
                g = bnoise._octave_field((n0, n1), p, seed, 100 + octave, oy, ox)
                c += amp * ((np.float32(_ABS_MEAN) - np.abs(g))
                            / np.float32(_ABS_STD))
            cgain = np.clip((np.float32(cons_hi) - s)
                            / np.float32(cons_hi - cons_lo),
                            np.float32(0.0), np.float32(1.0))
            out += c * cgain
        return out

    return roughness


# --------------------------------------------------------------------------- metrics

def block_mean(z, factor):
    if factor == 1:
        return np.asarray(z, dtype=np.float64)
    h = (z.shape[0] // factor) * factor
    w = (z.shape[1] // factor) * factor
    return (np.asarray(z[:h, :w], dtype=np.float64)
            .reshape(h // factor, factor, w // factor, factor).mean(axis=(1, 3)))


def g10_panel(z, cell):
    g = geomorphon_histogram(z, cell, search_m=10 * cell)
    return {
        "g10_ridge_peak": g.frac("ridge", "peak"),
        "g10_valley_pit": g.frac("valley", "pit"),
        "g10_slope": g.fractions["slope"],
        "g10_flat": g.fractions["flat"],
        "g10_hollow_footslope": g.frac("hollow", "footslope"),
        "g10_spur": g.fractions["spur"],
        "g10_shoulder": g.fractions["shoulder"],
    }


def slope_ladder(z, cell):
    out = {}
    for f in (1, 2, 4, 8, 16):
        b = block_mean(z, f)
        s = slope_statistics(b, cell * f)
        out[f"mean_deg_{cell * f:g}m"] = s.to_dict()["mean_deg"]
    return out


def band1875_panel(z, *, flow=True):
    z = np.ascontiguousarray(z, dtype=np.float64)
    out = {}
    out.update(g10_panel(z, CELL))
    out.update(slope_ladder(z, CELL))
    d = greport.describe(z, CELL, skip_flow=not flow)
    for k in ("mean_deg", "p95_deg", "frac_flat", "frac_slope", "frac_ridge_peak",
              "frac_valley_pit", "frac_flat_coarse_thresh", "curvature_asymmetry",
              "tail_asymmetry", "hurst_overall", "theta", "slope_area_r2",
              "hack_h", "hack_r2", "relief_m"):
        if k in d:
            out[k] = d[k]
    return out


def band30_panel(s1_full):
    # point-sample every 16th cell, crop 2 (EDGE_CROP_30M)
    z = np.ascontiguousarray(s1_full[::16, ::16][2:-2, 2:-2], dtype=np.float64)
    d = greport.describe(z, 30.0)
    return {f"b30_{k}": d[k] for k in
            ("theta", "slope_area_r2", "hack_h", "hack_r2", "mean_deg",
             "frac_flat", "frac_ridge_peak", "frac_valley_pit",
             "frac_flat_coarse_thresh", "curvature_asymmetry", "tail_asymmetry")}


# --------------------------------------------------------------------------- bake plumbing

def coarse_fetch():
    cache = {}

    def fetch(x, y):
        if (x, y) not in cache:
            p = TILES_DIR / f"{x}_{y}.vxtl"
            cache[(x, y)] = (tile_codec.decode(p.read_bytes()).elevation.astype(np.float32)
                             if p.exists() else None)
        return cache[(x, y)]

    return fetch


def build_consts(overrides: dict) -> bp.BakeConstants:
    typed = {}
    base = bp.CONSTANTS
    for k, v in overrides.items():
        cur = getattr(base, k)
        if isinstance(cur, bool):
            typed[k] = v in ("1", "true", "True", True)
        elif isinstance(cur, (int, float)) and not isinstance(cur, bool):
            typed[k] = type(cur)(float(v)) if not isinstance(cur, str) else v
        else:
            typed[k] = v
    return dataclasses.replace(base, **typed)


def run_bake(config: str, tile: str, overrides: dict, style: dict | None):
    tx, ty = TILE_XY[tile]
    geom = bp.PRODUCTION
    geom.assert_production()
    consts = build_consts(overrides)
    kernels = bp.load_kernels()
    if style:
        kernels = dataclasses.replace(kernels, roughness=make_roughness(style))
    fetch = coarse_fetch()
    lvl = bp.FlowLevel(0, geom, consts)
    sx, sy = bp.superblock_index(tx, ty, lvl)
    print(f"[{config}/{tile}] superblock ({sx},{sy})...", flush=True)
    t0 = time.time()
    inflow = bp.build_flow_superblock(fetch, sx, sy, lvl, kernels)
    print(f"  superblock {time.time()-t0:.0f}s wall, missing {len(inflow.missing_tiles)}",
          flush=True)

    grabbed = {}

    def sink(name, interior):
        if name == "B0B1.carrier_rough":
            grabbed["S1a"] = np.array(interior[WOFF:WOFF + WN, WOFF:WOFF + WN])
        elif name == "B2d.incised":
            grabbed["S1b"] = np.array(interior[WOFF:WOFF + WN, WOFF:WOFF + WN])
        elif name == "B2c.accumulation_m2":
            grabbed["acc"] = np.array(interior[WOFF:WOFF + WN, WOFF:WOFF + WN])
        elif name == "B2d.incision_depth_m":
            grabbed["depth"] = np.array(interior[WOFF:WOFF + WN, WOFF:WOFF + WN])

    t0 = time.time()
    res = bp.bake_tile(world_seed=SEED, tile_x=tx, tile_y=ty, coarse_fetch=fetch,
                       kernels=kernels, geom=geom, consts=consts,
                       inflow_source=inflow, stage_sink=sink)
    wall = time.time() - t0
    print(f"  bake {wall:.0f}s wall, relief {res.stats['relief_m']:.1f} m", flush=True)

    s1 = res.elevation_m
    win = np.array(s1[WOFF:WOFF + WN, WOFF:WOFF + WN])
    (OUT / "windows").mkdir(parents=True, exist_ok=True)
    np.save(OUT / "windows" / f"{config}_{tile}_S1.npy", win.astype(np.float32))
    np.save(OUT / "windows" / f"{config}_{tile}_S1a.npy", grabbed["S1a"].astype(np.float32))
    np.save(OUT / "windows" / f"{config}_{tile}_acc.npy", grabbed["acc"].astype(np.float32))
    np.save(OUT / "windows" / f"{config}_{tile}_depth.npy",
            grabbed["depth"].astype(np.float32))

    consts_used = consts
    rec = {
        "config": config, "tile": tile, "mode": "bake",
        "overrides": overrides, "noise_style": style or {},
        "bake_stats": {k: res.stats[k] for k in
                       ("relief_m", "incision_mean_m", "incision_p99_m",
                        "channel_cells", "interior_dead_ends")},
        # fraction of the WINDOW past the channel-initiation area, and past one
        # voxel of carve: the "did we buy ridges by making everything channel"
        # guards.
        "win_frac_above_acrit": float(
            (grabbed["acc"] >= consts_used.channel_init_area_m2).mean()),
        "win_frac_carved_gt_0.1m": float((grabbed["depth"] > 0.1).mean()),
        "wall_s": wall,
    }
    print("  measuring S1 1.875 m band...", flush=True)
    rec["S1"] = band1875_panel(win, flow=True)
    print("  measuring S1a...", flush=True)
    rec["S1a"] = band1875_panel(grabbed["S1a"], flow=False)
    rec["S1b_g10"] = g10_panel(np.ascontiguousarray(grabbed["S1b"], np.float64), CELL)
    print("  measuring 30 m band...", flush=True)
    rec["S1_30m"] = band30_panel(s1)
    write_rec(rec)


def run_s1a(config: str, tile: str, style: dict | None):
    """Fast B0+B1 window: carrier over a coarse subgrid + variant roughness."""
    tx, ty = TILE_XY[tile]
    geom = bp.PRODUCTION
    fetch = coarse_fetch()
    padded, missing = bp.assemble_padded_coarse(fetch, tx, ty, geom)
    if missing:
        print(f"  WARNING missing ring tiles: {missing}")
    # coarse rows covering fine window rows [WOFF, WOFF+WN) of the INTERIOR,
    # in padded coarse coords (+apron), with margin for the IIR prefilter.
    m = 32
    a = geom.apron_coarse_px + WOFF // geom.scale - m
    b = geom.apron_coarse_px + (WOFF + WN) // geom.scale + 1 + m
    sub = padded[a:b, a:b]
    kern = bp.load_kernels()
    fine = np.asarray(kern.carrier(sub, geom.scale), dtype=np.float32)
    cell = geom.fine_pixel_m
    gy, gx = np.gradient(fine, cell)
    slope = np.hypot(gx, gy).astype(np.float32)
    prow0, pcol0 = geom.padded_origin_cells(tx, ty)
    origin = (prow0 + a * geom.scale, pcol0 + a * geom.scale)
    seed = bp.roughness_seed(SEED, origin, bake_version=bp.BAKE_VERSION,
                             anchor_pitch_fine_px=bp.CONSTANTS.noise_anchor_pitch_fine_px)
    rfn = make_roughness(style or {})
    fine = fine + np.asarray(rfn(fine, cell, slope, seed,
                                 src_nyquist_m=bp.CONSTANTS.src_nyquist_m,
                                 origin_cells=origin), dtype=np.float32)
    # crop the exact window: window fine index (interior) WOFF -> padded fine
    # index apron+WOFF -> sub fine index apron+WOFF - a*scale
    o = geom.apron_fine_px + WOFF - a * geom.scale
    win = fine[o:o + WN, o:o + WN]
    rec = {"config": config, "tile": tile, "mode": "s1a", "noise_style": style or {},
           "S1a": band1875_panel(win, flow=False)}
    write_rec(rec)


def run_seampair(config: str, tile: str, overrides: dict, style: dict | None):
    """Bake TILE and its WEST neighbour with the same constants; measure the join.

    The bar (pipeline.PROFILE_SEAM): the mean step across the join must sit AT
    the terrain's own one-cell step (ratio ~1.000), with the tail bounded.
    """
    tx, ty = TILE_XY[tile]
    geom = bp.PRODUCTION
    consts = build_consts(overrides)
    kernels = bp.load_kernels()
    if style:
        kernels = dataclasses.replace(kernels, roughness=make_roughness(style))
    fetch = coarse_fetch()
    lvl = bp.FlowLevel(0, geom, consts)
    edges = {}
    for name, x in (("W", tx - 1), ("E", tx)):
        sx, sy = bp.superblock_index(x, ty, lvl)
        inflow = bp.build_flow_superblock(fetch, sx, sy, lvl, kernels)
        print(f"[seam {config}/{tile}] baking ({x},{ty})...", flush=True)
        res = bp.bake_tile(world_seed=SEED, tile_x=x, tile_y=ty, coarse_fetch=fetch,
                           kernels=kernels, geom=geom, consts=consts,
                           inflow_source=inflow)
        z = res.elevation_m
        edges[name] = (np.array(z[:, -2:]) if name == "W" else np.array(z[:, :2]))
        del res, z
    step = np.abs(edges["W"][:, 1] - edges["E"][:, 0]).astype(np.float64)
    own = 0.5 * (np.abs(np.diff(edges["W"], axis=1)[:, 0])
                 + np.abs(np.diff(edges["E"], axis=1)[:, 0])).astype(np.float64)
    rec = {
        "config": config, "tile": tile, "mode": "seampair",
        "overrides": overrides, "noise_style": style or {},
        "join_mean_step_m": float(step.mean()),
        "terrain_own_step_m": float(own.mean()),
        "ratio": float(step.mean() / own.mean()),
        "join_p99_step_m": float(np.percentile(step, 99)),
        "join_max_step_m": float(step.max()),
    }
    (OUT / "metrics").mkdir(parents=True, exist_ok=True)
    p = OUT / "metrics" / f"seam_{config}_{tile}.json"
    p.write_text(json.dumps(rec, indent=1))
    print(f"[seam {config}/{tile}] join mean {rec['join_mean_step_m']:.4f} m vs own "
          f"{rec['terrain_own_step_m']:.4f} m  ratio {rec['ratio']:.3f}  "
          f"max {rec['join_max_step_m']:.2f} m", flush=True)


def write_rec(rec):
    (OUT / "metrics").mkdir(parents=True, exist_ok=True)
    p = OUT / "metrics" / f"{rec['config']}_{rec['tile']}.json"
    p.write_text(json.dumps(rec, indent=1))
    key = "S1" if "S1" in rec else "S1a"
    m = rec[key]
    print(f"[{rec['config']}/{rec['tile']}] {key}: "
          f"g10 ridge+peak {m['g10_ridge_peak']:.4f}  valley+pit {m['g10_valley_pit']:.4f}  "
          f"mean_deg {m.get('mean_deg_1.875m', float('nan')):.2f}", flush=True)
    print(f"  -> {p}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["bake", "s1a", "seampair"])
    ap.add_argument("--config", required=True)
    ap.add_argument("--tile", required=True, choices=list(TILE_XY))
    ap.add_argument("--set", action="append", default=[], metavar="K=V")
    ap.add_argument("--noise", default=None, help="JSON style dict")
    a = ap.parse_args()
    overrides = dict(s.split("=", 1) for s in a.set)
    style = json.loads(a.noise) if a.noise else None
    if a.mode == "bake":
        run_bake(a.config, a.tile, overrides, style)
    elif a.mode == "seampair":
        run_seampair(a.config, a.tile, overrides, style)
    else:
        run_s1a(a.config, a.tile, style)


if __name__ == "__main__":
    main()
