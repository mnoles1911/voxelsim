"""Artifacts: rigid human-made craft. A lofted hull, or a panel on spars.

THE FIRST KIND HERE THAT WAS NEVER ALIVE. Everything else in this package grows
or erodes: a tree colonises space, a rock accretes and is carved, a tuft sprays
stems, an animal is a body with parts hung off it at angles. A canoe does none
of that. It was drawn, once, by a person, out of two curves — and the whole
reason a canoe reads as a canoe is that those curves are *fair*: continuous,
monotone where they should be, and with no step in them anywhere.

That single fact decides the shape of this file.

WHAT IS DELIBERATELY ABSENT
---------------------------
No recursion. No growth model. No habit table. No allometry. No competition, no
targets, no iteration count. There is no per-voxel thinning pass and there will
not be one: `docs/` records what per-voxel thinning did to silhouettes at 2 cm,
and a hull plank is two voxels thick — a pass that removed voxels independently
would not thin it, it would perforate it.

HOW IT DRAWS, AND WHY THAT IS THE SHAPE RULE
--------------------------------------------
**Every surface here is a FIELD evaluated over the whole grid at once, not a
stack of stations.** A hull is the set of voxels satisfying

    keel(x) <= z <= sheer(x)   and   |y - centre(x)| <= halfbeam(x, z)

where all three are smooth closed-form functions of the continuous coordinate.
Nothing is drawn at a station and interpolated to the next one, so there is no
station to alias against — which is what "quantise the field, not the surface"
means, and it is why this generator cannot produce contour rings. The naive
alternative, and the one a boat-building analogy pushes you toward, is to loft
N stations and sweep between them; at 2.5 cm on a 4 m hull that is a ring every
few voxels, and the rings are the first thing a person sees.

The interior is the SAME three functions re-evaluated one shell thickness in,
and subtracted. Not an erosion, not a morphological open — an analytic inner
solid. That matters for the same reason: an erosion of a quantised surface
inherits every step the surface has, and doubles it.

Nothing is cut on a diagonal. Both families are axis-aligned in the grid: the
hull's length is +x, the wing's chord is +x and its span is ±y. A swept wing
gets its sweep from `x_le(y)`, a function, and never from rotating a drawn
shape — a rotated raster is exactly the off-axis cut this package has paid for
before.

Thresholds are by DEPTH, in metres and in voxels, never by quantile. The shell
is `shell_vox` voxels of plank; the gunwale stands `gunwale_vox` voxels proud;
the sail is `panel_vox` voxels thick. Not "the thinnest 8% of the hull".

WHAT IT REPORTS, AND WHY
------------------------
Every drawing step returns a voxel-count delta, and a step that was asked to
draw something and changed nothing is an ERROR carried out in `out["steps"]`
for `pipeline.health` to raise. That is not defensiveness, it is this project's
signature failure: a pass that runs, reports success, and does nothing. The
weathering pass in this same package removed 20 voxels of 90,000 for months.
Nine steps here can each fail that way — a gunwale band with zero depth, a
thwart placed above the sheer, a cross-bar at span 0, a strake subset that came
out empty — and every one of them leaves an asset that still builds, still
looks like a boat, and is missing a part.

SEED VARIATION IS SUBTLE AND IS NOT STRUCTURAL
----------------------------------------------
A canoe is a manufactured object; two of them are the same object. So a seed
moves three things and no more: the size, slightly, through the shared
`variation.*` sliders; which planking runs carry the second material; and how
far the centreline bows to one side (`artifact.asym_vox`, in VOXELS, default
0.8). The asymmetry is applied to the FIELD — the whole section slides — so it
cannot open a seam or shed a voxel the way a per-voxel jitter would.
"""

from __future__ import annotations

import math

import numpy as np

from . import materials
from .grid import VoxelGrid
from .spec import BY_PATH, get

