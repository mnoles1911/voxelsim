"""What an asset IS, as opposed to which generator drew it.

THE DISTINCTION THIS FILE EXISTS TO MAKE. `kind` answers "which generator ran":
a skeleton, an accretion, a tuft, a lofted solid. `category` answers "what is
this thing in the game": scenery, an animal, an item a player can make. Those
two questions have had the same answer for every asset here so far, which is
exactly why the difference had never been written down -- and it is about to
stop being true. A torch and a canoe are the same generator and different
things; a rope and a vine could be the same generator and different things.

Three categories, and NOT ONE OF THEM IS NEW INFORMATION. Every boundary below
is a line the code already drew and never named:

  environment   `manifest.KINDS_ON_SCATTER` -- the six kinds the engine composes
                into terrain chunks and places by deterministic per-chunk
                scatter. Asserted equal, in manifest.py, so the two cannot
                drift.
  creature      the 382 animals. In the manifest (they have biome weights and
                are spawned from it) but NOT on the scatter, and README's own
                words: "authored and deliberately not rendered until animation
                exists".
  craftable     `lattice == "entity"` (ADR-0010) -- own pitch, own transform,
                never indexed by chunk coordinate, no bank, no biome. Asserted
                equal to `manifest.KINDS_ENTITY`.

WHY THE DEFAULT LIVES ON THE KIND AND THE OVERRIDE LIVES ON THE SPEC
--------------------------------------------------------------------
Both, deliberately, with one resolver -- the shape `biomes.allowed()` and
`spec.curation()` already have, so "what category is this" has exactly one
answer everywhere.

**The kind carries the default** because that is where it can be stated once
for 830 specs at zero risk. A per-spec field would have to be authored 830
times, and a field authored 830 times is a field wrong on some of them.

**The spec may override** because category is a statement about the OBJECT and
kind is a statement about the GENERATOR, and those provably diverge. Forcing
category onto the kind alone would mean a new kind per category -- a generator
per classification, which is precisely the mistake `grass`/`reed`/`flower`
avoided in the other direction (three kinds, one generator, because the
designer authors them separately and they go in different places).

**The override is NOT a parameter and must never become one.** It rides in the
spec beside `curation`, `biome_allow` and `biome_rules`, and like those three it
is excluded from `spec_hash` and `seed_hash` -- see `spec._hash_body`. That is
not tidiness, it is the arithmetic: `validate` starts from `default_spec()`, so
a row in `spec.PARAMS` puts a new key in ALL 830 canonical JSONs and moves both
hashes for every one of them. Every species would become a different individual
and every bank would read as stale. A classification must not redraw the thing
it classifies.

**Nothing in the library authors the override today**, and that is worth saying
plainly rather than leaving a reader to wonder: canoe and glider are craftable
because `artifact` is, which is the common case and the one that should stay
common. The first real user will be an item whose generator belongs to another
category -- a torch, a rope. The path is exercised by
`tools/artifactprobe.py --arms` so that it cannot rot while it is unused.

AN ILLEGIBLE CATEGORY RESOLVES TO NOTHING, LOUDLY
--------------------------------------------------
The one thing this must not do is fall back to the kind's default when a human
wrote something unreadable. That is the blossom-pink failure exactly: the
substituted value is what gets saved, the file is self-consistent from that
moment on, and no gate downstream can ever tell. So an unreadable value becomes
`ILLEGIBLE`, `of()` returns None, and every consumer refuses the species by name
-- the same route `biome_rules` takes for the same reason.
"""

from __future__ import annotations

from dataclasses import dataclass

from . import kinds as kindlib


@dataclass(frozen=True)
class Category:
    key: str
    label: str
    blurb: str
    kinds: tuple[str, ...]
    # Does the engine compose this into terrain chunks and scatter it per chunk?
    scattered: bool
    # Does it get a species record in the VXM manifest at all? Animals do --
    # they are spawned from it -- and craftables do not.
    in_manifest: bool


# The sentinel a spec's `category` block becomes when it cannot be read. Same
# device, and the same reasoning, as `biome_rules.__illegible__`.
ILLEGIBLE = "__illegible__"


CATEGORIES: tuple[Category, ...] = (
    Category(
        "environment", "Environment",
        "Static world content. The engine composes it into terrain chunks on "
        "both the CPU and GPU paths and the deterministic per-chunk scatter "
        "decides where it goes. Trees and rocks join the world voxel grid at "
        "10 cm and are destructible as terrain is; the rest carry their own "
        "finer grid and are drawn as instances in the detail ring.",
        ("tree", "bush", "rock", "grass", "reed", "flower"),
        scattered=True, in_manifest=True),
    Category(
        "creature", "Creatures",
        "Living things that are spawned rather than composed. They carry their "
        "own pitch under their own transform (ADR-0010), they are published in "
        "the manifest with biome weights so a spawner can find them, and they "
        "are authored and deliberately not rendered until animation exists.",
        ("fish", "cetacean", "bird", "quadruped"),
        scattered=False, in_manifest=True),
    Category(
        "craftable", "Craftable items",
        "Human-made objects a player can make, carry and use. Rigid, spawned, "
        "on their own pitch under their own transform, and outside world "
        "composition entirely: no bank, no biome weight, no manifest record. "
        "The boat and the glider are the first two.",
        ("artifact",),
        scattered=False, in_manifest=False),
)

