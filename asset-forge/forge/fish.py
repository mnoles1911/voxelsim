"""Fish: small swimming detail entities, drawn to read at 20-40 voxels long.

Nothing here grows a skeleton and nothing here is a rock. A fish is *a solid
whose cross-section changes along a straight axis* — a loft — plus a handful of
thin plates stuck on it, plus a colour scheme. That is three separate jobs and
they are kept separate in the code, because at this size they fail in completely
different ways.

The axis is straight, not curved. A real fish bends, and a swimming one bends
constantly, but this generator produces the ASSET and whatever animates it will
bend it; building the bend in would freeze one frame of a swimming cycle into
every individual of the species.

WHAT THIS IS BUILT FOR, AND WHAT IT IS NOT. The reference is a shoal of cubic
fish twenty to thirty voxels long in dark water. At that size a fish has roughly
600 solid voxels and about 120 of them are on the silhouette. That budget cannot
carry anatomy, so it is spent on the three things the eye actually uses to tell
one fish from another at a glance:

  1. the OUTLINE — how deep the body is, where it is deepest, how the tail is
     shaped;
  2. the COLOUR SCHEME — dark back, pale belly, and one strong mark (a stripe, a
     row of bars, spots);
  3. the EYE — one dark voxel with a pale voxel beside it.

Everything else the ichthyology literature offers (elliptic Fourier outlines,
Turing patterns, superformula cross-sections with six free exponents) describes
detail an order of magnitude finer than one voxel here. `docs/fish-shape-
research.md` says which of it was adopted, which was rejected, and why.

THE FOUR THINGS THAT WERE GOT WRONG FIRST, kept here because each one is a trap
the next person will fall into as well. The fourth was found by a machine and
the other three were not, which is the argument for `tools/fishprobe.py`.

**A fin drawn at its true position falls off.** Every fin is a plate one voxel
thick standing on a curved body. Placing it *at* the body surface leaves it
touching at a corner or not at all, depending on which side of a rounding
decision the surface landed — and a fin that comes off is a second asset, which
`tools/buildcheck.py` rejects. So every fin here is drawn starting ONE VOXEL
INSIDE the body it hangs off. That is the same rule `grid.capsule` uses when it
lays a branch's centreline down before thickening it: connectivity is
guaranteed by construction, never measured afterwards and hoped for.

**A body drawn only by its cross-section can vanish.** The snout and the caudal
peduncle are the thinnest parts of a fish, and at 1 cm they are one or two
voxels across. A superellipse of half-width 0.4 voxels contains no cell centre
at all, so the nose and the tail wrist came out as gaps and the fish shipped in
three pieces. The body axis is therefore stamped as a solid one-voxel run FIRST,
along every station, and the cross-sections are added to it.

**The tail-notch slider ran backwards from its own help text**, and nothing
about the renders said so — a fish with the wrong fork depth is still a fish.
`fish.caudal_fork` is documented as the share of the fin's length the V eats
into, so 0.6 should be a deep lunate fork; it was written as "the notch begins
at `fork`", which made 0.6 a shallow one. Every species had been authored
against the documentation, so all ten were getting the opposite of what they
asked for. `tools/fishprobe.py` caught it as a measurement that was identical at
both ends of the slider's range, which is what a backwards parameter looks like
from the outside when its effect is also clamped.

**Colour is a per-voxel material, not a texture.** ADR-0008 gives every voxel
face one flat colour from `vxc::kMaterialPalette`, so a fish's markings have to
be an assignment of material IDs to voxels. There is no second colour path here
and there must never be one. The skin materials this uses do not exist in the
engine yet — see `forge/materials.py` — exactly as bark and the leaf variants
did not when the trees were written.
"""

from __future__ import annotations

import math

import numpy as np

from . import materials
from . import parts
from .grid import VoxelGrid
from .spec import (_CAUDAL_SHAPES, _DORSAL_SHAPES, _FIELD_CURVES,
                   _FISH_PATTERNS, _SEXES, get)

# Caudal fin outlines. The names are the ones ichthyology uses, and they are
# choices rather than a single "forkiness" number because `rounded` and `forked`
# are not two ends of one axis — a rounded tail is convex where a forked one is
# concave. Lunate and emarginate ARE the same axis (they are a deep and a
# shallow fork), so they are not separate entries: they come out of
# `fish.caudal_fork`, which is documented to say so.
CAUDAL_SHAPES = ("forked", "truncate", "rounded", "pointed", "none")

# Dorsal fin outlines. Same reasoning: these differ in WHERE along the fin the
# height is, which no single slider expresses.
DORSAL_SHAPES = ("triangular", "sail", "spiny", "ridge", "none")

# Colour patterns. See `_paint`.
PATTERNS = ("none", "stripe", "bars", "spots", "mottle", "saddle")

# Shapes the boundary between two colour fields may take. See `_field_lines`.
# These are NOT another entry in `PATTERNS`, and the difference is the whole
# point: a pattern is ink laid ON a field, and these bend the EDGE BETWEEN two
# fields. A cape drawn as a blotch would have to be re-drawn every time the
# countershading moved.
FIELD_CURVES = ("flat", "cape", "flame", "hourglass")

# Which sex of the species is being drawn. See `_sex_scale`.
SEXES = ("unsexed", "female", "male")

# The parameter table offers these as choices and this file is what implements
# them. Checked at import rather than trusted, because a shape name that falls
# through to a default LOOKS like a shape that works -- which is exactly how
# `spire` and `ovoid` crowns rendered as spheres for as long as they did.
assert set(CAUDAL_SHAPES) == set(_CAUDAL_SHAPES), (
    f"fish.py CAUDAL_SHAPES and spec.py _CAUDAL_SHAPES disagree: "
    f"{set(CAUDAL_SHAPES) ^ set(_CAUDAL_SHAPES)}")
assert set(DORSAL_SHAPES) == set(_DORSAL_SHAPES), (
    f"fish.py DORSAL_SHAPES and spec.py _DORSAL_SHAPES disagree: "
    f"{set(DORSAL_SHAPES) ^ set(_DORSAL_SHAPES)}")
assert set(PATTERNS) == set(_FISH_PATTERNS), (
    f"fish.py PATTERNS and spec.py _FISH_PATTERNS disagree: "
    f"{set(PATTERNS) ^ set(_FISH_PATTERNS)}")
assert set(FIELD_CURVES) == set(_FIELD_CURVES), (
    f"fish.py FIELD_CURVES and spec.py _FIELD_CURVES disagree: "
    f"{set(FIELD_CURVES) ^ set(_FIELD_CURVES)}")
assert set(SEXES) == set(_SEXES), (
    f"fish.py SEXES and spec.py _SEXES disagree: {set(SEXES) ^ set(_SEXES)}")


def build(spec: dict, rng: np.random.Generator, voxel_m: float,
          out: dict | None = None) -> VoxelGrid:
    """One fish, nose at +x, back at +z, seen broadside from -y.

    `out`, when given, collects the part tags on the way past -- `out["tags"]`
    is a uint8 array parallel to the returned grid, valued by the `P_*`
    constants. Same pass as the geometry, so the two cannot drift.

    The orientation is fixed rather than random because a fish is the first
    asset here with a FRONT. A rock has no correct way round and a tree is a
    surface of revolution to within its own lopsidedness, so both are given a
    random facing in `spec.realize`. A fish that spawns facing a random way is
    the job of whatever swims it, and giving it one here would only mean the
    preview camera has to search for the side view every time.
    """
    p = _params(spec, rng, voxel_m)
    grid = VoxelGrid(p["shape"], (0, 0, 0), voxel_m)

    body = _body(p)
    fins, fin_kind = _fins(p, body)
    _paint(grid, p, body, fins, fin_kind)
    _barbels(grid, p)
    if out is not None:
        # Body first, fins over it. A fin starts ONE VOXEL INSIDE the body (see
        # `_fins`), so the overlap is deliberate and those voxels belong to the
        # fin -- they are what holds it on, and a fin whose root is tagged body
        # would tear off the moment it rotated.
        tags = np.where(grid.data != 0, np.uint8(P_BODY), np.uint8(P_NONE))
        tags[fins] = fin_kind[fins]
        tags[grid.data == 0] = P_NONE
        out["tags"] = parts.to_shared(tags, _TO_SHARED)
    return grid


# --- parameters -------------------------------------------------------------