# THE PARAMETER TABLE AND THIS FILE MUST NOT DRIFT, and this is the same guard
# `forge/ground.py` puts on the plant menus and `forge/rasterize.py` on foliage
# habits. Both directions cost something real: a path this file reads that the
# table does not carry returns None and crashes on the first build (loud, fine);
# a path the table carries that this file never reads is a DEAD SLIDER, which
# moves in the app, saves into the spec, changes the spec hash, and draws
# nothing. That second one is silent, and silent is the one that ships.
_READS: frozenset[str] = frozenset({
    "artifact.form", "artifact.length_m", "artifact.beam_m", "artifact.depth_m",
    "artifact.shell_vox", "artifact.asym_vox", "artifact.strake_share",
    "artifact.sheer", "artifact.rocker", "artifact.fullness", "artifact.entry",
    "artifact.bilge", "artifact.gunwale_m", "artifact.gunwale_vox",
    "artifact.thwarts", "artifact.thwart_w_m", "artifact.thwart_t_m",
    "artifact.deck_frac",
    "artifact.sweep", "artifact.taper", "artifact.panel_vox",
    "artifact.billow_m", "artifact.droop_m", "artifact.spar_r_m",
    "artifact.keel_r_m", "artifact.keel_m", "artifact.crossbar_at",
    "artifact.frame_drop_m", "artifact.frame_at", "artifact.frame_base_m",
    "artifact.frame_r_m", "artifact.seat", "artifact.seat_len_m",
})
_TABLE = frozenset(path for path, row in BY_PATH.items() if row.group == "artifact")
assert _READS == _TABLE, (
    "forge/artifact.py and spec.PARAMS group 'artifact' have drifted.\n"
    f"  in the table, never read (DEAD SLIDERS): {sorted(_TABLE - _READS)}\n"
    f"  read here, not in the table (will crash): {sorted(_READS - _TABLE)}")

_MATERIAL_SLOTS = ("materials.hull", "materials.trim", "materials.strake",
                   "materials.frame")
assert all(BY_PATH[s].choices == materials.ARTIFACT_NAMES for s in _MATERIAL_SLOTS), (
    "forge/spec.py: the four artifact material slots no longer offer the same "
    "menu. An out-of-menu choice is not refused, it is REPLACED with that row's "
    "default -- which is how four freshwater plants shipped in blossom pink "
    "(docs/aquatic-species.md 8.6a).")
assert all(n in materials.BY_NAME for n in materials.ARTIFACT_NAMES), (
    "forge/materials.py: ARTIFACT_NAMES lists something BY_NAME cannot resolve")

# The choice menu, mirrored, and checked the same way for the same reason: a
# form that falls through to a default is indistinguishable from one that works.
_FORMS = ("hull", "wing")
assert set(BY_PATH["artifact.form"].choices) == set(_FORMS), (
    "forge/spec.py: artifact.form offers a form this generator does not build")


# --- step accounting ---------------------------------------------------------


class _Steps:
    """Every drawing step's voxel delta, and whether it was allowed to be zero.

    `pipeline.health` reads this. A step declared `must_change` that changed
    nothing is reported by name as a problem, not swallowed. Repaints count as
    change: a gunwale that adds no voxels because the lip is flush but recolours
    the whole rim band DID something, and a gunwale that touches neither did
    not.
    """

    def __init__(self) -> None:
        self.rows: list[dict] = []

    def run(self, name: str, grid: VoxelGrid, fn, *, must_change: bool = True,
            note: str = "") -> dict:
        before = grid.data.copy()
        fn()
        changed = grid.data != before
        added = int(np.count_nonzero(changed & (before == 0)))
        repainted = int(np.count_nonzero(changed & (before != 0)))
        row = {"step": name, "added": added, "repainted": repainted,
               "must_change": bool(must_change), "note": note,
               "voxels_after": int(np.count_nonzero(grid.data))}
        self.rows.append(row)
        return row

    def note(self, name: str, note: str) -> None:
        """A step that was authored OFF. Recorded so the absence is visible in
        the stats rather than being an unexplained gap in the list."""
        self.rows.append({"step": name, "added": 0, "repainted": 0,
                          "must_change": False, "note": note,
                          "voxels_after": None})

    def silent(self) -> list[str]:
        return [f"{r['step']}: ran and changed no voxels"
                for r in self.rows if r["must_change"]
                and r["added"] + r["repainted"] == 0]


