"""Material IDs and their colours.

SLOTS 0-15 MIRROR `vxc::Material` in
`voxel-core/include/voxelcore/core.h:303-335` EXACTLY. That enum is documented
append-only ("never renumber an existing entry, it would invalidate every saved
edit log"), so these numbers are stable and this table only ever grows.

SLOTS 16-46 WERE PROPOSED HERE FIRST AND HAVE SINCE BEEN APPENDED to
`vxc::Material` for real -- the ten wood and leaf materials in 2026-08, the ten
creature skins on 2026-08-13, the eleven plumage materials the same day.
`ENGINE_MATERIAL_COUNT` below is read from the
GENERATED `forge/palette.py`, so this file can no longer disagree with the
engine about which ids exist; `forge.cli materials` prints the table with each
id's status.

An append has five tails, and every one of them is guarded rather than
remembered: mine costs in `VoxelAgentSubsystem.cpp:64` (a `static_assert` on the
array length), the count assertion in `test_assetgrid.cpp`, the positional
palette table in `materialpalette.h` (order checked against the enum by
`tools/gen_palette.py`), `ue-project/Tools/terrain_palette.py` (which refuses to
generate until a BIOME_TINT decision exists for each new row), and the two
generated copies -- `forge/palette.py` and `VoxelMaterialPalette.ush` -- each
with a check that fails if it was not regenerated. The per-material majority
arrays in `mips.h:60-77` size themselves, and the golden digests in
`test_mesher.cpp:123-132` are deliberately pinned to a literal so that appending
cannot silently invalidate them.

Anything ADDED here from now on is proposed again until it is in the header.
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

# HOW MANY MATERIALS THE ENGINE ACTUALLY HAS, read from the generated palette
# rather than typed here.
#
# This was a hand-written `16` with the comment "vxc::kMaterialCount as of
# 2026-08-09", and it was wrong twice inside a week: once when the ten tree
# materials were appended to core.h, and again when the ten skin materials
# were. Both times `forge.cli materials` went on reporting materials the engine
# had as "PROPOSED, not in engine" -- a hand-maintained mirror of a number that
# lives somewhere else, which is this repo's documented failure mode.
#
# `forge/palette.py` is GENERATED from the engine header by
# `tools/gen_palette.py`, and `forge.cli selftest` fails if it has drifted. So
# reading the count from there cannot go stale without something shouting.
from .palette import MATERIAL_COUNT as ENGINE_MATERIAL_COUNT  # noqa: E402

# --- tree materials ---------------------------------------------------------
#
# APPENDED TO THE ENGINE, 2026-08. These were proposed here first and baked
# into .vxa files before vxc::Material had them, which is why core.h's own
# comment says "the numbers are not free choices".

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

# --- creature-skin materials ------------------------------------------------
#
# APPENDED TO THE ENGINE, 2026-08-13, after owner approval. Proposed here
# first, exactly as the tree materials were; see
# docs/fish-colour-proposal.md for the table and what the append touched.
#
# TEN COLOURS, FOR EVERY ANIMAL, NOT TEN FOR FISH. Fish are the first animal
# here and they will not be the last, so these are named for what they LOOK
# like rather than for what wears them: a frog is `skin_green`, a trout's back
# is `skin_olive`, a clownfish and a goldfish are both `skin_orange`.
#
# Ten is the number that came out of trying to author the species set with
# fewer. Six terrain materials are already close enough to a fish to be worth
# listing, and every one of them is wrong in a way that shows: MAT_SAND is a
# yellow-grey with sandstone's patch mottle, MAT_SNOW has no jitter at all so a
# white belly reads as a printed decal, MAT_LEAF_AUTUMN is the only orange in
# the engine and it is a brown-orange with a leaf's very high per-voxel jitter,
# which on a twelve-voxel flank is static. See `docs/fish-shape-research.md`
# and the colour proposal in the handover for what the append costs.
#
# The four numbers each of these needs in the engine's palette table are the
# same four every material has: per-voxel lightness jitter, per-voxel warm/cool
# tilt, patch strength and patch wavelength. Fish want LOW jitter and LOW patch
# strength -- an animal is one smooth creature, not a granular surface -- which
# is the opposite end of both ranges from foliage and gravel.
MAT_SKIN_DARK = 26      # near-black: eyes, stripes, bars, the top of a back
MAT_SKIN_PALE = 27      # off-white belly, the pale half of countershading
MAT_SKIN_SILVER = 28    # bright silver flank; the commonest fish colour there is
MAT_SKIN_OLIVE = 29     # olive-green back: trout, perch, pike
MAT_SKIN_BROWN = 30     # warm brown: bottom fish, speckled backs
MAT_SKIN_ORANGE = 31    # clownfish, goldfish, koi
MAT_SKIN_YELLOW = 32    # reef yellow
MAT_SKIN_RED = 33       # red fins, rudd, snapper
MAT_SKIN_BLUE = 34      # reef blue and the blue-green of open water
MAT_SKIN_GREEN = 35     # bright green: wrasse, weed-bed fish

# --- plumage materials ------------------------------------------------------
#
# APPENDED TO THE ENGINE, 2026-08-13, after owner approval ("I accept all bird
# colours"). Proposed here first, exactly as the tree and skin materials were;
# see `docs/bird-colour-proposal.md` for the table, the contrast measurements
# behind each entry and every file the append touched.
#
# ELEVEN, AND THE BRIEF IS "COLOURFUL AND STYLISED". That is the owner's
# wording and it decided the size of this set. A field-guide-accurate library
# would need three of these; a library where a raven, a pigeon and a sparrow
# can sit on a sheet next to a kingfisher without reading as three grey-brown
# lozenges and one bird needs eleven.
#
# The ten skin materials already carry black, off-white, silver, olive, brown,
# orange, yellow, red, blue and green, and every species in the bird library
# authors most of itself out of them. These eleven are what that set cannot do.
# Four are the NEUTRALS birds need and fish did not; six are SATURATED HUES the
# palette has no equivalent for; one is keratin.
#
# The neutrals:
#
#   white     `skin_pale` (232,226,212) is a fish belly -- cream, and warm. A
#             gull, an egret and a winter ptarmigan are white. Against
#             `plume_grey` the true white measures a WCAG contrast ratio of
#             2.55; `skin_pale` against the same grey measures 2.14. The
#             difference decides whether a gull's mantle separates from its
#             own head at twenty voxels.
#   grey      `skin_silver` is the closest and it is the ONE entry in the skin
#             table whose faces are deliberately inverted -- its sides are
#             brighter than its top, because a fish flank is a mirror. On a
#             bird's back that lights the wrong surface: the back is what the
#             sun reaches and the flanks are what sits in shadow.
#   slate     nothing in the palette is a dark COOL grey. The nearest is
#             `bedrock` (68,68,74), which is terrain: patch strength 30 over a
#             22-voxel wavelength, so a heron's back would carry one slow
#             mottle across the whole animal.
#   buff      `sand` (214,192,140) is right in hue and is sandstone -- the
#             granular material the desert rocks are made of, at the top of the
#             jitter range.
#
# The hues, and why each is a colour rather than an adjustment of one already
# here. Every pair below is quoted as its WCAG contrast ratio, because that is
# the number `tools/birdprobe.py --read` gates on and the reason value
# separation, not hue, is what makes a marking read at this size:
#
#   rufous    the gap between `skin_orange` (226,118,34) and `skin_brown`
#             (110,82,52) is where half the small birds in the world live: a
#             robin's breast, a kestrel's back, a swallow's throat, a jay's
#             body, a kingfisher's underparts. Authored in orange they read as
#             plastic toys; in brown they vanish into the branch.
#   crimson   `skin_red` (170,46,40) is a dark brick and it is the right red
#             for a fin. A macaw and a woodpecker's nape are a bright red, and
#             the number that matters is what they sit NEXT to: against
#             `skin_dark`, which is a macaw's face and a woodpecker's back,
#             `plume_crimson` measures 2.53 and `skin_red` measures 1.96 --
#             below the 2.0 floor `tools/birdprobe.py --read` gates on, so the
#             brick red merges into the black and the bird loses its red.
#   lime      `skin_green` (58,148,92) is a mid forest green at relative
#             luminance 0.229. A parrot, a greenfinch and a bee-eater are a
#             yellow-green at 0.494 -- 2.2x as bright, and the only green in
#             the set light enough to carry a dark marking on top of it.
#   cyan      `skin_blue` (46,96,168) is a royal blue at luminance 0.118. A
#             kingfisher, a roller and a macaw's wing are electric turquoise at
#             0.395 -- a different hue and 3.3x the brightness. This is the most
#             stylised entry here and the one the kingfisher exists for.
#   lilac     the palette has NO violet at all. `leaf_blossom` (226,168,190) is
#             a pale pink for cherry trees. A jay's wing coverts, a roller's
#             breast and the purple half of a starling's gloss have nowhere
#             else to go; against `plume_slate` it measures 2.21. THIS IS THE
#             FIRST ENTRY TO CUT if eleven is too many -- see the proposal.
#   irides.   starling, mallard drake, magpie, raven gloss. A saturated dark
#             teal-green is not `skin_green` darkened -- it is a different
#             colour, and on a starling it is the single most recognisable
#             thing about the animal at any size.
#
# And the keratin:
#
#   horn      a bill and a pair of legs are two to six voxels and they are a
#             different SUBSTANCE from feather. Every field guide prints bill
#             and leg colour because it is diagnostic; drawing them in the head
#             colour throws that away for nothing. Yellow and orange bills use
#             `skin_yellow` and `skin_orange`, so this is only the grey one.
#
# Same four appearance numbers as everything else, and the same end of every
# range the skins asked for: low jitter, low hue tilt, low patch strength,
# short patch wavelength. A bird is one smooth creature, not a granular
# surface.
MAT_PLUME_WHITE = 36    # true white: gull, egret, ptarmigan, wing bars, rumps
MAT_PLUME_GREY = 37     # neutral mid grey: pigeon, gull mantle, tit, dove
MAT_PLUME_SLATE = 38    # dark cool grey: heron, jay wing, falcon back
MAT_PLUME_BUFF = 39     # sandy tan: larks, gamebirds, hoopoe, female duck
MAT_PLUME_RUFOUS = 40   # chestnut: robin breast, kestrel back, swallow throat
MAT_PLUME_CRIMSON = 41  # bright scarlet: macaw, woodpecker nape, cardinal
MAT_PLUME_LIME = 42     # bright yellow-green: parrot, greenfinch, bee-eater
MAT_PLUME_CYAN = 43     # electric turquoise: kingfisher, roller, macaw wing
MAT_PLUME_LILAC = 44    # violet: jay covert, roller breast, purple gloss
MAT_PLUME_IRIDESCENT = 45  # saturated dark teal: starling, mallard head
MAT_BEAK_HORN = 46      # grey keratin: bills, legs and feet

# The first id the engine does NOT have. Derived, for the same reason the count
# above is: a literal here has already been stale twice.
FIRST_PROPOSED = ENGINE_MATERIAL_COUNT

# Names used in species specs. Specs never carry raw integers, so if the engine
# picks different numbers only this file changes.
#
# Terrain materials the ENGINE ALREADY HAS. Worth stating plainly, because it
# changes what can ship first: a rock is made of the same rock, gravel and sand
# the terrain is made of, and a grass tuft or a reed is made of MAT_GRASS and
# MAT_SAVANNA_GRASS. Neither needs a single new material. They still need the
# streaming bound to accept solids above the surface, but that is one blocker
# instead of two, and it is the only blocker between them and the world.
#
# Trees, bushes and flowers are the ones that need the append: bark, the leaf
# variants, and a bloom colour have no equivalent in the terrain palette.
BY_NAME = {
    "air": MAT_AIR,
    "bedrock": MAT_BEDROCK,
    "rock": MAT_ROCK,
    "gravel": MAT_GRAVEL,
    "sand": MAT_SAND,
    "subsoil": MAT_SUBSOIL,
    "topsoil": MAT_TOPSOIL,
    "snow": MAT_SNOW,
    "grass": MAT_GRASS,
    "jungle_soil": MAT_JUNGLE_SOIL,
    "savanna_grass": MAT_SAVANNA_GRASS,
    "podzol": MAT_PODZOL,
    "permafrost": MAT_PERMAFROST,
    "mud": MAT_MUD,
    "clay": MAT_CLAY,
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
    "skin_dark": MAT_SKIN_DARK,
    "skin_pale": MAT_SKIN_PALE,
    "skin_silver": MAT_SKIN_SILVER,
    "skin_olive": MAT_SKIN_OLIVE,
    "skin_brown": MAT_SKIN_BROWN,
    "skin_orange": MAT_SKIN_ORANGE,
    "skin_yellow": MAT_SKIN_YELLOW,
    "skin_red": MAT_SKIN_RED,
    "skin_blue": MAT_SKIN_BLUE,
    "skin_green": MAT_SKIN_GREEN,
    "plume_white": MAT_PLUME_WHITE,
    "plume_grey": MAT_PLUME_GREY,
    "plume_slate": MAT_PLUME_SLATE,
    "plume_buff": MAT_PLUME_BUFF,
    "plume_rufous": MAT_PLUME_RUFOUS,
    "plume_crimson": MAT_PLUME_CRIMSON,
    "plume_lime": MAT_PLUME_LIME,
    "plume_cyan": MAT_PLUME_CYAN,
    "plume_lilac": MAT_PLUME_LILAC,
    "plume_iridescent": MAT_PLUME_IRIDESCENT,
    "beak_horn": MAT_BEAK_HORN,
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
SKIN_NAMES = (
    "skin_dark",
    "skin_pale",
    "skin_silver",
    "skin_olive",
    "skin_brown",
    "skin_orange",
    "skin_yellow",
    "skin_red",
    "skin_blue",
    "skin_green",
)
PLUME_NAMES = (
    "plume_white",
    "plume_grey",
    "plume_slate",
    "plume_buff",
    "plume_rufous",
    "plume_crimson",
    "plume_lime",
    "plume_cyan",
    "plume_lilac",
    "plume_iridescent",
    "beak_horn",
)
# What a bird spec may choose from. BOTH sets, on purpose: eighteen of the
# twenty species in the library author most of their plumage out of the fish
# skins, and only reach into the seven new entries for the colours listed
# above. Offering the bird sections a plumage-only menu would have forced a
# second black, a second yellow and a second red into the engine for no reason
# beyond tidiness of naming.
CREATURE_NAMES = SKIN_NAMES + PLUME_NAMES

# The same tuple under the name the bird rows have always used. Kept as an
# alias rather than a second literal, because two lists of twenty-one strings
# that have to stay equal is exactly the drift this file's own header warns
# about.
BIRD_NAMES = CREATURE_NAMES

# AND FISH MAY CHOOSE FROM IT TOO, from 2026-08-13, for the reason the comment
# above gives in the other direction. The eleven plumage entries are already in
# the engine -- appending them cost nothing further -- and the ten skins have no
# warm mid-tone at all. A common dolphin's thoracic patch, the forward half of
# its hourglass, is a tan-to-chestnut field sitting directly against a pale grey
# one, and what decides whether that reads at twenty voxels is VALUE contrast
# and not hue. Measured against `skin_silver`, which is the flank beside it:
#
#     plume_buff     1.05      <- the obvious choice, and invisible
#     skin_yellow    1.14
#     skin_orange    1.56
#     plume_rufous   2.52      <- what the common dolphin uses
#     skin_brown     3.65      <- reads, but as a dark blotch rather than a field
#
# `tools/fishprobe.py --read` gates at 1.5 and caught the buff at 1.05 on its
# first run after the hourglass was taught to count as a marking. Refusing a
# dolphin a colour on the grounds that its name says "plume" would be tidiness
# of naming beating the picture, which is the argument this file already
# rejected once in the other direction.
FISH_NAMES = CREATURE_NAMES

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
    MAT_SKIN_DARK: (46, 48, 56),
    MAT_SKIN_PALE: (232, 226, 212),
    MAT_SKIN_SILVER: (176, 186, 196),
    MAT_SKIN_OLIVE: (86, 96, 54),
    MAT_SKIN_BROWN: (110, 82, 52),
    MAT_SKIN_ORANGE: (226, 118, 34),
    MAT_SKIN_YELLOW: (232, 194, 54),
    MAT_SKIN_RED: (170, 46, 40),
    MAT_SKIN_BLUE: (46, 96, 168),
    MAT_SKIN_GREEN: (58, 148, 92),
    MAT_PLUME_WHITE: (246, 246, 242),
    MAT_PLUME_GREY: (150, 156, 164),
    MAT_PLUME_SLATE: (78, 92, 112),
    MAT_PLUME_BUFF: (208, 176, 118),
    MAT_PLUME_RUFOUS: (180, 84, 36),
    MAT_PLUME_CRIMSON: (208, 40, 56),
    MAT_PLUME_LIME: (152, 202, 70),
    MAT_PLUME_CYAN: (52, 184, 206),
    MAT_PLUME_LILAC: (166, 132, 208),
    MAT_PLUME_IRIDESCENT: (28, 78, 70),
    MAT_BEAK_HORN: (96, 88, 76),
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
