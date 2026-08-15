"""Rocks: boulders, slabs and cobbles.

Nothing here draws a skeleton, because a rock has none. It is built the way a
rock actually reads at voxel scale — a lumpy mass with a roughened surface,
sliced by a few flat fracture planes, then part-buried.

Four steps, each one a thing you can see in the result:

1. **Mass** — a handful of ellipsoids scattered near the centre and unioned as a
   FIELD, then the surface is pushed in and out by low-frequency noise before it
   is thresholded into voxels. The noise is not a detail pass; it is the step
   that makes the thing read as stone. Unioning ellipsoids straight into voxels
   gave a perfectly smooth surface, and a smooth curved surface voxelized at
   twenty-odd voxels across shows its stair-steps as clean concentric contour
   rings — the exact look of a Minecraft sphere, and nothing like a rock.
   Breaking the surface with noise breaks the rings, and no amount of cutting or
   eroding afterwards does that.
2. **Faceting** — half-space cuts at random orientations. This is the difference
   between a river cobble and a freshly fractured block, and it is the single
   parameter that most changes what kind of stone it looks like.
3. **Erosion** — take coherent patches off the most exposed places. With the
   noise doing the heavy lifting this is now a light finishing pass.
4. **Burial** — cut everything below z=0. A boulder that sits ON the ground
   reads as dropped; one cut into looks settled.
"""

from __future__ import annotations

import math

import numpy as np

from . import materials
from .grid import VoxelGrid, ground_band, m_to_vox
from .spec import get

# Ceiling on the estimated float32 working set for one rock build, in GiB.
# `hero-arch-colossal`, the largest thing anyone has authored, comes to 19.7 on
# its worst seed; anything much past that is a spec mistake rather than an
# ambition, and it is worth saying so before the machine swaps. The estimate
# under-states the real high-water mark -- see the measurements at the guard
# itself in `_build_once` -- so this is not 20 GB of RAM.
MAX_WORKING_GB = 20.0

# How much harder the rock beside an arch's opening is than the rest of the
# stone. Not a fudge factor: load-bearing sandstone genuinely weathers slower
# (Bruthans 2014), which is why a real arch stands on legs far thinner than
# anything else on the rock. Sized by measurement -- below about 3 the legs
# still go on some seeds, and above about 6 the whole span stops weathering and
# the arch keeps the machined edge the carve gave it.
ARCH_LEG_HARDNESS = 4.5


def build(spec: dict, rng: np.random.Generator, voxel_m: float,
          *, with_rubble: bool = False) -> VoxelGrid:
    """Build a stone whose longest dimension is `rock.size_m`.

    `with_rubble` is OFF for asset builds. One generation makes ONE entity, and
    a scree ring round the foot of a stone is a second one -- several dozen
    loose stones that are not connected to the rock and never were. They are
    real scenery and they are coming back as their own species, generated at
    their own size and placed by whatever places scenery; they are not part of
    this rock. The parameter and the code that draws it are left wired up
    behind this switch rather than deleted, so moving them costs an argument
    rather than an archaeology session. See `_build_once`.

    Faceting, erosion and the burial cut all take mass away, so the raw lumps
    have to start larger than the answer. Predicting by how much from the
    parameters did not work: a formula tuned so a boulder came out right left a
    3.2 m standing stone at 4.9 m, because the burial cut removes a different
    share of a tall stone than of a flat one. So this MEASURES instead --
    build, compare, correct, at most three times. Same seed each attempt, so the
    corrections change the scale and nothing else about the stone.
    """
    target = float(get(spec, "rock.size_m"))
    seed = int(rng.integers(1 << 62))

    # Find the scale on a COARSE copy first. The correction is a ratio of
    # lengths and barely depends on the lattice, so searching for it at the
    # authored size means paying for a 9 m boulder three times over. Coarse
    # search plus one real build is the same answer for a third of the work.
    # The probe lattice is chosen so the search runs on a stone about 160
    # voxels across whatever it is being asked for. That used to be pinned at
    # 10 cm, which meant a hero got no probe at all -- the search ran at the
    # authored lattice, and since the first guess overshoots by roughly half
    # again in every direction, a 90 m arch asked for three and a half times
    # the working set of the arch it finally built. It did not fail gracefully;
    # it asked numpy for nine gigabytes.
    probe_m = max(voxel_m, 0.10, target / 160.0)
    scale = 1.0
    if probe_m > voxel_m * 1.01:
        for _ in range(3):
            probe = _build_once(spec, np.random.default_rng(seed), probe_m, scale,
                                with_rubble=with_rubble)
            got = max(_extent_m(probe))
            if got <= 0.0:
                break
            err = target / got
            if 0.95 <= err <= 1.05:
                break
            scale = min(4.0, max(0.3, scale * err))

    grid = None
    for _ in range(3 if probe_m <= voxel_m * 1.01 else 2):
        grid = _build_once(spec, np.random.default_rng(seed), voxel_m, scale,
                           with_rubble=with_rubble)
        got = max(_extent_m(grid))
        if got <= 0.0:
            break
        err = target / got
        if 0.92 <= err <= 1.08:
            break
        scale = min(4.0, max(0.3, scale * err))
    return grid


