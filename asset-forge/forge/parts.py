"""One part vocabulary for every animal, and the left/right split.

WHY THIS IS SHARED RATHER THAN PER-GENERATOR
--------------------------------------------
`bird.py` and `fish.py` each grew their own tag numbering, because each needed
one for painting: the paint pass has to tell a wing from a body, and a fin from
a flank. Those numbers mean different things -- `bird.T_WING` is 6 and
`fish.P_PAIRED` is 3 -- which is fine while the tags never leave the file that
made them, and useless the moment they ship. A baked asset that says "part 6"
has to mean the same thing whoever reads it.

So the generators keep their private painting tags and map into THIS table on
the way out. A quadruped will add to it rather than invent a third numbering.

WHY SIDES ARE SEPARATE PARTS, WHICH IS THE POINT OF THE FILE
-----------------------------------------------------------
A painting tag answers "what colour is this voxel". Both of a raven's wings are
wing-coloured, so one tag serves. A RIGGING tag answers "what moves together",
and the two wings emphatically do not: a bird that flaps rotates them in
opposite directions, and an animal that walks moves four legs on four different
schedules.

Measured on `common-raven` before this existed: tag `wing` covered y 0..15 of a
16-deep grid, 248 voxels left of the midline and 238 right, all under one id.
Rigged from that, a raven could not flap -- both wings were one rigid body --
and could not walk, for the same reason on its legs. Nothing rendered wrong,
because painting never asked the question.

The split is by the MIDLINE of the asset's own depth axis, which is exact here
rather than approximate: every animal in this library is built facing +x with
its mirror plane down the middle of y (`fish.build`, `bird.build` both say so in
their docstrings), and a paired part is drawn on both sides of it. A part that
straddles the midline -- a body, a dorsal fin, a tail -- is NOT in `PAIRED` and
is never split.

AND A QUADRUPED HAS FOUR LEGS, WHICH LEFT/RIGHT ALONE CANNOT SAY
----------------------------------------------------------------
Two ids -- leg-L and leg-R -- were enough while the only legged animal was a
bird, because a bird has exactly two legs and they are on opposite sides of one
line. A horse has four, and front-left and rear-left are both on the left. Under
the left/right split alone they land under ONE id, which is the raven's-wings
defect exactly: nothing renders wrong, and the animal cannot walk, because a
walk is four limbs moving on four different schedules and a rig can only move
what the file distinguishes.

So there is a SECOND stride, `AXIS_STRIDE`, applied to the fore/hind axis the
way `SIDE_STRIDE` is applied to the left/right one. Four ids fall out of two
strides without a new base id:

    leg fore-L   11          leg fore-R   11 + 64 =  75
    leg hind-L   11 + 32     leg hind-R   11 + 96 = 107

WHY A STRIDE RATHER THAN A NEW BASE ID for the hind leg. Both work and both are
append-only, so neither renumbers a file already baked. The stride wins on three
counts. It keeps a leg ONE anatomical thing -- `PARENT`, the naming table and any
future rule about limbs is written once and covers all four, where a second base
id would need every such table to list both and would go stale the first time one
was updated alone. It composes: the day something needs a fore/hind distinction
on a part that is not a leg, the mechanism is already there. And it keeps the two
questions separable -- `base_id` strips both strides and gets back "this is a
leg", which is what a paint rule or a material rule wants to ask, while
`side_of` and `axis_of` answer "which one" separately.

The cost is an arithmetic constraint, and it is ASSERTED below rather than
remembered: every base id must stay under `AXIS_STRIDE`, or a base plus a stride
collides with a different base. There are 14 base ids and the ceiling is 32.

WHY THE FORE/HIND SPLIT IS NOT MEASURED THE WAY THE LEFT/RIGHT ONE IS. The
left/right split reads the array midline, and that is exact because the animal is
mirrored about it. There is no equivalent line for fore and hind: the array's
x-midpoint sits somewhere in a trunk with a head and neck projecting off one end
and a tail off the other, so on a squirrel -- whose tail is longer than its body
-- the midpoint falls BEHIND the hips and both pairs of legs come out "fore".
That is a silent mislabelling, which is the failure this file exists to stop. So
the GENERATOR says which is which, by drawing the two pairs under two private
tags, and `to_shared` maps one of them onto the strided id. The generator knows
where it put the hips; nothing here has to guess.
"""
from __future__ import annotations

