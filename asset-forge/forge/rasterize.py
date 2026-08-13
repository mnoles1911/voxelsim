"""Skeleton -> voxels.

This is the step that makes the tool voxel-native rather than a mesh tool with
a converter bolted on. The alternative -- build a nice mesh, then voxelize it
with trimesh or cuda_voxelizer -- looks easier and is worse here: at 10 cm every
branch under a voxel across is lost or arbitrarily fattened by the sampler, and
the crown can end up not connected to the trunk. Drawing the skeleton straight
into the grid means the thin-branch decision is ours to make, once, explicitly.
See `docs/tree-asset-generator-research.md` section 4.
"""

from __future__ import annotations

import numpy as np

from . import materials
from . import envelope
from .envelope import crown_bounds
from .grid import VoxelGrid, m_to_vox
from .skeleton import Skeleton
from .spec import get, _FOLIAGE_HABITS


def bounds(skel: Skeleton, spec: dict, voxel_m: float) -> tuple[np.ndarray, tuple[int, int, int]]:
    """Grid origin (voxels) and shape, with room for wood and foliage."""
    pad_m = float(skel.radius.max())
    if get(spec, "foliage.enabled"):
        pad_m = max(
            pad_m,
            float(get(spec, "foliage.clump_radius_m")) + abs(float(get(spec, "foliage.droop_m"))),
        )
    pad = int(np.ceil(m_to_vox(pad_m, voxel_m))) + 2

    lo = np.floor(m_to_vox(skel.pos.min(axis=0), voxel_m)).astype(np.int64) - pad
    hi = np.ceil(m_to_vox(skel.pos.max(axis=0), voxel_m)).astype(np.int64) + pad
    lo[2] = 0  # the base sits on the ground plane; nothing below it
    shape = tuple(int(v) for v in (hi - lo + 1))
    return lo, shape


def wood(grid: VoxelGrid, skel: Skeleton, spec: dict, origin: np.ndarray) -> None:
    bark = materials.resolve(get(spec, "materials.bark"))
    core = materials.resolve(get(spec, "materials.core"))
    core_mat = core if core != bark else None

    pos_vox = m_to_vox(skel.pos, grid.voxel_m) - origin
    r_vox = m_to_vox(skel.radius, grid.voxel_m)
    parents, children = skel.segments()

    def draw(pi, ci):
        grid.capsule(pos_vox[pi], pos_vox[ci], float(r_vox[pi]),
                     float(r_vox[ci]), bark, core_mat=core_mat)

    lobes = int(get(spec, "trunk.lobes"))
    fluting = lobes >= 2 and float(get(spec, "trunk.lobe_depth")) > 0.0

    if not fluting:
        for pi, ci in zip(parents, children):
            draw(pi, ci)
        return

    # Draw the bole, groove it, and only then hang the branches on it.
    #
    # Order is the whole trick. Grooving a trunk that already has branches
    # welded to it cuts them off at the join -- the trunk's surface is exactly
    # where limbs attach, so any surface carve severs something. Six species
    # out of seven shed wood that way, and neither a height limit nor masking
    # the junctions fixed it, because on a thick tree the limbs attach inside
    # the trunk's own outline and are indistinguishable from it by then.
    # Cutting first and attaching afterwards means there is nothing to sever,
    # and the branches bridge into the grooves as they are drawn.
    trunk = skel.order[children] == 0
    for pi, ci in zip(parents[trunk], children[trunk]):
        draw(pi, ci)
    _flute_trunk(grid, spec, origin, lobes,
                 float(get(spec, "trunk.lobe_depth")))
    for pi, ci in zip(parents[~trunk], children[~trunk]):
        draw(pi, ci)


