"""Birds: small flying detail entities, drawn to read at 20-90 voxels long.

A fish was one solid whose cross-section changes along a straight axis. A bird
is not. A bird is a JOINTED THING -- a body, a neck, a head, a bill, a tail and
two wings, each at its own angle to the others -- and the angles are most of
what tells one apart from another. A heron and a mallard have nearly the same
body; what separates them at twenty voxels is that the heron's neck is a third
of its length and its bill is a spike and its legs trail out behind.

So this file is a LAYOUT followed by six drawing passes, not a loft. The layout
is worked out first, in floating-point voxel coordinates, as a handful of
points and radii; the grid is then sized from the layout's own bounding box
rather than from a formula; and each part is drawn from one voxel inside the
part it hangs off.

WHAT THE VOXEL BUDGET IS SPENT ON. `docs/bird-shape-research.md` has the
sources; the short version is that birders identify birds by SHAPE AND STANCE
before colour, they have a word for it, and the cues they name are exactly the
ones that survive here:

  1. PROPORTION -- how much of the animal is tail, how much is neck, how big
     the head is against the body. This is the whole of `bird.tail_frac`,
     `bird.neck_frac` and `bird.head_size`, and it is the strongest lever in
     the file.
  2. POSTURE -- a robin sits at 45 degrees, a duck lies flat, a heron stands
     upright. One number, and it changes the silhouette more than any colour.
  3. THE BILL -- four to sixteen voxels, and it says what the bird eats, which
     is most of what says what the bird is.
  4. THE TAIL OUTLINE -- forked, square, rounded, graduated, wedge. Five
     recognisable shapes out of one function.
  5. THE WING -- but only when it is spread. See the pose note below.
  6. ONE MARK PER REGION -- head, wing and body each carry at most one, and
     they are disjoint, which is why a bird may carry three where a fish may
     carry one.

THE POSE PROBLEM, AND WHAT WAS DECIDED. A perched raven is a dark lozenge and a
flying raven is a cross. Those are not one shape at two rotations: the folded
wing is a three-voxel-thick bulge lying along the flank and the spread wing is
a one-voxel plate reaching thirty voxels out, and no rotation turns one into
the other. `tools/buildcheck.py` enforces one connected piece per generation,
so an asset cannot hold both.

`bird.pose` is therefore an authored parameter with two settings, and every
species ships in the pose it is most often seen in. **All twenty are authorable
in either**; the authored value is where the species is usually seen, not a
restriction, and `tools/birdprobe.py --pose` builds all forty and checks each
one is a single piece at 26-connectivity.

A POSE IS A POSTURE, NOT A DIFFERENT BIRD, and that took a fix outside this
file. `pipeline.rng_for` used to seed from `spec.spec_hash`, which the pose is
part of, so species X seed 7 perched and species X seed 7 flying were two
different individuals -- different length, different marking phase. A bird could
not land without changing size on the way down. It now seeds from
`spec.seed_hash`, which normalises `bird.pose` to its default before hashing;
see `spec.SEED_INVARIANT` for why that leaves every other species in the library
on the seed it already had. Nothing in this file changed for it, and nothing in
this file should: the pose still decides the geometry, it just no longer decides
the animal.

MALE AND FEMALE, AND WHY THAT IS TWO MECHANISMS AND NOT ONE. A fish's sexual
difference is nearly all SIZE, so `fish.sex` covered twenty-three species with
three ratios and a square root. A bird's is nearly all COLOUR. The largest
single difference between two animals of one species anywhere in this library
is a mallard drake against a hen -- bottle-green head, white collar, grey body
and yellow bill against uniform mottled brown -- and not one voxel of it is a
proportion. So this file carries `_sex_scale`, which is the fish rule verbatim
and moves two measurements, and `_alt`, which swaps colours and markings
outright. Eight of the twenty species carry a difference and twelve are an
honest null; `docs/bird-dimorphism-research.md` has the sources and the numbers
that decided each rejection, and `tools/birdprobe.py --sex` measures all of it.

The one thing worth reading here rather than there: SEX RESEEDS AND A POSE DOES
NOT, which is the opposite of the paragraph above and is deliberate. A perched
raven and a flying raven are one animal in two postures. A drake and a hen are
two animals -- there is no individual that is "the same mallard, but female" --
so `bird.sex` is NOT in `spec.SEED_INVARIANT` and seed 7 male and seed 7 female
are two different ducks.

THE FOUR THINGS THAT WERE GOT WRONG FIRST, with what they measured as, because
each one is a trap and none of them looked like a bug in a render:

**Every bird wore a slab of head colour across its shoulders.** The neck starts
one voxel INSIDE the body, because that is the only way to guarantee it is
joined; drawn after the body it then painted its own colour over the chunk of
body it is buried in. On the first robin, 44 voxels came back tagged as neck
against 35 tagged as head. The fix is the drawing ORDER in `build`: the parts
that start inside go down first and the body reclaims its own volume, and only
the head, bill and crest are allowed to win over it.

**The folded wing covered five sixths of the animal**, so the whole library came
out the colour of its wings: 119 wing voxels against 88 of body on a robin,
which is not a robin. Two causes, both of them a symmetry that felt right. The
bulge was added to the body's DEPTH as well as its width, which pushed the wing
up over the spine; and the band ran from the belly to the back instead of
covering the middle of the flank. A folded wing is a rind on the SIDE of a bird
with the mantle showing above it.

**Every head came out twice the length its own share asked for** -- a 24-voxel
robin with a six-voxel skull -- because the radius was computed as `0.5 * head *
size * 2.0`, in which the halving and the doubling cancel. It looked like a
style choice. Nothing caught it until `tools/birdprobe.py` measured head
diameter against `bird.head_frac` and found a slope of two.

**`bill_gape` did nothing at all on any bird smaller than a heron.** It was
written as `bill_depth * (0.35 + 0.9 * gape)`, so the two multiplied together
and a shallow bill could not be made wide however far the slider went: a robin's
bill reached 1.02 voxels at gape 1.0 against a threshold of 1.2. The probe
measured it as identical at both ends of its range, which is what a parameter
gated behind another one looks like from outside. Width is measured against the
HEAD now.

**And one design decision that is not a bug but reads like one.** The tail's
plane depends on the pose: a spread tail is a horizontal fan and a folded tail
is a VERTICAL blade. Drawn flat in both, a perched bird's tail is a single line
from the side -- which is the camera a perched bird gets -- and the fork on a
perched swallow does not exist.

**Colour is a per-voxel material, not a texture.** ADR-0008 gives every voxel
face one flat colour from `vxc::kMaterialPalette`, so a wing bar is an
assignment of material ids to voxels. The plumage materials this uses do not
exist in the engine yet -- see `forge/materials.py` and
`docs/bird-colour-proposal.md`.
"""

from __future__ import annotations

import math

import numpy as np

from . import materials
from . import parts
from .grid import VoxelGrid
from .spec import BY_PATH as _BY_PATH
from .spec import (_ALT_BODY_MARKS, _ALT_HEAD_MARKS, _ALT_WING_MARKS,
                   _BIRD_POSES, _BODY_MARKS, _HEAD_MARKS, _SEXES, _TAIL_SHAPES,
                   _WING_MARKS, _WING_SHAPES, get)

# Tail outlines. Named the way a field guide names them, because that is the
# vocabulary a designer authoring a species will be reading from. They are
# choices rather than one "forkiness" number because a GRADUATED tail (central
# feathers longest) and a FORKED tail (outer feathers longest) are opposite
# signs of the same thing but a ROUNDED one is neither -- it is a square tail
# with the corners taken off, and no setting of a fork slider produces it.
TAIL_SHAPES = ("square", "rounded", "graduated", "wedge", "notched",
               "forked", "pointed")

# Wing planforms, after Savile's scheme. These differ in how the chord is
# distributed along the span and in what happens at the tip, which no single
# slider says: an elliptical wing is broad to the last few centimetres and then
# rounds off, a high-speed wing tapers to a point from halfway out, a
# high-aspect-ratio wing is nearly a constant-chord plank, and a slotted one is
# broad and ends in separated fingers.
WING_SHAPES = ("elliptical", "pointed", "soaring", "slotted")

# See the pose note in the module docstring. Two, not three: "gliding" would be
# `flying` with a different sweep and dihedral, and a choice that duplicates a
# slider position is a choice that silently ignores it.
POSES = ("perched", "flying")

# Markings, by REGION. Three regions, at most one mark each.
HEAD_MARKS = ("none", "cap", "mask", "supercilium", "throat", "collar")
WING_MARKS = ("none", "bar", "doublebar", "panel", "tip")
BODY_MARKS = ("none", "barred", "streaked", "speckled", "breastband")

# Which sex is being drawn. Same three words the fish use, and the same import
# check, because the two files answer to one parameter table.
SEXES = ("unsexed", "female", "male")

# Which sex the authored colours belong to. See `_alt` and `bird.sex_plumage`.
PLUMAGE_AUTHORED = ("same", "male", "female")

# The seven colour slots an author may override for the other sex, and the
# three markings. Listed once, here, and read by `_alt_mat` and `_alt_mark`
# rather than typed out at each site -- fourteen literal strings spread over a
# function is how `materials.bird_head` gets overridden and
# `materials.bird_head_mark` quietly does not.
ALT_SLOTS = ("back", "belly", "head", "wing", "mark", "head_mark", "bill")
ALT_MARKS = ("head_mark", "wing_mark", "body_mark")

