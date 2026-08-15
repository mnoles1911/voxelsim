"""Command line for the generator.

    python -m forge.cli gen    specs/temperate-oak.json --seed 7
    python -m forge.cli batch  specs/temperate-oak.json --count 100
    python -m forge.cli survey                       # every spec, side by side
    python -m forge.cli check  specs/temperate-oak.json
    python -m forge.cli schema                       # slider table as JSON
    python -m forge.cli selftest
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from . import biomes as biomelib, contact, kinds, parts as partslib, materials, pipeline, render, spec as specmod, vox, vxa

ROOT = Path(__file__).resolve().parent.parent
SPECS = ROOT / "specs"
OUT = ROOT / "out"

# THE SINGLE-ASSET RULE (owner, 2026-08-11)
#
# One generation produces one thing: 1 tree, 1 rock, 1 grass clump, 1 clump of
# reeds, 1 clump of flowers. Small, medium and large are separate assets, saved
# separately and placed together later by placement logic -- so a secondary
# boulder, a scree ring or a shed block that arrives in the same grid is not a
# feature of the species, it is a second asset the library cannot address,
# cannot cost and cannot place.
#
# This supersedes the weaker rule that came before it, which only asked whether
# a loose piece was SUPPORTED (`pipeline._drop_airborne` ->
# `stats["airborne_kept"]`). A rubble ring sitting flat on the ground passed
# that and still ships two assets in one file. Support is still worth measuring
# and is still reported below, but it is no longer the bar.
#
# Specs KNOWN to break the rule today. Each entry names the plan item that
# removes it; the entry goes when the item lands, never the check. `--no-allow`
# on tools/buildcheck.py ignores this list entirely, which is how a fix gets
# verified and how the true count is read off at any time.
#
# An entry whose spec now builds as one piece is itself a FAILURE -- see
# `stale_allowances`. Same rule the shader and unity lints use for their
# silencing annotations, and for the same reason: a permission that outlives
# the defect it was written for is how a check gets switched off by accident.
KNOWN_MULTIPIECE: dict[str, str] = {}


def stale_allowances(built: dict[str, bool]) -> list[str]:
    """Allow-list entries for specs that came out as one piece after all.

    `built` maps spec name -> did it break the rule. Only names actually built
    in this run are judged, so `--kind rock` cannot report the tree entries as
    stale.
    """
    return sorted(name for name, broke in built.items()
                  if name in KNOWN_MULTIPIECE and not broke)


# The specs that dominate a whole-library pass. Measured, not guessed -- times
# are `tools/buildcheck.py` at seed 1 and the AUTHORED lattice on a dev box
# (which is 10 cm for the two arches and the sea stack, 5 cm for the rest), and
# the cut is at one minute:
#
#     hero-arch-colossal     24.1M voxels   20   min      <- 65% of the library
#     hero-sea-stack          1.5M voxels    1.8 min
#     hero-balanced-rock      5.2M voxels    1.6 min
#     hero-sequoia           19.3M voxels    1.2 min
#     everything else (61)                  ~10   min
#
# `hero-arch-colossal` is a LANDMARK, not a species -- one arch, at one place,
# in one world. It is here because it is authored as a spec like everything
# else, and it should be baked to the library once and placed from there rather
# than rebuilt. Its cost is per-seed and it varies a lot with the seed: 24.1M
# voxels on seed 1 against 39.6M on seed 2, which is what a 2.4 elongation and
# a size-fitting loop do to each other.
#
# CI splits on this set rather than skipping it: the cheap 62 run on every pull
# request, the heavy four on pushes to main. Nothing is excluded from CI, and
# the local commands take `--skip-heavy` / `--only-heavy` for the same split.
HEAVY_SPECS = {
    "hero-arch-colossal",
    "hero-sea-stack",
    "hero-balanced-rock",
    "hero-sequoia",
}


def spec_paths(kind: str | None = None) -> list[Path]:
    """Every spec on disk, optionally of one kind. Read from the files rather
    than a list kept here, so a new species is covered the day it is authored."""
    out = []
    for p in sorted(SPECS.glob("*.json")):
        if kind is None:
            out.append(p)
            continue
        try:
            if json.loads(p.read_text(encoding="utf-8")).get("kind") == kind:
                out.append(p)
        except (OSError, ValueError):
            out.append(p)  # unreadable is a failure for the caller to report
    return out


def pieces(asset) -> tuple[int, list[int]]:
    """How many separate pieces a build produced, largest first.

    26-connectivity -- `np.ones((3,3,3))`, the neighbourhood `_drop_airborne`
    and `attached_frac` already use. That is the LENIENT reading: a piece
    touching the body at a single corner counts as attached. Deliberate. The
    strict face-connected rule is already applied where a corner join really
    does fall off, by the wood check; using it here as well would fail specs
    for foliage speckle rather than for shipping two rocks.
    """
    import numpy as np

    try:
        from scipy import ndimage
    except ImportError as e:      # loud, not silent
        raise SystemExit(
            "the one-piece check needs scipy (pip install scipy)."
        ) from e
    # The warning that used to sit here -- that `pipeline` would silently do
    # nothing without scipy and so a clean run proved nothing -- was true and is
    # no longer: `pipeline._ndimage()` raises instead of returning the healthy
    # answer. Left as a note rather than deleted, because a stale caveat that
    # tells you not to trust a check is worse than none at all.

    occ = asset.grid.data != 0
    if not occ.any():
        return 0, []
    lab, n = ndimage.label(occ, structure=np.ones((3, 3, 3), bool))
    sizes = sorted((int(v) for v in np.bincount(lab.ravel())[1:]), reverse=True)
    return int(n), sizes


def loose_summary(asset) -> tuple[int, float, str]:
    """(pieces beyond the first, their share of the asset, one line saying so)."""
    n, sizes = pieces(asset)
    if n <= 1:
        return 0, 0.0, ""
    loose = sum(sizes[1:])
    total = max(sum(sizes), 1)
    airborne = asset.stats.get("airborne_kept") or []
    return n - 1, loose / total, (
        f"{n - 1} loose piece{'s' if n != 2 else ''}, {loose:,} voxels "
        f"({loose / total:.2%} of the asset), largest {sizes[1]:,}"
        + (f"; {len(airborne)} of them unsupported" if airborne else "")
    )


def _load(path: str) -> tuple[dict, Path]:
    p = Path(path)
    if not p.exists() and not p.is_absolute():
        alt = SPECS / p.name
        if alt.exists():
            p = alt
    s, rep = specmod.load(p)
    for w in rep.warnings:
        print(f"  spec warning: {w}", file=sys.stderr)
    return s, p


def _label(tree: pipeline.Tree) -> str:
    st = tree.stats
    return f"seed {tree.seed}   {st['height_m']:.1f} m   {st['voxels']:,} vox"


# --- commands ---------------------------------------------------------------


def cmd_gen(args) -> int:
    s, path = _load(args.spec)
    tree = pipeline.build(s, args.seed, resolution_cm=getattr(args, "res", None))
    problems = pipeline.health(tree)

    out = Path(args.out) if args.out else OUT / specmod.get(s, "name")
    out.mkdir(parents=True, exist_ok=True)
    stem = f"{specmod.get(s, 'name')}-{args.seed:04d}"

    # Through the kind's own camera. `gen` used to be hardcoded to the
    # isometric, so `gen specs/brown-trout.json` wrote a picture of a fish seen
    # from above -- which is the one angle that shows nothing about a fish.
    img = render.view(tree.grid, render.camera_for(s), target_px=args.px)
    img.save(out / f"{stem}.png")
    models = vox.write(tree.grid, out / f"{stem}.vox", name=stem)
    size = vxa.write(tree.grid, out / f"{stem}.vxa", tree.parts,
                     partslib.joints(tree.parts))
    (out / f"{stem}.json").write_text(
        json.dumps({"spec": s, "stats": tree.stats, "problems": problems}, indent=2),
        encoding="utf-8",
    )

    st = tree.stats
    # Branch statistics on something with no branches, and a wood-connectivity
    # figure of "nan%", are a column of zeroes pretending to mean something.
    # The kind decides which lines print, the same rule the app's detail sheet
    # and `pipeline.health` already follow.
    branchy = st.get("kind") not in pipeline.BRANCHLESS
    swims = st.get("kind") in pipeline.SWIMS
    print(f"{stem}")
    if swims:
        print(f"  {st.get('length_m', 0):.2f} m long, {st['height_m']:.2f} m deep, "
              f"{st['footprint_m'][1]:.2f} m across, {st['extent_vox']} voxels "
              f"at {st['voxel_cm']:g} cm")
    else:
        print(f"  {st['height_m']:.1f} m tall, footprint {st['footprint_m'][0]:.1f} x "
              f"{st['footprint_m'][1]:.1f} m, {st['extent_vox']} voxels "
              f"at {st['voxel_cm']:g} cm ({st['grid_mb']:,.0f} MB grid)")
    print(f"  {st['voxels']:,} solid voxels"
          + (f", {st['nodes']:,} skeleton nodes, max branch order {st['max_order']}"
             if branchy else f", {st['pieces_built']} piece"
             f"{'s' if st['pieces_built'] != 1 else ''}"))
    print(f"  materials: " + ", ".join(
        f"{materials.NAME_BY_ID.get(m, m)}={c:,}" for m, c in sorted(st["by_material"].items())))
    print(("  wood one piece {:.2%}, ".format(st.get("wood_connected", float("nan")))
           if branchy else "  ")
          + f"attached {st.get('attached_frac', float('nan')):.2%}"
          + ("" if swims else f", ground contact {st['ground_contact']} voxels"))
    print(f"  {st['ms_grow']:.0f} ms grow + {st['ms_raster']:.0f} ms raster = "
          f"{st['ms_total']:.0f} ms")
    print(f"  wrote {stem}.png, .vox ({models} model{'s' if models != 1 else ''}), "
          f".vxa ({size:,} bytes), .json")
    for p in problems:
        print(f"  ! {p}")
    return 0


def cmd_batch(args) -> int:
    s, path = _load(args.spec)
    name = specmod.get(s, "name")
    out = Path(args.out) if args.out else OUT / name
    out.mkdir(parents=True, exist_ok=True)

    camera = render.camera_for(s)
    scale = render.scale_for_camera([render.predicted_extent(s)], camera, args.px)
    cells = []
    records = []
    t0 = time.perf_counter()
    for i in range(args.count):
        seed = args.seed + i
        tree = pipeline.build(s, seed)
        problems = pipeline.health(tree)
        img = render.view(tree.grid, camera, scale=scale)
        if args.keep_png:
            img.save(out / f"{name}-{seed:04d}.png")
        if args.keep_vox:
            vox.write(tree.grid, out / f"{name}-{seed:04d}.vox", name=f"{name}-{seed:04d}")
        cells.append((img, _label(tree), problems))
        records.append({"seed": seed, "stats": tree.stats, "problems": problems})
        print(f"\r  {i + 1}/{args.count}  {tree.stats['ms_total']:.0f} ms", end="", flush=True)
    elapsed = time.perf_counter() - t0
    print()

    (out / f"{name}-batch.jsonl").write_text(
        "\n".join(json.dumps(r) for r in records) + "\n", encoding="utf-8"
    )

    bad = sum(1 for r in records if r["problems"])
    heights = [r["stats"]["height_m"] for r in records]
    voxels = [r["stats"]["voxels"] for r in records]
    subtitle = (
        f"spec {specmod.spec_hash(s)}   seeds {args.seed}-{args.seed + args.count - 1}   "
        f"height {min(heights):.1f}-{max(heights):.1f} m   "
        f"{min(voxels):,}-{max(voxels):,} voxels   "
        f"{bad} flagged   {elapsed / args.count * 1e3:.0f} ms/tree"
    )
    img = contact.sheet(cells, title=f"{name}  x{args.count}", subtitle=subtitle,
                        columns=args.columns)
    p = contact.save(img, out / f"{name}-sheet.png")

    print(f"  {elapsed:.1f} s total, {elapsed / args.count * 1e3:.0f} ms/tree")
    print(f"  {bad}/{args.count} flagged by the health check")
    print(f"  sheet: {p}")
    return 0


def cmd_survey(args) -> int:
    """One seed from every spec, on one page. The 'do the species read as
    different trees' check."""
    paths = sorted(SPECS.glob("*.json"))
    if not paths:
        print("no specs found", file=sys.stderr)
        return 1
    loaded = [specmod.load(p)[0] for p in paths]
    # ONE SCALE for the whole survey, so a sapling and an emergent are not the
    # same size on the page -- but the CAMERA is per species, because the survey
    # now spans kinds that need different ones. The scale is picked from the
    # isometric because most of the page is isometric and the three projections
    # size an asset within a few percent of each other.
    scale = render.scale_for([render.predicted_extent(s) for s in loaded], args.px)
    cells = []
    for s in loaded:
        for i in range(args.each):
            tree = pipeline.build(s, args.seed + i)
            img = render.view(tree.grid, render.camera_for(s), scale=scale)
            # An animal is quoted by its LENGTH and everything else by its
            # height. A 24 cm robin labelled "0.15 m" is reporting how deep it
            # is, which is the same mistake the sheet made for fish.
            size = (f"{tree.stats.get('length_m', 0):.2f} m long"
                    if specmod.get(s, "kind") in ("fish", "cetacean", "bird",
                                                  "quadruped")
                    else f"{tree.stats['height_m']:.1f} m")
            label = f"{specmod.get(s, 'name')}  {size}"
            cells.append((img, label, pipeline.health(tree)))
            print(f"\r  {len(cells)} rendered", end="", flush=True)
    print()
    img = contact.sheet(
        cells,
        title=f"species survey  ({len(paths)} specs x {args.each})",
        subtitle="one page per species set; checking that the biomes read as different trees",
        columns=args.each if args.each > 1 else args.columns,
    )
    p = contact.save(img, OUT / "survey.png")
    print(f"  sheet: {p}")
    return 0


