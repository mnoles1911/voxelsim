"""Ground truth for each vista screenshot: where it is, and what it is.

A screenshot filename that says "desert" is a claim, and until this ran it was
an UNVERIFIED one -- the sites were picked off a biome map by eye. This script
re-derives, at the exact spawn column of every shot, the same two labels the
game itself uses:

  * BIOME     via world_map.classify(), which parses
                voxel-core/include/voxelcore/biome.h at run time and therefore
                cannot drift from the client.
  * PROVINCE  via bake/province.py:province_fields -- the exact function
                bake/pipeline.py:2465 calls, on the same padded domain built by
                assemble_padded_coarse / assemble_padded_climate.

Never reimplement either one. A hand-rolled province discriminant was tried on
2026-08-02 and produced plausible, wrong shapes over the whole world.

It reports TWO scales per site, because they answer different questions:

  * AT THE SPAWN COLUMN -- what the player is standing on. This is what the
    filename should be named for.
  * OVER THE VISIBLE RADIUS -- what is actually in the frame. A vista from
    1200 m sees several km, so a shot named for its spawn pixel can be
    dominated on screen by a neighbouring biome. Where those two disagree the
    manifest says so, rather than letting the filename overclaim.

Usage:
    PY=D:/terrain-diffusion/.venv/Scripts/python.exe
    PYTHONPATH=D:/vox-int/terrain-service $PY vista_sites.py <seed_dir> <out.md>
"""
import pathlib
import sys

import numpy as np

sys.path.insert(0, "D:/vox-int/terrain-service/tools")
from world_map import _read_constants, classify, BIOMES        # noqa: E402
from terrain_service import tile_codec                          # noqa: E402
from terrain_service.bake import pipeline as bp                 # noqa: E402
from terrain_service.bake import province as _province          # noqa: E402

SEED_DIR = pathlib.Path(sys.argv[1])
OUT = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None

GEOM = bp.PRODUCTION
CONSTS = bp.CONSTANTS
APRON = GEOM.apron_coarse_px          # 32
TILE = GEOM.coarse_tile_px            # 512
PX_M = GEOM.coarse_pixel_m            # 30.0
TILE_M = TILE * PX_M                  # 15360 m

# The nine production shots, as `capture-all.ps1` fired them: name -> world XY
# in METRES (-VoxelSpawnAt is metres; VoxelEarthGameMode.h:7 converts to UU).
# Every one is a tile CENTRE, i.e. (t + 0.5) * 15360.
#
# THREE OF THESE ARE NOT TILE CENTRES, because three of the original nine were
# bad sites and only recomputing the labels here showed it:
#
#   BEACH       spawned at -142 m with ZERO land inside 6 km -- open water.
#   TAIGA       sat in the leftmost tile column,
#   RAINFOREST  sat in the top-right tile CORNER.
#
# The last two matter because a 1200 m camera at -20 deg sees far past its own
# tile, so an edge site fills its far field with the flat fallback plane --
# the exact failure voxel-capture.ps1's header warns about. All three were
# replaced from find_site.py, which scores the spawn column's own class AND
# what is in frame AND distance from the world edge.
SHOTS = [
    ("vista-desert-arid",            -38400.0, -161280.0),
    ("vista-grassland-arid",          -7680.0,   23040.0),
    ("vista-temperate_forest-fluvial", -115200.0, -176640.0),
    ("vista-savanna-fluvial",          38400.0, -115200.0),
    ("vista-rainforest-lowland",     -100470.0, -171510.0),
    ("vista-beach",                   -61200.0, -153360.0),
    ("vista-taiga",                  -102390.0,  -23670.0),
    ("vista-tundra_alpine-glacial",   -38400.0,  -38400.0),
    ("vista-rainforest-fluvial",      -23040.0,  -53760.0),
]

# What a 1200 m / -20 deg camera actually has in frame. Not a render-accurate
# frustum -- a stated, checkable radius, so "the label disagrees with the
# picture" can be diagnosed instead of argued.
VIEW_RADIUS_M = 6000.0

# Audition candidates from find_site.py without editing the production list:
#   VISTA_SITES="cand-a:-102390:-23670;cand-b:47370:-169590"
import os                                                            # noqa: E402
if os.environ.get("VISTA_SITES"):
    SHOTS = [(p.split(":")[0], float(p.split(":")[1]), float(p.split(":")[2]))
             for p in os.environ["VISTA_SITES"].split(";") if p]