# The parameter table offers these as choices and this file implements them.
# Checked at import rather than trusted: a shape name that falls through to a
# default LOOKS like a shape that works, which is how `spire` and `ovoid`
# crowns rendered as spheres for as long as they did.
for _mine, _theirs, _what in (
    (TAIL_SHAPES, _TAIL_SHAPES, "TAIL_SHAPES"),
    (WING_SHAPES, _WING_SHAPES, "WING_SHAPES"),
    (POSES, _BIRD_POSES, "POSES"),
    (HEAD_MARKS, _HEAD_MARKS, "HEAD_MARKS"),
    (WING_MARKS, _WING_MARKS, "WING_MARKS"),
    (BODY_MARKS, _BODY_MARKS, "BODY_MARKS"),
    (SEXES, _SEXES, "SEXES"),
    # And the three `alt` lists really are the three above with one extra
    # entry. Checked because they are written as a derived tuple in spec.py and
    # as a literal here, and a sentinel that exists on one side only would show
    # up as `bird.sex_alt_body_mark` silently refusing `breastband`.
    (("same",) + HEAD_MARKS, _ALT_HEAD_MARKS, "ALT_HEAD_MARKS"),
    (("same",) + WING_MARKS, _ALT_WING_MARKS, "ALT_WING_MARKS"),
    (("same",) + BODY_MARKS, _ALT_BODY_MARKS, "ALT_BODY_MARKS"),
):
    assert set(_mine) == set(_theirs), (
        f"bird.py {_what} and spec.py disagree: {set(_mine) ^ set(_theirs)}")
del _mine, _theirs, _what

# AND THE SLOT LISTS ARE CHECKED AGAINST THE PARAMETER TABLE ITSELF, not against
# another list of the same strings. `ALT_SLOTS` decides which colours `_alt_mat`
# will swap and which the probe will exercise, so a slot added to spec.py and
# forgotten here would be a row that exists in the browser, sits at its default
# forever, and appears in no table anywhere -- a feature nobody can tell is not
# working. Reading the parameter table back is the only check that cannot itself
# go stale.
for _slot in ALT_SLOTS:
    assert f"materials.bird_alt_{_slot}" in _BY_PATH, (
        f"bird.py ALT_SLOTS names {_slot!r} and spec.py has no "
        f"materials.bird_alt_{_slot}")
for _m in ALT_MARKS:
    assert f"bird.sex_alt_{_m}" in _BY_PATH, (
        f"bird.py ALT_MARKS names {_m!r} and spec.py has no bird.sex_alt_{_m}")
_declared = {p.split("materials.bird_alt_")[1] for p in _BY_PATH
             if p.startswith("materials.bird_alt_")}
assert _declared == set(ALT_SLOTS), (
    f"spec.py declares alt colour slots bird.py will never swap: "
    f"{_declared ^ set(ALT_SLOTS)}")
del _slot, _m, _declared

# Part tags. Drawn into a scratch grid so that the paint pass can tell a wing
# from a body without a second geometry pass, exactly as `fish._fins` returns
# a `kind` array alongside its mask.
T_NONE, T_BODY, T_NECK, T_HEAD, T_BILL = 0, 1, 2, 3, 4
T_TAIL, T_WING, T_LEG, T_CREST = 5, 6, 7, 8

# The same tags, named, for anything downstream that has to move a part rather
# than paint one. THIS IS WHY THE TAG GRID LEAVES THIS FILE.
#
# Animals are rigid-part animated and ship in ONE pose (owner, 2026-08-14 --
# see docs/animal-rigging-decision.md), which means the runtime rotates a wing
# about a shoulder rather than swapping to a differently-baked bird. It can
# only do that if the asset says which voxels are the wing, and until now this
# grid was scratch: built because the paint pass needs to tell a wing from a
# body, thrown away three lines later.
# The private painting tags above map into the shared rigging vocabulary in
# `forge/parts.py` on the way out. They stay private because they answer a
# different question -- "what colour is this voxel" -- and because a baked
# asset that says "part 6" has to mean the same thing to every reader, which a
# number chosen inside this file cannot.
_TO_SHARED = {
    T_NONE: parts.P_NONE, T_BODY: parts.P_BODY, T_NECK: parts.P_NECK,
    T_HEAD: parts.P_HEAD, T_BILL: parts.P_JAW, T_TAIL: parts.P_TAIL,
    T_WING: parts.P_WING, T_LEG: parts.P_LEG, T_CREST: parts.P_CREST,
}


def build(spec: dict, rng: np.random.Generator, voxel_m: float,
          out: dict | None = None) -> VoxelGrid:
    """One bird, bill at +x, back at +z, seen broadside from -y.

    `out`, when given, collects the part tags on the way past -- `out["tags"]`
    is a uint8 array parallel to the returned grid, valued by the `T_*`
    constants, and `out["part_names"]` names them. An out-dict rather than a
    second return value so no existing caller changes, and rather than a
    separate entry point so the tags cannot drift from the geometry they
    describe: they are the same pass.

    Same convention as a fish, and for the same reason: this is the first thing
    in the library after a fish that has a FRONT, and a preview camera that has
    to search for the side view every time is a preview camera nobody uses.

    A flying bird is drawn with its wings out along +/-y, so the broadside view
    shows them edge-on -- which is why `render.camera_for` sends a flying bird
    to the isometric instead. That is a camera decision, not a geometry one.
    """
    p = _params(spec, rng, voxel_m)
    tag = VoxelGrid(p["shape"], (0, 0, 0), voxel_m)
    body = _body(p)

    # DRAWING ORDER IS A COLOUR DECISION, and getting it wrong is invisible in
    # the geometry and glaring in the render. The neck and the legs both START
    # ONE VOXEL INSIDE THE BODY, because that is the only way to guarantee they
    # are joined to it. Drawn after the body they therefore paint their own
    # colour over a chunk of the body they are buried in: on the first robin,
    # 44 voxels came back tagged as neck against 35 tagged as head, and every
    # bird in the library wore a slab of head colour across its shoulders.
    #
    # So the buried parts go down first and the body reclaims its own volume.
    # The head, bill and crest come after and win, because a short-necked bird
    # -- an owl, a kingfisher -- carries its head INSIDE the body's outline and
    # the head colour has to survive that.
    _neck(tag, p)
    _legs(tag, p)
    tag.data[body] = T_BODY
    _head_bill(tag, p)
    _tail(tag, p)
    _wings(tag, p, body)

    grid = VoxelGrid(p["shape"], (0, 0, 0), voxel_m)
    _paint(grid, tag.data, p, body)
    if out is not None:
        out["tags"] = parts.to_shared(tag.data, _TO_SHARED)
    return grid


# --- parameters and layout --------------------------------------------------


def _sex_scale(spec: dict, ratio_path: str) -> float:
    """Multiplier on one measurement for the sex this bird is being drawn as.

    IDENTICAL IN BEHAVIOUR TO `fish._sex_scale`, and deliberately not shared
    with it: the two files read different parameter paths and the only thing a
    common helper would save is four lines, against the cost that a change made
    for a whale silently reshapes every bird in the library. What is shared is
    the RULE, and it is stated in both places because it is the part that is
    easy to get wrong.

    THE AUTHORED NUMBER IS THE SPECIES AVERAGE AND THE RATIO IS SPLIT BOTH
    WAYS. Each `bird.sex_*` row is a male-to-female ratio, so the male gets
    `sqrt(r)` and the female `1/sqrt(r)`: male divided by female is exactly `r`
    whatever the authored value is, and `unsexed` is the geometric mean of the
    two. Neither sex is the default, which is the whole reason for the square
    root -- the obvious version, "the authored bird is the female and the male
    is scaled up", makes every unsexed spec in the library silently female.

    UNDER 1 IS NOT AN EDGE CASE HERE. Four of the twenty species author a ratio
    below one because the female is the larger bird: reversed sexual size
    dimorphism is the rule in raptors and owls, and the golden eagle, common
    buzzard, common kestrel and tawny owl all carry it. The fish library had
    exactly one species that way round.

    A species with no measured difference leaves the ratio at 1.0, and then
    this returns 1.0 for all three sexes and the choice genuinely changes no
    geometry. That is a real answer and not a broken one -- `tools/birdprobe.py
    --sex` prints the per-species movement in voxels so that "changes nothing"
    is a measurement rather than an assumption.
    """
    sex = str(get(spec, "bird.sex"))
    if sex == "unsexed":
        return 1.0
    r = max(float(get(spec, ratio_path)), 1e-3)
    return math.sqrt(r) if sex == "male" else 1.0 / math.sqrt(r)


def _alt(spec: dict) -> bool:
    """Is this bird wearing the OTHER sex's plumage?

    THE MECHANISM THE FISH DID NOT NEED, AND THE ONE THING IN THIS FILE THAT
    CANNOT BE A RATIO. A mallard drake against a hen is the largest single
    difference in this library and none of it is a proportion: bottle-green
    head, white collar, grey body against uniform mottled brown. Halfway
    between green and brown is not an unsexed mallard, it is a colour no duck
    has ever worn.

    So the swap is CATEGORICAL. `bird.sex_plumage` names which sex the authored
    colours are -- `same` for a monomorphic species, which is twelve of the
    twenty here -- and asking for the other sex substitutes whichever of the
    ten `alt` rows the author filled in. Three consequences, all of them
    intended and all of them checked by `tools/birdprobe.py --sex`:

      * UNSEXED DRAWS THE AUTHORED PLUMAGE, so on a dimorphic species unsexed
        and one named sex are the same bird. That is stated rather than hidden;
        the probe prints which species it is true of and how many voxels apart
        the two sexes are.
      * ASKING FOR THE SEX THE SPEC IS ALREADY AUTHORED AS CHANGES NOTHING, by
        construction, so `sex_plumage=male` plus `sex=male` is a no-op and only
        `sex=female` swaps.
      * A MONOMORPHIC SPECIES IGNORES THE `alt` ROWS ENTIRELY. Not "applies
        them and finds them empty" -- ignores them, so an `alt` row left behind
        by a species that was later measured as monomorphic cannot come back to
        life the day somebody sets the sex.
    """
    authored = str(get(spec, "bird.sex_plumage"))
    if authored == "same":
        return False
    sex = str(get(spec, "bird.sex"))
    return sex in ("male", "female") and sex != authored