def cmd_check(args) -> int:
    s, path = _load(args.spec)
    print(f"{path}: {specmod.get(s, 'name')}  hash {specmod.spec_hash(s)}")
    tree = pipeline.build(s, args.seed)
    problems = pipeline.health(tree)
    print(json.dumps(tree.stats, indent=2, default=str))
    for p in problems:
        print(f"  ! {p}")
    return 1 if problems else 0


def cmd_serve(args) -> int:
    from . import server

    server.serve(host=args.host, port=args.port, open_browser=not args.no_open)
    return 0


def cmd_schema(args) -> int:
    print(json.dumps(specmod.ui_schema(), indent=2))
    return 0


def cmd_materials(args) -> int:
    print(f"{'id':>4}  {'name':<18} {'colour':<16} status")
    for mid in sorted(materials.COLORS):
        name = materials.NAME_BY_ID.get(mid, f"<engine slot {mid}>")
        status = "engine (core.h)" if mid < materials.FIRST_PROPOSED else "PROPOSED, not in engine"
        print(f"{mid:>4}  {name:<18} {str(materials.color(mid)):<16} {status}")
    print()
    proposed = [m for m in materials.COLORS if m >= materials.FIRST_PROPOSED]
    if proposed:
        print(f"{len(proposed)} proposed ID(s) must be reconciled with vxc::Material "
              f"when they are appended.")
        print("See docs/tree-asset-generator-research.md section 8A for what that "
              "append touches.")
    else:
        # Said out loud rather than left as silence. "No warning" and "nothing
        # to warn about" look identical, and this line has been printed
        # unconditionally through two appends that resolved it.
        print(f"Every material in this table exists in vxc::Material "
              f"(kMaterialCount = {materials.ENGINE_MATERIAL_COUNT}).")
    return 0