K = _read_constants()
_cache = {}


def load(tx, ty):
    if (tx, ty) not in _cache:
        p = SEED_DIR / f"{tx}_{ty}.vxtl"
        if not p.exists():
            _cache[(tx, ty)] = None
        else:
            t = tile_codec.decode(p.read_bytes())
            _cache[(tx, ty)] = (t.elevation.astype(np.float32), t.climate)
    return _cache[(tx, ty)]


def elev_of(tx, ty):
    s = load(tx, ty)
    return None if s is None else s[0]


def clim_of(tx, ty):
    s = load(tx, ty)
    return None if s is None else s[1]


def world_to_tile_px(x_m, y_m):
    """World metres -> (tile x, tile y, pixel col, pixel row)."""
    tx, ty = int(np.floor(x_m / TILE_M)), int(np.floor(y_m / TILE_M))
    px = int(np.floor((x_m - tx * TILE_M) / PX_M))
    py = int(np.floor((y_m - ty * TILE_M) / PX_M))
    return tx, ty, px, py


def decode_climate(c):
    """uint8 planes -> physical units, the providers.diffusion encoding."""
    return (c[0] / 255.0 * 80.0 - 40.0,          # bio_1  deg C
            c[1] / 255.0 * 3000.0,               # bio_4  sd x100
            c[2] / 255.0 * 12000.0)              # bio_12 mm/yr


def stitched(tx, ty, cx, cy, rad_px):
    """Elevation + climate over +-rad_px about pixel (cx,cy) of a tile.

    Spans neighbouring tiles, because a 6 km radius is 200 px and a site need
    not sit at a tile centre -- the replacement BEACH site does not.
    """
    n = 2 * rad_px
    e = np.full((n, n), np.nan, np.float32)
    c = np.zeros((4, n, n), np.uint8)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            src = load(tx + dx, ty + dy)
            if src is None:
                continue
            se, sc = src
            # Destination index of this neighbour's pixel (0,0). A source pixel
            # (ys,xs) sits at (dy*TILE + ys - cy) relative to the site, and the
            # window's origin is -rad_px, so dest = dy*TILE - cy + rad_px + ys.
            # Getting this sign backwards shifted every window by 2*(rad-c) --
            # 11.5 km at the off-centre BEACH site, which then reported 0% land
            # for a shoreline that find_site.py had measured at 51.9%.
            oy, ox = dy * TILE - cy + rad_px, dx * TILE - cx + rad_px
            ys, ye = max(0, -oy), min(TILE, n - oy)
            xs, xe = max(0, -ox), min(TILE, n - ox)
            if ys >= ye or xs >= xe:
                continue
            e[oy + ys:oy + ye, ox + xs:ox + xe] = se[ys:ye, xs:xe]
            c[:, oy + ys:oy + ye, ox + xs:ox + xe] = sc[:, ys:ye, xs:xe]
    return e, c