def _flute_trunk(grid: VoxelGrid, spec: dict, origin: np.ndarray,
                 lobes: int, depth: float) -> None:
    """Run vertical grooves up the trunk so it is not a circular post.

    Every branch here is drawn as a capsule, which makes every cross-section a
    circle. Real trunks are not round -- they run in ridges and hollows, and on
    a big tree those flutes are the most legible thing about the bole because
    they catch light along their whole length. The classic tree grammars carry
    it as a parameter for exactly that reason.

    Cut rather than added, and normalised so the widest lobe is the radius the
    trunk already had. Adding ridges outward would fatten the trunk and quietly
    undo the radius the taper model worked out.

    The local radius is measured from the wood that is actually there, one
    height at a time, instead of being recomputed from the skeleton. That way
    the groove depth follows the real taper, including the root flare, with
    nothing to keep in step.
    """
    data = grid.data
    nz = data.shape[2]
    # Stop below the lowest branch, not at the crown base. The trunk's surface
    # is exactly where branches attach to it, so grooving that surface cuts the
    # join and leaves the limb floating -- three hundred voxels of willow came
    # off that way. Everything under the branch-free height is safe to cut
    # because there is nothing joined to it.
    clear_m = float(get(spec, "height_m")) * float(get(spec, "trunk.clear_frac"))
    limit_m = min(envelope.crown_bounds(spec)[0], clear_m) * 0.95
    top = int(m_to_vox(limit_m, grid.voxel_m) - origin[2])
    top = max(0, min(nz, top))
    ax, ay = -float(origin[0]), -float(origin[1])

    xs = (np.arange(data.shape[0], dtype=np.float32) + 0.5 - ax)[:, None]
    ys = (np.arange(data.shape[1], dtype=np.float32) + 0.5 - ay)[None, :]
    rad = np.sqrt(xs * xs + ys * ys)
    ang = np.arctan2(ys, xs)
    # Peaks at 1 so the trunk never grows, troughs at (1-d)/(1+d).
    shape = (1.0 + depth * np.cos(lobes * ang)) / (1.0 + depth)

    try:
        from scipy import ndimage
    except ImportError:
        return
    ai, aj = int(round(ax)), int(round(ay))
    if top <= 0 or not (0 <= ai < data.shape[0] and 0 <= aj < data.shape[1]):
        return

    # Work out where the bole ends and something joins it, BEFORE cutting
    # anything. A groove is a cut in the trunk's surface, and the trunk's
    # surface is exactly where limbs attach -- so cutting blind severs joins
    # and leaves limbs floating. It did, on six species out of seven. Anything
    # within a couple of voxels of wood that is not the bole is left alone.
    col = np.zeros(data[:, :, :top].shape, bool)
    for z in range(top):
        solid = data[:, :, z] != 0
        if not solid.any():
            continue
        lab, n = ndimage.label(solid)
        if n and lab[ai, aj]:
            col[:, :, z] = lab == lab[ai, aj]
    joins = ndimage.binary_dilation((data[:, :, :top] != 0) & ~col,
                                    iterations=2)

    for z in range(top):
        solid = data[:, :, z] != 0
        if not solid.any():
            continue
        # Only the blob the trunk axis is standing in. A limb crossing this
        # height is a separate island in the slice, and carving it is how a
        # willow's trailing branches ended up cut off from their own tree --
        # a height limit could not fix that, because the willow trails all the
        # way to the ground and there is no height without branches.
        solid = col[:, :, z] & ~joins[:, :, z]
        if not solid.any():
            continue
        here = rad[col[:, :, z]]
        # A high quantile, not the maximum: one stray voxel from a low branch
        # passing the trunk would otherwise set the radius for the whole slab
        # and the flutes would stop biting.
        r_local = float(np.quantile(here, 0.97))
        if r_local < 2.5:      # too thin for a groove to read
            continue
        # Cut only the trunk's own outer skin. Taking every wood voxel beyond
        # the fluted radius also takes branches that happen to pass this height
        # further out -- a willow's trailing limbs come back down past their own
        # trunk, and eighty voxels of one were sliced off it and left floating.
        # An upper bound keeps the groove on the bole where it belongs.
        # NO OUTER BOUND ON THE CUT. There used to be one -- only wood between
        # the groove floor and 1.06 of the local radius was removed -- and it
        # is what stranded the wood this check kept reporting. The trunk's
        # surface is not a clean cylinder: taper, root flare and relief all put
        # voxels further out than 1.06, and taking the band UNDER them while
        # leaving them in place cuts their footing away. What is left is a rind
        # of surface material attached to the bole at its corners, which was
        # 11,133 voxels on `hero-sequoia` seed 3 and nothing you can see.
        #
        # The symptom is worth writing down because every obvious reading of it
        # is wrong. Nothing floated: all 19,842,433 wood voxels were a single
        # piece under 26-CONNECTIVITY. The breaks were nowhere near a branch --
        # the lowest limb on that tree attaches at 41.7 m and the fluting stops
        # at 39.3 m, so the join protection above was doing its job. The wood
        # check reads FACE connectivity, correctly, and a corner-only join is
        # not a join. Smoothing the per-slice radius was tried first, on the
        # theory that a jittering groove floor cut the rind into steps; it took
        # 11,133 to 10,669 and was reverted, because a change that does not
        # explain the number is not the fix.
        #
        # The bound was there to protect a willow's trailing limbs where they
        # come back down past their own trunk, and that job is already done
        # properly by `joins` above -- wood belonging to any island that is not
        # the bole, dilated by two, is excluded from `solid` before we get
        # here. Two guards for one hazard, and the cruder one was causing a
        # second hazard of its own. `weeping-willow` is in the verification for
        # this change for exactly that reason.
        data[:, :, z][solid & (rad > r_local * shape)] = 0


