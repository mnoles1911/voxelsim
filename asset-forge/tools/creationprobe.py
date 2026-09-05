"""Prove the creation grammar wrong, or fail trying (plan P4, step 1).

`language.create` turns a sentence into a new species. The two failures this
probe exists to catch are the two the plan names:

  * A DEAD RECIPE (the `fishprobe` precedent): a creation fixture that
    produces the kind's `default_spec()` unchanged is a failure -- the
    descriptors were swallowed and the user got a stock asset wearing their
    words as a name.
  * A SILENT DEFAULT: nonsense input must come back with a non-empty
    `ignored` list, no kind and no spec -- never a quietly-materialised
    default species.

    python tools/creationprobe.py
"""
import json
import sys

import _path  # noqa: F401  (sys.path bootstrap)
from forge import language, spec as sm


def main() -> int:
    ok = True

    # 1. Every KIND_WORDS entry lands on its kind, deterministically.
    for word, kind in sorted(language.KIND_WORDS.items()):
        r = language.create(f"a {word}")
        good = r["kind"] == kind and r["spec"] is not None and r["name"]
        if not good:
            print(f"  ! '{word}' -> kind {r['kind']!r} (wanted {kind!r})")
            ok = False
        r2 = language.create(f"a {word}")
        if json.dumps(r["spec"], sort_keys=True) != json.dumps(r2["spec"], sort_keys=True):
            print(f"  ! '{word}' is not deterministic")
            ok = False
    print(f"  {len(language.KIND_WORDS)} kind words resolve, deterministically")

    # 2. THE DEAD-RECIPE ARM: one descriptor fixture per kind must move at
    #    least one measurable off the defaults. A fixture that stops moving
    #    anything means the creation path detached from the edit vocabulary.
    fixtures = {
        "tree": "a gnarled dead willow for tundra",
        "bush": "a very tall sparse bush",
        "rock": "a much taller rock",
        "grass": "much taller grass",
        "reed": "much taller reeds",
        "flower": "a much taller flower",
        "fish": "a small fish",
        "cetacean": "a small dolphin",
        "bird": "a small bird",
        "quadruped": "a small animal",
        "artifact": "a small craft",
    }
    for kind, sentence in sorted(fixtures.items()):
        r = language.create(sentence)
        if r["kind"] != kind:
            print(f"  ! fixture {sentence!r} resolved kind {r['kind']!r}, wanted {kind!r}")
            ok = False
            continue
        if not r["edits"]:
            print(f"  ! DEAD recipe: {sentence!r} produced default_spec() "
                  f"unchanged (understood: {r['understood']})")
            ok = False
        else:
            moved = ", ".join(e["path"] for e in r["edits"][:4])
            print(f"  {kind:<10} {len(r['edits'])} edits ({moved}"
                  + (", ..." if len(r["edits"]) > 4 else "") + ")")

    # 3. New species arrive DRAFT (the /api/import law: a brand-new spec has
    #    never been looked at), and validate() must carry the block through.
    r = language.create("a gnarled dead willow for tundra")
    cur = sm.curation(r["spec"])
    # `curated` reads True for ANY present block (it means "not
    # grandfathered", the same as an imported species) -- the gate that
    # matters is the status: draft does not export.
    if cur["status"] != "draft":
        print(f"  ! created species is not draft: {cur}")
        ok = False
    revalidated, _ = sm.validate(r["spec"])
    if sm.curation(revalidated)["status"] != "draft":
        print("  ! the draft verdict does not survive validate()")
        ok = False
    print("  created species arrive draft, and the verdict survives validate()")

    # 4. THE NONSENSE ARM: unknown input says so; nothing materialises.
    r = language.create("fluffy wuzzle blorp")
    fired = bool(r["kind"] is None and r["spec"] is None and not r["edits"]
                 and r["ignored"] and r["warnings"])
    print(("  fires" if fired else "  ! SILENT") +
          ": nonsense returns no species, names its ignored words "
          f"({', '.join(r['ignored'])})")
    ok &= fired

    # 5. A sentence with a kind but unknown descriptors REPORTS them.
    r = language.create("a zorblatt encrusted tree")
    if "zorblatt" not in r["ignored"]:
        print(f"  ! unknown descriptor not reported: ignored={r['ignored']}")
        ok = False
    else:
        print("  fires: unknown descriptors are reported, not swallowed")

    # 6. THE SUB-CATEGORY ARMS (owner directive 2026-09-05): the grouping
    #    label must move NEITHER hash (it is a statement about what the
    #    species IS), must survive revalidation, and an unreadable label
    #    must warn with the consequence and drop -- never half-store.
    base, _ = sm.load(
        __import__("pathlib").Path(__file__).resolve().parents[1]
        / "specs" / "brown-trout.json")
    tag = dict(base); tag["subcategory"] = "Eels "
    tag, trep = sm.validate(tag)
    neutral = (tag.get("subcategory") == "eels" and not trep.warnings
               and sm.spec_hash(tag) == sm.spec_hash(base)
               and sm.seed_hash(tag) == sm.seed_hash(base))
    print(("  fires" if neutral else "  ! SILENT")
          + ": a subcategory label cleans to a slug and moves NEITHER hash")
    ok &= neutral
    again, _ = sm.validate(tag)
    survives = again.get("subcategory") == "eels"
    print(("  fires" if survives else "  ! SILENT")
          + ": the label survives revalidation")
    ok &= survives
    bad = dict(base); bad["subcategory"] = "Not A Slug!!"
    bad, brep = sm.validate(bad)
    refused = ("subcategory" not in bad
               and any("subcategory" in w for w in brep.warnings))
    print(("  fires" if refused else "  ! SILENT")
          + ": an unreadable label is DROPPED with the consequence named")
    ok &= refused

    print("creationprobe:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