def main_piece(occ: np.ndarray) -> np.ndarray:
    """The largest connected lump. One generation, one entity.

    26-connectivity, the same as everything else here uses, so "touching at a
    corner" counts as attached -- which for a heap of boulders is the whole
    point.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return occ
    lab, n = ndimage.label(occ, structure=np.ones((3, 3, 3), bool))
    if n <= 1:
        return occ
    sizes = np.bincount(lab.ravel())
    sizes[0] = 0
    return lab == int(np.argmax(sizes))


def _extent_m(grid: VoxelGrid) -> tuple[float, float, float]:
    """How big the STONE is -- the main body, not the contents of the grid.

    `build` fits `rock.size_m` by measuring this and correcting, so whatever
    this measures is what the size slider ends up meaning. It used to measure
    every occupied voxel, which included the rubble ring scattered out to 1.2
    times the stone's half-width and anything the weathering had knocked loose.
    A high-rubble spec therefore spent its size budget on debris: `hero-tor-stack`
    asked for 17 m and got 9.8 m of stone inside a 17 m circle of scree, and the
    workaround was to cut its rubble to 0.3 -- which is a spec being detuned to
    correct a measurement error, and it is baked into several specs now.

    Measuring the main body is the fix, and it stays the fix even with the
    rubble ring switched off: a block that weathering knocks off the top and
    `_seat` drops onto the ground beside the stone would otherwise widen the
    thing being measured just as effectively.
    """
    occ = grid.data != 0
    if not occ.any():
        return (0.0, 0.0, 0.0)
    xs, ys, zs = np.nonzero(main_piece(occ))
    if xs.size == 0:
        return (0.0, 0.0, 0.0)
    return tuple(float(np.ptp(a) + 1) * grid.voxel_m for a in (xs, ys, zs))


def _build_once(spec: dict, rng: np.random.Generator, voxel_m: float,
                scale_hint: float, *, with_rubble: bool = False) -> VoxelGrid:
    size = float(get(spec, "rock.size_m")) * scale_hint
    lumps = int(get(spec, "rock.lumps"))
    spread = float(get(spec, "rock.spread"))
    flatten = float(get(spec, "rock.flatten"))
    elongate = float(get(spec, "rock.elongate"))
    angular = float(get(spec, "rock.angular"))
    facets = int(get(spec, "rock.facets"))
    erode = float(get(spec, "rock.erode"))
    cavernous = float(get(spec, "rock.cavernous"))
    bedding = float(get(spec, "rock.bedding"))
    bed_thick = float(get(spec, "rock.bed_thickness_m"))
    bed_dip = float(get(spec, "rock.bed_dip_deg"))
    rough = float(get(spec, "rock.rough"))
    joint_sets = int(get(spec, "rock.joint_sets"))
    joint_scatter = float(get(spec, "rock.joint_scatter"))
    joint_dip = float(get(spec, "rock.joint_dip_deg"))
    block_size = float(get(spec, "rock.block_size_m"))
    block_relief = float(get(spec, "rock.block_relief_m"))
    columns = int(get(spec, "rock.columns"))
    column_gap = float(get(spec, "rock.column_gap_m"))
    column_stagger = float(get(spec, "rock.column_stagger"))
    exfoliate = float(get(spec, "rock.exfoliate"))
    shell_m = float(get(spec, "rock.shell_m"))
    bury = float(get(spec, "rock.bury"))
    rubble = float(get(spec, "rock.rubble"))
    cross_beds = int(get(spec, "rock.cross_beds"))
    caprock = float(get(spec, "rock.caprock"))
    cap_frac = float(get(spec, "rock.cap_frac"))
    notch = float(get(spec, "rock.notch"))
    notch_z = float(get(spec, "rock.notch_z_m"))
    notch_spread = float(get(spec, "rock.notch_spread_m"))
    aspect = float(get(spec, "rock.aspect"))
    joint_taper = float(get(spec, "rock.joint_taper"))
    corestone = float(get(spec, "rock.corestone"))
    settle = float(get(spec, "rock.settle_m"))
    veins = int(get(spec, "rock.veins"))
    vein_width = float(get(spec, "rock.vein_width_m"))
    vein_hardness = float(get(spec, "rock.vein_hardness"))
    rind = float(get(spec, "rock.rind"))
    rind_m = float(get(spec, "rock.rind_m"))
    clasts = int(get(spec, "rock.clasts"))
    clast_size = float(get(spec, "rock.clast_size_m"))
    clast_hardness = float(get(spec, "rock.clast_hardness"))
    flutes = float(get(spec, "rock.flutes"))
    flute_width = float(get(spec, "rock.flute_width_m"))
    pans = float(get(spec, "rock.pans"))
    pan_depth = float(get(spec, "rock.pan_depth_m"))
    arch = float(get(spec, "rock.arch"))
    mat = materials.resolve(get(spec, "materials.rock"))

    # Half-extents in metres, then voxels. The grid gets a margin for rubble
    # and for the erosion pass to work against.
    hx = size * 0.5 * elongate
    hy = size * 0.5 / max(elongate, 0.2) ** 0.5
    hz = size * 0.5 * flatten
    # The margin exists to hold the rubble ring. With the ring switched off it
    # is 40% of empty grid in x and y, on the two axes a stretched blank is
    # already largest in -- and the working-set guard is quadratic in it.
    # `hero-arch-colossal` seed 2 did not build at all for this reason: at
    # `elongate` 2.4 and `rubble` 0.25 it asked for a 1738x470x1294 grid, over
    # the limit. Without the ring the same stone is half that and builds. A
    # hero that throws on some seeds is not a hero.
    margin = 1.0 + (rubble * 1.6 if with_rubble else 0.0)

    nx = max(4, int(m_to_vox(hx * 2 * margin, voxel_m)) + 4)
    ny = max(4, int(m_to_vox(hy * 2 * margin, voxel_m)) + 4)
    nz = max(4, int(m_to_vox(hz * 2 + size * 0.2, voxel_m)) + 4)

    # Rocks had no allocation guard, so an over-ambitious size came back as a
    # raw numpy MemoryError from whichever temporary happened to be unlucky --
    # which points at a line of arithmetic rather than at the spec that caused
    # it. This counts the whole-grid float32 arrays live at the peak: `field`,
    # `relief`, the durability field, and a couple of temporaries inside
    # whichever weathering term is running. It was six until the coordinate
    # meshgrids became open arrays and three of them went away; see `_axes`.
    #
    # IT IS A TRIPWIRE, NOT A MEMORY BUDGET, and the difference has bitten
    # before, so: measured peak committed memory against this estimate, seed 1
    # unless noted, minus the 0.75 GB the interpreter costs on its own --
    #
    #     hero-arch-colossal s2  1738x470x1294  est 19.7 GB  real 24.4  1.2x
    #     hero-tor-stack          396x302x625   est  1.4 GB  real  3.8  2.7x
    #     hero-natural-arch       476x116x349   est  0.4 GB  real  1.0  2.7x
    #     hero-balanced-rock      214x186x366   est  0.3 GB  real  0.9  3.5x
    #
    # It under-states, by between 1.2x and 3.5x, and no single factor fits:
    # what dominates the peak changes with the spec. scipy's labelling and
    # distance transforms allocate int32 and float64 copies, the size-fitting
    # loop in `build` runs the whole thing more than once, and a stone that
    # skips erosion never allocates half of this. So the guard is set where an
    # absurd spec trips it and every authored hero clears it, and nothing
    # downstream should read `gb` as a promise about RAM.
    #
    # Note the grids above are the SIZE-FITTED ones, which is what this line
    # actually sees -- `build` corrects the requested size by measurement, and
    # `hero-natural-arch` settles at 0.45 of its authored blank. Estimating
    # from `rock.size_m` instead gives numbers 5-10x too big.
    gb = nx * ny * nz * 4 * 5 / 2 ** 30
    if gb > MAX_WORKING_GB:
        raise MemoryError(
            f"rock.size_m {size:.1f} at {voxel_m * 100:.0f} cm needs a "
            f"{nx}x{ny}x{nz} grid, about {gb:.1f} GB of working set (limit "
            f"{MAX_WORKING_GB:.0f} GB). Coarsen resolution_cm or reduce size.")

    grid = VoxelGrid((nx, ny, nz), (0, 0, 0), voxel_m)
    cx, cy = nx / 2.0, ny / 2.0
    # Sink the body so `bury` of its height falls below the cut at z=0.
    cz = m_to_vox(hz, voxel_m) * (1.0 - 2.0 * bury) + 2.0

    # 1. mass ----------------------------------------------------------------
    # Union the lumps as a field rather than as voxels, so the surface can be
    # displaced before it is thresholded. Values are in units of "fraction of
    # the local radius": 0 is the surface, positive is inside.
    gx, gy, gz = _axes(nx, ny, nz)
    field = np.full((nx, ny, nz), -9.0, np.float32)
    for i in range(max(1, lumps)):
        if i == 0:
            off = np.zeros(3)
            scale = 1.0
        else:
            off = rng.normal(scale=spread * 0.45, size=3) * np.array([hx, hy, hz])
            scale = 0.45 + 0.55 * rng.random()
        # float32, not float64. The meshgrids are float32 already, but a
        # float64 centre promotes every expression they appear in back to
        # float64 -- and these are whole-grid temporaries. On a 26 m stone
        # at 5 cm that is a 9 GB allocation per array for a grid that is
        # 1.2 GB as voxels, which is how the hero batch died.
        c = (np.array([cx, cy, cz]) + m_to_vox(off, voxel_m)).astype(np.float32)
        rx = max(m_to_vox(hx * scale, voxel_m), 0.6)
        ry = max(m_to_vox(hy * scale, voxel_m), 0.6)
        rz = max(m_to_vox(hz * scale, voxel_m), 0.6)
        q = (((gx - c[0]) / rx) ** 2 + ((gy - c[1]) / ry) ** 2
             + ((gz - c[2]) / rz) ** 2)
        np.maximum(field, 1.0 - np.sqrt(q), out=field)

    relief = _surface_noise((nx, ny, nz), 1.0, int(rng.integers(1 << 30)))
    if rough > 0.0:
        field += relief * rough

    grid.data[field > 0.0] = mat

    # Joint orientations, shared by the faceting and the block relief so every
    # fracture on one rock belongs to the same small family of directions.
    sets_all = joint_frame(rng, joint_dip) if joint_sets >= 2 else None
    sets = sets_all[:joint_sets] if sets_all is not None else None

    # 2. faceting ------------------------------------------------------------
    if angular > 0.0 and facets > 0:
        # A fracture face is not a plane. Cutting with a true half-space gave
        # perfectly flat faces, and on a large rock those faces are metres
        # across -- the first 5-9 m boulders came out reading as cut gemstones
        # rather than stone. Wobbling the cut turns each face into a broken
        # surface.
        #
        # The SAME field does both jobs. Generating a second one doubled the
        # slowest part of the build for a difference nobody could see, and a
        # fracture following the same grain as the weathering is if anything the
        # more physical of the two.
        _facet(grid, rng, facets, angular,
               relief * (max(1.5, min(nx, ny, nz) * 0.10) / 0.55),
               sets=sets, scatter=joint_scatter)

    # 2b. jointing -----------------------------------------------------------
    # The plinth the taper below promises, made before the joints cut, so every
    # blade they carve comes down onto rock instead of onto the lens's hollow.
    # Gated on `block_relief`, not on the taper. Any mass about to be fractured
    # needs its foot to be rock: a lens-shaped blank's rim curves up away from
    # the ground, so the blocks the joints cut out of it stand on the hollow
    # under the lens rather than on anything. `karren-pavement` has no taper and
    # was shedding 29-62% of itself across seeds 1-3 for exactly that reason --
    # a limestone pavement is clints and grikes cut into continuous bedrock, and
    # it had no bedrock. The gate that matters is still there: a stone with no
    # jointing at all never comes here, so `hero-balanced-rock`'s undercut is
    # not at risk.
    if sets is not None and block_relief > 0.0:
        _plinth(grid)
        # NOT `sets`: only the steep planes may be opened into a gap, or the
        # bedding plane saws the stone into floating slices. See `_opening_sets`.
        # `sets_all` rather than `sets`, so dropping the bedding plane promotes
        # the third set instead of silently halving how many joints open.
        _block_relief(grid, gx, gy, gz, _opening_sets(sets_all, joint_sets),
                      m_to_vox(block_size, voxel_m),
                      m_to_vox(block_relief, voxel_m), int(rng.integers(1 << 30)),
                      taper=joint_taper)

    # 2b'. corestones --------------------------------------------------------
    # Shares the joint frame with the stage above, and reads the same distance
    # to the nearest joint plane, so it costs almost nothing on top of it.
    if sets is not None and corestone > 0.0:
        _corestones(grid, gx, gy, gz, sets, m_to_vox(block_size, voxel_m),
                    corestone, int(rng.integers(1 << 30)))

    # 2b''. settle -----------------------------------------------------------
    # After the joints are open and the cores are loose, and before anything
    # measures the silhouette.
    if settle > 0.0:
        _settle(grid, m_to_vox(settle, voxel_m), int(rng.integers(1 << 30)))

    # 2c. columns ------------------------------------------------------------
    if columns >= 2:
        _columns(grid, rng, columns, m_to_vox(column_gap, voxel_m), column_stagger)

    # 2d. arch ---------------------------------------------------------------
    # Before weathering, deliberately: the underside of a span is the smoothest
    # surface on the whole rock, and the way it gets that way is by being
    # weathered after the hole is there.
    # Gated on the AUTHORED size, not on `size`, which carries the correction
    # factor from the fitting loop in `build`. Reading the scaled one meant the
    # arch worked on the first attempt, the loop then scaled the stone down to
    # hit its target, and every attempt after that fell under the threshold and
    # silently skipped the carve -- so the slider did nothing at all while
    # looking like it was wired up.
    arch_out: dict = {}
    if arch > 0.0 and float(get(spec, "rock.size_m")) >= 4.0:
        _arch(grid, rng, arch, out=arch_out)

    # 3. weathering ----------------------------------------------------------
    if erode > 0.0:
        durability = _durability(
            (nx, ny, nz), gx, gy, gz, field, rng,
            bedding=bedding, bed_thick_vox=m_to_vox(bed_thick, voxel_m),
            bed_dip=bed_dip, cross_beds=cross_beds,
            caprock=caprock, cap_frac=cap_frac,
            veins=veins, vein_half_vox=m_to_vox(vein_width * 0.5, voxel_m),
            vein_hardness=vein_hardness, relief=relief,
            rind=rind, rind_frac=rind_m / max(size * 0.5, 1e-3),
            clasts=clasts, clast_r_vox=m_to_vox(clast_size * 0.5, voxel_m),
            clast_hardness=clast_hardness, sets=sets,
            salt=int(rng.integers(1 << 30)))
        # An arch's legs are the load path and load-bearing rock weathers
        # slower; without this the carve opens a hole and erosion closes it.
        # See `_arch`.
        lp = arch_out.get("load_path")
        if lp is not None and durability is not None:
            durability = durability.copy()
            durability[lp] *= ARCH_LEG_HARDNESS
            np.clip(durability, 0.05, 12.0, out=durability)
        elif lp is not None:
            durability = np.where(lp, np.float32(ARCH_LEG_HARDNESS),
                                  np.float32(1.0))
        _erode(grid, erode, cavernous, durability, voxel_m=voxel_m,
               notch=(notch, notch_z, notch_spread) if notch > 0.0 else None,
               aspect=(aspect, _unit(rng.normal(size=3) * [1.0, 1.0, 0.35]))
               if aspect > 0.0 else None)

    # 3b. exfoliation --------------------------------------------------------
    if exfoliate > 0.0:
        _exfoliate(grid, exfoliate, m_to_vox(shell_m, voxel_m),
                   int(rng.integers(1 << 30)))

    # 3c. running water ------------------------------------------------------
    if flutes > 0.0 or pans > 0.0:
        _flow(grid, flutes, m_to_vox(flute_width, voxel_m) * 0.5,
              pans, m_to_vox(pan_depth, voxel_m), int(rng.integers(1 << 30)))

    # 4. seat the loose blocks ------------------------------------------------
    # Last, after everything that removes mass. See `_seat`: settling before the
    # weathering pass is not enough, because weathering then eats the contacts
    # the settle made.
    _seat(grid)

    # 5. rubble around the base ----------------------------------------------
    # OFF for asset builds -- see `build`. This ring is a second entity: a few
    # dozen stones that never touch the rock and cannot, because they are drawn
    # from 0.55 to 1.2 times its half-width out from the centre. Scree is its
    # own species now.
    #
    # Not deleted, because the drawing is the only description of what that
    # scree should look like round a given stone and it will be wanted when the
    # rubble asset is written. Not left running either: it is behind a switch
    # that asset builds pass as False, so it either draws stones or is not
    # called, and never runs while doing nothing.
    if with_rubble and rubble > 0.0:
        count = int(rubble * 14)
        for _ in range(count):
            ang = rng.random() * 2.0 * math.pi
            dist = (0.55 + 0.65 * rng.random()) * max(hx, hy)
            r = size * (0.04 + 0.10 * rng.random())
            c = np.array([cx + m_to_vox(math.cos(ang) * dist, voxel_m),
                          cy + m_to_vox(math.sin(ang) * dist, voxel_m),
                          2.0])
            rv = m_to_vox(r, voxel_m)
            _ellipsoid(grid, c, rv, rv, rv * 0.7, mat)

    return grid


def _axes(nx: int, ny: int, nz: int, start=(0.0, 0.0, 0.0), half: float = 0.5):
    """Voxel coordinates as three OPEN arrays, not three full grids.

    Every stage that needs to know where a voxel is uses these the same way:
    somewhere in an expression that ends up whole-grid-shaped anyway, like
    `gx * n[0] + gy * n[1] + gz * n[2]`. Nobody indexes them, reshapes them or
    masks them. So they never needed to BE whole grids -- shapes (nx,1,1),
    (1,ny,1) and (1,1,nz) broadcast to exactly the same result and cost
    nx+ny+nz floats instead of 3*nx*ny*nz.

    That is not a micro-optimisation at this scale. `hero-arch-colossal` seed 2
    asked for a 1738x470x1294 grid: 14 KB of coordinates as open arrays,
    12.7 GB as meshgrids -- more than half of the estimated working set that
    put the stone over the allocation guard and made it fail to build at all.
    It builds now: 39.6 M voxels in 12 minutes, measured peak 24 GB.

    The one thing to watch: an expression that touches only SOME of the three
    stays open-shaped. `_bedding` builds its band from gz and gx alone and so
    comes back (nx,1,nz), and `_durability`'s caprock term is (1,1,nz) on its
    own. Anything that then in-place-adds a whole-grid array to it, or that
    hands it to a caller which will slice it by bounding box, has to broadcast
    it out first. Both places are marked.

    `start` and `half` exist so this reproduces both conventions already in the
    file exactly: the mass builder works in voxel CENTRES from the grid origin
    (`half=0.5`), and `_facet` works in whole indices offset to a bounding box
    (`start=box, half=0.0`). Same numbers as the meshgrids they replaced, so
    the builds stay byte-identical.
    """
    return (np.arange(nx, dtype=np.float32).reshape(nx, 1, 1) + (half + start[0]),
            np.arange(ny, dtype=np.float32).reshape(1, ny, 1) + (half + start[1]),
            np.arange(nz, dtype=np.float32).reshape(1, 1, nz) + (half + start[2]))


def joint_frame(rng, dip_deg: float) -> np.ndarray:
    """Three mutually perpendicular joint orientations for one rock.

    Real rock does not fracture in random directions. It fractures along a small
    number of JOINT SETS -- typically a bedding plane plus two near-vertical
    sets roughly at right angles -- and every block in an outcrop shares them.
    That shared orientation is what makes jointed granite look quarried rather
    than merely lumpy, and drawing each facet normal independently at random
    could never produce it, because the whole signal is the correlation between
    faces.

    Returns the three unit normals as rows.
    """
    dip = math.radians(dip_deg)
    az = rng.random() * 2.0 * math.pi
    n0 = np.array([math.sin(dip) * math.cos(az), math.sin(dip) * math.sin(az),
                   math.cos(dip)])
    t = np.array([math.cos(az + math.pi * 0.5), math.sin(az + math.pi * 0.5), 0.0])
    n1 = t - n0 * float(np.dot(t, n0))
    n1 /= max(float(np.linalg.norm(n1)), 1e-9)
    n2 = np.cross(n0, n1)
    return np.stack([n0, n1, n2])


def _opening_sets(sets: np.ndarray, count: int) -> np.ndarray:
    """The joint planes that may be OPENED into a gap, most vertical first.

    `joint_frame` returns the bedding plane first and the two near-vertical sets
    after it, and every caller used to take them in order -- so a spec asking
    for two joint sets at dip 0 got the bedding plane plus one vertical set, and
    `_block_relief` sawed the mass into horizontal wafers. Everything above the
    lowest wafer then stood on a 10 cm gap of air. Measured on
    `limestone-pinnacles`: 67-69% of the asset airborne across seeds 1-3, in
    slabs one block thick at regular heights, each with solid rock 10-30 cm
    below 94-100% of its footprint. That is not a floating rock, it is a rock
    cut into floating slices.

    `hero-tsingy-pinnacles` worked around it in the spec by authoring
    `joint_dip_deg` 90, which tips the whole frame so that the first two sets
    come out vertical. Its comment in `tools/seed_heroes.py` names the mechanism
    exactly. That is a workaround on one spec for a rule that belongs here, and
    the specs that did not know to apply it are the ones that float.

    So: a joint plane is opened only if it is STEEP. The physical reading is the
    honest one -- a bedding parting is held shut by the weight of the rock
    standing on it, while a vertical joint is where the water goes and is what
    gets washed open into a grike. A bedding plane still shapes the stone; it
    does it through `bedding` and the weathering pass, which cuts shelves into
    the rock instead of cutting it into slices.

    Three mutually perpendicular normals can have at most one with a large z
    component, so at least two always survive the filter and a spec asking for
    two open sets always gets two.
    """
    steep = [n for n in sets if abs(float(n[2])) < 0.7]
    if len(steep) < 1:                      # cannot happen for an orthogonal frame
        return sets[:count]
    return np.stack(steep[:count])


_SNAP_COS = 0.985   # about 10 degrees


def _snap_axis(n: np.ndarray) -> np.ndarray:
    """Pull a nearly-axis-aligned normal onto the axis.

    A cut plane a few degrees off an axis is the worst case for a voxel grid: it
    lands as a long shallow staircase of parallel ridges rather than a face.
    That artifact was reading as machining marks across the top of every rock,
    and it survived every attempt to fix it by changing the noise, because the
    noise was never what caused it. On the axis the same cut is one clean plane.
    """
    for a in range(3):
        if abs(n[a]) >= _SNAP_COS:
            out = np.zeros(3)
            out[a] = math.copysign(1.0, n[a])
            return out
    return n


def _surface_noise(shape, rough: float, salt: int) -> np.ndarray:
    """Surface displacement, in units of the local radius.

    Three octaves: two sized to the rock, which set the silhouette, and one
    sized to the voxel, which breaks up the stair-stepping. See the comment on
    the octave table below for why the third is not optional.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return np.zeros(shape, np.float32)

    span = max(shape)
    out = np.zeros(shape, np.float32)

    # Two octaves scaled to the ROCK for silhouette, and one scaled to the
    # VOXEL for surface break-up.
    #
    # The rock-relative pair alone is size-dependent in a way that only shows up
    # on large stones: their wavelength is a fixed fraction of the body, so a
    # 9 m boulder gets the same six undulations a 2 m one does, and between
    # those six the surface is still a smooth curve showing clean concentric
    # stair-steps. The whole point of this pass is to stop that happening, and
    # on the first large boulders it did not, because the relief had grown with
    # the rock while the terracing had not. A third octave a couple of voxels
    # wide fixes it at every size, since terracing is a property of the lattice.
    octaves = [(max(0.9, span * 0.16), 0.62), (max(0.9, span * 0.075), 0.28),
               (1.7, 0.22)]
    for k, (sigma, weight) in enumerate(octaves):
        # Build the coarse octaves SMALL and stretch them, rather than blurring
        # at full size. Their features are a sixth of the rock wide by
        # construction, so nothing in the full-resolution field survives the
        # blur anyway -- and blurring ten million cells with a sigma of thirty
        # is most of a minute. A 9 m tor went from 41 seconds to nine on this.
        # The voxel-scale octave has a small sigma already and stays full size.
        step = max(1, int(sigma / 3.0))
        small = tuple(max(4, s // step) for s in shape)
        n = ndimage.gaussian_filter(
            rng_field(small, salt ^ (k * 7919)).astype(np.float32),
            sigma=sigma / step)
        # Blurring collapses the range, so rescale each octave to [-1, 1] rather
        # than assuming it kept one.
        lo, hi = float(n.min()), float(n.max())
        n = (n - lo) / max(hi - lo, 1e-9) * 2.0 - 1.0
        if small != shape:
            n = ndimage.zoom(n, [shape[i] / small[i] for i in range(3)], order=1)
            # zoom lands within a voxel of the target; trim or edge-extend so the
            # field is exactly grid-shaped.
            n = _fit(n, shape)
        out += n * weight
    return out * (rough * 0.55)


def _unit(v) -> np.ndarray:
    v = np.asarray(v, np.float64)
    return v / max(float(np.linalg.norm(v)), 1e-9)


def _norm01(a: np.ndarray) -> np.ndarray:
    lo, hi = float(a.min()), float(a.max())
    return ((a - lo) / max(hi - lo, 1e-9)).astype(np.float32)


def _durability(shape, gx, gy, gz, field, rng, *, bedding, bed_thick_vox,
                bed_dip, cross_beds, caprock, cap_frac, veins, vein_half_vox,
                vein_hardness, relief, rind, rind_frac, clasts, clast_r_vox,
                clast_hardness, sets, salt):
    """How well each voxel resists weathering. 1.0 is ordinary rock.

    This started as bedding alone and has become the place where every "some
    parts of this stone are harder than others" mechanism lands, because they
    all end up saying the same thing to the erosion pass and they multiply
    together cleanly. What differs is only how the hard parts are SHAPED:

    - **beds** — parallel layers, the sedimentary default
    - **cross-bed sets** — wedges of steep layers, each truncated by the next
    - **a caprock** — one hard mass on top, which is what a hoodoo needs
    - **veins** — thin sheets at their own angle, which stand out as fins
    - **a rind** — a hard skin just under the original surface, which is what
      gives a tafoni hollow its overhanging lip
    - **clasts** — separate lumps in a softer matrix: breccia

    Returns None when nothing is set, so the erosion pass can skip the lookup
    entirely rather than multiply by an array of ones.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return None

    dur = None

    def mix(term):
        nonlocal dur
        dur = term if dur is None else dur * term

    if cross_beds >= 2:
        mix(_cross_beds(shape, gx, gy, gz, bedding if bedding > 0 else 0.6,
                        bed_thick_vox, cross_beds, rng))
    elif bedding > 0.0:
        mix(_bedding(shape, gx, gy, gz, bedding, bed_thick_vox, bed_dip,
                     salt ^ 0x11))

    if caprock > 0.0:
        # Where the solid actually stops, not where the grid does -- the grid
        # carries margin for rubble, and a cap measured against that would sit
        # in the air above a short stone.
        occ_z = np.flatnonzero((field > 0.0).any(axis=(0, 1)))
        if occ_z.size:
            z0, z1 = float(occ_z[0]), float(occ_z[-1])
            cut = z0 + (z1 - z0) * cap_frac
            # A soft edge, so the cap does not read as a sawn lid.
            soft = max(1.5, (z1 - z0) * 0.05)
            mix(1.0 + caprock * 3.0 / (1.0 + np.exp(-(gz - cut) / soft)))

    for i in range(max(0, veins)):
        # Veins often follow the joints, so borrow the frame when there is one.
        if sets is not None and rng.random() < 0.7:
            n = _unit(sets[rng.integers(len(sets))] + rng.normal(size=3) * 0.12)
        else:
            n = _unit(rng.normal(size=3))
        t = gx * n[0] + gy * n[1] + gz * n[2]
        # The same relief field the facets wobble with: a vein is a fracture
        # fill, so it should wander with the same grain the fractures do.
        t = t - relief * max(2.0, vein_half_vox * 3.0)
        centre = float(np.quantile(t[field > 0.0], rng.random() * 0.7 + 0.15)) \
            if (field > 0.0).any() else 0.0
        mix(np.where(np.abs(t - centre) < max(vein_half_vox, 0.7),
                     np.float32(vein_hardness), np.float32(1.0)))

    if rind > 0.0:
        # `field` is already a signed distance in units of the local radius, so
        # "just under the original surface" is free -- and it is the ORIGINAL
        # surface that matters, since the skin formed before any of this
        # weathering happened.
        skin = (field > 0.0) & (field < max(rind_frac, 0.005))
        term = np.where(skin, np.float32(1.0 + rind * 7.0), np.float32(1.0))
        # Punch holes in it. An unbroken skin never fails, so nothing ever
        # hollows out and the whole mechanism does nothing at all -- which is
        # the opposite of what it is for.
        patch = _norm01(ndimage.gaussian_filter(
            rng_field(shape, salt ^ 0x9ead).astype(np.float32),
            sigma=max(2.0, max(shape) * 0.05)))
        term[skin & (patch > 0.72)] = 1.0
        mix(term)

    if clasts > 0 and clast_r_vox >= 1.0:
        mix(_clasts(shape, field, rng, clasts, clast_r_vox, clast_hardness, sets))

    if dur is None:
        return None
    # WHOLE-GRID SHAPE, always. `_erode` slices this by the solid's bounding
    # box, and a term built from only one axis stays open-shaped -- a caprock
    # on its own is (1,1,nz), and slicing that by an x-range gives an empty
    # array rather than an error. Broadcasting before the clip costs one
    # allocation that was going to happen anyway.
    return np.clip(np.broadcast_to(dur, shape), 0.05, 12.0).astype(np.float32)


def _clasts(shape, field, rng, count: int, r_vox: float, hardness: float,
            sets) -> np.ndarray:
    """Lumps of a different rock set in a softer matrix: breccia.

    Splatted one at a time into the durability field rather than solved as a
    tessellation, because nothing here needs to know the distance to the
    nearest clast -- only where the clasts themselves are. That makes the cost
    proportional to how much clast there is, not to how big the grid is.

    A share of them are made SOFTER than the matrix instead of harder. Real
    breccia is a jumble of whatever the fault or the collapse broke up, so some
    fragments rot faster than what surrounds them and drop out. The empty
    sockets they leave are half of what the texture reads as; clasts that only
    ever stand proud look like a rash.
    """
    out = np.ones(shape, np.float32)
    inside = np.argwhere(field > 0.0)
    if not len(inside):
        return out
    nx, ny, nz = shape
    for _ in range(count):
        c = inside[rng.integers(len(inside))].astype(np.float64)
        # Lognormal-ish spread: a few big fragments among many small ones, which
        # is what a size distribution from fracturing actually looks like.
        r = r_vox * float(np.exp(rng.normal(scale=0.45)))
        r = float(np.clip(r, 1.0, r_vox * 3.0))
        rx, ry, rz = r, r * (0.6 + 0.5 * rng.random()), r * (0.6 + 0.5 * rng.random())
        x0, x1 = max(0, int(c[0] - rx) - 1), min(nx, int(c[0] + rx) + 2)
        y0, y1 = max(0, int(c[1] - ry) - 1), min(ny, int(c[1] + ry) + 2)
        z0, z1 = max(0, int(c[2] - rz) - 1), min(nz, int(c[2] + rz) + 2)
        if x0 >= x1 or y0 >= y1 or z0 >= z1:
            continue
        xs = (np.arange(x0, x1) + 0.5 - c[0]) / rx
        ys = (np.arange(y0, y1) + 0.5 - c[1]) / ry
        zs = (np.arange(z0, z1) + 0.5 - c[2]) / rz
        q = (xs[:, None, None] ** 2 + ys[None, :, None] ** 2
             + zs[None, None, :] ** 2)
        blob = q <= 1.0
        if sets is not None:
            # Angular fragments, cut on the parent rock's joints -- a breccia
            # clast inherits the fractures of whatever it broke off.
            for _ in range(3):
                n = sets[rng.integers(len(sets))] * (1.0 if rng.random() < 0.5 else -1.0)
                d = ((xs[:, None, None] * rx) * n[0] + (ys[None, :, None] * ry) * n[1]
                     + (zs[None, None, :] * rz) * n[2])
                blob &= d < r * (0.35 + 0.5 * rng.random())
        # About a third weather OUT rather than proud.
        out[x0:x1, y0:y1, z0:z1][blob] = (
            hardness if rng.random() > 0.34 else 0.45)
    return out


def _cross_beds(shape, gx, gy, gz, strength: float, thickness_vox: float,
                count: int, rng) -> np.ndarray:
    """Stacked wedges of steeply dipping layers, each cut off by the next.

    A dune migrates, dumps sand down its lee face at the angle of repose, and
    is then planed off across the top by the dune following it. So the rock is
    a stack of SETS: inside one set the laminae dip steeply downwind, and
    between sets there is a low-angle surface that truncates them. The wind
    shifts between sets, so each one dips a different way and the grain of one
    runs visibly across the grain of the next.

    That truncation is the entire signature and is exactly what ordinary
    bedding cannot produce, because it keeps every layer parallel to every
    other layer all the way through the stone.

    One deliberate cheat: the layer spacing is kept at the authored bed
    thickness rather than at a true aeolian lamina, which is millimetres. At
    5 cm voxels a real lamina is invisible; what does read is a bundle of them
    weathering as a unit, so that is what this draws.
    """
    nz = shape[2]
    dur = np.ones(shape, np.float32)
    lo = 0.0
    for k in range(count):
        # Bounding surfaces climb the stone, with a little wander so they are
        # not a stack of perfect shelves.
        hi = (k + 1) / count * nz + (rng.random() - 0.5) * nz * 0.08
        tilt = (gx * (rng.random() - 0.5) + gy * (rng.random() - 0.5)) * 0.08
        in_set = (gz >= lo + tilt) & (gz < hi + tilt)
        if not in_set.any():
            lo = hi
            continue
        # Angle of repose, not a free parameter. A shallow foreset is just
        # tilted bedding and loses the whole effect.
        dip = math.radians(25.0 + 9.0 * rng.random())
        az = rng.random() * 2.0 * math.pi
        t = (gz * math.cos(dip) + gx * math.sin(dip) * math.cos(az)
             + gy * math.sin(dip) * math.sin(az)) / max(thickness_vox, 1.0)
        band = np.sin(t * 2.0 * math.pi).astype(np.float32)
        dur = np.where(in_set, 1.0 + band * strength * 0.85, dur)
        lo = hi
    return dur.astype(np.float32)


def _bedding(shape, gx, gy, gz, strength: float, thickness_vox: float,
             dip_deg: float, salt: int):
    """Alternating hard and soft beds, as a per-voxel durability multiplier.

    Sedimentary rock is laid down in layers of differing hardness, and that
    single fact is responsible for most of the rock shapes people recognise.
    Weather a uniform block and you get a rounded lump whatever else you do to
    it; weather a layered one and the soft beds retreat while the hard ones
    stand proud, which is where banded cliffs, undercut pedestals, mushroom
    rocks and the capstone on a hoodoo all come from.

    The layers are planar with a dip, because bedding is deposited flat and
    then tilted, and modulated by noise so the boundaries are not perfect
    stripes. Returns 1.0 everywhere when strength is 0.
    """
    if strength <= 0.0:
        return None
    try:
        from scipy import ndimage
    except ImportError:
        return None

    dip = math.radians(dip_deg)
    # Distance along the bedding normal: vertical, tilted by the dip.
    t = (gz * math.cos(dip) + gx * math.sin(dip)) / max(thickness_vox, 1.0)
    band = np.sin(t * 2.0 * math.pi).astype(np.float32)

    # Roughen the boundary so beds are not laboratory stripes.
    jitter = ndimage.gaussian_filter(
        rng_field(shape, salt ^ 0x5eed).astype(np.float32),
        sigma=max(1.0, thickness_vox * 0.6))
    lo, hi = float(jitter.min()), float(jitter.max())
    # NOT `band +=`. `band` comes from gz and gx only, so with open coordinate
    # axes it is (nx,1,nz) while `jitter` is the whole grid -- an in-place add
    # cannot broadcast the result back into the smaller left-hand side and
    # raises. Out-of-place is also where the array stops being open, which is
    # what the caller needs.
    band = band + ((jitter - lo) / max(hi - lo, 1e-9) * 2.0 - 1.0) * 0.55

    # 1 +/- strength, so a hard bed resists and a soft bed goes first.
    return np.clip(1.0 + np.clip(band, -1.0, 1.0) * strength * 0.85, 0.05, 2.0)


def _columns(grid: VoxelGrid, rng, count: int, gap_vox: float,
             stagger: float) -> None:
    """Split the mass into vertical columns: basalt.

    Cooling lava contracts and cracks into a polygonal network which then
    propagates downward, giving the near-hexagonal columns of the Giant's
    Causeway. The literature generates that network as a Voronoi tessellation of
    seed points, somewhere between a random and a centroidal one -- fully
    centroidal comes out too regular, purely random too irregular.

    Here the network is 2D and extruded, which is what the columns are, and the
    crack is found by the standard F2-F1 test: a voxel lies on a cell boundary
    when the two nearest seeds are nearly equidistant. Columns also get their
    own top heights, because a colonnade with one flat top reads as a extruded
    shape rather than as stone.
    """
    nx, ny, nz = grid.shape
    if count < 2 or nz < 4:
        return

    # Jittered lattice, not uniform random: a Poisson-like spread is what puts
    # the cell statistics near real columnar rock. Fully random seeds clump and
    # give a few huge columns beside slivers.
    per = max(2, int(math.sqrt(count)))
    step_x, step_y = nx / per, ny / per
    seeds = []
    for i in range(per):
        for j in range(per):
            seeds.append(((i + 0.5 + (rng.random() - 0.5) * 0.55) * step_x,
                          (j + 0.5 + (rng.random() - 0.5) * 0.55) * step_y))
    seeds = np.asarray(seeds, np.float32)

    xs = np.arange(nx, dtype=np.float32)[:, None] + 0.5
    ys = np.arange(ny, dtype=np.float32)[None, :] + 0.5
    d = np.stack([np.hypot(xs - s[0], ys - s[1]) for s in seeds])   # (n, nx, ny)

    order = np.argsort(d, axis=0)
    nearest = np.take_along_axis(d, order[:2], axis=0)
    edge = (nearest[1] - nearest[0]) < gap_vox                       # (nx, ny)

    # Per-column top height, so the colonnade is not sawn flat.
    tops = rng.random(len(seeds))
    top_map = np.take_along_axis(
        np.asarray(tops, np.float32)[:, None, None] * np.ones((1, nx, ny), np.float32),
        order[:1], axis=0)[0]
    occ = grid.data != 0
    zs = np.arange(nz, dtype=np.float32)[None, None, :]
    solid_top = np.where(occ.any(axis=2), occ.shape[2] - 1 - np.argmax(
        occ[:, :, ::-1], axis=2), 0).astype(np.float32)
    cut_z = solid_top - top_map * stagger * nz

    # THE CRACKS STOP SHORT OF THE BASE, leaving a shared plinth.
    #
    # Cutting the network through the full height severs every column from its
    # neighbours and from the ground: measured on hero-basalt-colonnade, 64
    # columns came out as 64 separate pieces with 7 of them hanging in the air.
    # That is not a tuning error, it is what "extrude the crack network through
    # the whole grid" means, and no setting of gap or stagger can undo it.
    #
    # It is also wrong about the rock. Cooling cracks nucleate at the surface
    # and propagate INWARD as the flow cools; the lower part of a flow is still
    # hot when the top has already cracked, so a real colonnade stands on
    # continuous rock and the columns are only free for their upper length.
    # The Giant's Causeway is a pavement, not a bundle of loose sticks.
    #
    # The plinth is jittered per column so it is not a sawn line either.
    solid_bottom = np.where(occ.any(axis=2), np.argmax(occ, axis=2),
                            0).astype(np.float32)
    plinth = rng.random(len(seeds)).astype(np.float32) * 0.10 + 0.14
    plinth_map = np.take_along_axis(
        plinth[:, None, None] * np.ones((1, nx, ny), np.float32),
        order[:1], axis=0)[0]
    crack_floor = solid_bottom + plinth_map * np.maximum(
        solid_top - solid_bottom, 0.0)

    grid.data[np.broadcast_to(edge[:, :, None], grid.shape)
              & (zs > crack_floor[:, :, None])] = 0
    grid.data[zs > cut_z[:, :, None]] = 0


def _exfoliate(grid: VoxelGrid, amount: float, shell_vox: float, salt: int) -> None:
    """Peel curved shells off the surface: onion-skin weathering.

    Granite domes release pressure as the rock above them erodes away, and the
    stone splits into sheets PARALLEL TO THE SURFACE which then spall off. The
    signature is concentric steps, not random pitting, so it cannot come out of
    the noise-and-erode pass however it is tuned.

    Depth below the surface is a distance transform, and a coherent noise field
    decides how many whole shells each patch has lost. Peeling in whole shells
    rather than continuously is what leaves the stepped edge that reads as
    exfoliation.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return
    occ = grid.data != 0
    if not occ.any() or shell_vox < 1.0:
        return

    # Distance-transform the occupied box, not the whole grid. The grid carries
    # a margin for rubble and erosion which is pure air, and the transform is
    # the most expensive single operation in the build -- on a 12 m stone the
    # difference is seconds. Padding by one keeps a ring of background around
    # the solid so the surface still measures as depth zero.
    box = _occupied_box(occ)
    sub = tuple(slice(max(0, b.start - 1), min(s, b.stop + 1))
                for b, s in zip(box, grid.data.shape))
    occ_s = occ[sub]
    depth = ndimage.distance_transform_edt(occ_s).astype(np.float32)
    span = max(grid.data.shape)
    patch = ndimage.gaussian_filter(rng_field(occ_s.shape, salt),
                                    sigma=max(2.0, span * 0.11))
    lo, hi = float(patch.min()), float(patch.max())
    patch = (patch - lo) / max(hi - lo, 1e-9)

    # 0, 1, 2 ... whole shells gone in each patch.
    shells = np.floor(patch * (amount * 3.2)).astype(np.float32)
    grid.data[sub][occ_s & (depth <= shells * shell_vox)] = 0


def _corestones(grid: VoxelGrid, gx, gy, gz, sets: np.ndarray, size_vox: float,
                amount: float, salt: int) -> None:
    """Rot each block inward from its own joints, leaving a rounded core.

    Water gets into jointed granite along the fracture planes and decays the
    rock chemically from those faces inward. A block bounded on all sides is
    attacked from every direction at once, so its CORNERS go first -- three
    joints are close by there at the same time -- and what survives in the
    middle is a sphere. Strip the decayed material out and you have a tor: a
    heap of rounded boulders that still sit on the old rectangular grid.

    The rounding has to be built into the measure, and getting that wrong is
    easy. The obvious choice is the distance to the NEAREST joint plane, taken
    as the smallest across the sets -- but that is exactly the distance to the
    boundary of a box, and eating a box inward by its own boundary distance
    gives a smaller box. The first version of this produced a scatter of neat
    little cubes, which is the opposite of the thing it is named after.

    What actually rounds a corner is that a corner is close to three joints at
    once. So the measure here is the distance from the middle of the block,
    combined across the sets as a straight line rather than an axis at a time.
    A block corner is 1.7 times further from the centre than a face centre is,
    so it crosses the threshold first and goes first, with no separate rounding
    pass. With two joint sets instead of three the same expression gives
    cylinders, which is also right -- two sets do not enclose a block.

    The noise on the rot depth has to be coherent at BLOCK scale, not per
    voxel. Every block rotting by the same amount gives a tray of identical
    balls; what a real tor has is some cores nearly spherical and others barely
    touched, and that difference is between blocks rather than within them.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return
    if amount <= 0.0 or size_vox < 2.0:
        return

    shape = grid.data.shape
    # Distance from the centre of whichever block each voxel is in, measured
    # straight rather than one axis at a time.
    d2 = np.zeros(shape, np.float32)
    for n in sets:
        u = (gx * n[0] + gy * n[1] + gz * n[2]) / size_vox
        off = (u - np.floor(u) - 0.5) * size_vox      # -half .. +half
        d2 += (off * off).astype(np.float32)
    d = np.sqrt(d2, out=d2)

    noise = _norm01(ndimage.gaussian_filter(
        rng_field(shape, salt).astype(np.float32), sigma=max(1.0, size_vox * 0.5)))
    # A core radius of 0.87 blocks reaches the block's corners, so nothing is
    # lost; 0.5 is the ball that just fits inside it; below that the cores pull
    # apart from each other. So the low end of the slider only knocks the
    # corners off, the middle gives a tor whose boulders still touch, and the
    # top gives loose spheres in what used to be rotten rock -- which is grus,
    # and also a real thing to be able to build.
    #
    # The noise on the radius has to be coherent at BLOCK scale. Every block
    # rotting the same amount gives a tray of matching balls; what a real tor
    # has is some cores nearly spherical and others hardly touched, and that
    # difference lives between blocks rather than inside them.
    #
    # And a second, much finer term on the radius, which is not decoration.
    # The expression above is a perfectly smooth analytic sphere, and it is
    # applied AFTER the noise that roughened the original surface -- so
    # wherever a core survives, the stone it leaves is exactly the thing the
    # mass step goes to such trouble to avoid: a smooth curved surface
    # thresholded onto the lattice, which shows its stair-steps as clean
    # concentric contour rings. On a 4.5 m tor the cores are twenty voxels
    # across and it hardly shows; on a 17 m hero at 5 cm they are fifty and
    # the whole tor came back looking like a heap of scallop shells. The
    # roughness has to be a property of the CUT, because there is nothing left
    # afterwards to break it up.
    grain = _norm01(ndimage.gaussian_filter(
        rng_field(shape, salt ^ 0x2b7f).astype(np.float32), sigma=1.6))
    core = ((0.87 - 0.72 * amount) * size_vox * (0.8 + 0.4 * noise)
            * (0.94 + 0.12 * grain))
    grid.data[d > core] = 0


def _settle(grid: VoxelGrid, settle_vox: float, salt: int) -> None:
    """Let loose blocks drop and shift into the space weathering opened.

    Opening the joints is not enough on its own. Blocks that stay in perfect
    register read as one stone with grooves cut into it, because every gap
    stays dead parallel and exactly as wide as every other. Real jointed rock
    settles -- blocks slump into the void the rotted material left, slide a
    little along the joint, and end up out of line.

    Translation only, no rotation. Rotating each block costs an interpolation
    per block and changes its voxel footprint; what the eye actually picks up
    is that the gaps are no longer parallel and no longer constant width, and a
    whole-voxel shift delivers that. The largest block is held still so the
    stone does not walk away from the size measurement in `build`.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return
    if settle_vox < 1.0:
        return
    occ = grid.data != 0
    if not occ.any():
        return
    lab, n = ndimage.label(occ, structure=np.ones((3, 3, 3), bool))
    if n < 2:
        return

    xs, ys, zs = np.nonzero(lab)
    lv = lab[xs, ys, zs]
    mats = grid.data[xs, ys, zs]
    order = np.argsort(lv, kind="stable")
    xs, ys, zs, lv, mats = (a[order] for a in (xs, ys, zs, lv, mats))
    edges = np.searchsorted(lv, np.arange(1, n + 2))
    sizes = np.diff(edges)
    anchor = int(np.argmax(sizes)) + 1     # the block everything else moves around

    nx, ny, nz = grid.data.shape
    out = np.zeros_like(grid.data)
    rs = np.random.default_rng(salt)
    for k in range(1, n + 1):
        a, b = edges[k - 1], edges[k]
        if a >= b:
            continue
        if k == anchor:
            dx = dy = dz = 0
        else:
            dx, dy = np.round(rs.normal(scale=settle_vox * 0.6, size=2)).astype(int)
            # Down, on balance. Gravity is not symmetric.
            dz = int(round(rs.normal(scale=settle_vox * 0.5) - settle_vox * 0.7))
        x, y, z = xs[a:b] + dx, ys[a:b] + dy, zs[a:b] + dz
        # Drop what leaves the grid rather than clamping it to the edge.
        # Clamping looks harmless and is not: every voxel that would have gone
        # past the floor lands on the same plane instead, so a block sinking
        # four voxels has four layers collapse into one and three quarters of it
        # quietly disappears. Dropping loses only what actually fell out.
        ok = ((x >= 0) & (x < nx) & (y >= 0) & (y < ny) & (z >= 0) & (z < nz))
        out[x[ok], y[ok], z[ok]] = mats[a:b][ok]
    grid.data[...] = out


def _seat(grid: VoxelGrid) -> int:
    """Let every loose block fall straight down until it lands.

    This is the mechanism the tor was tuned AROUND rather than given.
    `tools/seed_heroes.py` says it in as many words: `corestone` was held just
    under the value where the cores come apart "because nothing in this pipeline
    drops a loose block onto the one below it". So the slider that makes a heap
    of boulders had a ceiling set by a missing mechanism, and even under that
    ceiling the blocks that did come apart stayed where the rot left them --
    four of them on `hero-tor-stack` seed 1, up to 166,000 voxels each, hanging
    0.65 to 1.95 m above the boulder they should have been resting on, with
    solid rock under 80-88% of their footprint. A tor is boulders resting on
    each other. They have to be able to fall.

    Run LAST, after every pass that removes mass. Settling before weathering is
    not enough on its own -- `_settle` shifts blocks into the space the rot
    opened, and then erosion eats the contact points it made, which is how a
    stack that was touching comes apart again. Measured on the same asset: the
    body was one piece at 100% after settling and 92.8% after eroding.

    Straight down, no rolling and no toppling. What the eye reads on a tor is
    that the blocks TOUCH, at a point rather than over a face; where a real
    block would have rolled off, this one lands with a smaller contact. It also
    means a piece can only ever end up lower than it started, so this pass
    cannot create a floating piece even if it does nothing useful.

    The ground is the lowest level the stone reaches, not z=0, so a stone whose
    blank was cut short does not have its blocks dropped into the hole beneath
    it. That is the same definition `grid.ground_band` uses.

    Returns how many pieces moved, and by how many voxels in total.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return 0
    occ = grid.data != 0
    if not occ.any():
        return 0
    lab, n = ndimage.label(occ, structure=np.ones((3, 3, 3), bool))
    if n < 2:
        return 0

    nz = grid.data.shape[2]
    zz = np.arange(nz, dtype=np.int32)
    z0, tol = ground_band(occ, grid.voxel_m)

    # Everything already standing on the ground is support and does not move.
    seated = np.zeros_like(occ)
    loose = []
    for c in range(1, n + 1):
        m = lab == c
        zs = np.flatnonzero(m.any(axis=(0, 1)))
        if zs.size == 0:
            continue
        if int(zs[0]) <= z0 + tol:
            seated |= m
        else:
            loose.append((int(zs[0]), c))

    # Lowest first, so whatever a block lands on has already come to rest.
    moved = 0
    for _, c in sorted(loose):
        m = lab == c
        xs = np.flatnonzero(m.any(axis=(1, 2)))
        ys = np.flatnonzero(m.any(axis=(0, 2)))
        x0, x1 = int(xs[0]), int(xs[-1]) + 1
        y0, y1 = int(ys[0]), int(ys[-1]) + 1
        p = m[x0:x1, y0:y1]
        s = seated[x0:x1, y0:y1]

        has = p.any(axis=2)
        bot = np.where(has, p.argmax(axis=2), nz).astype(np.int32)
        # The highest support in each column that is BELOW this block's foot.
        under = s & (zz[None, None, :] < bot[:, :, None])
        top = np.where(under.any(axis=2),
                       nz - 1 - under[:, :, ::-1].argmax(axis=2), -1).astype(np.int32)
        clear = np.where(top >= 0, bot - top - 1, bot - z0)
        drop = int(clear[has].min())

        if drop > 0:
            bx, by, bz = np.nonzero(m)
            mats = grid.data[bx, by, bz]
            grid.data[bx, by, bz] = 0
            grid.data[bx, by, bz - drop] = mats
            m = np.zeros_like(m)
            m[bx, by, bz - drop] = True
            moved += 1
        seated |= m
    return moved


def daylight(sil: np.ndarray) -> np.ndarray:
    """Mask of the openings you can see through, given a silhouette (across, z).

    Two framing details, both of which have silently reported "no arch" on an
    arch that was plainly there:

    - the bottom is closed by the GROUND, not by stone, so a floor is laid
      under the stone before looking; without it the opening drains out of the
      bottom of the picture.
    - `binary_fill_holes` treats the array edge as outside, and a grid cropped
      to the asset's bounding box has the stone touching all four edges, so an
      opening reaching the left or right leaks out sideways. Hence the margin.
    """
    from scipy import ndimage
    h, w = sil.shape
    framed = np.zeros((h + 2, w + 2), bool)
    framed[1:-1, 1:-1] = sil
    framed[:, 0] = True                       # ground
    return (ndimage.binary_fill_holes(framed) & ~framed)[1:-1, 1:-1]


def _arch(grid: VoxelGrid, rng, amount: float, out: dict | None = None) -> str:
    """Punch a hole through the stone: an arch, a window, a natural bridge.

    A fin of rock develops alcoves on both flanks along a weak layer; they
    deepen, meet, and the window that opens then widens by spalling until what
    is left is a span that carries its own load. Nothing else in this pipeline
    can produce a through-going hole -- hollows eaten in from either side meet
    only by accident, and a fin thin enough for them to meet usually fails
    first -- so the hole is carved explicitly and the weathering pass is left
    to do the rounding afterwards.

    The span is measured after cutting, and the cut is retried smaller if it
    left the arch too thin or in pieces. That check is cheap and it heads off
    the one way this goes badly wrong.

    Returns why it did what it did. Every exit here used to be a bare `return`
    that put the stone back and reported nothing, so an arch that never opened
    was indistinguishable from one that opened and got weathered shut -- see
    `tools/archprobe.py`, which exists because of exactly that.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return "no scipy"
    occ = grid.data != 0
    if not occ.any():
        return "nothing to cut"
    box = _occupied_box(occ)
    span = [box[i].stop - box[i].start for i in range(3)]
    # Bore across the thinner horizontal axis: that is the way through a fin.
    axis = 0 if span[0] <= span[1] else 1
    across = 1 - axis
    before = grid.data.copy()
    solid0 = int(occ.sum())
    conn = np.ones((3, 3, 3), bool)

    # How much of the stone was already one piece BEFORE the cut. Roughness and
    # faceting shed loose chips at the surface, and how many they shed depends
    # on how many voxels the stone is across -- so this is a property of the
    # lattice, not of the arch, and judging the cut against an absolute count
    # made the same rock pass at one size and fail at another.
    lbl0, n0 = ndimage.label(occ, structure=conn)
    whole0 = (np.bincount(lbl0.ravel())[1:].max() / solid0) if n0 else 0.0

    nx, ny, nz = grid.data.shape
    ga = np.arange(nx if across == 0 else ny, dtype=np.float32)[:, None] + 0.5
    gzz = np.arange(nz, dtype=np.float32)[None, :] + 0.5

    # Kept off the ends: with the opening a quarter of the width, a centre at
    # 0.35 leaves one leg a tenth of the stone thick and the arch reads as a
    # sea cave rather than a bridge.
    # Where the stone actually stands, not where its lowest voxel is. Faceting
    # leaves chips below the body, and taking the bottom of the bounding box
    # put the springing line under the rock: the arch was carved through empty
    # air, the roof measured 68% of the height because the "roof" was the whole
    # stone, and the carve reported a clean cut with no daylight in it. So the
    # floor is the lowest level carrying a real cross-section.
    per_z = (grid.data != 0).sum(axis=(0, 1))
    solid_z = np.nonzero(per_z >= 0.05 * per_z.max())[0]
    base = int(solid_z[0]) if solid_z.size else box[2].start
    span[2] = max(box[2].stop - base, 4)
    zz = gzz - base                # height above the foot of the stone

    # An arch spans between two FOOTINGS, and a stone does not necessarily have
    # two. Faceting cuts oblique planes, so a blank can arrive as a wedge whose
    # underside lifts clear of the ground down one whole side; cutting a span
    # into that leaves an opening that drains out from under the overhang
    # instead of being framed by a leg. Every attempt on the 90 m arch cut
    # cleanly, reported a healthy roof, and produced no daylight at all,
    # because the left leg was never standing on anything.
    #
    # So the opening is placed and sized against the part of the stone that
    # reaches the ground, not against its bounding box.
    foot_h = max(3, int(0.12 * span[2]))
    grounded = (occ.any(axis=axis))[:, base:base + foot_h].any(axis=1)
    edges = np.diff(np.concatenate(([0], grounded.view(np.int8), [0])))
    starts, stops = np.nonzero(edges == 1)[0], np.nonzero(edges == -1)[0]
    if starts.size == 0:
        return "the stone does not reach the ground"
    k = int(np.argmax(stops - starts))
    g0, g1 = int(starts[k]), int(stops[k])
    if g1 - g0 < 0.40 * span[across]:
        return (f"footing is only {(g1 - g0) / span[across]:.0%} of the width; "
                "nothing here to span between")
    c_a = g0 + (g1 - g0) * (0.46 + 0.08 * rng.random())
    r_a_max = (g1 - g0) * 0.5 - 0.07 * span[across]
    tries = []
    for attempt in range(6):
        shrink = 0.82 ** attempt
        # Proportions off measured arches rather than off caution. A span whose
        # opening is a third of its height carries a roof two thirds as deep as
        # the whole rock is tall, and no amount of weathering makes that read as
        # anything but a boulder with a tunnel in it. Landscape Arch and Rainbow
        # Bridge both run an opening around 70% of total height on legs about a
        # tenth of the width each; the retry loop below is what makes it safe to
        # start there and back off rather than the other way round.
        r_a = min(amount * span[across] * 0.45, r_a_max) * shrink
        r_z = amount * span[2] * 0.85 * shrink
        if r_a < 2.0 or r_z < 4.0:
            return f"opening would be under 2 voxels ({r_a:.1f} x {r_z:.1f})"

        # The opening reaches the GROUND. An ellipse centred inside the stone
        # leaves a slab under the hole and makes a window in a boulder, which
        # is what this produced for as long as the shape was an ellipse: 14000
        # px of daylight, none of it anything you could walk under. So the
        # profile is legs to the springing line and an elliptical roof above.
        spring = r_z * 0.45
        t = np.clip((zz - spring) / max(r_z - spring, 1.0), 0.0, 1.0)
        half = r_a * np.sqrt(np.clip(1.0 - t * t, 0.0, 1.0))
        # Footings: the legs flare where they meet the ground, so the opening
        # narrows slightly over its lowest fifth rather than standing on two
        # vertical cuts.
        half = half * (1.0 - 0.16 * np.clip(1.0 - zz / max(0.25 * r_z, 1.0), 0.0, 1.0))
        tube2d = (zz <= r_z) & (zz >= 0.0) & (np.abs(ga - c_a) <= half)

        cut = (np.broadcast_to(tube2d[None, :, :], grid.data.shape) if across == 1
               else np.broadcast_to(tube2d[:, None, :], grid.data.shape))
        grid.data[...] = before
        grid.data[cut] = 0

        # THE LEGS AND THE SPAN, so weathering cannot eat them.
        #
        # This carve worked and the asset still had no hole, which took a
        # stage-by-stage measurement to see: `desert-arch` seed 1 came out of
        # `_arch` with 660 px of daylight and out of `_erode` with ONE. The
        # opening is cut correctly and then erosion removes the legs that made
        # it an arch -- they are the thinnest rock on the stone, so they go
        # first -- and once a leg's foot is gone the opening drains sideways
        # and stops being an enclosed hole at all. Five of six seeds here, and
        # one of six on `hero-natural-arch`.
        #
        # The fix is not to weather less. It is that LOAD-BEARING ROCK WEATHERS
        # SLOWER, which is the finding this project already researched for the
        # arch work: Bruthans et al., "Sandstone landforms shaped by negative
        # feedback between stress and erosion", Nature Geoscience 7, 597-601
        # (2014). Under load the grains lock and erosion nearly stops, which is
        # exactly why real arches survive as thin legs under a heavy span. See
        # docs/arch-research-and-verdict.md.
        #
        # So the band around the opening is marked, and `_durability` makes it
        # hard. A wider band than the cut, because the leg is what stands
        # BESIDE the hole, not the hole itself.
        band = (zz <= r_z * 1.35) & (np.abs(ga - c_a) <= r_a * 2.2)
        load = (np.broadcast_to(band[None, :, :], grid.data.shape) if across == 1
                else np.broadcast_to(band[:, None, :], grid.data.shape))
        if out is not None:
            out["load_path"] = load & (grid.data != 0)

        occ2 = grid.data != 0
        solid = int(occ2.sum())
        if solid == 0:
            continue

        # What makes this an arch is that you can see daylight through it, so
        # that is what gets measured: a gap in the silhouette looking ALONG the
        # bore. A cavity that stops short leaves the silhouette solid and fails
        # here, which a mass count would have called a success.
        #
        sil = occ2.any(axis=axis)
        opening = int(daylight(sil).sum())

        lbl, n = ndimage.label(occ2, structure=conn)
        if n == 0:
            continue
        sizes = np.bincount(lbl.ravel())
        sizes[0] = 0
        main = int(sizes.argmax())
        whole = sizes[main] / solid
        # Roof over the hole and a leg on either side, all in the SAME piece,
        # and each of them thick enough to be believed. Asking only whether
        # SOMETHING survives above the opening is what let the aggressive first
        # attempt through as a thin stone ring: every test passed, and the
        # answer was a doughnut. Thickness is what separates an arch from one.
        big = (lbl == main).any(axis=axis)
        a0, a1, z1 = int(c_a - r_a), int(c_a + r_a), int(base + r_z)
        if a0 <= 0 or a1 >= big.shape[0] or z1 >= big.shape[1]:
            continue
        roof = float(np.median(big[a0:a1, z1:].sum(axis=1)))
        left = float(np.median(big[:a0, base:z1].sum(axis=0)))
        right = float(np.median(big[a1:, base:z1].sum(axis=0)))
        shape = (f"roof {roof / span[2]:.0%} of height, legs "
                 f"{left / span[across]:.0%}/{right / span[across]:.0%} of width, "
                 f"opening {opening / max(sil.sum(), 1):.1%} of the face, "
                 f"{100.0 * whole:.0f}% in one piece (was {100.0 * whole0:.0f}%)")
        tries.append(f"{shrink:.2f}: {shape}")
        standing = (roof >= 0.16 * span[2]
                    and min(left, right) >= 0.06 * span[across])

        # TRIED AND REVERTED: refusing a cut that would not survive weathering.
        #
        # `_erode` runs after this and its budget scales with the stone, so on a
        # 90 m arch it takes over a metre off every exposed face -- and a span is
        # exposed inside as well as out. It looked like the explanation for
        # `hero-arch-colossal`: the carve left 93,892 px of daylight on seed 6
        # and the weathering pass took it to 6.
        #
        # So this asked, before accepting a cut, whether the opening was still
        # there after thinning the silhouette by that retreat
        # (`binary_erosion` by `erosion_retreat_vox`, then `daylight` again),
        # and shrank if it was not. It is far too strict: 2D erosion of a
        # silhouette removes from the outline of the WHOLE stone at once, where
        # the real pass removes from surfaces, and it rejected all six attempts
        # on all eight seeds. Every seed came back with the stone whole and no
        # hole at all -- worse than the defect. Measured, reverted, and written
        # down so it is not tried again.
        #
        # The helper that measured the retreat went with it rather than being
        # left behind unused; the formula is `amount * 5 * clip(span/100, 1, 6)`
        # in `_erode` if it is wanted again. What is missing is a cheap 3D model
        # of what weathering does to a span, not a stricter 2D one.
        #
        # THE ACCEPTANCE TEST BELOW WENT MISSING WHEN THAT EXPERIMENT WAS
        # REVERTED, and the loop kept running without it: `standing` was
        # computed on every attempt and never read, so `_arch` tried six cuts,
        # reverted the stone and reported "no standing span" every single time,
        # on every seed, while the per-attempt lines it printed showed a roof of
        # 42% and an opening of 49.7% -- a healthy arch it then threw away. An
        # assignment nobody reads is this project's signature failure and it is
        # worth naming here: the diagnostic string was RIGHT and the code that
        # was supposed to act on it had been deleted.
        if standing and opening >= 0.02 * sil.sum() and whole >= 0.9 * whole0:
            return f"cut at {shrink:.2f}: {shape}"

        # Steer as well as shrink. Lumps land off-centre, so the middle of the
        # bounding box is not the middle of the STONE, and a centred cut on a
        # lopsided blank takes one leg off entirely -- which shrinking alone
        # never recovers, because a smaller hole in the same wrong place is
        # still in the wrong place. Six attempts of shrinking chased a 90 m
        # arch down to a third of its opening and still left a 0% leg.
        c_a += 0.5 * (right - left)
    grid.data[...] = before        # no credible arch here; leave the stone whole
    return "no standing span; attempts were\n      " + "\n      ".join(tries)


def _block_relief(grid: VoxelGrid, gx, gy, gz, sets: np.ndarray, size_vox: float,
                  relief: float, salt: int, taper: float = 0.0) -> None:
    """Open the joint planes so the mass reads as separate blocks.

    This is the voxel version of what the implicit-blocks literature does with
    signed distance fields: fracture the bedrock along its joint sets and treat
    each cell as a block that sits slightly proud of or recessed from its
    neighbours. What the eye reads is the GAP -- a continuous mass with faces
    drawn on it still looks like one stone; a mass with open joints between
    blocks looks like fractured bedrock.
    """
    if relief <= 0.0 or size_vox < 2.0:
        return
    coords = [gx * n[0] + gy * n[1] + gz * n[2] for n in sets]
    gap = np.zeros(grid.data.shape, bool)
    rs = np.random.default_rng(salt)

    # Water dissolving its way down a joint is spent as it goes, so the opening
    # is widest at the top and closes toward the base. That turns a grid of
    # blocks into a cluster of blades standing on a shared plinth, which is what
    # a limestone pinnacle forest is.
    occ_z = np.flatnonzero((grid.data != 0).any(axis=(0, 1)))
    if occ_z.size < 2:
        return
    z0, z1 = float(occ_z[0]), float(occ_z[-1])
    height = max(z1 - z0, 1.0)

    narrow = 1.0
    if taper > 0.0:
        h = np.clip((gz - z0) / height, 0.0, 1.0)
        narrow = (1.0 - taper) + taper * h ** 1.5

    for i, c in enumerate(coords):
        u = c / size_vox
        # Distance to the nearest joint plane in this direction, in voxels.
        dist = np.abs(u - np.round(u)) * size_vox
        # Per-block width, so joints are not all the same thickness.
        block = np.round(u).astype(np.int32)
        width = (0.35 + 0.65 * rs.random(64)[np.abs(block) % 64]) * relief

        # THE JOINTS STOP SHORT OF THE BASE, leaving a shared plinth.
        #
        # `_columns` already learned this and says why: a crack network cut
        # through the full height severs every block from its neighbours and
        # from the ground, and no setting of gap or width undoes it. The same
        # was true here. The taper was supposed to cover it -- its own comment
        # promises "a cluster of blades standing on a shared plinth" -- but a
        # taper only NARROWS the joint, and a joint one voxel wide still cuts
        # the rock in two. Measured on `limestone-pinnacles` at taper 0.85:
        # the gap at the foot came out 2 voxels, 10 cm, and the asset was
        # 8 blades that shared nothing.
        #
        # It is also wrong about the rock. Water works DOWN a joint and is
        # spent as it goes; the bottom of the grike is where it runs out. A
        # karst pinnacle field is blades standing on a continuous limestone
        # plateau. Jittered per block so the plinth line is not sawn either.
        foot = z0 + (0.10 + 0.06 * rs.random(64)[np.abs(block + 7 * i) % 64]
                     ) * height
        gap |= (dist < width * narrow) & (gz > foot)

    # Suppressing the cut over ungrounded columns was TRIED and it is the wrong
    # trade. It does remove the floating blades, but a lens-shaped blank's rim
    # is most of what you see in silhouette, so refusing to fracture it turned
    # the pinnacle forest into a domed loaf with grooves scratched on it. The
    # blades ARE the asset. The fix belongs upstream, in `_plinth`, which makes
    # the ground those blades were missing rather than declining to carve them.
    grid.data[gap] = 0


def _plinth(grid: VoxelGrid) -> int:
    """Fill the hollow under the stone, so a jointed mass stands on rock.

    `_block_relief`'s taper says it turns a grid of blocks into "a cluster of
    blades standing on a shared plinth". The blades were real; the plinth was
    not. A blank made by flattening and elongating a lump is lens-shaped, so its
    rim curves up away from the ground, and once the joints fracture that rim
    the pieces have nothing under them at all. Measured on
    `hero-tsingy-pinnacles`: three blades, 53,000 voxels, up to 16 m tall, with
    ZERO solid anywhere beneath their entire footprint. Nothing had been carved
    out from under them -- they had never touched the ground.

    So make the plinth. Every column that has stone in it gets stone all the way
    down to the base of the mass, which is what a karst pinnacle field actually
    is: blades standing on a continuous limestone plateau, not a bundle of
    sticks resting on a curved lens.

    Run BEFORE jointing, so the joints then cut real rock and every blade they
    make comes down onto it. Gated on `joint_taper`, because filling the
    underside of a balanced rock would destroy the one thing that asset is.
    """
    occ = grid.data != 0
    if not occ.any():
        return 0
    zz = np.arange(grid.data.shape[2], dtype=np.int32)[None, None, :]
    z0 = int(np.flatnonzero(occ.any(axis=(0, 1)))[0])
    has = occ.any(axis=2)
    lowest = np.argmax(occ, axis=2).astype(np.int32)
    hole = has[:, :, None] & (zz >= z0) & (zz < lowest[:, :, None])
    if not hole.any():
        return 0
    # Take each column's own lowest material rather than a constant, so a spec
    # with a rind or a vein at the base keeps its own rock down there.
    mat = np.take_along_axis(grid.data, lowest[:, :, None], axis=2)
    grid.data[hole] = np.broadcast_to(mat, grid.data.shape)[hole]
    return int(hole.sum())


def _occupied_box(occ: np.ndarray) -> tuple[slice, slice, slice]:
    """Bounding box of the solid voxels, as slices."""
    idx = [np.flatnonzero(occ.any(axis=tuple(a for a in range(3) if a != ax)))
           for ax in range(3)]
    return tuple(slice(int(i[0]), int(i[-1]) + 1) if i.size else slice(0, 0)
                 for i in idx)


def _fit(a: np.ndarray, shape) -> np.ndarray:
    """Crop or edge-replicate `a` to exactly `shape`."""
    if a.shape == tuple(shape):
        return a
    a = a[:shape[0], :shape[1], :shape[2]]
    pad = [(0, max(0, shape[i] - a.shape[i])) for i in range(3)]
    return np.pad(a, pad, mode="edge") if any(p[1] for p in pad) else a


def _ellipsoid(grid: VoxelGrid, c, rx: float, ry: float, rz: float, mat: int) -> None:
    rx, ry, rz = max(rx, 0.6), max(ry, 0.6), max(rz, 0.6)
    x0, x1 = int(c[0] - rx) - 1, int(c[0] + rx) + 2
    y0, y1 = int(c[1] - ry) - 1, int(c[1] + ry) + 2
    z0, z1 = int(c[2] - rz) - 1, int(c[2] + rz) + 2
    nx, ny, nz = grid.shape
    x0, y0, z0 = max(x0, 0), max(y0, 0), max(z0, 0)
    x1, y1, z1 = min(x1, nx), min(y1, ny), min(z1, nz)
    if x0 >= x1 or y0 >= y1 or z0 >= z1:
        return
    xs = (np.arange(x0, x1) + 0.5 - c[0]) / rx
    ys = (np.arange(y0, y1) + 0.5 - c[1]) / ry
    zs = (np.arange(z0, z1) + 0.5 - c[2]) / rz
    d = (xs[:, None, None] ** 2 + ys[None, :, None] ** 2 + zs[None, None, :] ** 2)
    block = grid.data[x0:x1, y0:y1, z0:z1]
    block[d <= 1.0] = mat


def _facet(grid: VoxelGrid, rng, facets: int, angular: float,
           wobble: np.ndarray | None = None, sets: np.ndarray | None = None,
           scatter: float = 0.12) -> None:
    """Slice flat faces off the mass with half-space cuts.

    The cut depth is measured ALONG EACH NORMAL, against the mass that is
    actually still there. An earlier version measured one extent for the whole
    rock -- the longest axis -- and used it for every cut, so any cut across a
    short axis landed outside the stone and removed nothing. Every rock came out
    a smooth ellipsoid and the angularity slider did nothing at all.
    """
    # Work inside the solid's bounding box. Everything outside it is air that
    # cannot be cut, and on a large rock the grid carries a margin for rubble
    # and erosion that would otherwise be re-evaluated once per facet.
    box = _occupied_box(grid.data != 0)
    sub = grid.data[box]
    wob = wobble[box] if wobble is not None else None
    gx, gy, gz = _axes(box[0].stop - box[0].start, box[1].stop - box[1].start,
                       box[2].stop - box[2].start,
                       start=(box[0].start, box[1].start, box[2].start), half=0.0)

    for i in range(facets):
        occ = sub != 0
        total = int(occ.sum())
        if total < 8:
            return
        xs, ys, zs = np.nonzero(occ)
        centre = np.array([xs.mean() + box[0].start, ys.mean() + box[1].start,
                           zs.mean() + box[2].start])

        if i == 0 and angular > 0.35:
            # A flat top, deliberately, before anything random. It is the single
            # most legible signal that a voxel lump is a ROCK rather than a
            # boulder-shaped nothing -- a horizontal face catches the light flat
            # while every side falls away, and the eye reads stone immediately.
            n = np.array([0.0, 0.0, 1.0])
        elif sets is not None:
            # Draw from a joint set rather than from nothing. Every face on this
            # rock then belongs to one of three orientations, which is what
            # makes an outcrop read as fractured bedrock instead of a lump.
            n = sets[rng.integers(len(sets))] * (1.0 if rng.random() < 0.5 else -1.0)
            n = n + rng.normal(size=3) * scatter
            n = n / max(np.linalg.norm(n), 1e-6)
        else:
            n = rng.normal(size=3)
            n[2] *= 0.6           # the rest lean toward upright faces
            n = _snap_axis(n / max(np.linalg.norm(n), 1e-6))
        n = n / max(np.linalg.norm(n), 1e-6)

        # NEVER cut from underneath. A facet plane takes the mass on the far
        # side of it away, so a normal pointing down slices a slab off the BASE
        # of the stone -- and the base is the one face that is not free. It is
        # sitting in the ground. Nothing downstream puts it back, so the whole
        # stone ends up hanging in the air over the hole it was cut out of.
        #
        # Measured on `hero-tor-stack`, which draws its facet normals from a
        # joint frame whose bedding plane is near horizontal and picks the sign
        # at random: with the cut allowed, the stone's lowest voxel went from
        # z=0 before faceting to z=21 (2.1 m up) immediately after, and stayed
        # there -- 98.8% of a 5.9 M voxel hero reported airborne on seed 2, the
        # entire body standing on nothing but its own rubble. Flipping the sign
        # rather than skipping the facet keeps the number of cuts the same, so
        # `facets` still means what it says.
        if n[2] < -0.5:
            n = -n

        d = (gx - centre[0]) * n[0] + (gy - centre[1]) * n[1] + (gz - centre[2]) * n[2]
        # Place the plane by QUANTILE of the mass, not by a fraction of the
        # reach. Depth-as-a-fraction-of-reach sounds equivalent and is not: an
        # ellipsoid's far point along a normal is much further out than its
        # bulk, so a cut at 80% of the reach shaved a cap of a few dozen voxels
        # and the silhouette stayed an egg. A quantile says what it means --
        # this plane takes this much stone off -- so `angular` reads directly as
        # how blocky the result is.
        frac = 0.04 + 0.16 * angular * (0.4 + 0.6 * rng.random())
        if i == 0 and angular > 0.35:
            # A shallow top. At full depth this one plane, combined with the
            # burial cut at z=0, turned every rock into a slanted wedge -- two
            # near-parallel faces and nothing left between them.
            frac *= 0.5
        # Quantile on a SAMPLE. Sorting a million projections eight times over
        # is most of a second for a threshold that only has to be right to a
        # fraction of a percent.
        proj = d[occ]
        if proj.size > 120_000:
            proj = proj[::proj.size // 120_000]
        cut = float(np.quantile(proj, 1.0 - frac))
        # Wobble the plane so the face comes out broken rather than machined.
        # The quantile is taken on the true plane, so the depth still means what
        # it says; only the surface it leaves behind is roughened.
        sub[occ & (d > cut + (wob if wob is not None else 0.0))] = 0


def rng_field(shape, salt: int = 0) -> np.ndarray:
    """Uniform noise the erosion pass draws against.

    Uses its own generator seeded from the grid shape rather than the build rng,
    so erosion stays deterministic without having to thread the generator
    through every call.
    """
    return np.random.default_rng(shape[0] * 73856093 ^ shape[1] * 19349663
                                 ^ shape[2] * 83492791 ^ salt).random(shape)


def coherent_noise(shape, salt: int, sigma: float) -> np.ndarray:
    """Spatially coherent noise that is still UNIFORM on [0, 1].

    Both weathering passes work by drawing a coherent field and keeping voxels
    where it falls below a chance, so that "chance" only means what it says if
    the field is uniformly distributed. Blurring uniform noise does not leave it
    uniform -- it leaves a narrow bell around the mean, because each output
    value is an average of a few hundred inputs. Rescaling that bell by its
    minimum and maximum, which is the obvious thing to do and what this code did
    for a long time, stretches the two most extreme voxels to 0 and 1 and leaves
    everything else bunched in the middle.

    The cost of getting this wrong is not subtle and it is silent. A chance of
    0.3 against a field like that removes 0.44% of the surface rather than 30%,
    so the weathering slider ran the whole pass, paid for it, and took twenty
    voxels off a stone of ninety thousand. Every rock in the library was tuned
    around a weathering pass that was doing almost nothing, which is why the
    shape of these rocks had to be won with roughness and cut planes instead.

    Mapping the bell through its own normal CDF -- a logistic is close enough
    and costs one exp -- puts it back on a flat distribution, so the thresholds
    downstream mean what they read as.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return np.full(shape, 0.5, np.float32)
    f = ndimage.gaussian_filter(rng_field(shape, salt).astype(np.float32),
                                sigma=max(0.4, sigma))
    z = (f - float(f.mean())) / (float(f.std()) + 1e-9)
    return (1.0 / (1.0 + np.exp(-1.6 * z))).astype(np.float32)


def _erode(grid: VoxelGrid, amount: float, cavernous: float, durability,
           passes: int = 3, voxel_m: float = 0.05, notch=None, aspect=None) -> None:
    """Weather the surface, driven by the SIGN of the local curvature.

    This is the mechanism from the weathering literature rather than a
    hand-rolled proxy, and it is what makes one generator produce two entirely
    different families of rock. Geologically:

    - **Spheroidal weathering** attacks CONVEX surfaces fastest. Corners and
      edges are exposed on more sides than a flat face, so they go first: a
      blocky stone rounds off, and granite sheds curved shells. It is why a
      boulder is a boulder.
    - **Cavernous weathering** attacks CONCAVE surfaces fastest. Once a pit
      exists it holds moisture and salt, so it deepens faster than the flat rock
      around it. Runaway pitting is what carves tafoni, honeycomb sandstone and
      the hollow-sided goblins of the Colorado Plateau.

    Both fall out of one number, the local solid fraction inside a small ball
    centred on each voxel. On a flat face it is a half; on a protruding corner
    less; inside a hollow more. So `0.5 - fraction` is a signed curvature, and
    which end of it drives the erosion decides what kind of rock you get.

    Two consequences worth naming. It has to be ITERATIVE -- a pit only runs
    away if the next pass sees the pit the last one made, and a single pass can
    only ever produce even pitting. And the noise has to be spatially COHERENT,
    because an independent draw per voxel removes a scatter of single voxels and
    turns the surface into sponge, which hides the faceting underneath.

    `durability` scales the rate per voxel: a hard bed resists, a soft bed goes
    first. That is what turns even weathering into differential weathering, and
    it is the whole reason a hoodoo has a cap.

    Two things modulate the rate that curvature knows nothing about:

    `notch` concentrates attack in a horizontal band at a given HEIGHT. Almost
    every undercut shape in nature is this and not curvature: sand blown along
    the ground bounces to roughly knee height and saws a waist into a desert
    rock, waves work the tidal band and cut the notch that eventually drops a
    sea stack, damp soil rots the base of a boulder. Curvature is blind to
    height, so without this the pipeline can only reach an undercut by tuning
    bedding until a soft layer happens to land in the right place -- and
    bedding repeats, so the same layer lands in five other wrong places too.

    `aspect` weathers one side harder than the other. Attack is rarely even:
    tafoni open on the damp shaded face, salt works the seaward side, frost the
    north. A stone weathered identically on all sides is one of the most
    reliable tells that it came out of a generator.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return
    if not (grid.data != 0).any():
        return

    # Work inside the solid's bounding box. The grid carries a margin for
    # rubble and for the erosion to bite into, and on a big stone that margin
    # is most of the cells -- a 7.7 m tor allocates 26 million of them and the
    # rock itself is under a third of that. Every blur in the loop below was
    # paying for the empty part three times over. The padding keeps enough air
    # around the solid for the curvature probe to still see an edge as an edge.
    #
    # Erosion only ever removes, so the box computed once stays valid for every
    # pass. `grid.data[box]` is a view, so the writes land in the real grid.
    occ0 = grid.data != 0
    box = _occupied_box(occ0)
    body = max(b.stop - b.start for b in box)       # the stone, in voxels
    probe0 = max(1.4, min(4.0, body * 0.05))
    pad = int(3.0 * probe0) + 2
    box = tuple(slice(max(0, b.start - pad), min(s, b.stop + pad))
                for b, s in zip(box, grid.data.shape))
    data = grid.data[box]

    span = max(data.shape)
    # The ball the curvature is measured over. Too small and it only sees the
    # voxel lattice; too large and it stops being local.
    probe = max(1.4, min(4.0, span * 0.05))

    # `amount` is how many voxel LAYERS come off, not what share of one layer.
    #
    # Dividing a fixed budget across a fixed number of passes caps the retreat
    # at well under a single voxel however hard the slider is pushed, and that
    # cap is what makes every differential-erosion mechanism useless: a vein
    # three times tougher than its matrix can only stand proud by however far
    # the matrix went back, so if the matrix never retreats a whole voxel the
    # vein never appears. Same for a caprock, a hard bed, a case-hardened rind
    # and a breccia clast -- all of them measured as doing nothing, all for this
    # one reason. Spending the budget as REPEATED passes at a strong per-pass
    # rate lets the soft rock go back several voxels while the hard rock goes
    # back one, which is the whole point of a durability field.
    #
    # And how many layers is measured against THE STONE, not against the
    # lattice. This budget used to be a flat number of voxels: `erode` 1.0 took
    # 4.7 layers off, measured, at every size from 3 m to 13 m -- 14% of the
    # radius of a 3 m boulder and 3% of a 13 m hero. So every shape that is made
    # by weathering one part harder than another simply stopped existing above a
    # few metres. The notch on the sea stack, the neck under the balanced rock's
    # cap, the soft beds between the hard ones: all of them ran, all of them
    # reported success, and none of them was more than a scratch, because a
    # scratch is what four voxels is on a stone two hundred and sixty across.
    # `tools/rockmech.py` could not catch it because it tests at 3 m only, which
    # is the one size where the absolute number happens to be right.
    #
    # So the budget is a fraction of the stone's own span. Two consequences
    # worth naming. The reference span is deliberately at the top of the
    # ordinary library rather than at the bottom, and the factor is floored at
    # 1.0, so nothing at or below 5 m moves by a single voxel -- twenty of the
    # twenty-seven ordinary rocks are byte-identical and their tuning survives.
    # And measuring in VOXELS rather than in metres is what makes the same spec
    # come out the same SHAPE at 5 cm and at 10 cm; with an absolute budget,
    # halving the voxel size halved every weathered feature relative to the rock
    # it was cut into.
    REF_SPAN_VOX = 100.0
    # Capped, because the cost is one blur of the whole grid per layer and the
    # 90 m arch is a 190-million-cell grid. Six times the old budget is 1.4 m of
    # retreat there, which is as much as that stone has any business losing.
    grow = float(np.clip(body / REF_SPAN_VOX, 1.0, 6.0))
    steps = max(amount, 1e-6) * 5.0 * grow

    # Patch size for the noise: a 5-voxel cobble cannot lose 3-voxel patches and
    # still be a cobble. But the patch also has to grow with the budget, and
    # this is the second half of the same size-blindness. The field is drawn
    # once and reused for every pass -- deliberately, so a weak patch is
    # attacked again and again and a pit runs away -- which means the patches
    # that erode go down by very nearly the WHOLE budget while the ones between
    # them are never touched at all. So the depth of the surface relief is the
    # budget, and its width is this sigma. At three voxels wide and five deep
    # that is stone. At three voxels wide and fifteen deep, which is what the
    # heroes now ask for, it is gravel: the first deep-weathered hero came back
    # covered in a three-voxel crust with contour rings showing through it.
    # Widening the patch in step keeps the proportions of a weathered surface.
    sigma = max(0.8, min(3.0 * grow, span * 0.06))

    # How many passes is set by the STRONGEST attack anywhere on the stone, not
    # by the average. Depth here comes from repetition -- one pass can take at
    # most the exposed shell, so a cut four voxels deep needs four passes that
    # reach it. With a fixed pass count, a band attacked four times harder than
    # its surroundings still only lost one layer per pass and came out level
    # with them: the sea stack had no notch and the hoodoo had no neck, even
    # though the rate field was plainly right. Running more passes at a
    # proportionally lower weight leaves the ordinary surface exactly where it
    # was and lets the singled-out band cut as deep as its rate deserves.
    peak = 1.0
    if notch is not None and notch[0] > 0.0:
        peak *= 1.0 + float(notch[0])
    if aspect is not None and aspect[0] > 0.0:
        peak *= 1.0 + float(aspect[0])
    # The ceiling has to move with the budget. A pass can remove at most the
    # one-voxel shell, so the pass count is also a hard cap on how deep the
    # deepest place can ever get -- and at 18 the deepest place on a hero was
    # 18 voxels whatever the notch was set to. That cap, not the notch, was
    # what flattened the undercut.
    passes = max(1, min(120, int(math.ceil(steps * peak))))
    weights = [steps / passes] * passes

    # ONE noise field for every pass. Regenerating it each time cost a blur of
    # the whole grid per pass for no benefit -- and reusing it is actively
    # better, because the same weak patches get attacked again and again, which
    # is the positive feedback a runaway pit needs. Fresh noise each pass
    # averages that feedback away and gives even pitting instead of cavities.
    field = coherent_noise(data.shape, int(span), sigma)
    dur = durability[box] if durability is not None else None

    # Height band. Measured from where the stone actually meets the ground, not
    # from the bottom of the grid -- the grid has margin under it and the body
    # is sunk into it by the burial fraction, so a band placed against the grid
    # would drift up and down the rock as `bury` changed.
    band = None
    if notch is not None and notch[0] > 0.0:
        attack, nz_m, spread_m = notch
        occ_z = np.flatnonzero((data != 0).any(axis=(0, 1)))
        if occ_z.size:
            zc = ((np.arange(data.shape[2], dtype=np.float32) + 0.5
                   - float(occ_z[0])) * voxel_m)
            band = (1.0 + attack * np.exp(
                -0.5 * ((zc - nz_m) / max(spread_m, 1e-3)) ** 2)
            ).astype(np.float32)[None, None, :]

    # THE FOOTING IS BURIED, SO IT DOES NOT WEATHER.
    #
    # This pass has no idea which way is down. It reads the shape around a voxel
    # and attacks convex surfaces hardest -- and the sharpest convex edge on a
    # part-buried stone is where it enters the ground, which is the one place
    # that is packed in soil and sees no rain, no frost and no wind at all. So
    # the pass ate every stone's feet off, and on anything standing on legs that
    # is fatal.
    #
    # Measured on `hero-natural-arch` at 20 cm, seed 1, walking the stages: the
    # carve left a clean span with 7,531 px of daylight through it, and the
    # weathering pass took that to 3. The span survived -- the arch is plainly
    # there in the silhouette -- but the legs lost their feet, so only 8 of 210
    # columns still reached the ground row and the opening drained out
    # underneath instead of being closed by the ground. Filling the bottom 8
    # rows back in puts 7,118 px of it straight back. That is not a measurement
    # artifact: an arch whose legs stop 1.6 m short of the ground is a floating
    # arch, and it is the same defect as every other one found today.
    #
    # A ramp rather than a hard stop, so the stone does not come out standing on
    # a machined plinth: nothing at ground level, full rate a short way up.
    occ_z0 = np.flatnonzero((data != 0).any(axis=(0, 1)))
    foot = None
    if occ_z0.size:
        foot_vox = max(2.0, 0.06 * float(span))
        zc = np.arange(data.shape[2], dtype=np.float32) - float(occ_z0[0])
        foot = np.clip(zc / foot_vox, 0.0, 1.0).astype(np.float32)[None, None, :]

    side = None
    for w in weights:
        occ = data != 0
        if not occ.any():
            return
        frac = ndimage.gaussian_filter(occ.astype(np.float32), sigma=probe)
        curv = 0.5 - frac                      # + convex, - concave

        # A base rate that curvature BIASES, not a rate made of curvature alone.
        #
        # Pure curvature looks right and does almost nothing. On a flat face the
        # solid fraction is a half by definition, so the signed curvature there
        # is exactly zero and so is the erosion; only sharp corners are ever
        # attacked. That means a stone rounds its corners off in the first pass
        # and then sits at a fixed point where nothing anywhere is convex enough
        # to erode, which is why this pass removed 2% of the rock at its maximum
        # setting and why the durability field -- beds, caps, veins, clasts,
        # every mechanism that works by making some places harder than others --
        # had nothing to modulate. Weathering attacks the whole exposed surface;
        # curvature decides where it attacks FASTER.
        lean = ((1.0 - cavernous) * np.clip(curv, 0.0, None)
                + cavernous * np.clip(-curv, 0.0, None))
        rate = 1.0 + 6.0 * lean

        if aspect is not None and aspect[0] > 0.0 and side is None:
            # The surface normal comes free from the field already blurred for
            # the curvature. Computed once and reused: it costs three full-grid
            # temporaries, and the normals barely move between passes.
            strength, direction = aspect
            gxf, gyf, gzf = np.gradient(frac)
            mag = np.sqrt(gxf * gxf + gyf * gyf + gzf * gzf) + 1e-9
            facing = -(gxf * direction[0] + gyf * direction[1]
                       + gzf * direction[2]) / mag
            side = (1.0 + strength * np.clip(facing, -1.0, 1.0)).astype(np.float32)
        if side is not None:
            rate = rate * side
        if band is not None:
            rate = rate * band
        if foot is not None:
            rate = rate * foot
        # Only the surface can weather; interior voxels are not exposed to
        # anything. Without this the concave term eats the solid core, because
        # deep inside the rock the solid fraction is 1 and the curvature is
        # maximally negative everywhere.
        #
        # Taken as the true one-voxel shell rather than as a threshold on the
        # blurred occupancy. Now that the whole exposed surface has a base rate,
        # a soft threshold would let the pass reach several voxels deep in one
        # go and hollow the stone out from just under its skin.
        shell = occ & ~ndimage.binary_erosion(
            occ, structure=ndimage.generate_binary_structure(3, 1),
            border_value=0)

        # Normalise the rate against its own average over the exposed surface,
        # so `w` is literally "how many voxel layers this pass takes off on
        # average" and the shape of the rate field decides only where they come
        # from. Without this the pass count leaks into the total: more passes
        # means each one is gentler, which means fewer of them run into the
        # per-pass ceiling, which means MORE stone goes overall. Turning the
        # notch up therefore thinned the entire rock rather than cutting a band
        # into it -- the band was working perfectly, and everything else was
        # quietly following it down.
        ref = float(rate[shell].mean()) if shell.any() else 1.0
        chance = np.clip(w * rate / max(ref, 1e-6), 0.0, 0.95)
        if dur is not None:
            chance = chance / np.maximum(dur, 0.05)
        data[shell & (field < chance)] = 0


def _flow(grid: VoxelGrid, flutes: float, sigma_vox: float, pans: float,
          pan_depth_vox: float, salt: int) -> None:
    """Run rain down the outside of the stone and dissolve what it passes over.

    Every other weathering pass here is isotropic: it looks at the shape around
    a voxel and cannot tell which way is down. Real limestone is carved by
    water that very much can, and the marks it leaves are all directional --
    near-vertical runnels that merge as they descend, and flat-bottomed pans
    wherever the water has nowhere lower to go. No amount of tuning a curvature
    rule reaches either of them.

    The process is self-reinforcing, which is why it makes grooves rather than
    an even etch: a shallow depression gathers more water, so it deepens, so it
    gathers more. What stops it running away entirely is that the effect
    reverses once the water film gets deep enough -- a thick sheet of water
    protects the rock beneath it instead of cutting into it, so flutes die out
    downslope and give way to a smooth apron. That shut-off is in the rate law
    below, and without it the whole face collapses into one deep gully.

    **Routing.** Ordinary flow accumulation is defined on a heightmap and
    cannot cope with an overhang. This does not need one: water always moves to
    strictly lower ground, so the grid can be swept one z-slab at a time and
    every voxel within a slab is independent of the others. That makes each
    slab a handful of whole-array operations instead of a per-voxel walk.

    **On scale.** The flutes on real limestone -- rillenkarren -- are one to
    three centimetres across. At a 5 cm lattice they are smaller than a voxel
    and nothing here can recover them; they are a job for the texture pass.
    What this draws is the next size class up, the decimetre runnels, which do
    read. Point it at the small ones and it returns noise.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return
    occ = grid.data != 0
    if not occ.any():
        return
    nx, ny, nz = grid.data.shape
    sigma = max(1.0, float(sigma_vox))
    sm = ndimage.gaussian_filter(occ.astype(np.float32), sigma=sigma)
    surf = occ & (sm < 0.92)
    if not surf.any():
        return

    gxs, gys, gzs = np.gradient(sm)
    mag = np.sqrt(gxs * gxs + gys * gys + gzs * gzs) + 1e-9
    nxg, nyg, nzg = -gxs / mag, -gys / mag, -gzs / mag      # outward normal

    # Where water stays long enough to do anything. It clings to surfaces that
    # face upward or stand steep, and drips off anything that overhangs, so the
    # attack peaks on the upper part of a steep face and is nil underneath a
    # ledge. Flat tops get little of it -- water sits there rather than running,
    # which is what makes a pan instead of a flute.
    wet = np.clip(nzg + 0.35, 0.0, 1.0) * (0.25 + 0.75 * (1.0 - nzg * nzg))

    # Gravity projected onto the surface: g - (g.n)n, with g pointing down.
    dxf = nzg * nxg
    dyf = nzg * nyg
    dzf = nzg * nzg - 1.0

    acc = surf.astype(np.float32)          # one unit of rain per surface voxel
    sink = np.zeros_like(surf)
    offs = [(i, j) for i in (-1, 0, 1) for j in (-1, 0, 1)]
    dirs = [_unit((i, j, -1.0)) for i, j in offs]

    for z in range(nz - 1, 0, -1):
        xi, yi = np.nonzero(surf[:, :, z])
        if xi.size == 0:
            continue
        best = np.full(xi.size, -2.0, np.float32)
        bx = np.zeros(xi.size, np.int64)
        by = np.zeros(xi.size, np.int64)
        dxi, dyi, dzi = dxf[xi, yi, z], dyf[xi, yi, z], dzf[xi, yi, z]
        for (i, j), v in zip(offs, dirs):
            xj, yj = xi + i, yi + j
            ok = (xj >= 0) & (xj < nx) & (yj >= 0) & (yj < ny)
            xc, yc = np.clip(xj, 0, nx - 1), np.clip(yj, 0, ny - 1)
            ok &= surf[xc, yc, z - 1]
            score = np.where(ok, (v[0] * dxi + v[1] * dyi + v[2] * dzi),
                             -2.0).astype(np.float32)
            upd = score > best
            best = np.where(upd, score, best)
            bx = np.where(upd, xc, bx)
            by = np.where(upd, yc, by)
        has = best > -1.9
        if has.any():
            contrib = np.bincount((bx[has] * ny + by[has]),
                                  weights=acc[xi[has], yi[has], z],
                                  minlength=nx * ny)
            acc[:, :, z - 1] += contrib.reshape(nx, ny).astype(np.float32)
        # Nowhere lower to run: water pools here. These are the pan sites, found
        # rather than placed.
        if (~has).any():
            sink[xi[~has], yi[~has], z] = True

    rs = np.random.default_rng(salt)

    if flutes > 0.0:
        vals = acc[surf]
        ref = float(np.quantile(vals, 0.90)) if vals.size else 1.0
        a = acc / max(ref, 1e-6)
        # Rises with flow, then shuts off once the film is deep enough to
        # protect the rock. Monotone erosion gives one gully; this gives a set
        # of runnels that fade out toward the bottom of the face.
        rate = np.power(np.maximum(a, 0.0), 0.6) * np.exp(-a / 1.8)
        rate = (rate / max(float(rate.max()), 1e-9) * wet).astype(np.float32)
        # Push the pattern a little way into the stone. It is defined on the
        # surface, and without this the first pass strips that surface and the
        # ones after it find a rate of zero everywhere -- so the groove would be
        # exactly one voxel deep however hard the slider was pushed.
        rate = ndimage.maximum_filter(rate, size=5)
        noise = coherent_noise(grid.data.shape, salt ^ 0x1f10, sigma * 0.5)
        for _ in range(3):
            occ2 = grid.data != 0
            if not occ2.any():
                break
            s2 = occ2 & (ndimage.gaussian_filter(
                occ2.astype(np.float32), sigma=sigma) < 0.92)
            grid.data[s2 & (noise < np.clip(flutes * rate * 0.6, 0.0, 0.8))] = 0

    if pans > 0.0 and pan_depth_vox >= 1.0 and sink.any():
        _pans(grid, sink, acc, pans, pan_depth_vox, rs)


def _pans(grid: VoxelGrid, sink, acc, amount: float, depth_vox: float, rs) -> None:
    """Dissolve flat-bottomed hollows where the water cannot get away.

    Taken as discrete sites rather than as every drainless voxel. On a flat top
    every voxel is drainless -- there is no lower neighbour anywhere on it --
    so treating the condition as the answer would plane the whole top off by
    the pan depth and call it geology. Real pans are separate hollows with rock
    between them, so the sites are picked a few at a time and kept apart.

    The floor is flat and level, which is the whole difference between a
    solution pan and an ordinary dent: standing water dissolves evenly across
    its own base. The footprint widens slightly with depth, so the rim
    overhangs a little, which is the other half of the read.
    """
    nx, ny, nz = grid.data.shape
    xs, ys, zs = np.nonzero(sink)
    if not xs.size:
        return
    # Prefer sites that actually collected drainage, but keep it noisy so a
    # flat top does not always pan in the same corner.
    score = acc[xs, ys, zs] + rs.random(xs.size).astype(np.float32) * 0.6
    order = np.argsort(-score)[:4000]
    radius = max(2.0, depth_vox * 1.6)
    want = int(round(1 + amount * 5))

    chosen: list[tuple[int, int, int]] = []
    for idx in order:
        p = (int(xs[idx]), int(ys[idx]), int(zs[idx]))
        if all((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 > (2.2 * radius) ** 2
               for q in chosen):
            chosen.append(p)
        if len(chosen) >= want:
            break

    for px, py, pz in chosen:
        r = radius * (0.7 + 0.6 * rs.random())
        depth = depth_vox * (0.6 + 0.8 * rs.random())
        floor = pz - depth
        x0, x1 = max(0, int(px - r) - 2), min(nx, int(px + r) + 3)
        y0, y1 = max(0, int(py - r) - 2), min(ny, int(py + r) + 3)
        z0, z1 = max(0, int(floor)), min(nz, pz + 2)
        if x0 >= x1 or y0 >= y1 or z0 >= z1:
            continue
        dx = (np.arange(x0, x1) + 0.5 - px)[:, None, None]
        dy = (np.arange(y0, y1) + 0.5 - py)[None, :, None]
        zz = (np.arange(z0, z1) + 0.5)[None, None, :]
        # Wider lower down, so the rim is undercut rather than a straight bore.
        widen = 1.0 + 0.18 * np.clip((pz - zz) / max(depth, 1.0), 0.0, 1.0)
        grid.data[x0:x1, y0:y1, z0:z1][
            (dx * dx + dy * dy) <= (r * widen) ** 2] = 0