def foliage(
    grid: VoxelGrid,
    skel: Skeleton,
    spec: dict,
    origin: np.ndarray,
    rng: np.random.Generator,
) -> int:
    """Leaf clumps on the twigs. Returns how many clumps were placed."""
    if not get(spec, "foliage.enabled"):
        return 0

    leaf = materials.resolve(get(spec, "materials.leaf"))
    min_order = int(get(spec, "foliage.min_order"))
    coverage = float(get(spec, "foliage.coverage"))
    density = float(get(spec, "foliage.density"))
    squash = float(get(spec, "foliage.squash"))
    droop = float(get(spec, "foliage.droop_m"))
    rough = float(get(spec, "foliage.rough"))
    r_vox = m_to_vox(float(get(spec, "foliage.clump_radius_m")), grid.voxel_m)

    # Clumps go on every twig, not only on the branch tips. Tips alone are few,
    # which forces each clump to be large to fill the crown, and a crown built
    # from a dozen big spheres reads as a pile of balls rather than foliage.
    # Many small clumps following the twigs read as a canopy.
    # Widening this to six tip-radii was TRIED, on the theory that the whorl
    # arms carrying a conifer's needle shoots are thicker than three and so get
    # no foliage at all. It changed the pine's candidate set not at all -- the
    # voxel count came back identical to the byte -- so the bare arms on a pine
    # are not caused by the radius filter, and the theory is wrong. Left at
    # three, with the note, so the next person does not spend the same hour.
    twig_max = float(get(spec, "growth.tip_radius_m")) * 3.0

    # `rosette` species carry most of their leaves on SHORT SHOOTS -- spurs a
    # few millimetres long -- spaced along wood that is two or three years old,
    # not only on the current season's growth. Birch, larch, apple and hawthorn
    # are all built this way, and it is the whole reason their inner branches
    # are not bare. No amount of reshaping a clump produces that if the clump is
    # only ever offered a twig to sit on, so this habit widens the leaf-bearing
    # wood itself: back one fork order, and out to wood two and a half times a
    # tip in thickness. The clump size the habit table gives it is small to
    # match -- a spur cluster, not a shoot.
    habit = str(get(spec, "foliage.habit"))
    if habit == "rosette":
        twig_max *= 2.5
        min_order = max(min_order - 1, 1)
    cand = np.flatnonzero((skel.order >= min_order) & (skel.radius <= twig_max))
    if cand.size == 0:
        # The tree never forked deeply enough to have twigs of that order.
        # Fall back to every tip so the species still renders as foliated,
        # rather than silently producing a bare skeleton.
        cand = np.flatnonzero(skel.is_tip)
    if cand.size == 0:
        return 0

    # Thin the candidates so clump centres keep their distance BEFORE the
    # coverage roll. Picking twigs at random and hoping for gaps does not work:
    # twigs are dense, so any coverage high enough to fill the crown also packs
    # the clumps until they fuse. Enforcing a minimum separation is what gives a
    # canopy distinct masses with daylight between them.
    separation = float(get(spec, "foliage.separation"))
    clustering = float(get(spec, "foliage.clustering"))
    min_dist = float(get(spec, "foliage.clump_radius_m")) * separation
    before = int(cand.size)
    if min_dist > 0.0 and cand.size > 1:
        # Spacing is not the same everywhere in the crown. Dart-throwing with
        # one distance for the whole tree gives blue noise -- the most EVEN
        # arrangement there is -- and real foliage is the opposite: strongly
        # aggregated, with dense masses and daylight voids between them. That
        # aggregation is a first-order effect, not a detail; treating a canopy
        # as evenly filled rather than clumped misjudges how much light reaches
        # the ground by tens of percent.
        #
        # So the required distance is varied across the crown instead: tight
        # where a mass is forming, loose between them. The average is left
        # alone, so the clumps redistribute rather than thin out, and the
        # separation slider goes on meaning what it did.
        dist = _clustered_spacing(skel.pos[cand], min_dist, clustering, rng)
        keep = np.zeros(cand.size, bool)
        # Tips get first refusal on the room, rather than an exemption from
        # needing any.
        #
        # A tip that loses its clump leaves bare wood sticking out past the
        # canopy with nothing on it, which is most of what reads as a bare
        # twig. Exempting tips from the spacing rule fixes that and costs far
        # too much: tips are the bulk of the candidate set, so the exemption
        # suspends spacing for most of the tree. Measured, it took the oak,
        # birch, elm and river-broadleaf to about 1% visible wood -- not a full
        # crown but a featureless green mat with no daylight in it and no
        # branch structure legible anywhere -- and drove the oak from 0.7 to
        # 3.7 million voxels. Dropping the exemption swung it back to 19-30%
        # wood, which is the bareness that started this.
        #
        # Ordering costs nothing and keeps both. Dart-throwing is greedy, so
        # whoever is offered a place first takes it; the survivor COUNT is set
        # by the spacing and does not move, but the survivors are now the tips.
        keep[_thin_by_distance(skel.pos[cand], dist, rng,
                               first=skel.is_tip[cand])] = True
        cand = cand[keep]

    if coverage < 1.0:
        roll = (rng.random(cand.size) < coverage) | skel.is_tip[cand]
        cand = cand[roll]
    if cand.size == 0:
        return 0

    # Vary each clump's size and position, so the canopy does not resolve into
    # a lattice of identical spheres on close inspection.
    jitter = float(get(spec, "foliage.clump_jitter"))

    # Give back some of the crown that separation took away.
    #
    # Raising `separation` deletes whole clumps -- measured, going from 1.2 to
    # 3.0 removes 85% of them and takes a quarter of the silhouette with it --
    # so the crown shrinks as a side effect of a control that reads as being
    # about spacing. Growing what survives holds the leaf area roughly steady.
    #
    # Scaled from the separation itself, against a reference of 1 where clumps
    # just touch. The obvious alternative -- comparing how many candidates went
    # in against how many came out -- is wrong by a mile, because what goes in
    # is every twig on the tree and what comes out is a few dozen clumps. That
    # ratio runs into the hundreds and pins every species at the cap, which
    # tripled the voxel count of an oak while claiming to correct for spacing.
    #
    # Deliberately partial. Full compensation would close the gaps the
    # separation was raised to open, and a few big masses are not the same
    # picture as many small ones even at equal area.
    compensate = float(get(spec, "foliage.compensate"))
    if compensate > 0.0 and separation > 1.0:
        r_vox *= min(1.9, separation ** (0.55 * compensate))

    radii = r_vox * (1.0 + jitter * (rng.random(cand.size) * 2.0 - 1.0))
    offsets = rng.normal(scale=jitter * r_vox * 0.5, size=(cand.size, 3))

    # Leaf density is not uniform through a crown. Measured on five temperate
    # broadleaves, it rises markedly from the bottom of the crown to the top in
    # every one of them -- the upper layer of small, steeply held leaves shades
    # the productive interior from a surplus of light and heat it cannot use.
    # Every model in the literature assumes a uniform density inside the crown
    # envelope, and every set of measurements says otherwise.
    #
    # The multiplier averages to one over the crown, so this REDISTRIBUTES
    # foliage rather than adding or removing it, and the density slider keeps
    # meaning what it did.
    top_bias = float(get(spec, "foliage.top_bias"))
    heights = None
    if top_bias > 0.0:
        low, high = crown_bounds(spec)
        heights = np.clip((skel.pos[cand][:, 2] - low) / max(high - low, 1e-3),
                          0.0, 1.0)

    # The direction of the twig each clump hangs from, so the clump can be a
    # shoot rather than a ball. Falls back to straight up at the root, which
    # only a degenerate tree reaches.
    stretch = float(get(spec, "foliage.stretch"))
    stretch, squash, distal, r_habit = _habit(habit, stretch, squash)
    # Applied to `radii`, NOT to `r_vox`: r_vox stopped being read the moment
    # `radii` was built from it above, so scaling it here would have been a
    # silent no-op -- the change would have run, reported nothing, and altered
    # nothing, which is the failure mode this file has already produced twice.
    radii = radii * r_habit   # keep the habit a change of shape, not of mass

    axes = seg_len = None
    par = skel.parent[cand]
    seg = skel.pos[cand] - skel.pos[np.where(par >= 0, par, cand)]
    seg_len = np.linalg.norm(seg, axis=1, keepdims=True)
    axes = np.where(seg_len < 1e-9, np.array([0.0, 0.0, 1.0]),
                    seg / np.maximum(seg_len, 1e-9))
    if habit == "pendulous":
        # Willow and weeping birch hang their sprays from the shoot rather than
        # carrying them along it, so the spray's long axis is gravity's, not the
        # twig's. Blended rather than replaced, or every clump on the tree ends
        # up parallel and the crown reads as combed.
        axes = axes * 0.35 + np.array([0.0, 0.0, -1.0])
        axes /= np.maximum(np.linalg.norm(axes, axis=1, keepdims=True), 1e-9)
    if habit == "flat_spray":
        # Two-ranked needles lie in one plane, and that plane is held level to
        # the light: the spray is wide across the twig and thin top to bottom.
        # `squash` already compresses in z, so the plane comes for free -- what
        # matters is that the clump is NOT elongated much down the twig as well,
        # or a flat spray and a radial one differ only in thickness.
        pass

    placed = 0
    droop_vox = m_to_vox(droop, grid.voxel_m)
    for k, i in enumerate(cand):
        anchor = m_to_vox(skel.pos[i], grid.voxel_m) - origin
        shift = offsets[k].copy()
        shift[2] -= droop_vox
        r = max(radii[k], 1.0)
        dens = density
        if heights is not None:
            dens = float(np.clip(
                density * (1.0 + top_bias * (heights[k] - 0.5) * 1.6),
                0.02, 1.0))
        # Keep the clump ON the twig it hangs from. Droop and jitter together
        # can carry a clump clear of its anchor, and the ball then sits in mid
        # air joined to nothing. That was invisible at 10 cm, where a clump is
        # two voxels across and any near miss still touches, and at 2 cm it left
        # whole thousand-voxel leaf masses floating -- the single largest source
        # of loose voxels in the library. Capping the displacement at three
        # quarters of the radius keeps every clump overlapping its twig while
        # leaving droop and jitter their visible range.
        # Slide the clump toward the growing end of its shoot. Foliage is borne
        # on the season's new growth, so it sits at the far end of a twig, and
        # a clump centred on the node leaves the last stretch of wood bare and
        # poking out of the canopy. In pine that offset is the whole look:
        # needles are held on only the last two or three years of growth, so
        # the tufts are at the shoot ends with clean twig behind them.
        if distal > 0.0:
            shift += axes[k] * (distal * float(seg_len[k, 0]) / grid.voxel_m)

        reach = float(np.linalg.norm(shift))
        cap = r * (0.75 + distal)
        if reach > cap:
            shift *= cap / reach
        grid.blob(
            anchor + shift, r, leaf, rng, density=dens, squash=squash,
            only_air=True, rough=rough,
            along=None if axes is None else axes[k], stretch=stretch,
        )
        # A solid core ON the twig, whatever the thinning did.
        #
        # Capping the displacement keeps the clump's VOLUME over its anchor, but
        # the voxels that actually bridge to the wood are the innermost ones,
        # and `density` is free to remove those like any others. When it did,
        # the clump ended up a shell starting two voxels out and floating -- two
        # of them on one acacia, twelve hundred voxels each. This is a handful
        # of voxels that guarantees the join rather than leaving it to chance.
        grid.ball(anchor, min(r * 0.4, 1.7), leaf, only_air=True)
        placed += 1
    return placed