# --- shared helpers ----------------------------------------------------------


def _axes(lo: np.ndarray, shape: tuple[int, int, int], voxel_m: float):
    """Voxel-centre coordinates in metres, shaped for broadcasting."""
    x = lo[0] + (np.arange(shape[0]) + 0.5) * voxel_m
    y = lo[1] + (np.arange(shape[1]) + 0.5) * voxel_m
    z = lo[2] + (np.arange(shape[2]) + 0.5) * voxel_m
    return x[:, None, None], y[None, :, None], z[None, None, :]


def _shape_for(lo, hi, voxel_m: float) -> tuple[int, int, int]:
    return tuple(max(3, int(math.ceil((hi[a] - lo[a]) / voxel_m)) + 1)
                 for a in range(3))


def _paint(grid: VoxelGrid, mask: np.ndarray, mat: int) -> None:
    grid.data[mask] = mat


def _bands(rng, counts, share: float) -> set[int]:
    """Which planking runs carry the contrast material, this seed.

    `share` IS A SHARE OF THE PLANKING, NOT A SHARE OF THE RUNS, and that
    distinction is not pedantry -- it was measured. The bands are equal slices
    of the SECTION DEPTH, and a hull's section is not remotely uniform in area:
    the lowest band is the garboard, which on a canoe is the whole floor. Taking
    "one run in four" off a four-run hull took 58% of the voxels with it and
    produced a two-tone boat rather than a boat with a strake in it. Counting
    voxels instead makes the slider mean what its label says at every depth,
    section shape and pitch.
    """
    counts = [int(c) for c in counts]
    total = sum(counts)
    if share <= 0.0 or total <= 0:
        return set()
    budget = share * total
    order = [int(i) for i in rng.permutation(len(counts))]
    chosen: set[int] = set()
    spent = 0
    for i in order:
        if counts[i] and spent + counts[i] <= budget:
            chosen.add(i)
            spent += counts[i]
    if not chosen:
        # Asked for a strake and the smallest run is already over budget. Draw
        # the smallest one rather than nothing: a step that was authored ON and
        # draws nothing is the failure this whole file is written against, and
        # "your share was too small for one plank" is a thing the note can say
        # while the voxels still move.
        live = [i for i, c in enumerate(counts) if c]
        if live:
            chosen.add(min(live, key=lambda i: counts[i]))
    return chosen


def _to_vox(p, lo: np.ndarray, voxel_m: float) -> np.ndarray:
    return (np.asarray(p, dtype=float) - lo) / voxel_m - 0.5


def _polyline(grid: VoxelGrid, pts, r_vox: float, mat: int) -> None:
    for a, b in zip(pts[:-1], pts[1:]):
        grid.capsule(a, b, r_vox, r_vox, mat)


# --- the hull family ---------------------------------------------------------
#
# Three curves and one section rule, all closed form, all continuous.
#
#   keel(t)     the bottom of the boat, rising toward the ends -- ROCKER
#   sheer(t)    the top of the side, rising toward the ends    -- SHEER
#   halfbeam(t) the plan view, going to zero at both ends      -- both stems
#
# and at every station the section is one superellipse between the keel and the
# sheer, whose exponent is the only thing that says whether the boat has a round
# bottom or a flat floor. One exponent for the whole hull, because a per-station
# section table is precisely the thing that has to be interpolated, and an
# interpolated section table at this pitch is a stack of rings.


def _plan(u: np.ndarray, half_beam: float, fullness: float,
          entry: float) -> np.ndarray:
    """Half-breadth at the sheer, as a function of distance from amidships.

    `u` is 0 amidships and 1 at either end, so this is zero at both stems and
    the boat is double-ended by construction. `fullness` holds the beam out
    toward the ends; `entry` sharpens the stem.
    """
    return half_beam * np.power(np.clip(1.0 - np.power(u, fullness), 0.0, 1.0),
                                entry)


