#!/usr/bin/env python3
"""Emit prompt rows for every library id the prompts doc does not have yet.

Writes into docs/sfx-prompts.md, immediately before its status section, inside
a pair of marker comments. Everything between the markers is regenerated on
each run; everything outside is never touched.

THE RULE THAT MATTERS: rows already in the doc are NEVER rewritten. 108 ids
have audio rendered from their exact prompt text, and changing that text would
silently break reproducibility -- a re-render would no longer reproduce the
file on disk. The generator only ADDS.

    python tools/gen-sfx-prompts.py            # write
    python tools/gen-sfx-prompts.py --check    # fail if writing would change
"""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROMPTS = os.path.join(ROOT, "docs", "sfx-prompts.md")

sys.path.insert(0, HERE)
import sfx_spec  # noqa: E402
from importlib.machinery import SourceFileLoader  # noqa: E402

lint = SourceFileLoader("lint", os.path.join(HERE, "lint-sfx-coverage.py")).load_module()

BEGIN = "<!-- BEGIN GENERATED PROMPTS -- edit tools/sfx_families/, not this block -->"
END = "<!-- END GENERATED PROMPTS -->"
ANCHOR = "## 8. Generation progress, spend & remaining work"

CATEGORY_TITLES = {
    "01": "Category 01 -- Locomotion (remaining surfaces, armour, traversal, breath)",
    "02": "Category 02 -- Combat: Player (universal, weapon matrix, throwables)",
    "03": "Category 03 -- Combat: Impacts & Enemies",
    "04": "Category 04 -- Tools & Voxel Interaction (remaining materials)",
    "05": "Category 05 -- Crafting & Stations",
    "06": "Category 06 -- Interactive Objects & Items",
    "07": "Category 07 -- Environment: Weather (full grid)",
    "08": "Category 08 -- Environment: Water (remaining)",
    "09": "Category 09 -- Fire & Camp (remaining)",
    "10": "Category 10 -- Region Ambient Beds",
    "11": "Category 11 -- Day/Night & Time Cues",
    "12": "Category 12 -- Systems: Lockpicking",
    "13": "Category 13 -- Systems: Mini-Games",
    "14": "Category 14 -- Investigation & Clue",
    "15": "Category 15 -- UI, Menu & Feedback",
    "16": "Category 16 -- Death & Respawn",
    "17": "Category 17 -- NPC Non-Verbal & Crowd",
    "18": "Category 18 -- Economy & Vendor",
    "19": "Category 19 -- Magic & Spellcraft",
}

PREAMBLE = """
## 7c. The rest of the library, generated

Every remaining id in `docs/sfx-library.md`, one row each, so the two documents
match 1:1. `tools/lint-sfx-coverage.py` proves it and fails if either side
drifts.

**These rows are generated from `tools/sfx_families/*.py`. Edit those, not
this block** -- anything typed between the markers is overwritten on the next
`python tools/gen-sfx-prompts.py`. The hand-authored Phase 1 and 2 sections
above are outside the markers and are never touched, because 108 of their ids
have audio rendered from that exact text.

Read section 8 before rendering any of this. The lesson already paid for is
that the 548 takes on disk sound rough because nothing was pruned, not because
there were too few of them -- so curate as you go rather than generating the
whole set and sorting it out later.
"""


def rows_for(entries):
    out = ["| id | prompt | dur | infl | loop | var | bus |",
           "|---|---|---|---|---|---|---|"]
    for e in entries:
        dur = e["dur"]
        dur = int(dur) if float(dur) == int(dur) else dur
        out.append("| %s | %s | %s | %s | %s | %d | %s |"
                   % (e["id"], e["prompt"], dur, e["infl"], e["loop"],
                      e["var"], e["bus"]))
    return out


def hand_authored(doc):
    """The doc with the generated block removed.

    THIS IS NOT A TIDINESS DETAIL. If "what does the doc already have?" is
    answered from the whole file, then on the second run every generated row
    counts as already-present, `todo` comes back empty, and the generator
    replaces its own output with nothing. That happened once; hence this.
    """
    if BEGIN not in doc:
        return doc
    head, rest = doc.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    return head + tail


def build_block(doc):
    spec = sfx_spec.load()
    have = set(lint.prompt_ids_from_text(hand_authored(doc)))
    have |= {k for k, v in sfx_spec.ALIASES.items() if v in have}
    todo = [e for e in spec if e["id"] not in have]

    lines = [BEGIN, PREAMBLE.rstrip(), ""]
    by_cat = {}
    for e in todo:
        by_cat.setdefault(e["sect"], []).append(e)

    total = 0
    for cat in sorted(by_cat):
        entries = by_cat[cat]
        total += len(entries)
        lines.append("### %s" % CATEGORY_TITLES.get(cat, "Category %s" % cat))
        lines.append("")
        lines.append("%d ids, %d files at the declared variation counts."
                     % (len(entries), sum(x["var"] for x in entries)))
        lines.append("")
        lines.extend(rows_for(entries))
        lines.append("")
        lines.append("---")
        lines.append("")
    lines.append(END)
    return "\n".join(lines) + "\n\n", total


def main():
    with open(PROMPTS, encoding="utf-8") as fh:
        doc = fh.read()
    block, total = build_block(doc)

    # Both paths must produce byte-identical output for the same spec, or
    # --check reports a permanent false "out of date" and stops being usable
    # in CI. Hence the explicit "\n\n" join on each side rather than relying
    # on whatever whitespace the previous write happened to leave behind.
    body = block.rstrip("\n") + "\n\n"
    if BEGIN in doc:
        head, rest = doc.split(BEGIN, 1)
        _, tail = rest.split(END, 1)
        new = head + body + tail.lstrip("\n")
    else:
        if ANCHOR not in doc:
            raise SystemExit("gen-sfx-prompts: anchor %r not found in %s"
                             % (ANCHOR, PROMPTS))
        head, tail = doc.split(ANCHOR, 1)
        new = head + body + ANCHOR + tail

    if "--check" in sys.argv:
        if new != doc:
            print("gen-sfx-prompts: docs/sfx-prompts.md is out of date "
                  "(run tools/gen-sfx-prompts.py)")
            return 1
        print("gen-sfx-prompts: up to date (%d generated rows)" % total)
        return 0

    if new == doc:
        print("gen-sfx-prompts: no change (%d generated rows)" % total)
        return 0
    with open(PROMPTS, "w", encoding="utf-8", newline="") as fh:
        fh.write(new)
    print("gen-sfx-prompts: wrote %d rows into %s" % (total, PROMPTS))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
