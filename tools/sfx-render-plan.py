#!/usr/bin/env python3
"""What would it cost to render the SFX library, and in what order.

The prompts now exist for all 724 library ids (docs/sfx-prompts.md). Nothing
here renders anything -- this answers the question that comes BEFORE spending:
how many ElevenLabs credits does each category cost, and what is the smallest
set that makes the game audible.

THE COST MODEL IS MEASURED, NOT GUESSED. docs/sfx-prompts.md section 8 records
it from two clean batches (Water 1,697 credits; Cat 04 ~3,000):

    ~8.2 credits/second + ~19 credits/generation floor

and notes that render_sfx.py deliberately used a slightly conservative 9 cr/s
and 20 cr/gen, because estimates ran ~6% high and a protective cap is the point.
This tool uses the same conservative pair for the same reason: an estimate that
comes in under budget is a nuisance, one that comes in over is a stopped render.

A "generation" is one take. An id with var 5 costs five generations, because the
variation takes are what stop footsteps sounding like a machine gun -- they are
not optional padding.

    python tools/sfx-render-plan.py              # per-category table
    python tools/sfx-render-plan.py --phases     # the library's phase order
    python tools/sfx-render-plan.py --rendered   # what is already done
"""

from __future__ import annotations

import os
import sys
import collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sfx_spec  # noqa: E402

CR_PER_SEC = 9.0     # conservative; measured ~8.2
CR_PER_GEN = 20.0    # conservative; measured ~19

CAT_NAMES = {
    "01": "Locomotion & foley", "02": "Combat: player", "03": "Impacts & enemies",
    "04": "Tools & voxel", "05": "Crafting & stations", "06": "Objects & items",
    "07": "Weather", "08": "Water", "09": "Fire & camp", "10": "Region ambient beds",
    "11": "Day/night & time", "12": "Lockpicking", "13": "Mini-games",
    "14": "Investigation", "15": "UI & menu", "16": "Death & respawn",
    "17": "NPC non-verbal & crowd", "18": "Economy & vendor", "19": "Magic",
}

# Already rendered in the Godot repo, from docs/sfx-prompts.md section 8. These
# are takes on disk there, NOT in voxelsim -- so they are a cost already paid,
# not a cost avoided here, until somebody ports them.
RENDERED_IDS = 108
RENDERED_TAKES = 548


def cost(entry):
    """Credits to render every take of one id."""
    return entry["var"] * (float(entry["dur"]) * CR_PER_SEC + CR_PER_GEN)


def table(rows, title):
    by = collections.defaultdict(list)
    for e in rows:
        by[e["sect"]].append(e)
    print("\n%s" % title)
    print("%-4s %-24s %5s %6s %10s" % ("cat", "", "ids", "takes", "credits"))
    print("-" * 54)
    tot_i = tot_t = tot_c = 0
    for cat in sorted(by):
        es = by[cat]
        ids, takes = len(es), sum(e["var"] for e in es)
        cr = sum(cost(e) for e in es)
        tot_i += ids; tot_t += takes; tot_c += cr
        print("%-4s %-24s %5d %6d %10s" % (cat, CAT_NAMES.get(cat, ""), ids, takes,
                                           "{:,}".format(int(round(cr)))))
    print("-" * 54)
    print("%-4s %-24s %5d %6d %10s" % ("", "TOTAL", tot_i, tot_t,
                                       "{:,}".format(int(round(tot_c)))))
    return tot_c


# The library's own suggested order (section 23), which is not the category
# order: it front-loads what the game is actually played with.
PHASES = [
    ("1. Combat & locomotion core",
     "The game is played here. Cat 02 universal verbs + the shipped spear + "
     "the longsword class + the 4 implemented enemies + live surfaces.",
     lambda e: e["sect"] in ("02", "03") or (e["sect"] == "01" and e["id"].startswith(("step_", "jumpland_")))),
    ("2. Voxel & tools",
     "Cat 04 for the wired materials, then outward as materials are wired.",
     lambda e: e["sect"] == "04"),
    ("3. Camp, weather, water basics",
     "Cat 09, the clear/rain/wind of 07, the swim/splash core of 08.",
     lambda e: e["sect"] in ("07", "08", "09")),
    ("4. Region beds",
     "Cat 10, prioritised by Act I locations. The G1 magic ambient beds ride "
     "here too, since they are environmental.",
     lambda e: e["sect"] in ("10", "11") or (e["sect"] == "19" and e["loop"] == "Y")),
    ("5. Systems & UI",
     "Cat 12-18, lockpicking gaps and mini-game extras first.",
     lambda e: e["sect"] in ("12", "13", "14", "15", "16", "17", "18")),
    ("6. Long tail",
     "Remaining locomotion foley, items, crafting, and the roadmap magic set.",
     lambda e: True),
]


def phases(rows):
    print("\nRENDER ORDER (docs/sfx-library.md section 23)")
    left = list(rows)
    running = 0.0
    for name, why, pred in PHASES:
        take = [e for e in left if pred(e)]
        left = [e for e in left if e not in take]
        if not take:
            continue
        cr = sum(cost(e) for e in take)
        running += cr
        print("\n%s" % name)
        print("   %s" % why)
        print("   %d ids, %d takes, %s credits (cumulative %s)"
              % (len(take), sum(e["var"] for e in take),
                 "{:,}".format(int(round(cr))), "{:,}".format(int(round(running)))))


def main():
    rows = sfx_spec.load()

    if "--rendered" in sys.argv:
        print("ALREADY RENDERED, in the Godot repo only:")
        print("  %d ids / %d takes / 25.1 MB of .mp3" % (RENDERED_IDS, RENDERED_TAKES))
        print("  locomotion 287, voxel 133, environment 122, ui 6")
        print("  Combat (Cat 02) is the one family never rendered.")
        print("\nNone of it is in voxelsim. Porting is free in credits and costs")
        print("25 MB in git; rendering it again would cost roughly:")
        print("  %s credits" % "{:,}".format(int(round(
            sum(cost(e) for e in rows if e["sect"] in ("01", "04", "07", "08", "09"))))))
        print("\nThe takes are unpruned and the in-game verdict was 'rough'.")
        print("Prune before porting, not after -- see docs/sfx-prompts.md section 8b.")
        return 0

    total = table(rows, "FULL LIBRARY")
    print("\nAt the measured rate (~8.2 cr/s, ~19 cr/gen) this lands near %s."
          % "{:,}".format(int(round(total * 8.2 / 9.0 * 0.97))))

    if "--phases" in sys.argv:
        phases(rows)
    else:
        print("\nRun with --phases for the library's suggested render order,")
        print("or --rendered for what already exists.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