def _sex_scale(spec: dict, ratio_path: str) -> float:
    """Multiplier on one measurement for the sex this asset is being drawn as.

    THE AUTHORED NUMBERS ARE THE SPECIES AVERAGE AND THE RATIO IS SPLIT BOTH
    WAYS. Each `fish.sex_*` row is a male-to-female ratio, so the male gets
    `sqrt(r)` and the female `1/sqrt(r)`: male divided by female is exactly `r`
    whatever the authored value is, and `unsexed` is the geometric mean of the
    two. Neither sex is the default, which is the whole reason for the square
    root -- the obvious version, "the authored animal is the female and the male
    is scaled up", makes every unsexed spec in the library silently female.

    IT WAS SILENTLY THE OTHER WAY ROUND BEFORE THIS EXISTED. `orca`,
    `whale-shark` and `sperm-whale` were each authored from a male reference and
    each said so in its own notes -- an orca's dorsal fin at 26% of body length
    is a bull's, a female's is 15% -- so "the orca" in this library was a male
    orca. Adding the sexes therefore meant re-authoring those three onto the
    average; `docs/marine-marking-research.md` has the before and after.

    A species with no measured difference leaves the ratios at 1.0, and then
    this returns 1.0 for all three sexes and the choice genuinely changes
    nothing. That is a real answer and not a broken one -- `tools/fishprobe.py
    --sex` prints the per-species movement in voxels so that "changes nothing"
    is a measurement rather than an assumption.
    """
    sex = str(get(spec, "fish.sex"))
    if sex == "unsexed":
        return 1.0
    r = max(float(get(spec, ratio_path)), 1e-3)
    return math.sqrt(r) if sex == "male" else 1.0 / math.sqrt(r)


def _params(spec: dict, rng: np.random.Generator, voxel_m: float) -> dict:
    """Everything the drawing code needs, in VOXELS, with this individual's
    variation already applied.

    Variation is folded in HERE rather than in `spec.realize`, because that
    function varies a tree: it moves `height_m`, the crown radius, the trunk
    radius and the foliage shells, none of which a fish has. Left to it, every
    seed of a species would be the same fish with a different pattern phase —
    which is precisely the failure the `variation` group was added to fix for
    trees ("the first 100-seed sheet came out as a hundred oaks all 14.2 m
    tall"). The three shared sliders it reuses are named in the help text on
    each of them, so nothing here is a hidden coupling.
    """
    amount = float(get(spec, "variation.amount"))

    def u() -> float:
        # Same draw `spec.realize` uses: pushed away from the middle, so a
        # batch of eight does not pile up on the species average.
        t = float(rng.random()) * 2.0 - 1.0
        return abs(t) ** 0.6 if t >= 0.0 else -(abs(t) ** 0.6)

    def vary(value: float, slider: str, scale: float = 1.0) -> float:
        return value * (1.0 + amount * float(get(spec, slider)) * scale * u())

    # Sex is applied BEFORE the individual variation draw, so a male and a
    # female of one species are two draws around two different means rather
    # than one draw scaled -- which is what "two animals" has to mean if the
    # word is doing any work. See `_sex_scale` and `fish.sex`'s help text.
    length_m = max(vary(float(get(spec, "fish.length_m"))
                        * _sex_scale(spec, "fish.sex_length"),
                        "variation.height"),
                   voxel_m * 5.0)
    depth_ratio = vary(float(get(spec, "fish.depth_ratio")), "variation.shape")
    width_ratio = vary(float(get(spec, "fish.width_ratio")), "variation.shape", 0.6)
    depth_at = min(0.80, max(0.12, vary(float(get(spec, "fish.depth_at")),
                                        "variation.proportion", 0.5)))
    peduncle = min(0.95, max(0.05, vary(float(get(spec, "fish.peduncle")),
                                        "variation.proportion", 0.4)))

    v = float(voxel_m)
    length_v = max(5.0, length_m / v)
    depth_v = max(2.0, length_m * depth_ratio / v)
    width_v = max(1.0, depth_v * width_ratio)

    caudal_len = float(get(spec, "fish.caudal_len"))
    caudal_span = float(get(spec, "fish.caudal_span"))
    dorsal_h = float(get(spec, "fish.dorsal_height")) * _sex_scale(
        spec, "fish.sex_dorsal")
    anal_h = float(get(spec, "fish.anal_height"))
    pect = float(get(spec, "fish.pectoral")) * _sex_scale(
        spec, "fish.sex_pectoral")
    belly = float(get(spec, "fish.belly"))
    # Tip to tip across the head, in voxels. Zero on everything but a
    # hammerhead, and the grid has to be sized against it or the wing is
    # clipped -- see `across` below.
    head_span_v = float(get(spec, "fish.head_width")) * (length_m / voxel_m)
    barbels = int(get(spec, "fish.barbels"))
    barbel_v = float(get(spec, "fish.barbel_len")) * length_v if barbels else 0.0

    nb = int(round(length_v))                       # body columns
    lc = int(round(caudal_len * length_v)) if get(spec, "fish.caudal_shape") != "none" else 0
    margin = 2

    # The grid is sized from what will actually be drawn and then cropped by the
    # pipeline, so being generous here costs a few thousand empty cells and
    # nothing else. A whole fish is under 0.5 MB even before the crop.
    #
    # Sized against the OUTLINE the drawing code will produce, not against a
    # round number. A barbel or a tall dorsal that runs off the edge of the grid
    # is clipped silently by `VoxelGrid._write`, which is exactly the class of
    # failure this project keeps shipping: the feature runs, reports success,
    # and half of it is not there.
    # A HORIZONTAL FLUKE SPENDS ITS SPAN ACROSS THE GRID, NOT UP IT, so the
    # box has to be sized on the axis the tail actually uses. Sized against the
    # vertical one regardless, a dolphin's fluke ran off both sides of the grid
    # and `VoxelGrid._write` clipped it silently -- the animal came out with
    # two stubs and reported success.
    fluke = str(get(spec, "fish.caudal_plane")) == "horizontal"
    tail_up = 0.0 if fluke else 0.5 * caudal_span
    up = depth_v * max(1.0, tail_up, (1.0 - belly) + dorsal_h)
    down = depth_v * max(1.0, tail_up, belly + anal_h,
                         belly + float(get(spec, "fish.pelvic")))
    across = (width_v + depth_v * pect * 2.0 + 2.0
              + (depth_v * caudal_span if fluke else 0.0))
    # A CEPHALOFOIL SPENDS ITS WHOLE SPAN ACROSS THE GRID, exactly as a
    # horizontal fluke does, and it is wider than anything else on the animal:
    # on a hammerhead the head is 39% of body length across against a body 9%
    # wide. Sized against the body's width alone the wing ran off both sides
    # and `VoxelGrid._write` clipped it in silence, which is this file's
    # documented failure mode and the reason the fluke line above exists.
    across = max(across, head_span_v + 2.0)
    nose_pad = margin + int(math.ceil(barbel_v * 0.6))

    nx = nb + lc + margin + nose_pad
    ny = int(round(across)) + 2 * margin
    nz = int(round(up + down + barbel_v * 0.7)) + 2 * margin

    return {
        "voxel_m": v,
        "shape": (max(nx, 5), max(ny, 5), max(nz, 5)),
        "nb": max(nb, 4),
        "lc": lc,
        "xtail": margin + lc,
        "xnose": margin + lc + max(nb, 4) - 1,
        "ycen": ny * 0.5,
        "zaxis": margin + down,
        "length_v": length_v,
        "depth_v": depth_v,
        "width_v": width_v,
        "snout": float(get(spec, "fish.snout")),
        "peduncle": peduncle,
        "depth_at": depth_at,
        "fullness": float(get(spec, "fish.fullness")),
        "belly": belly,
        "width_follow": float(get(spec, "fish.width_follow")),
        "section": float(get(spec, "fish.section")),
        "section_tail": float(get(spec, "fish.section_tail")),
        "head_frac": float(get(spec, "fish.head_frac")),
        "head_span_v": head_span_v,
        "caudal_shape": str(get(spec, "fish.caudal_shape")),
        "caudal_fork": float(get(spec, "fish.caudal_fork")),
        "caudal_span": caudal_span,
        "caudal_plane": str(get(spec, "fish.caudal_plane")),
        "caudal_upper": float(get(spec, "fish.caudal_upper")),
        "dorsal_shape": str(get(spec, "fish.dorsal_shape")),
        "dorsal_start": float(get(spec, "fish.dorsal_start")),
        "dorsal_len": float(get(spec, "fish.dorsal_len")),
        "dorsal_height": dorsal_h,
        "adipose": bool(get(spec, "fish.adipose")),
        "dorsal2_start": float(get(spec, "fish.dorsal2_start")),
        "dorsal2_len": float(get(spec, "fish.dorsal2_len")),
        "dorsal2_height": float(get(spec, "fish.dorsal2_height")),
        "anal_height": anal_h,
        "anal_len": float(get(spec, "fish.anal_len")),
        "pectoral": pect,
        "pectoral_aspect": float(get(spec, "fish.pectoral_aspect")),
        "pelvic": float(get(spec, "fish.pelvic")),
        "barbels": int(get(spec, "fish.barbels")),
        "barbel_len": float(get(spec, "fish.barbel_len")),
        "fin_thick": int(get(spec, "fish.fin_thick")),
        "eye": float(get(spec, "fish.eye")),
        "eye_patch": float(get(spec, "fish.eye_patch")),
        "blowhole": float(get(spec, "fish.blowhole")),
        "fin_min_vox": float(get(spec, "fish.fin_min_vox")),
        "back_frac": float(get(spec, "fish.back_frac")),
        "belly_frac": float(get(spec, "fish.belly_frac")),
        "field_curve": str(get(spec, "fish.field_curve")),
        "curve_at": float(get(spec, "fish.curve_at")),
        "curve_amount": float(get(spec, "fish.curve_amount")),
        "pattern": str(get(spec, "fish.pattern")),
        "pattern_count": int(get(spec, "fish.pattern_count")),
        "pattern_width": float(get(spec, "fish.pattern_width")),
        "pattern_pos": float(get(spec, "fish.pattern_pos")),
        "pattern_scale": float(get(spec, "fish.pattern_scale")),
        "pattern_strength": float(get(spec, "fish.pattern_strength")),
        "phase": float(rng.random()),
        "salt": int(rng.integers(1 << 30)),
        "mat_back": materials.resolve(get(spec, "materials.fish_back")),
        "mat_flank": materials.resolve(get(spec, "materials.fish_flank")),
        "mat_belly": materials.resolve(get(spec, "materials.fish_belly")),
        "mat_fin": materials.resolve(get(spec, "materials.fish_fin")),
        "mat_pattern": materials.resolve(get(spec, "materials.fish_pattern")),
        "mat_eye": materials.resolve(get(spec, "materials.fish_eye")),
        "mat_patch": materials.resolve(get(spec, "materials.fish_patch")),
    }