def _alt_mat(spec: dict, slot: str, alt: bool) -> int:
    """The material id for one colour slot, after the plumage swap.

    `same` is consumed here and never reaches `materials.resolve`, which raises
    on a name it does not know. That ordering is the point: a typo in an `alt`
    row fails loudly at build time instead of coming out as a silently
    substituted colour hundreds of assets later, which is the failure mode
    `materials.resolve`'s own docstring was written against.
    """
    if alt:
        name = str(get(spec, f"materials.bird_alt_{slot}"))
        if name != "same":
            return materials.resolve(name)
    return materials.resolve(get(spec, f"materials.bird_{slot}"))


def _alt_mark(spec: dict, which: str, alt: bool) -> str:
    """The marking for one region, after the plumage swap.

    A SEPARATE MECHANISM FROM THE COLOURS BECAUSE SOME DIFFERENCES ARE NOT
    COLOURS. A female kestrel is BARRED across the back where the male is
    SPOTTED, in the same dark brown; swapping her marking material would give
    her the male's spots in a new colour, which is a bird that does not exist.
    A female great spotted woodpecker is the opposite case and needs no colour
    at all -- she simply has no red on her head, so her row is `none`.
    """
    if alt:
        pick = str(get(spec, f"bird.sex_alt_{which}"))
        if pick != "same":
            return pick
    return str(get(spec, f"bird.{which}"))


def _params(spec: dict, rng: np.random.Generator, voxel_m: float) -> dict:
    """Everything the drawing code needs, in VOXELS, with this individual's
    variation already applied and the layout already solved.

    Variation is folded in HERE rather than in `spec.realize`, for the same
    reason it is for a fish: that function varies a tree's height, crown radius,
    trunk radius and foliage shells, and a bird has none of those. Left to it,
    every seed of a species would be the same bird with the marking phase
    shuffled -- which is exactly the failure the `variation` group was added to
    fix for trees, and it is far more obvious on a flock than on a forest.
    """
    amount = float(get(spec, "variation.amount"))

    def u() -> float:
        # The same draw `spec.realize` uses: pushed away from the middle, so a
        # flock of eight does not pile up on the species average.
        t = float(rng.random()) * 2.0 - 1.0
        return abs(t) ** 0.6 if t >= 0.0 else -(abs(t) ** 0.6)

    def vary(value: float, slider: str, scale: float = 1.0) -> float:
        return value * (1.0 + amount * float(get(spec, slider)) * scale * u())

    v = float(voxel_m)
    # Sex is applied BEFORE the individual variation draw, so a male and a
    # female of one species are two draws around two different means rather
    # than one draw scaled -- which is what "two birds" has to mean if the word
    # is doing any work. See `_sex_scale` and `bird.sex`'s help text.
    length_m = max(vary(float(get(spec, "bird.length_m"))
                        * _sex_scale(spec, "bird.sex_length"),
                        "variation.height"),
                   v * 8.0)
    length_v = max(8.0, length_m / v)

    # --- how the length is divided up ---------------------------------------
    #
    # NORMALISED, so `bird.length_m` really is the length. The five shares are
    # authored independently -- a designer reading a field guide knows that a
    # magpie is half tail and a heron is a third neck, and should be able to say
    # so without arithmetic -- and then they are scaled to sum to one. Without
    # the normalisation, raising the tail share made the whole bird longer and
    # the length slider was a suggestion.
    shares = {
        "bill": float(get(spec, "bird.bill_frac")),
        "head": float(get(spec, "bird.head_frac")),
        "neck": min(0.95, max(0.0, vary(float(get(spec, "bird.neck_frac")),
                                        "variation.proportion", 0.6))),
        "body": float(get(spec, "bird.body_frac")),
        "tail": min(0.95, max(0.02, vary(float(get(spec, "bird.tail_frac")),
                                         "variation.proportion", 0.5))),
    }
    total = max(sum(shares.values()), 1e-6)

    # --- and what the sex does to the tail, which the normalisation fights ---
    #
    # A BARN SWALLOW'S STREAMERS MAKE HIM LONGER; THEY DO NOT MAKE HIS HEAD
    # SMALLER. That is what the line below is careful about, and it is careful
    # by NOT recomputing `total`. Scaled into the normalisation the ordinary
    # way, a tail ratio of 1.20 on a swallow authored at tail 0.42 divides
    # every other share by a total 8% larger, so the head, the neck, the bill
    # and the body all shrink to pay for the streamers -- sub-voxel on each of
    # them, which is exactly the kind of wrong that never shows up in a render
    # and does show up as a male whose bill lost a voxel.
    #
    # Dividing by the total taken BEFORE the ratio leaves every unchanged share
    # exactly where it was and moves the tail by the ratio and nothing else.
    # The bird then genuinely comes out longer than `bird.length_m`, which is
    # correct and is what a streamer is: published lengths for this species are
    # 17-19 cm without the outer feathers and up to 21 cm with them.
    #
    # `length_v` is deliberately left alone, because the wingspan and the leg
    # length are fractions OF it. Scaling it would have given the male swallow
    # 8% more wing as well, and his wing chord differs from hers by about one
    # percent. `tools/birdprobe.py --sex` measures the tail in columns AND the
    # bill in columns AND the wing reach, because a compensation that overshoots
    # looks identical to one that works if you only measure the part that was
    # meant to move.
    shares["tail"] *= _sex_scale(spec, "bird.sex_tail")

    seg = {k: length_v * x / total for k, x in shares.items()}

    pose = str(get(spec, "bird.pose"))
    posture = math.radians(vary(float(get(spec, "bird.posture_deg")),
                                "variation.shape", 0.5))
    # A flying bird is not standing on anything, so its body lies along its
    # line of travel whatever it perches at. Halving rather than zeroing: a
    # climbing bird still carries its body nose-up, and a flat-zero read as a
    # plank on every species.
    if pose == "flying":
        posture *= 0.35

    depth_ratio = vary(float(get(spec, "bird.body_depth")), "variation.shape")
    body_v = seg["body"]
    depth_v = max(2.0, body_v * depth_ratio)
    width_v = max(1.5, depth_v * float(get(spec, "bird.body_width")))

    # --- layout, in float voxel coordinates about an arbitrary origin --------
    cp, sp = math.cos(posture), math.sin(posture)
    axis = np.array([cp, 0.0, sp])            # body axis, forward and up
    up = np.array([-sp, 0.0, cp])             # body "up", perpendicular to it

    p_rump = np.zeros(3)                      # rear of the body / tail base
    p_shoulder = p_rump + body_v * axis

    neck_a = posture + math.radians(float(get(spec, "bird.neck_up_deg")))
    neck_dir = np.array([math.cos(neck_a), 0.0, math.sin(neck_a)])
    # The head's SHARE gives its length; the radius is half of that. Written
    # first as `0.5 * head * size * 2.0`, in which the halving and the doubling
    # cancel, so every bird came out with a head twice the length its own share
    # asked for -- a 24-voxel robin with a six-voxel skull. It looked like a
    # style choice, which is exactly why nothing caught it until the probe
    # measured head diameter against `bird.head_frac` and found a slope of two.
    head_r = max(1.0, 0.5 * seg["head"] * float(get(spec, "bird.head_size")))
    p_head = p_shoulder + (seg["neck"] + head_r * 0.6) * neck_dir

    # The bill leaves the head pointing FORWARD, not along the neck. A robin
    # sits at 45 degrees and its bill is level; making the bill follow the
    # posture put every perched bird's bill in the air like a singing canary.
    bill_a = posture * 0.25
    bill_dir = np.array([math.cos(bill_a), 0.0, math.sin(bill_a)])
    p_bill_base = p_head + head_r * 0.75 * bill_dir

    # The tail continues the body line backward, but flatter: a perched bird's
    # tail hangs toward the vertical of whatever it is standing on rather than
    # carrying on out of its back at the posture angle.
    tail_a = posture * float(get(spec, "bird.tail_droop"))
    tail_dir = np.array([-math.cos(tail_a), 0.0, -math.sin(tail_a)])

    leg_v = float(get(spec, "bird.leg_len")) * length_v if pose == "perched" else 0.0
    wing_half = 0.5 * float(get(spec, "bird.wing_span")) * length_v
    wing_thick = max(1, int(round(float(get(spec, "bird.wing_thick")))))
    sweep = float(get(spec, "bird.wing_sweep"))
    dihedral = float(get(spec, "bird.wing_dihedral"))

    # --- bounding box, from what will actually be drawn ---------------------
    #
    # Sized against the LAYOUT, not against a round number. A crest or a
    # trailing leg that runs off the edge of the grid is clipped silently by
    # `VoxelGrid._write`, which is this project's signature failure: the
    # feature runs, reports success, and half of it is not there.
    pts = [
        p_rump + depth_v * up, p_rump - depth_v * up,
        p_shoulder + depth_v * up, p_shoulder - depth_v * up,
        p_head + head_r * 2.0, p_head - head_r * 2.0,
        p_bill_base + seg["bill"] * bill_dir + np.array([0.0, 0.0, -seg["bill"]]),
        p_bill_base + seg["bill"] * bill_dir + np.array([0.0, 0.0, seg["bill"]]),
        p_rump + seg["tail"] * tail_dir,
        p_rump + seg["tail"] * tail_dir + np.array([0.0, 0.0, seg["tail"] * 0.6]),
        p_rump + seg["tail"] * tail_dir - np.array([0.0, 0.0, seg["tail"] * 0.6]),
        p_head + np.array([0.0, 0.0, head_r + float(get(spec, "bird.crest")) * depth_v]),
        p_rump - np.array([0.0, 0.0, leg_v + depth_v]),
    ]
    if pose == "flying":
        tip = p_shoulder + np.array([-wing_half * sweep, 0.0, wing_half * dihedral])
        pts += [tip + np.array([0.0, 0.0, wing_half * 0.1]),
                tip - np.array([0.0, 0.0, wing_half * 0.1])]
    arr = np.array(pts, dtype=np.float64)
    lo = arr.min(axis=0)
    hi = arr.max(axis=0)

    margin = 3.0
    half_y = margin + max(width_v, wing_half if pose == "flying" else 0.0,
                          0.5 * float(get(spec, "bird.tail_width")) * seg["tail"] + 2.0)
    nx = int(math.ceil(hi[0] - lo[0])) + 2 * int(margin)
    ny = int(math.ceil(2.0 * half_y))
    nz = int(math.ceil(hi[2] - lo[2])) + 2 * int(margin)

    off = np.array([margin - lo[0], half_y, margin - lo[2]])

    # Read once and passed to the ten slots below. Read per slot instead, this
    # is ten calls that must all agree; read once, a species either wears the
    # other sex's plumage or it does not.
    alt = _alt(spec)

    return {
        "voxel_m": v,
        "shape": (max(nx, 6), max(ny, 6), max(nz, 6)),
        "pose": pose,
        "length_v": length_v,
        "body_v": body_v,
        "tail_v": seg["tail"],
        "neck_v": seg["neck"],
        "bill_v": seg["bill"],
        "head_r": head_r,
        "depth_v": depth_v,
        "width_v": width_v,
        "axis": axis,
        "up": up,
        "posture": posture,
        "p_rump": p_rump + off,
        "p_shoulder": p_shoulder + off,
        "p_head": p_head + off,
        "p_bill_base": p_bill_base + off,
        "neck_dir": neck_dir,
        "bill_dir": bill_dir,
        "tail_dir": tail_dir,
        # The layout is built about y = 0 and then shifted by `off`, so every
        # point already sits on this plane. Kept as its own entry because the
        # drawing code asks for it constantly.
        "ycen": float(half_y),
        "breast": float(get(spec, "bird.breast")),
        "rump": float(get(spec, "bird.rump")),
        "chest_at": float(get(spec, "bird.chest_at")),
        "fullness": float(get(spec, "bird.fullness")),
        "belly": float(get(spec, "bird.belly")),
        "section": float(get(spec, "bird.section")),
        "neck_thick": float(get(spec, "bird.neck_thick")),
        "crest": float(get(spec, "bird.crest")),
        "bill_depth": float(get(spec, "bird.bill_depth")),
        "bill_curve": float(get(spec, "bird.bill_curve")),
        "bill_hook": float(get(spec, "bird.bill_hook")),
        "bill_gape": float(get(spec, "bird.bill_gape")),
        "tail_shape": str(get(spec, "bird.tail_shape")),
        "tail_width": float(get(spec, "bird.tail_width")),
        "tail_fork": float(get(spec, "bird.tail_fork")),
        "tail_thick": max(1, int(round(float(get(spec, "bird.tail_thick"))))),
        "wing_shape": str(get(spec, "bird.wing_shape")),
        "wing_half": wing_half,
        "wing_aspect": float(get(spec, "bird.wing_aspect")),
        "wing_sweep": sweep,
        "wing_dihedral": dihedral,
        "wing_slots": int(get(spec, "bird.wing_slots")),
        "wing_thick": wing_thick,
        "wing_fold": float(get(spec, "bird.wing_fold")),
        "leg_v": leg_v,
        "leg_thick": float(get(spec, "bird.leg_thick")),
        "eye": float(get(spec, "bird.eye")),
        "upperparts": float(get(spec, "bird.upperparts")),
        # THE PLUMAGE SWAP HAPPENS HERE AND NOWHERE ELSE. Every colour and
        # every marking the drawing passes read comes out of this dict, so one
        # gate covers all ten of them and there is no second site to forget --
        # which matters, because `_paint` reads the marks and the marking
        # colours in four different functions and `_eye` picks its contrast
        # partner out of the palette by scanning it.
        #
        # `alt` is False for the twelve monomorphic species and for `unsexed`
        # on all twenty, and then every line below reads exactly what it read
        # before this parameter existed.
        "sex": str(get(spec, "bird.sex")),
        "alt_plumage": alt,
        "head_mark": _alt_mark(spec, "head_mark", alt),
        "wing_mark": _alt_mark(spec, "wing_mark", alt),
        "body_mark": _alt_mark(spec, "body_mark", alt),
        "mark_count": int(get(spec, "bird.mark_count")),
        "mark_width": float(get(spec, "bird.mark_width")),
        "mark_strength": float(get(spec, "bird.mark_strength")),
        "phase": float(rng.random()),
        "salt": int(rng.integers(1 << 30)),
        "mat_back": _alt_mat(spec, "back", alt),
        "mat_belly": _alt_mat(spec, "belly", alt),
        "mat_head": _alt_mat(spec, "head", alt),
        "mat_wing": _alt_mat(spec, "wing", alt),
        "mat_mark": _alt_mat(spec, "mark", alt),
        "mat_head_mark": _alt_mat(spec, "head_mark", alt),
        "mat_bill": _alt_mat(spec, "bill", alt),
        # NO `alt` ON THE EYE, and that is a measured decision rather than an
        # omission -- see the note on the `materials.bird_alt_*` block in
        # spec.py. Two voxels cannot carry an iris.
        "mat_eye": materials.resolve(get(spec, "materials.bird_eye")),
    }


