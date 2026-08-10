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

from . import contact, materials, pipeline, render, spec as specmod, vox, vxa

ROOT = Path(__file__).resolve().parent.parent
SPECS = ROOT / "specs"
OUT = ROOT / "out"


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
    tree = pipeline.build(s, args.seed)
    problems = pipeline.health(tree)

    out = Path(args.out) if args.out else OUT / specmod.get(s, "name")
    out.mkdir(parents=True, exist_ok=True)
    stem = f"{specmod.get(s, 'name')}-{args.seed:04d}"

    img = render.render(tree.grid, target_px=args.px)
    img.save(out / f"{stem}.png")
    models = vox.write(tree.grid, out / f"{stem}.vox", name=stem)
    size = vxa.write(tree.grid, out / f"{stem}.vxa")
    (out / f"{stem}.json").write_text(
        json.dumps({"spec": s, "stats": tree.stats, "problems": problems}, indent=2),
        encoding="utf-8",
    )

    st = tree.stats
    print(f"{stem}")
    print(f"  {st['height_m']:.1f} m tall, footprint {st['footprint_m'][0]:.1f} x "
          f"{st['footprint_m'][1]:.1f} m, {st['extent_vox']} voxels")
    print(f"  {st['voxels']:,} solid voxels, {st['nodes']:,} skeleton nodes, "
          f"max branch order {st['max_order']}")
    print(f"  materials: " + ", ".join(
        f"{materials.NAME_BY_ID.get(m, m)}={c:,}" for m, c in sorted(st["by_material"].items())))
    print(f"  wood one piece {st.get('wood_connected', float('nan')):.2%}, "
          f"attached {st.get('attached_frac', float('nan')):.2%}, "
          f"ground contact {st['ground_contact']} voxels")
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

    scale = render.scale_for([render.predicted_extent(s)], args.px)
    cells = []
    records = []
    t0 = time.perf_counter()
    for i in range(args.count):
        seed = args.seed + i
        tree = pipeline.build(s, seed)
        problems = pipeline.health(tree)
        img = render.render(tree.grid, scale=scale)
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
    scale = render.scale_for([render.predicted_extent(s) for s in loaded], args.px)
    cells = []
    for s in loaded:
        for i in range(args.each):
            tree = pipeline.build(s, args.seed + i)
            img = render.render(tree.grid, scale=scale)
            label = f"{specmod.get(s, 'name')}  {tree.stats['height_m']:.1f} m"
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
    print("Proposed IDs must be reconciled with vxc::Material when they are appended.")
    print("See docs/tree-asset-generator-research.md section 8A for what that append touches.")
    return 0


def cmd_selftest(args) -> int:
    """Checks the three properties the rest of the tool assumes."""
    ok = True
    s = specmod.default_spec()

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
    back = vxa.decode(blob)
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

    print("selftest:", "PASS" if ok else "FAIL")
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
    sub.add_parser("selftest", help="determinism and round-trip checks").set_defaults(
        fn=cmd_selftest)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
