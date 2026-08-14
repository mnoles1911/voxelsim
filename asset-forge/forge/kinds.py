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
    lattice: str = "detail"         # "terrain" or "detail"; see below


# --- WHICH LATTICE A KIND LIVES ON (owner, 2026-08-13) -----------------------
#
# This is the one property that is not a matter of taste, so it is recorded on
# the kind rather than left to each spec.
#
# "terrain" -- rocks and trees. They JOIN THE WORLD'S OWN VOXEL GRID and are
# destructible exactly as terrain is. A destructible voxel has to be
# addressable in the world grid, a world grid has exactly one cell size, and
# that size is `vxc::kVoxelSizeMm` = 100 mm. So a terrain-lattice asset is
# authored at 10 cm and nothing else is legal. Authoring one finer bought a
# better preview and nothing more: `AssetGrid::at(lx, ly, lz)` takes plain
# integer voxel coordinates with no scale factor, and there is no resampling
# anywhere in voxel-core, so a 5 cm asset read through it would come out at
# twice its intended size.
#
# "detail" -- bushes, ground cover and every animal. They never enter the
# terrain grid: they carry their own voxel grid and their own transform, they
# are not destructible in the terrain sense, and their lattice is therefore
# free. They keep whatever size their own measurements chose -- 5 cm for ground
# cover, 1 to 10 cm for animals by the rule in
# `docs/marine-megafauna-research.md`, which is that a species is drawn at the
# coarsest voxel at which its smallest identifying feature is still about three
# voxels across.
#
# The library was briefly ALL 5 cm (`tools/all_to_5cm.py`) on the reasoning
# that one lattice for everything means nothing downstream has to ask which
# lattice an object is on. Right about the goal, wrong about the number: the
# lattice that matters is the one the world uses, and half the library cannot
# join it at any size. `tools/all_to_10cm.py` is the move back, and
# `forge.cli.selftest` refuses a terrain-lattice spec that is not at 10 cm.
TERRAIN_LATTICE_CM = 10.0


# Groups every kind carries: identity, where it goes in the world, and what it
# is made of.
SHARED_GROUPS = ("general", "biome", "placement", "variation", "materials")

KINDS: tuple[Kind, ...] = (
    Kind("tree", "Trees",
         "Woody plants with a trunk and a crown. Three growth models: "
         "colonize, whorl and frond.", True,
         ("trunk", "crown", "growth", "whorl", "frond", "roots", "strand", "foliage"),
         lattice="terrain"),
    Kind("bush", "Bushes",
         "Low multi-stemmed woody scrub. Same growth machinery as a tree, "
         "authored short with branches to the ground.", True,
         ("trunk", "crown", "growth", "roots", "strand", "foliage")),
    Kind("rock", "Rocks",
         "Boulders and stones. No skeleton at all — an accretion of lumps, "
         "faceted and eroded, then part-buried.", True,
         ("rock",), lattice="terrain"),
    # The next three are ONE generator with three settings. A grass tuft, a
    # stand of reeds and a clump of daisies are all a spray of thin stems from a
    # common root; what differs is height, how far they arc, and what tops them.
    # They stay separate kinds because the designer authors them separately and
    # they go in different places, not because the code differs.
    Kind("grass", "Grass",
         "Tufts of blades arcing out from a root crown. Authored at 5 cm — "
         "at the terrain's 10 cm a tuft is four voxels and there is nothing "
         "to draw.", True,
         ("tuft",)),
    Kind("reed", "Reeds",
         "Tall near-vertical waterside stems, most carrying a seed head.", True,
         ("tuft",)),
    Kind("flower", "Flowers",
         "Small flowering plants: a few leaf stems and a few carrying a bloom.",
         True,
         ("tuft",)),
    # The first animal. It is a kind and not a variation on anything above,
    # because it is the first asset here that does not stand on the ground: it
    # is placed IN water, it faces a direction, and it is not meant to persist.
    # The `detail` group is where that last part is written down.
    Kind("fish", "Fish",
         "Swimming detail entities, from a 20 cm minnow to a 9 m whale shark. "
         "A lofted body with thin fins, coloured by countershading plus one "
         "mark. Voxel size is per species: 1 cm for small fish, 5 cm for "
         "sharks.", True,
         ("fish", "detail")),
    # THE SAME GENERATOR AS `fish`, and a separate kind for the same reason
    # grass, reeds and flowers are three kinds behind one tuft generator: the
    # designer authors them separately, they go in different places, and each
    # section should show only the sliders it can use. A whale has no pelvic
    # fin, no anal fin and no barbels; a fish has no blowhole.
    #
    # What differs in the GEOMETRY is small, and it is four parameters: the
    # tail lies horizontally, the cross-section runs from a barrel at the
    # middle to a blade at the wrist, the flippers are long and narrow, and
    # there is a blowhole. That is not a second generator's worth of
    # difference, which is why it is not one. Contrast `bird` below, which IS
    # a second generator because a bird is jointed and a fish is not.
    Kind("cetacean", "Whales & dolphins",
         "Air-breathing marine mammals: a horizontal fluke, flippers, a "
         "blowhole, and no pelvic or anal fin. Voxel size is per species — a "
         "25 m whale is authored at 10 cm.", True,
         ("fish", "detail")),
    # The second animal, and the first JOINTED one. A fish is a single solid
    # whose cross-section changes along one axis; a bird is a body, a neck, a
    # head, a bill, a tail and two wings at angles to each other, and the
    # angles are most of what tells one from another. That is why it is a
    # separate generator rather than a fish with different sliders.
    Kind("bird", "Birds",
         "Small flying detail entities, drawn to read at 20-90 voxels long. "
         "A tilted body with a neck, a bill and a tail fan, wings either "
         "folded or spread, and three markings on three regions. Authored at "
         "1 cm.", True,
         ("bird", "flock")),
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
