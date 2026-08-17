"""spec + seed -> voxels. The one entry point everything else calls.

`build()` is deterministic: the same spec and the same seed produce byte-for-byte
the same grid, on any machine, today or in six months. That is what lets a tree
be stored as a few hundred bytes instead of a voxel blob, and it is what makes
pre-baked seed banks safe -- the world can hold `(species, seed)` and trust that
what it gets back is what the designer approved.

The seed is mixed with a hash of the spec, so editing one slider reshuffles the
whole family rather than leaving seed 7 looking suspiciously like it did before
the edit.
"""

from __future__ import annotations

import os
import time
from dataclasses import dataclass, field
from typing import Any

import numpy as np

from . import (bird as birdlib, envelope, fish as fishlib, ground as groundlib,
               materials, quadruped as quadlib, rasterize, rock as rocklib)
from . import parts as partslib
from .grid import VoxelGrid, dense_bytes, ground_band
from .skeleton import Skeleton, add_roots, add_strands, grow, grow_frond, grow_whorl
from .spec import get, realize as spec_realize, seed_hash, spec_hash

# Which generator a kind goes to. Everything not listed here grows a skeleton.
BOULDER_KINDS = frozenset({"rock"})
TUFT_KINDS = frozenset({"grass", "reed", "flower"})
FISH_KINDS = frozenset({"fish", "cetacean"})
BIRD_KINDS = frozenset({"bird"})
QUAD_KINDS = frozenset({"quadruped"})
# Kinds with no branch structure, so the branch-shaped stats and the checks
# that read them do not apply.
BRANCHLESS = BOULDER_KINDS | TUFT_KINDS | FISH_KINDS | BIRD_KINDS | QUAD_KINDS
# Kinds that do not stand on the ground. A tree with nothing on its bottom slab
# is floating and broken; a fish is SUPPOSED to be in mid-water and the check
# would be a permanent false alarm on every one of them -- which is the fastest
# way to teach a designer to stop reading health flags. The `crop` in `build`
# means a fish always has voxels in its own bottom slab anyway, so this is about
# saying what is meant rather than about fixing a failure.
SWIMS = FISH_KINDS
# And the same again for a bird, which is either in the air or standing on a
# branch. What a perched bird's feet rest on is whatever the spawner sat it on,
# not the ground plane, so the check would fire on every one of the twenty
# species and mean nothing on any of them.
FLIES = BIRD_KINDS
UNGROUNDED = SWIMS | FLIES
# A LAND ANIMAL IS DELIBERATELY NOT IN `UNGROUNDED`. It is the first asset here
# after a tree that genuinely stands on the ground plane, so the floating check
# applies to it and should.
#
# Be clear about what that check is worth, though, because it is worth less than
# it looks: `build` CROPS the grid before the stat is taken, so the bottom slab
# of the finished asset is occupied whatever the feet did, and a quadruped whose
# legs stop short would still pass. What it catches is a grid with nothing in it
# at all. The real measurement -- how far each of the four feet is above the
# lowest voxel -- is `tools/quadprobe.py --stance`, and it exists because this
# one cannot answer the question.


@dataclass
class Asset:
    grid: VoxelGrid
    # None for kinds that have no branch structure at all, such as rocks.
    skeleton: Skeleton | None
    spec: dict          # the species, as authored
    seed: int
    realized: dict = field(default_factory=dict)  # this individual of it
    stats: dict[str, Any] = field(default_factory=dict)
    # WHICH PART EACH VOXEL BELONGS TO, parallel to `grid.data`, or None for a
    # kind that has no parts to move -- a rock, a tuft, a tree. Animals are
    # rigid-part animated and ship in one pose (owner, 2026-08-14; see
    # docs/animal-rigging-decision.md), so the runtime rotates a wing about a
    # shoulder and needs to be told which voxels are the wing. Both animal
    # generators already computed this to paint their markings and threw it
    # away; carrying it is the whole change.
    parts: Any = None
    part_names: dict[int, str] | None = None

    @property
    def name(self) -> str:
        return f"{get(self.spec, 'name')}-{self.seed:04d}"


