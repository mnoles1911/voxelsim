"""Land animals: a horizontal trunk on four limbs that reach the ground.

THE ONE THING THAT IS NOT LIKE ANY OTHER GENERATOR HERE. A fish is a solid whose
cross-section varies along a straight axis. A bird is six parts hanging off a
tilted body whose height above anything is nobody's business -- `bird._legs`
draws two threads downward and stops, and if they end in mid-air the bird is
flying. A land animal is the first asset in this library whose FOOT POSITIONS
ARE A CONSTRAINT ON THE BODY rather than an output of it. Four feet stand on one
plane; the trunk's height, its slope and the length of each limb are what has to
give to make that true.

So the layout here is solved backwards from the ground. `quad.shoulder_h` and
`quad.hip_h` say how high the shoulder joint and the hip joint sit above the
ground the animal is standing on, in units of its own body length; the trunk is
drawn between those two points, which is where its slope comes from; and each
limb is then a chain from its joint DOWN TO z = 0, so the limb length is derived
and cannot disagree with the stance. A bison's shoulder is 1.8 m and its hips
are 1.55; a spotted hyena's hips are three quarters of its shoulder height; a
Siberian musk deer is the other way round and hunches. Those are three numbers
in this file and they carry more between-species signal than anything else in
it, which is what `docs/biomes/README.md` §4.3 predicted.

WHAT THE VOXEL BUDGET IS SPENT ON, in the order the biome survey put them:

  1. THE PROFILE OF THE BACK -- shoulder height against hip height, plus the
     withers hump. This is the whole of the difference between a bison, a horse
     and a hyena, and it costs three numbers.
  2. THE NECK ANGLE, held ABSOLUTELY rather than relative to the back, because
     that is how a designer reads it off a photograph: a giraffe's neck is
     vertical off a sloping back and a weasel's is horizontal off a horizontal
     one, and expressing the second as an offset from the first means every
     species is authored by arithmetic.
  3. THE MUZZLE, which is a box and not a bill. `bird._bill` is a chain of
     shrinking balls on a curve; a muzzle continues the skull as a slab with a
     jawline, and the difference between a wolf and a bear is entirely in its
     depth and length.
  4. THE TAIL, which is a ROD and not a fan. A deer's 15 cm scut, a squirrel's
     full-length plume, a zebra's tassel, a kangaroo's counterweight thicker at
     the base than its own neck: one taper, one carriage angle and one terminal
     tuft cover all four.
  5. EARS. On a hare, a fennec fox, a kangaroo or an elephant the ears ARE the
     species, and neither the bird nor the fish generator has anything of the
     kind.
  6. HEADGEAR. A horn is a swept tapering cone. A stag's antlers are a very
     small tree -- see `_headgear` for why that tree is grown here rather than
     by `skeleton.grow`.
  7. ONE MARK. Zebra stripes are transverse bands wrapping a cylinder, which is
     the same mark the fish generator calls "vertical bars", floor rules and
     all.

THE STANCE, AND WHY A KANGAROO IS NOT A BUG
-------------------------------------------
A kangaroo stands on two hind legs and a heavy tail, with small forelimbs held
clear of the ground. A monitor lizard stands on four limbs held OUT TO THE SIDE
with its belly a few centimetres up. Neither is reachable by any setting of a
generator that assumes four limbs directly under a trunk, and discovering that
late means rewriting the limb code with species already authored against it.

`quad.stance` is therefore a CHOICE with three values, and the argument for a
choice rather than a slider is that the three differ in what is TRUE, not in how
much:

  * `standing`   four limbs under the trunk, all four on the ground. Everything
                 with hooves, paws or pads.
  * `sprawling`  limbs leave the FLANK rather than the belly and reach out
                 sideways before they reach down. Lizards, crocodilians. No
                 setting of an angle on a limb that starts under the body
                 produces this, because the attachment point itself moves.
  * `bipedal`    the hind limbs and the TAIL carry the animal. The tail's
                 carriage angle stops being authored and is SOLVED so the tip
                 lands on the ground, because a third leg that does not reach
                 the floor is not a third leg. The forelimbs attach forward on
                 the chest and are drawn short.

THE ALTERNATIVE THAT WAS REJECTED IS A LIMB COUNT. "A kangaroo is a biped" is
false in the way that matters here: it has four limbs, the forelimbs are visible
at any distance the animal is, and they are part of what makes the silhouette
read as a kangaroo rather than as a bird. A limb count of two deletes them. What
is actually different is which limbs touch the FLOOR, and that is a property of
the stance.

The second alternative -- three separate continuous knobs, one for how far the
forelimb reaches, one for whether the tail is a support, one for where the
forelimb attaches -- was rejected because only one combination of the three is
an animal, and the other seven are a spec nobody would notice was wrong. It is
the same argument `bird.pose` settled: when the combination is the thing, name
the combination.

AND `quad.stance` IS NOT A POSE, which matters because §4.2 of the biome survey
predicted a seed trap here and it does not arise. A pose is a posture one
individual can be in and come out of; `spec.SEED_INVARIANT` exists so that a
raven perched and the same raven flying are one raven. A kangaroo cannot stand
quadrupedally and a crocodile cannot stand like a horse -- the stance is a
species property, the way `fish.caudal_plane` is -- so it belongs IN the seeding
hash and is deliberately not added to the exclusion set. Animals ship in one
pose (owner, 2026-08-14; `docs/animal-rigging-decision.md`), so there is no
second authored pose for the trap to bite on.

FOUR LEGS ARE FOUR PARTS, AND THAT NEEDED A CHANGE OUTSIDE THIS FILE.
`forge/parts.py` split paired parts into left and right, which is exactly enough
for a bird and one short of enough here: front-left and rear-left are both on
the left, so under that split both landed under one id and the animal could not
walk. That is the raven's-wings defect -- 248 voxels left and 238 right under one
number, invisible in every render -- one axis over. `parts.AXIS_STRIDE` is the
fix and `_TO_SHARED` below is where this file uses it. The two pairs are drawn
under two private tags rather than separated afterwards by measurement, because
there is no line through a squirrel that has the hips on one side of it and the
shoulders on the other.

THE JOINT CAPS. Every joint carries a ball of the PARENT's material centred on
it (owner, 2026-08-14). A sphere is rotationally symmetric, so it covers the
wedge a rotating limb opens at any angle rather than at the angles someone
anticipated. Skipped below three voxels of limb thickness, where the wedge is
one voxel and the cap would be most of the limb -- which on a 22 cm squirrel at
1 cm it very nearly is.

COLOUR IS A PER-VOXEL MATERIAL. This file authors every species out of the ten
creature skins and eleven plumage materials that are already in the engine
(`forge/materials.py`); it proposes nothing new, and `docs/quadruped-notes.md`
records the two colours the mammal set genuinely wants and what appending them
would cost.
"""

from __future__ import annotations

import math

import numpy as np

from . import materials
from . import parts
from .grid import VoxelGrid
from .spec import PARAMS as _PARAMS
from .spec import (_QUAD_EARS, _QUAD_HORNS, _QUAD_MARKS, _QUAD_STANCES, _SEXES,
                   get)

# The three ways an animal meets the ground. See the module docstring.
STANCES = ("standing", "sprawling", "bipedal")

# Ear outlines. Named the way the biome files describe them, because that is the
# vocabulary whoever authors a species will be reading from. They are choices
# rather than a width slider because a BLADE (a hare, a fennec, a kangaroo -- a
# long flat paddle) and a FAN (an elephant -- a broad sheet lying against the
# neck) are not two settings of one shape: the blade stands up off the skull and
# the fan hangs off it.
EARS = ("none", "round", "pointed", "blade", "fan", "tufted")

# Headgear. Six shapes and a null, and the split between them is the one the
# biome survey found decides build order (`docs/biomes/README.md` §6): a PALMATE
# rack is flat blades 10-15 cm across and survives a 5 cm lattice unaltered,
# where a BRANCHED rack of round tines is 3-4 cm at the beam and 1-2 cm at the
# tip and reads at no lattice a life-size animal can use.
HORNS = ("none", "spike", "curve", "sweep", "spiral", "palmate", "branched")

# Markings. One per animal, not three: unlike a bird -- whose cap, wing bar and
# breast streaking sit on three disjoint sets of voxels -- a mammal's markings
# all compete for the same flank, which is the fish generator's situation and
# the reason a fish gets one.
MARKS = ("none", "bars", "spots", "saddle", "flankstripe", "dapple", "blotch")

SEXES = ("unsexed", "female", "male")

# Checked against the parameter table at import rather than trusted. A shape
# name that falls through to a default LOOKS like a shape that works, which is
# how `spire` and `ovoid` crowns rendered as spheres for as long as they did.
for _mine, _theirs, _what in (
    (STANCES, _QUAD_STANCES, "STANCES"),
    (EARS, _QUAD_EARS, "EARS"),
    (HORNS, _QUAD_HORNS, "HORNS"),
    (MARKS, _QUAD_MARKS, "MARKS"),
    (SEXES, _SEXES, "SEXES"),
):
    assert set(_mine) == set(_theirs), (
        f"quadruped.py {_what} and spec.py disagree: {set(_mine) ^ set(_theirs)}")
del _mine, _theirs, _what

# AND EVERY ROW IN THE `quad` GROUP IS READ SOMEWHERE IN THIS FILE, checked
# against the parameter table itself rather than against a second list of the
# same strings.
#
# This is `bird.py`'s ALT_SLOTS check, generalised, and it is here because a
# parameter group of eighty rows is exactly the size at which one gets added to
# spec.py and forgotten in the generator. What that looks like from outside is a
# slider that exists in the browser, sits at its default forever, and appears in
# no table anywhere -- a feature nobody can tell is not working, which is this
# project's documented failure mode. Reading the table back is the only check
# that cannot itself go stale, and it costs one import-time scan of the source.
#
# It says nothing about whether the row does anything USEFUL. That is
# `tools/quadprobe.py`, which sweeps each row and prints DEAD when the geometry
# does not move.
def _unread_rows() -> list[str]:
    import pathlib
    src = pathlib.Path(__file__).read_text(encoding="utf-8")
    return [p.path for p in _PARAMS
            if p.group == "quad" and p.path not in src]


_missed = _unread_rows()
assert not _missed, (
    "spec.py declares quadruped parameters this generator never reads: "
    + ", ".join(_missed))
del _missed

# Private painting tags. Same arrangement as `bird.py`: drawn into a scratch
# grid so the paint pass can tell a muzzle from a head without a second geometry
# pass, then mapped into the shared rigging vocabulary on the way out.
#
# THE TWO LEG TAGS ARE THE POINT. `T_LEG_FORE` and `T_LEG_HIND` exist so that
# `_TO_SHARED` can put the hind pair on `parts.P_LEG + parts.AXIS_STRIDE` and
# `parts.split_sides` can then make four ids out of them. Drawn under one tag
# and separated afterwards by position, a squirrel -- whose tail is longer than
# its body, so the grid's midpoint falls behind its hips -- would come out with
# all four legs labelled "fore".
T_NONE, T_BODY, T_NECK, T_HEAD, T_MUZZLE = 0, 1, 2, 3, 4
T_TAIL, T_LEG_FORE, T_LEG_HIND, T_EAR, T_HORN, T_MANE = 5, 6, 7, 8, 9, 10

_TO_SHARED = {
    T_NONE: parts.P_NONE,
    T_BODY: parts.P_BODY,
    T_NECK: parts.P_NECK,
    T_HEAD: parts.P_HEAD,
    T_MUZZLE: parts.P_JAW,
    T_TAIL: parts.P_TAIL,
    T_LEG_FORE: parts.P_LEG,
    T_LEG_HIND: parts.P_LEG + parts.AXIS_STRIDE,
    T_EAR: parts.P_EAR,
    T_HORN: parts.P_HORN,
    T_MANE: parts.P_CREST,
}


