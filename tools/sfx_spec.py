#!/usr/bin/env python3
"""The canonical SFX id list -- the join between the library and the prompts.

WHY THIS FILE EXISTS
--------------------
docs/sfx-library.md is a SPEC written as prose and matrices ("4 gaits x 12
surfaces x var 5 = 240 files"). docs/sfx-prompts.md is one row per file id with
the literal ElevenLabs prompt. Those are different granularities on purpose --
but it meant "does every library entry have a prompt?" could only be answered
by reading 1,200 lines of two documents and trusting the reader.

That is the shape of bug this project keeps getting bitten by: a join COMPUTED
rather than CHECKED. So the matrices are expanded here, in code, once, and
tools/lint-sfx-coverage.py fails if the prompts doc and this spec disagree in
either direction.

WHAT IS AUTHORITATIVE
---------------------
docs/sfx-library.md is authoritative for WHAT SOUNDS EXIST. The family files
are a transcription of it and each cites its library section. If the two
disagree, the library is right and the family file has the bug.

docs/sfx-prompts.md is authoritative for THE PROMPT TEXT OF RENDERED IDS. 108
ids have audio rendered from their exact prompt text; changing that text would
silently break reproducibility. The generator NEVER rewrites an existing row --
it only emits rows for ids the doc does not have yet.

WHAT THIS SPEC DELIBERATELY OMITS
---------------------------------
Growth slots the library names as patterns rather than sounds: the "reserved
growth-slot pattern for any future weapon class", the "generic future-enemy
stubs", and future-companion families. The library is explicit that these are
not invented yet; enumerating guesses would put ids in the lint that no
designer ever asked for.

LAYOUT
------
Families live in tools/sfx_families/*.py, one file per library category, loaded
in filename order. Each is executed with add/one/loop_/DRY/LOOPY in scope --
they are data, not importable modules, which is why they are exec'd rather than
imported (and why they do not need __init__.py or a package on sys.path).

    python tools/gen-sfx-prompts.py     # emit rows the prompts doc is missing
    python tools/lint-sfx-coverage.py   # check the join both directions
"""

from __future__ import annotations

import os
import glob

# --- shared prompt language ------------------------------------------------
# Lifted from the hand-authored Phase 1 rows so generated prompts read as the
# same document. The loop tail is not decoration: it is the hardened wording
# that made the Phase 1 loops come back seamless (prompts doc section 8,
# "loops re-rendered with hardened wording, validated seamless").

DRY = "dry close mono, no reverb, no music"
LOOPY = (
    "constant unchanging texture and level from the first instant to the last, "
    "no onset transient, no attack, no fade in or out, no swell, built to "
    "repeat with an inaudible join, "
)

FAMILY_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sfx_families")

# --- rendered-name drift ---------------------------------------------------
# THE PHASE 2 RENDER USED A DIFFERENT NAMING SCHEME THAN THE LIBRARY SPECIFIES,
# and 133 files on disk in the Godot repo carry the divergent names. The
# library says vox_<material>_<event> with the correct tool implied (section 6:
# "generate correct-tool strike/break/place fully"); the render wrote
# vox_<tool>_<event>_<material>. Same sounds, different ids.
#
# These are recorded as aliases rather than fixed in either direction, because
# fixing it is a decision with a cost on both sides: renaming the doc breaks
# reproducibility against 133 rendered files, and renaming the library breaks
# its own stated convention and the folder plan built on it. The alias keeps
# the lint honest -- it passes, and it PRINTS the drift every run so it cannot
# quietly become permanent.
#
# Left-hand side is the library id (canonical); right-hand side is what was
# actually rendered and what the prompts doc row is called.
ALIASES = {
    "vox_dirt_strike":  "vox_shovel_strike_dirt",
    "vox_dirt_break":   "vox_shovel_break_dirt",
    "vox_dirt_place":   "vox_place_dirt",
    "vox_sand_strike":  "vox_shovel_strike_sand",
    "vox_sand_break":   "vox_shovel_break_sand",
    "vox_sand_place":   "vox_place_sand",
    "vox_grass_strike": "vox_shovel_strike_grass",
    "vox_grass_break":  "vox_shovel_break_grass",
    "vox_grass_place":  "vox_place_grass",
    "vox_stone_strike": "vox_pick_strike_stone",
    "vox_stone_break":  "vox_pick_break_stone",
    "vox_stone_place":  "vox_place_stone",
    # The library's impact target is `stone_terrain`; the render dropped the
    # suffix on these two only -- the other 33 impact ids were never rendered,
    # so the drift stops here.
    "cmb_hit_slash_stone_terrain":  "cmb_hit_slash_stone",
    "cmb_hit_pierce_stone_terrain": "cmb_hit_pierce_stone",
}

# Rendered ids that are COARSER than the library, standing in for several
# library entries at once. These are NOT aliases: the library asks for one
# wrong-tool scrape per material (18 of them) and the render made two generic
# ones. So the per-material ids stay MISSING and still get prompts -- this list
# only stops the lint calling the coarse file an orphan.
COARSE_STANDINS = {
    "vox_wrongtool_soft": "stands in for the soft-material wrong-tool scrapes; "
                          "the library asks for one per material",
}


def one(_id, prompt, dur, infl, var, bus, sect, loop="N"):
    return dict(id=_id, prompt=prompt, dur=dur, infl=infl, loop=loop,
                var=var, bus=bus, sect=sect)


def loop_(_id, body, dur, infl, bus, sect):
    """A seamless loop. `body` names the sound; the hardened tail is appended."""
    return one(_id, "Perfectly seamless loop of %s, %s%s" % (body, LOOPY, DRY),
               dur, infl, 1, bus, sect, loop="Y")


def load():
    """Execute every family file and return the full entry list, in order."""
    entries = []

    def add(*rows):
        for r in rows:
            entries.append(r)

    env = dict(add=add, one=one, loop_=loop_, DRY=DRY, LOOPY=LOOPY)
    paths = sorted(glob.glob(os.path.join(FAMILY_DIR, "*.py")))
    if not paths:
        raise SystemExit("sfx_spec: no family files under %s" % FAMILY_DIR)
    for p in paths:
        with open(p, encoding="utf-8") as fh:
            src = fh.read()
        # Each family gets a FRESH namespace seeded with the helpers, so a
        # loop variable or descriptor table in one file cannot leak into the
        # next and silently change ids.
        exec(compile(src, p, "exec"), dict(env))

    seen = {}
    for e in entries:
        if e["id"] in seen:
            raise SystemExit("sfx_spec: duplicate id %s (sections %s and %s)"
                             % (e["id"], seen[e["id"]], e["sect"]))
        seen[e["id"]] = e["sect"]
    return entries


if __name__ == "__main__":
    rows = load()
    import collections
    by = collections.Counter(r["sect"] for r in rows)
    print("%d ids across %d categories" % (len(rows), len(by)))
    for k in sorted(by):
        print("  cat %s: %3d ids" % (k, by[k]))
    print("total files (ids x var): %d" % sum(r["var"] for r in rows))