# --- the body ---------------------------------------------------------------


def _depth_profile(t: np.ndarray, p: dict) -> np.ndarray:
    """Body depth along the bird, as a fraction of its maximum. t=0 is the
    breast, t=1 is the rump.

    The same three-number form the fish uses, and for the same reason: it is
    the only one where "how deep" and "where the deepest point is" are
    independent, so tuning a species does not mean chasing one slider with
    another.

        base(t) = breast + (rump - breast) * t
        bump(t) = (t/q)^(s*q) * ((1-t)/(1-q))^(s*(1-q))     peaks at t = q
        D(t)    = base + (1 - base) * bump

    WHERE IT DIFFERS FROM A FISH, and it is not a small difference. A fish's
    front end is a POINT -- `fish.snout` runs down to 0.05 -- because a fish
    swims into water and a sharp nose is worth having. A bird's front end is a
    BREAST: blunt, and typically 0.55 to 0.80 of the maximum depth, because the
    flight muscle is the deepest thing on the animal and it sits immediately
    behind the neck. Authoring the first six species with a fish's snout values
    gave six birds that read as fish standing on end, which is a sentence worth
    keeping because it was exactly what the renders showed.
    """
    q = min(max(p["chest_at"], 0.05), 0.95)
    s = max(p["fullness"], 0.2)
    base = p["breast"] + (p["rump"] - p["breast"]) * t
    tt = np.clip(t, 1e-6, 1.0 - 1e-6)
    bump = (tt / q) ** (s * q) * ((1.0 - tt) / (1.0 - q)) ** (s * (1.0 - q))
    return np.clip(base + (1.0 - base) * bump, 0.02, 1.0)


# How hard the body flattens toward the tail. NOT A SLIDER, and deliberately.
# A fish exposes this because a fast fish's caudal peduncle is a blade and a
# slow one's is a tube -- a factor of three between species. A bird's body is
# nearly a solid of revolution over its whole length: the widest and deepest
# points are within a couple of centimetres of each other on everything from a
# wren to a swan, because a bird's body is a rigid box carrying flight muscle
# rather than a swimming machine. A slider whose only correct setting is one
# value is a slider that can be set wrong.
_WIDTH_FOLLOW = 0.75


def _stations(p: dict):
    """Per-column body geometry in the body's own frame.

    Returns (s, h) maps of shape (nx, nz) -- distance along the body axis and
    height above it -- plus the half-width and half-depth profiles sampled on a
    fine parameter grid ready for `np.interp`.

    The maps are (nx, nz) and NOT (nx, ny, nz) on purpose. The body axis lies in
    the x-z plane, so neither s nor h depends on y at all, and a bird with a
    2.4 m wingspan is 240 voxels across: building three full meshgrids of that
    would be 50 MB of float64 to compute a quantity that is constant along one
    of the axes.
    """
    nx, ny, nz = p["shape"]
    xs = (np.arange(nx, dtype=np.float64) + 0.5) - p["p_rump"][0]
    zs = (np.arange(nz, dtype=np.float64) + 0.5) - p["p_rump"][2]
    ax, az = p["axis"][0], p["axis"][2]
    ux, uz = p["up"][0], p["up"][2]
    s = xs[:, None] * ax + zs[None, :] * az
    h = xs[:, None] * ux + zs[None, :] * uz
    return s, h


def _profiles(p: dict, n: int = 128):
    """(t grid, half-width, half-depth above axis, half-depth below axis)."""
    t = np.linspace(0.0, 1.0, n)
    d = _depth_profile(t, p)
    w = np.clip(d, 1e-4, 1.0) ** _WIDTH_FOLLOW
    # Half a voxel is the floor everywhere. Below it a cross-section contains no
    # cell centre and the body comes apart; the axis run stamped below is the
    # belt to this pair of braces.
    dtop = np.maximum(p["depth_v"] * d * (1.0 - p["belly"]), 0.5)
    dbot = np.maximum(p["depth_v"] * d * p["belly"], 0.5)
    half = np.maximum(p["width_v"] * 0.5 * w, 0.5)
    return t, half, dtop, dbot