def _section(hb: np.ndarray, v: np.ndarray, bilge: float) -> np.ndarray:
    """Half-width at height fraction `v` of the section, given its half-breadth.

    Superellipse: (1-v)**bilge + (w/hb)**bilge = 1. Below 1 a hard V, 2 a
    circular quarter, above 3 a flat floor with a hard turn of bilge. It is
    zero at v=0 for every exponent -- a boat has a keel line, not a keel plank
    -- but the RATE differs enormously, which is what "flat floor" means here:
    at bilge 4, five per cent of the way up the section is already two thirds
    of the beam.
    """
    q = np.clip(1.0 - v, 0.0, 1.0)
    return hb * np.power(np.clip(1.0 - np.power(q, bilge), 0.0, 1.0), 1.0 / bilge)


def _build_hull(spec: dict, rng, voxel_m: float, steps: _Steps) -> VoxelGrid:
    L = float(get(spec, "artifact.length_m"))
    B = float(get(spec, "artifact.beam_m"))
    D = float(get(spec, "artifact.depth_m"))
    shell_vox = int(get(spec, "artifact.shell_vox"))
    bilge = float(get(spec, "artifact.bilge"))
    fullness = float(get(spec, "artifact.fullness"))
    entry = float(get(spec, "artifact.entry"))
    sheer_rise = float(get(spec, "artifact.sheer")) * D
    rocker_rise = float(get(spec, "artifact.rocker")) * D
    gun_m = float(get(spec, "artifact.gunwale_m"))
    gun_vox = int(get(spec, "artifact.gunwale_vox"))
    n_thwarts = int(get(spec, "artifact.thwarts"))
    thwart_w = float(get(spec, "artifact.thwart_w_m"))
    thwart_t = float(get(spec, "artifact.thwart_t_m"))
    deck_frac = float(get(spec, "artifact.deck_frac"))
    share = float(get(spec, "artifact.strake_share"))
    asym_vox = float(get(spec, "artifact.asym_vox"))

    mat_hull = materials.resolve(get(spec, "materials.hull"))
    mat_trim = materials.resolve(get(spec, "materials.trim"))
    mat_strake = materials.resolve(get(spec, "materials.strake"))

    shell = shell_vox * voxel_m
    over = gun_vox * voxel_m
    # ONE DRAW, ONE INDIVIDUAL: the whole centreline bows to one side by up to
    # `asym_vox` voxels, sinusoidally, zero at both stems. A boat that is
    # crooked at the ends is a broken boat; a boat that is a voxel fat amidships
    # on one side is a hand-built boat.
    asym = asym_vox * voxel_m * (float(rng.random()) * 2.0 - 1.0)

    pad = 2.0 * voxel_m
    half_y = B * 0.5 + over + abs(asym) + pad
    lo = np.array([-pad, -half_y, -pad])
    hi = np.array([L + pad, half_y, D + sheer_rise + pad])
    shape = _shape_for(lo, hi, voxel_m)
    grid = VoxelGrid(shape, (0, 0, 0), voxel_m)

    X, Y, Z = _axes(lo, shape, voxel_m)

    t = np.clip(X / L, 0.0, 1.0)
    u = np.abs(2.0 * t - 1.0)
    within = (X >= 0.0) & (X <= L)

    keel = rocker_rise * u ** 2
    top = D + sheer_rise * u ** 2
    hb = np.where(within, _plan(u, B * 0.5, fullness, entry), 0.0)
    cy = asym * np.sin(math.pi * t) * within

    depth_t = np.maximum(top - keel, 1e-9)
    v = (Z - keel) / depth_t
    dy = np.abs(Y - cy)

    outer = (v >= 0.0) & (v <= 1.0) & (dy <= _section(hb, v, bilge))

    hb_in = np.maximum(hb - shell, 0.0)
    keel_in = keel + shell
    # The inner solid reaches ABOVE the sheer, which is what leaves the boat
    # open at the top. Its depth is the same `depth_t`, so the wall thickness at
    # the rim is exactly `shell` rather than a number that drifts with the
    # sheer.
    v_in = (Z - keel_in) / depth_t
    inner = (v_in >= 0.0) & (dy <= _section(hb_in, np.minimum(v_in, 1.0), bilge))

    steps.run("hull shell", grid,
              lambda: _paint(grid, outer & ~inner, mat_hull),
              note=f"{shell_vox} voxel ({shell * 100:.1f} cm) planking")

    # --- contrast strakes ----------------------------------------------------
    # BANDS OF CONSTANT HEIGHT, which run bow to stern: a strake. NOT bands of
    # constant station, which would run around the girth: a contour ring, and
    # this package's oldest shape defect wearing a paint job. The distinction is
    # the whole reason this is written as a `v` band and not an `x` band.
    n_bands = int(np.clip(round(D / (2.5 * voxel_m)), 4, 16))
    band = np.where(grid.data == mat_hull,
                    np.clip((v * n_bands).astype(np.int32), 0, n_bands - 1), -1)
    counts = [int(np.count_nonzero(band == i)) for i in range(n_bands)]
    chosen = _bands(rng, counts, share)
    if chosen:
        pick = np.isin(band, sorted(chosen))
        took = sum(counts[i] for i in chosen) / max(sum(counts), 1)
        steps.run("contrast strakes", grid, lambda: _paint(grid, pick, mat_strake),
                  note=f"{len(chosen)} of {n_bands} runs, {took:.0%} of the planking")
    else:
        steps.note("contrast strakes",
                   f"strake_share is {share:g}: no run selected")

    # --- gunwale -------------------------------------------------------------
    if gun_m > 0.0 or gun_vox > 0:
        rim = ((Z <= top) & (Z >= top - gun_m) & within
               & (dy <= _section(hb, np.clip(v, 0.0, 1.0), bilge) + over)
               & ~inner)
        steps.run("gunwale", grid, lambda: _paint(grid, rim, mat_trim),
                  note=f"{gun_m * 100:.0f} cm deep, {gun_vox} voxel overhang")
    else:
        steps.note("gunwale", "gunwale_m and gunwale_vox are both 0")

    # --- end decks -----------------------------------------------------------
    if deck_frac > 0.0:
        deck_t = max(2.0 * voxel_m, gun_m)
        ends = within & ((t < deck_frac) | (t > 1.0 - deck_frac))
        deck = (ends & (Z <= top) & (Z >= top - deck_t)
                & (dy <= _section(hb, np.clip(v, 0.0, 1.0), bilge)))
        steps.run("end decks", grid, lambda: _paint(grid, deck, mat_trim),
                  note=f"{deck_frac:.0%} of the length at each end")
    else:
        steps.note("end decks", "deck_frac is 0")

    # --- thwarts -------------------------------------------------------------
    if n_thwarts > 0:
        seats = np.zeros(shape, dtype=bool)
        for k in range(n_thwarts):
            xk = L * (k + 1) / (n_thwarts + 1)
            seats |= (np.abs(X - xk) <= thwart_w * 0.5) & (Z <= top) \
                & (Z >= top - thwart_t) \
                & (dy <= _section(hb, np.clip(v, 0.0, 1.0), bilge))
        steps.run("thwarts", grid, lambda: _paint(grid, seats, mat_trim),
                  note=f"{n_thwarts} x {thwart_w * 100:.0f} cm")
    else:
        steps.note("thwarts", "thwarts is 0")

    return grid