def _habit(habit: str, stretch: float, squash: float):
    """How a species carries its leaves: (stretch, squash, distal, radius x).

    Every tree in the library was drawing the same clump, and a pine, a spruce
    and a fir look nothing like each other for reasons that are entirely about
    how the foliage sits on the shoot:

      spiral      leaves all round the shoot on the 137.5 degree divergence --
                  oak, poplar, most broadleaves. The neutral case.
      distichous  two-ranked, so the whole spray lies in ONE plane held level
                  to the light. Beech, elm, fir, hemlock. Wide across, thin top
                  to bottom -- this is what makes a fir spray read as a spray
                  and not as a sausage.
      opposite    decussate pairs at ninety degrees -- maple, ash. Between the
                  other two, and shorter, because a pair occupies less shoot
                  than a spiral of the same leaf count.
      tuft        needles in fascicles held on only the last two or three years
                  of growth, so foliage sits at the shoot END and there is
                  clean twig behind it. Pine. The bare twig is the species, not
                  a defect -- pine crowns really are see-through.
      radial      single needles all round the shoot, retained five to seven
                  years, so the shoot is clothed along its whole length.
                  Spruce. The densest of these.
      rosette     leaves in compressed clusters on SHORT shoots spaced along
                  older wood, not only at the growing points. Birch, larch,
                  apple. This is the one that puts foliage back on the inner
                  branches: a model that foliates tips alone leaves exactly the
                  bare interior that a rosette species does not have.
      pendulous   sprays hanging from the shoot rather than borne along it.
                  Willow, weeping birch.

    Multipliers, not absolutes, so a species keeps the clump size it was
    authored with and only its ARRANGEMENT changes.

    Returns a radius multiplier as well, and it is not decoration. Flattening a
    clump with `squash` scales its z semi-axis, so it scales the VOLUME by the
    same factor: `distichous` at 0.45 was quietly removing 55% of a species'
    leaf mass, and elm, cypress and acacia came out the barest broadleaves in
    the library for no reason anyone authored. `stretch` has no such problem --
    the blob already trades length against width to hold volume -- which is
    exactly why the asymmetry went unnoticed. Undoing it on the radius keeps the
    habit a statement about SHAPE, which is the only thing it is supposed to be.

    Only the habit's own factor is compensated, not the designer's authored
    `foliage.squash`. That one is a deliberate choice made per species and the
    whole library is tuned around it.
    """
    a, b, d = _HABIT[habit] if habit in _HABIT else _HABIT["spiral"]
    return stretch * a, squash * b, d, b ** (-1.0 / 3.0)


