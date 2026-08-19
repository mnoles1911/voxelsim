"""karst_prototype.py -- hydrology-routed cave networks, the research prototype.

WHAT THIS IS. The Phase 0 prototype for replacing voxelsim's hash caves with a
conduit network routed by the world's own hydrology, adapting Paris, Guerin,
Peytavie, Collon & Galin, "Synthesizing Geologically Coherent Cave Networks"
(Computer Graphics Forum 40(7), Pacific Graphics 2021; reference implementation
at github.com/aparis69/Karst-Synthesis, MIT).

WHAT THIS IS NOT. It is not the shipping generator and it must never become it
by drift. voxel-core is integer-only under a CI float ban and every amplifier
behaviour is mirrored bit-exactly in worldgen.ush; this file is floats and
scipy, and it exists to answer "does the SHAPE read as karst" before a single
line of deterministic C++ is written. Its output is a picture and a table of
statistics, judged against real cave surveys. Nothing here writes a tile, bumps
a version, or touches the engine.

THE METHOD, AND WHERE IT DIFFERS FROM THE PAPER.

  1. Geological fields, derived from what the bake already has:
       inception horizons  -- a folded stratigraphic field, the same
                              construction as bake/noise.py's `repose_field`
                              strata term (which the bake computes and then
                              deletes at pipeline.py:4735), re-parameterised to
                              a karst wavelength. Horizons are the DESCENDING
                              zero crossings -- strong-over-weak contacts, which
                              is where inception happens.
       fracture fabric     -- structure tensor of the elevation field. Ridge and
                              valley fabric is structurally controlled in real
                              terrain, so this is inference from the only
                              structural evidence present, not invention.
       permeability        -- flow accumulation, curvature and climate.
       water table         -- a Toth-style regional solve, `div(K grad h) = -R`,
                              Dirichlet h = z at drains (channels, lakes, sea).
                              THE BAKE HAS NO WATER TABLE; this is the one field
                              that has to be invented, and everything about
                              vadose-vs-phreatic depends on it.

  2. Sinks and springs from the bake's own hydrology, not from taste:
       sinks   -- terminal basins (BASIN_TABLE kinds dry_playa / salt_flat /
                  seasonal / lake_terminal: water arrives and does not leave by
                  the surface, which is a ponor) plus stream cells that cross an
                  inception horizon outcrop.
       springs -- valley-floor cells where a horizon daylights at the water
                  table, plus lake shores and the coast.

  3. Route sinks to springs by Dijkstra over a 3D nearest-neighbour graph whose
     edge cost is anisotropic and geological, then prune with the paper's
     gamma-skeleton. Gamma is the morphology knob: ~2.0 branchwork conduits,
     ~1.05 spongework, a heavy fracture term gives rectilinear mazes.

  THE DEVIATION, STATED. The paper's domains are 750-1000 m square; a voxelsim
  superblock is 92 km. Poisson-sampling that volume is ~10M nodes per horizon.
  So systems are clustered first (sinks grouped by which spring the water-table
  gradient sends them to) and each system samples only its own corridor. What
  that costs is regional spongework and block-scale fracture mazes, which need
  the whole volume sampled; they can come back as a per-system mode, i.e. local
  mazes rather than regional ones. This is a real loss and it is a stated
  non-goal rather than a surprise.

WHAT IT WILL NOT TELL YOU. Whether the conduits look good as VOXELS. The
skeleton-to-geometry step is not in the reference code either, and the risk that
kills this project lives there: the paper's organic look comes from smooth-min
blended SDFs, which have no early-out and no bounded primitive count, and a hard
union of capsules reads as intersecting cylinders -- verbatim the owner's
original complaint about the system being replaced. That question belongs to
vxc_karstprobe, in C++, against the real voxel grid.

Usage:
    python tools/karst_prototype.py <tile.vxtl> --out DIR [--seed N] [--gamma G]
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402
from scipy.sparse import coo_matrix  # noqa: E402
from scipy.sparse.csgraph import dijkstra  # noqa: E402
from scipy.spatial import cKDTree  # noqa: E402

from terrain_service import tile_codec as tc  # noqa: E402

# --- geometry ---------------------------------------------------------------
FINE_PX_M = 1.875
TILE_PX = 8192
#: Work at 30 m, which is the coarse tile's own resolution and the scale the
#: flow superblock already routes drainage at. 8192 / 16 = 512 cells.
DOWN = 16
GRID_M = FINE_PX_M * DOWN          # 30.0 m
GRID_N = TILE_PX // DOWN           # 512

#: The owner's directive: uniformly generous, about 5x real. Every length below
#: that is "5x" carries this factor explicitly so the exaggeration is one number
#: in one place rather than a dozen tuned constants.
EXAGGERATION = 5.0


# ---------------------------------------------------------------------------
# 1. Fields from the tile
# ---------------------------------------------------------------------------

def load_tile(path: pathlib.Path) -> dict:
    """Elevation and hydrology at 30 m, plus the basin registry.

    ELEVATION IS AN APPROXIMATION AND SAYS SO. `elevation_cp` holds prefiltered
    B-spline CONTROL POINTS, not samples; the true surface is `evalCarrier` over
    a 4x4 stencil (voxelcore/carrier.h) and the amplified surface adds rills,
    bedding and warp on top of that. Block-averaging control points to 30 m
    recovers the low-frequency carrier to well within the metre, because the
    prefilter is a sharpening whose effect averaging undoes -- and 30 m routing
    cannot see anything finer anyway. It is NOT the drawn ground, and no number
    derived from it should be quoted as a ground elevation.
    """
    t = tc.decode_v2(path.read_bytes())
    # `quant` IS A CODE, NOT A MULTIPLIER. tile_codec.QUANT_MM maps 1 -> 100 mm
    # and 2 -> 250 mm. Multiplying by the code itself is a silent 100x error
    # that still decodes to plausible-looking terrain -- it reported 25 m of
    # relief across a 15 km alpine tile, which is only obviously wrong if you
    # know the region. This repo has already retracted one 100x decode error
    # (commit bb83002); this is the same trap in a different field.
    cp = t.elevation_cp.astype(np.float64) * tc.QUANT_MM[t.quant] + t.base_offset_mm
    elev_m = cp.reshape(GRID_N, DOWN, GRID_N, DOWN).mean(axis=(1, 3)) / 1000.0

    out = {"tile": path.stem, "seed": t.seed, "tx": t.x, "ty": t.y,
           "elev_m": elev_m, "bake_ver": t.bake_ver}

    if t.flow is not None:
        log2acc = (t.flow & tc.FLOW_LOG2_MASK).astype(np.float32)
        # Max-pool, not mean: a 30 m cell containing a river IS a river cell,
        # and averaging log2 accumulation over 16x16 would dissolve every
        # channel narrower than the block. The network is the thing being
        # detected, so the pooling has to preserve it.
        out["log2acc"] = log2acc.reshape(GRID_N, DOWN, GRID_N, DOWN).max(axis=(1, 3))
    else:
        out["log2acc"] = np.zeros((GRID_N, GRID_N), np.float32)

    out["basins"] = t.basins or []
    out["heads"] = t.heads or []

    # Lake surfaces, for the water table's Dirichlet condition. The water plane
    # is a DEPTH above this tile's own quantised bed, negative where dry.
    lake = np.zeros((GRID_N, GRID_N), bool)
    if t.water_cp is not None:
        wet = (t.water_cp > 0)
        lake = wet.reshape(GRID_N, DOWN, GRID_N, DOWN).any(axis=(1, 3))
    out["lake"] = lake
    return out


def _value_noise_1d(x: np.ndarray, seed: int, wavelength: float) -> np.ndarray:
    """Smooth 1-D value noise, world-anchored. Mirrors the SHAPE of
    bake/noise.py's `_strata_1d` (lattice + smoothstep interpolation) without
    importing it: this prototype must not become a second caller of a bake
    internal, because that is how two copies of one calibration drift apart --
    the failure this repo has recorded most often."""
    u = x / wavelength
    i0 = np.floor(u).astype(np.int64)
    f = u - i0
    def h(i):
        v = (i.astype(np.uint64) * np.uint64(0x9E3779B97F4A7C15)) ^ np.uint64(seed)
        v = (v ^ (v >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
        v = (v ^ (v >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
        v = v ^ (v >> np.uint64(31))
        return (v & np.uint64(0xFFFFFFFF)).astype(np.float64) / float(1 << 32) * 2.0 - 1.0
    a, b = h(i0), h(i0 + 1)
    s = f * f * (3.0 - 2.0 * f)
    return a * (1.0 - s) + b * s


#: Horizon constants, module-level so the network stage evaluates the SAME
#: field rather than growing a second copy of it.
HORIZON_WAVELENGTH_M = 36.0 * EXAGGERATION
HORIZON_FOLD_AMP_M = 12.0 * EXAGGERATION
HORIZON_FOLD_WAVELENGTH_M = 300.0


def inception_fold(x_m, y_m, seed: int):
    """The fold that displaces the stratigraphic datum. Split out because the
    network stage needs to evaluate horizons at a NODE'S OWN z, and a 2D array
    of the field sampled at the surface cannot answer that -- which is exactly
    the bug that made conduits straight: the horizon term carried no depth
    information, so it could not make a route follow a bedding plane."""
    return (HORIZON_FOLD_AMP_M * _value_noise_1d(x_m, seed ^ 0x11, HORIZON_FOLD_WAVELENGTH_M)
            + HORIZON_FOLD_AMP_M * _value_noise_1d(y_m, seed ^ 0x22,
                                                   HORIZON_FOLD_WAVELENGTH_M * 0.61))


def inception_at(z_m, fold, seed: int):
    """Signed stratigraphic field at an arbitrary z. Zero crossings are the
    horizons; descending crossings are strong-over-weak contacts."""
    return _value_noise_1d(z_m + fold, seed ^ 0x33, HORIZON_WAVELENGTH_M)


def inception_field(z_m: np.ndarray, x_m: np.ndarray, y_m: np.ndarray, seed: int,
                    wavelength_m: float = HORIZON_WAVELENGTH_M,
                    fold_amp_m: float = HORIZON_FOLD_AMP_M,
                    fold_wavelength_m: float = HORIZON_FOLD_WAVELENGTH_M) -> np.ndarray:
    """The stratigraphic field. Horizons are its DESCENDING zero crossings.

    Same construction as `repose_field`'s strata term: 1-D noise over elevation
    displaced by a world-anchored fold, so bedding undulates instead of sitting
    at flat Z. The wavelength is the repose term's 30 m scaled by EXAGGERATION;
    the fold amplitude likewise. Returning a signed field rather than a set of
    surfaces is what lets the edge cost use PROXIMITY to a horizon as a
    continuous term, which is what the reference method's `horizon_cost` needs.
    """
    fold = (fold_amp_m * _value_noise_1d(x_m, seed ^ 0x11, fold_wavelength_m)
            + fold_amp_m * _value_noise_1d(y_m, seed ^ 0x22, fold_wavelength_m * 0.61))
    return _value_noise_1d(z_m + fold, seed ^ 0x33, wavelength_m)


def fracture_fabric(elev_m: np.ndarray, sigma_cells: float = 8.0):
    """Dominant structural orientation and its coherence, from the elevation
    structure tensor. Returns (theta, coherence) per cell, theta in radians.

    Real ridge/valley fabric is structurally controlled, so this infers joint
    orientation from the only structural evidence the world carries. Coherence
    scales the anisotropy so bland terrain routes isotropically (spongework)
    rather than growing a fake rectilinear maze."""
    gy, gx = np.gradient(elev_m.astype(np.float64), GRID_M)
    jxx = ndimage.gaussian_filter(gx * gx, sigma_cells)
    jyy = ndimage.gaussian_filter(gy * gy, sigma_cells)
    jxy = ndimage.gaussian_filter(gx * gy, sigma_cells)
    diff = jxx - jyy
    norm = np.hypot(diff, 2.0 * jxy)
    trace = jxx + jyy
    coherence = np.where(trace > 1e-12, norm / np.maximum(trace, 1e-12), 0.0)
    # The structure tensor's principal direction is ACROSS the fabric; the
    # fabric itself (and therefore the joint set water would exploit) runs
    # perpendicular to it, hence the +pi/2.
    theta = 0.5 * np.arctan2(2.0 * jxy, diff) + math.pi / 2.0
    return theta, np.clip(coherence, 0.0, 1.0)


def water_table(elev_m: np.ndarray, drain: np.ndarray, recharge: float = 1.0,
                iters: int = 400) -> np.ndarray:
    """Regional water table by red-black Jacobi on `lap(h) = -R`, with h = z
    pinned at drains (Toth-style regional groundwater flow).

    THIS IS THE ONE FIELD THE BAKE DOES NOT HAVE, and everything downstream
    depends on it: phreatic vs vadose, which passages are tubes and which are
    canyons, where springs sit, and how multi-level systems stack. The solve is
    deliberately crude -- constant conductivity, fixed iteration count -- because
    its job here is to produce a plausible SHAPE for judging, not a calibrated
    head. It is clamped to the topographic surface: groundwater does not stand
    above the ground, it becomes a spring.
    """
    h = elev_m.copy()
    fixed = drain
    r = recharge * (GRID_M ** 2) * 1e-4
    for _ in range(iters):
        nb = np.empty_like(h)
        nb[1:-1, 1:-1] = 0.25 * (h[:-2, 1:-1] + h[2:, 1:-1] + h[1:-1, :-2] + h[1:-1, 2:])
        nb[0, :] = h[0, :]; nb[-1, :] = h[-1, :]
        nb[:, 0] = h[:, 0]; nb[:, -1] = h[:, -1]
        h = nb + r
        np.minimum(h, elev_m, out=h)          # never above ground
        h[fixed] = elev_m[fixed]              # Dirichlet at drains
    return h


# ---------------------------------------------------------------------------
# 2. Sinks and springs, from the bake's own hydrology
# ---------------------------------------------------------------------------

TERMINAL_KINDS = {tc.BASIN_KIND_DRY_PLAYA, tc.BASIN_KIND_SALT_FLAT,
                  tc.BASIN_KIND_SEASONAL, tc.BASIN_KIND_LAKE_TERMINAL}


def find_sinks(f: dict, incept_surface: np.ndarray, stream_log2: float = 18.0):
    """Ponors. Two independent sources, tagged so the mix is visible.

    * TERMINAL BASINS -- a basin whose water does not leave by the surface is,
      by definition, water going down. This is the strongest evidence the tile
      carries and it needs no threshold.
    * STREAM SINKS -- a channel cell crossing an inception-horizon outcrop. This
      is the textbook karst rule (a surface stream sinks where it meets a
      soluble contact) and it is why the horizon field has to exist before the
      sinks can be found.
    """
    pts, kinds = [], []
    for b in f["basins"]:
        if b.kind in TERMINAL_KINDS:
            px, py = int(b.seed_px[0]) // DOWN, int(b.seed_px[1]) // DOWN
            if 0 <= px < GRID_N and 0 <= py < GRID_N:
                pts.append((px, py)); kinds.append("basin")

    stream = f["log2acc"] >= stream_log2
    outcrop = np.abs(incept_surface) < 0.10
    ys, xs = np.nonzero(stream & outcrop)
    for x, y in zip(xs, ys):
        pts.append((int(x), int(y))); kinds.append("stream")
    return pts, kinds


def find_springs(f: dict, head: np.ndarray, incept_surface: np.ndarray,
                 stream_log2: float = 18.0):
    """Resurgences: valley-floor cells where the water table meets the surface
    and a horizon daylights, plus the coast.

    CROSS-CHECK AVAILABLE FOR FREE: the tile already ships SECTION_HEADWATERS,
    "where drawn water starts". A stream that appears from nowhere at full
    discharge IS a karst resurgence, so springs and baked headwaters should
    overlap. If they do not, the model is wrong -- and that is an automated test
    rather than an opinion. `spring_head_overlap` below reports it.
    """
    elev = f["elev_m"]
    at_table = (elev - head) < 1.0
    stream = f["log2acc"] >= stream_log2
    outcrop = np.abs(incept_surface) < 0.15
    sea = elev <= 0.5
    mask = (at_table & stream & outcrop) | (sea & stream)
    ys, xs = np.nonzero(mask)
    return [(int(x), int(y)) for x, y in zip(xs, ys)]


def spring_head_overlap(springs, heads, tol_cells: int = 3) -> float:
    """Fraction of baked headwaters within `tol_cells` of a modelled spring."""
    if not springs or not heads:
        return float("nan")
    tree = cKDTree(np.asarray(springs, float))
    # HeadEntry.px is the (x, y) PAIR, not an x -- see tile_codec's HeadEntry.
    hp = np.array([[h.px[0] / DOWN, h.px[1] / DOWN] for h in heads], float)
    d, _ = tree.query(hp)
    return float(np.mean(d <= tol_cells))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tile", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("karst-out"))
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    t0 = time.time()
    f = load_tile(args.tile)
    seed = args.seed or (f["seed"] & 0xFFFFFFFF)
    print(f"tile {f['tile']}  bake_ver {f['bake_ver']}  seed {seed}")
    print(f"  elevation  {f['elev_m'].min():8.1f} .. {f['elev_m'].max():8.1f} m")
    print(f"  basins {len(f['basins'])}  heads {len(f['heads'])}  "
          f"lake cells {int(f['lake'].sum())}")

    xs = (np.arange(GRID_N) + f["tx"] * GRID_N) * GRID_M
    ys = (np.arange(GRID_N) + f["ty"] * GRID_N) * GRID_M
    X, Y = np.meshgrid(xs, ys)

    incept_surface = inception_field(f["elev_m"], X, Y, seed)
    theta, coher = fracture_fabric(f["elev_m"])
    drain = (f["log2acc"] >= 20.0) | f["lake"] | (f["elev_m"] <= 0.5)
    print(f"  drain cells {int(drain.sum())} ({100.0 * drain.mean():.1f}%)")

    head = water_table(f["elev_m"], drain)
    depth = f["elev_m"] - head
    print(f"  water table depth  mean {depth.mean():6.1f} m   "
          f"p90 {np.percentile(depth, 90):6.1f} m   max {depth.max():6.1f} m")
    print(f"  fracture coherence mean {coher.mean():.3f}")

    sinks, kinds = find_sinks(f, incept_surface)
    springs = find_springs(f, head, incept_surface)
    nb = sum(1 for k in kinds if k == "basin")
    print(f"  sinks {len(sinks)}  (terminal basins {nb}, stream/horizon {len(sinks) - nb})")
    print(f"  springs {len(springs)}")
    ov = spring_head_overlap(springs, f["heads"])
    print(f"  baked headwaters within 3 cells of a modelled spring: {100 * ov:.1f}%")

    stats = {
        "tile": f["tile"], "seed": seed, "bake_ver": f["bake_ver"],
        "grid_m": GRID_M, "grid_n": GRID_N, "exaggeration": EXAGGERATION,
        "elev_min_m": float(f["elev_m"].min()), "elev_max_m": float(f["elev_m"].max()),
        "basins": len(f["basins"]), "heads": len(f["heads"]),
        "drain_frac": float(drain.mean()),
        "wt_depth_mean_m": float(depth.mean()),
        "wt_depth_p90_m": float(np.percentile(depth, 90)),
        "fracture_coherence_mean": float(coher.mean()),
        "sinks": len(sinks), "sinks_from_basins": nb,
        "springs": len(springs),
        "spring_headwater_overlap": None if ov != ov else float(ov),
        "seconds": round(time.time() - t0, 2),
    }
    (args.out / f"{f['tile']}-fields.json").write_text(json.dumps(stats, indent=2))
    fold = inception_fold(X, Y, seed)
    np.savez_compressed(args.out / f"{f['tile']}-fields.npz",
                        fold_m=fold.astype(np.float32),
                        seed_used=np.int64(seed),
                        elev_m=f["elev_m"].astype(np.float32),
                        head_m=head.astype(np.float32),
                        incept=incept_surface.astype(np.float32),
                        theta=theta.astype(np.float32),
                        coher=coher.astype(np.float32),
                        log2acc=f["log2acc"].astype(np.float32),
                        drain=drain,
                        sinks=np.asarray(sinks, np.int32).reshape(-1, 2),
                        springs=np.asarray(springs, np.int32).reshape(-1, 2))
    print(f"\nwrote {args.out}/{f['tile']}-fields.{{json,npz}}  ({stats['seconds']}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
