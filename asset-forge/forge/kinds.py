"""Asset kinds.

The world needs more than trees, and a rock has almost nothing in common with
an oak: no trunk, no crown, no growth model, no foliage. Rather than let one
parameter table sprawl across everything, each parameter declares which kinds
it applies to, and the app shows one kind at a time.

What every kind DOES share is the spine of the tool — a JSON spec, `(spec,
seed)` determinism, the health checks, the library, biome weights and placement
rules, the exports and the 3D viewer. Adding a kind means a generator and a
parameter group, not a second application.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Kind:
    key: str
    label: str
    blurb: str
    ready: bool          # is there a generator behind it yet?
    param_groups: tuple[str, ...]   # groups shown for this kind, beyond the shared ones


# Groups every kind carries: identity, where it goes in the world, and what it
# is made of.
SHARED_GROUPS = ("general", "biome", "placement", "variation", "materials")

KINDS: tuple[Kind, ...] = (
    Kind("tree", "Trees",
         "Woody plants with a trunk and a crown. Three growth models: "
         "colonize, whorl and frond.", True,
         ("trunk", "crown", "growth", "whorl", "frond", "roots", "strand", "foliage")),
    Kind("bush", "Bushes",
         "Low multi-stemmed woody scrub. Same growth machinery as a tree, "
         "authored short with branches to the ground.", True,
         ("trunk", "crown", "growth", "roots", "strand", "foliage")),
    Kind("rock", "Rocks",
         "Boulders and stones. No skeleton at all — an accretion of lumps, "
         "faceted and eroded, then part-buried.", True,
         ("rock",)),
    Kind("grass", "Grass", "Tufts of ground cover.", False, ()),
    Kind("reed", "Reeds", "Waterside stems.", False, ()),
    Kind("flower", "Flowers", "Small flowering ground plants.", False, ()),
)

BY_KEY = {k.key: k for k in KINDS}
READY = tuple(k for k in KINDS if k.ready)
KEYS = tuple(k.key for k in KINDS)


def groups_for(kind: str) -> tuple[str, ...]:
    k = BY_KEY.get(kind)
    return SHARED_GROUPS + (k.param_groups if k else ())


def applies(param_kinds: tuple[str, ...], kind: str) -> bool:
    """A parameter with no declared kinds applies to all of them."""
    return not param_kinds or kind in param_kinds