def _body(p: dict) -> np.ndarray:
    """The torso, as a boolean grid.

    A superellipse cross-section in the body's own frame, exactly as a fish has,
    swept along an axis that is TILTED rather than horizontal. Everything else
    in this file hangs off the two ends of that axis.
    """
    nx, ny, nz = p["shape"]
    s, h = _stations(p)
    tg, half, dtop, dbot = _profiles(p)

    body_v = max(p["body_v"], 1.0)
    # t runs 0 at the breast (the forward end) to 1 at the rump.
    tt = np.clip(1.0 - s / body_v, 0.0, 1.0)
    valid = (s >= -0.5) & (s <= body_v + 0.5)

    halfmap = np.interp(tt, tg, half)
    dmap = np.where(h >= 0.0, np.interp(tt, tg, dtop), np.interp(tt, tg, dbot))

    y = np.abs((np.arange(ny, dtype=np.float64) + 0.5) - p["ycen"])
    n = max(p["section"], 0.8)
    lhs = (y[None, :, None] / halfmap[:, None, :]) ** n
    rhs = (np.abs(h) / dmap)[:, None, :] ** n
    occ = (lhs + rhs) <= 1.0 + 1e-9
    occ &= valid[:, None, :]

    # THE AXIS GOES DOWN FIRST, as an unbroken run from rump to shoulder. Same
    # rule as `grid.capsule` and the same rule the fish needed: a shape thinner
    # than one voxel exists as its centreline or it does not exist at all. A
    # slim-rumped bird (a swallow, a swift) lost the last three columns of its
    # body without this and shipped as a torso plus a floating tail.
    tmp = VoxelGrid(p["shape"], (0, 0, 0), p["voxel_m"])
    tmp.line(p["p_rump"] + np.array([0.0, p["ycen"] - p["p_rump"][1], 0.0]),
             p["p_shoulder"] + np.array([0.0, p["ycen"] - p["p_shoulder"][1], 0.0]),
             1)
    occ |= tmp.data != 0
    return occ


# --- neck, head, bill, crest ------------------------------------------------


def _at(p: dict, point, dy: float = 0.0) -> np.ndarray:
    """A layout point, forced onto the mid-plane (or `dy` voxels off it)."""
    return np.array([point[0], p["ycen"] + dy, point[2]])


def _neck(tag: VoxelGrid, p: dict) -> None:
    """Shoulder to the middle of the head.

    Drawn with `VoxelGrid.capsule`, which lays a face-connected centreline down
    BEFORE it thickens anything. That is the only reason a goldcrest's neck --
    which is 0.4 voxels thick at the 1 cm lattice -- is a neck rather than a
    gap between a body and a floating head.

    It starts one voxel INSIDE the body rather than at the shoulder, and ends
    at the head's CENTRE rather than at its surface. Both joins are overlaps by
    construction; measuring a join afterwards and hoping is what put heads on
    the floor.
    """
    neck_r = max(0.5, p["neck_thick"] * p["depth_v"] * 0.5)
    start = p["p_shoulder"] - p["axis"] * min(2.0, 0.25 * p["body_v"])
    tag.capsule(_at(p, start), _at(p, p["p_head"]), neck_r, neck_r, T_NECK)


def _head_bill(tag: VoxelGrid, p: dict) -> None:
    """Head, crest and bill, all of which win over the body."""
    def at(point, dy: float = 0.0) -> np.ndarray:
        return _at(p, point, dy)

    r = p["head_r"]
    tag.ball(at(p["p_head"]), r, T_HEAD)
    # Slightly squashed rather than a bare ball: a bird's head is longer than
    # it is tall on everything except an owl, and a perfect sphere on the front
    # of a neck reads as a lollipop. Two extra balls half a radius forward and
    # back cost nothing and give the head a fore-aft axis.
    if r >= 1.6:
        tag.ball(at(p["p_head"] + p["bill_dir"] * r * 0.45), r * 0.80, T_HEAD)
        tag.ball(at(p["p_head"] - p["bill_dir"] * r * 0.40), r * 0.85, T_HEAD)

    # --- crest --------------------------------------------------------------
    # Four to eight voxels, and on a jay, a hoopoe, a lapwing or a lark it is
    # the single most identifiable thing about the animal. Swept BACK, because
    # a crest drawn straight up reads as an antenna.
    crest = p["crest"]
    if crest > 0.02:
        reach = crest * p["depth_v"] * 1.6
        if reach >= 1.0:
            base = p["p_head"] + np.array([0.0, 0.0, r * 0.55])
            tip = base + np.array([-reach * 0.55, 0.0, reach])
            tag.capsule(at(base), at(tip), max(0.6, r * 0.35), 0.5, T_CREST)

    # --- bill ---------------------------------------------------------------
    _bill(tag, p, at)


def _bill(tag: VoxelGrid, p: dict, at) -> None:
    """Four to sixteen voxels that say what the bird eats.

    Drawn as a chain of shrinking balls along a CURVED centreline, not as a
    straight capsule pointed at an angle. `bird.bill_curve` bends the
    centreline; it used to rotate the whole bill, which produced a straight
    bill aimed downwards and looked close enough in a render to survive several
    passes. `tools/birdprobe.py` found it: the bill's tip moved and the bill's
    own bend measured zero at both ends of the slider.

    The hook is separate from the curve because it is a different thing. A
    curlew's whole bill is a smooth arc; a raptor's is straight for four fifths
    of its length and then drops sharply, and that drop is the entire visual
    difference between an eagle and a stork.
    """
    n = p["bill_v"]
    if n < 1.0:
        return
    steps = max(2, int(math.ceil(n)))
    base = p["p_bill_base"]
    fwd = p["bill_dir"]
    depth0 = max(0.6, p["bill_depth"] * p["head_r"] * 2.0)
    # WIDTH IS MEASURED AGAINST THE HEAD, NOT AGAINST THE BILL'S OWN DEPTH.
    # Written the other way round -- `depth0 * (0.35 + 0.9 * gape)` -- the two
    # multiplied together, so a shallow bill could not be wide however far the
    # slider went: a robin's bill reached 1.02 voxels at gape 1.0 against a
    # threshold of 1.2 and the slider did NOTHING on any small bird in the
    # library. `tools/birdprobe.py` measured it as identical at both ends of
    # its range, which is what a parameter gated behind another one looks like
    # from outside.
    gape = p["bill_gape"] * p["head_r"] * 1.7

    prev = base
    for k in range(1, steps + 1):
        u = k / steps
        drop = p["bill_curve"] * n * 0.45 * (u ** 1.6)
        if p["bill_hook"] > 0.0 and u > 0.78:
            drop += p["bill_hook"] * depth0 * 1.5 * ((u - 0.78) / 0.22) ** 1.3
        pt = base + fwd * (n * u) + np.array([0.0, 0.0, -drop])
        r = max(0.5, 0.5 * depth0 * (1.0 - 0.75 * u))
        tag.capsule(at(prev), at(pt), r, max(0.5, r * 0.8), T_BILL)
        # A duck's bill and a heron's are the same length and nobody confuses
        # them, because one is a spoon and the other is a needle. Width is what
        # says which, and it cannot come out of a capsule -- a capsule is round.
        if gape >= 1.0:
            wide = max(1, int(round(0.5 * gape * (1.0 - 0.55 * u))))
            for dy in range(-wide, wide + 1):
                tag.capsule(at(prev, dy), at(pt, dy), max(0.5, r * 0.7),
                            max(0.5, r * 0.6), T_BILL)
        prev = pt


# --- tail -------------------------------------------------------------------


def _tail_length(e: np.ndarray, shape: str, fork: float) -> np.ndarray:
    """How long the tail is at lateral position `e` (0 centre, 1 outer edge),
    as a fraction of its maximum.

    THIS IS THE RIGHT WAY ROUND FOR A TAIL, and it is not the way round a fish's
    caudal fin is described. A fish's tail is drawn as an outline traced around
    the fin. A bird's tail is a fan of feathers of different LENGTHS, and every
    field-guide name for one is a statement about that length profile:
    graduated means the central pair is longest, forked means the outer pair is,
    square means they are all the same. Writing it this way makes all seven
    shapes one function of four lines, and it makes the fork a NOTCH rather than
    a hole -- which is what a fork is, and which is why nothing here needs the
    "keep the lobes at least 1.2 voxels wide" clamp the fish tail needed.
    """
    e = np.clip(np.abs(e), 0.0, 1.0)
    f = min(max(fork, 0.0), 0.95)
    if shape == "square":
        return np.ones_like(e)
    if shape == "rounded":
        return 1.0 - 0.28 * e ** 2
    if shape == "graduated":
        return 1.0 - 0.62 * e
    if shape == "wedge":
        return 1.0 - 0.30 * e ** 1.5
    if shape == "pointed":
        return np.maximum(1.0 - e, 0.0) ** 0.55
    if shape == "notched":
        return 1.0 - f * 0.30 * (1.0 - e ** 2)
    # forked: the outer feathers are longest and the centre is cut away
    return 1.0 - f * (1.0 - e ** 1.7)


def _tail(tag: VoxelGrid, p: dict) -> None:
    """The tail fan.

    ITS PLANE DEPENDS ON THE POSE, and that is not a cosmetic choice. A spread
    tail is a horizontal fan one or two voxels thick. Seen from the side -- the
    camera a perched bird gets -- a horizontal fan is a single line, so the
    first version of this drew a perched magpie with no tail worth the name and
    a perched swallow with no fork at all. A folded tail is a VERTICAL blade,
    which is what a perched bird actually has, and drawn that way the fork and
    the graduation read from exactly the angle the preview uses.
    """
    tail_v = p["tail_v"]
    if tail_v < 1.5:
        return
    nx, ny, nz = p["shape"]
    span = max(1.0, 0.5 * p["tail_width"] * tail_v)
    shape, fork = p["tail_shape"], p["tail_fork"]
    thick = p["tail_thick"]

    axis = p["tail_dir"]
    if p["pose"] == "flying":
        lateral = np.array([0.0, 1.0, 0.0])            # fan spreads sideways
        normal = p["up"]                                # thin vertically
    else:
        lateral = p["up"]                               # blade stands upright
        normal = np.array([0.0, 1.0, 0.0])              # thin across

    base = p["p_rump"] + np.array([0.0, p["ycen"] - p["p_rump"][1], 0.0])
    steps = int(math.ceil(span))
    for j in range(-steps, steps + 1):
        e = abs(j) / max(span, 1e-6)
        if e > 1.0:
            continue
        reach = float(_tail_length(np.array([e]), shape, fork)[0]) * tail_v
        if reach < 0.5:
            continue
        # STARTS ONE VOXEL INSIDE THE BODY. A tail feather rooted exactly at the
        # rump lands on whichever side of a rounding decision the body surface
        # fell, which on a slim-rumped bird means half the feathers are a
        # separate asset.
        p0 = base + lateral * j - axis * 1.2
        p1 = base + lateral * j + axis * reach
        for t in range(thick):
            d = normal * (t - (thick - 1) * 0.5)
            tag.line(p0 + d, p1 + d, T_TAIL, only_air=True)