# A dense grid this big is refused rather than allowed to thrash the machine.
# A 28 m tree at 2 cm needs about 1.7 GB. Raised to 8 GB when the size caps
# went up for hero assets: a 90 m tree at 5 cm is around 1.2 GB and a 60 m arch
# reaches several, so the old 3 GB ceiling refused the very assets the caps
# were raised to allow. It is still a ceiling and still catches a slider
# combination that would ask for 40 GB, and it is still overridable.
MAX_GRID_MB = int(os.environ.get("ASSET_FORGE_MAX_GRID_MB", "8192"))


class GridTooLarge(RuntimeError):
    """Raised before allocating, with the numbers and the way out."""


def resolution_m(spec: dict, override=None) -> float:
    """Metres per voxel for this build. `override` wins, for coarse previews."""
    cm = float(override) if override else float(get(spec, "resolution_cm"))
    return cm / 100.0


def _piece_count(grid: VoxelGrid) -> int:
    """How many separate lumps this grid holds. 1 is the healthy answer."""
    ndimage = _ndimage()
    occ = grid.data != 0
    if not occ.any():
        return 0
    return int(ndimage.label(occ, structure=np.ones((3, 3, 3), bool))[1])


def _ndimage():
    """`scipy.ndimage`, or a refusal that says what it costs.

    Twenty call sites across this package guard the scipy import and fall back
    to a value that means "nothing to report". Three of them decide whether an
    asset is whole: `pieces` returned **1** -- the healthy answer -- and
    `_drop_orphans` and `_drop_airborne` returned "removed nothing". So on a
    machine without scipy the connectivity rule would not be relaxed, it would
    be ANSWERED, in the affirmative, for every asset, and CI would go green
    having checked nothing.

    scipy is not optional here in any case: `rasterize` needs `cKDTree` for
    clump separation and the rock generator's distance transforms are scipy.
    A build without it does not degrade, it stops. So the checks say so instead
    of quietly passing.
    """
    try:
        from scipy import ndimage
    except ImportError as exc:                       # pragma: no cover
        raise RuntimeError(
            "scipy is required: the connectivity and support checks cannot run "
            "without it, and without them an asset that ships in five pieces "
            "reports as one. Install scipy rather than skipping the check."
        ) from exc
    return ndimage


def _drop_orphans(grid: VoxelGrid) -> int:
    """Delete anything not joined to the main body. Returns how much went.

    Clump displacement is capped so every clump overlaps its twig, and a solid
    core is stamped on the anchor, so a clump cannot be placed detached. What
    still happens is that `foliage.density` carves a clump into two lobes and
    the lobe without the anchor floats — a thinning artifact, not a placement
    one, and there is nothing to be done about it at placement time.

    This is a cleanup, not a cover-up, which is why it returns a count that goes
    into the stats. An asset going into a destructible world should be one
    piece; a designer should still be able to see when a spec is shedding a lot,
    because a spec shedding thousands of voxels is badly tuned even if the
    result is now watertight.

    Rocks do not come through here: their rubble is deliberately separate.
    """
    ndimage = _ndimage()
    occ = grid.data != 0
    if not occ.any():
        return 0
    lab, n = ndimage.label(occ, structure=np.ones((3, 3, 3), bool))
    if n <= 1:
        return 0
    sizes = ndimage.sum(occ, lab, range(1, n + 1))
    keep = int(np.argmax(sizes)) + 1
    doomed = occ & (lab != keep)
    removed = int(doomed.sum())
    grid.data[doomed] = 0
    return removed