import numpy as np

# Shared ids. Append only: a baked asset carries these numbers, so changing one
# silently re-labels every file already written.
P_NONE = 0
P_BODY = 1
P_HEAD = 2
P_NECK = 3
P_JAW = 4          # bill on a bird, muzzle on a quadruped
P_TAIL = 5
P_CREST = 6        # crest, mane, dorsal ridge
P_FIN_MEDIAN = 7   # dorsal and anal fins: on the midline, never paired
P_FIN_CAUDAL = 8
P_FIN_PAIRED = 9   # pectoral and pelvic
P_WING = 10
P_LEG = 11
P_EAR = 12         # unused until a quadruped exists; reserved so it is not
P_HORN = 13        # renumbered later, which would re-label baked files

# Paired parts become two: id and id+SIDE_STRIDE for the far side. A stride
# rather than a flag bit because it keeps every id a small integer that indexes
# a table directly, and 128 parts is far past anything anatomical.
SIDE_STRIDE = 64

# ... and fore/hind is a second stride on top of it. See the header: this is
# what makes four legs four parts instead of two. `SIDE_STRIDE` stays the larger
# of the two so that the ids a bird already bakes -- leg-L 11 and leg-R 75 --
# are untouched, and the hind pair lands in the gap between them.
AXIS_STRIDE = 32

PAIRED = frozenset({P_FIN_PAIRED, P_WING, P_LEG, P_EAR, P_HORN})

# Parts that come in a FORE set and a HIND set as well as a left and a right.
# Only legs, and deliberately only legs: an ear is not a fore ear, and a
# generator that shifted one by AXIS_STRIDE would be inventing a part nothing
# names. `to_shared` refuses an id outside this set carrying the stride.
FORE_HIND = frozenset({P_LEG})

_BASE_NAMES = {
    P_NONE: "none", P_BODY: "body", P_HEAD: "head", P_NECK: "neck",
    P_JAW: "jaw", P_TAIL: "tail", P_CREST: "crest",
    P_FIN_MEDIAN: "median-fin", P_FIN_CAUDAL: "caudal-fin",
    P_FIN_PAIRED: "paired-fin", P_WING: "wing", P_LEG: "leg",
    P_EAR: "ear", P_HORN: "horn",
}

# THE ARITHMETIC THAT MAKES TWO STRIDES SAFE, checked at import rather than
# trusted to a comment. Every base id has to be below the smaller stride: at
# base 32 the hind-leg id would be 64, which is the left/right stride, and the
# collision would show up as a hind leg baked under some other part's name. The
# strides also have to be distinct multiples that do not sum onto each other,
# which for 32 and 64 they are.
assert max(_BASE_NAMES) < AXIS_STRIDE, (
    f"part ids have grown past {AXIS_STRIDE}, which is the fore/hind stride; "
    f"a base id at or above it collides with a strided one. Raise both strides "
    f"together (they must stay under 256: a .vxa part id is one byte).")
assert AXIS_STRIDE < SIDE_STRIDE and max(_BASE_NAMES) + AXIS_STRIDE + SIDE_STRIDE < 256


def names() -> dict[int, str]:
    """Every id that can appear in a baked asset, named."""
    out = dict(_BASE_NAMES)
    for pid in PAIRED:
        base = _BASE_NAMES[pid]
        if pid in FORE_HIND:
            # THE UNSTRIDED PAIR IS NOT CALLED "fore", and that is a deliberate
            # asymmetry rather than an oversight. A bird's two legs bake as ids
            # 11 and 75 today and those files exist, so 11 cannot be renamed;
            # but a bird's legs are its HIND limbs, and calling them "leg-fore"
            # would be printing a wrong word on every bird in the library to
            # tidy up a table. "leg-L" is true of a bird's leg and true of a
            # horse's front leg, and "leg-hind-L" beside it is unambiguous about
            # which is which. Code that needs the distinction as data asks
            # `axis_of`, which does say "fore".
            out[pid] = f"{base}-L"
            out[pid + SIDE_STRIDE] = f"{base}-R"
            out[pid + AXIS_STRIDE] = f"{base}-hind-L"
            out[pid + AXIS_STRIDE + SIDE_STRIDE] = f"{base}-hind-R"
        else:
            out[pid] = f"{base}-L"
            out[pid + SIDE_STRIDE] = f"{base}-R"
    return out


