#!/usr/bin/env python3
"""Render a designer-facing world map: topography, sea level, and biomes.

WHAT THIS IS FOR. Picking where to pregen. `scan_land.py` answers "is there
land here" one expensive real tile at a time; this answers "what KIND of place
is here" across hundreds of kilometres in one image, off the COARSE model,
which is tiny and runs fine on CPU.

WHERE THE DATA COMES FROM. The terrain-diffusion repo already ships a web
explorer (`terrain_diffusion/inference/explorer/`) that serves the coarse world
over HTTP. Start it first -- it is a declared dependency of that repo, not
something this tool installs:

    cd <terrain-diffusion>
    python -c "from terrain_diffusion.inference.explorer.server import main; \\
               main(['--port','8899','--seed','20260719','--device','cpu'], \\
                    standalone_mode=False)"

then point this at it. Browsing/filtering interactively is what that UI is FOR
and this tool does not replace it; what it adds is the two things the UI has no
way to know about -- OUR sea level and OUR biome classification.

WHY THE THRESHOLDS ARE PARSED, NOT COPIED. voxel-core/include/voxelcore/biome.h
is the single source of truth for what counts as a desert. Copying its numbers
here would create a FOURTH independent climate calibration in this repo, which
is precisely the bug that made the whole world classify as desert for weeks
(biome.h, its HLSL mirror, VoxelClimateProbe's remap and gen_terrain_textures'
LUT had all drifted apart). So the thresholds are read out of the header at
run time. If someone retunes biome.h, this map retunes with it; if someone
renames a constant, this fails loudly instead of drawing a stale map.

HONEST LIMITS, because a map that looks authoritative and is not is worse than
no map:

  * COARSE, NOT THE GAME. One coarse cell is 256 fine pixels = 7.68 km. The
    game classifies per 30 m tile pixel from real generated tiles. This is a
    preview of climate zones, not a screenshot of the world.
  * NO SLOPE GATE. biome.h's cliff gate (BARE_ROCK) and its treeline both key
    off terrain steepness or elevation at 30 m scale. Slope is meaningless
    across a 7.68 km cell, so the cliff gate is omitted entirely and the
    treeline is applied to the cell's mean elevation. Real terrain will have
    considerably more rock and alpine than this map shows.
  * The coarse model is a SKETCH the fine model refines. Coastlines especially
    move between this and a real tile.

Usage:
    python tools/world_map.py --out world.png
    python tools/world_map.py --window -60,60,-60,60 --out wide.png
    python tools/world_map.py --verify-axes    # see check_axis_mapping()
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.request
from pathlib import Path

import numpy as np

# --- geometry ---------------------------------------------------------------

#: Fine (30 m) pixels per coarse cell. The 30 m checkpoint upsamples the coarse
#: sketch 256x on both axes (terrain-diffusion README, "TIFF export"), which is
#: what makes a coarse cell 256 * 30 m = 7.68 km -- the "7.7 km coarse" the
#: model card quotes.
FINE_PER_COARSE = 256
#: tile_codec.TILE_SIZE. A tile is therefore exactly 2 coarse cells across.
TILE_SIZE = 512
COARSE_PER_TILE = TILE_SIZE // FINE_PER_COARSE

#: The explorer indexes world.coarse[:, ci0:ci1, cj0:cj1], and diffusion.py's
#: backend maps i (rows) to tile Y and j (cols) to tile X. So ci <-> y, cj <-> x.
#: That mapping is exactly the `# ASSUMPTION:` still flagged in
#: TerrainDiffusionBackend.generate_rasters -- see check_axis_mapping().

_REPO = Path(__file__).resolve().parents[2]
_BIOME_H = _REPO / "voxel-core" / "include" / "voxelcore" / "biome.h"


# --- thresholds, read out of biome.h ---------------------------------------

def _read_constants() -> dict:
    """Pull biome.h's thresholds as PHYSICAL values.

    Since worldgen v8 the header states them physically and converts at compile
    time -- `kBiomeTempColdU8 = climateTempU8FromDegC(5)` -- so the physical
    number is right there in the source and this never has to touch the u8
    encoding at all. The coarse API already serves degrees C and mm/yr, so the
    whole comparison happens in physical units and quantization never enters.
    """
    if not _BIOME_H.is_file():
        sys.exit(f"cannot find {_BIOME_H} -- run this from inside the repo")
    src = _BIOME_H.read_text(encoding="utf-8")

    def phys(const: str, fn: str) -> float:
        m = re.search(rf"{const}\s*=\s*{fn}\(\s*(-?[\d']+)\s*\)", src)
        if not m:
            sys.exit(
                f"biome.h no longer defines {const} as {fn}(<physical value>). This tool "
                "reads the header so it cannot drift from the game; update it in the same "
                "commit that changed the header."
            )
        return float(m.group(1).replace("'", ""))

    def plain(const: str) -> float:
        m = re.search(rf"{const}\s*=\s*(-?[\d']+)\s*;", src)
        if not m:
            sys.exit(f"biome.h no longer defines {const} as a plain integer.")
        return float(m.group(1).replace("'", ""))

    return {
        "temp_cold_c": phys("kBiomeTempColdU8", "climateTempU8FromDegC"),
        "temp_warm_c": phys("kBiomeTempWarmU8", "climateTempU8FromDegC"),
        "temp_hot_c": phys("kBiomeTempHotU8", "climateTempU8FromDegC"),
        "precip_arid_mm": phys("kBiomePrecipAridU8", "climatePrecipU8FromMmPerYr"),
        "precip_semi_mm": phys("kBiomePrecipSemiU8", "climatePrecipU8FromMmPerYr"),
        "precip_mod_mm": phys("kBiomePrecipModU8", "climatePrecipU8FromMmPerYr"),
        "seasonal_high": phys("kBiomeSeasonalHighU8", "climateSeasonalityU8From"),
        "beach_lower_m": plain("kBiomeBeachLowerMm") / 1000.0,
        "beach_upper_m": plain("kBiomeBeachUpperMm") / 1000.0,
        "treeline_base_m": plain("kBiomeTreelineBaseMm") / 1000.0,
        "treeline_m_per_c": plain("kBiomeTreelineMmPerDegC") / 1000.0,
    }


# Names and colours. Order matches vxc::BiomeId; BARE_ROCK is omitted because
# the slope gate that produces it cannot be evaluated at coarse scale.
BIOMES = [
    ("OCEAN", (0.09, 0.20, 0.38)),
    ("BEACH", (0.85, 0.79, 0.56)),
    ("GRASSLAND", (0.55, 0.66, 0.32)),
    ("TEMPERATE_FOREST", (0.20, 0.42, 0.22)),
    ("RAINFOREST", (0.08, 0.31, 0.17)),
    ("DESERT", (0.80, 0.68, 0.44)),
    ("SAVANNA", (0.72, 0.68, 0.34)),
    ("TAIGA", (0.29, 0.40, 0.34)),
    ("TUNDRA_ALPINE", (0.72, 0.74, 0.76)),
]
OCEAN, BEACH, GRASSLAND, TEMPERATE_FOREST, RAINFOREST, DESERT, SAVANNA, TAIGA, TUNDRA_ALPINE = range(9)


def classify(elev_m, temp_c, precip_mm, seasonality, k) -> np.ndarray:
    """Vectorised mirror of vxc::classifyBiome, in physical units.

    Gate order follows the header exactly -- sea level, then treeline, then the
    Whittaker table. The cliff gate is deliberately absent (see module docstring).
    """
    b = np.full(elev_m.shape, -1, dtype=np.int8)
    b = np.where((b < 0) & (elev_m < k["beach_lower_m"]), OCEAN, b)
    b = np.where((b < 0) & (elev_m <= k["beach_upper_m"]), BEACH, b)
    treeline = np.maximum(
        k["treeline_base_m"] + temp_c * k["treeline_m_per_c"], k["beach_upper_m"]
    )
    b = np.where((b < 0) & (elev_m > treeline), TUNDRA_ALPINE, b)
    b = np.where((b < 0) & (temp_c < k["temp_cold_c"]), TAIGA, b)
    warm = temp_c >= k["temp_warm_c"]
    hot = temp_c >= k["temp_hot_c"]
    seasonal = seasonality >= k["seasonal_high"]
    b = np.where((b < 0) & (precip_mm < k["precip_arid_mm"]), np.where(hot, DESERT, GRASSLAND), b)
    b = np.where((b < 0) & (precip_mm < k["precip_semi_mm"]),
                 np.where(warm & seasonal, SAVANNA, GRASSLAND), b)
    b = np.where((b < 0) & (precip_mm < k["precip_mod_mm"]),
                 np.where(warm & seasonal, SAVANNA, TEMPERATE_FOREST), b)
    return np.where(b < 0, np.where(warm, RAINFOREST, TEMPERATE_FOREST), b).astype(np.int8)


# --- data ------------------------------------------------------------------

def fetch(url: str, ci0: int, ci1: int, cj0: int, cj1: int) -> dict:
    q = f"{url}/api/coarse_data.json?ci0={ci0}&ci1={ci1}&cj0={cj0}&cj1={cj1}"
    try:
        with urllib.request.urlopen(q, timeout=1800) as r:
            payload = json.load(r)
    except Exception as e:
        sys.exit(
            f"could not reach the coarse API at {url} ({e}).\n"
            "Start the terrain-diffusion explorer first -- see this file's docstring."
        )
    if "error" in payload:
        sys.exit(f"coarse API error: {payload['error']}")
    ch = payload["channels"]
    return {
        "elev": np.array(ch["Elev"], dtype=np.float64),
        "temp": np.array(ch["Temp"], dtype=np.float64),
        "tstd": np.array(ch["T std"], dtype=np.float64),
        "precip": np.array(ch["Precip"], dtype=np.float64),
    }


# --- image processing (numpy only; no new dependency) ------------------------

def _box_blur(a: np.ndarray, r: int) -> np.ndarray:
    """Separable box blur with edge clamping, via cumsum. O(n) per axis."""
    if r < 1:
        return a
    out = a
    for axis in (0, 1):
        pad = [(0, 0), (0, 0)]
        pad[axis] = (r, r)
        padded = np.pad(out, pad, mode="edge")
        c = np.cumsum(padded, axis=axis)
        lo = np.take(c, np.arange(0, out.shape[axis]), axis=axis)
        hi = np.take(c, np.arange(2 * r, 2 * r + out.shape[axis]), axis=axis)
        out = (hi - lo) / (2 * r)
    return out


def _gaussian(a: np.ndarray, sigma: float) -> np.ndarray:
    """Three box passes approximate a gaussian closely enough for shading."""
    if sigma <= 0:
        return a
    r = max(1, int(round(sigma * 1.2)))
    return _box_blur(_box_blur(_box_blur(a, r), r), r)


def upsample(a: np.ndarray, factor: int) -> np.ndarray:
    """Nearest-replicate then smooth -- a cheap smooth interpolation.

    THIS ADDS NO INFORMATION. The coarse model's resolution is 7.68 km and
    nothing here changes that; what it removes is the 7.68 km staircase, which
    otherwise dominates the hillshade and makes real landforms unreadable.
    Every rendered pixel between coarse samples is interpolation and the map
    says so in its subtitle.
    """
    big = np.kron(a, np.ones((factor, factor)))
    return _gaussian(big, factor * 0.55)


def hillshade(elev_m: np.ndarray, cell_m: float, altitude=45.0,
              azimuths=(315.0, 45.0, 135.0, 225.0), relief=1.0) -> np.ndarray:
    """Multi-scale, multi-azimuth hillshade in [0, 1].

    Two techniques, both from terrain-diffusion's own relief_map.py, because
    they are what make a heightfield read as landform rather than as noise:

      * MULTI-SCALE -- shade a heavily blurred copy and a lightly blurred copy
        and mix 3:1. The blurred copy carries ranges and basins; the sharp one
        carries ridgelines. A single-scale shade either flattens the big
        structure or drowns in small stuff.
      * MULTI-AZIMUTH -- average four sun positions. One low sun throws long
        shadows that hide everything on the lee side; four fills them while
        keeping the direction cue.

    `relief` exaggerates vertically. Terrain is far wider than it is tall, so
    a physically honest 1:1 shade of a 768 km window is nearly flat; 2-4x is
    standard practice for readability.
    """
    e = elev_m * relief
    big, small = _gaussian(e, 2.5), _gaussian(e, 0.8)
    acc = np.zeros_like(e)
    for src, weight in ((big, 0.75), (small, 0.25)):
        dy, dx = np.gradient(src, cell_m)
        slope = np.arctan(np.hypot(dx, dy))
        aspect = np.arctan2(-dx, dy)
        alt = np.radians(altitude)
        for az_deg in azimuths:
            az = np.radians(360.0 - az_deg + 90.0)
            hs = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
            acc += weight * np.clip(hs, 0.0, 1.0) / len(azimuths)
    return np.power(np.clip(acc, 0.0, 1.0), 0.85)


#: Hypsometric ramp: the cartographic convention for "how high is it", in
#: metres. Land only -- sea gets its own bathymetric ramp.
HYPSO = [
    (0, (0.36, 0.51, 0.31)),
    (200, (0.52, 0.60, 0.35)),
    (600, (0.71, 0.68, 0.42)),
    (1200, (0.72, 0.58, 0.38)),
    (2000, (0.62, 0.47, 0.38)),
    (2800, (0.70, 0.68, 0.68)),
    (3600, (0.95, 0.95, 0.96)),
]


def hypsometric(elev_m: np.ndarray) -> np.ndarray:
    stops = np.array([s for s, _ in HYPSO], dtype=np.float64)
    cols = np.array([c for _, c in HYPSO], dtype=np.float64)
    e = np.clip(elev_m, stops[0], stops[-1])
    return np.stack([np.interp(e, stops, cols[:, i]) for i in range(3)], axis=-1)


def bathymetric(elev_m: np.ndarray) -> np.ndarray:
    """Depth ramp. Shelf must read differently from abyss or the coast vanishes."""
    d = np.clip(-elev_m / 6000.0, 0.0, 1.0)
    return np.stack([0.18 - 0.12 * d, 0.38 - 0.24 * d, 0.62 - 0.30 * d], axis=-1)


def compose(elev, biome, k, style="blend", relief=3.0, cell_m=None, sea_level=None):
    """Colour + shade one field. Shared by the coarse and detail paths."""
    sea_level = k["beach_lower_m"] if sea_level is None else sea_level
    sea = elev < sea_level

    if style == "biome" and biome is not None:
        rgb = np.zeros(elev.shape + (3,))
        for i, (_, colour) in enumerate(BIOMES):
            rgb[biome == i] = colour
    elif style == "elevation" or biome is None:
        rgb = hypsometric(elev)
    else:
        # Height decides the colour, biome tints it. Elevation is what the eye
        # should read first -- a flat per-biome fill hides all topography
        # inside a biome, which is most of the map.
        rgb = hypsometric(elev)
        tint = np.zeros(elev.shape + (3,))
        for i, (_, colour) in enumerate(BIOMES):
            tint[biome == i] = colour
        # Smooth the tint COLOUR, never the biome ID. Blending a grassland
        # green into a forest green is an honest fade across a boundary whose
        # true position is unknown to within 7.68 km; blending the IDS would
        # manufacture a category that does not exist, and the legend's
        # percentages are still counted off the categorical map.
        tint = np.stack([_gaussian(tint[..., c], 3.0) for c in range(3)], axis=-1)
        rgb = 0.62 * rgb + 0.38 * tint

    rgb[sea] = bathymetric(elev)[sea]
    shade = hillshade(elev, cell_m, relief=relief)
    # Shade the sea too, but weakly -- it keeps basins legible without implying
    # seafloor detail the coarse sketch does not have.
    w = np.where(sea, 0.25, 0.85)
    rgb *= (1.0 - w + w * (0.45 + 1.15 * shade))[..., None]
    return np.clip(rgb, 0, 1), sea


# --- the axis check --------------------------------------------------------

def check_axis_mapping(url: str, tile_dir: Path) -> None:
    """Settle diffusion.py's open `# ASSUMPTION:` about tile axis order.

    TerrainDiffusionBackend.generate_rasters maps `i1 = y*TILE_SIZE,
    j1 = x*TILE_SIZE` and says outright that WorldPipeline.get's (i, j) order
    "was not independently confirmed from source alone -- verify at GPU
    bring-up". It never was.

    It is checkable for free now, without a GPU: the same coarse model underlies
    both, so the mean elevation of each generated .vxtl tile must correlate with
    the coarse cells at its own footprint. Correlate both orientations; the
    right one wins by a wide margin, and if they tie, the check is inconclusive
    rather than confirmatory and says so.
    """
    sys.path.insert(0, str(_REPO / "terrain-service"))
    from terrain_service.tile_codec import decode  # noqa: E402

    tiles = sorted(tile_dir.glob("*.vxtl"))
    if not tiles:
        sys.exit(f"no .vxtl tiles under {tile_dir}")

    means: dict[tuple[int, int], float] = {}
    for p in tiles:
        t = decode(p.read_bytes())
        means[(t.x, t.y)] = float(t.elevation.astype(np.float64).mean())
    xs = [x for x, _ in means]
    ys = [y for _, y in means]
    print(f"{len(means)} tiles, x=[{min(xs)},{max(xs)}] y=[{min(ys)},{max(ys)}]")

    # Coarse window covering every tile under EITHER orientation.
    lo = min(min(xs), min(ys)) * COARSE_PER_TILE
    hi = (max(max(xs), max(ys)) + 1) * COARSE_PER_TILE
    print(f"fetching coarse window [{lo},{hi}) on both axes ...")
    d = fetch(url, lo, hi, lo, hi)
    coarse = d["elev"]

    def cell_mean(ci: int, cj: int) -> float:
        i, j = ci - lo, cj - lo
        block = coarse[i:i + COARSE_PER_TILE, j:j + COARSE_PER_TILE]
        return float(block.mean()) if block.size else np.nan

    for label, (ci_of, cj_of) in (
        ("ci<-y, cj<-x  (what the code assumes)", (lambda x, y: y, lambda x, y: x)),
        ("ci<-x, cj<-y  (transposed)", (lambda x, y: x, lambda x, y: y)),
    ):
        a, b = [], []
        for (x, y), m in means.items():
            c = cell_mean(ci_of(x, y) * COARSE_PER_TILE, cj_of(x, y) * COARSE_PER_TILE)
            if not np.isnan(c):
                a.append(m)
                b.append(c)
        r = np.corrcoef(a, b)[0, 1] if len(a) > 2 else float("nan")
        print(f"  {label}: r = {r:+.3f}  over {len(a)} tiles")
    print(
        "\nThe orientation with the clearly higher correlation is the real one. If they "
        "are close, this is INCONCLUSIVE (a near-symmetric region cannot tell them "
        "apart) -- say so rather than recording a confirmation."
    )


def set_seed(url: str, seed: int) -> None:
    """Re-seed the running pipeline in place -- no restart needed.

    WorldPipeline.change_seed() rebuilds the internal tile hierarchy, so every
    coarse cell must be regenerated afterwards; a re-seed saves reloading the
    model, not the generation cost.
    """
    req = urllib.request.Request(
        f"{url}/api/seed", data=json.dumps({"seed": int(seed)}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            got = json.load(r).get("seed")
    except Exception as e:
        sys.exit(f"could not set seed on {url}: {e}")
    print(f"pipeline re-seeded to {got}")


def inland_reach_km(elev: np.ndarray, sea_level: float) -> float:
    """Farthest any land cell sits from open water, in km.

    THE METRIC THAT PREDICTS WHETHER A SEED CAN HAVE DESERTS. Aridity on Earth
    is mostly continentality and rain shadow: moisture comes off the ocean and
    is spent crossing land. A world of islands cannot be dry however hot it is,
    which is exactly what seed 20260719 turned out to be -- 6.9% of its land is
    arid, but hot desert covers 9 cells in a million km2, because no land is
    ever far from the sea.

    Land FRACTION does not tell you this: an archipelago and a continent can
    have identical land fractions and completely different climates. So this is
    the number to compare seeds on, alongside the biome census.

    Chamfer distance by iterative dilation -- these grids are ~64x64, so the
    naive version is instant and worth the clarity.
    """
    land = elev > sea_level
    if not land.any():
        return 0.0
    reach = np.zeros(elev.shape, dtype=np.int32)
    frontier = ~land
    for step in range(1, max(elev.shape) + 1):
        grown = frontier.copy()
        grown[1:, :] |= frontier[:-1, :]
        grown[:-1, :] |= frontier[1:, :]
        grown[:, 1:] |= frontier[:, :-1]
        grown[:, :-1] |= frontier[:, 1:]
        newly = grown & ~frontier & land
        if not newly.any():
            break
        reach[newly] = step
        frontier = grown
    return float(reach.max()) * FINE_PER_COARSE * 30 / 1000


def audition(url: str, seeds: list, half: int, k: dict, out: str) -> None:
    """Render several seeds side by side with the numbers that decide between them."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    n = len(seeds)
    cols = 2 if n <= 4 else 3
    rows = (n + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(6.2 * cols, 7.0 * rows), dpi=110)
    axes = np.atleast_1d(axes).ravel()
    span_km = 2 * half * FINE_PER_COARSE * 30 / 1000

    for ax, seed in zip(axes, seeds):
        set_seed(url, seed)
        print(f"  rendering seed {seed} ...")
        d = fetch(url, -half, half, -half, half)
        biome = classify(d["elev"], d["temp"], d["precip"], d["tstd"], k)
        f = 6
        elev = upsample(d["elev"], f)
        b_up = np.kron(biome, np.ones((f, f), dtype=np.int8))
        rgb, _ = compose(elev, b_up, k, style="blend", relief=3.0,
                         cell_m=FINE_PER_COARSE * 30.0 / f)
        ax.imshow(rgb, origin="upper", interpolation="bilinear")
        ax.set_xticks([])
        ax.set_yticks([])

        land = d["elev"] > k["beach_upper_m"]
        reach = inland_reach_km(d["elev"], k["beach_lower_m"])
        counts = np.bincount(biome.ravel(), minlength=len(BIOMES)) / biome.size * 100
        present = [BIOMES[i][0] for i in range(len(BIOMES)) if counts[i] >= 1.0]
        tmed = float(np.median(d["temp"][land])) if land.any() else float("nan")
        pmed = float(np.median(d["precip"][land])) if land.any() else float("nan")
        ax.set_title(
            "seed {}\nland {:.0f}%  |  inland reach {:.0f} km  |  peak {:.0f} m\n"
            "land median {:.1f} C, {:.0f} mm/yr  |  {} biomes >=1%\n{}".format(
                seed, land.mean() * 100, reach, d["elev"].max(), tmed, pmed,
                len(present), ", ".join(x.lower() for x in present)),
            fontsize=8.5)

    for ax in axes[n:]:
        ax.axis("off")
    fig.suptitle(
        "seed audition -- {:.0f} km window each, same thresholds from biome.h\n"
        "INLAND REACH is the desert predictor: an archipelago cannot be dry. "
        "This window caps it at {:.0f} km, so a reading well below that is real terrain, "
        "not a measurement artifact.".format(span_km, span_km / 2),
        fontsize=10, y=0.995)
    fig.tight_layout(rect=[0, 0, 1, 0.97])
    fig.savefig(out, bbox_inches="tight")
    print("\nwrote " + out)