def _bridge_corner_joins(grid: VoxelGrid) -> int:
    """Turn corner/edge contacts between pieces into face contacts. Returns
    how many voxels were added.

    THE MESHER DRAWS FACES, AND EVERY CONNECTIVITY RULE HERE COUNTS CORNERS.
    `_drop_orphans` labels at 26-connectivity, so a crown whose thinning left
    chains of diagonally-touching voxels is "one piece" to every check in this
    package -- and renders as dotted speckle hanging beside the tree, because
    two voxels sharing only a corner share no face for the mesher to close.
    Measured on the shipped banks (2026-08-17): 124 of 173 were one piece at
    26-connectivity and HUNDREDS of pieces at face-connectivity -- casuarina
    17% of its voxels, the conifers 3-5%, read on screen as floating leaf bits
    with gaps up to 0.9 m. That is a defect the owner saw in the first tree
    screenshot ever taken, and no check caught it because every check agreed
    with the lenient reading.

    Deleting the detached material is the wrong repair: it is a fifth of a
    casuarina's foliage, and per-voxel thinning artifacts are a known way this
    package quietly ruins silhouettes (the 2 cm thinning defect). Instead every
    diagonal contact gains the one voxel that makes it a face contact -- for a
    2-D diagonal (1,1,0) one step voxel; a 3-D corner (1,1,1) resolves over two
    sweeps, first to an edge contact, then to a face. Material is copied from
    the fragment side of the contact, so a leaf joins by leaf and bark by bark.
    The sweep repeats until face-connectivity reports one piece, which it must:
    26-connectivity already proved a corner path exists to the main body, and
    each sweep converts every link of every such path one step closer to faces.
    The guard is against a grid that was NOT one piece at 26-conn (rocks'
    deliberate rubble never comes through here; `_drop_orphans` runs first for
    trees), where corner chains to the main body need not exist: the loop stops
    when a sweep adds nothing rather than trusting the precondition.

    Cost honesty, per the silent-no-op rule: the count of added voxels goes to
    stats as `bridges_added`. Expected magnitude is 0.5-2% of the asset for a
    thinned conifer and 0 for a broadleaf whose blades are solid; a spec adding
    much more than that is shedding structure sideways and should be re-tuned,
    not silently welded.
    """
    ndimage = _ndimage()
    face = ndimage.generate_binary_structure(3, 1)
    added = 0
    # Diagonal neighbourhood: every 26-offset that is not a face offset.
    diagonals = [(dx, dy, dz)
                 for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1)
                 if abs(dx) + abs(dy) + abs(dz) >= 2]
    while True:
        occ = grid.data != 0
        lab, n = ndimage.label(occ, structure=face)
        if n <= 1:
            return added
        added_this_sweep = 0
        for dx, dy, dz in diagonals:
            occ = grid.data != 0
            lab, n = ndimage.label(occ, structure=face)
            if n <= 1:
                return added
            # Voxels whose neighbour at (dx,dy,dz) is occupied AND in a
            # different face-component: a corner/edge contact across pieces.
            here = lab[max(dx, 0) or None: grid.data.shape[0] + min(dx, 0) or None,
                       max(dy, 0) or None: grid.data.shape[1] + min(dy, 0) or None,
                       max(dz, 0) or None: grid.data.shape[2] + min(dz, 0) or None]
            there = lab[max(-dx, 0) or None: grid.data.shape[0] + min(-dx, 0) or None,
                        max(-dy, 0) or None: grid.data.shape[1] + min(-dy, 0) or None,
                        max(-dz, 0) or None: grid.data.shape[2] + min(-dz, 0) or None]
            contact = (here > 0) & (there > 0) & (here != there)
            if not contact.any():
                continue
            xs, ys, zs = np.nonzero(contact)
            # Back to full-grid coordinates of the "here" voxel.
            xs = xs + max(dx, 0)
            ys = ys + max(dy, 0)
            zs = zs + max(dz, 0)
            # The step voxel: advance the first nonzero axis only, which is
            # face-adjacent to `here` and strictly closer to `there`.
            if dx != 0:
                bx, by, bz = xs - dx, ys, zs
            elif dy != 0:
                bx, by, bz = xs, ys - dy, zs
            else:
                bx, by, bz = xs, ys, zs - dz
            empty = grid.data[bx, by, bz] == 0
            bx, by, bz = bx[empty], by[empty], bz[empty]
            if bx.size == 0:
                continue
            # Material from the fragment side of each contact, deduplicated --
            # np.unique keeps the first writer per cell deterministic.
            flat = (bx * grid.data.shape[1] + by) * grid.data.shape[2] + bz
            _, first = np.unique(flat, return_index=True)
            bx, by, bz = bx[first], by[first], bz[first]
            grid.data[bx, by, bz] = grid.data[xs[empty][first],
                                              ys[empty][first],
                                              zs[empty][first]]
            added += int(bx.size)
            added_this_sweep += int(bx.size)
        if added_this_sweep == 0:
            # No corner contacts left but still >1 piece: genuinely separated
            # material with a real air gap. Not this pass's job -- leave it for
            # the orphan rule rather than invent geometry across open air.
            return added