def build(spec: dict, rng: np.random.Generator, voxel_m: float,
          out: dict | None = None) -> VoxelGrid:
    """One land animal, nose at +x, back at +z, standing on z = 0.

    `out`, when given, collects the part tags on the way past -- `out["tags"]`
    is a uint8 array parallel to the returned grid, valued by the shared ids in
    `forge/parts.py`. An out-dict rather than a second return value so no
    existing caller changes, and rather than a separate entry point so the tags
    cannot drift from the geometry they describe: they are the same pass.

    Same convention as a fish and a bird, and for the same reason -- this is a
    thing with a front, and a preview camera that has to hunt for the side view
    every time is a preview camera nobody uses.

    THE GROUND PLANE IS PART OF THE CONTRACT here in a way it is not for either
    of those. A fish is in mid-water and a bird is in the air or on a branch;
    this animal is standing on something, so the layout is built with z = 0 as
    the ground and every foot is drawn down to it. `tools/quadprobe.py --stance`
    measures the gap between each foot and the lowest voxel of the asset,
    because a limb that stops one voxel short is a floating animal and nothing
    in the health checks can see it (`pipeline.build` crops the grid, so the
    bottom slab is occupied by definition whatever the feet did).
    """
    p = _params(spec, rng, voxel_m)
    tag = VoxelGrid(p["shape"], (0, 0, 0), voxel_m)

    # DRAWING ORDER IS A COLOUR DECISION, and this file inherited the lesson
    # from `bird.build` rather than rediscovering it. The neck, the tail and all
    # four limbs START ONE VOXEL INSIDE THE TRUNK, because an overlap by
    # construction is the only join that cannot come apart. Drawn after the
    # trunk they would then paint their own colour over the chunk of trunk they
    # are buried in, which on a bird put a slab of head colour across every
    # shoulder in the library.
    #
    # So the buried parts go down first and the trunk reclaims its own volume.
    # The head, the muzzle, the ears and the headgear come after and win,
    # because a short-necked animal -- a boar, a badger, a hedgehog -- carries
    # its head partly inside the trunk's outline and the head colour has to
    # survive that.
    _legs(tag, p)
    _tail(tag, p)
    _neck(tag, p)
    body = _body_mask(p)
    tag.data[body] = T_BODY
    # ... and the caps go on immediately after, so they are the TRUNK's voxels
    # and not the limb's. A cap belonging to two parts has no defined answer
    # when both rotate, which is the overlapping-collar problem wearing a hat.
    _caps(tag, p)
    _head(tag, p)
    _ears(tag, p)
    _headgear(tag, p)

    grid = VoxelGrid(p["shape"], (0, 0, 0), voxel_m)
    _paint(grid, tag.data, p)
    if out is not None:
        out["tags"] = parts.to_shared(tag.data, _TO_SHARED)
        out["layout"] = p
    return grid


# --- parameters and layout --------------------------------------------------


def _sex_scale(spec: dict, ratio_path: str) -> float:
    """Multiplier on one measurement for the sex being drawn.

    The fish and bird rule verbatim: the authored number is the average of the
    two sexes and the ratio is male-over-female, applied as its square root
    either way, so male divided by female is exactly the ratio and neither sex
    is the default. Written as `value * ratio` for the male instead, every
    unsexed spec in the library would silently have been describing a female.
    """
    sex = str(get(spec, "quad.sex"))
    if sex == "unsexed":
        return 1.0
    ratio = max(1e-3, float(get(spec, ratio_path)))
    return math.sqrt(ratio) if sex == "male" else 1.0 / math.sqrt(ratio)


def _sex_present(spec: dict, ratio_path: str) -> float:
    """The same, for a part that may be ABSENT on one sex rather than smaller.

    THIS IS THE MECHANISM `docs/biomes/README.md` §4.9 ASKED FOR, and it is
    genuinely different from a scale. `bird._sex_scale` moves a measurement; a
    red deer hind has no antlers at all, which is not a small rack. A ratio of
    0 therefore means "absent on the female", and the square-root rule cannot
    be applied to it -- sqrt(0) is 0 for the male too.

    So the zero is special-cased and says so, rather than being left to produce
    an antlerless stag that nobody would trace back to a rounding rule. Any
    ratio above zero behaves exactly like `_sex_scale`, which keeps a lion's
    mane (present on both, hugely bigger on the male) and a stag's antlers
    (present on one) in one parameter.
    """
    sex = str(get(spec, "quad.sex"))
    ratio = float(get(spec, ratio_path))
    if ratio <= 0.0:
        if sex == "unsexed":
            # An unsexed spec of a species whose male alone carries the part is
            # drawn WITH it. The unsexed draw is the one the library thumbnails
            # and the one a designer looks at, and a stag with no antlers is not
            # the species average, it is a different animal.
            return 1.0
        return 1.0 if sex == "male" else 0.0
    if sex == "unsexed":
        return 1.0
    return math.sqrt(ratio) if sex == "male" else 1.0 / math.sqrt(ratio)


def _unit(v: np.ndarray) -> np.ndarray:
    n = float(np.linalg.norm(v))
    return v / n if n > 1e-9 else np.array([1.0, 0.0, 0.0])


def _dir(deg: float, back: bool = False) -> np.ndarray:
    """A direction in the animal's own vertical plane, from an angle above the
    horizontal. `back` points it toward the tail."""
    a = math.radians(deg)
    return np.array([-math.cos(a) if back else math.cos(a), 0.0, math.sin(a)])