# --- the wing family ---------------------------------------------------------
#
# A thin panel over a swept tapered plan, and a frame of tubes. The panel is a
# height field, `z = surface(x, y)`, and the fabric is the voxels within half a
# panel thickness of it -- so a sail with billow and droop is a smooth curved
# sheet, not a staircase of flat plates.
#
# THE SPARS ARE DRAWN ON THE SURFACE, NOT BESIDE IT. Every tube's centreline is
# sampled from the same `surface()` the fabric came from, so the sail and the
# airframe are one face-connected piece BY CONSTRUCTION. The obvious
# alternative -- draw the tubes at their true offset under the sail -- produces
# an asset that is two pieces about half the time and one piece the rest, which
# is exactly the kind of intermittent failure a single-seed check cannot see.


def _wing_geometry(spec: dict):
    """The plan and the surface, as closures over the authored numbers."""
    c0 = float(get(spec, "artifact.length_m"))
    half = float(get(spec, "artifact.beam_m")) * 0.5
    sweep = float(get(spec, "artifact.sweep"))
    taper = float(get(spec, "artifact.taper"))
    billow = float(get(spec, "artifact.billow_m"))
    droop = float(get(spec, "artifact.droop_m"))

    def x_le(s):
        return sweep * s

    def chord(s):
        return c0 + (taper * c0 - c0) * (s / max(half, 1e-9))

    def surface(x, s, bias=0.0, side=0.0):
        """Height of the sail at chordwise x and spanwise distance s.

        Two authored terms and no more. `droop` lowers the tips as the square
        of the span fraction -- anhedral, and the thing that makes a hang
        glider read as a hang glider from the front. `billow` bows the sail
        between the leading edge and the trailing edge, tapering slightly
        outboard because a short tip chord cannot carry the same belly as the
        root.

        `bias` is the seed's asymmetry and it is DIFFERENTIAL TIP HEIGHT, not a
        bent coordinate: one tip sits a couple of centimetres lower than the
        other, as a rigged sail does, and the centreline stays exactly on the
        centreline. Warping the span coordinate instead -- which is what the
        hull's `asym` does, correctly, because a hull HAS no centreline
        structure -- would put the keel tube off the sail's own centre and open
        a seam between the two.
        """
        f = np.clip(s / max(half, 1e-9), 0.0, 1.0)
        c = np.clip((x - x_le(s)) / np.maximum(chord(s), 1e-9), 0.0, 1.0)
        return (-droop * f ** 2
                + bias * side * f ** 2
                + billow * np.sin(math.pi * c) * (1.0 - 0.25 * f ** 2))

    return c0, half, x_le, chord, surface