# --- the body ---------------------------------------------------------------


def _depth_profile(t: np.ndarray, p: dict) -> np.ndarray:
    """Body depth along the fish, as a fraction of its maximum. t=0 is the snout.

    THIS IS THE ONE FUNCTION THAT DECIDES WHAT KIND OF FISH IT IS. Fish
    morphometrics classifies bodies by exactly this curve — fusiform (a trout: a
    shallow bulge a third of the way back), compressiform (a perch or a bream:
    a deep bulge), anguilliform (an eel: almost no bulge at all), sagittiform (a
    pike: the bulge pushed far back toward the tail). All four are this formula
    with different numbers, which is why the shape family is a handful of
    sliders here rather than a menu of hand-drawn outlines.

        base(t) = snout + (peduncle - snout) * t          a straight taper
        bump(t) = (t/p)^a * ((1-t)/(1-p))^b               peaks at t = p
        D(t)    = base + (1 - base) * bump

    with a = fullness*p and b = fullness*(1-p), which is what makes the bump
    equal exactly 1 at t = p whatever p is. So `depth_at` moves the deepest
    point without changing how deep it is, and `fullness` changes how fast the
    body swells without moving the deepest point. Those two being independent
    is the whole reason for this form: the obvious version — a sine or a beta
    curve scaled to fit — moves both at once, and tuning a species then means
    chasing one slider with another.

    A monotone spline through control points (PCHIP, which scipy has) was tried
    first and abandoned: it needs five numbers to say what three say here, and
    at twenty voxels of length the extra freedom lands entirely inside a single
    voxel of depth.
    """
    q = min(max(p["depth_at"], 0.05), 0.95)
    s = max(p["fullness"], 0.2)
    base = p["snout"] + (p["peduncle"] - p["snout"]) * t
    # Clipped away from the exact ends so the powers stay finite; the body's
    # first and last column are the snout and the wrist, and both are `base`.
    tt = np.clip(t, 1e-6, 1.0 - 1e-6)
    bump = (tt / q) ** (s * q) * ((1.0 - tt) / (1.0 - q)) ** (s * (1.0 - q))
    return np.clip(base + (1.0 - base) * bump, 0.02, 1.0)


def _width_profile(t: np.ndarray, depth: np.ndarray, p: dict) -> np.ndarray:
    """Half-width along the fish, as a fraction of its maximum.

    Width follows depth rather than being authored separately, because that is
    what makes one slider — `fish.width_ratio` — turn a round-sectioned eel into
    a knife-edged bream. The exponent is where the interesting part is:

    THE EXPONENT IS NOT CONSTANT ALONG THE FISH. In front of the deepest point a
    fish keeps its width far better than its depth (a head is a blunt wedge, not
    a blade); behind it, the caudal peduncle is flattened much harder than it is
    shallowed. Using one exponent gave every species a pointed head one voxel
    wide, which read as a dart rather than as a fish and lost the eye — there
    was nowhere to put it. So the exponent is scaled down toward the snout.
    """
    q = min(max(p["depth_at"], 0.05), 0.95)
    aft = np.clip((t - q) / max(1.0 - q, 1e-6), 0.0, 1.0)
    e = max(p["width_follow"], 0.05) * (0.60 + 0.40 * aft)
    return np.clip(depth, 1e-4, 1.0) ** e


# Fore-and-aft depth of a cephalofoil as a fraction of its span. DERIVED
# rather than authored, because a hammerhead's head is a WING and a wing's
# chord goes with its span -- a slider for it could only ever be set wrong, the
# same argument that keeps the anal fin's position and the eye's out of the
# parameter table. See `docs/marine-marking-research.md` for where the number
# comes from and what the alternatives measured.
CEPHALOFOIL_CHORD = 0.34

# Where along that chord the wing is widest, 0 at the snout and 1 at the back
# of the hammer. The tips are set BACK from the midline of the leading edge:
# the anterior margin is a broad forward arc and the trailing edge is a hard
# sweep in to the neck, which is the difference between a hammer and a wedge.
CEPHALOFOIL_TIP_AT = 0.80

# Dorsoventral thickness of the hammer as a fraction of its span. THE HEAD HAS
# TO BE FLATTENED AS WELL AS WIDENED, and that is the same argument twice: the
# loft derives the head's DEPTH from the depth profile, and the depth profile
# is already two thirds of the way to full body depth by the station the hammer
# is widest at. Widened and not flattened, the first hammerhead measured six
# voxels thick across a 27-voxel span -- a ratio of 0.22 against a real
# animal's 0.09, which is a slab rather than a wing. The only hard number
# anywhere is the Sphyrna gilberti holotype: nacelle height 13 mm against a
# head width of 138 mm, so 0.094.
CEPHALOFOIL_THICK = 0.094


def _cephalofoil(t: np.ndarray, p: dict) -> np.ndarray:
    """Half-span of the head wing at each station, in voxels. Zero when off.

    THE ONE THING THE BODY LOFT COULD NOT SAY. Every other width in this file
    follows the depth profile -- `_width_profile` raises the depth to a power,
    so a station that is shallow is narrow -- and a hammerhead is the exact
    counter-example: its head is the SHALLOWEST part of the animal and by a
    long way the widest. There is no exponent that produces that, which is why
    this is an override on `half` rather than another term in the profile.

    It is applied as a MAXIMUM against the body's own half-width, so the
    parameter can only ever widen a head and never pinch one. A parameter that
    could narrow the snout would be a second, worse way to author
    `fish.snout`, and on a slim species it would cut the body in two.

    Connectivity is by construction and not by luck: this widens the loft
    itself, so the wing is the same solid as the body and the one-voxel axis
    run in `_body` is stamped straight through it. Nothing here is a plate
    stuck on afterwards, which is what every fin in this file has to be and
    what makes fins the fragile part.
    """
    span = float(p.get("head_span_v", 0.0))
    if span <= 0.0:
        return np.zeros_like(t)
    # At least two columns of chord, or the wing is a single column and reads
    # as one wide voxel rather than as a head.
    chord_v = max(CEPHALOFOIL_CHORD * span, 2.0)
    chord_t = min(max(chord_v / max(p["length_v"], 1.0), 1e-6), 0.9)
    u = np.clip(t / chord_t, 0.0, 1.0)
    lead = CEPHALOFOIL_TIP_AT
    # Leading edge as a quarter ellipse (a broad forward arc), trailing edge as
    # a fast sweep back. Both meet at 1 exactly at the tip station.
    front = np.sqrt(np.clip(1.0 - ((lead - u) / lead) ** 2, 0.0, 1.0))
    back = np.clip((1.0 - u) / (1.0 - lead), 0.0, 1.0) ** 0.60
    w = np.where(u <= lead, front, back)
    w = np.where(t <= chord_t, w, 0.0)
    return 0.5 * span * w