BY_KEY = {c.key: c for c in CATEGORIES}
KEYS = tuple(c.key for c in CATEGORIES)
BY_KIND = {k: c.key for c in CATEGORIES for k in c.kinds}

# EVERY KIND HAS EXACTLY ONE CATEGORY, AND A NEW KIND FAILS AT IMPORT.
#
# Not "falls back to environment", which is the silent version of this and the
# one that would ship. A kind with no category would be classified by whichever
# default somebody typed, and the wrong answer would be indistinguishable from
# the right one everywhere it is read -- the app's grouping, the library index,
# and the game's list of craftables. The eleventh kind (`artifact`) is the
# reason this file exists; the twelfth should not be able to arrive without a
# decision.
_missing = [k.key for k in kindlib.KINDS if k.key not in BY_KIND]
assert not _missing, (
    f"forge/categories.py: kind(s) {_missing} have no category. Add each one to "
    f"exactly one entry in CATEGORIES -- a kind that is not classified is "
    f"invisible to the library index and to the game's craftable list, and "
    f"nothing else would report it.")
_dupes = [k for c in CATEGORIES for k in c.kinds
          if sum(k in c2.kinds for c2 in CATEGORIES) > 1]
assert not _dupes, (
    f"forge/categories.py: kind(s) {_dupes} are in more than one category")
_unknown = [k for k in BY_KIND if k not in kindlib.BY_KEY]
assert not _unknown, (
    f"forge/categories.py: CATEGORIES names kind(s) {_unknown} that "
    f"forge/kinds.py does not have")
del _missing, _dupes, _unknown


_ABSENT = object()


def of(spec: dict) -> "str | None":
    """THE ONE RESOLVER: which category this species is in.

    ABSENT means "take the kind's default", which is what all 830 specs in the
    library mean today. PRESENT-BUT-UNREADABLE means NOTHING, and returns None.

    The asymmetry is the whole point and it is the same one `spec.curation`
    draws: an absent verdict resolves the permissive way because that is what
    the library already was; an illegible one must not, because somebody wrote
    SOMETHING and nobody can tell what. Falling back to the kind here would be
    the blossom-pink failure exactly -- the wrong answer, indistinguishable from
    the right one, everywhere it is read.

    Deliberately strict about the TYPE as well, not just the value. `validate`
    normalises an unreadable block to `ILLEGIBLE` before it reaches disk, but
    this is also called on specs in flight from the app, and a `category` that
    arrives as a dict or a list must not quietly become the kind's default on
    the way through.
    """
    raw = spec.get("category", _ABSENT)
    if raw is _ABSENT:
        return BY_KIND.get(spec.get("kind"))
    if isinstance(raw, str) and raw in BY_KEY:
        return raw
    return None


def source_of(spec: dict) -> str:
    """Where `of` got its answer: "spec", "kind", "illegible" or "unknown".

    Exists because "craftable" and "craftable because somebody said so" are
    different facts, and both the library report and the app print them as two
    -- the same reason `spec.curation` distinguishes an approved species from a
    grandfathered one.
    """
    raw = spec.get("category", _ABSENT)
    if raw is not _ABSENT:
        return "spec" if isinstance(raw, str) and raw in BY_KEY else "illegible"
    return "kind" if spec.get("kind") in BY_KIND else "unknown"


def members(category: str, specs: "list[tuple[str, dict]]") -> list[str]:
    """Every species name in a category, sorted. THE QUERY SEAM.

    `categories.members("craftable", ...)` is the Python half of the answer the
    game will one day want; `library/categories.json`, written by
    `tools/export_categories.py`, is the half that does not need Python. Both go
    through `of` above, so a crafting system and the app cannot disagree about
    what is craftable.
    """
    if category not in BY_KEY:
        raise KeyError(f"no such category {category!r}; known: {KEYS}")
    return sorted(name for name, body in specs if of(body) == category)


def clean(raw, rep) -> "str | None":
    """Validate an authored `category` block. Returns what to store, or None.

    `rep` is a `spec.Report`. NAMES THE CONSEQUENCE rather than the rule,
    because the consequence is the part that decides whether anybody acts on it.
    """
    if isinstance(raw, str) and raw in BY_KEY:
        return raw
    rep.warnings.append(
        f"category: {raw!r} is not a category, so this species can be "
        f"classified as NOTHING -- it will be refused by name from "
        f"library/categories.json and will not appear in the game's list for "
        f"any category. Menu: {KEYS}. Remove the block to take the default for "
        f"its kind.")
    return ILLEGIBLE