def _palette_should_be() -> str:
    """Re-run tools/gen_palette.py in memory: what palette.py ought to contain.

    Loaded by path because `tools/` is a directory of scripts, not a package.
    Its scripts start with `import _path`, so their own directory has to be on
    sys.path before this will import.
    """
    import importlib.util

    gen = ROOT / "tools" / "gen_palette.py"
    if str(gen.parent) not in sys.path:
        sys.path.insert(0, str(gen.parent))
    ms = importlib.util.spec_from_file_location("gen_palette", gen)
    mod = importlib.util.module_from_spec(ms)
    ms.loader.exec_module(mod)
    return mod.source()


def _palette_drift() -> str:
    """Empty if palette.py is what the header says; otherwise why not."""
    try:
        want = _palette_should_be()
    except SystemExit as e:  # bad header, or no header
        return str(e)
    # read_text translates the file's CRLF back to \n, so this compares content
    # and not line endings -- the generator writes through the same translation.
    have = (ROOT / "forge" / "palette.py").read_text(encoding="utf-8")
    if want == have:
        return ""
    w, h = want.splitlines(), have.splitlines()
    for i in range(max(len(w), len(h))):
        a = w[i] if i < len(w) else "<end of file>"
        b = h[i] if i < len(h) else "<end of file>"
        if a != b:
            return (f"line {i + 1} differs\n"
                    f"      header says: {a.strip()}\n"
                    f"      palette.py:  {b.strip()}")
    return "lengths differ"


