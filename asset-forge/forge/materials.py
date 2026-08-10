"""Material IDs and their colours.

SLOTS 0-15 MIRROR `vxc::Material` in
`voxel-core/include/voxelcore/core.h:303-335` EXACTLY. That enum is documented
append-only ("never renumber an existing entry, it would invalidate every saved
edit log"), so these numbers are stable and this table only ever grows.

SLOTS 16+ ARE PROPOSED AND DO NOT EXIST IN THE ENGINE YET. Nothing here can be
stamped into the world until they are appended to `vxc::Material` for real, and
that append has three tails recorded in `docs/tree-asset-generator-research.md`
section 8A: mine costs in `VoxelAgentSubsystem.cpp:64` (which carries a
`static_assert` on the array length), the per-material majority arrays in
`mips.h:60-77`, and the golden-digest trap warned about in
`test_mesher.cpp:123-132`.

When that append lands, the numbers below must be reconciled with whatever the
engine actually chose. `forge.cli check-materials` prints this table next to the
enum so the two can be compared by eye.
"""

from __future__ import annotations

# --- engine materials (mirror of core.h) ------------------------------------

MAT_AIR = 0
MAT_BEDROCK = 1
MAT_ROCK = 2
MAT_GRAVEL = 3
MAT_SAND = 4
MAT_SUBSOIL = 5
MAT_TOPSOIL = 6
MAT_SNOW = 7
MAT_GRASS = 8
MAT_JUNGLE_SOIL = 9
MAT_SAVANNA_GRASS = 10
MAT_PODZOL = 11
MAT_PERMAFROST = 12
MAT_MUD = 13
MAT_CLAY = 14
MAT_WATERMARK = 15

ENGINE_MATERIAL_COUNT = 16  # vxc::kMaterialCount as of 2026-08-09

# --- proposed tree materials (NOT in the engine yet) ------------------------

MAT_BARK = 16
MAT_HEARTWOOD = 17
MAT_DEADWOOD = 18
MAT_LEAF_BROADLEAF = 19
MAT_LEAF_NEEDLE = 20
MAT_LEAF_JUNGLE = 21
MAT_LEAF_DRY = 22
MAT_BARK_PALE = 23      # birch, aspen: white/silver bark
MAT_LEAF_BLOSSOM = 24   # cherry and other flowering species
MAT_LEAF_AUTUMN = 25

FIRST_PROPOSED = 16

# Names used in species specs. Specs never carry raw integers, so if the engine
# picks different numbers only this file changes.
BY_NAME = {
    "air": MAT_AIR,
    "bark": MAT_BARK,
    "heartwood": MAT_HEARTWOOD,
    "deadwood": MAT_DEADWOOD,
    "leaf_broadleaf": MAT_LEAF_BROADLEAF,
    "leaf_needle": MAT_LEAF_NEEDLE,
    "leaf_jungle": MAT_LEAF_JUNGLE,
    "leaf_dry": MAT_LEAF_DRY,
    "bark_pale": MAT_BARK_PALE,
    "leaf_blossom": MAT_LEAF_BLOSSOM,
    "leaf_autumn": MAT_LEAF_AUTUMN,
}

NAME_BY_ID = {v: k for k, v in BY_NAME.items()}

WOOD_NAMES = ("bark", "bark_pale", "heartwood", "deadwood")
LEAF_NAMES = (
    "leaf_broadleaf",
    "leaf_needle",
    "leaf_jungle",
    "leaf_dry",
    "leaf_blossom",
    "leaf_autumn",
)

# Preview colours. These drive the thumbnails and the .vox palette; they are a
# stand-in for the engine's real shading, not a claim about it.
COLORS = {
    MAT_AIR: (0, 0, 0),
    MAT_BEDROCK: (72, 70, 74),
    MAT_ROCK: (108, 104, 100),
    MAT_GRAVEL: (140, 132, 118),
    MAT_SAND: (214, 192, 140),
    MAT_SUBSOIL: (120, 94, 66),
    MAT_TOPSOIL: (94, 70, 48),
    MAT_SNOW: (238, 242, 248),
    MAT_GRASS: (96, 140, 62),
    MAT_JUNGLE_SOIL: (66, 50, 34),
    MAT_SAVANNA_GRASS: (170, 152, 78),
    MAT_PODZOL: (86, 72, 60),
    MAT_PERMAFROST: (176, 184, 190),
    MAT_MUD: (74, 64, 52),
    MAT_CLAY: (150, 118, 96),
    MAT_WATERMARK: (255, 0, 255),
    MAT_BARK: (88, 66, 48),
    MAT_HEARTWOOD: (146, 112, 76),
    MAT_DEADWOOD: (134, 122, 104),
    MAT_LEAF_BROADLEAF: (78, 122, 54),
    MAT_LEAF_NEEDLE: (46, 84, 62),
    MAT_LEAF_JUNGLE: (58, 108, 48),
    MAT_LEAF_DRY: (146, 138, 74),
    MAT_BARK_PALE: (186, 184, 174),
    MAT_LEAF_BLOSSOM: (226, 168, 190),
    MAT_LEAF_AUTUMN: (192, 122, 46),
}

MAX_ID = max(COLORS)


def color(mat: int) -> tuple[int, int, int]:
    return COLORS.get(mat, (255, 0, 255))


def resolve(name: str) -> int:
    """Spec material name -> id. Raises on an unknown name rather than
    silently substituting, because a silent substitution would show up as a
    wrong-coloured tree hundreds of assets later."""
    try:
        return BY_NAME[name]
    except KeyError:
        raise KeyError(
            f"unknown material {name!r}; known: {', '.join(sorted(BY_NAME))}"
        ) from None