def _flatten_head(t: np.ndarray, dtop, dbot, p: dict):
    """Squash the body's depth over the hammer, blended out behind it.

    Blended by the wing's OWN profile rather than switched on and off. A hard
    cap over the chord and none behind it puts a five-voxel step in the outline
    at one column, which reads as a cut rather than as a neck; weighting by the
    same curve that widens the head makes the thinning arrive and leave with
    the wing.
    """
    span = float(p.get("head_span_v", 0.0))
    if span <= 0.0:
        return dtop, dbot
    w = _cephalofoil(t, p) / max(0.5 * span, 1e-6)      # 0..1, the wing profile
    cap = max(0.5 * CEPHALOFOIL_THICK * span, 0.5)
    return (dtop * (1.0 - w) + np.minimum(dtop, cap) * w,
            dbot * (1.0 - w) + np.minimum(dbot, cap) * w)


def _stations(p: dict):
    """(t, valid, dtop, dbot, halfwidth) per grid column, all in voxels."""
    nx = p["shape"][0]
    xs = np.arange(nx, dtype=np.float64)
    span = max(p["xnose"] - p["xtail"], 1)
    t = np.clip((p["xnose"] - xs) / span, 0.0, 1.0)
    valid = (xs >= p["xtail"]) & (xs <= p["xnose"])

    d = _depth_profile(t, p)
    w = _width_profile(t, d, p)
    # Half a voxel is the floor everywhere: below it a cross-section contains no
    # cell centre and the body would come apart. The axis run stamped in `_body`
    # is the belt to this pair of braces.
    dtop = np.maximum(p["depth_v"] * d * (1.0 - p["belly"]), 0.5)
    dbot = np.maximum(p["depth_v"] * d * p["belly"], 0.5)
    half = np.maximum(p["width_v"] * 0.5 * w, 0.5)
    half = np.maximum(half, _cephalofoil(t, p))
    dtop, dbot = _flatten_head(t, dtop, dbot, p)
    return t, valid, dtop, dbot, half


def _body(p: dict) -> np.ndarray:
    """Solid body as a boolean grid.

    The cross-section is a SUPERELLIPSE, |y/w|^n + |z/d|^n <= 1, and the
    exponent `fish.section` is the one thing from the superformula literature
    that survives contact with a ten-voxel-deep body: at n = 1.3 the section is
    a diamond and the fish has a knife-edged back and belly (a bream, a
    surgeonfish); at n = 3 it is a rounded box and the fish is a tube (a
    catfish, an eel). The other five parameters a Gielis curve offers all move
    the outline by well under one voxel at this size, so they are not here.

    The vertical half-extent is different above and below the axis, which is not
    a refinement — it is how a fish is actually built. A trout's back is a
    shallow arc and its belly is a deep round one; forcing them equal makes a
    torpedo, and the countershading then has nothing to sit on because there is
    no top and bottom to tell apart.
    """
    nx, ny, nz = p["shape"]
    t, valid, dtop, dbot, half = _stations(p)

    y = (np.arange(ny, dtype=np.float64) + 0.5) - p["ycen"]
    z = (np.arange(nz, dtype=np.float64) + 0.5) - p["zaxis"]
    Y = np.abs(y)[None, :, None] / half[:, None, None]
    # Above the axis measured against dtop, below against dbot.
    zz = z[None, None, :]
    Z = np.abs(zz) / np.where(zz >= 0.0, dtop[:, None, None], dbot[:, None, None])

    # THE SECTION EXPONENT VARIES ALONG THE ANIMAL, and for a whale it has to.
    #
    # Measured cetacean trunks are treated as CIRCULAR in the literature -- the
    # standard way to get a whale's diameter is girth/pi, and that survives peer
    # review -- while the tailstock is explicitly "highly streamlined and
    # elliptical in cross-section", to the point that modelling it as a cone
    # gives "anomalously high" volume. So a dolphin is a barrel that becomes a
    # vertical blade, and a single exponent cannot say that.
    #
    # A fish sets both ends the same and nothing changes, which is why this is
    # one extra parameter rather than a second body model.
    aft = np.clip((t - min(max(p["depth_at"], 0.05), 0.95))
                  / max(1.0 - min(max(p["depth_at"], 0.05), 0.95), 1e-6), 0.0, 1.0)
    n = np.maximum(p["section"] + (p["section_tail"] - p["section"]) * aft, 0.6)
    n = n[:, None, None]
    occ = (Y ** n + Z ** n) <= 1.0 + 1e-9
    occ &= valid[:, None, None]

    # THE AXIS GOES DOWN FIRST, as an unbroken one-voxel run from snout to
    # wrist. Same rule as `grid.capsule`: a shape thinner than one voxel exists
    # as its centreline or it does not exist at all. Without this the snout and
    # the caudal peduncle -- the two thinnest parts of every fish -- dropped out
    # on the species with slim tails, and the asset shipped as a head, a body
    # and a tail fin: three pieces, which `tools/buildcheck.py` rejects.
    iy = int(p["ycen"])
    iz = int(p["zaxis"])
    occ[valid, iy, iz] = True
    return occ


# --- fins -------------------------------------------------------------------

# Which fin a voxel belongs to, so the paint pass can colour the caudal
# differently from the body without a second geometry pass.
FIN_NONE, FIN_MEDIAN, FIN_CAUDAL, FIN_PAIRED = 0, 1, 2, 3

# Part tags as the RIGGING sees them, which is not the same question `_fins`
# answers. `_fins` separates fins by how they are drawn and painted -- median,
# caudal, paired -- and that is the right split for colour. A rig wants the
# body as a part too, because the body is what everything else rotates against.
#
# Animals are rigid-part animated and ship in one pose (owner, 2026-08-14; see
# docs/animal-rigging-decision.md), so the asset has to say which voxels move
# together. Same numbering as the fin kinds with the body added at the end, so
# the translation is one addition rather than a table.
P_NONE, P_MEDIAN, P_CAUDAL, P_PAIRED, P_BODY = 0, 1, 2, 3, 4
# ... mapped into the shared rigging vocabulary on the way out; see bird.py for
# why the private numbering stays private.
_TO_SHARED = {
    P_NONE: parts.P_NONE, P_MEDIAN: parts.P_FIN_MEDIAN,
    P_CAUDAL: parts.P_FIN_CAUDAL, P_PAIRED: parts.P_FIN_PAIRED,
    P_BODY: parts.P_BODY,
}