def _params(spec: dict, rng: np.random.Generator, voxel_m: float) -> dict:
    """Everything the drawing code needs, in VOXELS, with this individual's
    variation applied and the layout already solved against the ground plane.

    Variation is folded in HERE rather than in `spec.realize`, for the same
    reason it is for a fish and a bird: that function varies a tree's height,
    crown radius, trunk radius and foliage shells, and a deer has none of those.
    Left to it, every seed of a species would be the same animal with the
    marking phase shuffled -- and a herd of identical zebra is far more obvious
    than a forest of similar oaks.
    """
    amount = float(get(spec, "variation.amount"))

    def u() -> float:
        t = float(rng.random()) * 2.0 - 1.0
        return abs(t) ** 0.6 if t >= 0.0 else -(abs(t) ** 0.6)

    def vary(value: float, slider: str, scale: float = 1.0) -> float:
        return value * (1.0 + amount * float(get(spec, slider)) * scale * u())

    v = float(voxel_m)
    stance = str(get(spec, "quad.stance"))

    # Sex is applied BEFORE the individual variation draw, so a stag and a hind
    # are two draws around two different means rather than one draw scaled --
    # which is what "two animals" has to mean if the word is doing any work.
    length_m = max(vary(float(get(spec, "quad.length_m"))
                        * _sex_scale(spec, "quad.sex_length"),
                        "variation.height"),
                   v * 10.0)
    # HEAD-BODY LENGTH, not total. Every size in `docs/biomes/*.md` is quoted
    # that way ("1.4 head-body / 0.8 shoulder") because a tail is the one
    # measurement field guides disagree about, and a squirrel authored on total
    # length would come out at half the body it should have.
    length_v = max(10.0, length_m / v)

    # --- how the head-body length is divided up -----------------------------
    #
    # NORMALISED, so `quad.length_m` really is the head-body length. The four
    # shares are authored independently -- a designer reading a description
    # knows a giraffe is a third neck and a weasel is nearly all trunk -- and
    # then scaled to sum to one. Without the normalisation, lengthening the
    # neck made the whole animal longer and the length slider was a suggestion.
    shares = {
        "trunk": float(get(spec, "quad.trunk_frac")),
        "neck": min(0.95, max(0.0, vary(float(get(spec, "quad.neck_frac")),
                                        "variation.proportion", 0.6))),
        "head": float(get(spec, "quad.head_frac")),
        "muzzle": float(get(spec, "quad.muzzle_frac")),
    }
    total = max(sum(shares.values()), 1e-6)
    seg = {k: length_v * x / total for k, x in shares.items()}

    trunk_v = max(4.0, seg["trunk"])
    depth_v = max(2.0, trunk_v * vary(float(get(spec, "quad.depth")),
                                      "variation.shape"))
    width_v = max(1.5, depth_v * float(get(spec, "quad.width")))

    # --- the ground solve ---------------------------------------------------
    #
    # THIS IS THE PART A BIRD DOES NOT HAVE. `shoulder_h` and `hip_h` are the
    # heights of the two limb joints above the ground the animal stands on, as
    # fractions of its head-body length, and the trunk is drawn between them.
    # Everything else about the stance falls out: the back's slope is
    # atan2(dz, run), the limb lengths are the two heights, and a bison's hump
    # is a separate bump on top rather than a way of faking a high shoulder.
    sh_v = max(1.0, float(get(spec, "quad.shoulder_h")) * length_v)
    hip_v = max(1.0, sh_v * float(get(spec, "quad.hip_h")))

    # The trunk keeps its authored LENGTH and the rise is what gives, not the
    # other way round. Solved the obvious way -- run = trunk, rise = dz -- a
    # steeply built animal comes out longer than `quad.length_m` says, and the
    # length slider stops meaning anything on exactly the species (bison,
    # hyena, kangaroo) that the height difference exists for.
    dz = float(np.clip(sh_v - hip_v, -0.92 * trunk_v, 0.92 * trunk_v))
    run = math.sqrt(max(trunk_v ** 2 - dz ** 2, 1.0))

    p_hip = np.array([0.0, 0.0, hip_v])
    p_shoulder = p_hip + np.array([run, 0.0, dz])
    axis = _unit(p_shoulder - p_hip)          # trunk axis, pointing forward
    up = np.array([-axis[2], 0.0, axis[0]])   # trunk "up", perpendicular to it

    # --- where things hang off the trunk ------------------------------------
    #
    # EVERY ATTACHMENT IS PLACED AGAINST THE BODY'S OWN LOCAL DEPTH, not against
    # its maximum. That is what `_attach` is for, and it is not tidiness: with
    # the ends of the trunk rounded off (`_profile`), a hip placed at a fixed
    # fraction of the DEEPEST part of the body sits outside the body at the
    # rump, where the body is half that deep. A limb whose joint is outside its
    # parent is a limb that ships as a separate piece -- which happened twice in
    # this file already, once to the tail and once to a kangaroo's forelimbs,
    # for 251 and 1,214 voxels respectively -- and it is the exact failure mode
    # `tools/buildcheck.py` exists for.
    chest_f = float(get(spec, "quad.chest"))
    waist_f = float(get(spec, "quad.waist"))
    rump_f = float(get(spec, "quad.rump"))
    belly_f = float(get(spec, "quad.belly"))
    hump_f = float(get(spec, "quad.hump"))
    hump_at = float(get(spec, "quad.hump_at"))

    def _attach(s_frac: float, up_frac: float) -> np.ndarray:
        """A point on the trunk's surface-ish: `s_frac` along it from the hip,
        and `up_frac` of the LOCAL half-depth above (or below) its axis."""
        t = 1.0 - s_frac
        prof = float(_profile(t, chest_f, waist_f, rump_f))
        half = (depth_v * (prof * (1.0 - belly_f) + float(_hump(t, hump_f, hump_at)))
                if up_frac >= 0.0 else depth_v * prof * belly_f)
        return p_hip + axis * (s_frac * trunk_v) + up * (up_frac * half)

    def _half_w(s_frac: float) -> float:
        t = 1.0 - s_frac
        return 0.5 * width_v * float(_profile(t, chest_f, waist_f, rump_f)) ** _WIDTH_FOLLOW

    # --- neck, head, muzzle -------------------------------------------------
    #
    # THE NECK ANGLE IS ABSOLUTE, measured from the horizontal rather than from
    # the back. See the module docstring: a giraffe holds a vertical neck off a
    # sloping back and a stoat holds a horizontal one off a horizontal back, and
    # authoring the second as an offset from the first means every species is
    # entered by arithmetic against a number it does not care about.
    neck_deg = vary(float(get(spec, "quad.neck_deg")), "variation.shape", 0.4)
    neck_dir = _dir(neck_deg)
    neck_v = seg["neck"]
    head_r = max(1.0, 0.5 * seg["head"] * float(get(spec, "quad.head_size")))
    # The neck leaves the withers high on the trunk rather than off its axis:
    # drawn from the axis, a deep-chested animal's neck emerges from the middle
    # of its own chest and the withers disappear.
    p_neck_base = _attach(S_NECK, 0.55)
    p_head = p_neck_base + neck_dir * (neck_v + head_r * 0.55)

    head_deg = float(get(spec, "quad.head_deg"))
    muz_dir = _dir(head_deg)
    muz_v = max(0.0, seg["muzzle"])
    p_muz_base = p_head + muz_dir * (head_r * 0.55)

    # --- tail ---------------------------------------------------------------
    tail_v = max(0.0, float(get(spec, "quad.tail_len")) * length_v)
    tail_r = max(0.5, float(get(spec, "quad.tail_thick")) * depth_v * 0.5)
    tail_deg = float(get(spec, "quad.tail_deg"))
    tail_arc = float(get(spec, "quad.tail_arc"))
    tail_solved = False
    if stance == "bipedal" and tail_v > 1.0:
        # THE TAIL IS A LEG NOW, so its angle is solved and not authored. The
        # tip has to land on the ground plane or the tripod has two legs; the
        # arcsine below is the angle that does that, and it is clamped rather
        # than allowed to fail, so a tail too short to reach simply lies as low
        # as it can and `tools/quadprobe.py --stance` reports the gap. Silently
        # authoring 0 here would give a kangaroo a tail sticking out behind it
        # at knee height, which reads as a bug in the renderer.
        #
        # SOLVED AGAINST THE HEIGHT THE TAIL IS ACTUALLY DRAWN FROM, which is
        # three separate terms above the hip joint and was the whole of this
        # mechanism's first failure. Solved against the hip alone, all three
        # bipeds in the library came out with the tail tip 5 to 9 voxels clear
        # of the floor: the tail leaves the rump ABOVE the hip (`_attach`), the
        # drawing insets its start FORWARD along the trunk -- which on a
        # near-vertical trunk is mostly upward -- and `quad.tail_arc` bends the
        # last of it away again. All three are here, and `--stance` is what
        # found that they had to be.
        base = _attach(S_TAIL, 0.55) + axis * min(3.0, 0.20 * trunk_v)
        lift = tail_arc * tail_v * 0.55
        want = float(np.clip((-base[2] - lift) / tail_v, -1.0, 1.0))
        tail_deg = math.degrees(math.asin(want))
        tail_solved = True
    tail_dir = _dir(tail_deg, back=True)
    # The tail leaves the rump ABOVE the trunk axis, which is where a tail
    # actually attaches; from the axis it emerges out of the animal's backside
    # at mid-height and a raised tail looks stuck on.
    #
    # ON the rump and not behind it. Set 12% of the trunk's length BACK from the
    # hip -- which is where it went first, on the reasoning that a tail hangs off
    # the back of an animal -- the base sat outside the body altogether, and
    # since `_tail` insets its start by only a couple of voxels the tail came
    # off on anything longer than about 20 voxels. It shipped as a 251-voxel
    # second piece.
    p_tail_base = _attach(S_TAIL, 0.55)

    # --- limbs --------------------------------------------------------------
    #
    # Attachment points, and this is the one place the stance changes GEOMETRY
    # rather than a number. A sprawling limb leaves the FLANK: its shoulder is
    # out at the side of the body at trunk height, and it reaches outward before
    # it reaches down. A standing or bipedal limb leaves the belly, under the
    # trunk. No angle on a limb attached under the body produces a sprawl,
    # which is why the attachment moves and not the angle.
    if stance == "sprawling":
        # OUT AT THE FLANK, at about the height of the trunk's own axis. The
        # attachment point itself moves, which is the thing no angle on a limb
        # hanging under the body can imitate.
        y_off = max(1.0, 0.90 * _half_w(S_FORE))
        p_fore = _attach(S_FORE, -0.15)
        p_hind = _attach(S_HIND, -0.15)
    else:
        y_off = max(1.0, 0.72 * _half_w(S_FORE))
        p_fore = _attach(S_FORE, -0.80)
        p_hind = _attach(S_HIND, -0.80)
    if stance == "bipedal":
        # The forelimbs come OFF THE FRONT OF THE CHEST, high and near the
        # trunk's own axis, which is where a kangaroo carries them. Left under
        # the shoulder they hang beside the belly and read as a quadruped that
        # has lost its footing.
        #
        # JUST INSIDE THE CHEST, not in front of it. Placed 16% of the trunk's
        # length forward of the shoulder -- which is where "off the chest" first
        # put it -- the attachment sat outside the body and both forelimbs
        # shipped as separate pieces, 602 and 612 voxels. The front of the trunk
        # is the front of the trunk; there is nothing in front of it to attach
        # to.
        p_fore = _attach(S_CHEST, 0.10)

    fore_reach = float(get(spec, "quad.fore_reach"))
    # DELIBERATELY NOT GATED ON THE STANCE. A parameter that only works in one
    # setting of another is this project's documented trap -- `bill_gape`
    # multiplied by `bill_depth` did nothing below heron size for as long as it
    # did -- so `fore_reach` is honoured in every stance, its default is 1.0,
    # and `tools/quadprobe.py --stance` measures the fore-foot's height above
    # the ground on every species so that an animal accidentally authored on
    # tiptoe is a number rather than a squint.
    fore_len = max(1.0, p_fore[2] * fore_reach)
    hind_len = max(1.0, p_hind[2])
    # Kept as its own entry rather than recovered from the two above, because
    # `_legs` works in grid coordinates and the two heights are in layout ones,
    # and dividing one by the other at the call site is a second expression of
    # the same fact -- which is what this project has paid for repeatedly.

    # --- HOW THICK A LIMB IS, AND WHAT IT IS THICK RELATIVE TO ---------------
    #
    # A LIMB'S THICKNESS IS A FRACTION OF ITS OWN LENGTH, and getting that
    # reference wrong is the whole of why the land animals shipped looking
    # wrong (owner, 2026-08-15: "tall lanky with narrow legs and bodies").
    #
    # It used to be a fraction of the TRUNK'S DEPTH, which is three
    # multiplications away from anything the eye judges:
    #
    #     leg_r = leg_thick * depth_v * 0.5
    #     depth_v = quad.depth * trunk_v
    #     trunk_v = (trunk_frac / the four shares) * length_v
    #
    # Every one of those factors is below 1, and the species that need the
    # thickest legs are the ones that shrink all three. A gemsbok is a quarter
    # neck, so its trunk share is 0.52 where a boar's is 0.58; a running build
    # is shallow, so its `quad.depth` is 0.42 where a boar's is 0.46. Nothing in
    # that chain knows how LONG the leg is, and the leg is long precisely
    # because the animal is tall -- so the taller the animal, the thinner its
    # legs looked, which is the definition of lanky and is exactly the set of
    # species the owner picked out.
    #
    # MEASURED, on the eight species he named: thickness over length came out
    # at 0.111 to 0.282 on the three he called solid and 0.055 to 0.102 on the
    # five he called wireframes -- two groups with no overlap. The two
    # obvious alternative ratios do NOT separate them, and both were tried
    # first: thickness in VOXELS put the wireframes on top (3.0 to 4.0 against
    # 3.0 to 3.5), and thickness over WITHERS HEIGHT put a wild boar at 0.055
    # against a gemsbok at 0.050. See `tools/quadprobe.py --bulk`.
    #
    # SO THE REFERENCE IS THE LIMB'S OWN LENGTH, joint to ground, averaged over
    # the fore and hind pair because one radius draws all four legs. That also
    # makes `quad.leg_thick` the SAME NUMBER the published corpora are quoted
    # in -- Infinigen's photoreal quadruped genome 0.111 behind and 0.140 in
    # front, Veloren 0.230, Minecraft 0.393
    # (`docs/quadruped-proportion-research.md`) -- so a designer can read a
    # figure off that table and type it in, instead of dividing it by a trunk
    # depth they have to derive first.
    #
    # A FLOOR OF TWO VOXELS AND NOT ONE. The old floor was half a voxel of
    # radius, which draws a limb one voxel across, and a one-voxel limb is a
    # wire at any lattice. Neither shipped voxel corpus contains one: the
    # thinnest load-bearing limb in ten Minecraft mobs is 2 units (wolf, fox)
    # and the thinnest of 48 Veloren quadruped parts is 2 voxels. One-voxel
    # features do ship in both, but only as detail that carries no weight --
    # Minecraft's cow horn is [1, 3, 1]. Forty-eight species in this library
    # had a foreleg one or two voxels across before this change.
    #
    # DELIBERATELY NOT A max() AGAINST THE OLD EXPRESSION. A floor would have
    # left `quad.leg_thick` authored on eighty species and doing nothing on
    # them, which is this project's signature failure and would have been
    # invisible in every render -- the sweep in `tools/quadprobe.py` would
    # have started reporting the row DEAD and been right. One reference, one
    # meaning, and the 131 authored values were converted to it.
    limb_v = max(1.0, 0.5 * (fore_len + hind_len))
    leg_r = max(1.0, 0.5 * float(get(spec, "quad.leg_thick")) * limb_v)

    # --- ears, headgear -----------------------------------------------------
    ear = str(get(spec, "quad.ear_shape"))
    ear_v = float(get(spec, "quad.ear_len")) * length_v
    horn = str(get(spec, "quad.horn_shape"))
    horn_scale = _sex_present(spec, "quad.sex_horn")
    horn_v = float(get(spec, "quad.horn_len")) * length_v * horn_scale
    mane = float(get(spec, "quad.mane")) * _sex_present(spec, "quad.sex_mane")

    # --- bounding box, from what will ACTUALLY be drawn ---------------------
    #
    # Sized against the LAYOUT, not against a round number. A horn or a raised
    # tail that runs off the edge of the grid is clipped silently by
    # `VoxelGrid._write`, which is this project's signature failure: the feature
    # runs, reports success, and half of it is not there. Every extreme below is
    # a point some species actually reaches.
    pts = [
        p_hip + up * depth_v, p_hip - up * depth_v,
        p_shoulder + up * depth_v, p_shoulder - up * depth_v,
        p_head + head_r * 1.4, p_head - head_r * 1.4,
        p_muz_base + muz_dir * muz_v + np.array([0.0, 0.0, -muz_v * 0.6]),
        p_muz_base + muz_dir * muz_v + np.array([0.0, 0.0, muz_v * 0.6]),
        p_tail_base + tail_dir * tail_v,
        p_tail_base + tail_dir * tail_v + np.array([0.0, 0.0, tail_r * 3.0]),
        p_tail_base + tail_dir * tail_v - np.array([0.0, 0.0, tail_r * 3.0]),
        p_head + np.array([0.0, 0.0, head_r + ear_v * 1.05]),
        p_head + np.array([0.0, 0.0, head_r + horn_v * 1.15]),
        p_head + np.array([horn_v * 0.9, 0.0, head_r]),
        p_head - np.array([horn_v * 1.1, 0.0, 0.0]),
        # The lowest point is the ground, always: every foot is drawn to z = 0
        # and nothing is allowed below it.
        np.array([p_fore[0], 0.0, 0.0]),
        np.array([p_hind[0] - hind_len * 0.5, 0.0, 0.0]),
        np.array([p_hind[0] + hind_len * 0.7, 0.0, 0.0]),
    ]
    arr = np.array(pts, dtype=np.float64)
    lo = arr.min(axis=0)
    hi = arr.max(axis=0)

    margin = 3.0
    # HOW FAR A SPRAWLED LIMB ACTUALLY REACHES SIDEWAYS, taken from the same
    # coefficients `_legs` uses rather than from a guess. Guessed at 0.55 of the
    # limb's length it was short by a third: the knee alone goes out 0.85 of it
    # and the heel a further 0.30, so a water monitor's hind feet were drawn
    # past the edge of its own grid and `VoxelGrid._write` dropped them without
    # a word. What shipped was an animal with one voxel of each hind foot cut
    # off and floating -- 2 voxels out of 1,325, found by the one-piece rule in
    # `tools/buildcheck.py` and by nothing else. It is the exact failure the
    # "size the box from the layout" rule is here to stop, and it still happened,
    # because the box was sized from a DIFFERENT expression of the layout.
    sprawl_reach = (1.15 * max(hind_len, fore_len) + leg_r
                    if stance == "sprawling" else 0.0)
    half_y = margin + max(
        0.5 * width_v + leg_r * 2.0,
        y_off + leg_r + sprawl_reach,
        (0.5 * width_v + ear_v * 0.9) if ear in ("blade", "fan") else 0.0,
        0.5 * float(get(spec, "quad.horn_spread")) * horn_v + 2.0,
    )
    nx = int(math.ceil(hi[0] - lo[0])) + 2 * int(margin)
    ny = int(math.ceil(2.0 * half_y))
    nz = int(math.ceil(hi[2] - min(lo[2], 0.0))) + int(margin) + 2
    # The GROUND SITS TWO VOXELS OFF THE FLOOR of the array rather than on it.
    # Not cosmetic: `grid.ball` writes a foot as a sphere, and a sphere centred
    # on z = 0 has half of itself at negative z, which `_write` drops in
    # silence. Two voxels of headroom means the foot is drawn whole and
    # `pipeline.build`'s crop takes the empty slabs back off, so the asset still
    # begins at its own soles.
    off = np.array([margin - lo[0], half_y, 2.0])

    return {
        "voxel_m": v,
        "shape": (max(nx, 8), max(ny, 8), max(nz, 8)),
        "stance": stance,
        "length_v": length_v,
        "trunk_v": trunk_v,
        "neck_v": neck_v,
        "muz_v": muz_v,
        "head_r": head_r,
        "depth_v": depth_v,
        "width_v": width_v,
        "axis": axis,
        "up": up,
        "ground_z": float(off[2]),
        "p_hip": p_hip + off,
        "p_shoulder": p_shoulder + off,
        "p_neck_base": p_neck_base + off,
        "p_head": p_head + off,
        "p_muz_base": p_muz_base + off,
        "p_tail_base": p_tail_base + off,
        "p_fore": p_fore + off,
        "p_hind": p_hind + off,
        "neck_dir": neck_dir,
        "muz_dir": muz_dir,
        "tail_dir": tail_dir,
        "tail_deg": tail_deg,
        "tail_solved": tail_solved,
        "ycen": float(half_y),
        "y_off": y_off,
        # trunk profile
        "chest": float(get(spec, "quad.chest")),
        "waist": float(get(spec, "quad.waist")),
        "rump": float(get(spec, "quad.rump")),
        "belly": float(get(spec, "quad.belly")),
        "section": float(get(spec, "quad.section")),
        "hump": float(get(spec, "quad.hump")),
        "hump_at": float(get(spec, "quad.hump_at")),
        # neck and head
        "neck_thick": float(get(spec, "quad.neck_thick")),
        "neck_taper": float(get(spec, "quad.neck_taper")),
        "mane": mane,
        "dewlap": float(get(spec, "quad.dewlap")),
        "muz_depth": float(get(spec, "quad.muzzle_depth")),
        "muz_width": float(get(spec, "quad.muzzle_width")),
        "muz_drop": float(get(spec, "quad.muzzle_drop")),
        "jaw": float(get(spec, "quad.jaw")),
        "eye": float(get(spec, "quad.eye")),
        # ears
        "ear": ear,
        "ear_v": ear_v,
        "ear_width": float(get(spec, "quad.ear_width")),
        "ear_deg": float(get(spec, "quad.ear_deg")),
        "ear_back": float(get(spec, "quad.ear_back")),
        # headgear
        "horn": horn,
        "horn_v": horn_v,
        "horn_thick": float(get(spec, "quad.horn_thick")),
        "horn_spread": float(get(spec, "quad.horn_spread")),
        "horn_curl": float(get(spec, "quad.horn_curl")),
        "horn_tines": int(get(spec, "quad.horn_tines")),
        # tail
        "tail_v": tail_v,
        "tail_r": tail_r,
        "tail_taper": float(get(spec, "quad.tail_taper")),
        "tail_tuft": float(get(spec, "quad.tail_tuft")),
        "tail_arc": tail_arc,
        # limbs
        "leg_r": leg_r,
        "fore_len": fore_len,
        "hind_len": hind_len,
        "fore_reach": fore_reach,
        "fore_bend": float(get(spec, "quad.fore_bend")),
        "hock": float(get(spec, "quad.hock")),
        "foot": float(get(spec, "quad.foot")),
        "stand_v": max(hind_len, fore_len),
        # colour
        "mark": str(get(spec, "quad.mark")),
        "mark_count": int(get(spec, "quad.mark_count")),
        "mark_width": float(get(spec, "quad.mark_width")),
        "mark_strength": float(get(spec, "quad.mark_strength")),
        "under": float(get(spec, "quad.under")),
        "cape": float(get(spec, "quad.cape")),
        "stocking": float(get(spec, "quad.stocking")),
        "tail_tip": float(get(spec, "quad.tail_tip")),
        "phase": float(rng.random()),
        "salt": int(rng.integers(1 << 30)),
        "sex": str(get(spec, "quad.sex")),
        "mat_back": materials.resolve(get(spec, "materials.quad_back")),
        "mat_belly": materials.resolve(get(spec, "materials.quad_belly")),
        "mat_head": materials.resolve(get(spec, "materials.quad_head")),
        "mat_leg": materials.resolve(get(spec, "materials.quad_leg")),
        "mat_tail": materials.resolve(get(spec, "materials.quad_tail")),
        "mat_mark": materials.resolve(get(spec, "materials.quad_mark")),
        "mat_horn": materials.resolve(get(spec, "materials.quad_horn")),
        "mat_eye": materials.resolve(get(spec, "materials.quad_eye")),
    }