# --- wings ------------------------------------------------------------------


def _chord(u: np.ndarray | float, shape: str) -> float:
    """Chord at fractional distance `u` out along the half-span, as a fraction
    of the root chord. Savile's four planforms; see the research doc."""
    u = min(max(float(u), 0.0), 1.0)
    if shape == "pointed":
        # High-speed: taper starts early and runs to a point. Falcon, swift,
        # swallow, tern.
        return max(0.0, 1.0 - u) ** 0.55
    if shape == "soaring":
        # High aspect ratio: a plank. Gull, albatross, shearwater. Nearly
        # constant chord until it rounds off in the last tenth.
        return (1.0 - 0.30 * u) * (1.0 if u < 0.90 else max(0.0, (1.0 - u) / 0.10) ** 0.5)
    if shape == "slotted":
        # Slotted high-lift: broad and barely tapered, because the tip is
        # finished with separated fingers rather than by narrowing. Eagle,
        # buzzard, stork, vulture.
        return 1.0 - 0.22 * u
    # Elliptical: broad nearly to the tip and then rounded off. Corvid,
    # gamebird, woodland songbird -- the manoeuvring wing.
    return max(0.0, 1.0 - u ** 2) ** 0.32


def _wings(tag: VoxelGrid, p: dict, body: np.ndarray) -> None:
    if p["pose"] == "flying":
        _wings_spread(tag, p)
    else:
        _wings_folded(tag, p, body)


def _wings_spread(tag: VoxelGrid, p: dict) -> None:
    """Two plates reaching out along +/-y.

    THE RUN STARTS AT THE CENTRELINE, not at the body surface, and steps one
    voxel of y at a time filling the z gap behind it. That is the same rule
    `fish._pectoral` needed and it is here for the same reason: the widest part
    of the body is also the part where the surface is moving fastest in z, so a
    plate begun at the surface lands beside the body about as often as on it.
    Everything drawn inside the body is discarded by the paint pass, so walking
    out from the middle costs nothing in the result.
    """
    nx, ny, nz = p["shape"]
    half = p["wing_half"]
    if half < 3.0:
        return
    # Chord from span and aspect ratio, which is the pair every wing-morphology
    # table is published in. AR = span^2 / area, and area = span x MEAN chord,
    # so mean chord = span / AR -- and the root chord is that divided by the
    # planform's own mean-to-root ratio, which is INTEGRATED from the chord
    # function rather than guessed.
    #
    # It was a flat 1.45 first, which is right for the pointed planform and
    # 20-30% too wide for the other three, and the error compounds with sweep:
    # a kestrel came out as a solid orange delta with its own body and tail
    # buried inside its wings. Computing it removes a magic number that only
    # one of four cases agreed with.
    #
    # AND THE PUBLISHED AREAS INCLUDE THE BODY. Every aspect ratio quoted in
    # the species specs comes from Alerstam et al. 2007, which follows
    # Pennycuick's protocol: wing area is measured with the body strip between
    # the wings included. Johnson measured the same strip separately on 113
    # owls and found it averages 15.4% of the total, so the WING's own mean
    # chord is 0.85 of what span/AR says. Left out, every wing in the library
    # was an eighth too broad and the two big soarers overlapped their own
    # tails.
    sweep, dihedral = p["wing_sweep"], p["wing_dihedral"]
    shape = p["wing_shape"]
    thick = p["wing_thick"]
    us = np.linspace(0.0, 1.0, 64)
    mean_ratio = max(float(np.mean([_chord(u, shape) for u in us])), 0.05)
    root = max(1.5, (2.0 * half) / max(p["wing_aspect"], 1.5) * 0.85 / mean_ratio)
    # A wing leaves the body at the SHOULDER, which is forward on a bird: the
    # wing root sits over the deepest part of the chest, not at the middle of
    # the back.
    sx = p["p_shoulder"][0] - 0.22 * p["body_v"] * p["axis"][0]
    sz = p["p_shoulder"][2] - 0.22 * p["body_v"] * p["axis"][2] + 0.25 * p["depth_v"]
    yc = p["ycen"]

    slot_at = 0.70 if shape == "slotted" else 2.0
    prev_z = {1: sz, -1: sz}
    finger_from: dict[int, tuple[float, float, float]] = {}

    steps = int(math.ceil(half))
    for j in range(0, steps + 1):
        u = j / max(steps, 1)
        if u > slot_at:
            continue
        c = root * _chord(u, shape)
        if c < 0.8:
            continue
        # The sweep multiplier is 0.85 and not 1. At 1.2 -- which is what
        # "carry the tip a bit further back than the slider says" produced --
        # a kestrel at sweep 0.48 pulled its wingtips 21 voxels back along a
        # 34-voxel bird, so the two wings met behind the tail and the whole
        # animal rendered as a solid orange delta with no body in it.
        x_le = sx - u * half * sweep * 0.85
        z_c = sz + u * half * dihedral * 0.8
        a = int(math.floor(x_le - c))
        b = int(math.ceil(x_le))
        for s in (-1, 1):
            y = int(round(yc + s * j))
            if not 0 <= y < ny:
                continue
            za, zb = sorted((z_c, prev_z[s]))
            z0 = int(math.floor(za - (thick - 1) * 0.5))
            z1 = int(math.ceil(zb + (thick - 1) * 0.5))
            xs = slice(max(a, 0), min(b, nx - 1) + 1)
            zs = slice(max(z0, 0), min(z1, nz - 1) + 1)
            sub = tag.data[xs, y, zs]
            sub[sub == T_NONE] = T_WING
            tag.data[xs, y, zs] = sub
            finger_from[s] = (x_le, float(y), z_c)
        prev_z = {s: z_c for s in (-1, 1)}

    # --- slotted tips -------------------------------------------------------
    #
    # The separated primaries of a soaring eagle or a buzzard, drawn as what
    # they are: several narrow blades fanning out from the wrist with daylight
    # between them.
    #
    # NOT AS A STRIPED PLATE, which is the obvious implementation and is wrong.
    # Cutting gaps ALONG the chord at the tip gives a combed wing -- parallel
    # slits in a solid trailing edge -- where a real slotted wing is separated
    # feathers fanning outward from the wrist. At a chord of twenty voxels the
    # difference between those two is not subtle.
    #
    # Each blade is a `grid.capsule` starting on a solid inner-wing voxel, so it
    # is part of the bird by construction. Drawn as single-voxel `grid.line`
    # runs first, the six fingers of an eagle came out as one-voxel hairs off a
    # twenty-voxel wing, which reads as damage rather than as feathers.
    if shape != "slotted" or p["wing_slots"] < 2:
        return
    n = min(max(p["wing_slots"], 2), 6)
    reach = half * (1.0 - slot_at)
    if reach < 2.0:
        return
    for s in (-1, 1):
        if s not in finger_from:
            continue
        x0, y0, z0 = finger_from[s]
        c0 = root * _chord(slot_at, shape)
        for i in range(n):
            f = i / max(n - 1, 1)
            # Fanned in x: the leading finger points forward, the trailing one
            # back, which is the shape of a spread hand.
            start = np.array([x0 - c0 * (0.12 + 0.72 * f), y0, z0])
            ang = math.radians(-32.0 + 64.0 * f)
            end = start + np.array([-reach * math.sin(ang), s * reach * math.cos(ang),
                                    z0 * 0.0 + reach * 0.10 * p["wing_dihedral"]])
            # A CAPSULE, not a line. Drawn as single voxel lines the fingers
            # came out as whiskers -- six one-voxel hairs off the end of a
            # twenty-voxel wing, which reads as damage rather than as feathers.
            # A finger is the widest single feather on the bird and it wants at
            # least two voxels across.
            r = max(0.7, 0.30 * p["wing_thick"] + 0.4)
            tag.capsule(start, end, r, max(0.5, r * 0.6), T_WING)