def fetch_detail(url: str, ci: int, cj: int, size: int) -> np.ndarray:
    """Real 30 m elevation for one window, straight off the full pipeline.

    Genuinely finer than the coarse map rather than interpolated -- and priced
    accordingly: ~2.5 min for a 512 window on CPU against ~35 s for an entire
    768 km coarse view. Browse coarse, then spend this on the one place worth
    looking at closely.

    Elevation only, by necessity: the endpoint carries temperature but not
    precipitation, so a full biome classification is impossible here. Which is
    fine -- this path exists to show topography.
    """
    q = f"{url}/api/detail_raw?ci={ci}&cj={cj}&detail_size={size}"
    print(f"fetching REAL 30 m detail at coarse ({ci},{cj}), {size}px = "
          f"{size * 30 / 1000:.1f} km across.\n"
          "This runs the full diffusion model -- minutes, not seconds ...")
    try:
        with urllib.request.urlopen(q, timeout=3600) as r:
            h, w = int(r.headers["X-Height"]), int(r.headers["X-Width"])
            raw = r.read()
    except Exception as e:
        sys.exit(f"detail fetch failed: {e}")
    return np.frombuffer(raw[: h * w * 2], dtype="<i2").reshape(h, w).astype(np.float64)


def _draw(rgb, elev, extent, xlabel, ylabel, title, out,
          contour_m=0, legend=None, box=None):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(13, 13), dpi=125)
    ax.imshow(rgb, origin="upper", interpolation="bilinear", extent=extent)

    if contour_m:
        xs = np.linspace(extent[0], extent[1], elev.shape[1])
        ys = np.linspace(extent[3], extent[2], elev.shape[0])
        hi = float(np.nanmax(elev))
        levels = np.arange(contour_m, hi + contour_m, contour_m)
        if len(levels):
            cs = ax.contour(xs, ys, elev, levels=levels, colors="black",
                            linewidths=0.35, alpha=0.30)
            if len(cs.levels) > 1:
                ax.clabel(cs, cs.levels[::2], inline=True, fontsize=6, fmt="%d")
        # The coastline, drawn solid. Single most useful line on the map.
        ax.contour(xs, ys, elev, levels=[0], colors="black", linewidths=1.0, alpha=0.7)

    if box is not None:
        x0, y0, x1, y1 = box
        ax.add_patch(plt.Rectangle((x0, y0), x1 - x0, y1 - y0, fill=False,
                                   edgecolor="red", linewidth=2.0))
        ax.text(x0, y0 - 0.6, "generated tiles", color="red", fontsize=9, weight="bold")

    if legend:
        handles = [plt.Rectangle((0, 0), 1, 1, color=c) for _, c in BIOMES]
        ax.legend(handles, legend, loc="upper left", bbox_to_anchor=(1.01, 1.0),
                  fontsize=8, frameon=False, title="biome tint")
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title, fontsize=10)
    fig.tight_layout()
    fig.savefig(out, bbox_inches="tight")
    print(f"\nwrote {out}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="http://127.0.0.1:8899")
    ap.add_argument("--window", default="-50,50,-50,50", help="ci0,ci1,cj0,cj1")
    ap.add_argument("--out", default="world_map.png")
    ap.add_argument("--tile-dir", default=None,
                    help="outline the generated tiles' footprint on the map")
    ap.add_argument("--verify-axes", action="store_true")
    ap.add_argument("--style", choices=("blend", "elevation", "biome"), default="blend")
    ap.add_argument("--relief", type=float, default=3.0,
                    help="vertical exaggeration for shading (default 3)")
    ap.add_argument("--smooth", type=int, default=8,
                    help="render upsample factor; INTERPOLATION only, adds no data")
    ap.add_argument("--contours", type=int, default=500,
                    help="contour interval in metres; 0 disables")
    ap.add_argument("--detail", default=None,
                    help="ci,cj[,size]: REAL 30 m data for one window (slow)")
    ap.add_argument("--seed", type=int, default=None,
                    help="re-seed the running pipeline before rendering")
    ap.add_argument("--sea-level", type=float, default=0.0,
                    help="render as if sea level were at this elevation (metres). "
                         "A VIEW only: the game's sea level is z=0. Lowering the sea by X "
                         "is arithmetically raising every elevation by X, so no regeneration "
                         "is needed.")
    ap.add_argument("--audition", default=None,
                    help="comma-separated seeds: contact sheet comparing candidate worlds")
    ap.add_argument("--audition-half", type=int, default=32,
                    help="half-width in coarse cells per panel (default 32 = 492 km)")
    args = ap.parse_args()

    if args.verify_axes:
        if not args.tile_dir:
            sys.exit("--verify-axes needs --tile-dir")
        check_axis_mapping(args.url, Path(args.tile_dir))
        return

    k = _read_constants()

    if args.audition:
        seeds = [int(v) for v in args.audition.split(",")]
        print(f"auditioning {len(seeds)} seeds. Each needs a fresh coarse generation "
              "(change_seed rebuilds the tile hierarchy), so allow ~15 s per seed.\n")
        audition(args.url, seeds, args.audition_half, k, args.out)
        return

    if args.seed is not None:
        set_seed(args.url, args.seed)

    if args.detail:
        parts = [int(v) for v in args.detail.split(",")]
        ci, cj = parts[0], parts[1]
        size = parts[2] if len(parts) > 2 else 512
        elev = fetch_detail(args.url, ci, cj, size)
        rgb, _ = compose(elev, None, k, style="elevation",
                         relief=max(1.0, args.relief / 3.0), cell_m=30.0)
        km = size * 30 / 1000
        _draw(rgb, elev, [0, km, km, 0], "km east", "km south",
              f"REAL 30 m terrain at coarse ({ci},{cj}) -- {km:.1f} km across, "
              f"no interpolation\nelevation {elev.min():.0f} to {elev.max():.0f} m, "
              f"{args.contours} m contours",
              args.out, contour_m=args.contours)
        print(f"  elevation {elev.min():.0f} .. {elev.max():.0f} m, "
              f"land {(elev > 0).mean() * 100:.1f}%")
        return

    ci0, ci1, cj0, cj1 = (int(v) for v in args.window.split(","))
    print("thresholds read from biome.h:")
    print(f"  temperature bands  cold <{k['temp_cold_c']:g}C  warm >={k['temp_warm_c']:g}C  "
          f"hot >={k['temp_hot_c']:g}C")
    print(f"  precipitation      arid <{k['precip_arid_mm']:g}  semi <{k['precip_semi_mm']:g}  "
          f"moderate <{k['precip_mod_mm']:g} mm/yr")
    print(f"  treeline           {k['treeline_base_m']:g} m at 0C, "
          f"{k['treeline_m_per_c']:g} m/C")

    span_km = (cj1 - cj0) * FINE_PER_COARSE * 30 / 1000
    print(f"\nfetching coarse window ci=[{ci0},{ci1}) cj=[{cj0},{cj1}) "
          f"= {span_km:.0f} km across (first call is slow; the model caches) ...")
    d = fetch(args.url, ci0, ci1, cj0, cj1)

    if args.sea_level:
        # Lowering the sea by X == raising the land by X. Doing it this way
        # keeps z=0 as "the waterline" for every downstream gate, so the biome
        # coastal band and the colour ramps all stay consistent without any of
        # them needing to know a sea level was applied.
        d["elev"] = d["elev"] - args.sea_level
        print(f"rendering at sea level {args.sea_level:+.0f} m (a VIEW; the game's is z=0)")

    biome_coarse = classify(d["elev"], d["temp"], d["precip"], d["tstd"], k)
    f = max(1, args.smooth)
    elev = upsample(d["elev"], f)
    # Biome is CATEGORICAL, so replicate it -- never interpolate between ids.
    # Blending DESERT with OCEAN would invent a biome that does not exist.
    biome = np.kron(biome_coarse, np.ones((f, f), dtype=np.int8))
    cell_m = FINE_PER_COARSE * 30.0 / f

    rgb, _ = compose(elev, biome, k, style=args.style, relief=args.relief, cell_m=cell_m)

    box = None
    if args.tile_dir:
        sys.path.insert(0, str(_REPO / "terrain-service"))
        from terrain_service.tile_codec import decode
        tp = sorted(Path(args.tile_dir).glob("*.vxtl"))
        if tp:
            xs, ys = [], []
            for q in tp:
                t = decode(q.read_bytes())
                xs.append(t.x)
                ys.append(t.y)
            box = (min(xs) * COARSE_PER_TILE, min(ys) * COARSE_PER_TILE,
                   (max(xs) + 1) * COARSE_PER_TILE, (max(ys) + 1) * COARSE_PER_TILE)

    counts = np.bincount(biome_coarse.ravel(), minlength=len(BIOMES))
    total = biome_coarse.size
    legend = [f"{n}  {100 * counts[i] / total:.1f}%" for i, (n, _) in enumerate(BIOMES)]
    _draw(rgb, elev, [cj0, cj1, ci1, ci0],
          "coarse cj  (tile x = cj/2)", "coarse ci  (tile y = ci/2)",
          f"seed preview -- {span_km:.0f} km across | style={args.style} | "
          f"relief x{args.relief:g} | {args.contours} m contours\n"
          "colour is ELEVATION, tinted by biome. Detail between 7.68 km samples is "
          "INTERPOLATED, not generated -- use --detail for real 30 m terrain.",
          args.out, contour_m=args.contours, legend=legend, box=box)

    print(f"  elevation {d['elev'].min():.0f} .. {d['elev'].max():.0f} m")
    for i, (n, _) in enumerate(BIOMES):
        if counts[i]:
            print(f"  {n:<18} {100 * counts[i] / total:5.1f}%")


if __name__ == "__main__":
    main()