_HABIT = {                        # stretch x, squash x, distal shift
    "spiral":     (1.00, 1.00, 0.18),
    "distichous": (0.70, 0.45, 0.20),
    "opposite":   (0.80, 0.80, 0.20),
    "tuft":       (1.50, 0.90, 0.45),
    "radial":     (1.30, 1.00, 0.10),
    "rosette":    (0.60, 0.90, 0.08),
    "pendulous":  (1.60, 1.00, 0.15),
}
# The distal shifts are deliberately smaller than the botany alone suggests.
# Foliage really does sit at the far end of a shoot, but the twig behind it is
# drawn in bark, and bark against a dark canopy reads as a dead orange stick
# rather than as a bare shoot. At 0.75 the pine was more visible twig than
# needle. What is being modelled here is the LOOK of the arrangement, and past
# about half a segment the arrangement stops being the thing you notice.

# A habit that quietly falls back to the default is indistinguishable from one
# that works. `spire` and `ovoid` crowns rendered as spheres for months for
# exactly that reason, so the table is checked against the spec's list here, at
# import, rather than being trusted.
assert set(_HABIT) == set(_FOLIAGE_HABITS), (
    "forge/rasterize.py _HABIT and forge/spec.py _FOLIAGE_HABITS disagree: "
    f"{set(_HABIT) ^ set(_FOLIAGE_HABITS)}")