# --- the swept solid, which draws the trunk AND the muzzle -------------------


def _stations(shape, ycen: float, origin, fwd, up):
    """Distance along an axis and height above it, per (x, z) cell.

    (nx, nz) and NOT (nx, ny, nz) on purpose, exactly as `bird._stations` is:
    the axis lies in the x-z plane so neither quantity depends on y at all, and
    a 3 m bison at 5 cm is 60 voxels wide -- three full meshgrids of that is
    tens of megabytes of float64 to compute something constant along one axis.
    """
    nx, ny, nz = shape
    xs = (np.arange(nx, dtype=np.float64) + 0.5) - origin[0]
    zs = (np.arange(nz, dtype=np.float64) + 0.5) - origin[2]
    s = xs[:, None] * fwd[0] + zs[None, :] * fwd[2]
    h = xs[:, None] * up[0] + zs[None, :] * up[2]
    return s, h


def _sweep(shape, ycen: float, origin, fwd, up, length: float,
           halfw, halfup, halfdn, section: float) -> np.ndarray:
    """A solid whose superellipse cross-section varies along one axis.

    ONE FUNCTION, THREE USERS -- the trunk, the muzzle and the dewlap all want
    exactly this and differ only in their three profiles. Written three times it
    would be three places for the half-voxel floor below to be forgotten, and
    that floor is what stops a thin end of anything from coming apart: below
    half a voxel a cross-section contains no cell centre at all.

    `halfw`, `halfup` and `halfdn` are arrays sampled on a uniform t grid from
    0 (the front end) to 1 (the back end).
    """
    nx, ny, nz = shape
    s, h = _stations(shape, ycen, origin, fwd, up)
    length = max(length, 1.0)
    tt = np.clip(1.0 - s / length, 0.0, 1.0)
    valid = (s >= -0.5) & (s <= length + 0.5)

    tg = np.linspace(0.0, 1.0, len(halfw))
    wmap = np.maximum(np.interp(tt, tg, halfw), 0.5)
    umap = np.maximum(np.interp(tt, tg, halfup), 0.5)
    dmap = np.maximum(np.interp(tt, tg, halfdn), 0.5)
    vmap = np.where(h >= 0.0, umap, dmap)

    y = np.abs((np.arange(ny, dtype=np.float64) + 0.5) - ycen)
    n = max(section, 0.8)
    lhs = (y[None, :, None] / wmap[:, None, :]) ** n
    rhs = (np.abs(h) / vmap)[:, None, :] ** n
    occ = (lhs + rhs) <= 1.0 + 1e-9
    occ &= valid[:, None, :]
    return occ


# How hard the trunk's width follows its depth. NOT A SLIDER, and deliberately,
# for the reason `bird._WIDTH_FOLLOW` gives: a slider whose only correct setting
# is one value is a slider that can be set wrong. A fish exposes the equivalent
# because a fast fish's caudal peduncle is a blade and a slow one's is a tube;
# a mammal's trunk is a barrel from the shoulder to the hip on everything from a
# stoat to an elephant.
_WIDTH_FOLLOW = 0.75

# WHERE ALONG THE TRUNK EACH THING HANGS OFF, as a distance from the hip in
# units of the trunk's own length. Named constants rather than numbers scattered
# through `_params`, because `_attach` has to evaluate the body's profile at
# exactly the same place the part is put -- and the whole point of `_attach` is
# that the two cannot disagree.
S_FORE = 0.88     # shoulder joint, near the front of the trunk
S_HIND = 0.14     # hip joint, near the back
S_NECK = 0.95     # where the neck leaves the withers
S_TAIL = 0.10     # where the tail leaves the rump
S_CHEST = 0.93    # the bipedal forelimb, which comes off the chest


def _profile(t, chest: float, waist: float, rump: float):
    """The trunk's depth along its own length, as a fraction of the deepest
    point. t = 0 at the shoulder end, t = 1 at the rump.

    THREE AUTHORED ANCHORS -- chest, waist, rump -- rather than a bump function,
    because that is how the shape is described in every source the species list
    was written from: "heaviest at the shoulder", "barrel", "high-rumped". The
    middle anchor is a genuine value at t = 0.5 and not a Bezier control point;
    the algebra below is what makes it so, and without it a designer who types
    0.8 for the waist gets 0.65 and never finds out why.

    AND THEN BOTH ENDS ARE ROUNDED OFF, which is the correction that mattered
    most in this file. Written without the cap the trunk is a cylinder with two
    flat walls, and since `_sweep` cuts it dead at s = 0 and s = length, every
    animal in the first render of the library came out as a SLAB: a rectangle in
    side view with a horizontal white plane for a belly, legs hanging off the
    underside of a box and a flat wall where the chest should be. It was the
    single most obvious thing wrong with the generator and it was one missing
    term.

    The cap is confined to the outer fifth at each end and takes the depth to
    about half, which is a rounded chest and a rounded rump rather than a
    tapered point -- an animal's trunk really does end bluntly, it just does not
    end in a wall. The three anchors therefore describe the BARREL between the
    two caps, which is worth knowing before retuning one.
    """
    t = np.asarray(t, dtype=np.float64)
    ctrl = max(0.05, 2.0 * waist - 0.5 * (chest + rump))
    prof = ((1 - t) ** 2) * chest + 2 * t * (1 - t) * ctrl + (t ** 2) * rump
    cap = 0.52 + 0.48 * np.sqrt(np.clip(1.0 - np.abs(2.0 * t - 1.0) ** 6, 0.0, 1.0))
    return np.clip(prof, 0.06, 2.0) * cap