def _fins(p: dict, body: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Every fin, as (mask, which-fin). Guaranteed to touch the body.

    Each fin is drawn from ONE VOXEL INSIDE the body outward. That is the whole
    trick, and it is not a fudge: a plate one voxel thick standing on a curved
    surface lands on whichever side of a rounding decision the surface happened
    to fall, so "start at the surface" means "start touching, or at a corner, or
    a voxel clear" depending on the station. A corner join is not a join --
    `forge.cli.pieces` counts at 26-connectivity and would pass it, and then the
    engine's mesher would hand back a fin nobody can dig out. Starting inside
    makes the overlap a fact rather than a hope.
    """
    nx, ny, nz = p["shape"]
    t, valid, dtop, dbot, half = _stations(p)
    fins = np.zeros((nx, ny, nz), bool)
    kind = np.zeros((nx, ny, nz), np.uint8)
    iy = int(p["ycen"])
    th = max(1, min(int(p["fin_thick"]), 3))
    y0, y1 = iy - (th - 1) // 2, iy + th // 2 + 1

    def plate(x: int, z_lo: float, z_hi: float, which: int) -> None:
        a = max(0, int(math.floor(z_lo)))
        b = min(nz, int(math.ceil(z_hi)) + 1)
        if b <= a:
            return
        fins[x, max(y0, 0):min(y1, ny), a:b] = True
        kind[x, max(y0, 0):min(y1, ny), a:b] = which

    # EVERY FIN HEIGHT IS FLOORED AT `fish.fin_min_vox`, and on a big animal
    # that floor is the only reason the fin exists.
    #
    # Fin size is authored as a fraction of body depth because that is how a
    # fish is proportioned -- but the fraction that identifies a species does
    # not scale with the animal. A blue whale's dorsal fin is 1.0-1.4% of its
    # body length, and it is exactly what separates a blue from a fin from a
    # sei. Faithfully proportioned it is under half a voxel at 100 voxels of
    # body and it disappears, taking the species with it. A minke's is 4.0% and
    # a delphinid's 8-12%, so the same floor is invisible on those.
    #
    # Floored on the fin's MAXIMUM rather than at each station, so the outline
    # still tapers to its ends instead of becoming a slab.
    fmin = float(p["fin_min_vox"])

    # --- dorsal, on the back ------------------------------------------------
    if p["dorsal_shape"] != "none" and p["dorsal_height"] > 0.0:
        d0 = p["dorsal_start"]
        dl = max(p["dorsal_len"], 1e-3)
        H = max(p["dorsal_height"] * p["depth_v"], fmin)
        for x in range(nx):
            if not valid[x]:
                continue
            u = (t[x] - d0) / dl
            if not 0.0 <= u <= 1.0:
                continue
            h = H * _fin_height(u, p["dorsal_shape"])
            if h < 0.6:
                continue
            top = p["zaxis"] + dtop[x]
            plate(x, top - 1.0, top + h, FIN_MEDIAN)

    # An adipose fin: three voxels behind the dorsal, and the single most
    # recognisable thing about a salmonid at any resolution. It is here because
    # it costs almost nothing and it is the only mark that separates a trout
    # from every other slim brown fish in a shoal.
    if p["adipose"]:
        for x in range(nx):
            if valid[x] and 0.74 <= t[x] <= 0.82:
                top = p["zaxis"] + dtop[x]
                plate(x, top - 1.0, top + max(1.2, 0.10 * p["depth_v"]), FIN_MEDIAN)

    # --- second dorsal, for the sharks --------------------------------------
    #
    # Most sharks have two, and the second is a nub: measured on a scalloped
    # hammerhead it is 2% of total length against the first dorsal's 13%, and
    # on a great white it is described as "minute". It is here anyway, because
    # two bumps on a back is a shark and one bump is a fish, and that reads at
    # any size the animal is drawn at. It shares the height floor above, which
    # is the only reason a 2%-of-length fin survives.
    if p["dorsal2_height"] > 0.0:
        dl2 = max(p["dorsal2_len"], 1e-3)
        H = max(p["dorsal2_height"] * p["depth_v"], fmin)
        for x in range(nx):
            if not valid[x]:
                continue
            u = (t[x] - p["dorsal2_start"]) / dl2
            if not 0.0 <= u <= 1.0:
                continue
            h = H * _fin_height(u, "triangular")
            if h < 0.6:
                continue
            top = p["zaxis"] + dtop[x]
            plate(x, top - 1.0, top + h, FIN_MEDIAN)

    # --- anal, under the tail end -------------------------------------------
    if p["anal_height"] > 0.0:
        al = max(p["anal_len"], 1e-3)
        # Position is derived, not authored: the anal fin ends just in front of
        # the caudal peduncle on every fish that has one, so an independent
        # "anal start" slider would only ever be set to one value and could be
        # set to a wrong one.
        a0 = max(0.05, 1.0 - al - 0.06)
        H = max(p["anal_height"] * p["depth_v"], fmin)
        for x in range(nx):
            if not valid[x]:
                continue
            u = (t[x] - a0) / al
            if not 0.0 <= u <= 1.0:
                continue
            h = H * _fin_height(u, "triangular")
            if h < 0.6:
                continue
            bot = p["zaxis"] - dbot[x]
            plate(x, bot - h, bot + 1.0, FIN_MEDIAN)

    # --- caudal, the tail ---------------------------------------------------
    if p["caudal_shape"] != "none" and p["lc"] > 0:
        _caudal(p, fins, kind, dtop, dbot, half, y0, y1)

    # --- paired fins, on the sides and the belly ----------------------------
    if p["pectoral"] > 0.0:
        _pectoral(p, fins, kind, t, valid, half, dtop, dbot)
    if p["pelvic"] > 0.0:
        _pelvic(p, fins, kind, t, valid, dbot, y0, y1)

    # Never let a fin overwrite the body; the body wins so the outline of the
    # fish stays the outline of the fish.
    fins &= ~body
    kind[~fins] = FIN_NONE
    return fins, kind


def _fin_height(u: float, shape: str) -> float:
    """Height along a median fin, 0..1, as a fraction of its maximum.

    The four shapes differ in WHERE the height is, which is exactly what no
    single slider can say. A sail is broad and high through the middle; a spiny
    first dorsal is tallest at its leading edge and rakes back; a ridge is a low
    even fold, which is what an eel and a catfish have instead of a fin.
    """
    u = min(max(u, 0.0), 1.0)
    if shape == "sail":
        return math.sin(math.pi * u) ** 0.45
    if shape == "spiny":
        return (1.0 - u) ** 0.55 * math.sin(math.pi * min(u * 4.0, 1.0)) ** 0.3
    if shape == "ridge":
        return min(1.0, 4.0 * u * (1.0 - u)) ** 0.20
    return math.sin(math.pi * u) ** 0.75      # triangular


def _caudal(p: dict, fins, kind, dtop, dbot, half, y0: int, y1: int) -> None:
    """The tail fin, aft of the wrist. Vertical for a fish, horizontal for a whale.

    Written as an INNER and an OUTER limit on how far from the axis the fin
    reaches at each distance aft, rather than as an outline traced round the
    fin. The inner limit is what a fork is: past the notch, the fin exists only
    out at the lobes and not in the middle. Tracing an outline instead needs a
    scan-fill to turn it into voxels and the fill leaks through any one-voxel
    gap the rounding leaves, which on a twelve-voxel tail is most seeds.

    The flare is the other half. A tail fin does not start at full span; it
    grows out of the wrist over the first third of its length. Without that the
    fin met the peduncle as a step several voxels tall, which reads as a paddle
    stuck on the back of a fish rather than as a tail.

    TWO THINGS HERE ARE NOT ABOUT FISH.

    **`fish.caudal_plane` swaps the axis the fin spans.** A whale's fluke is
    HORIZONTAL and a fish's tail is VERTICAL, and that one fact is most of what
    separates a dolphin from a shark in silhouette from any angle but dead
    broadside. It is a swap of which axis the offset `v` runs along, which is
    why it is one parameter rather than a second generator: everything else
    about a fluke -- flare out of the wrist, the outline family, the lobe
    minimum -- is what a caudal fin already does.

    A real fluke also has a median NOTCH, and it is not modelled. Measured: the
    notch is about 5% of the fluke's span, the span is about 25% of body
    length, so the notch is **1.2% of body length** -- under one voxel at 80
    voxels of body and one voxel at 120. Minecraft's dolphin fluke is a
    10x1x6 slab with no notch either. `fish.caudal_fork` will cut one for any
    species long enough to hold it, which is the honest place to leave it.

    **`fish.caudal_upper` makes the upper lobe longer than the lower.** Sharks
    are heterocercal and it is a strong silhouette cue: measured as the ratio
    of the dorsal caudal margin to the preventral margin, a requiem shark is
    about 3.1:1, a nurse shark 5:1 or more, and a lamnid -- the great white --
    is 1.1:1 and reads as almost symmetric. A bony fish is 1:1. So this is a
    continuous parameter with the fish at zero, not a `caudal_shape` entry:
    the great white and the trout would need the same entry and different
    numbers, which is exactly what a slider is for.
    """
    nx, ny, nz = fins.shape
    lc = p["lc"]
    S = 0.5 * p["caudal_span"] * p["depth_v"]
    # WHAT THE FIN HAS TO REACH ACROSS TO STAY ATTACHED depends on which way
    # it lies. A vertical tail meets the peduncle across its DEPTH; a
    # horizontal fluke meets it across its WIDTH. Using the depth for both put
    # a dolphin's fluke on a neck two voxels wide inside a body six wide, and
    # it came off.
    horizontal = p["caudal_plane"] == "horizontal"
    wrist = (half[p["xtail"]] if horizontal
             else max(dtop[p["xtail"]], dbot[p["xtail"]]))
    shape = p["caudal_shape"]
    fk = min(max(p["caudal_fork"], 0.0), 0.92)
    het = min(max(p["caudal_upper"], 0.0), 1.0)
    zc = p["zaxis"]

    for k in range(0, lc + 1):
        x = p["xtail"] - k
        if x < 0:
            break
        u = k / max(lc, 1)
        # Flare out of the wrist over the first third.
        vmax = wrist + (S - wrist) * min(1.0, u / 0.34)
        if shape == "rounded":
            outer = S * math.sqrt(max(0.0, 1.0 - u * u))
            inner = 0.0
        elif shape == "pointed":
            outer = S * max(0.0, 1.0 - u) ** 0.70
            inner = 0.0
        elif shape == "forked":
            outer = S
            # `fish.caudal_fork` IS THE SHARE OF THE FIN'S LENGTH THE NOTCH
            # EATS INTO, so the notch begins at `1 - fork` and a bigger number
            # is a deeper fork. It was written the other way round first --
            # notch begins AT `fork` -- which made the slider run backwards
            # from its own help text and from the species authored against it:
            # `shoal-herring` asked for 0.60 and the help says that is lunate,
            # and it was getting a notch in the last 40% of the fin, which is
            # emarginate. `tools/fishprobe.py` found it, by way of the fork
            # depth measuring as identical at both ends of the range.
            start = 1.0 - fk
            inner = (0.0 if u <= start
                     else S * ((u - start) / max(fk, 1e-6)) ** 1.1)
        else:                                   # truncate
            outer = S
            inner = 0.0
        outer = min(outer, vmax)
        # A LOBE HAS TO STAY A LOBE, and this has to be applied AFTER the flare
        # has clamped `outer`, not against the fin's full span.
        #
        # Against the span it was measuring a lobe that did not exist yet: over
        # the first third of the fin `outer` is still growing out of the wrist,
        # so an `inner` computed from the full span could exceed it and leave
        # the column empty -- and then the lobes further aft, where `outer`
        # finally reached the span, had nothing to attach to. On a dolphin
        # fluke that shipped as EIGHT loose pieces: the two lobe tips, and the
        # notch cutting the middle out of everything between them.
        inner = min(inner, max(0.0, outer - max(1.2, 0.25 * S)))
        # The two columns adjacent to the body always span the whole wrist, so
        # the fin cannot meet the peduncle at a single corner -- applied BEFORE
        # the "too small to draw" test, or a short fin on a slim tail skips its
        # own attachment column and ships as a second asset.
        if k <= 1:
            inner = 0.0
            outer = max(outer, wrist)
        if outer < 0.5:
            continue

        # HETEROCERCY IS A PER-SIDE REACH, not a per-side span.
        #
        # A shark's upper caudal lobe is LONGER -- it reaches further aft --
        # rather than taller, so the asymmetry has to live in how far back the
        # fin still exists on each side, which means testing `u` against a
        # different limit above and below the axis. Scaling the span instead
        # gives a tail that is taller on top, which is a different animal (and
        # a made-up one).
        #
        # The lower lobe is shortened rather than the upper lengthened, so
        # `fish.caudal_len` keeps meaning the length of the longest lobe on
        # every species and a shark does not silently grow when the slider is
        # turned up.
        lower_reach = 1.0 - 0.75 * het
        alive_lo = u <= lower_reach + 1e-9

        for i in range(max(int(math.floor(-outer)) - 1, -nz),
                       min(int(math.ceil(outer)) + 2, nz)):
            v = i + 0.5
            if abs(v) > outer or abs(v) < inner:
                continue
            if v < 0.0 and not alive_lo:
                continue
            # FLOOR, NOT ROUND. `grid.ball` says why in one line -- "a voxel
            # spans [i, i+1), so the voxel containing a point is its floor" --
            # and this is the second time in this repo that ignoring it cost a
            # day. `v` is always an integer plus a half, so `round` lands on an
            # exact tie at every single cell, and Python rounds ties to EVEN:
            # offsets ...,-0.5, 0.5, 1.5, 2.5... came back as ...,0, 0, 2, 2...
            # The fluke came out as alternate rows with empty rows between
            # them, which is eight loose pieces and, at a glance, a fluke.
            if horizontal:
                y = int(math.floor(p["ycen"] + v))
                if not 0 <= y < ny:
                    continue
                # A fluke is a horizontal plate, so its THICKNESS is vertical.
                for z in range(max(int(zc) - (p["fin_thick"] - 1) // 2, 0),
                               min(int(zc) + p["fin_thick"] // 2 + 1, nz)):
                    fins[x, y, z] = True
                    kind[x, y, z] = FIN_CAUDAL
            else:
                z = int(math.floor(zc + v))
                if not 0 <= z < nz:
                    continue
                fins[x, max(y0, 0):min(y1, ny), z] = True
                kind[x, max(y0, 0):min(y1, ny), z] = FIN_CAUDAL


def _pectoral(p: dict, fins, kind, t, valid, half, dtop, dbot) -> None:
    """The pair behind the gills.

    These are the only fins that stick out SIDEWAYS, which is why they are worth
    their cost: seen from anywhere but dead broadside they are most of what says
    "animal" rather than "lozenge". They sweep aft and down as they go out,
    because a pectoral held straight out reads as a wing.
    """
    nx, ny, nz = fins.shape
    reach = max(1, int(round(p["pectoral"] * p["depth_v"])))
    # Just behind the head. Derived rather than authored for the same reason the
    # anal fin's position is: there is one right answer and a slider could only
    # be set to a wrong one.
    station = min(0.92, p["head_frac"] + 0.04)
    xs = [x for x in range(nx) if valid[x] and abs(t[x] - station) < 0.06]
    if not xs:
        return
    x0 = int(np.mean(xs))
    # CHORD AS A FRACTION OF REACH, which is what tells a flipper from a fin.
    #
    # A fish's pectoral is roughly as long fore-and-aft as it sticks out. A
    # humpback's flipper is 30.8% of body length and 7.3% wide -- a ratio of
    # 0.24, and the most recognisable limb on any animal in the sea. It was a
    # fixed 10% of body length before, which could not express either end.
    plen = max(1, int(round(p["pectoral_aspect"] * reach)))
    iy = int(p["ycen"])
    out_to = int(round(half[x0])) + reach

    # THE RUN STARTS AT THE AXIS, not at the body surface.
    #
    # Starting at the surface is the obvious version and it is wrong at the
    # widest point of a fish: that is exactly where the body is thinnest in Z,
    # so a fin voxel placed a quarter of the way down the belly at the surface
    # y often has no body voxel beside it and the fin floats. Walking outward
    # from the axis makes every step adjacent to the one before it, and the
    # first step is unarguably inside the fish. Everything inside is then
    # discarded by `fins &= ~body`, so this costs nothing in the result.
    prev_z = int(round(p["zaxis"]))
    for j in range(0, out_to + 1):
        f = j / max(out_to, 1)
        xc = x0 - int(round(f * plen * 1.4))
        zc = int(round(p["zaxis"] - (0.20 + 0.50 * f) * dbot[x0]))
        n_len = max(1, int(round(plen * (1.0 - 0.55 * f))))
        # Fill the whole z step between this ring and the last, so a fin that
        # drops faster than one voxel per step stays face-connected instead of
        # becoming a diagonal chain.
        za, zb = (zc, prev_z) if zc <= prev_z else (prev_z, zc)
        prev_z = zc
        for dx in range(-n_len, 1):
            x = xc + dx
            if not 0 <= x < nx:
                continue
            for s in (-1, 1):
                y = iy + s * j
                if not 0 <= y < ny:
                    continue
                for z in range(max(za, 0), min(zb, nz - 1) + 1):
                    fins[x, y, z] = True
                    kind[x, y, z] = FIN_PAIRED


def _pelvic(p: dict, fins, kind, t, valid, dbot, y0: int, y1: int) -> None:
    """A small pair under the belly. Two or three voxels; they read as a notch
    in the ventral outline rather than as fins, and that notch is what stops a
    fish's underside being a smooth arc."""
    nx, ny, nz = fins.shape
    H = max(1.0, p["pelvic"] * p["depth_v"])
    # A window WIDE ENOUGH TO HOLD A SHAPE. At 0.06 of the body length this
    # caught a single column on a 30-voxel fish and drew the pelvic as a
    # one-voxel stalk two deep, which reads as a wire hanging off the belly
    # rather than as a fin. Three columns is the least that can taper.
    # 0.36 of standard length is the MEASURED median pelvic origin over 7,452
    # landmarked FishBase specimens; see docs/fish-shape-research.md. Derived
    # rather than authored for the same reason the anal fin's position is.
    t0, t1 = 0.36, 0.36 + max(0.10, 0.12 * p["pelvic"])
    for x in range(nx):
        if not valid[x] or not t0 <= t[x] <= t1:
            continue
        u = (t[x] - t0) / max(t1 - t0, 1e-6)
        h = H * math.sin(math.pi * min(max(u, 0.0), 1.0)) ** 0.5
        bot = p["zaxis"] - dbot[x]
        a = max(0, int(math.floor(bot - h)))
        b = min(nz, int(math.ceil(bot)) + 1)
        if b <= a:
            continue
        fins[x, max(y0, 0):min(y1, ny), a:b] = True
        kind[x, max(y0, 0):min(y1, ny), a:b] = FIN_PAIRED


def _barbels(grid: VoxelGrid, p: dict) -> None:
    """Whiskers off the snout: a catfish, a carp, a sturgeon.

    Drawn with `grid.line`, which is the Amanatides-Woo traversal every branch
    in this repo is drawn with, so each barbel is a face-connected run starting
    ON a snout voxel. That is the only reason a two-voxel-thick thread hanging
    off the front of the fish is not a second asset.
    """
    n = max(0, min(int(p["barbels"]), 4))
    if n == 0 or p["barbel_len"] <= 0.0:
        return
    reach = p["barbel_len"] * p["length_v"]
    if reach < 1.0:
        return
    nose = np.array([p["xnose"] - 0.5, p["ycen"], p["zaxis"]])
    for i in range(n):
        s = 1.0 if i % 2 == 0 else -1.0
        tier = i // 2
        start = nose + np.array([0.0, s * max(1.0, 0.25 * p["width_v"]),
                                 -0.15 * p["depth_v"] * (1 + tier)])
        end = start + np.array([reach * (0.55 - 0.20 * tier),
                                s * reach * 0.35,
                                -reach * (0.40 + 0.25 * tier)])
        grid.line(start, end, p["mat_fin"])


# --- colour -----------------------------------------------------------------

# How much of the animal a bent boundary bends over, as a half-width in
# fractions of body length. One constant rather than a slider, and it is a
# CONSTRUCTION: no source measures the fore-and-aft extent of a cape dip or a
# ventral flame, and the two that could be traced off published figures agreed
# to within a body length's tenth, so a slider would be a knob with one
# defensible setting. 0.22 makes the curve span a little under half the animal,
# which on the shortest species that can carry one -- a 64-voxel bottlenose
# dolphin -- is 28 columns. `docs/marine-marking-research.md` records what
# narrower and wider measured.
CURVE_HALF_WIDTH = 0.22


def _field_lines(p: dict, t: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Where the back and the belly colours stop, per column, 0 belly to 1 back.

    THIS IS THE NEW PRIMITIVE AND IT IS AN EDGE, NOT A MARK. Every marking in
    this file before it was ink laid ON a field -- a band (`stripe`, `bars`), a
    blotch (`spots`, `mottle`, `saddle`) or a patch (`eye_patch`) -- and all
    five are drawn against a countershading whose two boundaries are LEVEL
    LINES running the length of the animal. The three most recognisable colour
    schemes in the sea are none of those things. They are the boundary itself
    having a shape:

      * a delphinid's CAPE -- the dark back reaches down onto the flank around
        the dorsal fin and lifts again toward the tail;
      * an orca's ventral FLAME -- the white belly throws a blaze up the flank
        behind the middle of the animal, which is why an orca reads as two
        white shapes from the side and not one;
      * a common dolphin's HOURGLASS -- both at once, meeting, so the flank is
        pinched out where they cross and shows as two patches with a waist.

    The third is the argument for doing it this way. An hourglass is not a
    third mechanism: it is the first two at the same station. Drawn as a
    marking primitive it would have been a hand-drawn X that knew nothing about
    where the countershading was, and moving `fish.back_frac` would have left
    the X floating clear of the cape it is supposed to be cut out of.

    THE TWO LINES ARE INDEPENDENT AND THE PALE ONE WINS WHERE THEY OVERLAP.
    That is not the obvious answer and it was got wrong first: the first
    version clamped the pale edge to stop at the dark one, on the reasoning
    that a flank cannot be less than nothing. It cannot -- but the orca is
    exactly the animal where the two fields ALREADY touch. Its dark covers
    0.62 of its depth and its white 0.30, so a clamped flame had four
    hundredths of a body to climb through and could not move a single voxel on
    the one species the flame exists for. `tools/fishprobe.py` reported both
    curve parameters DEAD on its first run, which is what that looks like from
    the outside.

    Unclamped, the pale field simply climbs into the dark one and the flank is
    pinched out where they cross, which is the same waist by a shorter route.
    The belly is painted after the back in `_paint`, so the order is what
    resolves it and the order is deliberate: the pale ventral field has to stay
    one continuous shape or it stops reading as a belly, while a cape
    interrupted by it still reads as a cape.
    """
    back = np.full_like(t, 1.0 - p["back_frac"])
    belly = np.full_like(t, p["belly_frac"])
    shape = p["field_curve"]
    amount = max(p["curve_amount"], 0.0)
    if shape != "flat" and amount > 0.0:
        d = (t - p["curve_at"]) / max(CURVE_HALF_WIDTH, 1e-6)
        bump = np.where(np.abs(d) <= 1.0,
                        0.5 * (1.0 + np.cos(np.pi * np.clip(d, -1.0, 1.0))),
                        0.0)
        if shape in ("cape", "hourglass"):
            back = back - amount * bump
        if shape in ("flame", "hourglass"):
            belly = belly + amount * bump
    return np.clip(back, 0.0, 1.0), np.clip(belly, 0.0, 1.0)


def _paint(grid: VoxelGrid, p: dict, body: np.ndarray, fins: np.ndarray,
           fin_kind: np.ndarray) -> None:
    """Assign a material to every solid voxel.

    COLOUR IS THE MAIN CARRIER OF SPECIES IDENTITY HERE, not shape. Two fish of
    the same outline in different schemes read as two species; two fish of
    different outlines in the same scheme read as one species at two sizes. That
    is a claim about twenty-voxel fish specifically and it falls straight out of
    the voxel budget: the outline has about 120 voxels to work with and the
    flank has 400.

    Three layers, applied in order:

      1. COUNTERSHADING -- dark on top, pale underneath. Nearly every fish in
         open water has it, for the same reason military aircraft do: it cancels
         the light gradient and flattens the animal against whatever is behind
         it. It is also what gives a voxel fish a top and a bottom at all, which
         nothing about the silhouette does at this size.

         ITS TWO BOUNDARIES MAY BE CURVED RATHER THAN LEVEL, which is
         `_field_lines` and is the only structural change this pass has had.
         A dolphin's cape, an orca's ventral flame and a common dolphin's
         hourglass are all this one thing: the edge between two fields having
         a shape. They are not markings and they are deliberately not in the
         list below.
      2. ONE MARK -- a stripe, bars, spots, mottle or a saddle. One, not
         several: a twelve-voxel-deep flank cannot hold two marks without them
         reading as noise.
      3. THE EYE -- a dark voxel with a pale voxel beside it, at the front. It
         is two voxels and it does more than any other two voxels in the asset.
    """
    nx, ny, nz = p["shape"]
    t, valid, dtop, dbot, _half = _stations(p)
    occ = body | fins
    if not occ.any():
        return

    zc = p["zaxis"]
    z = (np.arange(nz, dtype=np.float64) + 0.5) - zc
    # Height within the local body outline, 0 at the belly line, 1 at the back.
    span = np.maximum(dtop + dbot, 1e-6)
    uz = (z[None, :] + dbot[:, None]) / span[:, None]      # (nx, nz)
    uz = np.clip(uz, 0.0, 1.0)

    mat = np.zeros((nx, ny, nz), np.uint8)
    mat[occ] = p["mat_flank"]

    # The two boundaries, per column rather than as two constants. `flat`
    # returns the constants and this is the same picture it always was.
    back_line, belly_line = _field_lines(p, t)
    back = uz >= back_line[:, None]
    belly = uz <= belly_line[:, None]
    mat[occ & back[:, None, :]] = p["mat_back"]
    mat[occ & belly[:, None, :]] = p["mat_belly"]

    # THE HOURGLASS IS FOUR COLOURS OUT OF TWO SHAPES, and that is Perrin's own
    # account of it rather than an invention here. Perrin 1972 on Delphinus:
    # the dark cape and a second dark overlay do not cover the same ground, so
    # "the buff thoracic patch represents the colour yielded by the pigment of
    # the cape alone, the grey flank patch that of the dorsal field overlay
    # alone, and the black dorsalmost area that of the combined effect" -- four
    # fields from two overlapping shapes, never drawn as four.
    #
    # This file has two curves rather than his two overlays, so it gets the
    # same picture the short way: the flank FORWARD of the waist is the
    # thoracic patch and takes the marking colour, and the flank behind it
    # stays the flank. It costs no parameter and no material role that an
    # hourglass species had spare -- the criss-cross IS that species' one
    # marking, which is why `fish.pattern` and this cannot both be wanted.
    if p["field_curve"] == "hourglass" and p["curve_amount"] > 0.0:
        fore = (t < p["curve_at"])[:, None, None]
        mat[occ & fore & ~back[:, None, :] & ~belly[:, None, :]] = p["mat_pattern"]

    mark = _pattern(p, t, uz, occ)
    if mark is not None:
        mat[occ & mark] = p["mat_pattern"]

    # Fins on top of the pattern. A stripe that runs onto the tail is a real
    # thing and a good one, but it is the pattern's job to say so, not an
    # accident of drawing order -- and every attempt to let markings decide fin
    # colour per voxel came out as a speckled tail, because a fin is one voxel
    # thick and the pattern was designed against a body twelve voxels deep.
    mat[fins & (fin_kind != FIN_NONE)] = p["mat_fin"]

    _eye(mat, p, t, valid, dtop, dbot, occ)
    grid.data[:] = mat


def _pattern(p: dict, t, uz, occ) -> np.ndarray | None:
    """The one mark, as an (nx, ny, nz) boolean.

    Reaction-diffusion was the obvious candidate and is not here; the research
    doc has the long version, and the short version is that Kondo and Asai's
    angelfish stripes are a pattern with a WAVELENGTH, and the wavelength of a
    real fish's stripes is a few millimetres. On a body twelve voxels deep the
    simulation resolves either one stripe or noise, and which one you get
    depends on the grid rather than on the fish.
    """
    kind = p["pattern"]
    if kind == "none":
        return None
    nx, ny, nz = occ.shape

    if kind == "stripe":
        # A horizontal band down the flank. Signals a schooling fish in open
        # water: it lines the shoal up and it points at the head, which is what
        # a predator has to find.
        band = np.abs(uz - p["pattern_pos"]) <= max(p["pattern_width"], 0.02) * 0.5
        return np.broadcast_to(band[:, None, :], occ.shape)

    if kind == "bars":
        # Vertical bars. The reef and weed-bed pattern: they break the outline
        # against a background of vertical structure.
        n = max(1, p["pattern_count"])
        phase = p["phase"]
        frac = np.mod(t * n + phase, 1.0)
        col = frac < min(max(p["pattern_width"], 0.03), 0.9)
        # Bars fade out on the belly, which is where every barred fish's bars
        # stop. Running them to the bottom made the fish look wrapped rather
        # than marked.
        band = col[:, None] & (uz >= 0.18)
        return np.broadcast_to(band[:, None, :], occ.shape)

    if kind == "spots":
        r = max(1.0, 0.5 * p["pattern_scale"] * p["length_v"])
        rs = np.random.default_rng(p["salt"])
        out = np.zeros((nx, nz), bool)
        xs = np.arange(nx, dtype=np.float64)[:, None]
        zs = np.arange(nz, dtype=np.float64)[None, :]
        for _ in range(max(1, p["pattern_count"]) * 3):
            cx = p["xtail"] + rs.random() * (p["xnose"] - p["xtail"])
            cz = p["zaxis"] + (rs.random() * 2.0 - 1.0) * 0.55 * p["depth_v"]
            out |= ((xs - cx) ** 2 + (zs - cz) ** 2) <= r * r
        return np.broadcast_to(out[:, None, :], occ.shape)

    # mottle / saddle: coherent blotches, thresholded by QUANTILE so
    # `pattern_strength` means the exact share of the fish that is marked. A
    # plain threshold on the noise means whatever the noise happened to do,
    # which is the same defect `grid.blob`'s density slider had.
    from .rock import coherent_noise

    sigma = max(1.0, 0.5 * p["pattern_scale"] * p["length_v"])
    field = coherent_noise((nx, ny, nz), p["salt"], sigma)
    if kind == "saddle":
        # Blotches over the back only: a trout's or a pike's saddles, which
        # break the outline seen from ABOVE, which is where a bird is.
        field = np.where(np.broadcast_to((uz >= 0.55)[:, None, :], occ.shape),
                         field, -1.0)
    vals = field[occ]
    if vals.size == 0:
        return None
    share = min(max(p["pattern_strength"], 0.0), 1.0)
    if share <= 0.0:
        return None
    cut = float(np.quantile(vals, 1.0 - share))
    return field >= cut


def _blowhole(mat, p: dict, t, valid, dtop, occ) -> None:
    """A dark mark on TOP of the head. A cetacean has one; a fish does not.

    Two or three voxels, and worth them for the same reason the eye is: it is
    on the silhouette's top edge, which is the edge a player looking down at a
    whale from a boat can see. Measured position on a bottlenose dolphin is
    about 0.06-0.09 of body length back from the snout -- the blowhole-to-
    dorsal-fin distance is the standard field proxy for total length, so it is
    one of the best-pinned landmarks on the animal.
    """
    rad = int(round(p["blowhole"]))
    if rad < 1:
        return
    nx, ny, nz = mat.shape
    xs = [x for x in range(nx) if valid[x] and abs(t[x] - 0.075) < 0.035]
    if not xs:
        return
    x = int(np.mean(xs))
    iy = int(p["ycen"])
    for dx in range(-(rad - 1), rad):
        xx = x + dx
        if not 0 <= xx < nx:
            continue
        col = np.flatnonzero(occ[xx, iy])
        if col.size == 0:
            continue
        for dy in range(-(rad - 1), rad):
            y = iy + dy
            if 0 <= y < ny and occ[xx, y, int(col[-1])]:
                mat[xx, y, int(col[-1])] = p["mat_eye"]


def _eye_patch(mat, p: dict, t, valid, dtop, occ) -> None:
    """A pale patch around the eye, drawn BEFORE the pupil goes on top of it.

    This is the orca, and it is the strongest single mark on any animal in this
    library. It is a horizontal lozenge and not a disc, and that shape is most
    of what makes it read as an orca rather than as a white smudge.

    THE NUMBER THAT USED TO BE HERE HAS NO SOURCE. This said "21.8 x 5.9 cm on
    a 6 m animal -- about 3.6% of body length and an aspect ratio near 3.7:1"
    and cited it as measured. A second search for it in 2026-08 found that no
    study anywhere reports orca eye patches in absolute units; the two numbers
    match a pair of dimensionless diversity indices in a saddle-patch paper,
    which is where they appear to have come from. What IS published is a ratio:
    patch length is 0.37-0.41 of the blowhole-to-dorsal-fin distance on the
    two large-patched Antarctic types (Durban et al. 2016, n=19), and the
    outline itself has been reduced to nine Fourier harmonics over 187 animals
    (Hutchings et al. 2025). Neither gives an aspect ratio, so the 1.85:1 drawn
    below is what the lattice can hold and is not a claim about the animal.
    `docs/marine-marking-research.md` §5 has the audit.

    It is also a general feature, which is why it is a slider on every species
    rather than an orca special case: plenty of fish carry a pale ring or a
    bright patch at the eye, and it is the cheapest way to make a dark head
    legible.

    A real orca's patches are LEFT-RIGHT ASYMMETRIC on about half of animals.
    Not modelled: at three voxels of patch there is no asymmetry to express
    that would not read as a mistake.
    """
    rad = int(round(p["eye_patch"]))
    if rad < 1:
        return
    nx, ny, nz = mat.shape
    station = min(0.9, max(0.02, p["head_frac"] * 0.26))
    xs = [x for x in range(nx) if valid[x] and abs(t[x] - station) < 0.05]
    if not xs:
        return
    x = int(np.mean(xs))
    z = int(round(p["zaxis"] + 0.32 * dtop[x]))
    if not 0 <= z < nz:
        return
    ys = np.flatnonzero(occ[x, :, z])
    if ys.size == 0:
        return
    # Longer fore-and-aft than it is tall. 1.85:1 is what two voxels of
    # half-height can carry; no published aspect ratio exists -- see above.
    rx = max(1, int(round(rad * 1.85)))
    for y in (int(ys[0]), int(ys[-1])):
        for dz in range(-(rad - 1), rad):
            for dx in range(-rx, rx + 1):
                xx, zz = x + dx, z + dz
                if (0 <= xx < nx and 0 <= zz < nz and occ[xx, y, zz]
                        and (dx / rx) ** 2 + (dz / max(rad, 1)) ** 2 <= 1.0 + 1e-9):
                    mat[xx, y, zz] = p["mat_patch"]


def _eye(mat, p: dict, t, valid, dtop, dbot, occ) -> None:
    """A dark voxel on each side of the head, with a pale one beside it.

    Two voxels per side, and they are worth more than any other two in the
    asset. A voxel animal without an eye reads as an object; with one it reads
    as facing somewhere. The pale voxel is not a highlight -- it is a contrast
    partner, and without it a dark eye on a dark head disappears entirely, which
    is what happened on every olive and brown species until it was added.
    """
    _blowhole(mat, p, t, valid, dtop, occ)
    _eye_patch(mat, p, t, valid, dtop, occ)
    rad = int(round(p["eye"]))          # eye radius in VOXELS; 0 turns it off
    if rad < 1:
        return
    nx, ny, nz = mat.shape
    # The measured median eye position is 0.072 of standard length back from
    # the snout, over 7,452 landmarked specimens, with a head ending at 0.278.
    # That ratio -- a bit over a quarter of the way into the head -- is what
    # this reproduces, rather than the halfway guess it started as, which put
    # the eye a voxel and a half too far back on every species.
    station = min(0.9, max(0.02, p["head_frac"] * 0.26))
    xs = [x for x in range(nx) if valid[x] and abs(t[x] - station) < 0.05]
    if not xs:
        return
    x = int(np.mean(xs))
    z = int(round(p["zaxis"] + 0.32 * dtop[x]))
    if not 0 <= z < nz:
        return
    ys = np.flatnonzero(occ[x, :, z])
    if ys.size == 0:
        return
    n = rad - 1
    for y in (int(ys[0]), int(ys[-1])):
        for dz in range(-n, n + 1):
            for dx in range(-n, n + 1):
                xx, zz = x + dx, z + dz
                if 0 <= xx < nx and 0 <= zz < nz and occ[xx, y, zz]:
                    mat[xx, y, zz] = p["mat_eye"]
        # The contrast partner, one voxel forward of the pupil.
        xx = min(x + rad, nx - 1)
        if occ[xx, y, z]:
            mat[xx, y, z] = p["mat_belly"]