def split_sides(tags: np.ndarray) -> np.ndarray:
    """Send the far half of every paired part to its own id.

    `tags` is (nx, ny, nz) and y is the depth axis every animal here mirrors
    about. Voxels at y below the midline keep the base id; the rest move up by
    `SIDE_STRIDE`.

    The midline is taken from the ARRAY, not from the animal's centre of mass.
    They are the same thing for a mirrored body and the array is the one that
    cannot drift: a bird whose head is turned would move its centroid off the
    plane its wings were drawn about, and the wings are what is being split.

    THE FORE/HIND STRIDE IS ALREADY ON THE ID BY THE TIME THIS RUNS, and this
    function has to preserve it: a hind-left leg arrives as 43 and must leave as
    43 or 107, never as 11 or 75. So the loop is over the ids PRESENT rather
    than over `PAIRED`, and membership is tested on the base. Written the
    original way -- `for pid in PAIRED` -- the hind legs would match nothing,
    stay on their left-side id whichever side they are, and a horse would walk
    with its two right legs locked together. That is the same bug this file was
    written to end, one axis over.
    """
    if tags is None:
        return None
    ny = tags.shape[1]
    far = np.arange(ny)[None, :, None] >= (ny / 2.0)
    out = tags.copy()
    for pid in (int(v) for v in np.unique(tags)):
        if pid == P_NONE or pid >= SIDE_STRIDE or base_id(pid) not in PAIRED:
            continue
        m = (tags == pid) & far
        if m.any():
            out[m] = pid + SIDE_STRIDE
    return out


def side_of(pid: int) -> str:
    """"L" or "R" for a paired part, "" for one on the midline."""
    if base_id(pid) not in PAIRED:
        return ""
    return "R" if pid >= SIDE_STRIDE else "L"


def axis_of(pid: int) -> str:
    """"fore" or "hind" for a limb that has both, "" otherwise."""
    if base_id(pid) not in FORE_HIND:
        return ""
    return "hind" if (pid % SIDE_STRIDE) >= AXIS_STRIDE else "fore"


def to_shared(tags: np.ndarray, table: dict[int, int]) -> np.ndarray:
    """Translate a generator's private painting tags into the shared ids, and
    split the paired parts left from right.

    Both steps here rather than at the call site, because doing one without the
    other is the bug: private ids that never got split rig an animal whose
    wings are one body, and split ids that never got translated collide with
    whatever the next generator numbered differently.
    """
    out = np.zeros_like(tags)
    for private, shared in table.items():
        base = base_id(shared)
        # A mapping table is written by hand once per generator and then read
        # for the life of the file, so it is checked here rather than reviewed.
        # The failure it catches is a generator that shifts, say, an ear by
        # AXIS_STRIDE -- producing id 44, which names nothing, and which
        # `names()` would not list, so the part would ship anonymous.
        if shared != base and base not in PAIRED:
            raise ValueError(
                f"part {shared} is {_BASE_NAMES.get(base, base)} carrying a "
                f"stride, but {_BASE_NAMES.get(base, base)} is not a paired part")
        if shared >= SIDE_STRIDE:
            raise ValueError(
                f"part {shared} already carries SIDE_STRIDE; a generator hands "
                f"over the LEFT id and `split_sides` makes the right one")
        if shared >= AXIS_STRIDE and base not in FORE_HIND:
            raise ValueError(
                f"part {shared} carries AXIS_STRIDE, but "
                f"{_BASE_NAMES.get(base, base)} has no fore/hind pair")
        out[tags == private] = shared
    return split_sides(out)


# Who each part hangs off. A rig needs a tree, and the tree is anatomy rather
# than something a generator should be free to choose per species.
#
# A part whose parent is absent falls through to the next one up: plenty of
# birds have no crest, and a fish has no neck, so a head must still find the
# body. `joints` walks this until it lands on something the animal actually has.
PARENT = {
    P_NECK: P_BODY,
    P_HEAD: P_NECK,
    P_JAW: P_HEAD,
    P_CREST: P_HEAD,
    P_TAIL: P_BODY,
    P_FIN_MEDIAN: P_BODY,
    P_FIN_CAUDAL: P_BODY,
    P_FIN_PAIRED: P_BODY,
    P_WING: P_BODY,
    P_LEG: P_BODY,
    P_EAR: P_HEAD,
    P_HORN: P_HEAD,
}