def _single_piece(grid: VoxelGrid) -> tuple[int, list[int]]:
    """Reduce a rock to ONE connected lump. Returns (voxels deleted, big ones).

    ONE GENERATION MAKES ONE ENTITY. One rock, one tree, one clump of grass. A
    build that hands back a stone plus nine separate lumps has produced ten
    things, and everything downstream -- placement, streaming, destruction --
    has to guess which of them was the asset. Small, medium and large stones are
    separate species, generated separately and placed by placement logic; they
    are not side effects of building a big one.

    This replaces a support test. The old rule kept anything that reached the
    ground or rested on something that did, on the premise that a rock's rubble
    ring is deliberately separate from the rock. That premise is withdrawn: the
    ring is a second asset and `rock.build` no longer draws one. What is left
    over after `rock._seat` has dropped every loose block onto whatever is
    beneath it is genuinely detached material -- a blade weathering cut off, a
    crumb shed from under a face that was later carved away -- and it cannot
    ship inside a single-entity asset.

    So it goes, and the SIZE of anything worth noticing goes into the stats.
    Deleting a 30,000-voxel blade quietly is how a visible defect becomes an
    invisible one; half a percent of the stone is well above a weathering crumb
    (`hero-basalt-colonnade`'s largest was 0.05%) and well below a structural
    piece. A non-empty list means the spec is shedding structure and needs
    re-authoring, which is a different thing from a spec shedding dust.

    The name of the stat is unchanged -- `airborne_kept` -- because
    `forge.cli.unsupported` and `tools/buildcheck.py` read it and treat a
    non-empty list as a failure, which is still exactly the right reading. What
    changed underneath it is that those pieces are now deleted rather than left
    in the asset. Those two files belong to someone else this session; their
    wording needs a pass.
    """
    ndimage = _ndimage()
    occ = grid.data != 0
    if not occ.any():
        return 0, []
    lab, n = ndimage.label(occ, structure=np.ones((3, 3, 3), bool))
    if n <= 1:
        return 0, []

    sizes = np.bincount(lab.ravel(), minlength=n + 1)
    sizes[0] = 0
    keep = int(np.argmax(sizes))
    limit = 0.005 * float(occ.sum())
    big = [int(sizes[c]) for c in range(1, n + 1)
           if c != keep and sizes[c] > limit]

    doomed = occ & (lab != keep)
    removed = int(doomed.sum())
    grid.data[doomed] = 0
    return removed, sorted(big, reverse=True)


def rng_for(spec: dict, seed: int) -> np.random.Generator:
    """The random stream that decides WHICH INDIVIDUAL of a species you get.

    SEEDED FROM `spec.seed_hash`, NOT FROM `spec.spec_hash`, and the difference
    is one field. A pose is a posture, not a different animal: the owner's ask
    was one bird that can be drawn perched or flying, same size, same markings,
    same colours, wings folded or spread. Seeded from the spec hash -- which the
    pose is part of -- `common-raven` seed 7 perched and `common-raven` seed 7
    flying were two different ravens, so a bird could not land without changing
    size on the way down. `spec.SEED_INVARIANT` is the list of fields left out
    of this hash and the argument for keeping it to exactly one.

    The spec hash is still what identifies the SPEC everywhere else -- the
    library entry, the .vxa metadata, the preview cache key -- because a perched
    raven and a flying raven are genuinely two assets.
    """
    h = int(seed_hash(spec), 16)
    return np.random.default_rng([int(seed), h & 0xFFFFFFFF, (h >> 32) & 0xFFFFFFFF])