def _wings_folded(tag: VoxelGrid, p: dict, body: np.ndarray) -> None:
    """A closed wing, as a shell over the upper flank plus a projecting tip.

    A folded wing is not a small spread wing. It is the body's own outline made
    two or three voxels fatter over the middle two thirds of its length, and a
    pair of primary tips that reach back past the rump. Building it that way --
    rather than placing a lozenge beside the body -- makes it adjacent to the
    body everywhere by construction, and it means a species' wing colour and
    wing bar land exactly where the eye expects them: on the outline.

    HOW FAR THE TIPS REACH IS THE CUE WORTH HAVING. On a long-winged bird the
    primaries reach past the tail base and most of the way down the tail; on a
    short-winged one they stop at the rump. That single measurement separates a
    swift from a wren more reliably than any colour, and it is `bird.wing_fold`.
    """
    nx, ny, nz = p["shape"]
    s, h = _stations(p)
    tg, half, dtop, dbot = _profiles(p)
    body_v = max(p["body_v"], 1.0)
    tt = np.clip(1.0 - s / body_v, 0.0, 1.0)

    # THE BULGE IS LATERAL ONLY. Adding it to the depth as well seemed
    # symmetrical and was wrong twice over: it pushed the wing up over the
    # spine, where a real wing does not go, and it put wing-coloured voxels on
    # the top of the back, which is the one place the upperparts colour has to
    # be seen. A folded wing is a rind on the SIDE of the bird.
    bulge = max(1.0, 0.16 * p["width_v"])
    halfmap = np.interp(tt, tg, half) + bulge
    dmap = np.where(h >= 0.0, np.interp(tt, tg, dtop), np.interp(tt, tg, dbot))

    # The closed wing runs from just behind the shoulder to the rump and covers
    # the MIDDLE of the flank -- the mantle shows above it and the flank and
    # belly below. Drawn edge to edge instead, it covered five sixths of every
    # bird and the whole library came out the colour of its wings: 119 wing
    # voxels against 88 of body on a robin, which is not a robin.
    band = ((s >= 0.02 * body_v) & (s <= 0.88 * body_v)
            & (h >= -0.22 * dmap) & (h <= 0.66 * dmap))

    y = np.abs((np.arange(ny, dtype=np.float64) + 0.5) - p["ycen"])
    n = max(p["section"], 0.8)
    shell = ((y[None, :, None] / halfmap[:, None, :]) ** n
             + (np.abs(h) / dmap)[:, None, :] ** n) <= 1.0 + 1e-9
    shell &= band[:, None, :]
    shell &= ~body
    tag.data[shell & (tag.data == T_NONE)] = T_WING

    # --- primary tips -------------------------------------------------------
    reach = p["wing_fold"] * p["tail_v"]
    if reach < 1.5:
        return
    yoff = max(1.0, 0.42 * p["width_v"])
    start_s = 0.10 * body_v
    base = (p["p_rump"] + p["axis"] * start_s
            + np.array([0.0, p["ycen"] - p["p_rump"][1], 0.0])
            + p["up"] * (0.15 * p["depth_v"]))
    for sgn in (-1, 1):
        a = base + np.array([0.0, sgn * yoff, 0.0])
        b = a + p["tail_dir"] * (reach + start_s) - p["up"] * (0.20 * p["depth_v"])
        # THIN, and tapering to nothing. Drawn at a tenth of the body depth it
        # came out as thick as the tail itself, and the two of them merged into
        # one wedge behind the bird -- so the wing reach, which is the whole
        # point of drawing them, could not be seen against the tail it was
        # supposed to be measured against.
        tag.capsule(a, b, max(0.55, 0.06 * p["depth_v"]), 0.5, T_WING)


# --- legs -------------------------------------------------------------------


def _legs(tag: VoxelGrid, p: dict) -> None:
    """Two threads and two feet, and only when the bird is perched.

    A tarsus is under a centimetre thick on everything smaller than a heron, so
    at the 1 cm lattice a leg is a one-voxel line whatever the species. What
    varies, and varies enormously, is LENGTH: a heron's legs are a third of its
    total length and a swift's are barely visible. So `bird.leg_thick` is
    almost always 1 and `bird.leg_len` is the parameter that matters.

    A flying bird's legs are tucked into the belly feathers on nearly every
    species, so nothing is drawn -- which is honest rather than lazy: a pair of
    one-voxel threads trailing off a flying bird is four voxels of noise
    hanging under the only part of the silhouette that reads.
    """
    if p["leg_v"] < 1.0:
        return
    r = max(0.5, 0.5 * p["leg_thick"])
    yoff = max(1.0, 0.30 * p["width_v"])
    # The hip sits under the body's balance point, which on a bird is forward
    # of the middle -- everything behind it is tail, and a bird whose legs are
    # under its tail falls over. Placing them at the body's midpoint gave every
    # species the stance of a penguin.
    hip = (p["p_rump"] + p["axis"] * (0.42 * p["body_v"])
           + np.array([0.0, p["ycen"] - p["p_rump"][1], 0.0]))
    for sgn in (-1, 1):
        a = hip + np.array([0.0, sgn * yoff, -0.25 * p["depth_v"]])
        knee = a + np.array([0.0, 0.0, -p["leg_v"] * 0.55])
        foot = knee + np.array([p["leg_v"] * 0.18, 0.0, -p["leg_v"] * 0.45])
        tag.capsule(a, knee, r, r, T_LEG)
        tag.capsule(knee, foot, r, r, T_LEG)
        # Three toes forward, one back. Six voxels, and without them a perched
        # bird ends in two cut wires.
        #
        # KEPT SHORT. Sized at a twentieth of the animal's length they reached
        # nearly two voxels each way, and two feet of four toes seen from the
        # side stacked into a solid slab under the bird -- nineteen voxels of
        # foot on a robin that has twenty-four voxels of everything else.
        toe = max(1.0, 0.030 * p["length_v"])
        for dx, dy in ((1.3, 0.0), (0.9, 0.7), (0.9, -0.7), (-0.9, 0.0)):
            tip = foot + np.array([dx * toe, dy * toe * 0.8, 0.0])
            tag.line(foot, tip, T_LEG)


# --- colour -----------------------------------------------------------------


def _paint(grid: VoxelGrid, tags: np.ndarray, p: dict, body: np.ndarray) -> None:
    """Assign a material to every solid voxel.

    Four layers, applied in order:

      1. THE PART. A bird is made of visibly different regions and a field
         guide names them: upperparts, underparts, head, wing, bill, legs. This
         is where most of the colour information is, and it costs nothing
         because the parts already exist as tags.
      2. COUNTERSHADING, over the body only. Dark above, pale below.
      3. ONE MARK PER REGION -- head, wing, body. THREE, WHERE A FISH GETS ONE,
         and the reason is that they do not compete. A fish's stripe and its
         bars are drawn on the same twelve-voxel flank, so two of them is
         noise. A bird's cap is on its head, its wing bar is on its wing and
         its streaking is on its breast, and those three sets of voxels are
         disjoint. `tools/birdprobe.py --read` checks the value contrast of
         each region against what it sits on SEPARATELY for exactly that
         reason -- one check against one base colour passed every species in
         the library and would have shipped a white wing bar on a white
         ptarmigan.
      4. THE EYE -- a dark voxel with a contrasting one beside it. Two voxels,
         and they do more than any other two in the asset. The partner is
         chosen from the species' own palette rather than fixed; see `_eye`.
    """
    nx, ny, nz = p["shape"]
    occ = tags != T_NONE
    if not occ.any():
        return
    mat = np.zeros((nx, ny, nz), np.uint8)

    s, h = _stations(p)
    tg, half, dtop, dbot = _profiles(p)
    body_v = max(p["body_v"], 1.0)
    tt = np.clip(1.0 - s / body_v, 0.0, 1.0)
    span = np.maximum(np.interp(tt, tg, dtop) + np.interp(tt, tg, dbot), 1e-6)
    # Height within the local body outline: 0 at the belly line, 1 at the back.
    uz = np.clip((h + np.interp(tt, tg, dbot)) / span, 0.0, 1.0)
    uz3 = np.broadcast_to(uz[:, None, :], (nx, ny, nz))

    mat[occ] = p["mat_belly"]
    upper = uz3 >= (1.0 - p["upperparts"])
    mat[occ & (tags == T_BODY) & upper] = p["mat_back"]
    mat[occ & (tags == T_NECK)] = p["mat_head"]
    mat[occ & (tags == T_HEAD)] = p["mat_head"]
    mat[occ & (tags == T_CREST)] = p["mat_head"]
    mat[occ & (tags == T_WING)] = p["mat_wing"]
    mat[occ & (tags == T_TAIL)] = p["mat_wing"]
    mat[occ & (tags == T_BILL)] = p["mat_bill"]
    mat[occ & (tags == T_LEG)] = p["mat_bill"]

    _head_mark(mat, tags, p)
    _wing_mark(mat, tags, p, tt)
    _body_mark(mat, tags, p, tt, uz3)
    _eye(mat, tags, p)
    grid.data[:] = mat


def _body_front(p: dict) -> np.ndarray:
    """How far back along the body each cell is, 0 at the breast and 1 at the
    rump, as an (nx, nz) map. The same `tt` the paint pass uses."""
    s, _h = _stations(p)
    return np.clip(1.0 - s / max(p["body_v"], 1.0), 0.0, 1.0)


def _head_mark(mat: np.ndarray, tags: np.ndarray, p: dict) -> None:
    """One mark on the head. All five are placed against the head's own centre
    and radius rather than against the grid, so they stay put when the neck
    length or the posture changes."""
    kind = p["head_mark"]
    if kind == "none":
        return
    nx, ny, nz = mat.shape
    c = p["p_head"]
    r = max(p["head_r"], 1.0)
    xs = (np.arange(nx, dtype=np.float64) + 0.5 - c[0])[:, None, None]
    ys = (np.arange(ny, dtype=np.float64) + 0.5 - p["ycen"])[None, :, None]
    zs = (np.arange(nz, dtype=np.float64) + 0.5 - c[2])[None, None, :]

    head = (tags == T_HEAD) | (tags == T_CREST)
    w = max(p["mark_width"], 0.05)
    if kind == "cap":
        sel = zs >= r * (0.55 - 1.4 * w)
    elif kind == "mask":
        # A band through the eye. Field guides call the whole family of these
        # eye lines, and Barlow's result on fish holds here too: the mark works
        # by putting the eye ON its boundary rather than in its middle.
        sel = np.abs(zs - r * 0.22) <= max(0.8, r * w * 1.8)
    elif kind == "supercilium":
        sel = np.abs(zs - r * 0.55) <= max(0.8, r * w * 1.2)
    elif kind == "throat":
        # A THROAT MARK RUNS ONTO THE BREAST, and that is not a liberty. A
        # robin's orange, a great tit's black bib and a swallow's chestnut all
        # cover the face, the throat AND the front of the chest; a mark
        # confined to the head is four voxels on a twenty-voxel bird and the
        # probe reported it at 2.7% of the animal, which is what "present in
        # the voxels and invisible in the render" measures as. The breast half
        # is bounded by how far back along the body it reaches, which is what
        # `bird.mark_width` means for this mark.
        sel = (zs <= -r * (0.55 - 1.4 * w)) & (xs >= -r * 0.4)
        head = head | (tags == T_NECK)
        chest = (tags == T_BODY) & np.broadcast_to(
            (_body_front(p) <= max(w, 0.10) * 1.8)[:, None, :],
            mat.shape)
        mat[chest & (np.arange(nz)[None, None, :]
                     <= c[2] - r * (0.20 - 1.2 * w))] = p["mat_head_mark"]
    else:                                   # collar
        head = (tags == T_NECK)
        sel = np.ones((1, 1, 1), bool)
    mat[head & np.broadcast_to(sel, mat.shape)] = p["mat_head_mark"]