def _build_wing(spec: dict, rng, voxel_m: float, steps: _Steps) -> VoxelGrid:
    c0, half, x_le, chord, surface = _wing_geometry(spec)
    keel_over = float(get(spec, "artifact.keel_m"))
    panel_vox = int(get(spec, "artifact.panel_vox"))
    droop = float(get(spec, "artifact.droop_m"))
    billow = float(get(spec, "artifact.billow_m"))
    spar_r = float(get(spec, "artifact.spar_r_m"))
    keel_r = float(get(spec, "artifact.keel_r_m"))
    cross_at = float(get(spec, "artifact.crossbar_at"))
    drop = float(get(spec, "artifact.frame_drop_m"))
    frame_at = float(get(spec, "artifact.frame_at"))
    base_m = float(get(spec, "artifact.frame_base_m"))
    frame_r = float(get(spec, "artifact.frame_r_m"))
    want_seat = bool(get(spec, "artifact.seat"))
    seat_len = float(get(spec, "artifact.seat_len_m"))
    share = float(get(spec, "artifact.strake_share"))
    asym_vox = float(get(spec, "artifact.asym_vox"))

    mat_sail = materials.resolve(get(spec, "materials.hull"))
    mat_trim = materials.resolve(get(spec, "materials.trim"))
    mat_panel = materials.resolve(get(spec, "materials.strake"))
    mat_frame = materials.resolve(get(spec, "materials.frame"))

    # A ONE-VOXEL SHEET THROUGH A SLOPE IS A SIEVE, and the arithmetic is not
    # subtle enough to leave to the author. The fabric is a VERTICAL band of
    # half-height h about a sloping surface, so two neighbouring voxel columns
    # only share a face if the surface moved less than 2h between them:
    # 2h >= voxel * |grad z|, i.e. panel_vox >= |grad z|. Measured off the
    # authored curves rather than guessed, raised if short, and SAID so in the
    # step note -- silently drawing a perforated sail would pass every
    # connectivity check this package has, because the holes are one voxel wide
    # and the sheet stays one piece around them.
    tip_chord = max(chord(half), 1e-9)
    grad = math.hypot(2.0 * abs(droop) / max(half, 1e-9),
                      abs(billow) * math.pi / tip_chord)
    need_vox = max(panel_vox, int(math.ceil(grad)))
    half_t = need_vox * voxel_m * 0.5
    asym = asym_vox * voxel_m * (float(rng.random()) * 2.0 - 1.0)

    pad = 2.0 * voxel_m
    x_max = max(x_le(half) + chord(half), c0 + keel_over) + pad
    reach = abs(droop) + abs(billow) + abs(asym) + half_t
    z_lo = min(-reach, -drop - reach) - pad
    z_hi = reach + pad
    lo = np.array([-pad, -(half + pad), z_lo])
    hi = np.array([x_max, half + pad, z_hi])
    shape = _shape_for(lo, hi, voxel_m)
    grid = VoxelGrid(shape, (0, 0, 0), voxel_m)

    X, Y, Z = _axes(lo, shape, voxel_m)
    s = np.abs(Y)
    side = np.sign(Y)
    plan = (s <= half) & (X >= x_le(s)) & (X <= x_le(s) + chord(s))
    surf = surface(X, s, asym, side)

    sail = plan & (np.abs(Z - surf) <= half_t)
    steps.run("sail panel", grid, lambda: _paint(grid, sail, mat_sail),
              note=(f"{need_vox} voxel fabric"
                    + (f" (raised from {panel_vox}: surface gradient {grad:.2f})"
                       if need_vox != panel_vox else "")))

    # --- sail panels ---------------------------------------------------------
    # SPANWISE BANDS, so the seams run chordwise -- which is how a sail is
    # actually cut and sewn. The hull's strakes are the same idea rotated: bands
    # that follow the long axis of the object, never bands that ring it.
    n_bands = int(np.clip(round(half / 0.4), 3, 12))
    band = np.where(grid.data == mat_sail,
                    np.clip((s / max(half, 1e-9) * n_bands).astype(np.int32),
                            0, n_bands - 1), -1)
    counts = [int(np.count_nonzero(band == i)) for i in range(n_bands)]
    chosen = _bands(rng, counts, share)
    if chosen:
        pick = np.isin(band, sorted(chosen))
        took = sum(counts[i] for i in chosen) / max(sum(counts), 1)
        steps.run("sail panels", grid, lambda: _paint(grid, pick, mat_panel),
                  note=f"{len(chosen)} of {n_bands} panels, {took:.0%} of the sail")
    else:
        steps.note("sail panels", f"strake_share is {share:g}: no panel selected")

    def vx(x, y, z):
        return _to_vox((x, y, z), lo, voxel_m)

    # --- leading-edge spars --------------------------------------------------
    n_seg = max(8, int(half / max(voxel_m * 4.0, 1e-9)))
    def draw_le():
        for sign in (-1.0, 1.0):
            pts = []
            for i in range(n_seg + 1):
                sv = half * i / n_seg
                pts.append(vx(x_le(sv), sign * sv,
                              float(surface(x_le(sv), sv, asym, sign))))
            _polyline(grid, pts, spar_r / voxel_m, mat_frame)
    steps.run("leading-edge spars", grid, draw_le,
              note=f"2 x {spar_r * 100:.1f} cm radius")

    # --- keel spar -----------------------------------------------------------
    def draw_keel():
        pts = []
        n = max(8, int((c0 + keel_over) / max(voxel_m * 4.0, 1e-9)))
        for i in range(n + 1):
            xv = (c0 + keel_over) * i / n
            zv = float(surface(min(xv, c0), 0.0, asym, 0.0))
            pts.append(vx(xv, 0.0, zv))
        _polyline(grid, pts, keel_r / voxel_m, mat_frame)
    steps.run("keel spar", grid, draw_keel,
              note=f"{c0 + keel_over:.2f} m, {keel_over * 100:.0f} cm aft of the sail")

    # --- cross-bar -----------------------------------------------------------
    if cross_at > 0.0:
        sc = cross_at * half
        xc = x_le(sc)
        def draw_cross():
            pts = []
            n = max(8, int(2.0 * sc / max(voxel_m * 4.0, 1e-9)))
            for i in range(n + 1):
                yv = -sc + 2.0 * sc * i / n
                pts.append(vx(xc, yv,
                              float(surface(xc, abs(yv), asym,
                                            math.copysign(1.0, yv)))))
            _polyline(grid, pts, frame_r / voxel_m, mat_frame)
        steps.run("cross-bar", grid, draw_cross,
                  note=f"at {cross_at:.0%} of the half-span")
    else:
        steps.note("cross-bar", "crossbar_at is 0")

    # --- control frame -------------------------------------------------------
    apex_x = frame_at * c0
    apex_z = float(surface(apex_x, 0.0, asym, 0.0))
    foot_z = apex_z - drop
    if drop > 0.0:
        def draw_frame():
            for sign in (-1.0, 1.0):
                grid.capsule(vx(apex_x, 0.0, apex_z),
                             vx(apex_x, sign * base_m * 0.5, foot_z),
                             frame_r / voxel_m, frame_r / voxel_m, mat_frame)
            grid.capsule(vx(apex_x, -base_m * 0.5, foot_z),
                         vx(apex_x, base_m * 0.5, foot_z),
                         frame_r / voxel_m, frame_r / voxel_m, mat_frame)
        steps.run("control frame", grid, draw_frame,
                  note=f"{drop:.2f} m drop, {base_m:.2f} m base bar")
    else:
        steps.note("control frame", "frame_drop_m is 0")

    # --- seat ----------------------------------------------------------------
    if want_seat:
        seat_x = apex_x + 0.18 * c0
        seat_z = apex_z - drop * 0.55
        strap_z = float(surface(min(seat_x, c0), 0.0, asym, 0.0))
        seat_half_w = 0.17
        seat_t = 3.0 * voxel_m

        def draw_seat():
            grid.capsule(vx(seat_x, 0.0, strap_z), vx(seat_x, 0.0, seat_z),
                         max(frame_r / voxel_m * 0.6, 0.5),
                         max(frame_r / voxel_m * 0.6, 0.5), mat_frame)
            slab = ((np.abs(X - seat_x) <= seat_len * 0.5)
                    & (np.abs(Y) <= seat_half_w)
                    & (Z <= seat_z) & (Z >= seat_z - seat_t))
            _paint(grid, slab, mat_trim)
        steps.run("seat", grid, draw_seat,
                  note=f"{seat_len:.2f} m on a strap")
    else:
        steps.note("seat", "seat is off")

    return grid