def cmd_selftest(args) -> int:
    """Checks the three properties the rest of the tool assumes."""
    ok = True
    s = specmod.default_spec()

    # forge/palette.py is GENERATED from the engine's materialpalette.h, and
    # nothing makes anyone regenerate it. Edit the header, forget the command,
    # and the forge keeps showing the old colours while the game shows the new
    # ones -- with no error anywhere, because a wrong colour still looks like a
    # colour. That is the same failure mode as the material-ID drift this
    # generation step was introduced to end.
    #
    # The generator also checks the header's own order against the enum, and
    # that check comes along for free here: the table is POSITIONAL, entry N is
    # material N, and the C++ static_assert counts entries without looking at
    # their order. When it was first written grouped by type, MAT_BARK_PALE
    # (id 23) sat with the other woods at index 19 and shifted every id above
    # it, which dressed every broadleaf in the library in birch bark.
    drift = _palette_drift()
    print(f"  palette matches the engine header: {'pass' if not drift else 'FAIL'}"
          + (f"\n    {drift}\n    run python tools/gen_palette.py" if drift else ""))
    ok &= not drift

    a = pipeline.build(s, 3)
    b = pipeline.build(s, 3)
    same = (a.grid.data == b.grid.data).all() and (a.grid.origin == b.grid.origin).all()
    print(f"  determinism (same spec+seed -> same voxels): {'pass' if same else 'FAIL'}")
    ok &= bool(same)

    c = pipeline.build(s, 4)
    differs = c.grid.data.shape != a.grid.data.shape or not (
        c.grid.data.shape == a.grid.data.shape and (c.grid.data == a.grid.data).all()
    )
    print(f"  seeds actually differ: {'pass' if differs else 'FAIL'}")
    ok &= bool(differs)

    blob = vxa.encode(a.grid)
    back, _, _ = vxa.decode(blob)
    round_ok = (back.data == a.grid.data).all() and (back.origin == a.grid.origin).all()
    ratio = len(blob) / max(a.grid.data.size, 1)
    print(f"  vxa round trip: {'pass' if round_ok else 'FAIL'} "
          f"({len(blob):,} bytes, {ratio:.1%} of dense)")
    ok &= bool(round_ok)

    wood = a.stats["wood_connected"]
    print(f"  wood is a single face-connected piece: "
          f"{'pass' if wood >= 1.0 else 'FAIL'} ({wood:.3%})")
    ok &= wood >= 1.0

    # .vox is written for a designer's Blender/MagicaVoxel round trip, so a
    # malformed chunk would only surface when they try to open a file. Walk it
    # back and check the voxels survived and no model broke the 256 limit.
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        big, _ = specmod.load(SPECS / "jungle-emergent.json") if (
            SPECS / "jungle-emergent.json"
        ).exists() else (s, None)
        tall = pipeline.build(big, 2)
        p = Path(td) / "t.vox"
        n_models = vox.write(tall.grid, p, name="t")
        info = vox.inspect(p)
        vox_ok = (
            info["voxels"] == tall.stats["voxels"]
            and not info["oversized"]
            and info["chunks"].get("nSHP", 0) == n_models
            and info["palette_entries"] == 256
        )
        print(f"  vox export ({max(tall.grid.shape)} voxels tall -> {n_models} model"
              f"{'s' if n_models != 1 else ''}): {'pass' if vox_ok else 'FAIL'} "
              f"({info['voxels']:,} voxels read back, largest model "
              f"{max(max(m) for m in info['models'])})")
        ok &= bool(vox_ok)

    attached = a.stats["attached_frac"]
    print(f"  no free-floating voxels: "
          f"{'pass' if attached >= 0.98 else 'FAIL'} ({attached:.3%} attached)")
    ok &= attached >= 0.98

    # THE SINGLE-ASSET RULE, over the whole library. See KNOWN_MULTIPIECE.
    #
    # Nothing enforced this before. `stats["airborne_kept"]` and
    # `stats["orphans_removed"]` were both measured per build and read by
    # nobody, which is why all three floating-rock defects in this library were
    # found by a person squinting at a render.
    #
    # One seed per species, not three. This is one of the two commands the
    # README says to run before calling a change done, so it has to stay
    # something people will actually run; three seeds and per-kind filtering
    # are `python tools/buildcheck.py --seeds 1 2 3`, which is what CI runs and
    # what a Phase 1 fix gets signed off with.
    if getattr(args, "quick", False):
        print("  every build is ONE piece: SKIPPED (--quick)")
        print("  every asset's materials exist in the engine: SKIPPED (--quick)")
    else:
        bad, known, worst = [], [], 0.0
        # HEAVY_SPECS are left out and named in the output, not silently
        # dropped: hero-arch-colossal alone is a multi-gigabyte working set at
        # its authored 10 cm and half an hour of wall clock, so a selftest that
        # built it would be a selftest nobody could run.
        paths = [p for p in spec_paths() if p.stem not in HEAVY_SPECS]
        broke = {}
        # THE OTHER THING WORTH READING OFF THESE BUILDS, and it costs nothing
        # because they are being built anyway.
        #
        # `assetgrid.h:178` gates every asset the engine loads on
        # `maxMaterialId() < kMaterialCount`, and until a material append lands
        # that gate REFUSES every asset authored against the new ids -- which is
        # correct, and which is the position the trees, the fish and then the
        # birds were each in for a while. Nothing on this side could see it: the
        # forge draws a proposed material in its preview colour and the asset
        # looks finished.
        #
        # So the same question is asked here, in Python, against the material
        # count the GENERATED palette carries. That number comes from the engine
        # header, so this cannot pass by agreeing with itself: if an append is
        # forgotten, half-done, or done without regenerating, the species that
        # need it name themselves.
        over = []
        limit = materials.ENGINE_MATERIAL_COUNT
        for p in paths:
            ls, _ = specmod.load(p)
            asset = pipeline.build(ls, 1)
            hist = asset.stats.get("by_material") or {}
            top = max((int(m) for m in hist if m), default=0)
            if top >= limit:
                over.append(f"{p.stem}: uses material {top} "
                            f"({materials.NAME_BY_ID.get(top, '?')}), "
                            f"engine has {limit}")
            extra, frac, why = loose_summary(asset)
            broke[p.stem] = bool(extra)
            if not extra:
                continue
            if p.stem in KNOWN_MULTIPIECE:
                known.append(f"{p.stem}: {why}  [known: {KNOWN_MULTIPIECE[p.stem]}]")
            else:
                bad.append(f"{p.stem}: {why}")
                worst = max(worst, frac)
        for name in stale_allowances(broke):
            bad.append(f"{name}: allow-listed in KNOWN_MULTIPIECE but it now builds as "
                       f"one piece -- delete the entry ({KNOWN_MULTIPIECE[name]})")
        print(f"  every build is ONE piece ({len(paths)} specs, seed 1): "
              f"{'pass' if not bad else 'FAIL'} "
              f"({len(bad)} shipped more than one piece, worst {worst:.2%} of an asset loose"
              + (f"; {len(known)} known-failing allowed" if known else "") + ")")
        print(f"    not covered here: {', '.join(sorted(HEAVY_SPECS))} -- too big to "
              f"build in a pre-commit check; run tools/buildcheck.py --only-heavy")
        for line in bad:
            print(f"    ! {line}")
        for line in known:
            print(f"    - {line}")
        ok &= not bad

        print(f"  every asset's materials exist in the engine "
              f"(kMaterialCount = {limit}): {'pass' if not over else 'FAIL'} "
              f"({len(over)} of {len(paths)} would be refused by "
              f"AssetGrid::materialsWithinEngine)")
        for line in over:
            print(f"    ! {line}")
        ok &= not over

    # A TERRAIN-LATTICE ASSET IS AUTHORED AT 10 CM, AND NOTHING ELSE IS LEGAL.
    #
    # Rocks and trees join the world's own voxel grid and are destructible as
    # terrain is, so they have to be on the grid's cell size --
    # `vxc::kVoxelSizeMm` = 100 mm. `AssetGrid::at` takes plain integer voxel
    # coordinates with no scale factor and nothing in voxel-core resamples, so
    # a 5 cm rock read through it comes out at twice its intended size. There
    # is no diagnostic for that: it is a boulder that is simply wrong, in a
    # world full of boulders.
    #
    # Cheap and unconditional, so it runs under `--quick` too. The whole
    # library was on the wrong lattice as recently as this morning
    # (`tools/all_to_5cm.py`), which is exactly how a rule with no check ends.
    off = []
    for p in spec_paths():
        try:
            s, _ = specmod.load(p)
        except (OSError, ValueError) as e:
            off.append(f"{p.stem}: unreadable ({e})")
            continue
        k = kinds.BY_KEY.get(specmod.get(s, "kind"))
        if k is None or k.lattice != "terrain":
            continue
        cm = float(specmod.get(s, "resolution_cm"))
        if cm != kinds.TERRAIN_LATTICE_CM:
            off.append(f"{p.stem} ({k.key}) is authored at {cm:g} cm; a "
                       f"terrain-lattice kind must be at "
                       f"{kinds.TERRAIN_LATTICE_CM:g} cm")
    print(f"  terrain-lattice assets are on the terrain lattice: "
          f"{'pass' if not off else 'FAIL'}")
    for line in off:
        print(f"    ! {line}")
    ok &= not off

    # A SPECIES MAY ONLY BE WEIGHTED INTO A BIOME THAT HOSTS ITS KIND.
    #
    # `spec.py` gates the app's sliders with `kinds=b.hosts`, which is a UI
    # rule and not a validation one: `specmod.patch` will happily set
    # `biomes.bare_rock` on a flower and `validate` returns no warning, so a
    # spec can claim to live somewhere nothing of its kind can be placed. Two
    # were doing exactly that when this was written -- `herb-robert` and
    # `moss-cushion` on bare rock, which hosts no plant kind -- and neither had
    # ever produced a diagnostic.
    #
    # This is cheap, so it runs under `--quick` too. It is also the check that
    # will fire the next time somebody opens a `hosts` tuple and forgets that
    # weights were authored against the old one.
    astray = []
    for p in spec_paths():
        try:
            s, _ = specmod.load(p)
        except (OSError, ValueError):
            continue        # the lattice check above already reports these
        kind = specmod.get(s, "kind")
        for bk, w in (s.get("biomes") or {}).items():
            b = biomelib.BY_KEY.get(bk)
            if b is None:
                astray.append(f"{p.stem}: no such biome {bk!r}")
            elif float(w) > 0.0 and kind not in b.hosts:
                astray.append(f"{p.stem} ({kind}) is weighted {float(w):g} into "
                              f"{bk}, which hosts {', '.join(b.hosts) or 'nothing'}")
    print(f"  every species lives somewhere that hosts it: "
          f"{'pass' if not astray else 'FAIL'}")
    for line in astray:
        print(f"    ! {line}")
    ok &= not astray

    verdict = "PASS" if ok else "FAIL"
    if getattr(args, "quick", False):
        verdict += "  (quick: the single-asset check over the library was NOT run)"
    print("selftest:", verdict)
    return 0 if ok else 1


