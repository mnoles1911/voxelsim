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

PAIRED = frozenset({P_FIN_PAIRED, P_WING, P_LEG, P_EAR, P_HORN})

_BASE_NAMES = {
    P_NONE: "none", P_BODY: "body", P_HEAD: "head", P_NECK: "neck",
    P_JAW: "jaw", P_TAIL: "tail", P_CREST: "crest",
    P_FIN_MEDIAN: "median-fin", P_FIN_CAUDAL: "caudal-fin",
    P_FIN_PAIRED: "paired-fin", P_WING: "wing", P_LEG: "leg",
    P_EAR: "ear", P_HORN: "horn",
}


def names() -> dict[int, str]:
    """Every id that can appear in a baked asset, named."""
    out = dict(_BASE_NAMES)
    for pid in PAIRED:
        out[pid] = f"{_BASE_NAMES[pid]}-L"
        out[pid + SIDE_STRIDE] = f"{_BASE_NAMES[pid]}-R"
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
    """
    if tags is None:
        return None
    ny = tags.shape[1]
    far = np.arange(ny)[None, :, None] >= (ny / 2.0)
    out = tags.copy()
    for pid in PAIRED:
        m = (tags == pid) & far
        if m.any():
            out[m] = pid + SIDE_STRIDE
    return out


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
        out[tags == private] = shared
    return split_sides(out)