def _hump(t, amount: float, at: float):
    """Extra depth over the withers, TOP ONLY.

    Added to the depth instead, it would push the belly down as well and a bison
    would come out with a bulge underneath it that no animal has. `at` is
    measured from the shoulder end, so 0.15 is just behind the withers.
    """
    return amount * np.exp(-(((np.asarray(t, dtype=np.float64) - at) / 0.22) ** 2))


def _trunk_profiles(p: dict, n: int = 96):
    """Half-width, half-depth above the axis and half-depth below it, sampled
    along the trunk ready for `np.interp`."""
    t = np.linspace(0.0, 1.0, n)
    prof = _profile(t, p["chest"], p["waist"], p["rump"])
    d = p["depth_v"]
    up = d * (prof * (1.0 - p["belly"]) + _hump(t, p["hump"], p["hump_at"]))
    dn = d * prof * p["belly"]
    # WIDTH FOLLOWS DEPTH, but not one for one. `_WIDTH_FOLLOW` is the exponent
    # the bird file settled on for the same reason: a body narrows toward its
    # ends more slowly than it shallows, so a width proportional to the depth
    # profile pinches the rump into a blade. The bird fixes this at 0.75 and
    # does not expose it; the same applies here and for a stronger reason, since
    # a mammal's trunk really is close to a solid of revolution.
    w = 0.5 * p["width_v"] * (np.clip(prof, 1e-4, None) ** _WIDTH_FOLLOW)
    return np.maximum(w, 0.5), np.maximum(up, 0.5), np.maximum(dn, 0.5)


def _body_mask(p: dict) -> np.ndarray:
    """The trunk, as a boolean grid."""
    w, up, dn = _trunk_profiles(p)
    occ = _sweep(p["shape"], p["ycen"], p["p_hip"], p["axis"], p["up"],
                 p["trunk_v"], w, up, dn, p["section"])

    # THE AXIS GOES DOWN AS AN UNBROKEN RUN, hip to shoulder. Same rule as
    # `grid.capsule` and the same rule the fish and the bird both needed: a
    # shape thinner than one voxel exists as its centreline or it does not exist
    # at all. A stoat at 1 cm is under two voxels through the waist, and without
    # this it ships as a front half and a back half.
    tmp = VoxelGrid(p["shape"], (0, 0, 0), p["voxel_m"])
    tmp.line(_mid(p, p["p_hip"]), _mid(p, p["p_shoulder"]), 1)
    occ |= tmp.data != 0
    return occ


def _mid(p: dict, point, dy: float = 0.0) -> np.ndarray:
    """A layout point forced onto the mirror plane (or `dy` voxels off it)."""
    return np.array([point[0], p["ycen"] + dy, point[2]])


# --- neck, head, muzzle ------------------------------------------------------


def _neck(tag: VoxelGrid, p: dict) -> None:
    """Withers to the middle of the head, plus the mane and the dewlap.

    Drawn with `VoxelGrid.capsule`, which lays a face-connected centreline down
    BEFORE it thickens anything. That is the only reason a stoat's neck -- which
    is under a voxel at the 1 cm lattice -- is a neck rather than a gap between
    a body and a floating head.

    It starts one voxel INSIDE the trunk and ends at the head's CENTRE rather
    than at its surface. Both joins are overlaps by construction; measuring a
    join afterwards and hoping is what puts heads on the floor.
    """
    r0 = max(0.5, p["neck_thick"] * p["depth_v"] * 0.5)
    r1 = max(0.5, r0 * p["neck_taper"])
    start = p["p_neck_base"] - p["axis"] * min(2.5, 0.22 * p["trunk_v"])
    end = p["p_head"]
    tag.capsule(_mid(p, start), _mid(p, end), r0, r1, T_NECK)

    # --- the mane / dorsal crest --------------------------------------------
    # A zebra's stiff brush, a wildebeest's beard, a boar's bristle crest, a
    # lion's mane, a hyena's erectile ridge. Swept along the TOP of the neck
    # rather than added to its radius, because a thicker neck is a different
    # animal and a ridge on top of a normal neck is this one.
    if p["mane"] > 0.02 and r0 >= 0.8:
        rise = p["mane"] * p["depth_v"] * 0.8
        if rise >= 1.0:
            perp = np.array([-p["neck_dir"][2], 0.0, p["neck_dir"][0]])
            steps = max(2, int(round(np.linalg.norm(end - start) / max(1.0, r0 * 0.7))))
            prev = None
            for k in range(steps + 1):
                s = k / steps
                c = start + (end - start) * s
                rr = r0 + (r1 - r0) * s
                top = c + perp * (rr * 0.75)
                if prev is not None:
                    tag.capsule(_mid(p, prev), _mid(p, top),
                                max(0.5, rise * 0.5), max(0.5, rise * 0.5), T_MANE)
                prev = top

    # --- the dewlap / throat bell -------------------------------------------
    # A moose's bell, a kudu's fringe, a Barbary sheep's chest hair, a zebu's
    # dewlap. A hanging lobe under the throat, tagged as neck because that is
    # what it rotates with.
    if p["dewlap"] > 0.02:
        hang = p["dewlap"] * p["depth_v"]
        if hang >= 1.2:
            mid = start + (end - start) * 0.55
            perp = np.array([-p["neck_dir"][2], 0.0, p["neck_dir"][0]])
            base = mid - perp * (r0 * 0.6)
            tag.capsule(_mid(p, base), _mid(p, base - np.array([0.0, 0.0, hang])),
                        max(0.6, r0 * 0.75), max(0.5, r0 * 0.45), T_NECK)


def _head(tag: VoxelGrid, p: dict) -> None:
    """Skull and muzzle. Both win over the neck and the trunk."""
    r = p["head_r"]
    c = _mid(p, p["p_head"])
    tag.ball(c, r, T_HEAD)
    if r >= 1.6:
        # A mammal's skull is longer than it is tall on everything except a cat
        # and an ape, and a bare sphere on the end of a neck reads as a
        # lollipop. Two extra balls fore and aft cost nothing and give it an
        # axis, which is the same trick `bird._head_bill` uses.
        tag.ball(_mid(p, p["p_head"] + p["muz_dir"] * r * 0.40), r * 0.86, T_HEAD)
        tag.ball(_mid(p, p["p_head"] - p["muz_dir"] * r * 0.38), r * 0.88, T_HEAD)
    _muzzle(tag, p)


def _muzzle(tag: VoxelGrid, p: dict) -> None:
    """A BOX THAT CONTINUES THE SKULL, with a jawline. Not a bill.

    `bird._bill` is a chain of shrinking balls along a curved centreline, which
    is right for a spike of keratin and wrong for a face: a wolf's muzzle is a
    square-sided block that gets slightly narrower toward the nose and keeps its
    depth almost to the end, and drawn as a cone it becomes a beak. So this is
    the same swept superellipse the trunk uses, at a high section exponent, and
    the difference between a bear (short, deep, blunt) and a wolf (long,
    shallow, tapering) is three numbers rather than a different primitive.

    `muzzle_drop` bends the centreline DOWNWARD along its length rather than
    rotating the whole muzzle, and that distinction is the one `bird.bill_curve`
    got wrong for several passes: rotated, the tip moves and the muzzle stays
    straight, which looks close enough in a render to survive review. A moose's
    overhanging muzzle is a bend.
    """
    n = 24
    t = np.linspace(0.0, 1.0, n)
    if p["muz_v"] < 1.0:
        return
    r = p["head_r"]
    # t = 0 is the NOSE end and t = 1 is where it meets the skull, to match
    # `_sweep`'s convention (0 at the far end of the axis).
    #
    # THE NOSE END KEEPS 60% OF ITS WIDTH. Taken to 42% the muzzle read as a
    # beak on every long-faced species in the library -- which is the exact
    # distinction between this and `bird._bill`, so getting it wrong here meant
    # writing a second bill and calling it a muzzle.
    taper = 0.60 + 0.40 * t
    half_w = np.maximum(r * p["muz_width"] * taper, 0.5)
    half_up = np.maximum(r * p["muz_depth"] * (0.55 + 0.45 * t), 0.5)
    # The JAWLINE: the lower half is deeper than the upper one and squarer,
    # which is what makes a muzzle read as a mouth rather than as a snout.
    half_dn = np.maximum(half_up * (0.75 + 0.75 * p["jaw"]), 0.5)

    # Bent, not rotated. `_sweep` works on a straight axis, so the bend is a
    # chain of short straight sweeps -- three is enough for a bend of the size a
    # muzzle has, and each one costs a full grid pass, so more is not free.
    #
    # THE INDEXING HERE IS THE PART THAT WENT WRONG FIRST and the failure is
    # worth naming, because it looked exactly like a working muzzle. `_sweep`
    # takes the axis's REAR end as its origin and runs forward, and the arrays
    # it interpolates are ordered with t = 0 at the FAR end. Handed
    # `base + d * seg_len` as the origin, the whole muzzle was drawn one segment
    # further forward than the skull -- 145 voxels of perfectly good muzzle
    # floating in front of the animal's face. It rendered as a gap of one voxel
    # in a side view and `pipeline.build`'s connectivity check found it, which is
    # the entire argument for that check running on this kind.
    segs = 3 if p["muz_drop"] > 0.05 else 1
    occ = np.zeros(p["shape"], bool)
    base = p["p_muz_base"].copy()
    seg_len = p["muz_v"] / segs
    for k in range(segs):
        a = k / segs
        b = (k + 1) / segs
        deg = -p["muz_drop"] * 34.0 * (a + b) * 0.5
        d = _unit(p["muz_dir"] + np.array([0.0, 0.0, math.sin(math.radians(deg))]))
        # Distance from the skull is (1 - t) * muz_v, so this segment covers t
        # from 1-b at its forward end to 1-a at its rear one -- and `_sweep`
        # wants index 0 to be the forward end.
        idx = np.linspace(1.0 - b, 1.0 - a, 12)
        occ |= _sweep(p["shape"], p["ycen"], base, d, np.array([0.0, 0.0, 1.0]),
                      seg_len,
                      np.interp(idx, t, half_w),
                      np.interp(idx, t, half_up),
                      np.interp(idx, t, half_dn),
                      max(2.4, p["section"]))
        base = base + d * seg_len
    tag.data[occ] = T_MUZZLE


# --- ears --------------------------------------------------------------------