def build(spec: dict, seed: int, *, connectivity: bool = True,
          resolution_cm=None) -> Tree:
    t0 = time.perf_counter()
    voxel_m = resolution_m(spec, resolution_cm)
    rng = rng_for(spec, seed)

    # Pick this individual out of the species before growing it, so two seeds
    # differ in height, spread and lean and not only in twig placement.
    live, _ = spec_realize(spec, rng)
    # Crown size can be derived from trunk thickness rather than authored.
    # Applied AFTER the individual is drawn, so a seed that came out taller or
    # thicker than its siblings gets the crown that goes with the tree it
    # actually is, not the one the species average would have had.
    live = envelope.apply_allometry(live)

    kind = get(live, "kind")
    # Only the animal generators produce these; a rock has no parts to move.
    parts = None
    part_names = None
    model = get(live, "growth.model")
    skel = None
    clumps = 0
    orphans = 0
    pieces_built = 1
    airborne_kept: list[int] = []

    if kind in BRANCHLESS:
        # No skeleton. A rock is accreted and carved rather than grown; a blade
        # of grass does not branch; a fish is a lofted solid with plates stuck
        # on it; a bird is six jointed parts. All four size their own grid from
        # what they actually traced.
        t_grow = time.perf_counter()
        gen = (rocklib if kind in BOULDER_KINDS
               else fishlib if kind in FISH_KINDS
               else birdlib if kind in BIRD_KINDS
               else quadlib if kind in QUAD_KINDS else groundlib)
        gen_out: dict = {}
        grid = (gen.build(live, rng, voxel_m, out=gen_out)
                if kind in FISH_KINDS or kind in BIRD_KINDS or kind in QUAD_KINDS
                else gen.build(live, rng, voxel_m))
        parts = gen_out.get("tags")
        part_names = partslib.names() if parts is not None else None
        pieces_built = _piece_count(grid)
        bridges = 0
        if kind in BOULDER_KINDS:
            orphans, airborne_kept = _single_piece(grid)
            # Rocks shatter at face-connectivity the same way crowns do --
            # hero-tor-stack was 35 face-pieces inside one 26-conn lump.
            bridges = _bridge_corner_joins(grid)
        need_mb = dense_bytes(grid.shape) / 1e6
        t_raster = time.perf_counter()
    else:
        skel = {"whorl": grow_whorl, "frond": grow_frond}.get(model, grow)(live, rng)
        if model != "frond":
            skel = add_strands(skel, live, rng)
        skel = add_roots(skel, live, rng)
        t_grow = time.perf_counter()

        origin, shape = rasterize.bounds(skel, live, voxel_m)
        need_mb = dense_bytes(shape) / 1e6
        if need_mb > MAX_GRID_MB:
            raise GridTooLarge(
                f"{get(live, 'name')} at {voxel_m * 100:g} cm needs a "
                f"{shape[0]}x{shape[1]}x{shape[2]} grid ({need_mb / 1000:.1f} GB), over the "
                f"{MAX_GRID_MB / 1000:.1f} GB limit. Use a coarser voxel size, a smaller "
                f"asset, or raise ASSET_FORGE_MAX_GRID_MB."
            )
        grid = VoxelGrid(shape, tuple(origin), voxel_m)
        rasterize.wood(grid, skel, live, origin)
        clumps = (rasterize.frond_blades if model == "frond" else rasterize.foliage)(
            grid, skel, live, origin, rng)
        pieces_built = _piece_count(grid)
        orphans = _drop_orphans(grid)
        bridges = _bridge_corner_joins(grid)
        t_raster = time.perf_counter()

    # The tags ride along, cropped by the same box -- see VoxelGrid.crop.
    if parts is not None:
        grid, parts = grid.crop(also=parts)
    else:
        grid = grid.crop()

    stats: dict[str, Any] = {
        "seed": seed,
        "spec_hash": spec_hash(spec),
        "kind": kind,
        "nodes": skel.n if skel else 0,
        "segments": int(skel.n - 1) if skel else 0,
        "max_order": int(skel.order.max()) if skel and skel.n else 0,
        "iterations": skel.iterations if skel else 0,
        "targets_left": skel.targets_left if skel else 0,
        "clumps": clumps,
        # Voxels deleted because they were not joined to the body.
        # Kept visible: a spec shedding thousands is badly tuned even
        # though the asset that comes out is watertight.
        "orphans_removed": orphans,
        # Voxels ADDED to turn corner-only contacts into face contacts, so the
        # mesher sees the same single piece the 26-conn checks see. See
        # _bridge_corner_joins for why deleting was the wrong repair.
        "bridges_added": bridges,
        # Detached pieces too big to delete quietly. Non-empty means the spec is
        # shedding structure, not debris -- see _single_piece and _drop_orphans.
        "airborne_kept": airborne_kept,
        # How many separate lumps the generator produced before it was reduced
        # to one. 1 is the healthy answer; anything else is here so that "we
        # deleted the difference" is never invisible.
        "pieces_built": pieces_built,
        "voxel_cm": round(voxel_m * 100, 4),
        "height_m": round(grid.shape[2] * voxel_m, 2),
        # The LONG horizontal axis. For everything that stands up this is the
        # width and `height_m` is the interesting number; for a fish it is the
        # other way round, and a contact sheet that labelled a 30 cm trout
        # "0.1 m" was reporting how DEEP it is.
        "length_m": round(grid.shape[0] * voxel_m, 3),
        "footprint_m": (
            round(grid.shape[0] * voxel_m, 2),
            round(grid.shape[1] * voxel_m, 2),
        ),
        "grid_mb": round(need_mb, 1),
        "extent_vox": tuple(int(v) for v in grid.shape),
        # How many .vox models this needs: the format caps a model at 256
        # voxels per axis, so anything larger is written as a scene of several.
        "vox_models_needed": int(np.prod([
            max(1, -(-int(v) // 256)) for v in grid.shape])),
        "voxels": grid.count(),
        "by_material": grid.histogram(),
        "ground_contact": rasterize.ground_contact(grid),
        "ms_grow": round((t_grow - t0) * 1e3, 1),
        "ms_raster": round((t_raster - t_grow) * 1e3, 1),
    }
    if connectivity and (kind in TUFT_KINDS or kind in FISH_KINDS
                         or kind in BIRD_KINDS or kind in QUAD_KINDS):
        # Ground cover has no wood, but "is this one piece?" is exactly the
        # question that matters for it -- a tuft whose blades do not reach the
        # root crown is a handful of floating threads. A fish is the same
        # question with a different failure: a fin that comes off the body, and
        # a bird has five more ways to fail it than a fish does -- a head off
        # the end of a thin neck, a bill off the head, a wingtip finger, a tail
        # feather, a leg. A land animal has more ways still: four limbs each
        # joined only at a hip or a shoulder, a tail, a neck, a pair of ears and
        # a pair of antlers, and a leg coming off at the hip is the failure this
        # generator was expected to ship with.
        attached = grid.component_fraction(None, connectivity=3)
        stats["attached_frac"] = round(attached, 4)
        stats["detached"] = int(round(stats["voxels"] * (1.0 - attached)))
    elif connectivity and kind not in BRANCHLESS:
        wood_ids = {
            materials.resolve(get(live, "materials.bark")),
            materials.resolve(get(live, "materials.core")),
        }
        wood = grid.material_mask(wood_ids)
        wood_total = int(wood.sum())
        wood_frac = grid.component_fraction(wood, connectivity=1)
        stats["wood_connected"] = round(wood_frac, 4)
        stats["wood_detached"] = int(round(wood_total * (1.0 - wood_frac)))
        attached = grid.component_fraction(None, connectivity=3)
        stats["attached_frac"] = round(attached, 4)
        stats["detached"] = int(round(stats["voxels"] * (1.0 - attached)))
    stats["ms_total"] = round((time.perf_counter() - t0) * 1e3, 1)

    return Asset(grid=grid, skeleton=skel, spec=spec, seed=seed, realized=live,
                 stats=stats, parts=parts, part_names=part_names)


# Kept as an alias: the tool grew up as a tree generator and plenty of call
# sites still say Tree.
Tree = Asset


def health(tree: Asset) -> list[str]:
    """Things that would make this asset unusable in the world.

    Reported per tree rather than asserted, because a spec being explored with
    sliders is allowed to be briefly nonsense; a spec being saved to the
    library is not.
    """
    problems: list[str] = []
    s = tree.stats

    if s["voxels"] == 0:
        problems.append("empty: no voxels generated")
        return problems
    if s.get("wood_detached", 0):
        problems.append(
            f"broken: {s['wood_detached']} wood voxels are not joined to the trunk"
        )
    kind = s.get("kind", "tree")
    if s.get("detached", 0) > max(8, 0.01 * s["voxels"]):
        problems.append(f"loose: {s['detached']:,} voxels float free of the asset")
    if s["ground_contact"] == 0 and kind not in UNGROUNDED:
        problems.append("floating: nothing touches the ground plane")
    if kind not in BRANCHLESS and s["max_order"] == 0:
        problems.append("bare: the trunk never branched")
    # Exceeding 256 voxels on an axis is NOT a problem and is no longer flagged.
    # It used to be rare enough to be worth pointing at; at the 5 cm asset
    # lattice most trees exceed it, the writer splits them and the selftest
    # checks the round trip. Leaving it in `health` meant ten of forty-two
    # species carried a permanent warning flag, which made "Keep all clean" skip
    # every large tree -- a check that fires on the normal case trains you to
    # ignore checks. The model count is in the stats instead.
    if (kind not in BRANCHLESS
            and get(tree.spec, "foliage.enabled") and s["clumps"] == 0):
        problems.append("bald: foliage is on but no clump was placed")
    return problems
