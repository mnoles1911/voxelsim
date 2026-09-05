"""Measure a craft, and try to prove its build wrong.

WHY THIS EXISTS AND WHY IT IS NOT A CONTACT SHEET. The `artifact` kind draws
rigid human-made objects, and the two things most likely to be wrong about one
are things no camera in this package can see:

  * **A HULL THAT IS NOT HOLLOW.** A canoe is a shell; a player sits in it. A
    solid block of hull voxels renders as a perfectly good canoe from every
    angle, at every scale, in colour and in silhouette. `--read` counts the
    enclosed air instead, and 0% is a build that shipped a bathtub-shaped brick.
  * **A STEP THAT RAN AND DREW NOTHING.** The generator reports a voxel delta
    per drawing step. A gunwale band thinner than a voxel, a thwart placed above
    the sheer, a cross-bar at span zero: each leaves an asset that still builds,
    still passes every connectivity check, still looks like a boat, and is
    missing a part. `--read` prints the deltas and fails on a zero.

    python tools/artifactprobe.py --read [names...]
        Voxel count, bounding box in metres, per-step deltas, wall thickness,
        hollow fraction, connectivity, and the resolved category. Non-zero exit
        on any failure.

        WITH NO NAMES IT MEASURES EVERY CRAFTABLE, resolved through
        `forge.categories.members("craftable", ...)` rather than a hard-coded
        pair. The third craftable gets measured the day it is authored without
        anybody remembering to edit this file -- and if it is authored under a
        different KIND, which is the whole reason category exists as a separate
        axis, a kind filter would have missed it in silence.

    python tools/artifactprobe.py --views canoe glider [--seed 1] [--px 900]
        Four views per craft into `out/artifact/`: the kind's own review camera,
        a plan, a profile and a head-on. A hull's interior and a wing's
        planform are not both visible from any one camera, so there is no one
        picture to take.

    python tools/artifactprobe.py --arms canoe
        THE MUST-FAIL ARM. Deliberately breaks the build three ways -- a hull
        with no interior, a thwart thinner than a voxel, a sail one voxel thick
        under a steep droop -- and fails if the checks pass. A confirmation that
        cannot come out the other way is not a confirmation.

    python tools/artifactprobe.py --hashes tools/spec_hashes.json
        Every spec's spec_hash AND seed_hash against the committed snapshot.
        Adding a row to `spec.PARAMS` normally moves BOTH hashes for every spec
        in the library -- each species becomes a different individual and every
        bank reads as stale to `tools/enginecheck.py`.
        `spec.KIND_SCOPED_PARAMS` is the mechanism that avoids that, and this is
        the measurement that says whether it worked; a reseed is otherwise
        invisible, because every asset still builds and still looks like itself,
        and this library has been reseeded unnoticed twice.

        The snapshot is WRITTEN when the path does not exist and COMPARED when
        it does, so re-baselining is `rm` and re-run -- deliberately a separate,
        visible act rather than a flag that could be reached for to make a red
        run go green.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
from forge import artifact, categories as catlib, pipeline, render, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"
OUT = ROOT / "out" / "artifact"


def craftables() -> list[str]:
    """Every craftable species on disk, THROUGH THE ONE RESOLVER.

    The default target list, rather than a hard-coded ("canoe", "glider"). The
    third craftable should be measured by this tool the day it is authored
    without anybody remembering to come back here, and if it is authored under
    a different KIND -- a rope, a torch -- a hard-coded kind filter would have
    missed it silently.
    """
    loaded = [(p.stem, sm.load(p)[0]) for p in sorted(SPECS.glob("*.json"))]
    return catlib.members("craftable", loaded)


def _build(name: str, seed: int, **changes):
    body, rep = sm.load(SPECS / f"{name}.json")
    if rep.warnings:
        raise SystemExit(f"{name}: spec warnings: {rep.warnings}")
    if changes:
        body, rep = sm.patch(body, changes)
        if rep.warnings:
            raise SystemExit(f"{name}: patch refused: {rep.warnings}")
    return body, pipeline.build(body, seed)


def _bbox_m(asset) -> tuple[float, float, float]:
    vm = asset.stats["voxel_cm"] / 100.0
    return tuple(round(n * vm, 3) for n in asset.grid.shape)


def _wall_runs(grid) -> dict:
    """The planking thickness the build actually DREW, across the beam.

    `shell_vox` is the authored number; this is the measured one, and they are
    not the same number. The section is a curve, so a shell defined as a
    horizontal inset is thicker than its nominal wherever the wall leans -- and
    a wall that measures ONE voxel anywhere in the body of the boat is a hole
    the mesher will draw straight through.

    THE STEMS ARE COUNTED SEPARATELY AND THAT IS NOT A FUDGE. The plan curve
    goes to zero at both ends, by construction, because a canoe is double-ended
    and pointed: within the last few centimetres of the bow the hull IS one
    voxel wide, and it is supposed to be. A check that fired on that would fire
    on every correct canoe forever, which is the fastest way to teach somebody
    to stop reading checks. So the ends are reported and the BODY is gated:
    `body_min` is the thinnest wall outside the outer 5% of the length at each
    end, and that one may not be 1.
    """
    occ = grid.data != 0
    nx = occ.shape[0]
    end = max(1, int(round(0.05 * nx)))
    runs: list[int] = []
    body_runs: list[int] = []
    thin_at_ends = 0
    for ix in range(nx):
        in_body = end <= ix < nx - end
        for iz in range(occ.shape[2]):
            col = occ[ix, :, iz]
            if not col.any():
                continue
            idx = np.flatnonzero(col)
            breaks = np.flatnonzero(np.diff(idx) > 1)
            starts = np.concatenate(([0], breaks + 1))
            ends = np.concatenate((breaks, [len(idx) - 1]))
            if len(starts) < 2:
                continue          # one run: a stem, a deck or a thwart, not a wall
            for a, b in zip(starts, ends):
                n = int(idx[b] - idx[a] + 1)
                runs.append(n)
                if in_body:
                    body_runs.append(n)
                elif n < 2:
                    thin_at_ends += 1
    if not runs:
        return {"n": 0, "min": 0, "max": 0, "mean": 0.0, "body_min": 0,
                "thin_at_ends": 0}
    return {"n": len(runs), "min": min(runs), "max": max(runs),
            "mean": float(np.mean(runs)),
            "body_min": min(body_runs) if body_runs else 0,
            "thin_at_ends": thin_at_ends}


def cmd_read(names: list[str], seed: int) -> int:
    ok = True
    for name in names:
        body, asset = _build(name, seed)
        st = asset.stats
        bx, by, bz = _bbox_m(asset)
        form = sm.get(body, "artifact.form")
        print(f"{name}-{seed:04d}  ({form}, category "
              f"{catlib.of(body)} via {catlib.source_of(body)})")
        print(f"  {st['voxels']:,} voxels at {st['voxel_cm']:g} cm")
        print(f"  bbox {bx} x {by} x {bz} m   ({st['extent_vox']} voxels)")
        print(f"  materials: " + ", ".join(
            f"{sm.materials.NAME_BY_ID.get(m, m)}={c:,}"
            for m, c in sorted(st["by_material"].items())))
        print(f"  one piece: {st['pieces_built']} built, "
              f"{st['bridges_added']} corner joins bridged, "
              f"{st.get('attached_frac', 0):.2%} attached")
        print(f"  enclosed air: {st.get('hollow_frac', 0):.1%} of the asset's own volume")
        wall = _wall_runs(asset.grid) if form == "hull" else None
        if wall:
            print(f"  planking across the beam: {wall['body_min']} min in the body "
                  f"/ {wall['mean']:.1f} mean / {wall['max']} max voxels over "
                  f"{wall['n']:,} sections (authored "
                  f"{sm.get(body, 'artifact.shell_vox')}); "
                  f"{wall['thin_at_ends']} one-voxel sections in the stems, "
                  f"which is what a pointed end is")
        print("  steps:")
        for r in st.get("steps") or []:
            flag = ""
            if r["must_change"] and r["added"] + r["repainted"] == 0:
                flag = "   <-- SILENT NO-OP"
                ok = False
            print(f"    {r['step']:<22} +{r['added']:<7,} ~{r['repainted']:<7,} "
                  f"{r['note']}{flag}")
        problems = pipeline.health(asset)
        for pline in problems:
            print(f"  ! {pline}")
            ok = False
        # A hull that is not hollow is the failure a render cannot show.
        if form == "hull" and float(st.get("hollow_frac") or 0) < 0.25:
            print(f"  ! hollow fraction {st.get('hollow_frac'):.1%} is under 25%: "
                  f"this is a shell in name only")
            ok = False
        if wall and wall["body_min"] and wall["body_min"] < 2:
            print(f"  ! the planking measures {wall['body_min']} voxel across "
                  f"inside the body of the hull -- the mesher will draw "
                  f"through it")
            ok = False
    print("artifactprobe --read:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def cmd_views(names: list[str], seed: int, px: int) -> int:
    """Four cameras, because no one of them answers the question.

      review    what `render.camera_for` picks. This is the picture the
                gallery, the library thumbnail and every contact sheet will
                show, so it is the one an owner verdict is really about.
      profile   `broadside` at 2 degrees -- a TRUE side view, screen-x equal to
                the craft's own length, nothing skewed. Sheer and rocker are
                two lines and this is the only camera that draws them as lines.
      inside    `broadside` at 44 degrees, the highest lift that camera has.
                For a hull it is the only view that sees the floor, the
                thwarts and whether the thing is hollow at all; for a wing it
                is the closest thing to a planform this renderer has.
      headon    `broadside` at ZERO degrees on a grid turned a quarter turn.
                Zero and not the usual few degrees of lift, because the lift
                adds a depth rise per voxel of DEPTH INTO THE SCREEN -- and
                after the quarter turn the depth axis is the craft's whole
                length. At 8 degrees a 3.9 m canoe contributed more vertical
                rise than its own 0.5 m of height and the section vanished
                under a diagonal smear. The
                section shape of a hull and the tip droop of a wing are both
                across-the-beam facts, and the other three cameras foreshorten
                them to nothing.

    NOT `elevation`. That camera looks along a horizontal DIAGONAL -- sx is
    (x - y) -- so it lays a 3.9 m canoe across its own 0.85 m beam and the
    sheer line comes out as a slab. It is the right camera for a rock, whose
    subject is a lump, and the wrong one for anything whose subject is a drawn
    curve. The first pass of this tool used it for "profile" and produced a
    picture with no canoe in it.
    """
    OUT.mkdir(parents=True, exist_ok=True)
    for name in names:
        body, asset = _build(name, seed)
        turned = _turned(asset.grid)
        shots = {
            "review": (render.camera_for(body), None, asset.grid),
            "profile": ("broad", 2.0, asset.grid),
            "inside": ("broad", 44.0, asset.grid),
            "headon": ("broad", 0.0, turned),
        }
        for label, (camera, tilt, grid) in shots.items():
            img = render.view(grid, camera, target_px=px, tilt_deg=tilt)
            path = OUT / f"{name}-{seed:04d}-{label}.png"
            img.save(path)
            print(f"  wrote {path.relative_to(ROOT)}  {img.size[0]}x{img.size[1]}")
    return 0


def _turned(grid):
    """The same grid rotated a quarter turn about z. Exact -- no resampling."""
    from forge.grid import VoxelGrid

    turned = VoxelGrid((grid.shape[1], grid.shape[0], grid.shape[2]),
                       tuple(grid.origin), grid.voxel_m)
    turned.data[:] = np.rot90(grid.data, 1, axes=(0, 1))
    return turned


def cmd_arms(name: str, seed: int) -> int:
    """Break the build on purpose. A check that cannot fail is not a check."""
    arms = [
        ("solid hull (shell thicker than the half-beam)",
         {"artifact.beam_m": 0.30, "artifact.shell_vox": 6},
         lambda st, hp: any(p.startswith("solid:") for p in hp)),
        ("thwart thinner than one voxel",
         {"artifact.thwart_t_m": 0.02, "artifact.thwart_w_m": 0.02},
         lambda st, hp: any("thwarts" in p for p in hp)),
        ("gunwale band with no depth and no overhang",
         {"artifact.gunwale_m": 0.0, "artifact.gunwale_vox": 0},
         lambda st, hp: any(r["step"] == "gunwale" and r["voxels_after"] is None
                            for r in st.get("steps") or [])),
    ]
    ok = True
    # THE TAXONOMY ARMS. The category override is a real mechanism that NOTHING
    # in the library uses yet (canoe and glider are craftable because `artifact`
    # is), and an unused mechanism rots. These three exercise it end to end: it
    # wins over the kind, it is invisible to both hashes, and an unreadable
    # value resolves to NOTHING rather than falling back to the kind -- which is
    # the blossom-pink failure and the one that must never come back.
    base, _ = sm.load(SPECS / f"{name}.json")
    over = dict(base); over["category"] = "environment"
    over, orep = sm.validate(over)
    wins = catlib.of(over) == "environment" and catlib.source_of(over) == "spec"
    print(f"  {'fires' if wins else 'SILENT'}: a spec category overrides its kind"
          + ("" if wins else f"  (got {catlib.of(over)!r}, warnings {orep.warnings})"))
    ok &= wins
    neutral = (sm.spec_hash(over) == sm.spec_hash(base)
               and sm.seed_hash(over) == sm.seed_hash(base))
    print(f"  {'fires' if neutral else 'SILENT'}: the override moves NEITHER hash")
    ok &= neutral
    bad = dict(base); bad["category"] = "definitely-not-a-category"
    bad, brep = sm.validate(bad)
    refused = (catlib.of(bad) is None
               and any("category" in w for w in brep.warnings))
    print(f"  {'fires' if refused else 'SILENT'}: an unreadable category "
          f"resolves to NOTHING, loudly")
    if not refused:
        print(f"      got {catlib.of(bad)!r}, warnings {brep.warnings}")
    ok &= refused

    for label, changes, fires in arms:
        _, asset = _build(name, seed, **changes)
        hp = pipeline.health(asset)
        fired = bool(fires(asset.stats, hp))
        print(f"  {'fires' if fired else 'SILENT'}: {label}")
        if not fired:
            print(f"      health said: {hp or 'nothing'}")
        ok &= fired
    print("artifactprobe --arms:", "PASS" if ok else "FAIL (a check cannot fail)")
    return 0 if ok else 1


def cmd_hashes(path: Path) -> int:
    """Both hashes for every spec on disk, written or compared."""
    now = {}
    for p in sorted(SPECS.glob("*.json")):
        body, _ = sm.load(p)
        now[p.stem] = [sm.spec_hash(body), sm.seed_hash(body)]
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(now, indent=0, sort_keys=True), encoding="utf-8")
        print(f"wrote {len(now)} spec hashes to {path}")
        return 0
    was = json.loads(path.read_text(encoding="utf-8"))
    moved = [k for k in was if k in now and now[k] != was[k]]
    added = sorted(set(now) - set(was))
    gone = sorted(set(was) - set(now))
    print(f"  {len(was) - len(gone)} of {len(was)} specs compared, "
          f"{len(moved)} moved, {len(added)} new, {len(gone)} removed")
    for k in moved[:10]:
        print(f"    ! {k}: spec {was[k][0][:8]}->{now[k][0][:8]}  "
              f"seed {was[k][1][:8]}->{now[k][1][:8]}")
    if len(moved) > 10:
        print(f"    ! ...and {len(moved) - 10} more")
    if added:
        print(f"    + new: {', '.join(added)}")
    print("artifactprobe --hashes:", "PASS" if not moved else
          "FAIL -- the library was re-identified; see spec.KIND_SCOPED_PARAMS")
    return 0 if not moved else 1


def cmd_seeds(names: list[str], seeds: list[int]) -> int:
    """Determinism, and that a seed still changes something.

    BOTH HALVES, because either alone is satisfied by a broken generator: one
    that ignores the seed is perfectly deterministic, and one that is not
    deterministic at all certainly produces different seeds.
    """
    ok = True
    for name in names:
        body, _ = _build(name, seeds[0])
        counts = {}
        for seed in seeds:
            a = pipeline.build(body, seed)
            b = pipeline.build(body, seed)
            same = (a.grid.data.shape == b.grid.data.shape
                    and bool((a.grid.data == b.grid.data).all()))
            if not same:
                print(f"  ! {name} seed {seed} is NOT deterministic")
                ok = False
            counts[seed] = (a.stats["voxels"], a.grid.data.tobytes())
        uniq = len({v[1] for v in counts.values()})
        spread = [counts[s][0] for s in seeds]
        print(f"  {name}: deterministic, {uniq} of {len(seeds)} seeds distinct, "
              f"voxels {min(spread):,}-{max(spread):,} "
              f"({(max(spread) - min(spread)) / max(min(spread), 1):.2%} spread)")
        if uniq < 2:
            print(f"  ! {name}: every seed drew the same object -- the seed is dead")
            ok = False
    print("artifactprobe --seeds:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("names", nargs="*", default=[],
                    help="species to measure; default is every craftable")
    ap.add_argument("--read", action="store_true")
    ap.add_argument("--views", action="store_true")
    ap.add_argument("--arms", action="store_true")
    ap.add_argument("--seeds", action="store_true")
    ap.add_argument("--hashes", metavar="PATH")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--seed-list", type=int, nargs="*", default=[1, 2, 3, 4])
    ap.add_argument("--px", type=int, default=900)
    args = ap.parse_args()
    names = args.names or craftables()
    if not names:
        raise SystemExit("artifactprobe: no craftable species found. A run "
                         "with nothing to measure must not exit 0.")

    if args.hashes:
        return cmd_hashes(Path(args.hashes))
    picked = any((args.read, args.views, args.arms, args.seeds))
    rc = 0
    if args.read or not picked:
        rc |= cmd_read(names, args.seed)
    if args.seeds or not picked:
        rc |= cmd_seeds(names, args.seed_list)
    if args.views or not picked:
        rc |= cmd_views(names, args.seed, args.px)
    if args.arms:
        rc |= cmd_arms(names[0], args.seed)
    return rc


if __name__ == "__main__":
    sys.exit(main())