def _clustered_spacing(points: np.ndarray, base: float, strength: float,
                       rng) -> np.ndarray:
    """Required spacing per candidate: tight inside a mass, loose between them.

    A handful of centres are drawn from the twigs themselves, so the masses
    form where there is actually wood to hang them on rather than at arbitrary
    points in the crown volume. Each candidate is then scored by how close it
    is to the nearest centre, and that score pulls its required spacing below
    or above the authored one.

    The scores are turned into spacings around a mean of one, so the crown
    keeps roughly the number of clumps it would have had. This moves foliage
    about; it does not add or remove it.
    """
    n = points.shape[0]
    if strength <= 0.0 or n < 8:
        return np.full(n, base, np.float64)

    # Fewer, larger masses as the strength rises.
    centres = max(3, int(round(11 - 6.0 * strength)))
    pick = rng.choice(n, size=min(centres, n), replace=False)
    span = float(np.ptp(points, axis=0).max()) or 1.0
    scale = span * (0.34 - 0.13 * strength)

    d2 = ((points[:, None, :] - points[pick][None, :, :]) ** 2).sum(axis=2)
    near = np.sqrt(d2.min(axis=1))
    score = np.exp(-(near / max(scale, 1e-6)) ** 2)
    # Centre the score on its own mean, so the spacing it produces averages to
    # the authored one instead of drifting with however the centres landed.
    score = score - float(score.mean())
    hi = float(np.abs(score).max()) or 1.0
    return base * (1.0 - 0.75 * strength * (score / hi))


