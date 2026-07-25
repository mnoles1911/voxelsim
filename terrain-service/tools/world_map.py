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


def hillshade(elev_m: np.ndarray, cell_m: float, azimuth=315.0, altitude=45.0) -> np.ndarray:
    """Standard Horn hillshade, returned in [0, 1]."""
    dy, dx = np.gradient(elev_m, cell_m)
    slope = np.arctan(np.hypot(dx, dy))
    aspect = np.arctan2(-dx, dy)
    az, alt = np.radians(360.0 - azimuth + 90.0), np.radians(altitude)
    shade = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
    return np.clip(shade, 0.0, 1.0)


def render(d: dict, k: dict) -> tuple[np.ndarray, np.ndarray]:
    elev, temp, precip, tstd = d["elev"], d["temp"], d["precip"], d["tstd"]
    biome = classify(elev, temp, precip, tstd, k)

    rgb = np.zeros(elev.shape + (3,), dtype=np.float64)
    for i, (_, colour) in enumerate(BIOMES):
        rgb[biome == i] = colour

    # Sea: a depth ramp rather than one flat blue, so shelf reads differently
    # from abyss -- that is most of what makes a coastline legible.
    sea = elev < k["beach_lower_m"]
    if sea.any():
        depth = np.clip(-elev[sea] / 6000.0, 0.0, 1.0)
        rgb[sea] = np.stack([0.16 - 0.10 * depth, 0.34 - 0.20 * depth, 0.58 - 0.26 * depth], -1)

    # Hillshade the land only. Shading the sea floor would imply detail the
    # coarse sketch does not have and would fight the depth ramp.
    shade = hillshade(elev, FINE_PER_COARSE * 30.0)
    land = ~sea
    rgb[land] *= (0.55 + 0.65 * shade[land])[:, None]
    return np.clip(rgb, 0, 1), biome


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


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="http://127.0.0.1:8899")
    ap.add_argument("--window", default="-50,50,-50,50", help="ci0,ci1,cj0,cj1")
    ap.add_argument("--out", default="world_map.png")
    ap.add_argument("--tile-dir", default=None,
                    help="outline the generated tiles' footprint on the map")
    ap.add_argument("--verify-axes", action="store_true")
    args = ap.parse_args()

    if args.verify_axes:
        if not args.tile_dir:
            sys.exit("--verify-axes needs --tile-dir")
        check_axis_mapping(args.url, Path(args.tile_dir))
        return

    ci0, ci1, cj0, cj1 = (int(v) for v in args.window.split(","))
    k = _read_constants()
    print("thresholds read from biome.h:")
    print(f"  temperature bands  cold <{k['temp_cold_c']:g}C  warm >={k['temp_warm_c']:g}C  "
          f"hot >={k['temp_hot_c']:g}C")
    print(f"  precipitation      arid <{k['precip_arid_mm']:g}  semi <{k['precip_semi_mm']:g}  "
          f"moderate <{k['precip_mod_mm']:g} mm/yr")
    print(f"  treeline           {k['treeline_base_m']:g} m at 0C, "
          f"{k['treeline_m_per_c']:g} m/C")

    span_km = (ci1 - ci0) * FINE_PER_COARSE * 30 / 1000
    print(f"\nfetching coarse window ci=[{ci0},{ci1}) cj=[{cj0},{cj1}) "
          f"= {span_km:.0f} km across (first call is slow; the model caches) ...")
    d = fetch(args.url, ci0, ci1, cj0, cj1)
    rgb, biome = render(d, k)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib is required to write the image")

    fig, ax = plt.subplots(figsize=(11, 11), dpi=110)
    ax.imshow(rgb, origin="upper", interpolation="nearest",
              extent=[cj0, cj1, ci1, ci0])
    ax.set_xlabel("coarse cj  (tile x = cj/2)")
    ax.set_ylabel("coarse ci  (tile y = ci/2)")

    if args.tile_dir:
        sys.path.insert(0, str(_REPO / "terrain-service"))
        from terrain_service.tile_codec import decode
        tp = sorted(Path(args.tile_dir).glob("*.vxtl"))
        if tp:
            xs, ys = [], []
            for p in tp:
                t = decode(p.read_bytes())
                xs.append(t.x)
                ys.append(t.y)
            x0, x1 = min(xs) * COARSE_PER_TILE, (max(xs) + 1) * COARSE_PER_TILE
            y0, y1 = min(ys) * COARSE_PER_TILE, (max(ys) + 1) * COARSE_PER_TILE
            ax.add_patch(plt.Rectangle((x0, y0), x1 - x0, y1 - y0, fill=False,
                                       edgecolor="red", linewidth=2))
            ax.text(x0, y0 - 1, f"{len(tp)} generated tiles", color="red", fontsize=9)

    counts = np.bincount(biome.ravel(), minlength=len(BIOMES))
    total = biome.size
    handles = [plt.Rectangle((0, 0), 1, 1, color=c) for _, c in BIOMES]
    labels = [f"{n}  {100 * counts[i] / total:.1f}%" for i, (n, _) in enumerate(BIOMES)]
    ax.legend(handles, labels, loc="upper left", bbox_to_anchor=(1.01, 1.0),
              fontsize=8, frameon=False)
    ax.set_title(f"coarse world preview, {span_km:.0f} km across\n"
                 "climate zones only -- no slope gate, see --help", fontsize=10)
    fig.tight_layout()
    fig.savefig(args.out, bbox_inches="tight")
    print(f"\nwrote {args.out}")
    for i, (n, _) in enumerate(BIOMES):
        if counts[i]:
            print(f"  {n:<18} {100 * counts[i] / total:5.1f}%")


if __name__ == "__main__":
    main()