# --- wiring -----------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="forge", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen", help="generate one tree")
    g.add_argument("spec")
    g.add_argument("--seed", type=int, default=1)
    g.add_argument("--out")
    g.add_argument("--px", type=int, default=640)
    g.add_argument("--res", help="voxel size in cm; overrides the spec (e.g. --res 2)")
    g.set_defaults(fn=cmd_gen)

    b = sub.add_parser("batch", help="generate many seeds and a contact sheet")
    b.add_argument("spec")
    b.add_argument("--count", type=int, default=24)
    b.add_argument("--seed", type=int, default=1)
    b.add_argument("--out")
    b.add_argument("--px", type=int, default=340)
    b.add_argument("--columns", type=int, default=6)
    b.add_argument("--keep-png", action="store_true")
    b.add_argument("--keep-vox", action="store_true")
    b.set_defaults(fn=cmd_batch)

    s = sub.add_parser("survey", help="one page across every spec")
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--each", type=int, default=3)
    s.add_argument("--px", type=int, default=420)
    s.add_argument("--columns", type=int, default=6)
    s.set_defaults(fn=cmd_survey)

    c = sub.add_parser("check", help="validate a spec and dump its stats")
    c.add_argument("spec")
    c.add_argument("--seed", type=int, default=1)
    c.set_defaults(fn=cmd_check)

    w = sub.add_parser("serve", help="open the app in a browser")
    w.add_argument("--port", type=int, default=8731)
    w.add_argument("--host", default="127.0.0.1")
    w.add_argument("--no-open", action="store_true")
    w.set_defaults(fn=cmd_serve)

    sub.add_parser("schema", help="slider table as JSON").set_defaults(fn=cmd_schema)
    sub.add_parser("materials", help="material table and engine status").set_defaults(
        fn=cmd_materials)
    t = sub.add_parser("selftest", help="determinism, round trips, one-piece assets")
    # The library pass is most of the wall clock (hero-arch-colossal alone is
    # 38M voxels and minutes). --quick exists so the slow half is skippable
    # without anyone deleting it for being slow -- and it prints SKIPPED on the
    # check's own line AND on the verdict line, so a quick run can never be
    # mistaken for a clean one in a log.
    t.add_argument("--quick", action="store_true",
                   help="skip the whole-library single-asset pass and say so")
    t.set_defaults(fn=cmd_selftest)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