def _thin_by_distance(points: np.ndarray, min_dist, rng,
                      first: np.ndarray | None = None) -> np.ndarray:
    """Greedy dart-throwing: keep points no closer than `min_dist` to each other.

    Visits candidates in random order and accepts one only if nothing already
    accepted is within the radius, using a spatial hash so the check stays local
    rather than comparing against every accepted point.

    `min_dist` may be one distance for the whole set or one per point. Where it
    varies, a pair is judged against the LARGER of the two claims, so a point
    that wants room gets it even next to one that does not, and the test stays
    symmetric -- without that, whether two points clash would depend on which
    the shuffle happened to visit first.

    `first` marks points to offer a place BEFORE the rest, still shuffled within
    each group. Greedy dart-throwing is order-dependent by nature -- whoever
    arrives first takes the room -- so this decides who that is rather than
    leaving it to the shuffle. It changes which points survive, never how many,
    which is the whole reason it exists: exempting a class of point from the
    spacing rule instead suspends the rule, and the rule is what puts daylight
    between the masses.
    """
    per_point = np.ndim(min_dist) > 0
    dists = np.asarray(min_dist, np.float64) if per_point else None
    cell = max(float(np.max(min_dist)) if per_point else float(min_dist), 1e-6)
    buckets: dict[tuple[int, int, int], list[int]] = {}
    keep: list[int] = []
    r2 = 0.0 if per_point else float(min_dist) ** 2

    order = rng.permutation(points.shape[0])
    if first is not None:
        pri = np.asarray(first, bool)[order]
        order = np.concatenate([order[pri], order[~pri]])

    for i in order:
        p = points[i]
        cx, cy, cz = (int(np.floor(p[0] / cell)), int(np.floor(p[1] / cell)),
                      int(np.floor(p[2] / cell)))
        clash = False
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for j in buckets.get((cx + dx, cy + dy, cz + dz), ()):
                        d = points[j] - p
                        lim = r2
                        if per_point:
                            want = max(dists[i], dists[j])
                            lim = want * want
                        if float(d @ d) < lim:
                            clash = True
                            break
                    if clash:
                        break
                if clash:
                    break
            if clash:
                break
        if not clash:
            buckets.setdefault((cx, cy, cz), []).append(int(i))
            keep.append(int(i))
    return np.asarray(sorted(keep), dtype=np.int64)