def province_at(tx, ty, px, py):
    """Province weights at pixel (px,py) AND averaged over the whole tile."""
    pe, _ok = bp.assemble_padded_coarse(elev_of, tx, ty, GEOM)
    pc = bp.assemble_padded_climate(clim_of, tx, ty, GEOM)
    pf = _province.province_fields(pe, pc, coarse_pixel_m=PX_M,
                                   consts=CONSTS, max_half=APRON // 4)
    cs = slice(APRON, APRON + TILE)
    point = {k: float(v[APRON + py, APRON + px]) for k, v in pf.weights.items()}
    over = {k: float(v[cs, cs].mean()) for k, v in pf.weights.items()}
    return point, over


rows = []
for name, x_m, y_m in SHOTS:
    tx, ty, px, py = world_to_tile_px(x_m, y_m)
    src = load(tx, ty)
    if src is None:
        print(f"!! {name}: tile ({tx},{ty}) NOT ON DISK -- skipped")
        continue
    e, c = src
    elev_pt = float(e[py, px])
    t_pt, s_pt, p_pt = (float(v[py, px]) for v in decode_climate(c))

    # point biome, through the real classifier (1x1 arrays keep the same path)
    b_pt = int(classify(np.array([[elev_pt]], np.float32),
                        np.array([[t_pt]], np.float32),
                        np.array([[p_pt]], np.float32),
                        np.array([[s_pt]], np.float32), K)[0, 0])

    # what is in frame
    rad_px = int(round(VIEW_RADIUS_M / PX_M))
    ve, vc = stitched(tx, ty, px, py, rad_px)
    vt, vs, vp = decode_climate(vc)
    vb = classify(np.nan_to_num(ve), vt, vp, vs, K)
    land = np.isfinite(ve) & (ve > 0)
    n_land = max(int(land.sum()), 1)
    share = {}
    for i, (bname, _col) in enumerate(BIOMES):
        if i == 0:
            continue
        f = float((vb[land] == i).sum()) / n_land
        if f > 0.005:
            share[bname] = f
    share = dict(sorted(share.items(), key=lambda kv: -kv[1]))

    prov_pt, prov_tile = province_at(tx, ty, px, py)
    relief = float(np.nanmax(ve) - np.nanmin(np.where(land, ve, np.nan))) if land.any() else 0.0

    rows.append(dict(
        name=name, x_m=x_m, y_m=y_m, tx=tx, ty=ty, px=px, py=py,
        elev=elev_pt, temp=t_pt, seas=s_pt / 100.0, precip=p_pt,
        biome=BIOMES[b_pt][0], share=share,
        prov_pt=prov_pt, prov_tile=prov_tile,
        prov_top=max(prov_pt, key=prov_pt.get),
        relief=relief, land_frac=float(land.sum()) / ve.size,
    ))

# --------------------------------------------------------------------------
for r in rows:
    ptop = r["prov_top"].upper()
    print(f"\n{r['name']}")
    print(f"  tile ({r['tx']},{r['ty']}) px ({r['px']},{r['py']})   "
          f"world {r['x_m']:.0f}, {r['y_m']:.0f} m")
    print(f"  BIOME at spawn   {r['biome']}")
    print(f"  PROVINCE at spawn {ptop}  " +
          "  ".join(f"{k[:4]} {v:.2f}" for k, v in sorted(r["prov_pt"].items())))
    print(f"  elev {r['elev']:.0f} m   T {r['temp']:.1f} C   "
          f"P {r['precip']:.0f} mm/yr   bio4sd {r['seas']:.1f} C")
    print(f"  in frame (r={VIEW_RADIUS_M/1000:.0f} km): relief {r['relief']:.0f} m, land "
          f"{100*r['land_frac']:.0f}%,  " +
          "  ".join(f"{k} {100*v:.0f}%" for k, v in list(r["share"].items())[:4]))

if OUT is None:
    sys.exit(0)

PROV_SHORT = {"fluvial": "FLUVIAL", "glacial": "GLACIAL", "arid": "ARID", "lowland": "LOWLAND"}
lines = []
for r in rows:
    r["slug"] = f"{r['biome'].lower()}-{PROV_SHORT[r['prov_top']].lower()}-t{r['tx']}_{r['ty']}"
    lines.append(r)

with open(OUT, "w", encoding="utf-8") as fh:
    fh.write("| file | biome (spawn) | province (spawn) | tile | -VoxelSpawnAt (m) | UE world (cm) | "
             "elev | T | precip | in frame |\n")
    fh.write("|---|---|---|---|---|---|---|---|---|---|\n")
    for r in lines:
        inframe = ", ".join(f"{k} {100*v:.0f}%" for k, v in list(r["share"].items())[:3])
        fh.write(f"| `vista-{r['slug']}.png` | {r['biome']} | {PROV_SHORT[r['prov_top']]} "
                 f"{r['prov_pt'][r['prov_top']]:.2f} | ({r['tx']},{r['ty']}) | "
                 f"`{r['x_m']:.0f},{r['y_m']:.0f}` | {r['x_m']*100:.0f}, {r['y_m']*100:.0f} | "
                 f"{r['elev']:.0f} m | {r['temp']:.1f} C | {r['precip']:.0f} mm | {inframe} |\n")
print(f"\nwrote {OUT}")

# Machine-readable twin, so vista_map.py plots exactly the sites that were
# classified here rather than a second hand-copied list that can drift.
import json                                                          # noqa: E402
JOUT = OUT.with_suffix(".json")
with open(JOUT, "w", encoding="utf-8") as fh:
    json.dump(lines, fh, indent=1)
print(f"wrote {JOUT}")
for r in lines:
    print(f"  {r['name']:<26} -> vista-{r['slug']}")