def _ears(tag: VoxelGrid, p: dict) -> None:
    """A paired plate or cone on the skull.

    On a hare, a fennec fox, a kangaroo or an African elephant the ears are the
    entire species, and nothing in this library made one before. Six outlines,
    and they differ in what they ARE rather than in how big they are:

      round    a low disc lying against the skull -- a bear, a mouse, a boar.
      pointed  an erect cone -- a fox, a wolf, a cat, a deer.
      blade    a long flat paddle standing up off the skull -- a hare, a
               kangaroo, a donkey. This is the one whose length is the species.
      fan      a broad sheet hanging back along the neck -- an elephant. Wider
               than it is long, which no setting of `blade` reaches.
      tufted   a pointed ear with a spike of hair off the tip -- a lynx, a
               caracal, a red squirrel in winter.

    Every ear STARTS INSIDE THE SKULL, one radius in, for the same reason the
    neck does: an overlap by construction is the only join that survives, and
    `forge.parts.joints` reports no joint at all for a part touching its parent
    only at a corner.
    """
    if p["ear"] == "none" or p["ear_v"] < 1.0:
        return
    r = p["head_r"]
    ear_v = p["ear_v"]
    width = max(0.5, p["ear_width"] * ear_v * 0.5)
    # Set back on the skull and up, which is where an ear is; centred on the
    # head they emerge from the temples and the animal looks startled.
    back = p["p_head"] - p["muz_dir"] * (r * 0.35) + np.array([0.0, 0.0, r * 0.45])
    a = math.radians(p["ear_deg"])
    for sgn in (-1.0, 1.0):
        # Out and up: `ear_deg` 90 is straight up, 0 is straight out sideways.
        d = np.array([-p["ear_back"] * 0.7, sgn * math.cos(a), math.sin(a)])
        d = d / max(np.linalg.norm(d), 1e-6)
        root = back + np.array([0.0, sgn * r * 0.45, 0.0]) - d * (r * 0.35)
        tip = root + d * ear_v

        if p["ear"] in ("round",):
            tag.capsule(root, root + d * (ear_v * 0.65),
                        max(0.6, width), max(0.6, width * 0.85), T_EAR)
        elif p["ear"] in ("pointed", "tufted"):
            tag.capsule(root, tip, max(0.6, width), 0.5, T_EAR)
            if p["ear"] == "tufted":
                # The tuft is a quarter of the ear again, and on a lynx it is
                # the thing you can see at a hundred metres.
                tag.capsule(tip, tip + d * (ear_v * 0.35), 0.5, 0.5, T_EAR)
        else:
            # A PLATE, NOT A ROD, and this is the correction the first render
            # made obvious. Drawn as a chain of balls along its own length -- the
            # obvious way, and what `pointed` correctly does -- a blade ear comes
            # out as a tapering CYLINDER: on a red kangaroo the ears rendered as
            # two thin spikes and the animal's most identifiable feature after
            # its stance was a pair of antennae. An ear is flat, and what makes
            # it flat is that its width lies in a plane and its thickness does
            # not.
            #
            # The plane is the one perpendicular to the ear's own axis and to
            # the animal's mirror plane, which works out as roughly fore-aft:
            # that is the correct way round, because a long ear seen from the
            # side -- which is the review camera -- should show its whole
            # outline and not its edge.
            w_dir = np.array([-d[2], 0.0, d[0]])
            w_dir = w_dir / max(np.linalg.norm(w_dir), 1e-6)
            long_v = ear_v if p["ear"] == "blade" else ear_v * 0.8
            span = width if p["ear"] == "blade" else max(width, ear_v * 0.62)
            thick = max(0.5, min(1.4, span * 0.35))
            steps = max(2, int(round(long_v)))
            for k in range(steps + 1):
                s = k / steps
                c = root + d * (long_v * s)
                # Widest a third of the way up and tapering to a rounded tip,
                # which is the outline of every long ear there is; a constant
                # width reads as a plank.
                ww = span * (0.45 + s * (1.0 - s) * 2.2)
                tag.capsule(c - w_dir * ww, c + w_dir * ww, thick, thick, T_EAR)


# --- headgear ----------------------------------------------------------------


def _headgear(tag: VoxelGrid, p: dict) -> None:
    """Horns and antlers.

    WHY THE ANTLERS ARE NOT GROWN BY `forge/skeleton.py`. The biome survey's §4.5
    is right that a stag's antlers are a very small tree, and `skeleton.grow`
    does build branching structures with orders, radii and taper -- but it is a
    SPACE COLONIZATION algorithm: it fills an authored crown envelope by growing
    toward attraction points, driven by about twenty tree parameters (influence
    radius, kill distance, step length, phototropism, shade). Handed a rack's
    worth of that, it produces a bush. An antler's tines do not grow toward
    light; they leave the beam at authored positions in an authored order, which
    is what makes a six-point rack a six-point rack. So this is a beam and a
    tine loop -- about forty lines -- and `skeleton.py` is left to the thing it
    is good at.

    The one measured fact from the survey that this file has to respect is that
    ROUND TINES DO NOT SURVIVE A COARSE LATTICE AND PALMATE BLADES DO. That is
    why `palmate` is a separate shape rather than `branched` with wide tines:
    a moose's rack is a flat blade with points on its edge, and at 5 cm the
    blade is three voxels thick and reads while a 3 cm round beam is under one.
    """
    if p["horn"] == "none" or p["horn_v"] < 1.5:
        return
    r = p["head_r"]
    base_r = max(0.6, p["horn_thick"] * p["horn_v"] * 0.5)
    spread = p["horn_spread"]
    L = p["horn_v"]

    for sgn in (-1.0, 1.0):
        # Rooted INSIDE the skull, above and behind the eye, which is where a
        # pedicle is. Started on the surface, a horn parts from the head the
        # first time the head radius varies with the seed.
        root = (p["p_head"]
                + np.array([-r * 0.15, sgn * r * 0.45, r * 0.55])
                - np.array([0.0, 0.0, r * 0.30]))
        if p["horn"] == "spike":
            tip = root + np.array([L * 0.15, sgn * spread * L * 0.5, L])
            tag.capsule(root, tip, base_r, max(0.5, base_r * 0.25), T_HORN)
        elif p["horn"] in ("curve", "sweep", "spiral"):
            _swept_horn(tag, p, root, sgn, L, base_r)
        elif p["horn"] == "palmate":
            _palmate(tag, p, root, sgn, L, base_r)
        else:
            _branched(tag, p, root, sgn, L, base_r)


def _swept_horn(tag: VoxelGrid, p: dict, root, sgn: float, L: float,
                base_r: float) -> None:
    """One tapering horn arcing back over the skull.

    `curve` is a ram's semicircle, `sweep` is an oryx's near-straight spear and
    `spiral` is a kudu's two and a half open turns. They are one centreline with
    `horn_curl` deciding how far it comes round, which is honest -- these really
    are the same shape at three curvatures -- plus a lateral wobble that only
    the spiral uses.
    """
    steps = max(4, int(round(L)))
    curl = p["horn_curl"] * (2.6 if p["horn"] == "curve" else
                             1.0 if p["horn"] == "spiral" else 0.35)
    prev = root
    for k in range(1, steps + 1):
        s = k / steps
        # Angle swings from up-and-back round toward down-and-forward as the
        # curl grows. A straight ray reads as an antenna; the arc is the horn.
        a = math.radians(80.0 - curl * 150.0 * s)
        radial = L / max(steps, 1)
        step = np.array([math.cos(a) * -1.0, 0.0, math.sin(a)]) * radial
        step[1] = sgn * p["horn_spread"] * radial * (0.35 + 0.8 * s)
        if p["horn"] == "spiral":
            # The twist. Small, and it is the whole difference between a
            # gemsbok and a kudu at twenty voxels.
            step[1] += sgn * math.sin(s * math.pi * 2.4) * radial * 0.55
            step[0] += math.cos(s * math.pi * 2.4) * radial * 0.35
        nxt = prev + step
        r0 = base_r * (1.0 - 0.75 * (s - 1.0 / steps))
        r1 = base_r * (1.0 - 0.75 * s)
        tag.capsule(prev, nxt, max(0.5, r0), max(0.5, r1), T_HORN)
        prev = nxt


def _palmate(tag: VoxelGrid, p: dict, root, sgn: float, L: float,
             base_r: float) -> None:
    """A beam and a flat blade with points on its edge. Moose, fallow deer.

    THE SHAPE THAT SURVIVES A COARSE LATTICE. `docs/biomes/README.md` §6 works
    out that a red deer's round main beam is 3-4 cm and its tine tips 1-2 cm, so
    at the 5 cm lattice its rack disappears and the stag becomes a hind -- while
    a moose's or a fallow's palm is a 10-15 cm blade, which is three voxels at
    5 cm and reads unaltered. That is the whole reason this is its own shape.
    """
    beam_end = root + np.array([L * 0.12, sgn * p["horn_spread"] * L * 0.55, L * 0.42])
    tag.capsule(root, beam_end, base_r, max(0.5, base_r * 0.7), T_HORN)

    # The palm: a flat slab out and up from the end of the beam. Drawn as a row
    # of overlapping balls in the palm's own plane, because a slab of arbitrary
    # orientation is a sweep and this is four lines.
    span = L * 0.55
    thick = max(0.5, base_r * 0.55)
    tines = max(2, p["horn_tines"])
    for i in range(tines + 1):
        u = i / tines
        along = beam_end + np.array([L * 0.30 * (u - 0.15),
                                     sgn * span * 0.55 * u,
                                     L * 0.34])
        tag.capsule(beam_end, along, thick * 1.6, thick, T_HORN)
        # A point off the leading edge of the palm.
        if i > 0:
            tip = along + np.array([L * 0.10, sgn * span * 0.16, L * 0.16])
            tag.capsule(along, tip, thick, max(0.5, thick * 0.5), T_HORN)


def _branched(tag: VoxelGrid, p: dict, root, sgn: float, L: float,
              base_r: float) -> None:
    """A beam with round tines off it. A red deer stag, an elk.

    The one shape the survey warns cannot be made to read at life size, and it
    is built anyway because the answer to "how thin is too thin" should be a
    render and not an argument. A spec using it is expected to author the animal
    ABOVE life size or the rack thicker than life, and to say so in its own
    notes -- which is already house practice for four birds and a clownfish, and
    the note is what stops the next person correcting it back.
    """
    steps = max(4, int(round(L * 0.8)))
    tines = max(1, p["horn_tines"])
    pts = []
    prev = root
    for k in range(1, steps + 1):
        s = k / steps
        # Up and back, curving outward: the main beam of a red deer sweeps up,
        # back, and then forward again at the crown.
        step = np.array([L * (-0.16 + 0.34 * s) / steps * steps / steps,
                         sgn * p["horn_spread"] * L * 0.55 / steps,
                         L * 0.95 / steps])
        step[0] = L * (0.34 * s - 0.14) / steps
        nxt = prev + step
        r0 = base_r * (1.0 - 0.6 * (s - 1.0 / steps))
        r1 = base_r * (1.0 - 0.6 * s)
        tag.capsule(prev, nxt, max(0.5, r0), max(0.5, r1), T_HORN)
        pts.append(nxt)
        prev = nxt

    # Tines off the FRONT of the beam at authored heights, which is what a
    # points count is. Growing them toward light -- which is what
    # `skeleton.grow` would do -- puts them wherever the attraction points fell.
    for i in range(tines):
        u = (i + 0.6) / (tines + 0.4)
        j = min(len(pts) - 1, int(u * (len(pts) - 1)))
        at = pts[j]
        # Tines shorten going up the beam, which is what a rack does; the brow
        # tine over the face is the longest.
        tlen = L * (0.42 - 0.22 * u)
        tip = at + np.array([tlen * 0.85, sgn * tlen * 0.22, tlen * 0.55])
        tr = max(0.5, base_r * (0.55 - 0.2 * u))
        tag.capsule(at, tip, tr, 0.5, T_HORN)


# --- tail --------------------------------------------------------------------