def frond_blades(
    grid: VoxelGrid,
    skel: Skeleton,
    spec: dict,
    origin: np.ndarray,
    rng: np.random.Generator,
) -> int:
    """Leaf blades along each frond's midrib.

    Not clumps on twigs: the blade is widest about a third of the way out and
    tapers to a point at both ends, and it is flattened vertically because a
    palm leaf is a plane of leaflets, not a sausage. Stamping a squashed
    ellipsoid at every midrib node draws exactly that, and at voxel resolution
    a dense row of overlapping ellipsoids *is* a blade.
    """
    if not get(spec, "foliage.enabled") or skel.along is None:
        return 0

    leaf = materials.resolve(get(spec, "materials.leaf"))
    width = float(get(spec, "frond.width_m"))
    squash = float(get(spec, "foliage.squash"))
    density = float(get(spec, "foliage.density"))
    jitter = float(get(spec, "foliage.clump_jitter"))

    cand = np.flatnonzero((skel.order >= 1) & (skel.along > 0.0))
    placed = 0
    for i in cand:
        s = float(skel.along[i])
        # Peak just inboard of centre, zero at both ends.
        profile = (4.0 * s * (1.0 - s)) ** 0.55 if 0.0 < s < 1.0 else 0.0
        w = width * profile * (1.0 + jitter * (rng.random() - 0.5) * 0.5)
        r_vox = m_to_vox(w, grid.voxel_m)
        if r_vox < 0.6:
            continue
        c = m_to_vox(skel.pos[i], grid.voxel_m) - origin
        grid.blob(c, r_vox, leaf, rng, density=density, squash=squash, only_air=True)
        placed += 1
    return placed


def ground_contact(grid: VoxelGrid) -> int:
    """How many solid voxels sit on the bottom slab.

    Zero means the asset would float when placed, which is the failure a
    stamped voxel tree shows most obviously and most embarrassingly.
    """
    return int(np.count_nonzero(grid.data[:, :, 0]))