# --- entry point -------------------------------------------------------------


def build(spec: dict, rng, voxel_m: float, out: dict | None = None) -> VoxelGrid:
    """One craft. Deterministic in (spec, seed) exactly as every kind here is.

    `out["steps"]` carries the per-step voxel deltas; `pipeline.build` copies
    them into the stats and `pipeline.health` raises any step that ran and
    changed nothing.
    """
    form = str(get(spec, "artifact.form"))
    steps = _Steps()
    if form == "wing":
        grid = _build_wing(spec, rng, voxel_m, steps)
    else:
        grid = _build_hull(spec, rng, voxel_m, steps)
    if out is not None:
        out["steps"] = steps.rows
        out["silent_steps"] = steps.silent()
        out["form"] = form
    return grid


def hollow_fraction(grid: VoxelGrid) -> float:
    """Share of the asset's bounding box that is enclosed air.

    THE ONE MEASUREMENT A RENDER CANNOT MAKE. A canoe is supposed to be a
    shell: a player sits in it. A solid block of hull voxels renders as a
    perfectly good canoe from every camera this package has, and the only way
    to tell the difference from outside is to count. Zero means the hollowing
    step did nothing -- the silent no-op wearing the shape of a boat.

    Air that is enclosed by the asset in the X-Y plane at each height, which is
    the right question for an open-topped hull: a flood fill from the outside in
    3-D would run in through the open top and report a canoe as solid.
    """
    from scipy import ndimage

    occ = grid.data != 0
    if not occ.any():
        return 0.0
    enclosed = 0
    for z in range(occ.shape[2]):
        layer = occ[:, :, z]
        if not layer.any():
            continue
        holes = ndimage.binary_fill_holes(layer) & ~layer
        enclosed += int(holes.sum())
    return enclosed / float(max(int(occ.sum()) + enclosed, 1))