def _tail(tag: VoxelGrid, p: dict) -> None:
    """A ROD, and everything a rod can be.

    A bird's tail is a flat fan of feathers (`bird._tail`). Nothing in this
    library made the other kind, and the other kind covers a deer's 15 cm scut,
    a zebra's tasselled whip, a squirrel's full-length plume, a fox's brush, an
    ox's switch and a kangaroo's counterweight -- which is thicker at the base
    than the animal's own neck and, in the bipedal stance, is one of the three
    things it stands on.

    Four numbers: length, base thickness, how hard it tapers, and a terminal
    tuft. A plume is not a fifth thing, it is `tail_taper` near 1 with a thick
    base -- which is worth saying because a `plume` switch was the obvious
    design and it would have been a second way to spell a number that already
    exists.
    """
    if p["tail_v"] < 1.0:
        return
    r0 = p["tail_r"]
    r1 = max(0.5, r0 * p["tail_taper"])
    steps = max(2, int(round(p["tail_v"] / max(1.0, r0 * 0.8))))
    # STARTS INSIDE THE RUMP. Same rule as the neck and the legs, and inset
    # along the TRUNK'S axis rather than back along the tail's own direction: a
    # tail carried straight up leaves the rump at 90 degrees to the trunk, so
    # backing up along its own direction moves the start down through the
    # animal's hindquarters rather than forward into them.
    start = p["p_tail_base"] + p["axis"] * min(3.0, 0.20 * p["trunk_v"])
    prev = start
    for k in range(1, steps + 1):
        s = k / steps
        # `tail_arc` bends the centreline, which is what a squirrel's S over the
        # back and a wolf's low straight brush differ by. Bending the LINE
        # rather than rotating the whole tail is the same distinction the muzzle
        # makes, and for the same reason.
        lift = p["tail_arc"] * p["tail_v"] * (s ** 1.5) * 0.55
        c = start + p["tail_dir"] * (p["tail_v"] * s) + np.array([0.0, 0.0, lift])
        rr0 = r0 + (r1 - r0) * ((k - 1) / steps)
        rr1 = r0 + (r1 - r0) * s
        tag.capsule(_mid(p, prev), _mid(p, c), max(0.5, rr0), max(0.5, rr1), T_TAIL)
        prev = c
    if p["tail_tuft"] > 0.02:
        # SIZED AGAINST THE TAIL'S THICKNESS, not its length. Scaled off the
        # length -- which is the obvious reading of "a tuft is a fraction of the
        # tail" -- a zebra's 27-voxel tail grew an 8-voxel-radius ball on the
        # end, a sphere wider than the animal's own thigh hanging behind it. A
        # tuft is a spray of hair on the end of a thin rod and its size is set
        # by the rod.
        tuft = r0 * (0.9 + 2.6 * p["tail_tuft"])
        if tuft >= 1.0:
            tag.ball(_mid(p, prev), tuft, T_TAIL)
            tag.ball(_mid(p, prev - p["tail_dir"] * tuft * 0.6), tuft * 0.8, T_TAIL)


# --- limbs -------------------------------------------------------------------


def _legs(tag: VoxelGrid, p: dict) -> None:
    """Four limbs, drawn from their joints DOWN TO THE GROUND PLANE.

    THE ONE PLACE THIS GENERATOR IS NOT LIKE THE BIRD. `bird._legs` draws two
    threads a fraction of the body length long and stops wherever they stop,
    which is right for an animal that is either flying or gripping a branch. A
    land animal's feet are on a plane, and the plane is z = 0 here, so every
    chain below ends there rather than at an authored length. Change the
    shoulder height and the foreleg gets longer; there is no second number to
    contradict the first.

    A FORE LEG IS TWO SEGMENTS AND A HIND LEG IS THREE, which is anatomy and not
    detail. A horse's foreleg is nearly a straight column from the elbow down; a
    hind leg zigzags -- hip forward to the stifle, back to the hock, forward
    again along a long foot. `quad.hock` is how pronounced that zigzag is, and
    it runs from a horse's almost-column at 0.15 to a kangaroo's deep Z at 0.9.
    Drawn as two segments both ways, every animal in the library stands like a
    table.
    """
    r = max(0.5, p["leg_r"])
    # A FOOT IS BARELY WIDER THAN THE LEG ABOVE IT. At 1.6x the limb radius the
    # feet came out as spheres half again the thickness of the leg -- four dark
    # balls under a bison, which read as a toy on castors. A hoof or a paw is
    # about the width of the pastern; what varies between species is its LENGTH
    # along the ground, which is the toe run below and not this.
    foot_r = max(r, p["foot"] * r * 1.05)
    gz = p["ground_z"]
    sprawl = p["stance"] == "sprawling"

    for sgn in (-1.0, 1.0):
        # --- fore -----------------------------------------------------------
        a = p["p_fore"] + np.array([0.0, sgn * p["y_off"], 0.0])
        # A LIMB LONGER THAN THE GAP REACHES FORWARD, NOT DOWNWARD. `fore_reach`
        # above 1.0 is a gorilla: its arms are longer than its legs, and what a
        # knuckle-walker does with the difference is place its hands well in
        # front of its shoulders -- not push them into the floor.
        #
        # Taken as a straight multiple of the drop, that is exactly what
        # happened: the gorilla's hands were drawn 0.15 of a limb BELOW the
        # ground plane, they became the lowest voxels of the asset, and its hind
        # feet then measured two voxels in the air. `--stance` reported it as
        # floating hind feet, which is what it looked like from outside and is
        # the opposite end of the animal from the cause.
        reach = p["fore_reach"]
        drop = max(1.0, (a[2] - gz) * min(reach, 1.0))
        over = max(0.0, reach - 1.0) * (a[2] - gz)
        foot_z = a[2] - drop
        if p["stance"] == "bipedal":
            # HELD AT THE CHEST, pointing down and FORWARD. A kangaroo's
            # forelimbs are not a shortened version of a walking limb hanging
            # under the shoulder; they are carried in front of the animal, and
            # drawn straight down they read as a quadruped that has stumbled.
            elbow = a + np.array([drop * 0.35, sgn * r * 0.6, -drop * 0.62])
            foot = np.array([elbow[0] + drop * 0.30, elbow[1],
                             elbow[2] - drop * 0.34])
        elif sprawl:
            # OUT BEFORE DOWN. The elbow is carried well outboard, which is what
            # a sprawl is, and nothing about a limb hanging under the body
            # reaches it.
            elbow = a + np.array([drop * 0.15, sgn * drop * 0.85, -drop * 0.45])
            foot = np.array([a[0] + drop * 0.55, elbow[1] + sgn * drop * 0.35,
                             foot_z + foot_r])
        else:
            elbow = a + np.array([-drop * p["fore_bend"] * 0.45 + over * 0.45,
                                  sgn * r * 0.4, -drop * 0.52])
            foot = np.array([a[0] + drop * p["fore_bend"] * 0.22 + over, a[1],
                             foot_z + foot_r])
        tag.capsule(a, elbow, r * 1.25, r, T_LEG_FORE)
        tag.capsule(elbow, foot, r, r * 0.9, T_LEG_FORE)
        tag.ball(foot, foot_r, T_LEG_FORE)

        # --- hind -----------------------------------------------------------
        b = p["p_hind"] + np.array([0.0, sgn * p["y_off"], 0.0])
        hdrop = max(1.0, b[2] - gz)
        hock = p["hock"]
        if sprawl:
            knee = b + np.array([-hdrop * 0.15, sgn * hdrop * 0.85, -hdrop * 0.45])
            heel = knee + np.array([-hdrop * 0.25, sgn * hdrop * 0.30, -hdrop * 0.40])
            foot = np.array([heel[0] - hdrop * 0.25, heel[1], gz + foot_r])
        else:
            # Stifle forward, hock back, foot forward again. The three
            # displacements are all fractions of `hock`, so one slider takes an
            # elephant's column to a kangaroo's folded Z without any of the
            # three ever crossing another.
            #
            # THE CONSTANT TERMS ARE SMALL ON PURPOSE. Written with a floor of
            # 0.10 the zigzag was a fifth of the limb's length at `hock` = 0,
            # which is where a horse and an elephant sit -- so the two animals
            # whose hind legs are nearly straight columns came out visibly bent,
            # and a gemsbok at 0.32 walked like a grasshopper.
            knee = b + np.array([hdrop * (0.04 + 0.40 * hock), sgn * r * 0.4,
                                 -hdrop * (0.46 - 0.10 * hock)])
            heel = knee + np.array([-hdrop * (0.03 + 0.50 * hock), 0.0,
                                    -hdrop * (0.40 - 0.06 * hock)])
            foot = np.array([heel[0] + hdrop * (0.02 + 0.42 * hock), heel[1],
                             gz + foot_r])
        tag.capsule(b, knee, r * 1.35, r * 1.05, T_LEG_HIND)
        tag.capsule(knee, heel, r * 1.05, r * 0.85, T_LEG_HIND)
        tag.capsule(heel, foot, r * 0.9, r * 0.85, T_LEG_HIND)
        # A HIND FOOT IS LONGER THAN A FORE FOOT on nearly everything and
        # enormously longer on a kangaroo and a hare, where it is a diagnostic
        # feature rather than a detail. Drawn as a run forward from the ankle
        # rather than as a ball, because that is what it is.
        toe = foot + np.array([hdrop * (0.05 + 0.30 * hock), 0.0, 0.0])
        tag.capsule(foot, toe, foot_r, foot_r * 0.8, T_LEG_HIND)


def _caps(tag: VoxelGrid, p: dict) -> None:
    """A ball of the PARENT's material at every joint (owner, 2026-08-14).

    Rotating a rigid limb about a joint opens a wedge of empty space on the
    outside of the bend. A sphere is rotationally symmetric, so it presents the
    same silhouette from every direction and covers that wedge at ANY angle
    rather than at the angles someone anticipated -- which is what an
    overlapping collar does and why a collar fails as soon as the limb swings
    past what it was built for. It is also what the anatomy is: a shoulder IS a
    ball.

    OWNED BY THE PARENT, NOT SHARED. A voxel belonging to two parts has no
    defined answer when both rotate, which is the collar problem wearing a
    different hat. So these are drawn AFTER the trunk and carry the trunk's tag,
    and they overwrite whatever limb voxels were already there.

    SKIPPED BELOW THREE VOXELS OF LIMB THICKNESS, which is the owner's own
    limit and it bites immediately: a 22 cm squirrel at 1 cm has legs two voxels
    thick, and a ball big enough to cover their swing is most of the leg. At
    that size the wedge is one voxel and invisible, so the cap buys nothing and
    costs the animal its legs.
    """
    _limb_caps(tag, p)
    _axial_caps(tag, p)


def _limb_caps(tag: VoxelGrid, p: dict) -> None:
    """The four shoulder and hip balls.

    SEPARATE FROM `_axial_caps` SO EACH GATE CAN BE MEASURED ON ITS OWN. The two
    are gated on different thicknesses -- a moose has limbs two voxels through
    and a neck nine, so it correctly gets no shoulder cap and correctly does get
    one at the withers -- and with both in one function `tools/quadprobe.py
    --caps` could only measure their sum. It duly reported the moose as drawing
    a limb cap it had not drawn, and a meerkat as leaking six voxels from the
    neck cap into the shoulder's measuring box. Splitting the function is the
    fix; the probe suppresses this one alone.
    """
    if 2.0 * p["leg_r"] < 3.0:
        return
    for sgn in (-1.0, 1.0):
        for at in (p["p_fore"], p["p_hind"]):
            c = at + np.array([0.0, sgn * p["y_off"], 0.0])
            tag.ball(c, p["leg_r"] * 1.35, T_BODY)


