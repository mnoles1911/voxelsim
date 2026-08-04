"""Where streams, rivers and lakes will actually be placed -- over the world.

THE QUESTION THIS ANSWERS, verbatim from the owner: "a world map overlay that
shows where streams, rivers, and lakes will actually be placed". So it draws
the two halves of the watershed system on one hillshade, and it is careful to
say which half is MEASURED and which is PREDICTED, because they are not the
same kind of statement:

  LAKES are MEASURED. They come from the basin registry the bake ships
  (SECTION_BASIN_TABLE, work item 3) as recorded by ``tools/lake_survey.py
  dump`` -- the real bake, the real ``basins.survey_basins`` classifier, the
  real water balance. A tile with no dump has NO lakes drawn and is hatched
  out. Nothing is interpolated across an unsurveyed tile, ever: a lake is a
  hole in a specific hillside, and there is no such thing as an estimate of
  one.

  CHANNELS come from THE COARSE TILES -- all of them -- swept here at the
  30 m pitch, which is the pitch of the flow superblocks the bake routes
  against. Every kernel and every constant is the shipped one:
  ``flow.fill_depressions``, ``flow.d8_receivers``, ``flow.accumulate_d8``,
  ``water.runoff_field_mm_yr``, ``water.discharge_source``,
  ``water.q_drawable_m3_yr``, ``water.water_head_mask``. Nothing about the
  water rule is defined in this file any more.

  It is deliberately NOT derived from fine tiles, and that is the whole reason
  the channel layer can be world-scale at all: 256 of 289 tiles have ever been
  surveyed and far fewer are resident, so a fine-derived network would cover a
  fraction of the map and would have to be hatched like the lakes are.

WHY THE TWO HALVES HAVE DIFFERENT COVERAGE, which is the honest part. Lakes
need a fine bake: ~9 minutes and ~5 GiB per 15.36 km tile. Channels need only
the coarse tiles, all 289 of which exist. So the channel network covers the
whole world and the lake layer covers exactly the tiles that were baked. The
legend prints both counts and the map hatches the difference.

WHAT bake_ver 11 CHANGED HERE (2026-08-04), since this map is only worth
anything if it agrees with the bake:

  * **The cut.** Water is drawn at ``q_drawable``, the threshold the shipped
    plane is written over, not at ``Q_perennial``. See ``q_drawable()`` below
    for why the pitch passed to it is the FINE tier's and not this map's --
    that is the one place this file exercises judgement.
  * **D8.** The discharge sweep is single-receiver, matching
    ``water_flow_single_receiver``. The terrain's MFD routing is untouched;
    this map does not compute an area field at all any more.
  * **One sweep, with ``source=``.** The old two-sweeps-and-subtract form
    predates ``flow.accumulate_*(source=)``, which exists because of this tool.
  * **Carried discharge changes NOTHING here**, and that is worth saying rather
    than passing over: this map has one domain and one sweep, so every
    catchment was always whole. The pyramid's carried-Q fix exists to bring the
    fine tier up to what this sweep already computed.

BAKE VERSION. ``roughness_seed`` takes ``BAKE_VERSION`` (pipeline.py:1130), so
tiles baked at 7 and 8 are DIFFERENT WORLDS, not the same world measured
twice. This tool refuses to mix them: every dump must carry the same
``bake_version``, and that number is printed on the image.

DISCHARGE, one currency (plan 4.1.1). Q(cell) = sum over the upstream network
of runoff(c) * cell_area, m^3/yr. ONE sweep, seeded per cell by
``water.discharge_source`` through ``accumulate_*(source=)`` -- the kernel
parameter that exists because this tool used to have to fake it with two
sweeps and a subtraction.

TWO THRESHOLDS, AND THE MAP SHOWS BOTH. ``Q_PERENNIAL`` is the hydrologic
statement -- 10 litres a second of mean discharge, the scale at which a
watercourse is wet in every month. ``q_drawable`` is the raster statement: the
smallest Q whose channel is wide enough for the shipped fine plane to hold a
wet bed. Water is DRAWN at ``q_drawable``, because that is where the game puts
it; the band between the two is on the map in grey, because those reaches are
genuinely wet and the raster simply declines to draw them, and hiding that
would let "the map is sparse" read as "the world is dry".

Both beat the carve thresholds, which is why neither is one: the bake carves a
swale at 156 m^2 of catchment and ``rivernet.h`` calls a channel a river at
around 0.03 km^2, both dry gullies in most climates. The resulting drainage
density is printed on the image so the number can be argued with against
Earth's 0.5-2 km/km^2 for perennial networks.

WIDTH is ``channel.h``'s own law, imported from ``bake.water`` -- 1.5 m at
Q_PERENNIAL, w proportional to Q^0.3984, capped at 400 m. The LINE WIDTHS ON
THE MAP ARE SYMBOLIC: a 9 m river is a fourteenth of a 120 m pixel. The
colour, not the thickness, carries the discharge.

USAGE (matplotlib and scipy are not in the system python -- see this
directory's README):

    TILES=D:/voxelsim/tile-cache/<provider>/<seed:016x>/s1
    PY=D:/terrain-diffusion/.venv/Scripts/python.exe
    export PYTHONPATH=D:/voxelsim/terrain-service

    $PY tools/worldmaps/water.py "$TILES" out/05-water-placement.png \
        --dumps out/watermap-dumps [--cache out/water-flow.npz]

``--cache`` stores the (expensive) discharge field so re-rendering is seconds
rather than minutes. It is keyed to the tile set and the constants and refuses
to load against a different one.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import lake_survey                                         # noqa: E402
from terrain_service import tile_codec                     # noqa: E402
from terrain_service.bake import basins as bs              # noqa: E402
from terrain_service.bake import flow                      # noqa: E402
from terrain_service.bake import pipeline as bp            # noqa: E402
from terrain_service.bake import province as bprov         # noqa: E402
from terrain_service.bake import water as bw               # noqa: E402

COARSE_PX_M = 30.0
TILE_PX = 512

# --------------------------------------------------------------------------- the rule
#
# NOTHING IN THIS SECTION IS DEFINED HERE ANY MORE. Every constant and every law
# below used to be a local copy, written when this tool was the only thing in
# the repo that had a discharge at all. `bake/water.py` now owns them and the
# bake itself runs on them, so a copy here is a second law free to drift away
# from the one that ships -- exactly the failure `02-biomes.png` still carries a
# visible scar from. One law, one home.

Q_PERENNIAL_M3_YR = bw.Q_PERENNIAL_M3_YR                  # 315,576 m^3/yr, 10 L/s
Q_RIVER_M3_YR = bw.Q_RIVER_M3_YR                          # 1 m^3/s
Q_MAJOR_M3_YR = bw.Q_MAJOR_M3_YR                          # 10 m^3/s
channel_width_m = bw.channel_width_m


def q_drawable(consts, geom) -> float:
    """The cut the SHIPPED bake actually writes water at, m^3/yr.

    THIS IS THE ONE JUDGEMENT IN THIS FILE AND IT IS WORTH THE PARAGRAPH.

    `bw.q_drawable_m3_yr(cell_m, min_width_px)` answers "what is the smallest Q
    whose channel fills `min_width_px` of a `cell_m` raster". It is therefore a
    statement about A PARTICULAR PITCH, and the temptation -- the wrong thing --
    is to evaluate it at this map's own pitch because that is the raster being
    drawn. Do that and the 1.5 px rule at the coarse 30 m pitch asks for a 45 m
    wide channel, i.e. 1.6e9 m^3/yr, 51 m^3/s: a cut a thousand times above the
    shipped one that would erase all but a handful of trunk rivers and describe
    a world that does not exist.

    The map is not trying to say what ITS raster can hold. It is trying to say
    where the GAME will put water, and the game's water plane is written on the
    fine tier at 1.875 m. So the pitch passed here is the fine tier's, and the
    number that comes out is the number the bake uses -- 1.5286e6 m^3/yr at
    `water_min_width_px` 1.5, 0.048 m^3/s. Evaluated, never hard-coded, so a
    later change to either constant moves this map with it.
    """
    return bw.q_drawable_m3_yr(geom.fine_pixel_m, consts.water_min_width_px,
                               consts.water_q_perennial_m3_yr)


#: Colours. The first entry is NOT DRAWN WATER: it is the band that is
#: perennially wet (>= 10 L/s) and still below the bake's drawable cut, and it
#: is on the map in a deliberately washed-out grey-blue because leaving it off
#: would let "the raster declines to draw it" read as "the world is dry". Every
#: saturated blue on this map is water the bake writes.
UNDRAWN_COLOUR = "#a3b2bd"


def classes(q_draw):
    """Drawn-channel classes, floored at the SHIPPED cut rather than at perennial.

    Before bake_ver 11 this map's floor was `Q_PERENNIAL_M3_YR` and the caption
    called the whole layer a prediction, because no baked tile carried a
    runoff-weighted Q to compare against. Both halves of that have changed: the
    bake computes this quantity now, and it draws at `q_drawable`. A map whose
    floor is 4.8x below the shipped cut is drawing streams the game does not
    place.
    """
    return (
        ("stream", q_draw, Q_RIVER_M3_YR, "#5fa8d3"),
        ("river", Q_RIVER_M3_YR, Q_MAJOR_M3_YR, "#1f6fb2"),
        ("major river", Q_MAJOR_M3_YR, np.inf, "#0b3d66"),
    )

KIND_COLOUR = {
    "dry_playa": "#a87c46",
    "salt_flat": "#e8d47a",
    "seasonal": "#57b7a8",
    "lake_terminal": "#1b7f6e",
    "lake_overflowing": "#2f7fd0",
}
KIND_ORDER = ("lake_overflowing", "lake_terminal", "seasonal", "salt_flat", "dry_playa")


# --------------------------------------------------------------------------- world

def stitch(seed_dir: pathlib.Path):
    """Every coarse tile in one array, at the native 30 m pitch.

    Returns (elev_m, climate_u8, present_mask, tx0, ty0, ntx, nty). Absent
    tiles are NaN in ``elev_m`` and False in ``present_mask`` -- they are not
    filled with sea level, because a fill would route rivers across them.
    """
    files = sorted(seed_dir.glob("*.vxtl"))
    if not files:
        raise SystemExit(f"error: no .vxtl tiles in {seed_dir}")
    txs = sorted({int(f.stem.split("_")[0]) for f in files})
    tys = sorted({int(f.stem.split("_")[1]) for f in files})
    tx0, ty0 = txs[0], tys[0]
    ntx, nty = txs[-1] - tx0 + 1, tys[-1] - ty0 + 1
    h, w = nty * TILE_PX, ntx * TILE_PX
    elev = np.full((h, w), np.nan, np.float32)
    clim = np.zeros((len(bprov.CLIMATE_ORDER), h, w), np.uint8)
    present = np.zeros((nty, ntx), bool)
    for f in files:
        tx, ty = (int(v) for v in f.stem.split("_"))
        t = tile_codec.decode(f.read_bytes())
        j, i = ty - ty0, tx - tx0
        sl = (slice(j * TILE_PX, (j + 1) * TILE_PX),
              slice(i * TILE_PX, (i + 1) * TILE_PX))
        elev[sl] = t.elevation.astype(np.float32)
        clim[(slice(None),) + sl] = np.asarray(t.climate, np.uint8)
        present[j, i] = True
    return elev, clim, present, tx0, ty0, ntx, nty


def discharge(elev, clim, consts):
    """Q over the whole stitched world, by the bake's own B6 construction.

    THIS IS NOW A COPY OF `pipeline.bake_tile`'s WATER PASS, not a paraphrase of
    it. Three things changed with bake_ver 11 and all three are here:

    ``source=`` INSTEAD OF TWO SWEEPS AND A SUBTRACTION. This function used to
    run ``accumulate_mfd`` twice over one filled surface and subtract, because
    when it was written the kernel had no way to replace the per-cell seed. It
    does now -- ``flow.accumulate_*(source=)`` exists precisely because of this
    tool, and flow.py names it. One sweep, half the cost, and the seed is built
    by ``bw.discharge_source``, the shipped function, at ``scale=1`` because
    this map's climate grid and its flow grid are the same grid.

    D8, NOT MFD, FOR THE DISCHARGE. ``consts.water_flow_single_receiver``. The
    terrain still routes MFD everywhere it matters -- ``mfd_p`` decides the area
    field, hence ``A^m``, hence every height -- and nothing here touches that,
    because nothing here computes an area field at all any more. This sweep's
    only consumer is a hard threshold, and splitting a reach's discharge across
    every lower neighbour is what put 2,014 pieces and a 1,113 m longest reach
    in the measured corridor. The forest is built on ``filled``, the same
    surface the sweep runs over, exactly as B6 builds it.

    CARRIED DISCHARGE IS NOT A CHANGE HERE, and this is the reason to say so
    out loud rather than quietly not mention it: the pyramid's carried-Q fix
    exists to make the FINE tier agree with what a single sweep over the
    stitched coarse world already computes. There is no pyramid on this map and
    no injection cell -- one domain, one sweep, every catchment whole. The
    coarse world is the reference that measured the shortfall (1.27e6 against
    58.7e6 at the corridor's coast), so this layer already had the answer the
    fine tier has just moved from 2.2% to ~40% of.

    Returns (filled, receivers, q, runoff_mm).
    """
    # Absent tiles are sea, for routing purposes only: fill_depressions needs a
    # finite domain, and MISSING_ELEVATION_M is the bake's own answer for a
    # tile that is not there (pipeline.MISSING_ELEVATION_M, mirrored by
    # tilestore.cpp's "a tile that is not loaded is open ocean").
    z = np.where(np.isnan(elev), np.float32(bp.MISSING_ELEVATION_M), elev)
    z = np.ascontiguousarray(z, np.float32)

    # Budyko runoff, smoothed the way every climate consumer smooths (the uint8
    # wire LSBs are 0.31 C and 47 mm/yr, bigger than several province
    # boundaries). `bw.runoff_field_mm_yr` is the bake's own routine for this
    # and takes the same WaterBalance the lake rule uses, so a basin and its
    # outlet river cannot disagree about how much water the sky delivers.
    runoff_mm = bw.runoff_field_mm_yr(
        clim, bp.basin_balance(consts), consts.province_smooth_m, COARSE_PX_M)

    t0 = time.time()
    filled = np.asarray(flow.fill_depressions(z), np.float32)
    print(f"  fill_depressions {time.time() - t0:.0f}s", flush=True)
    del z

    # No pyramid here, so no inflow term at all: the domain edge IS the coast.
    src = bw.discharge_source(runoff_mm, filled.shape, 1, COARSE_PX_M)

    t0 = time.time()
    rec, _ = flow.d8_receivers(filled, COARSE_PX_M)
    print(f"  d8_receivers {time.time() - t0:.0f}s", flush=True)
    t0 = time.time()
    if consts.water_flow_single_receiver:
        q = flow.accumulate_d8(filled, COARSE_PX_M, source=src, receivers=rec)
        print(f"  accumulate_d8 (discharge) {time.time() - t0:.0f}s", flush=True)
    else:
        q = flow.accumulate_mfd(filled, COARSE_PX_M, p=consts.mfd_p, source=src)
        print(f"  accumulate_mfd (discharge) {time.time() - t0:.0f}s", flush=True)
    del src
    np.clip(q, 0.0, None, out=q)
    return filled, rec, q.astype(np.float32), runoff_mm


def channel_length_km(q, shape, rec, land, thresholds):
    """Length of the channel network, by class, following the D8 tree.

    Cell COUNT x cell size overstates a diagonal reach by up to 41%. This
    walks ``d8_receivers`` -- the bake's own kernel -- and sums the true step
    distance for every channel cell whose receiver is also a channel cell, so
    a diagonal step costs sqrt(2) * 30 m and nothing is double counted.

    ``rec`` is now the SAME forest the discharge was accumulated down, not a
    second one built here. It always was the same surface; passing it makes the
    length a measurement of the network that was actually drawn rather than of
    a coincidentally identical one.
    """
    rec = np.asarray(rec).ravel()
    h, w = shape
    has = rec >= 0
    ry, rx = np.divmod(np.where(has, rec, 0).astype(np.int64), w)
    cy, cx = np.divmod(np.arange(h * w, dtype=np.int64), w)
    # Each channel cell contributes ONE step, the one to its own receiver, so
    # nothing is counted twice and a diagonal costs sqrt(2) cells. A cell with
    # no receiver (the mouth, at the domain edge after an epsilon fill)
    # contributes nothing, which is right: the reach ends there.
    step = np.where((ry != cy) & (rx != cx), np.sqrt(2.0), 1.0) * COARSE_PX_M
    step[~has] = 0.0
    del ry, rx, cy, cx
    out = {}
    qf, lf = q.ravel(), land.ravel()
    for name, lo, hi, _c in thresholds:
        m = lf & (qf >= lo) & (qf < hi)
        out[name] = float(step[m].sum()) / 1000.0
    return out


def check_against_bake(q, land, dumps, tx0, ty0, q_draw):
    """Do the PREDICTED channels sit where the BAKE actually routes water?

    A predicted layer with no cross-check is an assertion, and this map draws
    one over the whole world. So: the bake's own MFD accumulation, dumped per
    tile from the real fine bake at 1.875 m/px, is an independent answer to
    "where does water go" computed at 16x this map's resolution on the shipped
    surface. The two use different currencies (m^2 of catchment there, m^3/yr
    of delivered runoff here) so they cannot be compared value for value -- but
    they can be compared as PLACES.

    The statistic is a ratio: median baked accumulation UNDER the predicted
    channel, over median baked accumulation across the tile's land. A network
    drawn on the wrong lines gives ~1. A network on the bake's own thalwegs
    gives orders of magnitude.

    Reduction is by MAX, not mean: a channel is one or two cells wide at 15 m
    and a mean would average it away against its own valley walls -- the same
    mistake the plateau-area lesson records for run lengths.

    Returns a list of (tile, ratio, n_channel_cells); tiles without a resident
    .npz are skipped, which is normal and not an error.
    """
    out = []
    for (tx, ty), d in sorted(dumps.items()):
        if not d["npz"].exists():
            continue
        with np.load(d["npz"]) as z:
            acc = np.asarray(z["acc_m2"], np.float64)
        f = acc.shape[0] // TILE_PX
        if f < 1 or acc.shape[0] % TILE_PX:
            continue
        accd = acc[: f * TILE_PX, : f * TILE_PX].reshape(
            TILE_PX, f, TILE_PX, f).max(axis=(1, 3))
        j, i = ty - ty0, tx - tx0
        sl = (slice(j * TILE_PX, (j + 1) * TILE_PX),
              slice(i * TILE_PX, (i + 1) * TILE_PX))
        lm = land[sl]
        ch = lm & (q[sl] >= q_draw)
        if ch.sum() < 50 or lm.sum() < 1000:
            continue
        base = float(np.median(accd[lm]))
        under = float(np.median(accd[ch]))
        out.append(((tx, ty), under / max(base, 1.0), int(ch.sum()),
                    float(d["meta"]["tile_stats"]["relief_m"])))
    return out


# --------------------------------------------------------------------------- lakes

def water_area_m2(b: dict) -> float:
    """How much water this basin's surface actually covers, m^2.

    NOT ``area_m2``, AND THIS IS A TRAP WORTH THE PARAGRAPH. The registry's
    ``area_m2`` is the depression's area at its SPILL level -- the size of the
    hole -- because that is what the registry is for. The water sits at
    ``surface_m``, and for an endorheic basin those are wildly different
    things: the largest lake in this world is 2,377 ha of BOWL holding 33 ha of
    WATER, because its balance settles 549 m below its own outlet.

    Using area_m2 as a lake area therefore overstates by 73x on that one basin
    and by some amount on every basin that is not overflowing -- which is 8,374
    of 21,942 of them. It read as plausible, which is what made it dangerous:
    the number is real, it is just the answer to a different question.

    The right answer is on the wire already: A(h) is the hypsometric curve the
    balance was solved on, so the water area is that curve read at the surface.
    For an overflowing lake surface == spill and this returns area_m2 exactly,
    so there is one rule and not two.
    """
    lv = b.get("hyps_levels_m") or []
    ar = b.get("hyps_areas_m2") or []
    if len(lv) < 2:
        return float(b["area_m2"]) if b["surface_m"] >= b["spill_m"] else 0.0
    return float(np.interp(float(b["surface_m"]), np.asarray(lv, np.float64),
                           np.asarray(ar, np.float64)))


def load_dumps(dump_dir: pathlib.Path, consts):
    """Registered basins per tile, from lake_survey dumps, one bake version.

    The dump records every component above a permissive 1 m floor. Everything
    that turns those into a registry -- the filter, the water balance, the kind
    -- is ``lake_survey._refilter``, the same function the survey report calls,
    handed the SHIPPED constants (``pipeline.basin_filter`` /
    ``basin_balance``, the ones hashed into the bake identity) rather than the
    report's sweepable defaults. A second copy of that arithmetic here is
    exactly how a map starts disagreeing with the thing it maps.
    """
    recs = {}
    vers, fps = set(), set()
    filt = bp.basin_filter(consts)
    wb = bp.basin_balance(consts)
    for r in lake_survey._load(dump_dir):
        vers.add(int(r["bake_version"]))
        fps.add(r.get("bake_fingerprint", ""))
        rows = lake_survey._refilter(r, filt, wb)
        for b in rows:
            b["water_area_m2"] = water_area_m2(b)
        recs[(r["tile"][0], r["tile"][1])] = {
            "meta": r,
            "basins": rows,
            "npz": pathlib.Path(r["_path"]).with_suffix(".npz"),
        }
    if not recs:
        raise SystemExit(f"error: no lake-survey dumps in {dump_dir}")
    if len(vers) > 1:
        raise SystemExit(
            f"error: dumps mix bake versions {sorted(vers)}. roughness_seed "
            "takes BAKE_VERSION, so those are different worlds and drawing "
            "them on one map would be a fabrication. Re-dump at one version.")
    if len(fps) > 1:
        raise SystemExit(f"error: dumps mix bake fingerprints {sorted(fps)}")
    return recs, vers.pop()


# --------------------------------------------------------------------------- render

def _maxpool(a, f):
    h, w = a.shape
    return a[: h // f * f, : w // f * f].reshape(h // f, f, w // f, f).max(axis=(1, 3))


def _anypool(a, f):
    h, w = a.shape
    return a[: h // f * f, : w // f * f].reshape(h // f, f, w // f, f).any(axis=(1, 3))


def _dilate(m, r):
    if r <= 0:
        return m
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dy * dy + dx * dx > r * r:
                continue
            out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("seed_dir", type=pathlib.Path,
                    help="coarse tile directory (.../<seed>/s1)")
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--dumps", type=pathlib.Path, required=True,
                    help="lake_survey dump directory (the MEASURED half)")
    ap.add_argument("--ds", type=int, default=4,
                    help="output downsample from 30 m/px (4 = 120 m/px)")
    ap.add_argument("--cache", type=pathlib.Path, default=None,
                    help="npz to store/reuse the discharge field")
    ap.add_argument("--seed-label", type=int, default=20260719)
    args = ap.parse_args()

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.colors import LightSource
    from matplotlib.lines import Line2D
    from matplotlib.patches import Patch, Rectangle

    consts = bp.CONSTANTS
    print(f"stitching {args.seed_dir} ...", flush=True)
    elev, clim, present, tx0, ty0, ntx, nty = stitch(args.seed_dir)
    h, w = elev.shape
    n_tiles = int(present.sum())
    print(f"  {n_tiles} coarse tiles -> {w}x{h} px @ {COARSE_PX_M:.0f} m/px "
          f"= {w * COARSE_PX_M / 1000:.0f} km", flush=True)

    Q_DRAW = q_drawable(consts, bp.PRODUCTION)
    CLASSES = classes(Q_DRAW)
    print(f"  drawable cut {Q_DRAW:,.0f} m3/yr = {Q_DRAW / 3.156e7:.4f} m3/s "
          f"({consts.water_min_width_px} px at {bp.PRODUCTION.fine_pixel_m} m, "
          f"routing {'D8' if consts.water_flow_single_receiver else 'MFD'})",
          flush=True)

    # The cache key carries the drawable cut and the routing rule, not just the
    # perennial constant: a cache written at bake_ver 10 holds an MFD field and
    # loading it under a D8 caption would be the map lying about its own
    # construction. Different key -> recomputed.
    key = hashlib.sha256(
        f"{tx0},{ty0},{ntx},{nty},{n_tiles},{bp.bake_fingerprint(bp.PRODUCTION, consts)},"
        f"{Q_PERENNIAL_M3_YR},{Q_DRAW},"
        f"{bool(consts.water_flow_single_receiver)},v2".encode()).hexdigest()[:16]
    cached = None
    if args.cache and args.cache.exists():
        z = np.load(args.cache)
        if str(z["key"]) == key:
            cached = z
            print(f"  reusing discharge field from {args.cache}", flush=True)
    if cached is None:
        print("computing discharge (fill + one single-receiver sweep) ...", flush=True)
        filled, rec, q, runoff_mm = discharge(elev, clim, consts)
        if args.cache:
            np.savez_compressed(args.cache, key=key, q=q, rec=rec,
                                filled=filled, runoff_mm=runoff_mm)
    else:
        filled, rec, q = cached["filled"], cached["rec"], cached["q"]
        runoff_mm = cached["runoff_mm"]
    del clim

    land = np.isfinite(elev) & (elev > bs.SEA_LEVEL_M)
    land_km2 = float(land.sum()) * COARSE_PX_M ** 2 / 1e6

    # The two counts, from the bake's own function on the bake's own cut. This
    # is what stops "the map is sparse" reading as "the world is dry": the
    # perennial network is the hydrologic statement and the drawn one is the
    # raster statement, and the gap between them is a real, reported number
    # rather than something absorbed silently by the threshold.
    _wet, _heads, head_stats = bw.water_head_mask(
        np.where(land, q, 0.0), rec, q_drawable=Q_DRAW,
        q_perennial=consts.water_q_perennial_m3_yr)
    del _wet, _heads
    n_drawn = int(head_stats["drawn_cells"])
    n_perennial = int(head_stats["perennial_cells"])
    n_undrawn = n_perennial - n_drawn
    print(f"  perennial {n_perennial:,} cells / drawn {n_drawn:,} / "
          f"heads {int(head_stats['head_cells']):,}", flush=True)

    print("channel network ...", flush=True)
    lens = channel_length_km(q, filled.shape, rec, land, CLASSES)
    total_km = sum(lens.values())
    del filled

    # Per-tile spread, which is the deliverable and not a total. Over tiles
    # that are AT LEAST HALF LAND: this world is 44% land, and averaging a
    # drainage density over open ocean would report a number about the sea.
    per_tile_km, per_tile_dens, land_tiles = [], [], 0
    tile_km2 = (TILE_PX * COARSE_PX_M / 1000.0) ** 2
    tile_has_land = np.zeros((nty, ntx), bool)
    for j in range(nty):
        for i in range(ntx):
            if not present[j, i]:
                continue
            sl = (slice(j * TILE_PX, (j + 1) * TILE_PX),
                  slice(i * TILE_PX, (i + 1) * TILE_PX))
            a = float(land[sl].sum()) * COARSE_PX_M ** 2 / 1e6
            tile_has_land[j, i] = a > 0.01 * tile_km2
            if a < 0.5 * tile_km2:
                continue
            land_tiles += 1
            km = float((land[sl] & (q[sl] >= Q_DRAW)).sum()) * COARSE_PX_M / 1000.0
            per_tile_km.append(km)
            per_tile_dens.append(km / a)
    per_tile_km = np.array(per_tile_km)
    per_tile_dens = np.array(per_tile_dens)

    print(f"loading dumps from {args.dumps} ...", flush=True)
    dumps, bake_ver = load_dumps(args.dumps, consts)
    print(f"  {len(dumps)} surveyed tiles at bake_ver {bake_ver}", flush=True)

    agree = check_against_bake(q, land, dumps, tx0, ty0, Q_DRAW)
    if agree:
        ratios = np.array([r for _t, r, _n, _v in agree])
        print(f"  cross-check on {len(agree)} tiles with a baked accumulation: "
              f"ratio min {ratios.min():.0f} / med {np.median(ratios):.0f} / "
              f"max {ratios.max():.0f}", flush=True)

    # ---- the picture -----------------------------------------------------
    f = args.ds
    px_m = COARSE_PX_M * f
    shade_src = np.where(np.isnan(elev), np.float32(bp.MISSING_ELEVATION_M), elev)
    ds_elev = shade_src[::f, ::f]
    ls = LightSource(azdeg=292.5, altdeg=45)
    shade = ls.hillshade(ds_elev, vert_exag=3.0, dx=px_m, dy=px_m)
    H, W = ds_elev.shape

    rgb = np.empty((H, W, 3), np.float32)
    ds_land = _anypool(land, f)[:H, :W]
    rgb[...] = np.array([0.72, 0.71, 0.67])[None, None, :]        # land
    rgb[~ds_land] = np.array([0.16, 0.24, 0.36])                  # sea
    rgb *= (0.42 + 0.58 * shade)[..., None]
    ds_absent = ~_anypool(np.isfinite(elev), f)[:H, :W]
    rgb[ds_absent] = 1.0

    # LAKES AS PIXELS, NOT MARKERS, and the arithmetic is the argument: a
    # 120 m/px output cell is 1.4 ha, and this world's MEDIAN registered water
    # body is 0.71 ha -- 95 m across. So one pixel per lake is very nearly to
    # scale, and anything bigger is drawn at its own radius.
    #
    # The first draft used scatter markers with a 3.2 pt floor. That is 1 km on
    # the ground: a 13x overstatement of the median pond, and with 21,942 of
    # them the land vanished under dots and took the river network with it. A
    # legibility floor that large stops being a rendering choice and becomes a
    # claim about the world, so it is gone. What survives is the true footprint
    # with a HALF-PIXEL minimum -- dense country reads as dense country because
    # the ponds really are that dense, not because the marker is fat.
    kind_idx = np.full((H, W), -1, np.int8)
    kind_area = np.zeros((H, W), np.float32)
    scale = bp.PRODUCTION.fine_pixel_m / COARSE_PX_M / f
    tpx = TILE_PX // f
    yy, xx = np.mgrid[-8:9, -8:9]
    for (tx, ty), d in dumps.items():
        for b in d["basins"]:
            cx = int(round((tx - tx0) * tpx + b["seed_px"][0] * scale))
            cy = int(round((ty - ty0) * tpx + b["seed_px"][1] * scale))
            if not (0 <= cx < W and 0 <= cy < H):
                continue
            r = max(np.sqrt(b["water_area_m2"] / np.pi) / px_m, 0.5)
            ki = KIND_ORDER.index(b["kind_name"])
            if r <= 0.5:
                if b["water_area_m2"] > kind_area[cy, cx]:
                    kind_idx[cy, cx], kind_area[cy, cx] = ki, b["water_area_m2"]
                continue
            ri = int(min(np.ceil(r), 8))
            sub = (yy[8 - ri:9 + ri, 8 - ri:9 + ri] ** 2 +
                   xx[8 - ri:9 + ri, 8 - ri:9 + ri] ** 2) <= r * r
            y0, y1 = max(cy - ri, 0), min(cy + ri + 1, H)
            x0, x1 = max(cx - ri, 0), min(cx + ri + 1, W)
            sm = sub[y0 - (cy - ri):sub.shape[0] - ((cy + ri + 1) - y1),
                     x0 - (cx - ri):sub.shape[1] - ((cx + ri + 1) - x1)]
            win = (slice(y0, y1), slice(x0, x1))
            take = sm & (b["water_area_m2"] > kind_area[win])
            kind_idx[win] = np.where(take, ki, kind_idx[win])
            kind_area[win] = np.where(take, b["water_area_m2"], kind_area[win])
    for ki, k in enumerate(KIND_ORDER):
        m = kind_idx == ki
        if not m.any():
            continue
        c = np.array(matplotlib.colors.to_rgb(KIND_COLOUR[k]), np.float32)
        rgb[m] = c * (0.55 + 0.45 * shade[m])[..., None]

    # CHANNELS LAST, so the drainage network stays readable across a shore that
    # is thick with ponds. An overflowing lake and its own outlet share cells by
    # construction, and there the outlet is the thing worth seeing.
    qmax = _maxpool(np.where(land, q, 0.0), f)[:H, :W]

    # UNDER them, the perennial-but-undrawable band, in grey rather than blue.
    # These reaches carry 10 L/s or more -- they are wet all year -- and the
    # fine raster still declines to draw them because their channel is under
    # 2.8 m across. Omitting them entirely would make the world look drier than
    # it is; drawing them blue would promise water the game does not place. So
    # they are on the map and they are not blue.
    m_und = (qmax >= Q_PERENNIAL_M3_YR) & (qmax < Q_DRAW)
    c = np.array(matplotlib.colors.to_rgb(UNDRAWN_COLOUR), np.float32)
    rgb[m_und] = c * (0.55 + 0.45 * shade[m_und])[..., None]

    for ci, (name, lo, hi, col) in enumerate(CLASSES):
        m = (qmax >= lo) & (qmax < hi)
        m = _dilate(m, ci)         # streams 1 px, rivers 3 px, major 5 px wide
        c = np.array(matplotlib.colors.to_rgb(col), np.float32)
        rgb[m] = c * (0.55 + 0.45 * shade[m])[..., None]

    FIG_W, FIG_H = 19.5, 15.2
    MAP_W = 0.585
    fig = plt.figure(figsize=(FIG_W, FIG_H), dpi=130)
    axm = fig.add_axes([0.004, 0.330, MAP_W, 0.647])
    axm.imshow(np.clip(rgb, 0, 1), origin="upper", interpolation="nearest")
    axm.set_axis_off()

    # unsurveyed tiles: hatched, because a lake is never an estimate.
    # Only tiles that HAVE LAND are hatched -- an all-ocean tile has no
    # depression to miss, and hatching it would overstate the gap.
    n_unsurveyed = n_unsurveyed_ocean = 0
    for j in range(nty):
        for i in range(ntx):
            if not present[j, i] or (tx0 + i, ty0 + j) in dumps:
                continue
            if not tile_has_land[j, i]:
                n_unsurveyed_ocean += 1
                continue
            n_unsurveyed += 1
            axm.add_patch(Rectangle((i * tpx - .5, j * tpx - .5), tpx, tpx,
                                    facecolor="none", edgecolor="#d0402c",
                                    lw=0.5, hatch="///", alpha=0.75))

    bar_km, y = 50.0, H * 0.975
    axm.plot([W * 0.02, W * 0.02 + bar_km * 1000 / px_m], [y, y], "k-", lw=3)
    axm.text(W * 0.02 + bar_km * 500 / px_m, y - H * 0.008, f"{bar_km:.0f} km",
             ha="center", va="bottom", fontsize=10, weight="bold")
    axm.set_title(
        f"STREAMS, RIVERS AND LAKES  |  seed {args.seed_label}  |  "
        f"channels from {n_tiles} COARSE tiles, lakes from {len(dumps)} FINE  |  "
        f"{w * COARSE_PX_M / 1000:.0f} km @ {px_m:.0f} m/px",
        fontsize=11.5, weight="bold")

    # ---- exemplar panels: the same code, a wet tile and a dry one ---------
    # TWO of them, side by side, because the point of classifying a hole by its
    # water balance is that the SAME algorithm gives a lake in a fluvial
    # province and a salt pan in an arid one. One panel cannot show that.
    def exemplar(ax, tx, ty, d, label, npool):
        z = np.load(d["npz"])
        stride = int(z["stride"])
        zo = z["z_open_m"]
        epx_m = bp.PRODUCTION.fine_pixel_m * stride
        eshade = ls.hillshade(zo, vert_exag=2.0, dx=epx_m, dy=epx_m)
        ergb = np.repeat((0.30 + 0.70 * eshade)[..., None], 3, axis=2) * \
            np.array([0.80, 0.78, 0.72])[None, None, :]
        # Channels FIRST, water bodies over them: where a lake meets its own
        # outlet, the lake is the thing being shown at real extent.
        eq = q[(ty - ty0) * TILE_PX:(ty - ty0 + 1) * TILE_PX,
               (tx - tx0) * TILE_PX:(tx - tx0 + 1) * TILE_PX]
        up = zo.shape[0] // eq.shape[0]
        eqq = np.repeat(np.repeat(eq, up, 0), up, 1)
        for ci, (_n, lo, hi, col) in enumerate(CLASSES):
            m = _dilate((eqq >= lo) & (eqq < hi), ci)
            ergb[m] = np.array(matplotlib.colors.to_rgb(col))
        drawn = 0
        for b in d["basins"]:
            m = bs.lake_extent_mask(
                zo, (b["seed_px"][0] // stride, b["seed_px"][1] // stride),
                b["surface_m"],
                (b["bbox_px"][0] // stride, b["bbox_px"][1] // stride,
                 b["bbox_px"][2] // stride + 1, b["bbox_px"][3] // stride + 1))
            if not m.any():
                continue
            drawn += 1
            ergb[m] = np.array(matplotlib.colors.to_rgb(KIND_COLOUR[b["kind_name"]]))
        ax.imshow(np.clip(ergb, 0, 1), origin="upper", interpolation="nearest")
        ax.set_axis_off()
        prov = max(d["meta"].get("province_mix", {"?": 1.0}).items(),
                   key=lambda kv: kv[1])[0]
        pm = np.mean([b["precip_mm"] for b in d["basins"]]) if d["basins"] else float("nan")
        pet = np.mean([b["pet_mm"] for b in d["basins"]]) if d["basins"] else float("nan")
        ax.set_title(
            f"{label} of {npool} with a raster here: ({tx},{ty}), {prov.lower()}\n"
            f"P/PET {pm / max(pet, 1):.2f}   {len(d['basins'])} basins, "
            f"{drawn} hold water\n"
            f"extents filled at {epx_m:.0f} m/px, the survey raster", fontsize=8.6)
        return drawn

    def wetness(kv):
        return sum(b["water_area_m2"] for b in kv[1]["basins"] if b["kind"] >= bs.KIND_SEASONAL)

    def dryness(kv):
        return sum(1 for b in kv[1]["basins"] if b["kind"] <= bs.KIND_SEASONAL)

    # Only tiles whose npz is actually here can be drawn at ground scale. The
    # JSON half of a dump is 1.5 MB and the raster half is 11 MB, so a run that
    # pulled only the registries off the pod is a normal state, not an error --
    # it just cannot draw these two panels from a tile it has no raster for.
    havenpz = {k: v for k, v in dumps.items() if v["npz"].exists()}
    if not havenpz:
        raise SystemExit(
            f"error: none of the {len(dumps)} dumps has its .npz beside it, so "
            "no tile can be drawn at ground scale. Copy the .npz files for at "
            "least the wettest and driest tiles.")
    (wtx, wty), wd = max(havenpz.items(), key=wetness)
    (dtx, dty), dd = max((kv for kv in havenpz.items() if kv[0] != (wtx, wty)),
                         key=dryness, default=((wtx, wty), wd))
    print(f"  exemplars: wettest ({wtx},{wty}), driest ({dtx},{dty}) "
          f"of {len(havenpz)} tiles with rasters", flush=True)
    axe1 = fig.add_axes([0.603, 0.580, 0.193, 0.320])
    axe2 = fig.add_axes([0.802, 0.580, 0.193, 0.320])
    exemplar(axe1, wtx, wty, wd, "WETTEST", len(havenpz))
    exemplar(axe2, dtx, dty, dd, "DRIEST", len(havenpz))

    # ---- legend / numbers -------------------------------------------------
    n_by_kind = {k: 0 for k in KIND_ORDER}
    a_by_kind = {k: 0.0 for k in KIND_ORDER}
    per_tile_lakes, per_tile_lake_ha = [], []
    for (tx, ty), d in dumps.items():
        c = 0
        aa = 0.0
        for b in d["basins"]:
            n_by_kind[b["kind_name"]] += 1
            a_by_kind[b["kind_name"]] += b["water_area_m2"]
            if b["kind"] >= bs.KIND_SEASONAL:
                c += 1
                aa += b["water_area_m2"]
        per_tile_lakes.append(c)
        per_tile_lake_ha.append(aa / 1e4)
    per_tile_lakes = np.array(per_tile_lakes)
    per_tile_lake_ha = np.array(per_tile_lake_ha)
    lake_areas = np.array([b["water_area_m2"] for d in dumps.values()
                           for b in d["basins"] if b["kind"] >= bs.KIND_SEASONAL])
    surveyed_land_m2 = 0.0
    for (tx, ty) in dumps:
        j, i = ty - ty0, tx - tx0
        surveyed_land_m2 += float(
            land[j * TILE_PX:(j + 1) * TILE_PX,
                 i * TILE_PX:(i + 1) * TILE_PX].sum()) * COARSE_PX_M ** 2

    n_lake_bodies = sum(n_by_kind[k] for k in
                        ("lake_overflowing", "lake_terminal", "seasonal"))
    n_lake_all = sum(n_by_kind.values())

    ch_handles = [
        Line2D([], [], color=c, lw=2.6,
               label=f"{n:<12s} Q {lo / 3.156e7:>6.3f}-"
                     f"{'inf' if not np.isfinite(hi) else f'{hi / 3.156e7:.0f}'} m3/s  "
                     f"w {channel_width_m(lo):.1f}-"
                     f"{'400' if not np.isfinite(hi) else f'{channel_width_m(hi):.0f}'} m  "
                     f"{lens[n]:,.0f} km")
        for (n, lo, hi, c) in CLASSES]
    ch_handles.append(
        Line2D([], [], color=UNDRAWN_COLOUR, lw=2.6,
               label=f"{'(not drawn)':<12s} Q {Q_PERENNIAL_M3_YR / 3.156e7:6.3f}-"
                     f"{Q_DRAW / 3.156e7:.3f} m3/s  perennial, under the cut"))
    lk_handles = [
        Patch(facecolor=KIND_COLOUR[k], edgecolor="#101010", lw=.4,
              label=f"{k:<17s} {n_by_kind[k]:>5,d}   {a_by_kind[k] / 1e4:>9,.0f} ha")
        for k in KIND_ORDER]

    axl = fig.add_axes([0.603, 0.330, 0.393, 0.235])
    axl.set_axis_off()
    l1 = axl.legend(
        handles=ch_handles, loc="upper left", fontsize=8.8,
        title=("STREAMS & RIVERS -- from the %d COARSE tiles, at the SHIPPED cut\n"
               "%s drawn cells   %s km   cut %s m3/yr, D8"
               % (n_tiles, f"{n_drawn:,}", f"{total_km:,.0f}", f"{Q_DRAW:,.0f}")),
        title_fontsize=9.8, framealpha=.95, bbox_to_anchor=(0.0, 1.0))
    l1._legend_box.align = "left"
    axl.add_artist(l1)
    l2 = axl.legend(
        handles=lk_handles, loc="upper left", fontsize=8.8,
        title=("LAKES -- MEASURED from the FINE bake, %d of %d tiles, bake_ver %d\n"
               "%s registered, %s hold water, %s ha of surface"
               % (len(dumps), n_tiles, bake_ver, f"{n_lake_all:,}",
                  f"{n_lake_bodies:,}", f"{lake_areas.sum() / 1e4:,.0f}")),
        title_fontsize=9.8, framealpha=.95, bbox_to_anchor=(0.0, 0.60))
    l2._legend_box.align = "left"

    if agree:
        rr = np.array([r for _t, r, _n, _v in agree])
        lo = min(agree, key=lambda a: a[1])
        agree_line = (
            f"on the {len(agree)} baked tiles whose accumulation raster is here, the bake's OWN 1.875 m accumulation under\n"
            f"                these channels is {np.median(rr):,.0f}x the tile median.  Different currency, same PLACES: the coarse network sits on the\n"
            f"                thalwegs the shipped surface routes down.  Range {rr.min():,.0f} to {rr.max():,.0f}, and THE LOW END IS REAL, not noise -- tile\n"
            f"                {lo[0]} reads {lo[1]:.1f}x with {lo[3]:,.0f} m of relief over 15 km, and on ground that flat there is no thalweg to sit on.")
    else:
        agree_line = ("NOT DONE -- no dump here carries its accumulation raster, so this layer is\n"
                      "                unchecked against the bake and should be read as a proposal only.")

    def spread(a, fmt="{:.1f}"):
        if a.size == 0:
            return "n/a"
        return (f"min {fmt.format(a.min())} / med {fmt.format(np.median(a))} / "
                f"p90 {fmt.format(np.percentile(a, 90))} / max {fmt.format(a.max())}")

    txt = (
        f"WHAT EACH HALF IS DERIVED FROM, because the two halves do not have the same coverage and the difference is the honest part.\n"
        f"  channels  THE {n_tiles} COARSE TILES, stitched at 30 m and swept ONCE here through the bake's own kernels (fill_depressions, d8_receivers, accumulate_d8,\n"
        f"            water.runoff_field_mm_yr / discharge_source / q_drawable_m3_yr / water_head_mask).  NOT from fine tiles -- only {len(dumps)} of {n_tiles} have ever been\n"
        f"            baked, so a fine-derived network could not cover this map.  All {n_tiles} tiles, whole catchments, no pyramid and no injected boundary.\n"
        f"  lakes     THE FINE BAKE at 1.875 m: the shipped basin registry (tools/lake_survey.py dump), filtered by pipeline.basin_filter, classified by basins.classify.\n"
        f"            {n_unsurveyed} land-bearing tiles are hatched red: NOT BAKED, so no lake is drawn on them and none is implied ({n_unsurveyed_ocean} more unbaked tiles are all ocean).\n"
        f"            Nothing about a lake is ever interpolated between tiles: a lake is a hole in one specific hillside.  bake_ver {bake_ver}; a mixed set is refused.\n"
        f"\n"
        f"THE CUT IS THE BAKE'S OWN, AND IT MOVED.  Water is drawn where Q >= q_drawable = {Q_DRAW:,.0f} m3/yr ({Q_DRAW / 3.156e7:.4f} m3/s) =\n"
        f"           water.q_drawable_m3_yr({bp.PRODUCTION.fine_pixel_m} m, {consts.water_min_width_px} px), at the FINE tier's pitch and not this map's -- the same rule at 30 m asks for a 45 m\n"
        f"           channel ({bw.q_drawable_m3_yr(COARSE_PX_M, consts.water_min_width_px):.1e} m3/yr) and would erase all but the trunks.  This map used to draw at Q_perennial, {Q_DRAW / Q_PERENNIAL_M3_YR:.1f}x below the shipped cut.\n"
        f"           ROUTING IS D8 for the discharge (water_flow_single_receiver); MFD still decides the terrain -- mfd_p and the area field are untouched by this.\n"
        f"           Width: channel.h's own law (1.5 m at Q_perennial, w ~ Q^0.398, cap 400 m).  LINE WIDTHS ARE SYMBOLIC -- a 9 m river is 1/13 of a {px_m:.0f} m pixel.\n"
        f"\n"
        f"WATER CELLS AT 30 m   drawn {n_drawn:,} ({100.0 * n_drawn / max(float(land.sum()), 1.0):.2f}% of land cells), of which {int(head_stats['head_cells']):,} are heads (no wet donor) -- the sources.  Perennial\n"
        f"           (>= 10 L/s) {n_perennial:,}, so {n_undrawn:,} cells -- {100.0 * n_undrawn / max(n_perennial, 1):.1f}% of the perennial network -- are wet all year and under the cut.  Those are the GREY\n"
        f"           threads, not blue: 4.1's honesty clause kept visible rather than absorbed by the threshold.\n"
        f"\n"
        f"CHANNEL SPREAD over the {land_tiles} tiles that are at least half land (a density averaged over open ocean would be a number about the sea):\n"
        f"    per tile  length {spread(per_tile_km, '{:,.0f}')} km      drainage density {spread(per_tile_dens, '{:.2f}')} km/km2\n"
        f"    world     {total_km:,.0f} km of DRAWN channel over {land_km2:,.0f} km2 of land = {total_km / max(land_km2, 1):.2f} km/km2 (Earth: 0.5-2 in humid terrain, far less in deserts).\n"
        f"              The perennial network is denser by the count above; this figure is what gets water.\n"
        f"    CHECKED     {agree_line}\n"
        f"\n"
        f"LAKE SPREAD over the {len(dumps)} baked tiles ({tile_km2:.0f} km2 each).  AREA IS THE WATER SURFACE, read off each basin's own hypsometric curve at its datum --\n"
        f"    NOT the registry's area_m2, which is the size of the HOLE at its spill.  For an endorheic basin those differ enormously: the biggest lake\n"
        f"    here is 2,377 ha of BOWL holding 33 ha of WATER, because its balance settles 549 m below its own outlet.\n"
        f"    per tile  water bodies (seasonal or wetter) {spread(per_tile_lakes, '{:.0f}')}      their area {spread(per_tile_lake_ha, '{:,.0f}')} ha\n"
        f"    per lake  area {spread(lake_areas / 1e4, '{:,.2f}')} ha.  A {px_m:.0f} m pixel is {px_m * px_m / 1e4:.1f} ha and the median water body is {np.median(lake_areas) / 1e4:.2f} ha, so ONE PIXEL IS\n"
        f"              VERY NEARLY TO SCALE -- drawn at their own radius with a half-pixel floor, no marker and no size inflation.  Where the land looks\n"
        f"              stippled it is stippled.  Standing water covers {100.0 * lake_areas.sum() / max(surveyed_land_m2, 1.0):.3f}% of the land on the tiles that were baked.\n"
    )
    fig.text(0.006, 0.004, txt, fontsize=7.9, family="monospace", va="bottom",
             linespacing=1.25)

    fig.savefig(args.out, dpi=130, facecolor="white")
    mb = args.out.stat().st_size / 1e6
    print(f"wrote {args.out} ({mb:.1f} MB)")
    if mb > 10.0:
        print("  WARNING: over the ~10 MB upload limit (worldmaps/README.md); "
              "raise --ds or write .jpg", file=sys.stderr)

    print(f"\nchannel length by class (km): " +
          "  ".join(f"{k} {v:,.0f}" for k, v in lens.items()))
    print(f"drainage density world {total_km / max(land_km2, 1):.3f} km/km2; "
          f"per tile {spread(per_tile_dens, '{:.2f}')}")
    print(f"lakes by kind: " +
          "  ".join(f"{k} {n_by_kind[k]}" for k in KIND_ORDER))
    print(f"surveyed {len(dumps)}/{n_tiles} tiles at bake_ver {bake_ver}; "
          f"{n_unsurveyed} tiles carry no lake data")
    print(f"water cells @30m: drawn {n_drawn:,}  perennial {n_perennial:,}  "
          f"undrawn {n_undrawn:,}  heads {int(head_stats['head_cells']):,}")
    print(f"cut q_drawable {Q_DRAW:,.0f} m3/yr; routing "
          f"{'D8' if consts.water_flow_single_receiver else 'MFD'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
