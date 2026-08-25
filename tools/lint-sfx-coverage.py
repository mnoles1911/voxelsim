#!/usr/bin/env python3
"""Check that docs/sfx-library.md and docs/sfx-prompts.md agree, 1:1.

The two documents are different granularities on purpose -- the library is a
spec written as matrices, the prompts doc is one row per file id. That makes
"does every entry have a prompt?" a question nobody can answer by reading, and
an unanswerable question is how a join rots.

tools/sfx_spec.py expands the library's matrices into the canonical id list.
This script compares that list against the ids actually present in the prompts
doc and fails if either side has an id the other does not.

    MISSING PROMPT  -- the library asks for a sound with no prompt to render it.
    ORPHAN PROMPT   -- the prompts doc has a row for a sound the library does
                       not list. Usually a typo in an id; occasionally a real
                       sound somebody added without updating the library.

Exit 0 clean, 1 on any mismatch. Run it after editing either document.
"""

from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROMPTS = os.path.join(ROOT, "docs", "sfx-prompts.md")

sys.path.insert(0, HERE)
import sfx_spec  # noqa: E402

ID_RE = re.compile(r"^[a-z][a-z0-9_]{3,}$")


def prompt_ids(path):
    """Every id in the prompts doc, as {id: line number}."""
    with open(path, encoding="utf-8") as fh:
        return prompt_ids_from_text(fh.read())


def prompt_ids_from_text(text):
    """Every id in prompts-doc text, as {id: line number}.

    A row is `| id | prompt | dur | infl | loop | var | bus |`. The header row
    and the |---| separator are skipped by the id pattern, which requires a
    lowercase snake_case token.
    """
    found = {}
    for n, line in enumerate(text.splitlines(), 1):
        if not line.startswith("|"):
            continue
        cells = [c.strip().strip("`*") for c in line.strip().strip("|").split("|")]
        if len(cells) < 3 or not ID_RE.match(cells[0]):
            continue
        if cells[0] in found:
            print("DUPLICATE PROMPT ROW: %s (lines %d and %d)"
                  % (cells[0], found[cells[0]], n))
        found[cells[0]] = n
    return found


def main():
    spec = sfx_spec.load()
    spec_ids = {e["id"]: e for e in spec}
    doc_ids = prompt_ids(PROMPTS)

    # An aliased library id is satisfied by its rendered name, and that rendered
    # name is not an orphan. Both directions, or the alias only half-works.
    alias = {k: v for k, v in sfx_spec.ALIASES.items() if k in spec_ids}
    satisfied = set(doc_ids) | {k for k, v in alias.items() if v in doc_ids}

    missing = sorted(set(spec_ids) - satisfied)
    orphan = sorted(set(doc_ids) - set(spec_ids)
                    - set(alias.values()) - set(sfx_spec.COARSE_STANDINS))

    print("library spec : %4d ids (%d files at declared var)"
          % (len(spec_ids), sum(e["var"] for e in spec)))
    print("prompts doc  : %4d ids" % len(doc_ids))

    if missing:
        print("\n%d IDS WITH NO PROMPT:" % len(missing))
        for i in missing[:40]:
            print("  %-40s (library cat %s)" % (i, spec_ids[i]["sect"]))
        if len(missing) > 40:
            print("  ... and %d more" % (len(missing) - 40))
    if orphan:
        print("\n%d PROMPT ROWS WITH NO LIBRARY ENTRY:" % len(orphan))
        for i in orphan[:40]:
            print("  %-40s (prompts doc line %d)" % (i, doc_ids[i]))
        if len(orphan) > 40:
            print("  ... and %d more" % (len(orphan) - 40))

    # Printed EVERY run, pass or fail. The drift is tolerated, not resolved,
    # and an untracked tolerance becomes permanent by default.
    live_alias = {k: v for k, v in alias.items() if v in doc_ids}
    if live_alias:
        print("\n%d IDS RENDERED UNDER A DIFFERENT NAME THAN THE LIBRARY GIVES:"
              % len(live_alias))
        for k in sorted(live_alias):
            print("  library %-32s rendered as %s" % (k, live_alias[k]))
        print("  (see ALIASES in tools/sfx_spec.py for why this is not silently fixed)")
    if sfx_spec.COARSE_STANDINS:
        print("\n%d COARSE STAND-IN(S) ON DISK:" % len(sfx_spec.COARSE_STANDINS))
        for k, why in sorted(sfx_spec.COARSE_STANDINS.items()):
            print("  %-24s %s" % (k, why))

    if missing or orphan:
        print("\nFAIL: the library and the prompts doc do not match 1:1.")
        print("Fix by adding the prompt row, or by correcting tools/sfx_families/.")
        return 1
    print("\nOK: every library id has exactly one prompt, and vice versa.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