def _wing_mark(mat: np.ndarray, tags: np.ndarray, p: dict, tt: np.ndarray) -> None:
    """One mark on the wing.

    A wing bar runs ACROSS the wing, not along it -- it is the pale tips of one
    row of coverts, so it is a line at a fixed distance from the wing's leading
    edge and it runs the whole way out. That distinction matters here: drawn
    along the span instead, it came out as a racing stripe down the middle of
    the wing, which is a thing no bird has.
    """
    kind = p["wing_mark"]
    if kind == "none":
        return
    wing = tags == T_WING
    if not wing.any():
        return
    nx, ny, nz = mat.shape
    ys = np.abs(np.arange(ny, dtype=np.float64) + 0.5 - p["ycen"])
    # Fractional position out along the wing, 0 at the body and 1 at the tip.
    # On a folded wing "out" is meaningless, so the fore-aft position along the
    # body is used instead -- which is the same thing on a closed wing, because
    # a folded wing lies along the body with its tip pointing aft.
    if p["pose"] == "flying":
        reach = max(ys[wing.any(axis=(0, 2))].max(), 1.0)
        outward = np.broadcast_to((ys / reach)[None, :, None], mat.shape)
    else:
        outward = np.broadcast_to(tt[:, None, :], mat.shape)

    w = max(p["mark_width"], 0.04)
    if kind == "tip":
        sel = outward >= 1.0 - max(w, 0.12)
    elif kind == "panel":
        # THE OUTER THIRD, not the outer half. At 0.45 a raven's violet wing
        # panel covered more of the animal than its own body colour did and it
        # rendered as a purple slab; a panel is a patch on a wing, and if it
        # reaches past the wrist it stops reading as one.
        sel = outward >= 0.64
    elif kind == "bar":
        sel = np.abs(outward - 0.42) <= w * 0.6
    elif kind == "doublebar":
        sel = (np.abs(outward - 0.34) <= w * 0.45) | (np.abs(outward - 0.56) <= w * 0.45)
    else:
        return
    mat[wing & sel] = p["mat_mark"]


def _body_mark(mat: np.ndarray, tags: np.ndarray, p: dict, tt: np.ndarray,
               uz3: np.ndarray) -> None:
    """One mark on the body.

    All four are drawn as parametric marks rather than as a noise field, for
    the same reason a fish's are: at this size a reaction-diffusion pattern has
    a wavelength the grid decides rather than the bird. The one that IS noise
    -- `speckled` -- is thresholded by QUANTILE, so `mark_strength` means the
    exact share of the bird that is spotted rather than whatever the noise
    happened to do.
    """
    kind = p["body_mark"]
    if kind == "none":
        return
    region = (tags == T_BODY) | (tags == T_NECK)
    if not region.any():
        return
    nx, ny, nz = mat.shape
    n = max(1, p["mark_count"])
    w = min(max(p["mark_width"], 0.03), 0.9)

    if kind == "breastband":
        sel = (np.abs(tt - 0.16) <= max(w, 0.06))[:, None, :]
        mat[region & np.broadcast_to(sel, mat.shape)] = p["mat_mark"]
        return
    if kind == "barred":
        frac = np.mod(tt * n + p["phase"], 1.0)
        sel = np.broadcast_to((frac < w)[:, None, :], mat.shape)
        # Bars stop at the waterline of the underparts on nearly every barred
        # bird; running them over the back made the animal read as wrapped.
        mat[region & sel & (uz3 <= 0.72)] = p["mat_mark"]
        return
    if kind == "streaked":
        # Streaks run ALONG the bird, so they are periodic across it, not along
        # it. Drawn against y and height together so they survive on the
        # rounded front of the breast, where a pure-y stripe collapses to one
        # streak in the middle.
        ys = (np.arange(ny, dtype=np.float64) + 0.5 - p["ycen"])
        q = np.mod((ys[None, :, None] / max(1.5, p["width_v"] / max(n, 1))
                    + uz3 * 0.0) + p["phase"], 1.0)
        sel = q < w
        mat[region & sel & (uz3 <= 0.62)] = p["mat_mark"]
        return

    # speckled: coherent blotches, thresholded by quantile.
    from .rock import coherent_noise

    sigma = max(1.0, 0.5 * p["mark_width"] * p["length_v"])
    field = coherent_noise((nx, ny, nz), p["salt"], sigma)
    vals = field[region]
    if vals.size == 0:
        return
    share = min(max(p["mark_strength"], 0.0), 1.0)
    if share <= 0.0:
        return
    cut = float(np.quantile(vals, 1.0 - share))
    mat[region & (field >= cut)] = p["mat_mark"]


def _eye(mat: np.ndarray, tags: np.ndarray, p: dict) -> None:
    """A dark voxel on each side of the head, with a pale one beside it.

    Worth more than any other two voxels in the asset. A voxel animal without
    an eye reads as an object; with one it reads as facing somewhere. The pale
    partner is not a highlight -- it is a contrast partner, and without it a
    dark eye on a dark head disappears, which is what happened on the raven,
    the starling and the woodpecker until it was added.

    A BIRD'S EYE IS PROPORTIONALLY MUCH BIGGER THAN A FISH'S and much further
    forward: the measured position is a little over a third of the way back
    along the head from the bill, high on the side. On a small songbird the eye
    is nearly a quarter of the head's height, which is why `bird.eye` runs to 3
    where `fish.eye` runs to 3 on an animal three times the size.

    THE CONTRAST PARTNER IS CHOSEN, NOT FIXED. The fish version uses the belly
    colour, which works because a fish is countershaded and its belly is always
    the pale end of its own scheme. A bird is not: a raven, a starling and a
    macaw are all one value from head to tail, and on all three the fish rule
    put a near-black partner beside a near-black pupil and the eye disappeared
    entirely. So the partner is whichever of the species' OWN materials has the
    highest relative luminance, and it is skipped rather than drawn if even
    that one is within a contrast ratio of 1.4 of the pupil -- a partner that
    does not contrast is two wasted voxels and a smudge.
    """
    rad = int(round(p["eye"]))
    if rad < 1:
        return
    nx, ny, nz = mat.shape
    c = p["p_head"]
    x = int(round(c[0] + p["head_r"] * 0.35))
    z = int(round(c[2] + p["head_r"] * 0.30))
    if not (0 <= x < nx and 0 <= z < nz):
        return
    head = tags == T_HEAD
    # THE INTENDED POINT IS A HINT, NOT AN ADDRESS. On a small head -- a
    # hoopoe's is seventeen voxels and a starling's eighteen -- the computed
    # station rounds to a cell just outside the ball about a third of the time,
    # and the eye was then not drawn at all. It failed silently, which is worse
    # than failing: `tools/birdprobe.py --lattice` showed a 1 cm hoopoe with
    # zero eye voxels sitting in a column of otherwise healthy numbers. So the
    # nearest head column to the hint is used instead, searched outward.
    if not head[x, :, z].any():
        nx, ny, nz = mat.shape
        best = None
        for dx in range(-2, 3):
            for dz in range(-2, 3):
                xx, zz = x + dx, z + dz
                if not (0 <= xx < nx and 0 <= zz < nz):
                    continue
                if not head[xx, :, zz].any():
                    continue
                d = dx * dx + dz * dz
                if best is None or d < best[0]:
                    best = (d, xx, zz)
        if best is None:
            return
        _, x, z = best
    ys = np.flatnonzero(head[x, :, z])
    if ys.size == 0:
        return

    palette = [p[k] for k in ("mat_back", "mat_belly", "mat_head", "mat_wing",
                              "mat_mark", "mat_head_mark", "mat_bill")]
    partner = max(palette, key=lambda m: _luminance(materials.color(m)))
    if _contrast(materials.color(partner), materials.color(p["mat_eye"])) < 1.4:
        partner = None

    n = rad - 1
    for y in (int(ys[0]), int(ys[-1])):
        for dz in range(-n, n + 1):
            for dx in range(-n, n + 1):
                xx, zz = x + dx, z + dz
                if 0 <= xx < nx and 0 <= zz < nz and head[xx, y, zz]:
                    mat[xx, y, zz] = p["mat_eye"]
        xx = min(x + rad, nx - 1)
        if partner is not None and head[xx, y, z]:
            mat[xx, y, z] = partner


def _luminance(rgb) -> float:
    """WCAG relative luminance. Duplicated from `tools/birdprobe.py` on
    purpose: the probe must be able to disagree with the generator, and a
    shared helper is a shared mistake."""
    def lin(c: float) -> float:
        c = c / 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4

    r, g, b = (lin(v) for v in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def _contrast(c1, c2) -> float:
    a, b = sorted((_luminance(c1), _luminance(c2)), reverse=True)
    return (a + 0.05) / (b + 0.05)
