"""lake_survey.py -- the basin sizing instrument (watershed plan work item 2).

WHY THIS EXISTS
---------------
Every threshold in the watershed plan's items 3 and 7 -- how deep a hole has
to be before it becomes a lake bed on the wire, how big before it is worth a
table row, what the water balance says a basin holds -- is fitted here. Before
this tool the entire evidence base was **two tiles**:

    tile          basin_cells_frac  basin_max_depth_m  max_accumulation_km2
    arid (-3,-3)  1.70%             42.4 m             381.5
    wet  (-2,-4)  2.17%             135.2 m             84.3

Two rows is a gap, not a distribution. A threshold fitted on two tiles is a
threshold fitted on one tile and its neighbour's rumour, and this codebase has
already had two confident causal explanations overturned by widening a sample
from nine tiles to 289 (tools/worldmaps/README.md). So the tool reports
**spread** -- per-tile counts, per-province fractions, depth and area
distributions, and the count of what the tile-spanning exclusion throws away
-- and never a single row.

TWO HALVES, DELIBERATELY SPLIT
------------------------------
``dump`` is the expensive half: one bake per tile (~150 CPU-s, ~5.5 GiB peak
RSS, the same call ``pregen --mode bake`` makes, superblock and climate
included), which is why it runs on the pod and why the thread caps in
``tools/bootstrap_pod.sh``/``bakeset2.sh`` matter -- numba defaults to the
core count, so N parallel bakes oversubscribe by N.

``report`` is the cheap half: numpy over the dumps, no bake, no numba. It
applies the registry filter and the water-balance constants, so **every
threshold can be swept without re-baking anything**. That is only sound
because the filter is applied AFTER connected-component labelling: components
are a property of the surface alone, and the dump records every component
above a permissive floor. Raising ``--min-depth`` in the report removes rows;
it never re-labels.

DUMP CONTENTS
-------------
``<tx>_<ty>.json``  tile metadata, bake stats, province mix, and one record
                    per component above the permissive floor (§4.2 fields plus
                    the hypsometric curve and the climate the balance read).
``<tx>_<ty>.npz``   downsampled rasters for the overlays: the final surface,
                    the re-opened surface, the registered-basin id map, and
                    accumulation. 8x down (1024^2) -- enough for a world
                    overlay, three orders of magnitude smaller than the bake.

USAGE
-----
    # on the pod, one tile (cap the threads; see the module note above)
    NUMBA_NUM_THREADS=16 OMP_NUM_THREADS=16 python3 tools/lake_survey.py dump \\
        --tile -2 -4 --seed 20260719 \\
        --cache-dir /workspace/tile-cache \\
        --provider-id terrain-diffusion-unlabeled-80b9ca451a23eae4 \\
        --out /workspace/lake-survey

    # anywhere, over all the dumps
    python tools/lake_survey.py report --dumps out/lake-survey \\
        --out docs/lake-survey [--min-depth 2.0 --min-area 2500]

``report --overlays`` additionally writes per-tile hillshade PNGs with the
registered basins tinted by kind. That needs matplotlib, which is not in the
system python -- use the terrain-diffusion venv, exactly as
``tools/worldmaps/README.md`` prescribes for the standing deliverables.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec  # noqa: E402
from terrain_service.bake import basins as bs  # noqa: E402
from terrain_service.bake import pipeline as bp  # noqa: E402
from terrain_service.bake import province as bprov  # noqa: E402
from terrain_service.cache import TileCache  # noqa: E402

SCHEMA = "vxc.lakesurvey.v1"

#: The dump's permissive floor. Everything shallower is COUNTED but not
#: recorded, because the interesting sweep starts at "a hole a player could
#: stand in" and a 10 cm dimple would multiply the record count by the noise
#: floor of the roughness field.
DUMP_FILTER = bs.BasinFilter(min_depth_m=1.0, min_area_m2=0.0,
                             exclude_spanning=False, require_above_sea=False)

#: Overlay downsample factor. 8192 -> 1024 px per tile.
OVERLAY_STRIDE = 8


# =========================================================================
# dump
# =========================================================================


def _resolve_fine_provider_id(cache_dir: Path, provider_id: str,
                              override: str | None) -> str:
    """Find the bake-derived namespace beside the coarse one.

    ``fine_provider_id`` is ``f"{provider_id}-b{bake_fingerprint[:8]}"``
    (providers/diffusion.py), and deriving it here would mean importing the
    provider stack (torch) onto a box that only needs numpy. The cache
    directory already names it, so read it rather than recompute it -- and
    fail loudly on ambiguity instead of picking one, because picking the wrong
    one silently bakes against another world's hydrology.
    """
    if override:
        return override
    cands = sorted(p.name for p in cache_dir.iterdir()
                   if p.is_dir() and p.name.startswith(provider_id + "-b"))
    if not cands:
        raise SystemExit(
            f"error: no fine namespace '{provider_id}-b*' under {cache_dir}. "
            "Nothing has been baked into this cache; pass "
            "--fine-provider-id if you know it.")
    if len(cands) > 1:
        raise SystemExit(
            f"error: {len(cands)} fine namespaces under {cache_dir}: "
            f"{cands}. They are different bakes of the same world; pass "
            "--fine-provider-id to say which.")
    return cands[0]


def _fetchers(cache: TileCache, provider_id: str, seed: int):
    """``(elevation_fetch, climate_fetch)`` reading the cache only.

    Never generates. A dump is a measurement of a world that already exists;
    inventing a coarse tile here would measure a different one.
    """
    planes: dict[tuple[int, int], tuple | None] = {}

    def _planes(x: int, y: int):
        key = (x, y)
        if key not in planes:
            data = cache.get(provider_id, seed, x, y, 1)
            if data is None:
                planes[key] = None
            else:
                t = tile_codec.decode(data)
                planes[key] = (t.elevation.astype(np.float32), t.climate)
        return planes[key]

    def fetch(x: int, y: int):
        p = _planes(x, y)
        return None if p is None else p[0]

    def fetch_climate(x: int, y: int):
        p = _planes(x, y)
        return None if p is None else p[1]

    return fetch, fetch_climate


def _superblock(cache: TileCache, fine_provider_id: str, seed: int,
                tx: int, ty: int, geom, consts, fetch, kernels):
    """The level-0 flow superblock this tile bakes against.

    Reused from the cache when it is there -- it is what the shipped tile was
    baked against, and rebuilding it costs minutes -- and rebuilt when it is
    not. The COMPLETENESS of the block is reported either way: a tile baked
    against an incomplete superblock has permanently lost the rivers entering
    from the absent tiles, and a basin's catchment measured there is an
    understatement, not a measurement.
    """
    level = bp.FlowLevel(level=0, geom=geom, consts=consts)
    sx, sy = bp.superblock_index(tx, ty, level)
    blob = cache.get_flow(fine_provider_id, seed, 0, sx, sy)
    if blob is not None:
        try:
            sb, _ = bp.decode_flow_superblock(blob)
            return sb, True
        except ValueError as e:
            # THIS USED TO REBUILD, AND THAT WAS WRONG. `build_flow_superblock`
            # here takes no `parent`, so the rebuilt L0 block carries no inflow
            # from the levels above it -- while the cached one was built by
            # pregen WITH the pyramid. Silently substituting it understates
            # every catchment on the tile, which is precisely the quantity this
            # survey exists to measure, and it happens exactly when a
            # BAKE_VERSION bump invalidates the cache: the moment the numbers
            # matter most.
            raise SystemExit(
                f"error: cached flow superblock L0 ({sx},{sy}) is unusable "
                f"({e}).\n"
                "  Rebuild the PYRAMID with `pregen --mode bake` against this "
                "cache before surveying. This tool refuses to build a\n"
                "  parentless L0 block, because a survey run against one "
                "reports catchments that are understatements, not measurements."
            ) from e
    print(f"  note: no cached superblock L0 ({sx},{sy}); building one. It will "
          "have NO parent inflow -- catchments are understatements.",
          file=sys.stderr)
    sb = bp.build_flow_superblock(fetch, sx, sy, level, kernels)
    return sb, False


def cmd_dump(args) -> int:
    tx, ty = args.tile
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    js = out_dir / f"{tx}_{ty}.json"
    npz = out_dir / f"{tx}_{ty}.npz"
    if js.exists() and npz.exists() and not args.force:
        print(f"  reusing {js} (pass --force to redo)")
        return 0

    cache_dir = Path(args.cache_dir)
    cache = TileCache(cache_dir)
    fine_id = _resolve_fine_provider_id(cache_dir, args.provider_id,
                                        args.fine_provider_id)
    geom = bp.PRODUCTION
    geom.assert_production()
    consts = bp.CONSTANTS
    fetch, fetch_climate = _fetchers(cache, args.provider_id, args.seed)
    if fetch(tx, ty) is None:
        raise SystemExit(f"error: coarse tile ({tx},{ty}) is not in {cache_dir}")

    kernels = bp.load_kernels()
    sb, cached_sb = _superblock(cache, fine_id, args.seed, tx, ty, geom, consts,
                                fetch, kernels)
    if not sb.complete:
        print(f"  warning: flow superblock for ({tx},{ty}) is INCOMPLETE "
              f"({len(sb.missing_tiles)} coarse tiles absent). Catchments here "
              "are an understatement.", file=sys.stderr)

    print(f"[{tx},{ty}] baking padded domain "
          f"({geom.padded_fine_px}^2 @ {geom.fine_pixel_m} m/px, "
          f"peak ~{bp.estimate_peak_bytes(geom) / 2**30:.1f} GiB)", flush=True)
    c0 = time.process_time()
    padded = bp.assemble_padded_coarse(fetch, tx, ty, geom)[0]
    padded_climate = bp.assemble_padded_climate(fetch_climate, tx, ty, geom)
    out = bp.bake_padded_domain(
        padded,
        world_seed=args.seed,
        tile_x=tx,
        tile_y=ty,
        kernels=kernels,
        geom=geom,
        consts=consts,
        inflow_source=sb,
        padded_climate=padded_climate,
    )
    cpu = time.process_time() - c0
    print(f"  bake {cpu:.0f} s cpu", flush=True)

    climate = (None if padded_climate is None
               else bprov.dequantize_climate(padded_climate))
    interior = geom.interior()
    c1 = time.process_time()
    survey = bs.survey_basins(
        z_final=out["z"],
        basin_depth=out["basin_depth"],
        accumulation_m2=out["acc"],
        climate=climate,
        cell_m=geom.fine_pixel_m,
        interior=interior,
        filt=DUMP_FILTER,
        # WHERE THIS TILE IS. Without it every tile's basins would be measured
        # as if the world began at their own corner, so `global_id` -- the
        # thing that says two tiles are looking at ONE lake -- would collide
        # across the survey instead of matching. The dump already sweeps whole
        # blocks of tiles, which is exactly where that shows up.
        world_origin_px=(tx * geom.fine_tile_px, ty * geom.fine_tile_px),
    )
    print(f"  survey {time.process_time() - c1:.0f} s cpu: "
          f"{survey.n_components} components, {len(survey.basins)} recorded",
          flush=True)

    # PROVINCE, through the real classifier -- the object the bake itself
    # built (pipeline.py's own province_fields call), not a re-derivation.
    prov = out.get("province")
    prov_mix: dict[str, float] = {}
    prov_at_basin: list[str] = []
    if prov is not None:
        cs = slice(geom.apron_coarse_px,
                   geom.apron_coarse_px + geom.coarse_tile_px)
        for name, wgt in prov.weights.items():
            prov_mix[name] = float(wgt[cs, cs].mean())
        names = list(prov.weights)
        ratio = out["z"].shape[0] // next(iter(prov.weights.values())).shape[0]
        stack = np.stack([prov.weights[n] for n in names])
        for b in survey.basins:
            cy = min((b.seed_px[1] + interior.start) // ratio, stack.shape[1] - 1)
            cx = min((b.seed_px[0] + interior.start) // ratio, stack.shape[2] - 1)
            prov_at_basin.append(names[int(np.argmax(stack[:, cy, cx]))])
        del stack
    else:
        prov_at_basin = ["UNKNOWN"] * len(survey.basins)

    z_int = np.ascontiguousarray(out["z"][interior, interior])
    acc_int = np.ascontiguousarray(out["acc"][interior, interior])
    rec = {
        "schema": SCHEMA,
        "tile": [tx, ty],
        "seed": args.seed,
        "provider_id": args.provider_id,
        "fine_provider_id": fine_id,
        "bake_version": bp.BAKE_VERSION,
        "bake_fingerprint": bp.bake_fingerprint(geom, consts),
        "fine_pixel_m": geom.fine_pixel_m,
        "fine_tile_px": geom.fine_tile_px,
        "padded_fine_px": geom.padded_fine_px,
        "superblock_fingerprint": sb.fingerprint_hex,
        "superblock_complete": bool(sb.complete),
        "superblock_missing_tiles": len(sb.missing_tiles),
        "superblock_from_cache": cached_sb,
        "cpu_seconds_bake": cpu,
        "dump_filter": {
            "min_depth_m": DUMP_FILTER.min_depth_m,
            "min_area_m2": DUMP_FILTER.min_area_m2,
            "exclude_spanning": DUMP_FILTER.exclude_spanning,
            "require_above_sea": DUMP_FILTER.require_above_sea,
            "border_margin_px": DUMP_FILTER.border_margin_px,
        },
        "tile_stats": {
            "relief_m": float(z_int.max() - z_int.min()),
            "elev_min_m": float(z_int.min()),
            "elev_max_m": float(z_int.max()),
            "max_accumulation_km2": float(acc_int.max()) / 1e6,
            "basin_cells_frac": float((out["basin_depth"][interior, interior] > 0).mean()),
            "basin_max_depth_m": float(out["basin_depth"][interior, interior].max()),
            "has_climate": climate is not None,
        },
        "province_mix": prov_mix,
        "survey": {
            "n_components": survey.n_components,
            "excluded_shallow": survey.excluded_shallow,
            "excluded_small": survey.excluded_small,
            "excluded_submarine": survey.excluded_submarine,
            "excluded_spanning": survey.excluded_spanning,
            "excluded_spanning_area_m2": survey.excluded_spanning_area_m2,
            "excluded_spanning_max_depth_m": survey.excluded_spanning_max_depth_m,
            "kept_near_padded_border": survey.kept_near_padded_border,
            "total_depression_volume_m3": survey.total_depression_volume_m3,
            "kept_volume_m3": survey.kept_volume_m3,
        },
        "basins": [
            {**b.as_dict(), "province": p}
            for b, p in zip(survey.basins, prov_at_basin)
        ],
    }
    js.write_text(json.dumps(rec, indent=1))

    s = OVERLAY_STRIDE
    z_open_int = bs.reopened_surface(out["z"], out["basin_depth"])[interior, interior]
    ids = np.zeros((geom.fine_tile_px // s, geom.fine_tile_px // s), np.int32)
    for b in survey.basins:
        x0, y0, x1, y1 = b.bbox_px
        ids[y0 // s:y1 // s + 1, x0 // s:x1 // s + 1] = np.maximum(
            ids[y0 // s:y1 // s + 1, x0 // s:x1 // s + 1], b.basin_id + 1)
    np.savez_compressed(
        npz,
        z_m=z_int[::s, ::s].astype(np.float32),
        z_open_m=np.ascontiguousarray(z_open_int[::s, ::s]).astype(np.float32),
        acc_m2=acc_int[::s, ::s].astype(np.float32),
        basin_bbox_id=ids,
        stride=np.int32(s),
    )
    print(f"  wrote {js.name} ({js.stat().st_size / 1e6:.1f} MB) and {npz.name}")
    return 0


# =========================================================================
# report
# =========================================================================


def _load(dump_dir: Path) -> list[dict]:
    recs = []
    for p in sorted(dump_dir.glob("*.json")):
        r = json.loads(p.read_text())
        if r.get("schema") != SCHEMA:
            continue
        r["_path"] = str(p)
        recs.append(r)
    if not recs:
        raise SystemExit(f"error: no {SCHEMA} dumps in {dump_dir}")
    return recs


def _refilter(rec: dict, filt: bs.BasinFilter, wb: bs.WaterBalance) -> list[dict]:
    """Re-apply the registry filter and re-run the balance on a dump's rows.

    Exact rather than approximate: components were labelled once, on the
    surface, and the filter only ever removes rows. The balance is re-run
    because ``pet()``/``budyko()`` are the thing being fitted -- the dump
    stores the climate the balance read, not just its verdict.
    """
    out = []
    for b in rec["basins"]:
        if b["depth_m"] < filt.min_depth_m:
            continue
        if b["area_m2"] < filt.min_area_m2:
            continue
        if filt.require_above_sea and b["spill_m"] <= bs.SEA_LEVEL_M:
            continue
        if filt.exclude_spanning and not b["interior"]:
            continue
        pet = float(bs.pet_mm_yr(b["temp_c"], wb))
        runoff = float(bs.budyko_runoff_mm_yr(b["precip_mm"], pet, wb))
        land = max(b["catchment_m2"] - b["area_m2"], 0.0)
        inflow = runoff / 1000.0 * land
        h, need = bs.equilibrium_level(
            np.array(b["hyps_levels_m"]), np.array(b["hyps_areas_m2"]),
            inflow, b["precip_mm"], pet)
        kind, surface = bs.classify(h, b["floor_m"], b["spill_m"],
                                    precip_mm=b["precip_mm"], pet_mm=pet,
                                    precip_cv=b["precip_cv"], wb=wb)
        nb = dict(b)
        nb.update(pet_mm=pet, runoff_mm=runoff, inflow_m3_yr=inflow,
                  balance_area_m2=need, kind=kind,
                  kind_name=bs.KIND_NAMES[kind], surface_m=surface,
                  water_depth_m=max(surface - b["floor_m"], 0.0))
        out.append(nb)
    return out


def _pct(vals, qs=(0, 25, 50, 75, 90, 100)):
    if not len(vals):
        return [float("nan")] * len(qs)
    return [float(np.percentile(vals, q)) for q in qs]


def _fmt(v: float, w: int = 8, p: int = 1) -> str:
    if v != v:
        return "-".rjust(w)
    if abs(v) >= 1e5:
        return f"{v:{w}.3g}"
    return f"{v:{w}.{p}f}"


def cmd_report(args) -> int:
    recs = _load(Path(args.dumps))
    filt = bs.BasinFilter(min_depth_m=args.min_depth, min_area_m2=args.min_area,
                          exclude_spanning=not args.keep_spanning)
    wb = bs.WaterBalance()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    lines: list[str] = []

    def emit(s: str = "") -> None:
        print(s)
        lines.append(s)

    emit("# Lake survey")
    emit()
    emit(f"`{SCHEMA}` -- {len(recs)} tiles, seed "
         f"{recs[0]['seed']}, bake_version {recs[0]['bake_version']}, "
         f"fingerprint `{recs[0]['bake_fingerprint'][:16]}`.")
    emit()
    emit(f"Registry filter: depth >= {filt.min_depth_m} m, area >= "
         f"{filt.min_area_m2:.0f} m2, spill above sea level "
         f"({bs.SEA_LEVEL_M:g} m), tile-spanning basins "
         f"{'EXCLUDED' if filt.exclude_spanning else 'KEPT'}.")
    emit()
    emit("**These numbers describe the bake_version above and no other.** "
         "`roughness_seed` takes `BAKE_VERSION` as an input "
         "(`pipeline.py:1130`), so bumping it reseeds the B1 roughness field "
         "and therefore changes the terrain everywhere -- which is the "
         "documented intent of a bump ('a bake change yields a new world'), "
         "and is why a survey does not survive one. Measured across the 7->8 "
         "bump on tile (-2,-4): 6,237 depression components became 5,905 "
         "(-5.3%) and 151 registered basins became 144 (-4.6%). Same order of "
         "magnitude, different world. Re-run the dumps after any bump -- and "
         "rebuild the flow pyramid first, which the bump also invalidates.")
    emit()
    emit(f"Water balance: PET = {wb.pet_a:g} + {wb.pet_b:g}T + {wb.pet_c:g}T^3 "
         f"(floor {wb.pet_floor_mm:g} mm/yr), Budyko n = {wb.budyko_n:g}, "
         f"lake if >= {wb.min_lake_depth_m:g} m deep, salt if P/PET < "
         f"{wb.salt_aridity:g}, seasonal if CV >= {wb.seasonal_cv_pct:g}%.")
    emit()

    per_tile = []
    all_basins: list[dict] = []
    for r in recs:
        kept = _refilter(r, filt, wb)
        all_basins.extend([dict(b, _tile=tuple(r["tile"])) for b in kept])
        prov = max(r["province_mix"].items(), key=lambda kv: kv[1])[0] \
            if r["province_mix"] else "UNKNOWN"
        per_tile.append({
            "tile": tuple(r["tile"]),
            "province": prov,
            "prov_frac": (max(r["province_mix"].values())
                          if r["province_mix"] else float("nan")),
            "n": len(kept),
            "lakes": sum(1 for b in kept if b["kind"] >= bs.KIND_LAKE_TERMINAL),
            "playas": sum(1 for b in kept if b["kind"] <= bs.KIND_SALT_FLAT),
            "seasonal": sum(1 for b in kept if b["kind"] == bs.KIND_SEASONAL),
            "spanning": r["survey"]["excluded_spanning"],
            "spanning_max_depth": r["survey"]["excluded_spanning_max_depth_m"],
            "spanning_area_ha": r["survey"]["excluded_spanning_area_m2"] / 1e4,
            "submarine": r["survey"]["excluded_submarine"],
            "near_edge": r["survey"]["kept_near_padded_border"],
            "components": r["survey"]["n_components"],
            "shallow": r["survey"]["excluded_shallow"],
            "basin_frac": r["tile_stats"]["basin_cells_frac"],
            "max_depth": r["tile_stats"]["basin_max_depth_m"],
            "max_acc_km2": r["tile_stats"]["max_accumulation_km2"],
            "relief": r["tile_stats"]["relief_m"],
            "temp": float(np.median([b["temp_c"] for b in kept])) if kept else float("nan"),
            "precip": float(np.median([b["precip_mm"] for b in kept])) if kept else float("nan"),
            "sb_complete": r["superblock_complete"],
        })

    emit("## Per tile")
    emit()
    emit("| tile | province | basins | lake | seasonal | playa | spanning excl "
         "| submarine excl | near edge | comps | basin% | max depth m "
         "| max catch km2 | relief m |")
    emit("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for t in per_tile:
        emit(f"| ({t['tile'][0]},{t['tile'][1]}) | {t['province']} "
             f"{t['prov_frac']*100:.0f}% | {t['n']} | {t['lakes']} | "
             f"{t['seasonal']} | {t['playas']} | {t['spanning']} "
             f"({t['spanning_area_ha']:.0f} ha, max {t['spanning_max_depth']:.0f} m) | "
             f"{t['submarine']} | {t['near_edge']} | {t['components']} | "
             f"{t['basin_frac']*100:.2f}% | "
             f"{t['max_depth']:.1f} | {t['max_acc_km2']:.1f} | "
             f"{t['relief']:.0f} |")
    emit()

    ns = np.array([t["n"] for t in per_tile], float)
    emit(f"**Basins per tile: min {ns.min():.0f}, median {np.median(ns):.0f}, "
         f"max {ns.max():.0f}, mean {ns.mean():.1f}** over {len(ns)} tiles "
         f"({int(ns.sum())} basins total). The spread is the point: a "
         f"per-tile budget sized on the median would be "
         f"{ns.max() / max(np.median(ns), 1):.1f}x short on the worst tile.")
    emit()

    # ---- distributions
    emit("## Distributions over all registered basins")
    emit()
    emit("| quantity | min | p25 | median | p75 | p90 | max |")
    emit("|---|---|---|---|---|---|---|")
    for label, key, scale in (
        ("depth to spill, m", "depth_m", 1.0),
        ("water depth, m", "water_depth_m", 1.0),
        ("area, ha", "area_m2", 1e-4),
        ("catchment, km2", "catchment_m2", 1e-6),
        ("inflow, 1e3 m3/yr", "inflow_m3_yr", 1e-3),
        ("precip, mm/yr", "precip_mm", 1.0),
        ("PET, mm/yr", "pet_mm", 1.0),
        ("runoff, mm/yr", "runoff_mm", 1.0),
    ):
        v = np.array([b[key] for b in all_basins], float) * scale
        q = _pct(v)
        emit(f"| {label} | " + " | ".join(_fmt(x, 1, 2).strip() for x in q) + " |")
    emit()

    # ---- kind mix, by province
    emit("## Lake vs playa, by province")
    emit()
    emit("Province is the bake's own `province_fields` weight, argmax at the "
         "basin's deepest cell -- the real classifier, per "
         "`tools/worldmaps/README.md`.")
    emit()
    provs = sorted({b.get("province", "UNKNOWN") for b in all_basins})
    emit("| province | basins | " + " | ".join(bs.KIND_NAMES) +
         " | lake frac | median P mm | median PET mm | median P/PET |")
    emit("|---|---|" + "---|" * (len(bs.KIND_NAMES) + 4))
    for p in provs:
        sel = [b for b in all_basins if b.get("province", "UNKNOWN") == p]
        counts = [sum(1 for b in sel if b["kind"] == k)
                  for k in range(len(bs.KIND_NAMES))]
        lake = sum(counts[bs.KIND_LAKE_TERMINAL:])
        pr = np.array([b["precip_mm"] for b in sel], float)
        pe = np.array([b["pet_mm"] for b in sel], float)
        emit(f"| {p} | {len(sel)} | " + " | ".join(str(c) for c in counts) +
             f" | {lake / max(len(sel), 1) * 100:.0f}% | "
             f"{np.median(pr):.0f} | {np.median(pe):.0f} | "
             f"{np.median(pr / pe):.2f} |")
    emit()

    # ---- the exclusion cost
    span = sum(t["spanning"] for t in per_tile)
    sub = sum(t["submarine"] for t in per_tile)
    near = sum(t["near_edge"] for t in per_tile)
    span_ha = sum(t["spanning_area_ha"] for t in per_tile)
    kept_n = len(all_basins)
    emit("## What the exclusions cost")
    emit()
    emit(f"**Tile-spanning ({span} components, {span_ha:.0f} ha, deepest "
         f"{max((t['spanning_max_depth'] for t in per_tile), default=0):.0f} m).** "
         f"Against {kept_n} registered interior basins that is "
         f"{span / max(span + kept_n, 1) * 100:.1f}% of qualifying components. "
         f"A basin crossing the tile edge is registered independently by each "
         f"tile that sees it, from a different padded domain, and the two need "
         f"not agree; its catchment also crosses the seam. v1 refuses them and "
         f"counts them (§4.2.4). The v2 fix is HYDROLOGY_RESIDUALS #6's: a fill "
         f"boundary condition from the superblock's shared `filled` raster.")
    emit()
    emit(f"**Submarine ({sub} components).** Depressions whose spill is at or "
         f"below sea level are sea floor, not lake basins -- the ocean already "
         f"covers them, and a lake surface there would be a second water plane "
         f"under the first. This exclusion is NOT in the plan and was added "
         f"after the survey found one: tile (-8,-14)'s largest basin sits at "
         f"-433 m.")
    emit()
    emit(f"**Near the padded edge ({near} of {kept_n} kept basins, "
         f"{near / max(kept_n, 1) * 100:.1f}%).** A diagnostic, not an "
         f"exclusion. NOTE: `pipeline.py`'s shipped "
         f"`basin_reaches_padded_border` / `padded_border_basin_frac` stats "
         f"are structurally ZERO on every tile ever baked -- "
         f"`fill_depressions` never raises a border cell, so `filled - fine` "
         f"is identically 0 there and no depression can contain one. This "
         f"near-miss margin is the honest replacement.")
    emit()

    # ---- threshold sensitivity
    emit("## Threshold sensitivity")
    emit()
    emit("Every row is the same labelled components re-filtered -- no re-bake, "
         "no re-labelling.")
    emit()
    emit("| min depth m | min area m2 | basins | per tile median | lakes | "
         "playas | bytes/tile at 32 B |")
    emit("|---|---|---|---|---|---|---|")
    for d in (1.0, 2.0, 3.0, 5.0, 10.0):
        for a in (0.0, 2500.0, 10000.0):
            f2 = bs.BasinFilter(min_depth_m=d, min_area_m2=a,
                                exclude_spanning=filt.exclude_spanning)
            counts, lk, pl = [], 0, 0
            for r in recs:
                k = _refilter(r, f2, wb)
                counts.append(len(k))
                lk += sum(1 for b in k if b["kind"] >= bs.KIND_LAKE_TERMINAL)
                pl += sum(1 for b in k if b["kind"] <= bs.KIND_SALT_FLAT)
            tot = int(sum(counts))
            emit(f"| {d:g} | {a:.0f} | {tot} | {np.median(counts):.0f} | "
                 f"{lk} | {pl} | {max(counts) * 32} |")
    emit()

    # ---- the climate the balance saw
    emit("## The climate the balance saw, and what it was fitted against")
    emit()
    ai = np.array([b["precip_mm"] / b["pet_mm"] for b in all_basins], float)
    bands = (("hyper-arid <0.05", ai < 0.05),
             ("arid 0.05-0.20", (ai >= 0.05) & (ai < 0.20)),
             ("semi-arid 0.20-0.50", (ai >= 0.20) & (ai < 0.50)),
             ("dry sub-humid 0.50-0.65", (ai >= 0.50) & (ai < 0.65)),
             ("humid >=0.65", ai >= 0.65))
    emit("UNEP aridity bands over every registered basin. Earth's land surface "
         "is roughly 8 / 12 / 18 / 6 / 56% across these five.")
    emit()
    emit("| band | basins | share | lake frac |")
    emit("|---|---|---|---|")
    for name, sel in bands:
        n = int(sel.sum())
        lk = sum(1 for b, s in zip(all_basins, sel)
                 if s and b["kind"] >= bs.KIND_LAKE_TERMINAL)
        emit(f"| {name} | {n} | {n / max(len(ai), 1) * 100:.1f}% | "
             f"{lk / max(n, 1) * 100:.0f}% |")
    emit()

    # ---- the spawn site item 11 asks for
    lakes = [b for b in all_basins
             if b["kind"] >= bs.KIND_LAKE_TERMINAL and b["interior"]]
    lakes.sort(key=lambda b: -b["water_depth_m"])
    emit("## Deepest interior lakes (the first-playable spawn sites, plan §11)")
    emit()
    emit("| tile | basin | kind | water depth m | area ha | catchment km2 | "
         "surface m | -VoxelSpawnAt |")
    emit("|---|---|---|---|---|---|---|---|")
    px = recs[0]["fine_pixel_m"]
    tpx = recs[0]["fine_tile_px"]
    for b in lakes[:10]:
        tx, ty = b["_tile"]
        wx = (tx * tpx + b["seed_px"][0]) * px
        wy = (ty * tpx + b["seed_px"][1]) * px
        emit(f"| ({tx},{ty}) | {b['basin_id']} | {b['kind_name']} | "
             f"{b['water_depth_m']:.1f} | {b['area_m2'] / 1e4:.1f} | "
             f"{b['catchment_m2'] / 1e6:.2f} | {b['surface_m']:.1f} | "
             f"`{wx:.0f}:{wy:.0f}` |")
    emit()

    md = out_dir / "lake-survey.md"
    md.write_text("\n".join(lines) + "\n")
    (out_dir / "lake-survey.json").write_text(json.dumps(
        {"schema": SCHEMA, "filter": filt.__dict__, "balance": wb.__dict__,
         "per_tile": [{**t, "tile": list(t["tile"])} for t in per_tile],
         "basins": [{**b, "_tile": list(b["_tile"])} for b in all_basins]},
        indent=1))
    print(f"\nwrote {md} and {out_dir / 'lake-survey.json'}")

    if args.overlays:
        _overlays(recs, all_basins, Path(args.dumps), out_dir, filt, wb)
    return 0


def _overlays(recs, all_basins, dump_dir: Path, out_dir: Path,
              filt: bs.BasinFilter, wb: bs.WaterBalance) -> None:
    """Per-tile hillshade with the registered basins tinted by kind.

    The standing world-generation deliverable convention: an overlay per
    exemplar, on the same ground, so "this hole is a lake and that identical
    hole is a playa" is a picture and not a claim.
    """
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.colors import ListedColormap
    except ImportError:
        print("note: matplotlib missing -- skipping overlays. Use the "
              "terrain-diffusion venv (tools/worldmaps/README.md).",
              file=sys.stderr)
        return
    kind_rgb = ListedColormap(
        [(0.78, 0.66, 0.42), (0.93, 0.93, 0.88), (0.62, 0.78, 0.70),
         (0.15, 0.42, 0.68), (0.10, 0.62, 0.85)])
    for r in recs:
        tx, ty = r["tile"]
        npz = dump_dir / f"{tx}_{ty}.npz"
        if not npz.exists():
            continue
        d = np.load(npz)
        z = d["z_m"]
        stride = int(d["stride"])
        cell = r["fine_pixel_m"] * stride
        gy, gx = np.gradient(z.astype(np.float64), cell)
        slope = np.arctan(np.hypot(gx, gy))
        aspect = np.arctan2(-gx, gy)
        az, alt = math.radians(315.0), math.radians(45.0)
        hs = (np.sin(alt) * np.cos(slope)
              + np.cos(alt) * np.sin(slope) * np.cos(az - aspect))
        fig, ax = plt.subplots(figsize=(8, 8), dpi=128)
        ax.imshow(hs, cmap="gray", vmin=0, vmax=1, origin="upper")
        kept = _refilter(r, filt, wb)
        for b in kept:
            x0, y0, x1, y1 = [v / stride for v in b["bbox_px"]]
            ax.add_patch(plt.Rectangle(
                (x0, y0), max(x1 - x0, 1.5), max(y1 - y0, 1.5),
                fill=True, alpha=0.55, lw=0.6,
                facecolor=kind_rgb(b["kind"]), edgecolor="k"))
        prov = (max(r["province_mix"].items(), key=lambda kv: kv[1])[0]
                if r["province_mix"] else "UNKNOWN")
        ax.set_title(f"tile ({tx},{ty})  {prov}  {len(kept)} basins  "
                     f"{sum(1 for b in kept if b['kind'] >= bs.KIND_LAKE_TERMINAL)}"
                     f" lakes", fontsize=10)
        ax.set_xticks([])
        ax.set_yticks([])
        p = out_dir / f"basins-{tx}_{ty}.png"
        fig.savefig(p, bbox_inches="tight")
        plt.close(fig)
        print(f"  wrote {p}")


# =========================================================================


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("dump", help="bake one tile and record its basins")
    d.add_argument("--tile", nargs=2, type=int, required=True, metavar=("TX", "TY"))
    d.add_argument("--seed", type=int, required=True)
    d.add_argument("--cache-dir", required=True)
    d.add_argument("--provider-id", required=True,
                   help="the COARSE provider id; the fine namespace is found beside it")
    d.add_argument("--fine-provider-id", default=None,
                   help="override, when the cache holds more than one bake")
    d.add_argument("--out", required=True)
    d.add_argument("--force", action="store_true")
    d.set_defaults(fn=cmd_dump)

    r = sub.add_parser("report", help="spread tables over a set of dumps")
    r.add_argument("--dumps", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--min-depth", type=float, default=bs.BasinFilter().min_depth_m)
    r.add_argument("--min-area", type=float, default=bs.BasinFilter().min_area_m2)
    r.add_argument("--keep-spanning", action="store_true",
                   help="do NOT exclude basins reaching the padded border")
    r.add_argument("--overlays", action="store_true",
                   help="write per-tile hillshade overlays (needs matplotlib)")
    r.set_defaults(fn=cmd_report)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