def _axial_caps(tag: VoxelGrid, p: dict) -> None:
    """The neck base and the tail base, on the same three-voxel floor applied to
    their own thicknesses."""
    neck_r = max(0.5, p["neck_thick"] * p["depth_v"] * 0.5)
    if 2.0 * neck_r >= 3.0:
        tag.ball(_mid(p, p["p_neck_base"]), neck_r * 1.25, T_BODY)
    if p["tail_v"] >= 1.0 and 2.0 * p["tail_r"] >= 3.0:
        tag.ball(_mid(p, p["p_tail_base"]), p["tail_r"] * 1.30, T_BODY)


# --- colour ------------------------------------------------------------------


def _paint(grid: VoxelGrid, tags: np.ndarray, p: dict) -> None:
    """Assign a material to every solid voxel.

    Four layers, applied in order:

      1. THE PART. A mammal is visibly different colours by region and the
         descriptions in `docs/biomes/*.md` are written that way -- "black
         stockings", "white-tipped brush", "pale grey saddle", "black facial
         mask". This is where most of the colour information is and it costs
         nothing, because the parts already exist as tags.
      2. COUNTERSHADING over the trunk and neck. Dark above, pale below, with an
         authored boundary height. On a pronghorn or a gemsbok the boundary is
         the marking.
      3. ONE MARK, on the flank. ONE, WHERE A BIRD GETS THREE, and the reason is
         the same one the fish generator gives: a bird's cap, wing bar and
         breast streaking sit on three disjoint sets of voxels, while every
         mammal marking here competes for the same flank.
      4. THE EYE -- two voxels, and they do more than any other two in the
         asset.
    """
    nx, ny, nz = p["shape"]
    occ = tags != T_NONE
    if not occ.any():
        return
    mat = np.zeros((nx, ny, nz), np.uint8)

    s, h = _stations(p["shape"], p["ycen"], p["p_hip"], p["axis"], p["up"])
    w, up, dn = _trunk_profiles(p)
    trunk_v = max(p["trunk_v"], 1.0)
    tt = np.clip(1.0 - s / trunk_v, 0.0, 1.0)
    tg = np.linspace(0.0, 1.0, len(up))
    span = np.maximum(np.interp(tt, tg, up) + np.interp(tt, tg, dn), 1e-6)
    # Height within the local trunk outline: 0 at the belly line, 1 at the back.
    uz = np.clip((h + np.interp(tt, tg, dn)) / span, 0.0, 1.0)
    uz3 = np.broadcast_to(uz[:, None, :], (nx, ny, nz))
    tt3 = np.broadcast_to(tt[:, None, :], (nx, ny, nz))

    mat[occ] = p["mat_back"]
    trunkish = (tags == T_BODY) | (tags == T_NECK)
    mat[occ & trunkish & (uz3 < p["under"])] = p["mat_belly"]
    mat[occ & (tags == T_HEAD)] = p["mat_head"]
    mat[occ & (tags == T_MANE)] = p["mat_mark"]
    mat[occ & (tags == T_EAR)] = p["mat_head"]
    mat[occ & (tags == T_MUZZLE)] = p["mat_head"]
    mat[occ & (tags == T_HORN)] = p["mat_horn"]
    mat[occ & (tags == T_TAIL)] = p["mat_tail"]
    legs = (tags == T_LEG_FORE) | (tags == T_LEG_HIND)
    mat[occ & legs] = p["mat_leg"]

    _stockings(mat, tags, p, legs)
    _tail_tip(mat, tags, p)
    _cape(mat, tags, p, tt3, occ)
    _mark(mat, tags, p, tt3, uz3, occ)
    _eye(mat, tags, p)
    grid.data[:] = mat


def _stockings(mat: np.ndarray, tags: np.ndarray, p: dict, legs: np.ndarray) -> None:
    """Dark lower legs. A red fox's black stockings, a gemsbok's leg bars, an
    Arabian oryx's black legs, an okapi's white ones.

    Measured from the GROUND UP rather than from the joint down, which is the
    only version that means the same thing on all four legs: the fore leg and
    the hind leg are different lengths on nearly every species here, so a
    fraction of each limb's own length puts the boundary at two different
    heights and the animal looks as though it is standing in a hole.
    """
    if p["stocking"] <= 0.01:
        return
    nz = tags.shape[2]
    zs = np.arange(nz, dtype=np.float64)
    top = p["ground_z"] + p["stocking"] * max(p["stand_v"], 1.0)
    band = np.broadcast_to((zs <= top)[None, None, :], tags.shape)
    mat[legs & band] = p["mat_mark"]


def _tail_tip(mat: np.ndarray, tags: np.ndarray, p: dict) -> None:
    """The last fraction of the tail in the marking colour.

    A red fox's white brush tip, a stoat's black one -- which stays black when
    the rest of the animal turns white in winter and is the only way to tell it
    from a weasel -- a lion's tuft, a zebra's tassel.

    Measured along the tail's OWN axis, from the base outward, rather than by a
    box at the far end of the grid. A tail carried up the back and a tail
    hanging down occupy completely different corners of the same grid, and a
    box would tip the wrong end of one of them.
    """
    if p["tail_tip"] <= 0.01 or p["tail_v"] < 2.0:
        return
    m = tags == T_TAIL
    if not m.any():
        return
    xs, ys, zs = np.nonzero(m)
    base = p["p_tail_base"]
    d = p["tail_dir"]
    along = ((xs + 0.5 - base[0]) * d[0] + (zs + 0.5 - base[2]) * d[2])
    reach = float(along.max()) if along.size else 0.0
    keep = along >= reach - p["tail_tip"] * max(reach, 1.0)
    mat[xs[keep], ys[keep], zs[keep]] = p["mat_mark"]


def _cape(mat: np.ndarray, tags: np.ndarray, p: dict, tt3: np.ndarray,
          occ: np.ndarray) -> None:
    """A dark field over the forequarter. A bison's shaggy cape, a hyena's
    mantle, a wildebeest's dark shoulders, a wolf's saddle.

    Runs from the shoulder BACKWARD by a fraction of the trunk, so `cape` at
    0.45 is "the front 45% of the animal is a different colour". The bison entry
    in `02-grassland.md` describes exactly this -- "shaggy dark cape over the
    shoulders and a bare rear" -- and it is one of the two things separating a
    bison from a wisent.
    """
    if p["cape"] <= 0.01:
        return
    sel = occ & ((tags == T_BODY) | (tags == T_NECK)) & (tt3 <= p["cape"])
    mat[sel] = p["mat_mark"]


def _mark(mat: np.ndarray, tags: np.ndarray, p: dict, tt3: np.ndarray,
          uz3: np.ndarray, occ: np.ndarray) -> None:
    """One marking on the flank.

    `bars` IS THE FISH GENERATOR'S "VERTICAL BARS", REUSED ON PURPOSE, floor
    rules included. `docs/biomes/README.md` §4.8 identified this: a zebra's
    stripes are transverse bands wrapping a cylinder, which is exactly what a
    perch's bars are, and writing a second one would have been a second place
    for the two-on-two-off floor to be forgotten. That floor is the finding that
    a band narrower than two voxels with two voxels of gap merges into a wash --
    so above about five bands on a twenty-voxel body they stop being bands, and
    `mark_count` looks DEAD to a probe that measures it on a small animal when
    it is in fact SATURATED.

    `rosette` and `reticulation` are NOT here and are the honest gap: a
    leopard's rosette is an annulus with a tawny centre and a giraffe's
    reticulation is a Voronoi partition, and neither is any setting of the five
    below. `docs/quadruped-notes.md` records what they would cost.
    """
    kind = p["mark"]
    if kind == "none" or p["mark_strength"] <= 0.02:
        return
    body = occ & ((tags == T_BODY) | (tags == T_NECK))
    if not body.any():
        return
    strength = float(np.clip(p["mark_strength"], 0.0, 1.0))
    nx, ny, nz = tags.shape
    rng = np.random.default_rng(p["salt"])

    if kind == "bars":
        n = max(1, p["mark_count"])
        phase = tt3 * n + p["phase"]
        sel = body & ((phase % 1.0) < np.clip(p["mark_width"] * 1.6, 0.05, 0.9))
    elif kind == "flankstripe":
        # A horizontal band along the flank at the countershading boundary. A
        # gemsbok, an impala, a dorcas gazelle, a springbok.
        half = max(0.04, p["mark_width"] * 0.55)
        sel = body & (np.abs(uz3 - p["under"]) < half)
    elif kind == "saddle":
        # A dark field over the back, boundary set by `mark_width`.
        sel = body & (uz3 > 1.0 - np.clip(p["mark_width"] * 1.8, 0.05, 0.95))
    elif kind in ("spots", "dapple", "blotch"):
        # A field of blobs, at a scale `mark_count` sets. `dapple` is a fallow
        # deer's fine white spotting and `blotch` is a hyena's coarse irregular
        # one; the difference is the scale and the threshold, which is honest --
        # they really are one mechanism -- and it is why they share a branch
        # instead of being three copies of it.
        scale = {"spots": 1.0, "dapple": 1.6, "blotch": 0.45}[kind]
        freq = max(1.0, p["mark_count"] * scale) / max(p["trunk_v"], 1.0)
        xs = np.arange(nx)[:, None, None] * freq * 6.283
        ys = np.arange(ny)[None, :, None] * freq * 6.283
        zs = np.arange(nz)[None, None, :] * freq * 6.283
        field = (np.sin(xs + p["phase"] * 6.0) * np.cos(zs * 1.31 + p["phase"] * 3.0)
                 + np.sin(zs * 0.77 + ys * 0.41) * 0.7)
        cut = np.quantile(field[body], 1.0 - np.clip(p["mark_width"], 0.02, 0.6))
        sel = body & (field > cut)
    else:
        return

    if strength < 1.0:
        # Partial strength thins the mark rather than blending its colour: a
        # voxel has one flat material (ADR-0008), so there is no half-tone to
        # blend to and the only honest reading of "weaker" is "less of it".
        keep = rng.random(sel.shape) < strength
        sel = sel & keep
    mat[sel] = p["mat_mark"]


def _eye(mat: np.ndarray, tags: np.ndarray, p: dict) -> None:
    """Two voxels on the side of the skull.

    Placed against the HEAD's own centre and radius rather than against the
    grid, so it stays put when the neck length or the stance changes. A mammal's
    eye sits forward and high on the skull and off to the side -- unlike a
    bird's, which is nearly on the midline of a much smaller head -- so this
    puts one on each flank of the skull rather than one anywhere.
    """
    if p["eye"] <= 0.0 or p["head_r"] < 1.4:
        return
    r = p["head_r"]
    c = p["p_head"] + p["muz_dir"] * (r * 0.42) + np.array([0.0, 0.0, r * 0.35])
    rad = max(1, int(round(p["eye"])))
    nx, ny, nz = tags.shape
    head = (tags == T_HEAD) | (tags == T_MUZZLE)
    for sgn in (-1, 1):
        cy = p["ycen"] + sgn * r * 0.78
        x0, x1 = int(c[0] - rad), int(c[0] + rad) + 1
        y0, y1 = int(cy - rad), int(cy + rad) + 1
        z0, z1 = int(c[2] - rad), int(c[2] + rad) + 1
        sub = (slice(max(0, x0), min(nx, x1)), slice(max(0, y0), min(ny, y1)),
               slice(max(0, z0), min(nz, z1)))
        m = head[sub]
        if m.any():
            block = mat[sub]
            block[m] = p["mat_eye"]
            mat[sub] = block