def base_id(pid: int) -> int:
    """The part without its side or its fore/hind. Both strides come off.

    This is what every table in this file is keyed on -- `PARENT`, `PAIRED`,
    `_BASE_NAMES` -- so it has to strip BOTH, and it has to strip them in the
    right order. `107 - 64 = 43`, `43 - 32 = 11`: a leg.
    """
    if pid >= SIDE_STRIDE:
        pid -= SIDE_STRIDE
    if pid >= AXIS_STRIDE:
        pid -= AXIS_STRIDE
    return pid


def joints(tags: np.ndarray) -> list[dict]:
    """Where each part turns about its parent, in local voxel coordinates.

    DERIVED FROM THE CONTACT SURFACE, not from the generator. A joint is the
    centroid of the voxels where a part touches its parent face-to-face, which
    is not an approximation of the anatomy -- it IS the anatomy. A shoulder is
    where the wing meets the body. A hip is where the leg does.

    Doing it this way rather than having each generator report its own joint
    positions is worth the paragraph. The generators place parts from
    parameters, so a reported joint would be a SECOND expression of the same
    fact, free to drift from the geometry when either changes -- and this
    project has already paid for that once, with a foliage parameter consumed
    twenty-five lines before it was modified. The contact surface cannot drift,
    because it is measured off the voxels that shipped. It also costs a future
    quadruped generator nothing: four legs meet a body somewhere, and the same
    measurement finds all four without knowing what a leg is.

    Returns one record per part present, `body` excluded -- the body is the
    root and turns about nothing. `parent` is the id it hangs off. `origin` is
    in voxels, fractional, relative to the array.
    """
    if tags is None:
        return []
    present = {int(v) for v in np.unique(tags) if v != P_NONE}
    out: list[dict] = []
    for pid in sorted(present):
        base = base_id(pid)
        if base == P_BODY:
            continue
        # Walk up until we reach a part this animal actually has.
        parent = PARENT.get(base, P_BODY)
        while parent not in present and parent != P_BODY:
            parent = PARENT.get(parent, P_BODY)
        if parent not in present:
            continue

        mine = tags == pid

        def contact(other: int) -> np.ndarray:
            # Face-adjacent contact, six directions. A corner touch is not a
            # joint for the same reason it is not a join: nothing can hang off
            # it.
            theirs = tags == other
            hit = np.zeros_like(mine)
            for ax in (0, 1, 2):
                for sh in (1, -1):
                    nb = np.roll(theirs, sh, axis=ax)
                    sl = [slice(None)] * 3
                    sl[ax] = 0 if sh > 0 else -1
                    nb[tuple(sl)] = False
                    hit |= mine & nb
            return hit

        # AND IF THE NEAREST ANCESTOR IS PRESENT BUT NOT TOUCHING, KEEP WALKING.
        #
        # The original rule walked up the chain until it found a part the animal
        # HAS, and then stopped. That is one condition short, and a gorilla
        # found the gap: it carries a neck share of 0.10 under a very deep
        # chest, so its neck is drawn and then almost entirely reclaimed by the
        # trunk (the buried parts go down first -- see `quadruped.build`), and
        # what survives is a handful of voxels that do not reach the skull. The
        # head's anatomical parent was therefore present, and not adjacent, and
        # the head shipped with no joint at all -- a rig defect on the one part
        # of an animal a player looks at.
        #
        # A part that cannot reach its own parent hangs off its grandparent,
        # which is what a short-necked animal's head physically does.
        touch = contact(parent)
        while not touch.any() and parent != P_BODY:
            parent = PARENT.get(parent, P_BODY)
            if parent not in present:
                break
            touch = contact(parent)
        if not touch.any() and parent != P_BODY and P_BODY in present:
            parent = P_BODY
            touch = contact(parent)
        if not touch.any():
            # A part that touches its parent only at a corner, or not at all.
            # Reported rather than dropped: a limb with no joint is a rigging
            # defect and silence is how it would ship.
            out.append({"part": pid, "parent": parent, "origin": None})
            continue
        xs, ys, zs = np.nonzero(touch)
        out.append({"part": pid, "parent": parent,
                    "origin": (float(xs.mean()), float(ys.mean()), float(zs.mean())),
                    "contact": int(touch.sum())})
    return out
