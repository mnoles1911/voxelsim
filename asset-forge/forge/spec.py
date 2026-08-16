"""The species spec: one JSON file that everything reads and writes.

This is the pivot the whole tool turns on. Sliders write a spec, plain-language
requests patch a spec, batch generation reads a spec, a seed varies within a
spec, the library stores a spec. Nothing downstream needs to know which of
those a change came from.

`PARAMS` below is the single source of truth. Validation clamps against it, the
UI will build its sliders from it, and the language model gets it as the list
of things it is allowed to touch (with ranges, so it cannot ask for a 400 m
oak). Adding a knob means adding one row here and reading it in the generator.
"""

from __future__ import annotations

import copy
import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import biomes as biomelib, kinds as kindlib, materials


@dataclass(frozen=True)
class Param:
    path: str
    label: str
    default: Any
    lo: float = 0.0
    hi: float = 1.0
    step: float = 0.01
    kind: str = "float"  # float | int | bool | choice | text
    choices: tuple[str, ...] = ()
    group: str = "general"
    help: str = ""
    kinds: tuple[str, ...] = ()   # asset kinds this row applies to; () = all of them


P = Param

# The crown envelopes that exist. Declared here because `envelope` imports this
# module and not the other way round; `envelope` checks at import that its own
# profile table matches this list exactly, so the two cannot drift apart again.
_CROWN_SHAPES = ("sphere", "cone", "spire", "ovoid", "umbrella", "column",
                 "vase", "wedge", "hanging")

# Leaf arrangements, declared here for the same reason: `rasterize` owns the
# table that turns each one into clump geometry and checks at import that it
# covers exactly this list. A habit that silently falls back to the default is
# indistinguishable from one that works, which is how `spire` and `ovoid`
# crowns rendered as spheres for as long as they did.
_FOLIAGE_HABITS = ("spiral", "distichous", "opposite", "tuft", "radial",
                   "rosette", "pendulous")

# Fish outlines and markings, declared here for the same reason and checked the
# same way: `forge/fish.py` asserts at import that its own tables cover exactly
# these lists. A choice that falls through to a default is indistinguishable
# from one that works, which is how `spire` and `ovoid` crowns rendered as
# spheres for as long as they did.
_CAUDAL_SHAPES = ("forked", "truncate", "rounded", "pointed", "none")
_DORSAL_SHAPES = ("triangular", "sail", "spiny", "ridge", "none")
_FISH_PATTERNS = ("none", "stripe", "stripes", "bars", "spots", "mottle",
                  "saddle")
# The shapes a countershading boundary may take. `flat` is a level line along
# the animal, which is what every marking here was until now; the other three
# bend it. See `fish._field_lines` and `docs/marine-marking-research.md`.
_FIELD_CURVES = ("flat", "cape", "flame", "hourglass")
# Which sex of a species to draw. `unsexed` is the species average and is what
# every spec that has not measured a difference carries. See `fish._sex_scale`.
_SEXES = ("unsexed", "female", "male")

# Bird outlines, poses and markings, declared here for the same reason and
# checked the same way: `forge/bird.py` asserts at import that its own tables
# cover exactly these lists.
_TAIL_SHAPES = ("square", "rounded", "graduated", "wedge", "notched",
                "forked", "pointed")
_WING_SHAPES = ("elliptical", "pointed", "soaring", "slotted")
_BIRD_POSES = ("perched", "flying")
_HEAD_MARKS = ("none", "cap", "mask", "supercilium", "throat", "collar")
_WING_MARKS = ("none", "bar", "doublebar", "panel", "tip")
_BODY_MARKS = ("none", "barred", "streaked", "speckled", "breastband")

# The same three lists with a "leave it alone" entry in front, for the OTHER
# SEX'S plumage. See `bird.sex_plumage` and `bird._alt`.
#
# THE SENTINEL IS NOT LAZINESS, IT IS DRIFT PROTECTION. The obvious design is
# that a dimorphic species authors the other sex's plumage in full -- all seven
# colours and all three markings -- and the code just swaps one complete set for
# the other. A great spotted woodpecker differs from its female in ONE field:
# he carries a crimson nape and she carries nothing there. Under the complete
# set she would restate six colours and two markings identically, and the day
# somebody retunes the male's wing panel the female keeps the old one and
# nothing anywhere says so. `same` means "whatever the species carries", so the
# only rows an author writes are the rows that actually differ.
_ALT_HEAD_MARKS = ("same",) + _HEAD_MARKS
_ALT_WING_MARKS = ("same",) + _WING_MARKS
_ALT_BODY_MARKS = ("same",) + _BODY_MARKS

# Land-animal stances, ear outlines, headgear and markings, declared here for
# the same reason and checked the same way: `forge/quadruped.py` asserts at
# import that its own tables cover exactly these lists.
#
# THE STANCE IS A SPECIES PROPERTY AND NOT A POSE, which is worth stating beside
# the list rather than only in the generator. A kangaroo cannot stand
# quadrupedally and a monitor lizard cannot stand like a horse, so this belongs
# in `spec_hash` and in `seed_hash` alike and is deliberately NOT added to
# `SEED_INVARIANT`. `docs/biomes/README.md` §4.2 predicted a repeat of the
# bird-pose seed trap here; it does not arise, because animals ship in one pose
# (owner, 2026-08-14) and there is no second authored posture to reseed against.
# WHAT A GROUND PLANT MAY BE MADE OF -- ONE MENU FOR THE STEM AND THE HEAD.
#
# THE FAILURE THIS ENDS, from `docs/aquatic-species.md` §8.6a. These were two
# different menus: `materials.stem` offered seven land-vegetation greens and
# browns, `materials.head` offered fourteen, and they overlapped in only four --
# there was no `leaf_jungle` and no `podzol` anywhere in `head`. A head material
# outside its own menu is not refused, it is REPLACED WITH THE DEFAULT, and the
# default here is `leaf_blossom`, a pink at (226,168,190). So the first draft of
# `tools/seed_freshwater_plants.py` shipped a brown-cigar cattail, a
# black-spiked pond sedge and two sets of lotus pads ALL WEARING BLOSSOM PINK,
# and every one of them validated clean, built clean and reported success.
#
# That is this project's signature failure exactly: it ran, it said it worked,
# and it drew the wrong thing. And note where the silence actually lives --
# `validate` does emit a warning, but the SAVED spec then holds `leaf_blossom`,
# so from that moment on the file on disk is self-consistent and no gate
# downstream can ever tell. `tools/buildcheck.py` fails on any spec warning at
# load and could not have caught this, because by the time the file existed
# there was nothing left to warn about. The window between authoring and saving
# is the whole window, and a printed warning is the only thing in it.
#
# So the fix is not a louder warning, it is to remove the mismatch: one tuple,
# read by both rows, and `forge/ground.py` -- the file that resolves both --
# asserts at import that the two rows still share it. That is the same guard
# `envelope.py` puts on crown shapes and `rasterize.py` puts on foliage habits,
# for the same reason and after the same class of accident.
#
# ADDITIVE, SO NOTHING RESEEDS. Neither default moves and no choice is removed,
# so every spec's canonical JSON is byte-identical and no individual changes.
# Every name here is already in `forge/palette.py` and inside the engine's
# `kMaterialCount`, which `forge.cli selftest` gates on.
#
# WHAT THE UNION BUYS BEYOND THE BUG. §8.6a's two other complaints were that
# there is no dark brown in `head` at all -- `broadleaf-cattail` and
# `lesser-pond-sedge` both carry `leaf_autumn`, an orange, and both say so in
# their notes -- and no dark red, which cost `marsh-cinquefoil` its blackish
# purple-red. `podzol` answers the first. `skin_red` is added for the second: it
# is the darkest red in the palette, against `plume_crimson` (208,40,56) which
# is the bright orange-red those specs are wearing now.
_PLANT_MATERIALS = (
    # vegetation: greens, browns, straw, snow
    "grass", "savanna_grass", "leaf_broadleaf", "leaf_needle", "leaf_jungle",
    "leaf_dry", "leaf_autumn", "podzol", "snow",
    # flower and seed-head colours
    "leaf_blossom", "plume_white", "plume_crimson", "plume_lilac", "plume_buff",
    "skin_blue", "skin_yellow", "skin_orange", "skin_red",
)

_QUAD_STANCES = ("standing", "sprawling", "bipedal")
_QUAD_EARS = ("none", "round", "pointed", "blade", "fan", "tufted")
_QUAD_HORNS = ("none", "spike", "curve", "sweep", "spiral", "palmate", "branched")
_QUAD_MARKS = ("none", "bars", "spots", "saddle", "flankstripe", "dapple",
               "blotch")

# Every kind EXCEPT fish. Two placement rows are meaningless for something that
# swims -- ground steepness, and distance to water for a thing that is in it --
# and the house rule is that a section does not show a slider it cannot use, it
# leaves it out. `Param.kinds` is a whitelist, so excluding one kind means
# naming the rest.
#
# A BIRD IS IN THE LIST. It is not standing on the ground the way a tree is,
# but both rows still say something true about one: a bird perches on what
# grows on the ground, so ground steepness gates where it can be, and a heron
# and a kingfisher are defined by being near water. Excluding them would have
# thrown away the only two placement rows that separate a riverbank species
# from a hillside one.
#
# A QUADRUPED IS IN THE LIST TOO, and unlike the bird it is in it literally: it
# is the first asset in this library that genuinely stands on the ground the
# slope row describes. Ground steepness is a real gate on a hoofed animal --
# `09-bare-rock.md` argues the 35-degree threshold is the angle of repose for
# LOOSE MATERIAL and not the angle at which an ibex loses its footing, which is
# a placement question this row is the right place to answer -- and distance to
# water separates a hippo from an addax.
_LAND_KINDS = ("tree", "bush", "rock", "grass", "reed", "flower", "bird",
               "quadruped")

# The two animal kinds that SWIM. They share one generator and one parameter
# group, the way grass, reeds and flowers share theirs; what separates them is
# which rows apply, which species the designer authors, and where they go.
_SWIM_KINDS = ("fish", "cetacean")

# Anything that lives IN water, not only what swims. The Ocean biome hosts rock,
# grass, reed and bush as of 2026-08-15, so kelp, seagrass and coral are aquatic
# too -- and the depth rows below are the only place an asset can say how deep
# it lives. Until this existed they were scoped to the two swimming kinds, which
# gates the APP'S SLIDERS and not `validate`: patching a kelp spec with a depth
# succeeded silently, so every aquatic plant recorded its depth band in PROSE in
# its notes, where placement cannot read it.
#
# Widening a `kinds=` tuple changes no spec bytes and reseeds nothing -- every
# validated body already carries every group. Doing it now is free; doing it
# after placement ships is a kWorldGenVersion bump with goldens to re-bless.
_WET_KINDS = _SWIM_KINDS + ("grass", "reed", "bush", "flower", "rock")

# How a tail is held. A fish's caudal fin is VERTICAL and beats side to side; a
# whale's fluke is HORIZONTAL and beats up and down, which is the single most
# reliable way to tell a dolphin from a shark at any distance and from any
# angle except dead broadside.
_CAUDAL_PLANES = ("vertical", "horizontal")

PARAMS: tuple[Param, ...] = (
    P("name", "Species name", "unnamed", kind="text", group="general"),
    P("notes", "Notes", "", kind="text", group="general",
      help="Free text for the designer; ignored by the generator."),
    P("kind", "Asset kind", "tree", kind="choice", group="general",
      choices=kindlib.KEYS,
      help="What sort of asset this is. It decides which parameters apply and "
           "which generator runs — a rock has no trunk, crown or foliage."),
    P("height_m", "Height (m)", 12.0, 0.05, 150.0, 0.01, group="general",
      help="Ground to the top of the crown, or the length of a stem. The range "
           "spans a 5 cm cushion plant to a 150 m hero tree, because one slider "
           "serves every kind that grows; rocks ignore it and use their own "
           "size. Anything much over about 30 m is a set piece rather than "
           "scenery — check the grid size it asks for before generating."),
    # THE HELP TEXT BELOW WAS REWRITTEN 2026-08-15 BECAUSE IT DESCRIBED A WORLD
    # THAT NO LONGER EXISTS. It read "5 cm IS THE DEFAULT AND EVERY ASSET USES
    # IT" and "nothing authored at 2.5 cm or below can be put in the world",
    # both of which the owner's 2026-08-14 ruling on lattices (see
    # `forge/kinds.py`) overturned and which the library on disk contradicts:
    # measured over all 828 specs, 312 are at 5 cm, 226 at 1 cm, 187 at 10 cm
    # and 103 at 2 cm. Of 131 quadrupeds, 63 are at 2 cm, 42 at 1 cm and 26 at
    # 5 cm. A help string that a reader can falsify by opening any bird spec is
    # worse than none, because it is the sentence they will trust.
    #
    # AND IT IS A CHOICE, WHICH MEANS AN OUT-OF-MENU VALUE IS REPLACED WITH "5"
    # AND THE ASSET STILL BUILDS. `resolution_cm` is the most expensive place in
    # this table for that to happen -- it scales an asset's voxel count by the
    # cube of the ratio, so a spec authored "3" builds at 5 cm and comes back
    # about 4.6x lighter than the author thinks it is. It happened: six
    # quadrupeds authored at "3" were measured against the same six at "5" and
    # came back BYTE-IDENTICAL -- elephant 62,635 both, hippo 20,682 both, rhino
    # 18,308, yak 12,651, camel 4,112, bison 6,748 -- and a whole measurement
    # pass was spent believing a 3 cm option had been priced. Same failure as
    # the blossom-pink cattail in `_PLANT_MATERIALS`, with a bigger bill.
    # `forge.cli selftest` now trips a bogus value through EVERY choice
    # parameter in this table, this one included, every run.
    P("resolution_cm", "Voxel size", "5", kind="choice", group="general",
      choices=("10", "5", "2.5", "2", "1"),
      help="Edge length of one voxel, and the size the asset EXPORTS at.\n\n"
           "THIS IS A MENU, NOT A NUMBER. A value that is not on it is not "
           "refused — it is silently replaced with 5 and the asset builds "
           "anyway, roughly 4.6x lighter than a 3 cm author expects. There is "
           "no 3 cm and no 4 cm.\n\n"
           "WHICH SIZE IS NOT A MATTER OF TASTE, it follows from the kind "
           "(owner, 2026-08-14; see forge/kinds.py). A tree or a rock JOINS THE "
           "WORLD'S OWN VOXEL GRID and is destructible as terrain is, so it is "
           "authored at the terrain's 10 cm and nothing else is legal — the "
           "selftest refuses one that is not. Everything else — bushes, ground "
           "cover, fish, birds and land animals — is a DETAIL asset carrying "
           "its own grid and its own transform, so its lattice is free.\n\n"
           "For those, the rule is measured rather than chosen: the COARSEST "
           "voxel at which the species' smallest identifying feature is still "
           "about three voxels across. That is why the library sits where it "
           "does — 5 cm for ground cover, 1 cm for a bird whose eye stripe is a "
           "centimetre, 2 cm for most land animals. Previews pick their own "
           "size to stay cheap and say so when it differs from this."),

    P("trunk.radius_base_m", "Trunk radius at base (m)", 0.30, 0.01, 12.0, 0.01, group="trunk"),
    # THE STEM HAD NO d(z) AT ALL, AND THE LIBRARY MEASURED IT.
    #
    # `skeleton._radii` derives every node's radius from Murray's law over the
    # branching topology and rescales the whole tree so the root equals
    # `trunk.radius_base_m`. Thickness was therefore entirely a consequence of
    # WHERE BRANCHES FORK, and a stem that forks little came out a post:
    # `birch` measured a taper ratio of 1.00 on all three seeds -- a perfect
    # cylinder from the ground to the first fork -- and `hero-sequoia` measured
    # 0.977 / 0.977 / 0.988, i.e. a 90 m untapered column.
    # `docs/plant-proportion-research.md` §5.4 calls this the one finding there
    # that a spec value cannot reach, because there was no term to author.
    #
    # This is that term. `r(z) = r_base * ((H - z) / H) ** taper`, applied as a
    # CEILING on the radius Murray's law already worked out rather than as a
    # multiplier on it, so it can only remove wood and never add any: where the
    # tree has already forked, Murray is thinner than this envelope and nothing
    # changes; on the unforked bole, where Murray says "cylinder", this is what
    # is left. Floored at `growth.tip_radius_m`, so the envelope going to zero
    # at the top of the tree cannot thin a twig below the twig radius the
    # species authored -- without that floor the leader of a whorled conifer
    # tapers away to nothing and takes its foliage anchors with it.
    #
    # THE EXPONENT IS THE CLASSICAL SOLID OF REVOLUTION and the default is not
    # a fitted number. 0 is a cylinder, 1/3 is Metzger's cubic paraboloid (the
    # stem as a beam of uniform resistance to bending, 1893), 0.5 is the plain
    # paraboloid that most of the taper literature treats as the shape of the
    # merchantable bole, 1.0 is a cone and above that runs neiloid, which is
    # what the butt swell under breast height really is -- and that part is
    # `trunk.buttress`, not this. The default sits at the paraboloid.
    #
    # WHAT THE DEFAULT IS WORTH AT 10 CM, so nobody tunes the second decimal:
    # on a 20 m tree, halfway up, 0.4 and 0.6 differ by 0.758 vs 0.660 of the
    # base radius. On a 40 cm stem that is under half a voxel. One significant
    # figure is all this lattice can carry, which is the same argument
    # `envelope.lame` makes for not separating Scots pine from Norway spruce by
    # 0.07 of a crown exponent.
    #
    # NOT MEASURED AGAINST A PUBLISHED TAPER EQUATION. The plant fit looked for
    # one and found none it could licence-clear (research §8, "Trunk taper, for
    # any species -- no taper equation was obtained"), so the library's 0.53-1.71
    # spread was measured against nothing. It still is. What has changed is that
    # there is now a term to put a measurement INTO when one arrives, and a
    # per-species knob to put it in per species.
    P("trunk.taper", "Trunk taper", 0.50, 0.0, 1.5, 0.05, group="trunk",
      help="How fast the stem thins with height. 0 is a true cylinder — which "
           "is the right answer for a palm, a bamboo and a tree fern, because "
           "a monocot stem does not lay down new wood and really is the same "
           "thickness all the way up. 0.33 is a stem built like a beam of "
           "uniform bending strength, 0.5 a paraboloid (the default, and the "
           "usual description of a woody bole), 1.0 a cone. Only ever thins "
           "the tree: it is a ceiling on the thickness the branching model "
           "already worked out, so it bites on the unforked bole and leaves "
           "the crown alone, and it never thins a twig below the twig radius."),
    P("trunk.clear_frac", "Branch-free height", 0.35, 0.0, 0.90, 0.01, group="trunk",
      help="Fraction of total height with bare trunk. High for jungle emergents, low for bushes."),
    P("trunk.lean_deg", "Lean", 3.0, 0.0, 40.0, 0.5, group="trunk"),
    P("trunk.lean_dir_deg", "Lean direction", 0.0, 0.0, 360.0, 1.0, group="trunk"),
    P("trunk.wander", "Wander", 0.15, 0.0, 1.0, 0.01, group="trunk",
      help="How much the trunk wobbles on its way up. Gnarled desert wood is high."),
    P("trunk.buttress", "Root flare", 0.0, 0.0, 1.0, 0.01, group="trunk",
      help="Thickening at the very base. Kapok and jungle trees want this. The "
           "falloff is scaled to the tree, so the same setting reads the same "
           "on a sapling and on an emergent."),
    P("trunk.lobes", "Trunk lobes", 0, 0, 12, 1, kind="int", group="trunk",
      help="Runs vertical grooves up the trunk instead of leaving it a "
           "circular post. Every branch here is drawn as a capsule, so every "
           "cross-section is a perfect circle; real boles run in ridges and "
           "hollows, and on a large tree those are the most legible thing "
           "about it because they catch light along their whole length. "
           "NOT SAFE YET — leave at 0. It cuts wood loose: the trunk's surface "
           "is where limbs and roots attach, so grooving it severs joins and "
           "leaves pieces floating. Restricting it to the bare bole, to the "
           "trunk's own outline in each slice, to away from junctions, and "
           "cutting before the branches are drawn each reduced the damage and "
           "none removed it — the roots radiate out through the grooved zone "
           "and are cut regardless. Doing this properly means fluting the "
           "capsule as it is drawn rather than carving afterwards."),
    P("trunk.lobe_depth", "Lobe depth", 0.18, 0.0, 0.6, 0.01, group="trunk",
      help="How deep the grooves cut, as a fraction of the trunk radius. Cut "
           "inward rather than added outward, so the trunk keeps the thickness "
           "the taper model gave it."),

    # Choices come from the envelope module rather than being spelled out
    # again here. A second copy of this list went stale the moment two profiles
    # were added: the new names failed validation, fell back to "sphere", and
    # the only reason it was not shipped that way is that the patch report
    # happened to be printed.
    P("crown.shape", "Crown shape", "sphere", kind="choice", group="crown",
      choices=_CROWN_SHAPES,
      help="Which envelope the crown grows into. 'cone' and 'spire' are fitted "
           "to laser-scanned conifers — widest a third and a sixth of the way "
           "up respectively, not at the base."),
    P("crown.allometry", "Size from trunk", "off", kind="choice", group="crown",
      choices=("off", "broadleaf", "conifer"),
      help="Works out the crown radius, length and base height from how thick "
           "the trunk is, using the fitted relations foresters use, instead of "
           "you setting three sliders and keeping them consistent by hand. "
           "Turn it on and the crown follows the trunk; change the height and "
           "the crown still fits. It OVERRIDES the three controls below, which "
           "is why it is off unless asked for. The relations are for "
           "forest-grown trees — an open-grown crown on the same stem runs "
           "10-30% wider."),
    # THE FOUR FLOORS BELOW USED TO PUT A HALF-METRE MINIMUM UNDER A KIND WHOSE
    # LATTICE IS FREE. `crown.radius_m` 0.30, `growth.influence_m` 0.40,
    # `growth.step_m` 0.08 and `trunk.radius_base_m` 0.05 are metre-scale bounds,
    # and a `bush` is a detail-lattice kind that may be authored at 1 cm. On a
    # 25 cm plant the crown-radius floor alone was 1.2x the whole plant's height
    # and the reach floor was 1.6x it, so every growth target sat outside the
    # plant. Five aquatic species were moved to tuft kinds to get round it and
    # `dead-mans-fingers` did not merely come out thin -- it failed
    # `pipeline.health` outright at seed 2 with "bare: the trunk never
    # branched", after building 1,079 voxels at seed 1
    # (docs/aquatic-species.md §8.8).
    #
    # MEASURED BEFORE LOWERING THEM, a 0.25 m plant on the same spec, five seeds
    # each, against the 94-185 voxel band a shipped small tuft occupies:
    #
    #     0.60 m at 5 cm, as `sea-oak-weed` ships    212-246 vox, fork order 3-4
    #     0.25 m at 5 cm, floors as they were         66- 89 vox, order 2-3
    #     0.25 m at 5 cm, floors lowered              45- 55 vox, order 2-4
    #     0.25 m at 2 cm, floors lowered             171-196 vox, order 3-5
    #     0.25 m at 1 cm, floors lowered             626-660 vox, order 5-8
    #
    # So the floor was never really a floor on the PLANT, it was a floor on the
    # LATTICE: at 5 cm nothing rescues a 25 cm bush, because eight growth steps
    # of 8 cm is 0.64 m and that is what it takes to reach a fork -- which is
    # why 0.60 m is the smallest bush in the library that works, to within a
    # voxel of the arithmetic. Bring the lattice down with the plant and the
    # same species builds in the shipped band WITH the dichotomous forking that
    # moving it to a tuft threw away.
    #
    # WHAT STOPS SOMEBODY SETTING NONSENSE, now that the blanket floors are
    # gone: `validate`'s cross-checks below already refuse the combinations that
    # matter -- kill >= influence ("the tree will be a bare trunk", which is
    # exactly what `dead-mans-fingers` printed) and step > influence. Those are
    # relationships between the three, which is what actually governs whether
    # colonization can branch; a fixed metre bound never was. And the runaway
    # these might be imagined to guard against does not exist: a 30 m beech at
    # `step_m` 0.02 was measured at 10.0 s and 7,227 voxels, i.e. it degenerates
    # into a scribble at the same cost as any other bad number, rather than
    # exploding. MAX_NODES already holds that end.
    P("crown.radius_m", "Crown radius (m)", 3.5, 0.05, 60.0, 0.1, group="crown"),
    P("crown.height_frac", "Crown height", 0.65, 0.10, 1.0, 0.01, group="crown",
      help="Crown's vertical extent as a fraction of tree height."),
    P("crown.center_frac", "Crown centre", 0.66, 0.20, 0.98, 0.01, group="crown"),
    P("crown.shell_upper", "Leafy depth, upper", 0.55, 0.0, 1.0, 0.01,
      group="crown",
      help="How deep the leafy layer goes in from the crown surface, above the "
           "widest point, as a fraction of the local radius. 1 fills the crown "
           "solidly; 0.25 leaves a skin. This replaced a single hollowness "
           "number because the forestry models state it as a thickness above "
           "and below the widest point separately, and one symmetric number "
           "cannot describe a crown whose two halves are filled differently."),
    P("crown.shell_lower", "Leafy depth, lower", 0.40, 0.0, 1.0, 0.01,
      group="crown",
      help="The same, below the widest point. Typically thinner than the "
           "upper: a mature broadleaf carries a solid crown on top and little "
           "but bare branches underneath, because nothing down there gets the "
           "light to keep leaves. 0 empties the lower crown completely."),
    P("crown.squash", "Vertical squash", 1.0, 0.30, 2.5, 0.01, group="crown"),
    P("crown.lean_deg", "Crown lean", 0.0, 0.0, 45.0, 0.5, group="crown",
      help="Wind-shear. Leans the whole crown off the trunk axis."),
    P("crown.lean_dir_deg", "Crown lean direction", 0.0, 0.0, 360.0, 1.0, group="crown"),
    P("crown.asymmetry", "Lopsidedness", 0.25, 0.0, 1.0, 0.01, group="crown",
      help="How far from circular the crown is when seen from above. Foresters "
           "measure four to eight radii around a trunk rather than one, because "
           "one does not describe a real crown, and the spread between them "
           "widens as a tree ages. At 0 every crown is a surface of revolution, "
           "which is a shape that occurs in nature about as often as a perfect "
           "sphere."),
    P("crown.offset", "Crown displacement", 0.20, 0.0, 0.8, 0.01, group="crown",
      help="Slides the crown sideways off its trunk, fully at the top and not "
           "at all at the base. Trees lean their crowns away from their "
           "neighbours and into any gap they can reach: measured on old-growth "
           "beech, a forest-grown crown's centre sits about 0.37 of its own "
           "radius away from its stem, while an isolated open-grown tree "
           "manages roughly half that. Use the low end for a specimen tree "
           "standing alone and the high end for anything grown in a stand."),
    P("crown.points", "Growth targets", 900, 80, 60000, 10, kind="int", group="crown",
      help="More targets means denser, finer branching and a slower generate."),

    P("growth.model", "Growth model", "colonize", kind="choice", group="growth",
      choices=("colonize", "whorl", "frond"),
      help="How branches are placed. 'colonize' grows toward scattered targets and "
           "suits irregular broadleaf crowns. 'whorl' builds rings of branches up a "
           "straight leader, which is what a conifer actually is — a spruce's tiers "
           "are a real structure, not an irregular crown that happens to look tiered."),
    P("growth.step_m", "Segment length (m)", 0.35, 0.02, 6.0, 0.01, group="growth",
      help="How far a branch grows in one iteration. See the floors note on crown.radius_m: this one is the binding constraint on how small a plant the branching model can draw, because it takes about eight steps of trunk to reach a fork."),
    P("growth.influence_m", "Reach (m)", 3.0, 0.03, 60.0, 0.1, group="growth",
      help="How far a branch tip can see a growth target."),
    P("growth.kill_m", "Target consumption (m)", 0.70, 0.02, 16.0, 0.05, group="growth",
      help="Low values give long thin twigs, high values give stubby branching."),
    P("growth.gravity", "Droop", -0.12, -1.0, 1.0, 0.01, group="growth",
      help="Negative droops branches down (willow), positive lifts them up."),
    P("growth.phototropism", "Reach for light", 0.10, 0.0, 1.0, 0.01, group="growth"),
    P("growth.inertia", "Straightness", 0.45, 0.0, 1.0, 0.01, group="growth"),
    P("growth.jitter", "Wobble", 0.05, 0.0, 0.6, 0.01, group="growth"),
    P("growth.max_iter", "Growth iterations", 260, 20, 900, 10, kind="int", group="growth"),
    P("growth.shade", "Competition for light", 0.40, 0.0, 1.0, 0.01,
      group="growth",
      help="Makes branches avoid growing into space that is already shaded by "
           "wood above it. Without this, growth targets pull just as hard from "
           "inside a solid mass of existing crown as they do from open air — "
           "nothing in the model knows the space is taken — so the crown fills "
           "in evenly everywhere and reads as a cloud of twigs. Real branches "
           "are competing for light and lose that competition in shade. At 0 "
           "you get the old behaviour."),
    P("growth.tip_radius_m", "Twig radius (m)", 0.045, 0.01, 2.0, 0.005, group="growth"),
    P("growth.radius_exp", "Branch thickness falloff", 2.30, 1.50, 3.50, 0.05, group="growth",
      help="Murray's law exponent. 2 splits thickness evenly, 3 keeps parents thick."),

    P("whorl.count", "Whorls", 11, 3, 30, 1, kind="int", group="whorl",
      help="Rings of branches up the trunk. More rings, denser tiers."),
    P("whorl.branches", "Branches per whorl", 5, 2, 10, 1, kind="int", group="whorl"),
    P("whorl.stagger", "Whorl rotation", 0.42, 0.0, 1.0, 0.01, group="whorl",
      help="How far each ring is rotated from the one below. Near 0 lines branches "
           "up into visible columns; the default keeps them interleaved."),
    P("whorl.rise", "Branch lift at the trunk", 0.28, -0.5, 1.0, 0.01, group="whorl",
      help="How much a branch angles upward where it leaves the trunk, before "
           "gravity takes over further out."),
    P("whorl.droop", "Branch droop at the tip", 0.55, 0.0, 2.0, 0.01, group="whorl"),
    P("whorl.sub", "Sub-branches per branch", 2, 0, 5, 1, kind="int", group="whorl"),
    P("whorl.sub_angle", "Sub-branch spread", 38.0, 5.0, 80.0, 1.0, group="whorl"),
    P("whorl.irregular", "Irregularity", 0.22, 0.0, 1.0, 0.01, group="whorl",
      help="Random variation in ring spacing and branch length, so tiers are not "
           "mechanically even."),
    P("whorl.leader", "Leader above the top whorl", 0.03, 0.0, 0.4, 0.01, group="whorl",
      help="Bare spire left above the highest ring."),

    P("frond.count", "Fronds", 14, 3, 40, 1, kind="int", group="frond"),
    P("frond.length_m", "Frond length (m)", 2.6, 0.4, 8.0, 0.1, group="frond"),
    P("frond.width_m", "Blade width (m)", 0.55, 0.05, 2.5, 0.05, group="frond",
      help="Half-width of the leaf blade at its widest point, roughly a third of the "
           "way along the midrib."),
    P("frond.rise", "Frond lift", 0.85, -0.5, 2.0, 0.01, group="frond",
      help="How steeply a frond leaves the crown before arcing over. High values give "
           "the upright shuttlecock of a young palm; low values a flat spread."),
    P("frond.droop", "Frond droop", 1.15, 0.0, 3.0, 0.01, group="frond"),
    P("frond.dead", "Hanging dead fronds", 0.15, 0.0, 0.6, 0.01, group="frond",
      help="Share of fronds that have collapsed and hang against the trunk — the "
           "skirt of brown leaves under a real palm's crown."),
    P("frond.irregular", "Frond irregularity", 0.25, 0.0, 1.0, 0.01, group="frond"),

    P("roots.count", "Surface roots", 0, 0, 16, 1, kind="int", group="roots",
      help="Roots radiating from the base as visible ridges. Zero for most species. "
           "This is real geometry, unlike Root flare, which only thickens the trunk."),
    P("roots.length_m", "Root length (m)", 1.8, 0.3, 8.0, 0.1, group="roots"),
    P("roots.rise", "Root arch", 0.35, 0.0, 1.5, 0.01, group="roots",
      help="How far a root humps above ground before running back down to it."),
    P("roots.thickness", "Root thickness", 0.55, 0.05, 1.0, 0.01, group="roots",
      help="Root radius where it leaves the trunk, as a fraction of the trunk's."),
    P("roots.irregular", "Root irregularity", 0.30, 0.0, 1.0, 0.01, group="roots"),

    P("strand.count", "Trailing strands", 0, 0, 400, 5, kind="int", group="strand",
      help="Long thin branches hanging from the crown. Zero for most species; this is "
           "what makes a weeping willow, and the same pass gives lianas and hanging "
           "moss. Space colonization cannot produce these — targets are consumed on "
           "arrival, so a branch stops rather than trails."),
    P("strand.length_m", "Strand length (m)", 2.5, 0.3, 12.0, 0.1, group="strand"),
    P("strand.from_frac", "Hang from", 0.35, 0.0, 1.0, 0.01, group="strand",
      help="How high up the crown strands start. 0 hangs from everywhere, 1 only from "
           "the very top."),
    P("strand.outer", "Prefer the crown edge", 0.7, 0.0, 1.0, 0.01, group="strand",
      help="Bias toward anchors far from the trunk, so strands form a curtain around "
           "the crown rather than a beard down the middle."),
    P("strand.drift", "Strand drift", 0.16, 0.0, 0.8, 0.01, group="strand",
      help="How much a strand wanders as it falls."),
    P("strand.spread", "Strand flare", 0.18, -0.5, 1.0, 0.01, group="strand",
      help="Outward lean as a strand descends. Negative tucks them under the crown."),

    P("foliage.enabled", "Foliage", True, kind="bool", group="foliage"),
    P("foliage.min_order", "Leaves from branch order", 2, 0, 8, 1, kind="int", group="foliage"),
    P("foliage.clump_radius_m", "Clump radius (m)", 0.65, 0.04, 12.0, 0.01, group="foliage",
      help="Radius of one leaf mass. The floor is set for 2 cm ground cover, "
           "not for trees -- a 15 cm floor was fine for an oak and far too "
           "coarse for a knee-high shrub."),
    P("foliage.density", "Clump density", 0.60, 0.05, 1.0, 0.01, group="foliage"),
    P("foliage.rough", "Clump raggedness", 0.55, 0.0, 1.0, 0.01, group="foliage",
      help="Breaks the OUTLINE of each leaf clump before it becomes voxels. "
           "At 0 a clump is a perfect ball on a rounded radius, so clumps of "
           "similar size come out identical and a canopy reads as repeated "
           "discs with stair-step rings around each one. Thinning the inside "
           "cannot fix that — hollow a sphere as much as you like and its "
           "silhouette is still a sphere. This is the same fix the rocks "
           "needed and for the same reason."),
    P("foliage.habit", "Leaf arrangement", "spiral", kind="choice",
      group="foliage", choices=_FOLIAGE_HABITS,
      help="How the species carries its leaves on the shoot, which is most of "
           "what makes one tree look unlike another. spiral: all round the "
           "shoot, the neutral broadleaf case. distichous: two-ranked into one "
           "flat plane held level to the light — beech, elm, fir, hemlock, and "
           "what makes a fir spray a spray. opposite: decussate pairs, maple "
           "and ash. tuft: needles held on only the last two or three years of "
           "growth, so foliage sits at the shoot ENDS with clean twig behind "
           "— pine, whose crowns really are see-through. radial: needles all "
           "round and retained five to seven years, so the whole shoot is "
           "clothed — spruce, the densest. rosette: compressed clusters on "
           "short shoots spaced along older wood — birch, larch, apple, and "
           "the one habit that puts foliage back on the inner branches. "
           "pendulous: sprays hanging from the shoot — willow."),
    P("foliage.stretch", "Shoot elongation", 2.2, 1.0, 6.0, 0.05, group="foliage",
      help="Stretches each leaf clump along the twig it hangs from, so it is a "
           "shoot rather than a ball. Leaves are borne on twenty to sixty "
           "centimetres of new growth, not at a point, so a clump drawn round "
           "is the wrong primitive however well its outline is broken up. It "
           "also gives the canopy an orientation: a ball has no direction, so a "
           "crown of them has none either, and that is what reads as placed "
           "rather than grown. Volume is preserved, so the clump radius keeps "
           "meaning what it did. 1 is the old ball; 2–3 suits most broadleaves; "
           "4 and up gives the long sprays of spruce and hemlock."),
    P("foliage.clustering", "Clumping", 0.45, 0.0, 1.0, 0.01, group="foliage",
      help="Gathers leaf clumps into masses with daylight between them, "
           "instead of spreading them as evenly as they will go. Even spacing "
           "is what you get by default from keeping every clump a fixed "
           "distance from its neighbours, and it is the least natural "
           "arrangement available — real foliage is strongly aggregated, and "
           "how strongly is a first-order property of a canopy rather than a "
           "finishing touch. This moves clumps about rather than removing "
           "them, so the crown keeps its weight."),
    P("foliage.compensate", "Spacing compensation", 0.60, 0.0, 1.0, 0.01,
      group="foliage",
      help="Grows the surviving clumps to make up for the ones that spacing "
           "deleted. Raising separation reads as a control over gaps, but it "
           "removes whole clumps — going from 1.2 to 3.0 drops 85% of them and "
           "takes a quarter of the crown's outline with it. This holds the "
           "total leaf area roughly steady so the tree does not shrink every "
           "time you open it up. Kept partial on purpose: compensating fully "
           "would close the gaps you widened."),
    P("foliage.top_bias", "Top-heavy foliage", 0.35, 0.0, 1.0, 0.01,
      group="foliage",
      help="Moves leaf density from the bottom of the crown to the top. Every "
           "canopy model in the literature assumes leaves are spread evenly "
           "through the crown envelope, and every set of field measurements "
           "disagrees: density climbs steadily from the crown base to the top "
           "in every species measured, because the upper layer of small, "
           "steeply held leaves shades an interior that cannot use any more "
           "light. This redistributes rather than adds — the total is "
           "unchanged, so the density slider still means what it says."),
    P("foliage.coverage", "Clump coverage", 0.80, 0.0, 1.0, 0.01, group="foliage",
      help="Share of surviving twigs that carry a clump, after separation has "
           "thinned them."),
    P("foliage.separation", "Clump separation", 1.7, 0.3, 4.0, 0.05, group="foliage",
      help="Minimum distance between clump centres, as a multiple of clump radius. "
           "Below about 1.6 neighbouring clumps overlap and the canopy fuses into "
           "one solid mass; above it the crown breaks into distinct boughs of "
           "foliage with daylight between them and the branch structure visible "
           "through. This is the single biggest lever on whether a broadleaf reads "
           "as a tree or as broccoli."),
    P("foliage.clump_jitter", "Clump variation", 0.35, 0.0, 1.0, 0.01, group="foliage",
      help="Random spread in clump size and position. Zero makes the canopy a lattice "
           "of identical spheres."),
    P("foliage.droop_m", "Clump droop (m)", 0.15, -6.0, 10.0, 0.05, group="foliage"),
    P("foliage.squash", "Clump squash", 0.80, 0.30, 2.5, 0.01, group="foliage"),

    # How much individuals of this species differ from each other. Without
    # this, every seed produces the same tree with the twigs shuffled: the
    # growth randomness varies but the height, spread and trunk do not, so a
    # bank of 64 seeds gives a forest no variety at all.
    P("variation.amount", "Variation", 1.0, 0.0, 3.0, 0.05, group="variation",
      help="Master scale on everything below. Zero makes every seed the same size and "
           "shape, varying only in how the branches happen to grow."),
    P("variation.height", "Height spread", 0.28, 0.0, 0.6, 0.01, group="variation"),
    P("variation.crown_radius", "Crown spread", 0.30, 0.0, 0.6, 0.01, group="variation"),
    P("variation.trunk_radius", "Trunk spread", 0.30, 0.0, 0.6, 0.01, group="variation"),
    P("variation.shape", "Shape spread", 0.22, 0.0, 0.5, 0.01, group="variation",
      help="Varies crown squash and how high the crown sits."),
    P("variation.proportion", "Proportion spread", 0.30, 0.0, 0.6, 0.01,
      group="variation",
      help="How much of an individual is crown and how much is bare bole. This "
           "is where the visible difference between two trees of one species "
           "actually lives: a batch that varies only height and radius is the "
           "same tree twelve times at twelve sizes, because every silhouette "
           "still has its crown starting at the same fraction of its height. "
           "The crown grows DOWNWARD from a fixed top rather than about a fixed "
           "centre, so a long-crowned individual eats into its bole instead of "
           "poking out above its own height. Measured over eight seeds, taking "
           "crown height from 0.42 to 0.85 of tree height moves the crown's "
           "share of the silhouette by 41-72% and is the single strongest "
           "proportion lever under every growth model."),
    P("variation.lopsided", "Lopsidedness spread", 0.45, 0.0, 1.0, 0.01,
      group="variation",
      help="Varies how far from circular each crown is and how far it sits off "
           "its own trunk. Only bites under the 'colonize' growth model: whorl "
           "and frond trees build their branches directly and never sample the "
           "crown envelope, so this measured as doing nothing at all on a "
           "conifer."),
    P("variation.foliage", "Leaf spread", 0.35, 0.0, 0.8, 0.01, group="variation",
      help="Varies how the leaf mass is distributed within the crown: the depth "
           "of the leafy shell above and below the widest point, how top-heavy "
           "it is, how strongly it gathers into masses, and how ragged each "
           "clump's outline is. The shell depths are much the largest of these "
           "-- on an oak the lower shell alone doubles the voxel count across "
           "its range -- and they, too, only apply under 'colonize'."),
    P("variation.lean_deg", "Lean spread", 10.0, 0.0, 30.0, 0.5, group="variation"),
    P("variation.droop", "Droop spread", 0.40, 0.0, 1.0, 0.01, group="variation"),
    P("variation.density", "Density spread", 0.25, 0.0, 0.6, 0.01, group="variation"),
    P("variation.rotate", "Random facing", True, kind="bool", group="variation",
      help="Point each individual's lean in a random direction. Cheap and it does more "
           "for a forest than any other single knob."),

    *tuple(
        P(f"biomes.{b.key}", b.label, 0.0, 0.0, 1.0, 0.05, group="biome",
          kinds=b.hosts,
          help=f"How common this species is in {b.label.lower()} ({b.climate}). "
               f"Zero means it never appears there. Surface: {b.surface}.")
        for b in biomelib.HOSTING
    ),

    P("placement.abundance", "Abundance", 0.5, 0.0, 1.0, 0.01, group="placement",
      help="Overall frequency of this species where it does occur, before the "
           "per-biome weights are applied."),
    # FLOOR LOWERED 0.5 -> 0.1 (owner, 2026-08-15). The old floor was set when
    # nothing in the library needed to be denser than a shrub, and ground cover
    # is: fifteen carpet species -- bluebell, wood anemone, ramsons, feather
    # moss, hair-cap moss, buffalo grass -- asked for 0.25 to 0.45 m and were
    # SILENTLY CLAMPED to 0.5, so a moss carpet could not be authored as dense
    # as moss actually grows and nothing said so. Nothing reads `placement`
    # yet, so this costs nothing today and stops a wrong number being baked
    # into fifteen specs before anything does.
    P("placement.spacing_m", "Minimum spacing (m)", 6.0, 0.1, 3000.0, 0.1, group="placement",
      help="Closest two individuals may stand. Roughly the canopy diameter for a "
           "closed forest, much larger for savanna or desert."),
    P("placement.cluster", "Grows in stands", 0.3, 0.0, 1.0, 0.01, group="placement",
      help="0 scatters individuals evenly; 1 gathers them into groves with open "
           "ground between."),
    # FLOOR -10 -> -200 m. Sea level is z=0, so a negative value is depth, and
    # -10 m could not describe most of what lives under water: a giant kelp
    # forest reaches -30 m and a whale shark ranges past -100. The old floor was
    # set when nothing in the library lived below the tideline.
    P("placement.elev_min_m", "Lowest elevation (m)", 0.0, -200.0, 4000.0, 10.0,
      group="placement", help="Metres above sea level. The engine's sea level "
                              "is z=0, so a negative value is depth."),
    P("placement.elev_max_m", "Highest elevation (m)", 2000.0, 0.0, 5000.0, 10.0,
      group="placement",
      help="Above the treeline nothing but tundra/alpine is classified anyway "
           "(900 m at 0 C, rising ~150 m per degree), so this mainly separates "
           "lowland species from montane ones inside a biome."),
    P("placement.slope_max_pct", "Steepest ground (% grade)", 45.0, 0.0, 70.0, 1.0,
      group="placement", kinds=_LAND_KINDS,
      help="Ground steeper than this will not carry the species. Stated as a grade "
           "in percent, the same currency the engine's cliff gate uses — above 70% "
           "(~35 degrees) the ground classifies as bare rock and carries nothing."),
    P("placement.water_max_m", "Distance to water (m)", 0.0, 0.0, 500.0, 5.0,
      group="placement", kinds=_LAND_KINDS,
      help="0 means it does not care. Above 0, the species only appears within this "
           "distance of a watercourse — riverbank willows and jungle understorey."),

    P("rock.size_m", "Size (m)", 1.6, 0.2, 150.0, 0.1, group="rock",
      help="Longest dimension of the finished stone, after faceting, erosion and "
           "the burial cut have taken their share. Measured, not estimated, so "
           "this is what you get."),
    P("rock.lumps", "Lumps", 5, 1, 16, 1, kind="int", group="rock",
      help="How many overlapping masses the rock is built from. One gives a clean "
           "ovoid; more gives a knobbly, weathered boulder."),
    P("rock.spread", "Lump spread", 0.42, 0.0, 1.0, 0.01, group="rock",
      help="How far the lumps scatter from the centre. High values make a broken "
           "pile rather than a single stone."),
    P("rock.flatten", "Flatten", 0.72, 0.15, 6.0, 0.01, group="rock",
      help="Vertical squash. Below 1 gives a slab; above 1 a standing stone."),
    P("rock.elongate", "Elongate", 1.25, 0.4, 6.0, 0.01, group="rock",
      help="Stretch along one horizontal axis."),
    P("rock.angular", "Angularity", 0.45, 0.0, 1.0, 0.01, group="rock",
      help="Slices flat faces off the mass. 0 is a rounded river cobble, 1 a "
           "freshly fractured block."),
    P("rock.facets", "Facet count", 4, 0, 10, 1, kind="int", group="rock"),
    P("rock.rough", "Surface roughness", 0.4, 0.0, 1.0, 0.01, group="rock",
      help="Pushes the surface in and out before it becomes voxels. This is the "
           "one that decides whether it reads as stone: at 0 the mass is a "
           "smooth ellipsoid, and a smooth curve at this voxel size shows clean "
           "concentric stair-steps that look like a Minecraft sphere. Measured "
           "as a fraction of the stone's radius."),
    P("rock.erode", "Weathering", 0.25, 0.0, 1.0, 0.01, group="rock",
      help="How much stone the weathering pass takes away. What SHAPE it takes "
           "away is set by the two below."),
    P("rock.cavernous", "Cavernous vs spheroidal", 0.0, 0.0, 1.0, 0.01,
      group="rock",
      help="Which way the weathering leans. At 0 it attacks convex surfaces "
           "fastest — corners and edges are exposed on more sides, so a blocky "
           "stone rounds off and granite sheds shells. At 1 it attacks concave "
           "surfaces fastest — a pit holds moisture and salt, so it deepens "
           "faster than the rock around it, and the runaway pitting carves "
           "tafoni, honeycomb sandstone and hollow-sided goblins. Same "
           "mechanism, opposite sign, two families of rock."),
    P("rock.bedding", "Bedding strength", 0.0, 0.0, 1.0, 0.01, group="rock",
      help="Alternating hard and soft layers. Sedimentary rock is laid down in "
           "beds of differing hardness, and that one fact is behind most rock "
           "shapes people recognise: weather a uniform block and you get a "
           "rounded lump, weather a layered one and the soft beds retreat while "
           "the hard ones stand proud. Banded cliffs, undercut pedestals, "
           "mushroom rocks and the cap on a hoodoo are all this."),
    P("rock.bed_thickness_m", "Bed thickness (m)", 0.5, 0.05, 25.0, 0.05,
      group="rock",
      help="How thick one hard-soft cycle is. Thin gives finely banded "
           "sandstone; thick gives a few massive ledges."),
    P("rock.bed_dip_deg", "Bed dip", 0.0, 0.0, 45.0, 1.0, group="rock",
      help="Tilt of the layers. Beds are laid down flat and tilted afterwards, "
           "so a non-zero dip reads as rock that has been moved."),
    P("rock.cross_beds", "Cross-bed sets", 0, 0, 6, 1, kind="int", group="rock",
      help="Stacked wedges of steeply dipping layers, each one cut off flat by "
           "the next: aeolian cross-bedding, the Navajo Sandstone. Migrating "
           "dunes dump sand down their lee face at the angle of repose, then the "
           "following dune planes the top off, and the wind shifts between them "
           "so each wedge dips a different way. The TRUNCATION is the whole "
           "signature — ordinary bedding keeps every layer parallel forever, and "
           "no dip setting can make one layer cut another off. 0 uses flat "
           "bedding; 2-4 gives the crossed grain."),
    P("rock.caprock", "Caprock", 0.0, 0.0, 1.0, 0.01, group="rock",
      help="Makes the top of the stone much harder than the rest, so weathering "
           "eats the neck and leaves the head: hoodoos, pedestals and mushroom "
           "rocks. Bedding can only do this by accident, because its hard layers "
           "repeat all the way down."),
    P("rock.cap_frac", "Cap height", 0.62, 0.2, 0.95, 0.01, group="rock",
      help="How far up the stone the hard cap starts, as a fraction of its "
           "height."),
    P("rock.notch", "Basal attack", 0.0, 0.0, 4.0, 0.05, group="rock",
      help="Extra weathering concentrated in one horizontal band, on top of "
           "whatever the curvature pass is doing. This is the missing half of "
           "every undercut shape: blown sand only bounces to about knee height, "
           "so it saws a waist into a desert rock; waves only attack the tidal "
           "band, so they cut the notch that eventually fells a sea stack; damp "
           "soil rots the very base of a boulder. 0 is off."),
    P("rock.notch_z_m", "Attack height (m)", 0.25, 0.0, 30.0, 0.05, group="rock",
      help="Height above the ground where the attack is strongest. 0.1-0.4 for "
           "wind-blown sand, 0.5-1.5 for a wave notch."),
    P("rock.notch_spread_m", "Attack spread (m)", 0.18, 0.03, 12.0, 0.01,
      group="rock",
      help="How tall the attacked band is. Narrow gives a sharp waist, wide "
           "gives a general thinning."),
    P("rock.aspect", "One-sidedness", 0.0, 0.0, 1.0, 0.01, group="rock",
      help="Weathers one side harder than the other. Real stone is not attacked "
           "evenly — tafoni open on the damp shaded face, salt works the seaward "
           "side, frost the north — and a rock weathered the same on all sides "
           "is the most reliable tell that it came out of a generator. The side "
           "is chosen per stone, so a scatter of them does not all lean the same "
           "way."),

    P("rock.joint_sets", "Joint sets", 0, 0, 3, 1, kind="int", group="rock",
      help="Rock does not fracture in random directions — it fractures along a "
           "few JOINT SETS, typically a bedding plane plus two near-vertical "
           "sets at right angles, and every face in an outcrop shares them. "
           "That shared orientation is what makes granite look quarried rather "
           "than merely lumpy. 0 keeps the old independent random faces; 2-3 "
           "gives jointed rock."),
    P("rock.joint_scatter", "Joint scatter", 0.12, 0.0, 0.6, 0.01, group="rock",
      help="How far a face may stray from its joint set. A little keeps it "
           "natural; a lot is the same as having no sets at all."),
    P("rock.joint_dip_deg", "Joint dip", 0.0, 0.0, 90.0, 1.0, group="rock",
      help="Tilt of the whole joint frame, measured from flat-lying. At 0 the "
           "frame is a horizontal bedding plane plus two vertical sets, which "
           "is the ordinary case. At 90 it is two VERTICAL sets plus a "
           "horizontal one, and that matters because `joint sets` takes them "
           "in order: ask for two sets at dip 0 and you get the bedding plane "
           "and one vertical set, so the joints cut horizontal shelves. A "
           "pinnacle forest needs the opposite pair, and 90 is the only way to "
           "put both vertical sets first."),
    P("rock.block_size_m", "Block size (m)", 1.2, 0.2, 40.0, 0.1, group="rock",
      help="Spacing of the joint planes, so the size of one fractured block."),
    P("rock.block_relief_m", "Joint opening (m)", 0.0, 0.0, 5.0, 0.01,
      group="rock",
      help="How wide the joints open. This is the parameter that turns one "
           "stone into an outcrop of separate blocks — a continuous mass with "
           "faces drawn on it still reads as one rock, and it is the visible "
           "GAP between blocks that reads as fractured bedrock. Needs joint "
           "sets."),
    P("rock.joint_taper", "Joint taper", 0.0, 0.0, 1.0, 0.01, group="rock",
      help="Narrows the joint openings with depth, so the blocks between them "
           "become blades that are wide apart at the top and nearly touching at "
           "the base. Dissolving water is used up as it works downward, which is "
           "how limestone pavements turn into the pinnacle forests of Shilin and "
           "Tsingy. Needs joint openings."),
    P("rock.corestone", "Corestones", 0.0, 0.0, 1.0, 0.01, group="rock",
      help="Rots the rock inward FROM the joints, so each block loses its "
           "corners first and a rounded core survives at the middle of it. This "
           "is how a granite tor becomes a stack of boulders with the old "
           "fracture grid still legible in how they sit — and it is not "
           "something ordinary weathering can reach, because that only ever sees "
           "the outside of the whole mass and leaves the blocks inside it "
           "prismatic. Turn it up far enough and the cores dissolve too, which "
           "is a gravel pile, also correct. Needs joint sets."),
    P("rock.settle_m", "Block settle (m)", 0.0, 0.0, 0.6, 0.01, group="rock",
      help="Lets separated blocks drop and shift into the space weathering took "
           "out, instead of staying in perfect alignment. Blocks that never move "
           "read as one stone with grooves cut into it, because the gaps between "
           "them stay dead parallel and exactly the same width. Needs joint "
           "openings or corestones."),

    P("rock.columns", "Columns", 0, 0, 600, 1, kind="int", group="rock",
      help="Split the stone into vertical columns: basalt. Cooling lava "
           "contracts into a polygonal crack network that propagates downward, "
           "which is the Giant's Causeway. 0 is off; 9-36 gives a colonnade."),
    P("rock.column_gap_m", "Column gap (m)", 0.08, 0.02, 3.0, 0.01, group="rock",
      help="Width of the crack between columns."),
    P("rock.column_stagger", "Column stagger", 0.18, 0.0, 0.8, 0.01, group="rock",
      help="How unevenly the columns end. At 0 they are sawn flat, which reads "
           "as an extruded shape rather than as stone."),

    P("rock.exfoliate", "Exfoliation", 0.0, 0.0, 1.0, 0.01, group="rock",
      help="Peels curved shells off the surface: onion-skin weathering. Granite "
           "domes release pressure as the rock above erodes away and split into "
           "sheets PARALLEL to the surface, which spall off. The signature is "
           "concentric steps, and no amount of tuning the ordinary weathering "
           "produces it because that pass has no notion of depth."),
    P("rock.shell_m", "Shell thickness (m)", 0.25, 0.05, 10.0, 0.05, group="rock",
      help="Thickness of one exfoliation sheet."),

    P("rock.veins", "Veins", 0, 0, 4, 1, kind="int", group="rock",
      help="Thin sheets of harder rock cutting through the mass — a quartz vein "
           "or a dyke. They survive the weathering that takes the rock around "
           "them and end up standing proud as fins, which is the most "
           "recognisable single detail on a weathered boulder. The eye follows a "
           "continuous line at far lower resolution than it reads a blob, so "
           "these carry further than their width suggests."),
    P("rock.vein_width_m", "Vein width (m)", 0.10, 0.03, 4.0, 0.01, group="rock",
      help="How thick one vein is. Below about 10 cm it is under two voxels and "
           "will come and go along its length."),
    P("rock.vein_hardness", "Vein hardness", 3.0, 1.0, 6.0, 0.1, group="rock",
      help="How much better than the surrounding rock the vein resists. 1 is no "
           "different, so nothing shows."),
    P("rock.rind", "Case-hardened rind", 0.0, 0.0, 1.0, 0.01, group="rock",
      help="A hard skin a few centimetres below the original surface, with "
           "softer rock behind it. Minerals wick outward and set near the face; "
           "when the skin is punctured the soft interior scoops out and leaves a "
           "hollow with a thin overhanging lip. That LIP is why tafoni read as "
           "tafoni — cavernous weathering on its own retreats the rim along with "
           "everything else and leaves a smooth bowl. Punctures are placed for "
           "you, or nothing would ever break through."),
    P("rock.rind_m", "Rind depth (m)", 0.08, 0.02, 3.0, 0.01, group="rock",
      help="How deep the hard skin goes."),
    P("rock.clasts", "Clasts", 0, 0, 6000, 5, kind="int", group="rock",
      help="Embeds separate lumps in a softer matrix: breccia. As the matrix "
           "weathers back, some clasts stand proud and others drop out and leave "
           "sockets, and that mix of bumps and holes is the texture. Note the "
           "size limit below — pebble conglomerate does not work here and is a "
           "job for the texture pass, not for geometry."),
    P("rock.clast_size_m", "Clast size (m)", 0.25, 0.08, 8.0, 0.01, group="rock",
      help="Typical lump across. Below about 15 cm they are one to three voxels "
           "and read as noise rather than as clasts."),
    P("rock.clast_hardness", "Clast hardness", 2.5, 1.0, 5.0, 0.1, group="rock",
      help="How much better than the matrix the clasts resist. About a third are "
           "flipped to weaker than the matrix whatever this says, because real "
           "breccia is a mix of lithologies and the ones that rot out are what "
           "make the sockets."),

    P("rock.flutes", "Solution flutes", 0.0, 0.0, 1.0, 0.01, group="rock",
      help="Runs rainwater down the outside of the stone and dissolves it in "
           "proportion to how much water passes, so grooves collect more water, "
           "deepen, and collect more. Near-vertical runnels that merge downhill "
           "— limestone karren. This is the only weathering here that knows "
           "which way is down; the curvature pass attacks a shape the same from "
           "every direction and can never make a directional mark. The flutes "
           "fade out lower down on their own, because once the water film gets "
           "deep enough it protects the rock instead of cutting it."),
    P("rock.flute_width_m", "Flute width (m)", 0.25, 0.10, 6.0, 0.01, group="rock",
      help="Spacing of the runnels. The centimetre-scale flutes on real "
           "limestone are far under one voxel and cannot be built here; this is "
           "the next size up, the decimetre runnels, which do read."),
    P("rock.pans", "Solution pans", 0.0, 0.0, 1.0, 0.01, group="rock",
      help="Hollows water cannot drain out of, dissolved flat-bottomed into the "
           "upper surfaces. They are found rather than placed: wherever the "
           "water running down the stone reaches a spot with nowhere lower to "
           "go, that is a pan."),
    P("rock.pan_depth_m", "Pan depth (m)", 0.2, 0.05, 6.0, 0.01, group="rock",
      help="How deep the pans cut. Flat floors and slightly overhung rims, which "
           "is what separates a solution pan from an ordinary dent."),
    P("rock.arch", "Arch", 0.0, 0.0, 1.0, 0.01, group="rock",
      help="Punches a hole clean through the stone and lets the weathering "
           "smooth the underside of the span: an arch, a window, a sea bridge. "
           "Nothing else here can make a through-going hole — hollows eaten from "
           "both sides only meet by luck, and a slab thin enough for them to "
           "meet usually breaks first. Wants a thin, tall stone to work on, and "
           "is refused below about 4 m because a small one is not credible."),

    P("rock.bury", "Buried fraction", 0.22, 0.0, 0.7, 0.01, group="rock",
      help="How much of the stone sits below ground. Everything under z=0 is cut "
           "away, so this controls how settled it looks rather than adding volume."),
    P("rock.rubble", "Rubble", 0.15, 0.0, 1.0, 0.01, group="rock",
      help="Loose stones scattered around the base."),
    P("materials.rock", "Rock", "rock", kind="choice", group="rock",
      choices=("rock", "bedrock", "gravel", "sand", "clay"),
      help="What the stone is made of, which decides its colour and how much "
           "it varies. 'rock' is the neutral grey; 'bedrock' is darker and "
           "reads as a fresh unweathered face; 'sand' is sandstone, and is what "
           "every desert and arch spec uses; 'gravel' suits scree and cobbles, "
           "because its high per-voxel jitter is what makes a heap read as "
           "loose stones; 'clay' is for badland and mudstone bluffs.\n\n"
           "PERMAFROST AND SNOW WERE OFFERED HERE AND HAVE BEEN REMOVED. They "
           "are engine materials, so they were valid, but neither is a rock: "
           "permafrost is frozen ground and snow is snow, and a boulder carved "
           "out of either is a landform nobody wants. Nothing in the library "
           "ever selected them. Snow ON a stone is a job for the renderer or a "
           "surface pass, not for what the stone is made of."),

    P("tuft.stems", "Stems", 24, 1, 600, 1, kind="int", group="tuft",
      help="How many blades or stems rise from the root crown. Grass wants "
           "dozens; a flowering plant wants a handful."),
    P("tuft.spread_m", "Root spread (m)", 0.06, 0.0, 6.0, 0.01, group="tuft",
      help="Radius of the patch the stems root in. Small keeps it a tuft; large "
           "makes a loose stand."),
    P("tuft.splay_deg", "Splay", 18.0, 0.0, 80.0, 1.0, group="tuft",
      help="How far from vertical a stem leaves the ground."),
    P("tuft.arc", "Arc", 0.55, 0.0, 1.0, 0.01, group="tuft",
      help="How far a stem bends toward horizontal along its length. 0 is a "
           "rigid spike, 1 lays the tip right over. Weighted toward the tip, so "
           "the base stays upright whatever this is."),
    P("tuft.width_m", "Stem width (m)", 0.02, 0.005, 1.5, 0.005, group="tuft",
      help="Thickness at the root. At 2 cm a value of 0.02 is a one-voxel "
           "thread, which is the real width of a blade of grass and the reason "
           "these are authored at 2 cm at all."),
    P("tuft.taper", "Taper", 0.5, 0.0, 1.0, 0.01, group="tuft",
      help="Tip thickness as a fraction of the root."),
    P("tuft.wander", "Wander", 0.35, 0.0, 1.5, 0.01, group="tuft",
      help="Sideways drift along a stem, so blades curve in plan rather than "
           "running dead straight out from the centre."),
    P("tuft.length_var", "Length spread", 0.3, 0.0, 0.8, 0.01, group="tuft",
      help="How much stems differ in length within one tuft. Zero makes a fan "
           "of identical copies, which reads as manufactured."),
    P("tuft.base_m", "Root crown (m)", 0.05, 0.0, 4.0, 0.01, group="tuft",
      help="A flat disc joining every stem at the ground. Without it each stem "
           "is a separate piece, which is botanically true and wrong for an "
           "asset that gets stamped into a world and dug out of it. Never "
           "smaller than the root spread, whatever this says — a crown that "
           "does not reach the stems is not doing its job. Set 0 to omit it."),
    P("tuft.head", "Head", "none", kind="choice", group="tuft",
      choices=("none", "spike", "bloom", "plume"),
      help="What tops a stem. None is grass; spike is a reed's seed head; bloom "
           "is a flower; plume is a feathery seed head."),
    P("tuft.head_m", "Head width (m)", 0.12, 0.01, 8.0, 0.01, group="tuft",
      help="Width of the bloom, spike or plume — across, not out from the "
           "centre. A daisy is about 0.12; a big jungle bloom 0.4."),
    P("tuft.head_frac", "Head length", 0.22, 0.02, 0.6, 0.01, group="tuft",
      help="How much of the stem's top the spike or plume occupies."),
    P("tuft.head_share", "Stems with a head", 1.0, 0.0, 1.0, 0.05, group="tuft",
      help="Fraction of stems that carry one. Below 1 the rest are plain stems, "
           "which is how a flowering plant gets its leaves for free."),
    # ONE MENU. `materials.stem` and `materials.head` used to be two, and the
    # mismatch between them was silent -- see `_PLANT_MATERIALS`.
    P("materials.stem", "Stem", "grass", kind="choice", group="tuft",
      choices=_PLANT_MATERIALS),
    # THE BLOOM PALETTE WAS SEVEN ENTRIES AND SIX OF THEM WERE FOLIAGE.
    # `leaf_blossom` is a pale cherry pink, `leaf_autumn` a brown-orange,
    # `leaf_dry` a straw yellow, and the other four are green, tan and snow. A
    # wildflower list written against that has no blue, no violet, no scarlet
    # and no saturated yellow -- which is most of a meadow. A poppy came out
    # pink, a cornflower came out pink, a knapweed came out pink, and three
    # species that are unmistakable in life were one colour on a hillside.
    #
    # THE SEVEN ADDED HERE ARE ALREADY IN THE ENGINE. They are creature
    # materials -- ids 27-44, appended for the fish and the birds and carried
    # in `forge/palette.py` -- so this is a WIDER MENU AND NOT A MATERIAL
    # APPEND: `forge.cli selftest` gates on every authored material existing in
    # `kMaterialCount`, and every one of these does. Nothing that already
    # authored a head changes, because no default moved and no existing choice
    # was removed, so the canonical JSON of every spec in the library is
    # untouched and nothing reseeds.
    #
    # They are named for what they look like rather than for what wears them,
    # which is the rule `materials.py` already states: a kingfisher's back and
    # a gentian are the same turquoise, and inventing a second one so the name
    # could read "petal" would be tidiness of naming beating the picture.
    P("materials.head", "Head", "leaf_blossom", kind="choice", group="tuft",
      choices=_PLANT_MATERIALS),

    # --- fish ---------------------------------------------------------------
    #
    # The whole group is written against the fact that a fish here is twenty to
    # forty voxels long. Nothing that moves the outline by less than one voxel
    # over its whole range is a slider; that is why there is no superformula and
    # no per-fin curvature. `docs/fish-shape-research.md` lists what was left
    # out and what it would have bought.
    P("fish.length_m", "Length (m)", 0.28, 0.03, 30.0, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Snout to the wrist of the tail — standard length, the measurement "
           "every fish reference is quoted in, not including the tail fin. At "
           "the 1 cm lattice this is also the voxel count: a 0.28 m trout is 28 "
           "voxels long.\n\n"
           "NO SPECIES IS AUTHORED UNDER 0.20 m (owner, 2026-08-13): below "
           "that a fish cannot hold a marking two voxels wide, and enlarging "
           "the animal was chosen over adding a lattice tier finer than 1 cm. "
           "The ceiling is 30 m because a blue whale is 25. IT USED TO BE 3, "
           "and every large species was silently clamped to it at authoring "
           "time — a 25 m whale came out 2.9 m long, the spec on disk said "
           "3.0, and nothing downstream had anything to complain about. Voxel "
           "size is per species and scales with the animal; see "
           "`tools/fishprobe.py --lattice`."),
    P("fish.depth_ratio", "Body depth", 0.24, 0.05, 0.80, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Deepest part of the body divided by its length, and the single "
           "biggest lever on what kind of fish it is. Real values: an eel is "
           "0.06-0.10, a trout or a herring 0.18-0.25, a perch 0.30, a bream or "
           "a sunfish 0.40-0.50, an angelfish over 0.6. Morphometrics calls "
           "these anguilliform, fusiform, compressiform — this slider is that "
           "axis."),
    P("fish.width_ratio", "Body width", 0.58, 0.08, 1.80, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Width across the fish divided by its depth. Below about 0.35 it is "
           "flattened side to side (a bream, a reef fish); around 0.9-1.1 it is "
           "round in section (an eel, a catfish); above 1.2 it is flattened top "
           "to bottom (a ray, a flatfish, a bullhead)."),
    P("fish.depth_at", "Deepest point", 0.36, 0.12, 0.80, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="How far back the body is deepest, as a fraction of its length. "
           "Around 0.35 for most fish; pushed back past 0.5 for an ambush "
           "predator that carries its mass toward the tail, which is what makes "
           "a pike read as a pike."),
    P("fish.fullness", "Swell", 3.0, 0.5, 12.0, 0.1, group="fish",
      kinds=_SWIM_KINDS,
      help="How quickly the body swells to its full depth and how quickly it "
           "falls away again. Low is a long even body that tapers gently; high "
           "is a short deep one that reaches full depth just behind the head. "
           "Deliberately independent of the deepest point above — the obvious "
           "curve moves both at once and tuning a species then means chasing "
           "one slider with another."),
    P("fish.snout", "Snout depth", 0.30, 0.05, 0.95, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Depth at the very front, as a fraction of the maximum. Low is a "
           "pointed head, high is a blunt one. An eel sits near 0.85 because it "
           "is the same thickness all the way along."),
    P("fish.peduncle", "Tail wrist", 0.30, 0.05, 0.95, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Depth where the tail fin joins, as a fraction of the maximum. A "
           "fast open-water fish has a very slim wrist (0.12-0.18); a slow "
           "bottom fish barely narrows at all."),
    P("fish.belly", "Belly share", 0.52, 0.20, 0.80, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="How much of the depth is below the body's axis rather than above "
           "it. Above 0.5 gives a deep round belly with a flatter back; below "
           "0.5 gives an arched back over a flat underside, which is what a "
           "bottom-living fish has."),
    P("fish.width_follow", "Width falloff", 1.15, 0.20, 2.50, 0.05, group="fish",
      kinds=_SWIM_KINDS,
      help="How hard the fish flattens toward its tail. High values give the "
           "knife-thin wrist a fast swimmer has; 1.0 keeps the section the same "
           "shape all the way back, which is an eel."),
    P("fish.section", "Section shape", 2.0, 1.0, 4.0, 0.05, group="fish",
      kinds=_SWIM_KINDS,
      help="Cross-section roundness, as a superellipse exponent. 2 is an "
           "ellipse. Near 1.2 the section is a diamond and the fish has a "
           "knife-edged back and belly (a bream, a surgeonfish); near 3 it is a "
           "rounded box and the fish is a tube (a catfish). This is the one "
           "thing from the superformula literature that still moves a whole "
           "voxel at this size."),
    P("fish.head_frac", "Head length", 0.26, 0.08, 0.50, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Snout to gill cover, as a fraction of body length. Nothing is drawn "
           "for the gills — this positions the eye and the pectoral fins, which "
           "is all a head is at this size."),
    P("fish.head_width", "Head span", 0.0, 0.0, 0.60, 0.01, group="fish",
      kinds=("fish",),
      help="How far the HEAD sticks out sideways, measured tip to tip as a "
           "fraction of body length. 0 leaves the head the width the body's "
           "own width profile gives it, which is right for every fish except "
           "one group.\n\n"
           "THIS IS THE HAMMERHEAD, and it is the only feature in the group "
           "the body loft could not say. Everything else about a fish's width "
           "follows its depth — one number, `Width falloff`, decides how the "
           "whole animal flattens from nose to tail — so a head that is wider "
           "than the body behind it was not expressible at all, and the "
           "`hammerhead` keyword produced a shark with hammerhead proportions "
           "and an ordinary head.\n\n"
           "Published spans are quoted against TOTAL length (tail included) "
           "and this slider is against BODY length (snout to tail wrist), "
           "which on a shark with a 31% tail is about 1.31 times larger: a "
           "scalloped hammerhead's 30% of total length is 0.39 here. "
           "`tools/fishprobe.py --head` measures the built animal both ways so "
           "the published figure can be checked directly. The fore-and-aft "
           "depth of the hammer is DERIVED at a third of its span rather than "
           "authored, because a cephalofoil is a wing and its chord goes with "
           "its span; a slider for it could only ever be set wrong."),

    P("fish.caudal_plane", "Tail plane", "vertical", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=_CAUDAL_PLANES,
      help="Which way the tail lies. A fish's is VERTICAL and beats side to "
           "side; a whale's or a dolphin's fluke is HORIZONTAL and beats up "
           "and down. It is one parameter rather than a second generator "
           "because everything else about a fluke — how it flares out of the "
           "wrist, its outline, the minimum a lobe may be — is what a tail fin "
           "already does.\n\n"
           "A real fluke also has a median NOTCH and this does not draw one: "
           "the notch is about 5% of the fluke's span and the span about 25% "
           "of body length, so the notch is 1.2% of the animal — under a voxel "
           "on anything short of about 120 voxels long. Use the tail notch "
           "slider if a species is big enough to hold one."),
    P("fish.caudal_upper", "Upper tail lobe", 0.0, 0.0, 1.0, 0.01, group="fish",
      kinds=("fish",),
      help="How much further aft the UPPER lobe of the tail reaches than the "
           "lower. 0 is a bony fish, where the two are equal. Sharks are "
           "heterocercal and this is a strong silhouette cue: measured as the "
           "ratio of the two lobes, a requiem shark is about 3:1, a nurse "
           "shark 5:1 or more, and a great white about 1.1:1 — nearly "
           "symmetric, which is why a slider says it better than a tail-shape "
           "entry could."),
    P("fish.caudal_shape", "Tail shape", "forked", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=_CAUDAL_SHAPES,
      help="The tail fin's outline. In real fish this tracks how the animal "
           "swims: a deeply forked or crescent tail is a cruiser that never "
           "stops, a truncate or rounded one is a fish that accelerates and "
           "turns in cover. LUNATE AND EMARGINATE ARE NOT SEPARATE ENTRIES — "
           "they are a deep and a shallow fork, so they come from 'forked' plus "
           "the notch slider below, and an entry that duplicated a slider "
           "position would be a choice that silently ignored it."),
    P("fish.caudal_len", "Tail length", 0.20, 0.0, 0.60, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Tail fin length as a fraction of the body's. 0 removes it."),
    P("fish.caudal_span", "Tail span", 1.15, 0.30, 3.00, 0.05, group="fish",
      kinds=_SWIM_KINDS,
      help="Tail fin height as a multiple of the body's maximum depth. Above "
           "about 1.6 with a deep notch it reads as the crescent tail of a tuna "
           "or a shark."),
    P("fish.caudal_fork", "Tail notch", 0.45, 0.0, 0.92, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="How deep the V is cut into the trailing edge, as a fraction of the "
           "fin's length. ONLY APPLIES TO THE 'forked' SHAPE. Under 0.25 is an "
           "emarginate tail (a shallow notch, a perch); over 0.6 with a large "
           "span is lunate (a crescent, a tuna)."),

    P("fish.dorsal_shape", "Dorsal fin", "triangular", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=_DORSAL_SHAPES,
      help="Outline of the fin on the back. These differ in WHERE along the fin "
           "the height is, which no single slider can say: 'sail' is broad and "
           "high through the middle, 'spiny' is tallest at its leading edge and "
           "rakes back, 'ridge' is the low even fold an eel or a catfish has."),
    P("fish.dorsal_start", "Dorsal position", 0.38, 0.05, 0.85, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Where the dorsal fin begins, as a fraction of the body length back "
           "from the snout."),
    P("fish.dorsal_len", "Dorsal length", 0.26, 0.03, 0.85, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="How much of the back it runs along. Long and low is an eel; short "
           "and tall is a perch."),
    P("fish.dorsal_height", "Dorsal height", 0.38, 0.0, 2.0, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Fin height as a fraction of the body's maximum depth. 0 removes it. "
           "Under about 0.15 on a small fish it is one voxel and has no shape "
           "left, so it reads as a rough edge rather than as a fin."),
    P("fish.dorsal2_height", "Second dorsal height", 0.0, 0.0, 1.0, 0.01,
      group="fish", kinds=("fish",),
      help="A second fin further back on the spine. 0 is off, and off is right "
           "for every bony fish here. Most SHARKS have one and it is usually a "
           "nub — 2% of total length on a hammerhead against the first "
           "dorsal's 13%, and 'minute' on a great white — but two bumps on a "
           "back reads as a shark where one bump reads as a fish. The fin "
           "height floor below is the only reason a fin that small survives."),
    P("fish.dorsal2_start", "Second dorsal position", 0.62, 0.10, 0.95, 0.01,
      group="fish", kinds=("fish",)),
    P("fish.dorsal2_len", "Second dorsal length", 0.10, 0.03, 0.40, 0.01,
      group="fish", kinds=("fish",)),
    P("fish.adipose", "Adipose fin", False, kind="bool", group="fish",
      kinds=("fish",),
      help="The small fleshy bump between the dorsal and the tail that trout, "
           "salmon and charr have and almost nothing else does. Three voxels, "
           "and the only mark that separates a trout from every other slim "
           "brown fish in a shoal."),
    P("fish.anal_height", "Anal fin height", 0.32, 0.0, 1.2, 0.01, group="fish",
      kinds=("fish",),
      help="The fin under the tail end, as a fraction of body depth. Its "
           "POSITION is not a slider: it ends just in front of the tail wrist "
           "on every fish that has one."),
    P("fish.anal_len", "Anal fin length", 0.16, 0.03, 0.50, 0.01, group="fish",
      kinds=("fish",)),
    P("fish.pectoral", "Pectoral fins", 0.26, 0.0, 1.2, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="The pair behind the head, as a fraction of body depth. These are "
           "the only fins that stick out SIDEWAYS, so from anywhere but dead "
           "broadside they are most of what says 'animal' rather than "
           "'lozenge'. 0 removes them."),
    P("fish.pectoral_aspect", "Pectoral chord", 1.20, 0.15, 3.0, 0.05,
      group="fish", kinds=_SWIM_KINDS,
      help="How long the paired fins are fore-and-aft, as a fraction of how "
           "far they stick out. Around 1 is a fish's pectoral — about as long "
           "as it is wide. Low values give the long narrow blade a whale has "
           "for a flipper: a humpback's is 31% of its body length and 7% wide, "
           "a ratio of about 0.24, and it is the most recognisable limb in the "
           "sea."),
    P("fish.pelvic", "Pelvic fins", 0.18, 0.0, 1.0, 0.01, group="fish",
      kinds=("fish",),
      help="A small pair under the belly. At this size they read as a notch in "
           "the underside rather than as fins, and that notch is what stops the "
           "belly being a smooth arc."),
    P("fish.barbels", "Barbels", 0, 0, 4, 1, kind="int", group="fish",
      kinds=("fish",),
      help="Whiskers off the snout: a catfish, a carp, a sturgeon. Drawn as "
           "face-connected threads starting on a snout voxel, so they are part "
           "of the fish rather than a second asset floating in front of it."),
    P("fish.barbel_len", "Barbel length", 0.10, 0.0, 0.60, 0.01, group="fish",
      kinds=("fish",),
      help="As a fraction of body length. Under about 0.05 there is nothing to "
           "draw."),
    P("fish.fin_thick", "Fin thickness", 1, 1, 3, 1, kind="int", group="fish",
      kinds=_SWIM_KINDS,
      help="Fin thickness in voxels. 1 is right for anything under about half a "
           "metre; a bigger fish wants 2 or its fins disappear edge-on."),
    P("fish.section_tail", "Section at the tail", 2.0, 1.0, 4.0, 0.05,
      group="fish", kinds=_SWIM_KINDS,
      help="Cross-section roundness at the tail wrist, where the slider above "
           "sets it at the deepest point. Equal values give one section shape "
           "all the way along, which is a fish. A CETACEAN NEEDS THEM "
           "DIFFERENT: the literature gets a whale's diameter by dividing its "
           "girth by pi — the trunk really is a barrel — while the tailstock "
           "is explicitly elliptical, to the point that modelling it as a cone "
           "gives the wrong volume. So a dolphin is 2.4 at the middle and 1.3 "
           "at the wrist: a barrel that becomes a vertical blade."),
    P("fish.fin_min_vox", "Smallest fin", 2.0, 0.0, 8.0, 0.5, group="fish",
      kinds=_SWIM_KINDS,
      help="No fin is drawn shorter than this many voxels, whatever its "
           "authored fraction works out to. ON A LARGE ANIMAL THIS FLOOR IS "
           "THE ONLY REASON THE FIN EXISTS. Fin size is authored as a share of "
           "body depth, but the share that identifies a species does not scale "
           "with the animal: a blue whale's dorsal fin is 1.0-1.4% of its "
           "length and is exactly what separates a blue from a fin from a sei, "
           "and faithfully proportioned it is under half a voxel. A minke's is "
           "4% and a dolphin's 8-12%, so this floor is invisible on those. "
           "Applied to the fin's peak, so the outline still tapers."),
    P("fish.blowhole", "Blowhole", 0.0, 0.0, 3.0, 1.0, group="fish",
      kinds=("cetacean",),
      help="Radius in voxels of a dark mark on top of the head; 0 is off. A "
           "cetacean breathes air and a fish does not, and this is the mark "
           "that says so. Placed at 7.5% of the body length back from the "
           "snout — the blowhole-to-dorsal-fin distance is the standard field "
           "proxy for a dolphin's total length, so it is one of the "
           "best-pinned landmarks on the animal."),
    P("fish.eye_patch", "Eye patch", 0.0, 0.0, 4.0, 1.0, group="fish",
      kinds=_SWIM_KINDS,
      help="Radius in voxels of a pale patch around the eye, with the pupil "
           "drawn on top of it; 0 is off. This is the orca, and it is the "
           "strongest single mark on any animal here. Drawn as a lozenge "
           "roughly twice as long as it is tall.\n\n"
           "THE '21.8 BY 5.9 cm ON A 6 m ANIMAL' THIS ROW USED TO QUOTE HAS "
           "NO SOURCE — nobody has published orca eye patches in absolute "
           "units, and those two figures match a pair of dimensionless "
           "diversity indices in a saddle-patch paper. What is published is a "
           "ratio: patch length is 0.37–0.41 of the blowhole-to-dorsal-fin "
           "distance on the large-patched Antarctic types (Durban et al. "
           "2016, n=19). No aspect ratio exists, so the one drawn is what the "
           "lattice can hold. Real orca patches differ left from right on "
           "about half of animals; at three voxels there is no asymmetry to "
           "express that would not read as a mistake."),
    P("fish.eye", "Eye", 1.0, 0.0, 3.0, 1.0, group="fish", kinds=_SWIM_KINDS,
      help="Eye radius in voxels; 0 turns it off. Two voxels a side, and they "
           "do more than any other two in the asset — a voxel animal without an "
           "eye reads as an object, and with one it reads as facing somewhere. "
           "A pale voxel is placed in front of the dark one, because a dark eye "
           "on an olive or brown head vanishes without a contrast partner."),

    P("fish.back_frac", "Dark back", 0.34, 0.0, 1.0, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Share of the body's depth, measured from the top down, that carries "
           "the back colour. Countershading — dark above, pale below — is on "
           "nearly every fish in open water for the same reason it is on "
           "military aircraft: it cancels the light gradient and flattens the "
           "animal against whatever is behind it. It is also the only thing "
           "that gives a voxel fish a top and a bottom, because the silhouette "
           "does not at this size."),
    P("fish.belly_frac", "Pale belly", 0.26, 0.0, 1.0, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="The same, measured from the underside up."),

    # --- shaped colour boundaries -------------------------------------------
    #
    # The two rows above draw the dark back and the pale belly as LEVEL LINES
    # running the length of the animal. That is right for a fish and wrong for
    # every dolphin: the three most recognisable colour schemes in the sea are
    # not bands, blotches or patches, they are boundaries with a SHAPE. These
    # three rows bend the two lines and nothing else, which is why they are
    # three rows rather than a fourth colour field.
    P("fish.field_curve", "Boundary shape", "flat", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=_FIELD_CURVES,
      help="Whether the dark back and the pale belly meet the flank along a "
           "level line or a curved one.\n\n"
           "FLAT is a level line at the height the two rows above set, which "
           "is what a fish wears and what everything here wore until now. "
           "CAPE dips the BACK's lower edge down onto the flank at one place "
           "and lets it rise again — the dark saddle every dolphin carries, "
           "which points down at the dorsal fin. FLAME lifts the BELLY's upper "
           "edge up the flank at one place — the white blaze that flares up an "
           "orca's side behind its middle, and the reason an orca reads as two "
           "white shapes from the side rather than one. HOURGLASS does both at "
           "the same station, so the dark and the white meet and pinch the "
           "flank out between them; that crossing is the common dolphin's "
           "criss-cross and it is the same curve twice, not a third mechanism."),
    P("fish.curve_at", "Boundary waist", 0.55, 0.10, 0.95, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Where the curve reaches its extreme, as a fraction of body length "
           "back from the snout. A delphinid cape dips under the dorsal fin "
           "(0.40-0.50); an orca's ventral flame flares at 0.70-0.75."),
    P("fish.curve_amount", "Boundary reach", 0.0, 0.0, 0.70, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="How far the boundary moves at that station, as a share of the "
           "body's depth there. 0 leaves it flat whatever shape is chosen "
           "above, which makes this the off switch for the whole mechanism.\n\n"
           "IT HAS TO BE WORTH TWO VOXELS. The flank of a bottlenose dolphin "
           "at its authored size is about twelve voxels deep, so 0.20 moves "
           "the boundary two and a half voxels and 0.08 moves it one — which "
           "is not a curve, it is a ragged line. `tools/fishprobe.py --marks` "
           "prints the movement in voxels per species and flags anything under "
           "two."),

    # --- sex ----------------------------------------------------------------
    #
    # SEX RESEEDS, ON PURPOSE, AND THAT IS THE OPPOSITE OF WHAT `bird.pose`
    # DOES. A perched raven and a flying raven are one animal in two postures,
    # so the pose is excluded from the seeding hash (`SEED_INVARIANT`) and both
    # come out the same raven. A male orca and a female orca are two animals.
    # There is no individual that is "the same whale, but female", so seed 7
    # male and seed 7 female are two different whales and this field is
    # deliberately NOT in `SEED_INVARIANT`.
    P("fish.sex", "Sex", "unsexed", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=_SEXES,
      help="Which sex of the species to draw. UNSEXED is the species average "
           "and is what a spec carries until someone measures a difference; "
           "MALE and FEMALE move the three measurements below apart.\n\n"
           "The authored numbers describe the AVERAGE of the two sexes, and "
           "the three ratios below are each a male-to-female ratio, applied as "
           "the square root either way — so male divided by female is exactly "
           "the ratio and neither sex is the default. That rule is why three "
           "species were re-authored when this arrived: `orca`, `whale-shark` "
           "and `sperm-whale` were each drawn from a male reference and said "
           "so in their own notes, so their authored numbers were one sex "
           "wearing the species' name."),
    P("fish.sex_length", "Male:female length", 1.0, 0.40, 2.20, 0.01,
      group="fish", kinds=_SWIM_KINDS,
      help="Adult body length of the male divided by the female's. 1.0 is no "
           "difference and is right for most of this library. Under 1 means "
           "the FEMALE is the larger animal, which is the usual way round for "
           "sharks: a whale shark is 8-9 m male against 14.5 m female, a ratio "
           "of 0.60 and the largest sexual difference of any species here. "
           "A sperm whale is the other way, 16 m against 11."),
    P("fish.sex_dorsal", "Male:female dorsal", 1.0, 0.40, 3.00, 0.01,
      group="fish", kinds=_SWIM_KINDS,
      help="Height of the back fin, male divided by female. The orca is the "
           "reason this exists and is the only species here that needs it: the "
           "male's fin is 22-30% of his body length and the female's 13-18% of "
           "hers, a ratio near 1.7, and it is the single most obvious "
           "difference between two animals of one species anywhere in this "
           "library."),
    P("fish.sex_pectoral", "Male:female flipper", 1.0, 0.40, 2.50, 0.01,
      group="fish", kinds=_SWIM_KINDS,
      help="How far the paired fins reach, male divided by female. Orca "
           "flippers are about 20% of body length in males against 11-13% in "
           "females."),
    P("fish.pattern", "Marking", "none", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=_FISH_PATTERNS,
      help="ONE mark, not several: a flank twelve voxels deep cannot hold two "
           "without them reading as noise. A horizontal STRIPE is the open-water "
           "schooling mark; vertical BARS break the outline against weed and "
           "reef; SPOTS and MOTTLE are the freshwater camouflage; a SADDLE is "
           "blotches over the back only, which is what breaks the outline seen "
           "from above by a bird. STRIPE is ONE lateral band; STRIPES is "
           "several, which is a different fish -- a bluestripe snapper wears "
           "four and a single band would be a different species."),
    # CLASPERS (owner, 2026-08-15). Male sharks and rays carry a pair of rods
    # behind the pelvic fins, and they are the most visible external difference
    # between the sexes on the largest animals in the library. Measured at 8.8%
    # of total length on a mature whale shark, which is ~10 voxels on a great
    # white -- well clear of the three-voxel floor, so the lattice was never
    # the reason they were absent.
    #
    # Drawn only when `fish.sex` is `male`. A female or unsexed spec carrying a
    # non-zero value here draws nothing, which is correct rather than a silent
    # no-op: the parameter says how long the claspers ARE on this species, not
    # whether this individual has them.
    P("fish.claspers", "Clasper length", 0.0, 0.0, 0.25, 0.005, group="fish",
      kinds=_SWIM_KINDS,
      help="Paired rods behind the pelvic fins, as a share of body length. "
           "MALES ONLY -- a female or unsexed individual of the same species "
           "draws none. Sharks and rays have them; bony fish do not, so this "
           "is 0 for most of the library."),
    P("fish.pattern_count", "Marking count", 6, 1, 24, 1, kind="int", group="fish",
      kinds=_SWIM_KINDS,
      # NAMED, because it was not read by every pattern and nothing said so.
      # `stripe` draws ONE band by definition and ignores this, which is
      # correct -- but `bluestripe-snapper` shipped carrying 6 here and drawing
      # one stripe, and the parameter looked wired up. The fix was not to make
      # `stripe` obey it: all ten stripe species carry 6 because 6 is the
      # DEFAULT, so obeying it would have given six bands to nine fish that
      # never asked. `stripes` is the pattern for a fish that wears several.
      help="Number of bars, of stripes under the STRIPES pattern, or roughly a "
           "third of the number of spots. The single STRIPE pattern draws one "
           "band and does not read this."),
    P("fish.pattern_width", "Marking width", 0.22, 0.02, 0.90, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="For a stripe, its thickness as a share of body depth. For bars, the "
           "share of each bar's spacing that is bar rather than gap."),
    P("fish.pattern_pos", "Stripe height", 0.50, 0.0, 1.0, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Where the stripe sits: 0 is the belly line, 1 the top of the back. "
           "Stripe only."),
    P("fish.pattern_scale", "Blotch size", 0.20, 0.02, 1.0, 0.01, group="fish",
      kinds=_SWIM_KINDS,
      help="Spot and blotch size as a fraction of body length. Spots, mottle "
           "and saddle only."),
    P("fish.pattern_strength", "Blotch coverage", 0.5, 0.0, 1.0, 0.01,
      group="fish", kinds=_SWIM_KINDS,
      help="The EXACT share of the fish the blotches cover, taken as a quantile "
           "of the noise rather than as a threshold on it. Mottle and saddle "
           "only. A plain threshold means whatever the noise happened to do, "
           "which is the defect that left the rock weathering pass removing 20 "
           "voxels out of 90,000 while reporting success."),

    P("materials.fish_back", "Back", "skin_olive", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=materials.FISH_NAMES),
    P("materials.fish_flank", "Flank", "skin_silver", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=materials.FISH_NAMES),
    P("materials.fish_belly", "Belly", "skin_pale", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=materials.FISH_NAMES),
    P("materials.fish_fin", "Fins", "skin_olive", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=materials.FISH_NAMES),
    P("materials.fish_pattern", "Marking", "skin_dark", kind="choice",
      group="fish", kinds=_SWIM_KINDS, choices=materials.FISH_NAMES),
    P("materials.fish_eye", "Eye", "skin_dark", kind="choice", group="fish",
      kinds=_SWIM_KINDS, choices=materials.FISH_NAMES),
    P("materials.fish_patch", "Eye patch", "skin_pale", kind="choice",
      group="fish", kinds=_SWIM_KINDS, choices=materials.FISH_NAMES),

    # --- detail entity ------------------------------------------------------
    #
    # NONE OF THIS IS READ BY ANY CODE, AND THAT IS DELIBERATE. Spawning fish
    # into water bodies is a job for worldgen, the same way scattering trees is,
    # and the `placement` group above has been authored-and-unread since the
    # library began for exactly the same reason. What these rows are is the
    # SPECIFICATION a spawner will be written against, stated in the same file
    # the shape is stated in, so the two cannot drift apart.
    #
    # The one thing here that is not like `placement`: a fish is a DETAIL
    # ENTITY. It has no saved state, nothing that happens to it is recorded, and
    # it is deleted shortly after the player leaves. That is a promise about
    # cost, and it is why a shoal of forty is affordable when forty trees would
    # not be.
    P("detail.entity_class", "Entity class", "detail", kind="choice",
      group="detail", kinds=_SWIM_KINDS, choices=("detail", "persistent"),
      help="'detail' means nothing about this individual is saved: it is "
           "spawned from (species, seed) when the player is near, and when it "
           "despawns it is gone. Anything that has to survive being left and "
           "come back the same is 'persistent' and costs a save slot."),
    P("detail.despawn_m", "Despawn distance (m)", 45.0, 5.0, 1000.0, 5.0,
      group="detail", kinds=_SWIM_KINDS,
      help="How far the player has to get before this individual is removed. "
           "Water is murky and a 25 cm fish is under a voxel of screen space "
           "well before this, so the number is about cost, not visibility."),
    P("detail.despawn_delay_s", "Despawn delay (s)", 4.0, 0.0, 120.0, 0.5,
      group="detail", kinds=_SWIM_KINDS,
      help="Grace period after the player passes the distance above. Without "
           "one, walking along a bank at the despawn radius makes the shoal "
           "flicker in and out."),
    P("detail.school_min", "Shoal size, least", 1, 1, 200, 1, kind="int",
      group="detail", kinds=_SWIM_KINDS),
    P("detail.school_max", "Shoal size, most", 8, 1, 400, 1, kind="int",
      group="detail", kinds=_SWIM_KINDS,
      help="A shoal is spawned as one decision: N individuals of this species "
           "from consecutive seeds, so they vary the way the `variation` group "
           "says. Solitary species set both ends to 1."),
    P("detail.school_radius_m", "Shoal spread (m)", 2.5, 0.2, 500.0, 0.5,
      group="detail", kinds=_SWIM_KINDS,
      help="Radius the shoal occupies. Tight for a bait ball, wide for a few "
           "trout holding station in a pool."),
    P("detail.water", "Water type", "any", kind="choice", group="detail",
      kinds=_SWIM_KINDS,
      choices=("any", "ocean", "river", "lake", "shallow", "reef"),
      help="Which water bodies this species may spawn in. 'shallow' is the "
           "margin of anything; 'reef' is shallow salt water, which the world "
           "does not classify yet and which this row is a request for."),
    P("detail.depth_min_m", "Depth below surface, least (m)", 0.3, 0.0, 200.0,
      0.1, group="detail", kinds=_WET_KINDS,
      help="How far under the surface the fish holds. A surface-feeding minnow "
           "is near 0; a bottom fish sets both of these deep."),
    P("detail.depth_max_m", "Depth below surface, most (m)", 6.0, 0.1, 2000.0,
      0.5, group="detail", kinds=_WET_KINDS),
    P("detail.min_water_depth_m", "Needs water at least (m) deep", 0.5, 0.05,
      100.0, 0.05, group="detail", kinds=_WET_KINDS,
      help="Water shallower than this holds none of this species. This is the "
           "gate that keeps a pike out of a puddle."),
    P("detail.per_100m2", "Individuals per 100 m² of water", 3.0, 0.0, 300.0,
      0.5, group="detail", kinds=_SWIM_KINDS,
      help="Expected number over 100 square metres of water SURFACE, before the "
           "biome weights are applied. Surface area rather than volume because "
           "that is what a worldgen pass can cheaply measure per chunk."),

    # --- bird ---------------------------------------------------------------
    #
    # A bird here is twenty to ninety voxels long. Same rule as the fish group:
    # nothing that moves the outline by less than one voxel over its whole
    # range is a slider, and `tools/birdprobe.py` prints DEAD for anything that
    # turns out not to. `docs/bird-shape-research.md` lists what was left out
    # and what it would have bought.
    #
    # WHAT MAKES THIS GROUP DIFFERENT FROM THE FISH GROUP. A fish is one solid
    # and every parameter shapes it. A bird is six parts at angles to each
    # other, and the five `*_frac` rows below -- which say how the length is
    # divided between bill, head, neck, body and tail -- are, between them, the
    # strongest thing in the file. Birders identify birds by proportion and
    # stance before colour and have a word for it; those five rows plus
    # `posture_deg` are that.
    P("bird.length_m", "Length (m)", 0.24, 0.15, 2.50, 0.01, group="bird",
      kinds=("bird",),
      help="Bill tip to tail tip, which is the measurement every field guide "
           "quotes. At the 1 cm lattice this is also the voxel count: a 0.24 m "
           "robin is 24 voxels long.\n\n"
           "THE FLOOR IS 15 cm AND EVERY SPECIES IN THE LIBRARY IS AT LEAST "
           "20 cm, INCLUDING THE ONES THAT ARE NOT. A goldcrest is 9 cm and a "
           "wren is 10, and at 9 voxels there is no bird there — no bill, no "
           "eye, no tail shape. The species that are enlarged say so in their "
           "own notes, so nobody later 'corrects' them back to life size."),
    P("bird.bill_frac", "Bill share", 0.07, 0.01, 0.30, 0.005, group="bird",
      kinds=("bird",),
      help="Share of the total length that is bill. The five share rows are "
           "NORMALISED, so they are proportions and the length slider still "
           "means what it says. Real values: a songbird 0.05, a woodpecker "
           "0.10, a heron 0.17, a kingfisher 0.20, a curlew 0.25."),
    P("bird.head_frac", "Head share", 0.13, 0.04, 0.35, 0.005, group="bird",
      kinds=("bird",),
      help="Share of the total length that is head. An owl and a kingfisher "
           "are big-headed; a heron and a swan are not."),
    P("bird.neck_frac", "Neck share", 0.05, 0.0, 0.45, 0.005, group="bird",
      kinds=("bird",),
      help="Share of the total length that is visible neck. A songbird sits "
           "near 0.03 — it has a neck and keeps it pulled in — a duck 0.10, a "
           "heron 0.28, a swan 0.40. This is the single clearest separator "
           "between a wading bird and everything else at twenty voxels."),
    P("bird.body_frac", "Body share", 0.40, 0.15, 0.70, 0.005, group="bird",
      kinds=("bird",)),
    P("bird.tail_frac", "Tail share", 0.35, 0.05, 0.65, 0.005, group="bird",
      kinds=("bird",),
      help="Share of the total length that is tail. A wren 0.20, a robin 0.33, "
           "a magpie 0.55, a macaw 0.58. Long tails are the other half of the "
           "proportion story and they cost almost nothing to draw."),
    P("bird.posture_deg", "Posture", 30.0, -15.0, 82.0, 1.0, group="bird",
      kinds=("bird",),
      help="How far the body is tilted nose-up from level. A duck and a heron "
           "lie flat at 0-10; a thrush sits at 25-35; a robin, a wren and an "
           "owl sit up at 45-60; a woodpecker clinging to a trunk is "
           "near-vertical at 75-80. One number, and it changes the silhouette "
           "more than any colour does. A flying bird uses a third of this, "
           "because a bird in the air lies along its own line of travel "
           "whatever it perches at."),

    P("bird.body_depth", "Body depth", 0.62, 0.25, 1.40, 0.01, group="bird",
      kinds=("bird",),
      help="Deepest part of the body divided by the body's own length (not the "
           "total length). Around 0.55-0.70 for most birds; a plump gamebird or "
           "a robin in cold weather goes past 0.9; a swift or a swallow drops "
           "to 0.4."),
    P("bird.body_width", "Body width", 0.82, 0.35, 1.60, 0.01, group="bird",
      kinds=("bird",),
      help="Width across the body divided by its depth. Birds are much rounder "
           "in section than fish: nearly everything sits between 0.75 and 0.95. "
           "A duck or a gull is broader (1.0-1.2)."),
    P("bird.chest_at", "Deepest point", 0.32, 0.10, 0.75, 0.01, group="bird",
      kinds=("bird",),
      help="How far back the body is deepest, measured from the BREAST. A "
           "bird's mass sits forward, over the flight muscle, which is the "
           "opposite of a fish — so this is usually 0.25-0.40 where a fish's is "
           "0.36-0.58."),
    P("bird.breast", "Breast fullness", 0.66, 0.20, 0.98, 0.01, group="bird",
      kinds=("bird",),
      help="Depth at the very front of the body, as a fraction of the maximum. "
           "A BIRD'S FRONT END IS BLUNT: the flight muscle is the deepest thing "
           "on the animal and it starts immediately behind the neck. Authoring "
           "this at a fish's snout values (0.25-0.40) gives a bird that reads "
           "as a fish standing on end."),
    P("bird.rump", "Rump depth", 0.42, 0.08, 0.95, 0.01, group="bird",
      kinds=("bird",),
      help="Depth where the tail joins, as a fraction of the maximum. Low on a "
           "swallow or a swift, high on a duck."),
    P("bird.fullness", "Swell", 3.2, 0.5, 12.0, 0.1, group="bird",
      kinds=("bird",),
      help="How quickly the body swells to full depth and falls away again. "
           "Deliberately independent of the deepest point above, so tuning a "
           "species does not mean chasing one slider with another."),
    P("bird.belly", "Belly share", 0.52, 0.20, 0.80, 0.01, group="bird",
      kinds=("bird",),
      help="How much of the depth sits below the body axis rather than above "
           "it. Above 0.5 gives the deep round belly of a pigeon or a "
           "gamebird; below 0.5 an arched back over a flat underside."),
    P("bird.section", "Section shape", 2.1, 1.0, 4.0, 0.05, group="bird",
      kinds=("bird",),
      help="Cross-section roundness, as a superellipse exponent. 2 is an "
           "ellipse and nearly every bird is close to it. Above 2.6 the body is "
           "a rounded box, which is what a duck floating is."),
    P("bird.neck_thick", "Neck thickness", 0.45, 0.08, 1.20, 0.01, group="bird",
      kinds=("bird",),
      help="Neck diameter as a fraction of body depth. A heron's is a pipe "
           "(0.18); an owl has no visible neck at all and wants this near 1.0 "
           "so head and body run together."),
    P("bird.neck_up_deg", "Neck angle", 30.0, -40.0, 85.0, 1.0, group="bird",
      kinds=("bird",),
      help="How far above the body axis the neck leaves the shoulders. High "
           "for an upright wader, near zero for a duck, negative for a bird "
           "reaching down to feed."),
    P("bird.head_size", "Head size", 1.0, 0.4, 2.2, 0.01, group="bird",
      kinds=("bird",),
      help="Multiplier on the head, over and above its share of the length. An "
           "owl and a kingfisher are 1.5-1.8; a heron and a swan are 0.6-0.8. "
           "Head size against body size is one of the cues a birder names first "
           "and one of the very few that survives at ten voxels."),
    P("bird.crest", "Crest", 0.0, 0.0, 1.0, 0.01, group="bird",
      kinds=("bird",),
      help="A back-swept spike of feathers off the crown. Four to eight voxels, "
           "and on a jay, a hoopoe, a lapwing or a lark it is the single most "
           "identifiable thing about the animal. 0 removes it."),

    P("bird.bill_depth", "Bill depth", 0.35, 0.06, 1.20, 0.01, group="bird",
      kinds=("bird",),
      help="Bill depth at its base, as a fraction of head diameter. THIS IS "
           "WHAT SAYS WHAT THE BIRD EATS. A seed-cracking finch is a deep cone "
           "(0.55-0.8); an insect-picking warbler is a needle (0.10-0.18); a "
           "raptor and a crow are in between and heavy."),
    P("bird.bill_curve", "Bill curve", 0.0, -0.6, 1.0, 0.01, group="bird",
      kinds=("bird",),
      help="Bends the bill's CENTRELINE. Positive is decurved — a curlew, a "
           "hoopoe, a treecreeper; negative is recurved — an avocet. 0 is "
           "straight, which is most birds. This bends the bill rather than "
           "aiming it: a straight bill pointing downhill is a different animal "
           "and looks close enough in a render to survive several passes."),
    P("bird.bill_hook", "Bill hook", 0.0, 0.0, 1.0, 0.01, group="bird",
      kinds=("bird",),
      help="Drops the last fifth of the bill sharply. A raptor's bill is "
           "straight for four fifths of its length and then hooks, which is "
           "the entire visual difference between an eagle and a stork with the "
           "same bill length. Separate from the curve above because it is a "
           "different shape, not more of the same one."),
    P("bird.bill_gape", "Bill width", 0.15, 0.0, 1.0, 0.01, group="bird",
      kinds=("bird",),
      help="How wide the bill is across. Near 0 is a needle or a dagger — a "
           "heron, a kingfisher, a warbler. Near 1 is a spoon — a duck, a "
           "spoonbill. A duck's bill and a heron's are the same length and "
           "nobody confuses them, and this is why."),

    P("bird.tail_shape", "Tail shape", "square", kind="choice", group="bird",
      kinds=("bird",), choices=_TAIL_SHAPES,
      help="The tail's outline, named the way a field guide names it. These "
           "are statements about the LENGTHS of the feathers across the fan: "
           "GRADUATED and WEDGE mean the central pair is longest (a magpie, a "
           "raven, a macaw), FORKED and NOTCHED mean the outer pair is (a "
           "swallow, a swift, a house martin), SQUARE means they are all the "
           "same, ROUNDED is square with the corners taken off, POINTED is "
           "graduated taken to its limit."),
    P("bird.tail_width", "Tail width", 0.42, 0.10, 1.40, 0.01, group="bird",
      kinds=("bird",),
      help="How wide the tail fan is, as a fraction of its own length. Wide on "
           "a buzzard or a jay, narrow on a swift."),
    P("bird.tail_fork", "Fork depth", 0.30, 0.0, 0.90, 0.01, group="bird",
      kinds=("bird",),
      help="How deeply the centre of the tail is cut away. ONLY APPLIES TO THE "
           "'forked' AND 'notched' SHAPES. A house martin is about 0.25; a barn "
           "swallow with full streamers is 0.7 and up."),
    P("bird.tail_droop", "Tail carriage", 0.55, -0.5, 1.5, 0.01, group="bird",
      kinds=("bird",),
      help="How closely the tail follows the body's angle. 1 continues the body "
           "line straight out; 0 hangs the tail level whatever the posture; "
           "negative cocks it up, which is a wren."),
    P("bird.tail_thick", "Tail thickness", 1, 1, 3, 1, kind="int", group="bird",
      kinds=("bird",),
      help="Tail thickness in voxels. 1 is right for anything under about half "
           "a metre; a big raptor wants 2 or the tail disappears edge-on."),

    P("bird.pose", "Pose", "perched", kind="choice", group="bird",
      kinds=("bird",), choices=_BIRD_POSES,
      help="Folded wings or spread ones. THIS IS TWO DIFFERENT ANIMALS AND NOT "
           "ONE ANIMAL AT TWO ROTATIONS: a folded wing is a three-voxel bulge "
           "lying along the flank and a spread one is a one-voxel plate "
           "reaching thirty voxels out, and no rotation turns one into the "
           "other. One generation produces one asset, so an asset carries one "
           "pose; producing the other costs one changed field in this spec.\n\n"
           "ONE THING A SPAWNER MUST KNOW: changing this changes the spec hash, "
           "so species X seed 7 perched and species X seed 7 flying are two "
           "different individuals, not one individual in two poses. If a bird "
           "has to land and stay the same bird, that is a change to "
           "`pipeline.rng_for`, not to this row."),
    P("bird.wing_shape", "Wing planform", "elliptical", kind="choice",
      group="bird", kinds=("bird",), choices=_WING_SHAPES,
      help="Savile's four wing types, which map almost one-to-one onto the "
           "groups a player would name. ELLIPTICAL: broad, rounded, built to "
           "turn in cover — corvids, gamebirds, woodland songbirds. POINTED: "
           "swept and tapering to a point, built for speed — falcons, swifts, "
           "swallows, terns. SOARING: a long narrow plank of near-constant "
           "chord — gulls, albatrosses. SLOTTED: broad, barely tapered, and "
           "finished with separated finger feathers — eagles, buzzards, storks, "
           "vultures. ONLY VISIBLE IN THE FLYING POSE; a folded wing is a "
           "folded wing whatever planform it opens into."),
    P("bird.wing_span", "Wingspan", 1.7, 0.8, 4.5, 0.05, group="bird",
      kinds=("bird",),
      help="Wingspan divided by total length. A wren 1.3, a songbird 1.5-1.8, "
           "a swallow 1.8, a pigeon 1.9, a buzzard 2.4, a gull 2.7, an "
           "albatross 4.0. Only drawn in the flying pose."),
    P("bird.wing_aspect", "Aspect ratio", 6.0, 2.5, 18.0, 0.1, group="bird",
      kinds=("bird",),
      help="Wingspan squared over wing area, the number every wing-morphology "
           "table is published in — and here it is what sets the CHORD, since "
           "the span is already set above. A gamebird is 4-5, a songbird 5-6, "
           "an eagle 6-7, a gull 10-12, an albatross 15+. Only drawn in the "
           "flying pose."),
    P("bird.wing_sweep", "Wing sweep", 0.25, -0.2, 1.0, 0.01, group="bird",
      kinds=("bird",),
      help="How far back the wingtips are carried. Near 0 the wings are held "
           "straight out from the shoulders; at 1 they rake right back, which "
           "is a falcon in a stoop or a swift. Only drawn in the flying pose."),
    P("bird.wing_dihedral", "Wing angle", 0.10, -0.6, 0.8, 0.01, group="bird",
      kinds=("bird",),
      help="Whether the wings are carried up in a shallow V, level, or drooped. "
           "A harrier and a vulture hold a strong V; a buzzard is nearly flat; "
           "a bird on a downstroke is negative. Cheap, and it is most of what "
           "stops a flock of eight from reading as eight copies."),
    P("bird.wing_slots", "Wingtip fingers", 0, 0, 6, 1, kind="int", group="bird",
      kinds=("bird",),
      help="Separated primary feathers at the wingtip. ONLY APPLIES TO THE "
           "'slotted' PLANFORM. 5-6 on an eagle or a vulture, 4 on a buzzard, "
           "0 everywhere else. This is real geometry — daylight between the "
           "fingers — and it is the most legible thing about a big soaring bird "
           "seen from below."),
    P("bird.wing_thick", "Wing thickness", 1, 1, 3, 1, kind="int", group="bird",
      kinds=("bird",),
      help="Wing thickness in voxels. 1 under about half a metre; a large "
           "raptor wants 2-3 or the wing vanishes when the camera is level "
           "with it."),
    P("bird.wing_fold", "Folded wing reach", 0.45, 0.0, 1.30, 0.01, group="bird",
      kinds=("bird",),
      help="How far the closed wingtips reach down the tail, as a fraction of "
           "the tail's length. ONLY APPLIES TO THE PERCHED POSE, and it is the "
           "one wing cue that survives folding: a swift's primaries reach past "
           "its tail tip (1.1+) and a wren's stop at its rump (0.1). More "
           "reliable than colour for separating a long-winged bird from a "
           "short-winged one."),

    P("bird.leg_len", "Leg length", 0.10, 0.0, 0.45, 0.005, group="bird",
      kinds=("bird",),
      help="Leg length as a fraction of total length. A songbird 0.08-0.12, a "
           "pigeon 0.09, a gull 0.15, a heron 0.33. Only drawn in the perched "
           "pose — a flying bird tucks its legs into its belly feathers on "
           "nearly every species, and a pair of one-voxel threads trailing "
           "under the silhouette is four voxels of noise."),
    P("bird.leg_thick", "Leg thickness", 1.0, 1.0, 4.0, 0.5, group="bird",
      kinds=("bird",),
      help="In voxels. Almost always 1: a tarsus is under a centimetre thick on "
           "everything smaller than a heron, so at the 1 cm lattice a leg is a "
           "one-voxel thread whatever the species. Length is the parameter that "
           "matters."),

    P("bird.eye", "Eye", 1.0, 0.0, 3.0, 1.0, group="bird", kinds=("bird",),
      help="Eye radius in voxels; 0 turns it off. Two voxels a side and they do "
           "more than any other two in the asset — a voxel animal without an "
           "eye reads as an object, and with one it reads as facing somewhere. "
           "A pale voxel goes in front of the dark one, because a dark eye on a "
           "dark head vanishes without a contrast partner."),
    P("bird.upperparts", "Upperparts share", 0.50, 0.0, 1.0, 0.01, group="bird",
      kinds=("bird",),
      help="Share of the body's depth, measured from the top down, that carries "
           "the upperparts colour; the rest is underparts. Upperparts against "
           "underparts is THE division every field guide uses, and it is a "
           "single boundary rather than the fish's three bands because a bird "
           "has no flank worth a third colour. 1.0 makes the bird one colour "
           "all over, which is a raven; 0.0 makes it underparts all over, which "
           "is nothing."),
    P("bird.head_mark", "Head marking", "none", kind="choice", group="bird",
      kinds=("bird",), choices=_HEAD_MARKS,
      help="One mark on the head. CAP is a coloured crown — a tit, a blackcap, "
           "a black-headed gull. MASK is a band through the eye — a shrike, a "
           "kingfisher; it works by putting the eye ON its edge rather than in "
           "its middle. SUPERCILIUM is the pale eyebrow stripe over it. THROAT "
           "is a coloured bib — a robin, a great tit, a swallow. COLLAR is a "
           "ring round the neck."),
    P("bird.wing_mark", "Wing marking", "none", kind="choice", group="bird",
      kinds=("bird",), choices=_WING_MARKS,
      help="One mark on the wing. BAR and DOUBLEBAR are wing bars — the pale "
           "tips of one or two rows of coverts, running ACROSS the wing. PANEL "
           "colours the outer wing, which is a speculum or a flash. TIP colours "
           "only the outermost part, which is a gull's black wingtips and the "
           "most reliable gull mark there is."),
    P("bird.body_mark", "Body marking", "none", kind="choice", group="bird",
      kinds=("bird",), choices=_BODY_MARKS,
      help="One mark on the body. BARRED runs across (a sparrowhawk's "
           "underparts, a gamebird); STREAKED runs along (a lark, a pipit, a "
           "song thrush); SPECKLED is spots (a starling); BREASTBAND is one "
           "band across the chest (a ringed plover, a great spotted "
           "woodpecker).\n\n"
           "THREE MARKS, WHERE A FISH GETS ONE, and that is not a relaxation of "
           "the rule — it is the same rule. A fish's stripe and its bars are "
           "drawn on the same twelve-voxel flank, so two of them is noise. A "
           "bird's cap is on its head, its wing bar is on its wing and its "
           "streaking is on its breast, and those three sets of voxels do not "
           "overlap at all."),
    P("bird.mark_count", "Marking count", 5, 1, 10, 1, kind="int", group="bird",
      kinds=("bird",),
      help="Number of bars along the bird, or of streaks across it. THE "
           "CEILING IS 10 AND IT USED TO BE 24, because 24 does not exist at "
           "this size: a 20 cm bird has a body eight voxels long, so 24 bars "
           "is a period of a third of a voxel and one bar and twenty-four "
           "come out identical — every column ends up carrying some mark. "
           "`tools/birdprobe.py` measured exactly that and reported the "
           "slider DEAD. Even 10 only reads on the largest species; on a "
           "songbird, five is the practical limit and it is the same "
           "two-on-two-off floor the fish work measured for bars."),
    P("bird.mark_width", "Marking width", 0.28, 0.02, 0.90, 0.01, group="bird",
      kinds=("bird",),
      help="How wide each mark is, as a share of its own spacing — or for the "
           "head and wing marks, how far the mark reaches. Below about 2 voxels "
           "a mark stops reading at all, whatever colour it is."),
    P("bird.mark_strength", "Speckle coverage", 0.35, 0.0, 1.0, 0.01,
      group="bird", kinds=("bird",),
      help="The EXACT share of the body the speckles cover, taken as a quantile "
           "of the noise rather than as a threshold on it. SPECKLED only. A "
           "plain threshold means whatever the noise happened to do, which is "
           "the defect that left the rock weathering pass removing 20 voxels "
           "out of 90,000 while reporting success."),

    # --- sex -----------------------------------------------------------------
    #
    # SEX RESEEDS, ON PURPOSE, AND THAT IS THE OPPOSITE OF WHAT `bird.pose`
    # DOES. The two decisions sit four hundred lines apart in this file and
    # they are easy to read as an inconsistency, so: a perched raven and a
    # flying raven are one animal in two postures, which is why the pose is
    # excluded from the seeding hash (`SEED_INVARIANT`) and both come out the
    # same raven. A drake and a hen are two animals. There is no individual
    # that is "the same mallard, but female", so seed 7 male and seed 7 female
    # are two different ducks and this field is deliberately NOT in
    # `SEED_INVARIANT`. `tools/birdprobe.py --sex` checks the hashes DIFFER
    # rather than trusting this comment.
    #
    # WHERE THIS DEPARTS FROM `fish.sex`, AND WHY IT HAD TO. A fish's sexual
    # difference is nearly all SIZE -- an orca's dorsal fin, a whale shark's
    # length -- so three ratios and a square root covered twenty-three species.
    # A bird's is nearly all COLOUR. The largest single difference anywhere in
    # this library is a mallard drake against a hen: a bottle-green head, a
    # white collar and a grey body against uniform mottled brown, and not one
    # voxel of it is a proportion. No ratio can express that, so there are two
    # mechanisms below rather than one: two ratios that work exactly like the
    # fish's, and a PLUMAGE SWAP that replaces colours and markings outright.
    P("bird.sex", "Sex", "unsexed", kind="choice", group="bird",
      kinds=("bird",), choices=_SEXES,
      help="Which sex of the species to draw. UNSEXED is what a spec carries "
           "until someone measures a difference, and on the twelve species "
           "here that have none it is the species and the choice changes "
           "nothing at all.\n\n"
           "The two RATIOS below are handled the way the fish are: the "
           "authored number is the average of the two sexes and the ratio is "
           "split as a square root either way, so male divided by female is "
           "exactly the ratio and neither sex is the default.\n\n"
           "PLUMAGE CANNOT WORK THAT WAY AND DOES NOT PRETEND TO. There is no "
           "average of a green head and a brown one. So a dimorphic species "
           "authors its colours as ONE sex, says which in `bird.sex_plumage`, "
           "and gives the other sex's colours in the `alt` rows; UNSEXED then "
           "draws the authored plumage, which on those species is one of the "
           "two sexes and not a compromise. That is a real limitation of "
           "drawing colour rather than a number, it is stated here, in "
           "`docs/bird-dimorphism-research.md` and in the probe's own table, "
           "and it is the reason `bird.sex_plumage` exists at all instead of "
           "the swap being inferred."),
    P("bird.sex_length", "Male:female length", 1.0, 0.70, 1.45, 0.01,
      group="bird", kinds=("bird",),
      help="Adult LINEAR size of the male divided by the female's, from wing "
           "chord where a ringing scheme publishes one. 1.0 is no difference "
           "and is right for most of this library.\n\n"
           "UNDER 1 MEANS THE FEMALE IS THE LARGER BIRD, and unlike the fish "
           "that is not an oddity here -- it is the rule for every raptor and "
           "owl in the set. Reversed sexual size dimorphism is one of the "
           "best-documented patterns in ornithology and the four species that "
           "carry it here (golden eagle, buzzard, kestrel, tawny owl) all "
           "author a ratio below 1.\n\n"
           "MASS RATIOS DO NOT GO IN THIS BOX. A golden eagle female is about "
           "a third heavier than a male, which is a LINEAR ratio near the cube "
           "root of that. Putting the mass ratio here would draw an eagle "
           "three times too dimorphic; the research doc shows the arithmetic "
           "for every species that only had mass published."),
    P("bird.sex_tail", "Male:female tail", 1.0, 0.60, 1.80, 0.01,
      group="bird", kinds=("bird",),
      help="Tail LENGTH, male divided by female. Separate from the size ratio "
           "because on the species that needs it the tail moves and the bird "
           "does not: a barn swallow's outer tail feathers are the classic "
           "measured ornament and its wing chord barely differs between the "
           "sexes at all.\n\n"
           "IT DOES NOT SHRINK THE REST OF THE BIRD. `bird.tail_frac` is one "
           "of five shares that are normalised to sum to one, so scaling the "
           "share alone lengthens the tail by taking length off the head, the "
           "neck and the body -- a longer-tailed swallow with a smaller head, "
           "which is not what a streamer is. `bird._params` compensates the "
           "overall length by the same factor, so this ratio moves the tail "
           "and leaves every other part where it was."),
    P("bird.sex_plumage", "Authored plumage", "same", kind="choice",
      group="bird", kinds=("bird",), choices=("same", "male", "female"),
      help="WHICH SEX THE COLOURS ABOVE DESCRIBE. SAME means the species is "
           "plumage-monomorphic and the twelve `alt` rows are ignored "
           "entirely, which is the honest answer for most of this library.\n\n"
           "MALE or FEMALE says the authored colours are that sex's, and then "
           "asking for the OTHER sex applies whichever `alt` rows are not left "
           "at `same`. Drawing the sex the spec is already authored as changes "
           "nothing, by construction.\n\n"
           "THIS IS A DECLARATION AND NOT A DEFAULT. It is the same trap the "
           "fish work found three times over -- an orca authored at a bull's "
           "fin height while claiming to be the species -- except that colour "
           "cannot be split down the middle to escape it. A mallard in this "
           "library IS a drake unless you ask otherwise; saying so in a field "
           "the probe can read is the difference between a known limitation "
           "and a silent one."),
    P("bird.sex_alt_head_mark", "Other sex: head marking", "same",
      kind="choice", group="bird", kinds=("bird",), choices=_ALT_HEAD_MARKS,
      help="The other sex's head marking, or SAME to keep the species'. This "
           "is the single field a great spotted woodpecker needs: the male "
           "carries a crimson nape and the female carries nothing there, so "
           "her row reads `none` and every other row stays `same`."),
    P("bird.sex_alt_wing_mark", "Other sex: wing marking", "same",
      kind="choice", group="bird", kinds=("bird",), choices=_ALT_WING_MARKS,
      help="The other sex's wing marking, or SAME to keep the species'. Rarely "
           "needed: a mallard's blue speculum is the one mark on the bird that "
           "is IDENTICAL in both sexes, which is exactly why it is the field "
           "mark you identify a hen by."),
    P("bird.sex_alt_body_mark", "Other sex: body marking", "same",
      kind="choice", group="bird", kinds=("bird",), choices=_ALT_BODY_MARKS,
      help="The other sex's body marking, or SAME to keep the species'. A "
           "female kestrel is BARRED where the male is SPOTTED, which is a "
           "different marking rather than a different colour and could not be "
           "said by swapping a material."),

    P("materials.bird_back", "Upperparts", "skin_brown", kind="choice",
      group="bird", kinds=("bird",), choices=materials.BIRD_NAMES),
    P("materials.bird_belly", "Underparts", "plume_white", kind="choice",
      group="bird", kinds=("bird",), choices=materials.BIRD_NAMES),
    P("materials.bird_head", "Head and neck", "skin_brown", kind="choice",
      group="bird", kinds=("bird",), choices=materials.BIRD_NAMES),
    P("materials.bird_wing", "Wing and tail", "skin_brown", kind="choice",
      group="bird", kinds=("bird",), choices=materials.BIRD_NAMES),
    P("materials.bird_mark", "Wing and body marking", "skin_dark", kind="choice",
      group="bird", kinds=("bird",), choices=materials.BIRD_NAMES),
    # THE HEAD GETS ITS OWN MARKING COLOUR and no other region does. That is
    # not symmetry-breaking for its own sake: CUB-200-2011, the standard
    # expert-annotated bird dataset, gives the head ELEVEN pattern values
    # (spotted, malar, crested, masked, unique, eyebrow, eyering, plain,
    # eyeline, striped, capped) and gives the breast, back, belly, wing and
    # tail FOUR each (solid, spotted, striped, multi-coloured). Ornithologists
    # spend nearly three times the vocabulary on the head, and the species that
    # needed this here agree: a great spotted woodpecker is white-panelled on
    # the wing and CRIMSON on the nape, and one shared marking colour makes it
    # choose.
    P("materials.bird_head_mark", "Head marking", "skin_dark", kind="choice",
      group="bird", kinds=("bird",), choices=materials.BIRD_NAMES),
    P("materials.bird_bill", "Bill and legs", "beak_horn", kind="choice",
      group="bird", kinds=("bird",), choices=materials.BIRD_NAMES),
    P("materials.bird_eye", "Eye", "skin_dark", kind="choice", group="bird",
      kinds=("bird",), choices=materials.BIRD_NAMES),

    # --- the other sex's colours --------------------------------------------
    #
    # SEVEN SLOTS, WHICH IS THE SAME SEVEN AS ABOVE MINUS THE EYE, and the eye
    # is missing on purpose rather than by oversight. It is two voxels. No
    # published account of any species here separates the sexes on iris colour
    # at a size a two-voxel eye could carry, and a slot that no species can
    # ever author is a slot that will sit at its default forever while looking
    # like a feature. `docs/bird-dimorphism-research.md` records that as a
    # rejection with the voxel count behind it.
    #
    # `same` MEANS THE SPECIES' OWN COLOUR, not a material. `materials.resolve`
    # would raise on it, which is the behaviour wanted: the sentinel is
    # consumed in `bird._alt_mat` and never reaches the palette, so a typo in
    # one of these rows cannot come out as a silently substituted colour.
    P("materials.bird_alt_back", "Other sex: upperparts", "same", kind="choice",
      group="bird", kinds=("bird",), choices=("same",) + materials.BIRD_NAMES),
    P("materials.bird_alt_belly", "Other sex: underparts", "same", kind="choice",
      group="bird", kinds=("bird",), choices=("same",) + materials.BIRD_NAMES),
    P("materials.bird_alt_head", "Other sex: head and neck", "same",
      kind="choice", group="bird", kinds=("bird",),
      choices=("same",) + materials.BIRD_NAMES),
    P("materials.bird_alt_wing", "Other sex: wing and tail", "same",
      kind="choice", group="bird", kinds=("bird",),
      choices=("same",) + materials.BIRD_NAMES),
    P("materials.bird_alt_mark", "Other sex: wing and body marking", "same",
      kind="choice", group="bird", kinds=("bird",),
      choices=("same",) + materials.BIRD_NAMES),
    P("materials.bird_alt_head_mark", "Other sex: head marking", "same",
      kind="choice", group="bird", kinds=("bird",),
      choices=("same",) + materials.BIRD_NAMES),
    P("materials.bird_alt_bill", "Other sex: bill and legs", "same",
      kind="choice", group="bird", kinds=("bird",),
      choices=("same",) + materials.BIRD_NAMES),

    # --- land-animal colours -------------------------------------------------
    #
    # EIGHT SLOTS AND NO NEW MATERIALS. Every species in the first tranche is
    # authored out of the twenty-one creature materials already in the engine
    # (`forge/materials.py`), because a material append has five separate tails
    # -- a static_assert in VoxelAgentSubsystem.cpp, a count assertion in
    # test_assetgrid.cpp, the positional table in materialpalette.h, a
    # BIOME_TINT decision in ue-project/Tools/terrain_palette.py that refuses to
    # generate without one, and two generated mirrors -- and it is not something
    # to spend before the shapes are approved.
    #
    # `docs/quadruped-notes.md` records the two colours the mammal set genuinely
    # wants and what they would cost, with the WCAG contrast numbers that decide
    # whether each one is a real gap or tidiness of naming. Neither is proposed
    # here.
    #
    # NO `alt` SLOTS, unlike the bird block above, and that is a measured
    # difference rather than an omission. A bird's sexual difference is nearly
    # all COLOUR -- a mallard drake against a hen is bottle green, white, grey
    # and yellow against uniform brown, and not one voxel of it is a proportion.
    # A mammal's is nearly all STRUCTURE: a stag's antlers, a lion's mane, a
    # bull's size. `quad.sex_horn` and `quad.sex_mane` carry that, and there is
    # no species in this library's queue whose two sexes differ mainly in hue.
    P("materials.quad_back", "Upperparts", "skin_brown", kind="choice",
      group="quad", kinds=("quadruped",), choices=materials.CREATURE_NAMES),
    P("materials.quad_belly", "Underparts", "skin_pale", kind="choice",
      group="quad", kinds=("quadruped",), choices=materials.CREATURE_NAMES),
    P("materials.quad_head", "Head, ears and muzzle", "skin_brown",
      kind="choice", group="quad", kinds=("quadruped",),
      choices=materials.CREATURE_NAMES),
    P("materials.quad_leg", "Legs", "skin_brown", kind="choice", group="quad",
      kinds=("quadruped",), choices=materials.CREATURE_NAMES),
    P("materials.quad_tail", "Tail", "skin_brown", kind="choice", group="quad",
      kinds=("quadruped",), choices=materials.CREATURE_NAMES),
    P("materials.quad_mark", "Marking, cape, stockings and mane", "skin_dark",
      kind="choice", group="quad", kinds=("quadruped",),
      choices=materials.CREATURE_NAMES,
      help="ONE COLOUR FOR ALL FIVE, which is a deliberate limit rather than a "
           "shortcut. A fox's black stockings, black ear backs and white brush "
           "tip are three colours on one animal and it would want two slots — "
           "but every species that wants a second is a species whose first is "
           "doing nothing, and five slots that are the same value on forty "
           "specs is five places for a retune to be applied to four of them."),
    P("materials.quad_horn", "Horns, antlers and hooves", "beak_horn",
      kind="choice", group="quad", kinds=("quadruped",),
      choices=materials.CREATURE_NAMES),
    P("materials.quad_eye", "Eye", "skin_dark", kind="choice", group="quad",
      kinds=("quadruped",), choices=materials.CREATURE_NAMES),

    # --- flock: a bird as a detail entity -----------------------------------
    #
    # NONE OF THIS IS READ BY ANY CODE, AND THAT IS DELIBERATE, exactly as with
    # the fish `detail` group above. Spawning birds is a job for worldgen; what
    # these rows are is the SPECIFICATION a spawner will be written against,
    # stated in the same file the shape is stated in so the two cannot drift.
    #
    # A SEPARATE GROUP RATHER THAN REUSING `detail`. Five of that group's
    # eleven rows are about water depth, and a bird does not have a water
    # depth. The four that would have transferred are cheaper to restate than
    # the four that would have had to be hidden per kind.
    P("flock.entity_class", "Entity class", "detail", kind="choice",
      group="flock", kinds=("bird",), choices=("detail", "persistent"),
      help="'detail' means nothing about this individual is saved: it is "
           "spawned from (species, seed) when the player is near, and when it "
           "despawns it is gone. Anything that has to survive being left and "
           "come back the same is 'persistent' and costs a save slot."),
    P("flock.despawn_m", "Despawn distance (m)", 90.0, 5.0, 800.0, 5.0,
      group="flock", kinds=("bird",),
      help="How far the player has to get before this individual is removed. "
           "Much larger than a fish's, and for a real reason: a soaring bird is "
           "the one detail entity in this library that is meant to be seen a "
           "long way off, and an eagle that pops out at 45 m is worse than no "
           "eagle."),
    P("flock.despawn_delay_s", "Despawn delay (s)", 6.0, 0.0, 120.0, 0.5,
      group="flock", kinds=("bird",),
      help="Grace period after the player passes the distance above. Without "
           "one, walking at the despawn radius makes the flock flicker."),
    P("flock.size_min", "Flock size, least", 1, 1, 200, 1, kind="int",
      group="flock", kinds=("bird",)),
    P("flock.size_max", "Flock size, most", 4, 1, 2000, 1, kind="int",
      group="flock", kinds=("bird",),
      help="A flock is spawned as one decision: N individuals of this species "
           "from consecutive seeds, so they vary the way the `variation` group "
           "says. Solitary species set both ends to 1; a starling roost is in "
           "the hundreds."),
    P("flock.spread_m", "Flock spread (m)", 12.0, 0.2, 400.0, 0.5,
      group="flock", kinds=("bird",),
      help="Radius the flock occupies."),
    P("flock.perch", "Where it perches", "canopy", kind="choice",
      group="flock", kinds=("bird",),
      choices=("ground", "shrub", "canopy", "cliff", "waterside", "water",
               "air"),
      help="What this species sits on when it is not flying, which is the gate "
           "a spawner needs before it can place one. 'air' means the species is "
           "essentially never seen perched — a swift — and should spawn flying "
           "whatever its own pose says."),
    P("flock.height_min_m", "Flying height, least (m)", 2.0, 0.0, 500.0, 1.0,
      group="flock", kinds=("bird",),
      help="Height above the ground this species flies at. A swallow hunts at "
           "1-15 m; a soaring eagle holds 100-400."),
    P("flock.height_max_m", "Flying height, most (m)", 25.0, 0.5, 3000.0, 5.0,
      group="flock", kinds=("bird",)),
    P("flock.flight_share", "Share of time flying", 0.35, 0.0, 1.0, 0.01,
      group="flock", kinds=("bird",),
      help="How often an individual of this species is in the air rather than "
           "perched. This is what tells a spawner which POSE to ask for, and it "
           "is why the pose above is a species property and not a global "
           "setting: a vulture is 0.9 and a wren is 0.05."),
    P("flock.per_hectare", "Individuals per hectare", 4.0, 0.0, 500.0, 0.5,
      group="flock", kinds=("bird",),
      help="Expected number over a hectare of suitable ground, before the biome "
           "weights are applied. A hectare rather than the fish group's 100 m² "
           "because birds are spread over two orders of magnitude more ground "
           "than fish are."),

    # --- quadruped: the first asset that stands on the ground ---------------
    #
    # WHAT MAKES THIS GROUP DIFFERENT FROM THE BIRD GROUP. A bird's body height
    # above anything is nobody's business: `bird.leg_len` is a fraction of its
    # length and wherever the legs stop is where they stop. A land animal's feet
    # are on a plane, so `shoulder_h` and `hip_h` below are HEIGHTS ABOVE THE
    # GROUND and the limb lengths are derived from them. That is one number
    # fewer than the obvious design and it is the reason a spec cannot say
    # "shoulder at 1.8 m" and "foreleg 1.1 m" and be wrong twice.
    #
    # THE THREE ROWS THAT CARRY MOST OF THE BETWEEN-SPECIES SIGNAL are
    # `shoulder_h`, `hip_h` and `neck_deg`. A bison's shoulder is well above its
    # hips and its neck is low; a giraffe's neck is vertical off a back that
    # slopes the same way; a hyena slopes and holds its head level; a musk deer
    # is higher at the hip than the shoulder. Those are three numbers and they
    # separate more species than every colour row below them put together.
    P("quad.length_m", "Head-body length (m)", 1.20, 0.10, 7.00, 0.01,
      group="quad", kinds=("quadruped",),
      help="Nose to rump, NOT including the tail. Every size in "
           "`docs/biomes/*.md` is quoted head-body for a reason: the tail is "
           "the one measurement sources disagree about, and a squirrel "
           "authored on total length comes out with half the body it should "
           "have.\n\n"
           "The tail is `tail_len` below, as a fraction of this."),
    P("quad.stance", "Stance", "standing", kind="choice", group="quad",
      kinds=("quadruped",), choices=_QUAD_STANCES,
      help="How the animal meets the ground, and the one row here that changes "
           "geometry rather than a number.\n\n"
           "STANDING — four limbs under the trunk, all four on the floor. "
           "Everything with hooves, paws or pads.\n\n"
           "SPRAWLING — the limbs leave the FLANK and reach out sideways "
           "before they reach down, with the belly close to the ground. "
           "Lizards and crocodilians. No angle on a limb attached under the "
           "body reaches this, because it is the attachment point that moves.\n\n"
           "BIPEDAL — the hind limbs and the TAIL carry the animal, as a "
           "tripod. The tail's carriage angle stops being authored and is "
           "solved so its tip lands on the ground, and the forelimbs attach "
           "forward on the chest. A kangaroo, a jerboa, a meerkat on watch.\n\n"
           "This is a species property and not a posture — a kangaroo cannot "
           "stand quadrupedally — so it is part of the seeding hash, unlike "
           "`bird.pose`."),
    P("quad.shoulder_h", "Shoulder height", 0.62, 0.12, 1.60, 0.01,
      group="quad", kinds=("quadruped",),
      help="Height of the SHOULDER JOINT above the ground, as a fraction of "
           "head-body length. This sets the foreleg length; there is no "
           "separate leg row to disagree with it.\n\n"
           "IT IS THE JOINT, NOT THE WITHERS, and that distinction cost the "
           "whole library. A published 'shoulder height' is measured to the TOP "
           "OF THE BACK; the joint sits half a trunk-depth below it. All 131 "
           "specs were authored by typing published shoulder figures straight "
           "into this row, so the generator put the joint where the withers "
           "belong and drew the trunk on top -- the library stood a half-trunk "
           "too tall, measured at 1.35x life over 108 standing species with NOT "
           "ONE inside 10%.\n\n"
           "The examples this help used to give were the same mistake: 'a "
           "giraffe 1.30' is a withers ratio. A number read out of "
           "docs/biomes/*.md or off a species page is almost certainly a "
           "withers height, and half the trunk depth has to come off before it "
           "belongs here. `tools/refstance.py report` is the regression check; "
           "docs/quadruped-stance-height.md has the account.\n\n"
           "Joint values, solved against reference silhouettes rather than "
           "typed: a badger 0.26, a wild boar 0.44, a red fox 0.42, a bison "
           "0.48, a moose 0.52. Under about 0.25 the animal is a low-slung "
           "mustelid."),
    # THE FLOOR IS 0.25 AND NOT 0.45, and the difference is the bipeds. A
    # quadruped never goes below about 0.7 -- a bison is 0.86 and a spotted
    # hyena, which is the most extreme sloped back of any land mammal, is 0.75.
    # A kangaroo's hip sits at a little over 0.4 of its shoulder height and a
    # meerkat standing vertical is lower still, and at a floor of 0.45 both were
    # silently clamped at authoring time. That is exactly the `fish.length_m`
    # ceiling of 3 m that clamped every whale in this library, one parameter
    # over: the spec said one thing, the asset was another, and the only sign
    # was a warning nobody was reading.
    P("quad.hip_h", "Hip height : shoulder height", 1.00, 0.25, 1.45, 0.01,
      group="quad", kinds=("quadruped",),
      help="Hip joint height divided by shoulder joint height, which is the "
           "SLOPE OF THE BACK and one of the strongest species cues there is.\n\n"
           "1.0 is level (a horse, a deer, a cat). Below 1 the animal is higher "
           "at the shoulder: a bison 0.86, a spotted hyena 0.75, a wildebeest "
           "0.80, a brown bear 0.90. Above 1 it is higher at the rump: a rabbit "
           "1.15, a Siberian musk deer 1.20. A kangaroo sits far below 1 "
           "because its hip is near the ground and its shoulders are not."),
    P("quad.trunk_frac", "Trunk share", 0.62, 0.25, 0.90, 0.005, group="quad",
      kinds=("quadruped",),
      help="Share of the head-body length that is trunk, rump to shoulder. The "
           "four share rows are NORMALISED, so they are proportions and the "
           "length row still means what it says."),
    P("quad.neck_frac", "Neck share", 0.12, 0.0, 0.55, 0.005, group="quad",
      kinds=("quadruped",),
      help="Share that is visible neck. A cat or a boar 0.06, a wolf 0.12, a "
           "horse 0.20, a camel 0.26, a giraffe 0.45. This is the clearest "
           "separator between a grazer and a hunter at twenty voxels."),
    P("quad.head_frac", "Head share", 0.16, 0.05, 0.40, 0.005, group="quad",
      kinds=("quadruped",),
      help="Share that is skull, excluding the muzzle. A hippo and a capybara "
           "are famously big-headed; a giraffe is not."),
    P("quad.muzzle_frac", "Muzzle share", 0.10, 0.0, 0.35, 0.005, group="quad",
      kinds=("quadruped",),
      help="Share that is muzzle, projecting forward of the skull. A cat 0.03, "
           "a bear 0.09, a wolf 0.13, a horse 0.17, a tapir 0.20."),
    P("quad.neck_deg", "Neck angle", 30.0, -45.0, 88.0, 1.0, group="quad",
      kinds=("quadruped",),
      help="How far above the horizontal the neck rises, measured ABSOLUTELY "
           "and not relative to the back — because that is how you read it off "
           "a photograph. A giraffe holds a vertical neck off a sloping back "
           "and a stoat holds a horizontal one off a horizontal back, and "
           "authoring the second as an offset from the first means entering "
           "every species by arithmetic.\n\n"
           "A grazing or browsing animal with its head down is NEGATIVE: a "
           "wildebeest carries its head at about -10, a moose reaching for "
           "water lower still. A meerkat on watch is near 85."),
    P("quad.head_deg", "Head angle", 0.0, -70.0, 45.0, 1.0, group="quad",
      kinds=("quadruped",),
      help="Which way the muzzle points, from the horizontal. Separate from "
           "the neck angle because an alert deer holds a raised neck with a "
           "level face, and a grazing one holds a lowered neck with the face "
           "pointing at the ground."),

    P("quad.depth", "Trunk depth", 0.42, 0.15, 0.90, 0.01, group="quad",
      kinds=("quadruped",),
      help="Depth of the trunk, belly to back, divided by the trunk's own "
           "length. A greyhound or a cheetah 0.30, a wolf 0.36, a horse 0.42, "
           "a bear 0.50, a hippo 0.62."),
    P("quad.width", "Trunk width", 0.62, 0.25, 1.30, 0.01, group="quad",
      kinds=("quadruped",),
      help="Width across the trunk divided by its depth. Mammals are much "
           "narrower than they are deep — a deep narrow chest is the running "
           "build — so most of this library sits between 0.5 and 0.75. A hippo "
           "or a badger goes past 0.9."),
    P("quad.chest", "Chest fullness", 1.00, 0.30, 1.60, 0.01, group="quad",
      kinds=("quadruped",),
      help="Depth at the shoulder end as a fraction of the deepest point."),
    P("quad.waist", "Waist fullness", 0.94, 0.30, 1.60, 0.01, group="quad",
      kinds=("quadruped",),
      help="Depth halfway along. This is a REAL VALUE AT THE MIDPOINT, not a "
           "curve control point — the algebra in `quadruped._trunk_profiles` is "
           "what makes it so, because a designer who types 0.8 and gets 0.65 "
           "never finds out why. A tucked-up hunting dog or a cheetah drops to "
           "0.72; a barrel-bodied pony or a capybara sits at 1.0."),
    P("quad.rump", "Rump fullness", 0.92, 0.30, 1.60, 0.01, group="quad",
      kinds=("quadruped",),
      help="Depth at the tail end. A kangaroo's hindquarters go past 1.2."),
    P("quad.belly", "Belly share", 0.52, 0.20, 0.80, 0.01, group="quad",
      kinds=("quadruped",),
      help="How much of the trunk's depth hangs BELOW its axis. Above 0.5 the "
           "animal is pot-bellied, which most grazers are."),
    P("quad.section", "Section squareness", 2.20, 1.20, 4.00, 0.05,
      group="quad", kinds=("quadruped",),
      help="Superellipse exponent of the trunk's cross-section. 2 is an "
           "ellipse; above 3 it squares off, which is what a bison, a hippo or "
           "a rhino actually looks like."),
    P("quad.hump", "Withers hump", 0.0, 0.0, 0.90, 0.01, group="quad",
      kinds=("quadruped",),
      help="Extra depth added to the TOP of the trunk over the withers, on top "
           "of the shoulder height. A bison 0.55, a wildebeest 0.35, a brown "
           "bear 0.30, a camel 0.70 (the fatty hump), everything else 0.\n\n"
           "Added to the top only, never to the depth: added to the depth it "
           "would push the belly down as well and a bison would carry a bulge "
           "underneath it that no animal has."),
    P("quad.hump_at", "Hump position", 0.16, 0.0, 0.70, 0.01, group="quad",
      kinds=("quadruped",),
      help="Where along the trunk the hump sits, 0 at the shoulder and 1 at "
           "the rump. A bison's is right at the withers; a camel's is further "
           "back."),

    P("quad.neck_thick", "Neck thickness", 0.52, 0.15, 1.10, 0.01,
      group="quad", kinds=("quadruped",),
      help="Neck diameter as a fraction of the trunk's depth. A stag in rut "
           "and a lion carry necks nearly as deep as their chests; a giraffe "
           "and a gazelle are under 0.3."),
    P("quad.neck_taper", "Neck taper", 0.72, 0.30, 1.20, 0.01, group="quad",
      kinds=("quadruped",),
      help="Neck thickness at the head divided by at the shoulder."),
    P("quad.mane", "Mane / dorsal crest", 0.0, 0.0, 1.00, 0.01, group="quad",
      kinds=("quadruped",),
      help="A ridge along the top of the neck, drawn in the marking colour. A "
           "zebra's stiff brush, a boar's bristle crest, a striped hyena's "
           "erectile mane, a lion's mane, a Przewalski's horse's upright "
           "black mane.\n\n"
           "A ridge ON TOP of a normal neck, not a thicker neck: a thicker "
           "neck is a different animal."),
    P("quad.dewlap", "Throat hang", 0.0, 0.0, 1.20, 0.01, group="quad",
      kinds=("quadruped",),
      help="A lobe hanging under the throat. A moose's bell, a greater kudu's "
           "fringe, a Barbary sheep's chest hair, a zebu's dewlap."),

    P("quad.head_size", "Head size", 1.00, 0.50, 1.90, 0.01, group="quad",
      kinds=("quadruped",),
      help="Multiplier on the skull's diameter, on top of its share."),
    P("quad.muzzle_depth", "Muzzle depth", 0.62, 0.20, 1.40, 0.01,
      group="quad", kinds=("quadruped",),
      help="Depth of the muzzle as a fraction of the skull's radius. A bear is "
           "deep and blunt; a wolf and an anteater are shallow."),
    P("quad.muzzle_width", "Muzzle width", 0.58, 0.15, 1.40, 0.01,
      group="quad", kinds=("quadruped",),
      help="Width of the muzzle as a fraction of the skull's radius. A white "
           "rhino's square wide lip and a hippo's blunt head are past 1.0."),
    P("quad.muzzle_drop", "Muzzle droop", 0.0, 0.0, 1.00, 0.01, group="quad",
      kinds=("quadruped",),
      help="How far the muzzle BENDS downward along its own length. A moose's "
           "overhanging muzzle and a tapir's short trunk are the reason this "
           "exists.\n\n"
           "A bend, not a rotation. Rotated, the tip moves and the muzzle stays "
           "straight — which looks close enough in a render to survive review, "
           "and is the mistake `bird.bill_curve` shipped with for several "
           "passes before the probe measured the bend itself."),
    P("quad.jaw", "Jaw heaviness", 0.35, 0.0, 1.00, 0.01, group="quad",
      kinds=("quadruped",),
      help="How much deeper the lower half of the muzzle is than the upper. "
           "This is what makes a muzzle read as a mouth rather than a snout; a "
           "hyena, a howler monkey and a mandrill are near 1."),
    P("quad.eye", "Eye size", 1.0, 0.0, 3.0, 1.0, kind="int", group="quad",
      kinds=("quadruped",),
      help="Radius in voxels of the eye patch on each side of the skull. Two "
           "voxels do more than any other two in the asset. 0 turns it off, "
           "which a mole or a giant anteater wants."),

    P("quad.ear_shape", "Ear shape", "pointed", kind="choice", group="quad",
      kinds=("quadruped",), choices=_QUAD_EARS,
      help="ROUND is a low disc against the skull (a bear, a boar). POINTED is "
           "an erect cone (a fox, a deer, a cat). BLADE is a long flat paddle "
           "standing off the skull (a hare, a kangaroo, a donkey) and is the "
           "one whose LENGTH is the species. FAN is a broad sheet lying back "
           "along the neck (an elephant) — wider than it is long, which no "
           "setting of BLADE reaches. TUFTED is a pointed ear with a spike off "
           "the tip (a lynx, a caracal, a winter red squirrel)."),
    P("quad.ear_len", "Ear length", 0.09, 0.0, 0.45, 0.005, group="quad",
      kinds=("quadruped",),
      help="Ear length as a fraction of head-body length. A bear 0.05, a fox "
           "0.10, a hare 0.20, a fennec fox 0.30, an African elephant 0.35.\n\n"
           "THIS IS THE ROW THAT SETS THE LATTICE on several species. A hare's "
           "ears are 3-4 cm wide, so at 2 cm they are two voxels and read as a "
           "mistake rather than as ears; the whole animal goes to 1 cm for "
           "them. See `docs/biomes/README.md` §6."),
    P("quad.ear_width", "Ear width", 0.45, 0.10, 1.60, 0.01, group="quad",
      kinds=("quadruped",),
      help="Ear width divided by its length. Above 1 is a fan."),
    P("quad.ear_deg", "Ear angle", 62.0, 0.0, 90.0, 1.0, group="quad",
      kinds=("quadruped",),
      help="90 is straight up, 0 is straight out sideways. A sand cat's ears "
           "are famously low and wide-set; a fox's are near vertical."),
    P("quad.ear_back", "Ear sweep", 0.25, -0.60, 1.00, 0.01, group="quad",
      kinds=("quadruped",),
      help="How far back the ears are carried. A running hare lays them flat "
           "back; an alert deer swings them forward, which is negative here."),

    P("quad.horn_shape", "Headgear", "none", kind="choice", group="quad",
      kinds=("quadruped",), choices=_QUAD_HORNS,
      help="SPIKE is a straight tapering cone. CURVE is a ram's or a Barbary "
           "sheep's semicircle. SWEEP is an oryx's or a gemsbok's near-straight "
           "spear. SPIRAL is a kudu's or an addax's open turns. PALMATE is a "
           "flat blade with points on its edge — a moose, a fallow deer. "
           "BRANCHED is a beam with round tines — a red deer stag, an elk.\n\n"
           "PALMATE AND BRANCHED ARE NOT ONE SHAPE WITH A WIDTH SLIDER, and the "
           "reason is measured. `docs/biomes/README.md` §6 works out that a red "
           "deer's round beam is 3-4 cm and its tine tips 1-2 cm, so at the "
           "5 cm lattice the rack disappears and the stag becomes a hind — "
           "while a moose's palm is a 10-15 cm blade, three voxels at 5 cm, and "
           "reads unaltered. A species using BRANCHED at life size is expected "
           "to author the rack thicker than life and say so in its own notes."),
    P("quad.horn_len", "Headgear length", 0.20, 0.0, 0.90, 0.005, group="quad",
      kinds=("quadruped",),
      help="Length as a fraction of head-body length. A gazelle 0.15, a red "
           "deer 0.45, a gemsbok 0.55, a kudu 0.60."),
    P("quad.horn_thick", "Headgear thickness", 0.16, 0.03, 0.50, 0.005,
      group="quad", kinds=("quadruped",),
      help="Base diameter as a fraction of the headgear's own length. Life "
           "size for a red deer is about 0.08 and that is under two voxels at "
           "any lattice the animal can use, so a stag spec authors this higher "
           "and says so in its notes."),
    P("quad.horn_spread", "Headgear spread", 0.55, 0.0, 2.00, 0.01,
      group="quad", kinds=("quadruped",),
      help="How far out to the sides the pair reaches, relative to its length."),
    P("quad.horn_curl", "Headgear curl", 0.35, 0.0, 1.00, 0.01, group="quad",
      kinds=("quadruped",),
      help="How far round the arc comes. Read by CURVE, SWEEP and SPIRAL."),
    P("quad.horn_tines", "Points a side", 3, 1, 9, 1, kind="int", group="quad",
      kinds=("quadruped",),
      help="Tines on each antler, for PALMATE and BRANCHED. A roe deer 3, a "
           "red deer 5-7, an elk 6-7."),

    P("quad.tail_len", "Tail length", 0.35, 0.0, 1.60, 0.01, group="quad",
      kinds=("quadruped",),
      help="Tail length as a fraction of head-body length. A bear 0.06, a deer "
           "0.12, a wolf 0.40, a fox 0.60, a squirrel 0.95, a howler monkey "
           "1.05, a monitor lizard 1.4."),
    P("quad.tail_thick", "Tail thickness", 0.20, 0.03, 1.10, 0.01,
      group="quad", kinds=("quadruped",),
      help="Base diameter as a fraction of the trunk's depth. A deer's scut "
           "0.10, a wolf 0.22, a fox's brush 0.40, a kangaroo's counterweight "
           "0.70 — which really is thicker than the animal's own neck."),
    P("quad.tail_taper", "Tail taper", 0.35, 0.05, 1.20, 0.01, group="quad",
      kinds=("quadruped",),
      help="Tip thickness divided by base thickness. A PLUME — a squirrel, a "
           "fox — is this near 1 with a thick base, which is why there is no "
           "separate plume switch: it would be a second way to spell a number "
           "that already exists."),
    P("quad.tail_deg", "Tail carriage", -35.0, -90.0, 90.0, 1.0, group="quad",
      kinds=("quadruped",),
      help="Angle above the horizontal the tail leaves the rump at. Negative "
           "hangs down. A wolf carries its brush low at -30, a warthog runs "
           "with its tail straight up at +85, a coati holds it vertical.\n\n"
           "IGNORED IN THE BIPEDAL STANCE, where the tail is a leg and its "
           "angle is solved so the tip reaches the ground. "
           "`tools/quadprobe.py --stance` prints the solved angle and the gap."),
    P("quad.tail_arc", "Tail arc", 0.0, -0.80, 1.20, 0.01, group="quad",
      kinds=("quadruped",),
      help="How far the tail BENDS along its length, on top of its carriage "
           "angle. A grey squirrel's S over the back is the extreme; a wolf's "
           "straight brush is 0."),
    P("quad.tail_tuft", "Tail tuft", 0.0, 0.0, 1.00, 0.01, group="quad",
      kinds=("quadruped",),
      help="A terminal tuft. A lion's, a zebra's tassel, an ox's switch, a "
           "jerboa's flag."),

    P("quad.leg_thick", "Limb thickness", 0.16, 0.05, 0.90, 0.01, group="quad",
      kinds=("quadruped",),
      help="Limb diameter as a fraction of THE LIMB'S OWN LENGTH, joint to "
           "ground. This is the ratio the eye judges as 'lanky' and it is the "
           "ratio published creature sets are quoted in, so a figure can be "
           "read off one and typed straight in: Infinigen's photoreal "
           "quadruped 0.11 behind and 0.14 in front, Veloren 0.23, Minecraft "
           "0.39 (`docs/quadruped-proportion-research.md`).\n\n"
           "IT USED TO BE A FRACTION OF THE TRUNK'S DEPTH, and that is what "
           "made every tall animal in the library look like a wireframe. Trunk "
           "depth is three multiplications away from the leg — it shrinks when "
           "the neck lengthens and when the build is a running one — and "
           "nothing in the chain knew how long the leg was. The taller the "
           "animal, the thinner its legs came out. All 131 species were "
           "converted; the old values do not mean anything here.\n\n"
           "A house-standard hoofed animal sits near 0.16, a bison or a bear "
           "near 0.20, a rhino or an elephant past 0.30, and a low-slung "
           "lizard or mustelid — whose legs are barely longer than they are "
           "thick — well above that.\n\n"
           "THIS ROW DECIDES WHETHER THE JOINT CAPS ARE DRAWN AT ALL. Below "
           "three voxels of limb thickness the cap is skipped (owner, "
           "2026-08-14): at that size the wedge a rotating limb opens is one "
           "voxel and invisible, and a ball big enough to cover it is most of "
           "the leg. On a 22 cm squirrel at 1 cm this bites immediately."),
    P("quad.fore_bend", "Foreleg bend", 0.25, 0.0, 1.00, 0.01, group="quad",
      kinds=("quadruped",),
      help="How far the elbow is carried back and the foot forward. A horse's "
           "foreleg is nearly a straight column at 0.12; a bear's or a "
           "gorilla's is well bent."),
    P("quad.hock", "Hind leg fold", 0.35, 0.0, 1.00, 0.01, group="quad",
      kinds=("quadruped",),
      help="How pronounced the hind leg's zigzag is — hip forward to the "
           "stifle, back to the hock, forward again along the foot. An "
           "elephant's hind leg is nearly a column at 0.10; a deer sits near "
           "0.45; a kangaroo and a hare fold deeply at 0.85.\n\n"
           "A FORE LEG IS TWO SEGMENTS AND A HIND LEG IS THREE, which is "
           "anatomy and not detail: drawn as two both ways, every animal in "
           "the library stands like a table."),
    P("quad.foot", "Foot size", 1.00, 0.30, 3.00, 0.05, group="quad",
      kinds=("quadruped",),
      help="Multiplier on the foot, against the limb's own thickness. A "
           "reindeer's splayed hooves, a hare's and a kangaroo's long hind "
           "feet, a wolverine's huge paws."),
    P("quad.fore_reach", "Forelimb reach", 1.00, 0.20, 1.25, 0.01,
      group="quad", kinds=("quadruped",),
      help="How far down the forelimb reaches, as a fraction of the distance "
           "from its joint to the ground. 1.0 puts the foot on the floor and "
           "is right for everything that walks on four legs; a kangaroo is "
           "near 0.35 and holds its forelimbs clear.\n\n"
           "NOT GATED ON THE STANCE, on purpose. A parameter that only works "
           "when another is set a particular way is this project's documented "
           "trap — `bird.bill_gape` was multiplied by `bill_depth` and did "
           "nothing below heron size for as long as it did — so this is "
           "honoured in every stance and `tools/quadprobe.py --stance` measures "
           "the fore foot's height above the ground on every species, which is "
           "what catches an animal accidentally authored on tiptoe."),

    P("quad.under", "Underside boundary", 0.30, 0.0, 0.85, 0.01, group="quad",
      kinds=("quadruped",),
      help="How far up the flank the pale underside reaches, 0 at the belly "
           "line and 1 at the spine. Countershading is on nearly every mammal "
           "here; on a pronghorn or a gemsbok the boundary itself is the "
           "marking, which is what `flankstripe` draws on."),
    P("quad.cape", "Shoulder cape", 0.0, 0.0, 0.80, 0.01, group="quad",
      kinds=("quadruped",),
      help="A dark field over the FRONT of the animal, in the marking colour, "
           "running back from the shoulder by this fraction of the trunk. A "
           "bison's shaggy cape over a bare rear, a wolf's saddle, a "
           "wildebeest's dark forequarter. It is one of the two things that "
           "separate an American bison from a wisent."),
    P("quad.stocking", "Dark stockings", 0.0, 0.0, 0.90, 0.01, group="quad",
      kinds=("quadruped",),
      help="How far up the legs the marking colour reaches, measured FROM THE "
           "GROUND UP as a fraction of standing height. A red fox's black "
           "stockings, an Arabian oryx's black legs, an okapi's white ones.\n\n"
           "From the ground rather than from each joint down, because the fore "
           "and hind legs are different lengths on nearly every species: a "
           "fraction of each limb's own length puts the boundary at two "
           "heights and the animal looks as though it is standing in a hole."),
    P("quad.tail_tip", "Tail tip", 0.0, 0.0, 0.70, 0.01, group="quad",
      kinds=("quadruped",),
      help="The last fraction of the tail in the marking colour. A red fox's "
           "white brush tip; a stoat's black one, which stays black when the "
           "rest of the animal turns white in winter and is the only reliable "
           "way to tell it from a weasel."),
    P("quad.mark", "Marking", "none", kind="choice", group="quad",
      kinds=("quadruped",), choices=_QUAD_MARKS,
      help="One marking, on the flank. ONE, WHERE A BIRD GETS THREE: a bird's "
           "cap, wing bar and breast streaking sit on three disjoint sets of "
           "voxels, while every mammal marking competes for the same flank.\n\n"
           "BARS are transverse bands wrapping the body — a zebra, a kudu, a "
           "bongo, a tiger. This is the fish generator's 'vertical bars', "
           "reused deliberately, floor rules included. SPOTS, DAPPLE and BLOTCH "
           "are one field of blobs at three scales — a leopard's rough stand-in, "
           "a fallow deer's fine white spotting, a hyena's coarse irregular "
           "patching. SADDLE is a dark field over the back. FLANKSTRIPE is a "
           "horizontal band at the countershading boundary — a gemsbok, an "
           "impala, a dorcas gazelle.\n\n"
           "ROSETTES AND RETICULATION ARE NOT HERE and that is the honest gap: "
           "a leopard's rosette is an annulus with a tawny centre and a "
           "giraffe's reticulation is a partition into plates, and neither is "
           "any setting of the six above."),
    P("quad.mark_count", "Marking count", 12, 1, 40, 1, kind="int",
      group="quad", kinds=("quadruped",),
      help="Bands along the body, or the scale of the blob field. A zebra "
           "carries about 25 body stripes; a bongo 12; a tiger's are fewer and "
           "wider.\n\n"
           "SATURATES ON A SMALL ANIMAL, which is a finding rather than a bug: "
           "a band narrower than two voxels with two voxels of gap merges into "
           "a wash, so above about five bands on a twenty-voxel body they stop "
           "being bands. `tools/quadprobe.py` sweeps this on a large species "
           "for exactly that reason."),
    P("quad.mark_width", "Marking width", 0.32, 0.02, 0.90, 0.01, group="quad",
      kinds=("quadruped",),
      help="How much of each period the mark occupies, or how much of the "
           "flank the blob field covers."),
    P("quad.mark_strength", "Marking strength", 1.0, 0.0, 1.0, 0.01,
      group="quad", kinds=("quadruped",),
      help="How solidly the mark is laid down. A voxel has one flat material "
           "(ADR-0008), so there is no half-tone to blend to and the only "
           "honest reading of 'weaker' is 'less of it' — this thins the mark "
           "rather than fading its colour."),

    # --- structural sexual dimorphism ---------------------------------------
    #
    # `docs/biomes/README.md` §4.9 called this out as the one place a quadruped
    # needs MORE than a bird: `bird._sex_scale` moves a measurement, and a red
    # deer hind against a stag is a part that is PRESENT OR ABSENT. So there are
    # two mechanisms here rather than one, and the second is `_sex_present`,
    # which reads a ratio of 0 as "the female does not have it" — a case the
    # square-root rule cannot express, because sqrt(0) removes it from the male
    # as well.
    P("quad.sex", "Sex", "unsexed", kind="choice", group="quad",
      kinds=("quadruped",), choices=_SEXES,
      help="Which sex to draw. UNSEXED is the species average and is what a "
           "spec carries until someone measures a difference. A sex is NOT a "
           "posture — there is no individual that is 'the same red deer, but "
           "female' — so this is part of the seeding hash and seed 7 male and "
           "seed 7 female are two different animals."),
    P("quad.sex_length", "Male:female length", 1.0, 0.50, 2.00, 0.01,
      group="quad", kinds=("quadruped",),
      help="Adult head-body length of the male divided by the female's. 1.0 is "
           "no difference. A red deer is about 1.15, a lion 1.20, a gorilla "
           "1.25, an elephant seal far higher."),
    P("quad.sex_horn", "Male:female headgear", 1.0, 0.0, 3.00, 0.01,
      group="quad", kinds=("quadruped",),
      help="Headgear length, male divided by female. 1.0 is 'both sexes carry "
           "the same' (an oryx, a gemsbok, a reindeer).\n\n"
           "ZERO MEANS THE FEMALE HAS NONE AT ALL, and that is a different "
           "mechanism from a small one: a red deer hind has no antlers, which "
           "is not a short rack. An UNSEXED draw of a species set to 0 is drawn "
           "WITH the headgear, because the unsexed draw is what the library "
           "thumbnails and a stag with no antlers is not the species average, "
           "it is a different animal."),
    P("quad.sex_mane", "Male:female mane", 1.0, 0.0, 3.00, 0.01,
      group="quad", kinds=("quadruped",),
      help="Mane size, male divided by female, on the same rule as the "
           "headgear above. A lion is the extreme and is the reason both this "
           "and the headgear row read the same helper: a mane is present on "
           "both sexes and hugely bigger on one, antlers are present on one, "
           "and one parameter covers both."),

    # --- herd: a land animal as a detail entity -----------------------------
    #
    # NONE OF THIS IS READ BY ANY CODE, and that is deliberate, exactly as with
    # the fish `detail` group and the bird `flock` group. Spawning animals is a
    # job for worldgen; what these rows are is the SPECIFICATION a spawner will
    # be written against, stated in the same file the shape is stated in so the
    # two cannot drift.
    #
    # A SEPARATE GROUP RATHER THAN REUSING `flock`. Six of that group's ten rows
    # are about flying — height above ground, share of time in the air, where it
    # perches — and a bison does none of those. What replaces them is the one
    # thing a land animal has that neither a bird nor a fish does: it walks on
    # ground of a particular kind, and a spawner has to know whether this
    # species belongs on open ground or under cover.
    P("herd.entity_class", "Entity class", "detail", kind="choice",
      group="herd", kinds=("quadruped",), choices=("detail", "persistent"),
      help="'detail' means nothing about this individual is saved: it is "
           "spawned from (species, seed) when the player is near, and when it "
           "despawns it is gone. Anything that has to survive being left and "
           "come back the same is 'persistent' and costs a save slot.\n\n"
           "A LARGE ANIMAL IS THE FIRST SERIOUS CANDIDATE FOR 'persistent' in "
           "this library. A shoal of sardines can pop in and out; a bison herd "
           "that vanishes when you turn round is the difference between a world "
           "with animals in it and a world with animal-shaped decorations."),
    P("herd.despawn_m", "Despawn distance (m)", 220.0, 10.0, 2000.0, 10.0,
      group="herd", kinds=("quadruped",),
      help="How far the player has to get before this individual is removed. "
           "Much larger than a bird's, because on open ground — and savanna is "
           "20.76% of all land — a herd is visible to the horizon and an "
           "elephant that pops out at 90 m is worse than no elephant."),
    P("herd.despawn_delay_s", "Despawn delay (s)", 20.0, 0.0, 300.0, 1.0,
      group="herd", kinds=("quadruped",),
      help="Grace period after the player passes the distance above."),
    P("herd.size_min", "Herd size, least", 1, 1, 200, 1, kind="int",
      group="herd", kinds=("quadruped",)),
    P("herd.size_max", "Herd size, most", 1, 1, 2000, 1, kind="int",
      group="herd", kinds=("quadruped",),
      help="A herd is spawned as one decision: N individuals of this species "
           "from consecutive seeds, so they vary the way the `variation` group "
           "says. Solitary species set both ends to 1; a wildebeest aggregation "
           "is in the hundreds and is the single largest thing a spawner in "
           "this library will ever be asked for."),
    P("herd.spread_m", "Herd spread (m)", 30.0, 0.5, 800.0, 0.5,
      group="herd", kinds=("quadruped",),
      help="Radius the group occupies."),
    P("herd.cover", "Cover it needs", "any", kind="choice", group="herd",
      kinds=("quadruped",),
      choices=("any", "open", "edge", "forest", "rock", "waterside"),
      help="What kind of ground this species is found on, which is the gate a "
           "spawner needs before it can place one. 'open' is a plains grazer "
           "that avoids trees, 'edge' is a deer that wants the boundary between "
           "the two, 'forest' is a species that stays under canopy, 'rock' is an "
           "ibex or a chamois, 'waterside' is a hippo or an otter.\n\n"
           "This is not the same question as the biome weights: grassland holds "
           "both a pronghorn and a badger, and only one of them is standing "
           "where you can see it."),
    P("herd.per_hectare", "Individuals per hectare", 0.8, 0.0, 200.0, 0.1,
      group="herd", kinds=("quadruped",),
      help="Expected number over a hectare of suitable ground, before the biome "
           "weights are applied. A hectare rather than the fish group's 100 m², "
           "for the same reason the bird group uses one."),
    P("herd.flee_m", "Flight distance (m)", 60.0, 0.0, 500.0, 5.0,
      group="herd", kinds=("quadruped",),
      help="How close a player gets before the animal moves away. This is the "
           "row that decides whether a species is ever seen at close range, and "
           "it is worth authoring honestly: a red deer's is a couple of hundred "
           "metres and a squirrel's is five, so a world tuned only for the "
           "large species would let a player walk up to nothing."),

    # Wood and leaf, and ONLY for the two kinds made of wood and leaf.
    #
    # The `materials` group is shared by every kind, so these three showed up in
    # the Rocks, Grass, Reeds, Flowers and Fish sections as well -- five
    # sections offering a choice of bark for something with no bark in it. The
    # house rule is that a section does not show a slider it cannot use, it
    # leaves it out, and `pipeline.build` only reads these two for the kinds
    # that grow a skeleton.
    P("materials.bark", "Bark", "bark", kind="choice", group="materials",
      kinds=("tree", "bush"), choices=materials.WOOD_NAMES),
    P("materials.core", "Heartwood", "heartwood", kind="choice", group="materials",
      kinds=("tree", "bush"), choices=materials.WOOD_NAMES),
    P("materials.leaf", "Leaf", "leaf_broadleaf", kind="choice", group="materials",
      kinds=("tree", "bush"), choices=materials.LEAF_NAMES),
)
del P

BY_PATH = {p.path: p for p in PARAMS}
GROUPS = tuple(dict.fromkeys(p.group for p in PARAMS))


# --- dotted-path access -----------------------------------------------------


def get(spec: dict, path: str, default: Any = None) -> Any:
    node: Any = spec
    for key in path.split("."):
        if not isinstance(node, dict) or key not in node:
            return default
        node = node[key]
    return node


def set_(spec: dict, path: str, value: Any) -> None:
    keys = path.split(".")
    node = spec
    for key in keys[:-1]:
        node = node.setdefault(key, {})
    node[keys[-1]] = value


def default_spec() -> dict:
    spec: dict = {}
    for p in PARAMS:
        set_(spec, p.path, p.default)
    return spec


# --- validation -------------------------------------------------------------


@dataclass
class Report:
    warnings: list[str] = field(default_factory=list)

    def __bool__(self) -> bool:
        return not self.warnings


def validate(spec: dict) -> tuple[dict, Report]:
    """Fill in defaults, clamp to range, coerce types.

    Always returns a usable spec. Anything it had to change is reported rather
    than raised, because the two things that write specs most often are a
    slider drag and a language model, and neither should be able to hard-fail
    a batch run.
    """
    rep = Report()
    out = default_spec()

    known = set(BY_PATH)
    for path in _leaf_paths(spec):
        if path not in known:
            rep.warnings.append(f"ignored unknown parameter {path!r}")

    for p in PARAMS:
        raw = get(spec, p.path, _MISSING)
        if raw is _MISSING:
            continue
        try:
            val = _coerce(p, raw)
        except (TypeError, ValueError):
            rep.warnings.append(f"{p.path}: {raw!r} is not a {p.kind}, using {p.default!r}")
            continue
        if p.kind in ("float", "int"):
            lo, hi = p.lo, p.hi
            if val < lo or val > hi:
                rep.warnings.append(f"{p.path}: {val} clamped to [{lo}, {hi}]")
                val = min(max(val, lo), hi)
            if p.kind == "int":
                val = int(round(val))
        elif p.kind == "choice" and val not in p.choices:
            # NAME THE CONSEQUENCE, NOT THE RULE. This used to read "not one of
            # (...), using 'leaf_blossom'", which is true and reads as
            # housekeeping; what actually happened is that four species were
            # DRAWN IN PINK and shipped. A warning that says what the asset will
            # look like is a different thing to scroll past.
            #
            # And this is the last moment anyone can see it. The substituted
            # value is what gets saved, so from here on the file on disk is
            # self-consistent and no later gate -- `buildcheck`, `selftest`,
            # `health` -- has anything to find. See `_PLANT_MATERIALS`.
            rep.warnings.append(
                f"{p.path}: {val!r} is not on the menu, so this asset will be "
                f"BUILT AND DRAWN AS {p.default!r} instead -- nothing "
                f"downstream will report it again. Menu: {p.choices}"
            )
            continue
        set_(out, p.path, val)

    # Cross-parameter checks. These are the combinations that produce a tree
    # that generates fine and looks wrong, so they warn rather than clamp.
    #
    # Scoped to the kind, because every one of them is about branch growth and a
    # rock has no branches. Unscoped, saving any rock spec printed a warning
    # about twig radius -- noise that trains a designer to stop reading warnings,
    # which is the only thing that makes the real ones useless.
    if "growth" not in kindlib.groups_for(get(out, "kind")):
        return out, rep

    if get(out, "growth.kill_m") >= get(out, "growth.influence_m"):
        rep.warnings.append(
            "growth.kill_m >= growth.influence_m: targets die before they can pull a "
            "branch, so the tree will be a bare trunk"
        )
    if get(out, "growth.step_m") > get(out, "growth.influence_m"):
        rep.warnings.append(
            "growth.step_m > growth.influence_m: branches overshoot their targets"
        )
    if get(out, "trunk.clear_frac") >= get(out, "crown.center_frac") + get(
        out, "crown.height_frac"
    ) / 2:
        rep.warnings.append(
            "trunk.clear_frac reaches above the crown: the crown will be sparse or empty"
        )
    # Half a voxel depends on the voxel size, so this warning has to be
    # resolution-aware: 4.5 cm twigs are under the floor at 10 cm and more than
    # two voxels thick at 2 cm.
    tip = get(out, "growth.tip_radius_m")
    half_voxel = float(get(out, "resolution_cm")) / 100.0 / 2.0
    if tip < half_voxel:
        rep.warnings.append(
            f"growth.tip_radius_m {tip} m is under half a voxel at "
            f"{get(out, 'resolution_cm')} cm; twigs will be drawn at the one-voxel "
            "minimum instead"
        )
    return out, rep


_MISSING = object()


def _coerce(p: Param, raw: Any) -> Any:
    if p.kind == "bool":
        if isinstance(raw, bool):
            return raw
        if isinstance(raw, (int, float)):
            return bool(raw)
        if isinstance(raw, str):
            return raw.strip().lower() in ("1", "true", "yes", "on")
        raise TypeError(raw)
    if p.kind in ("choice", "text"):
        if not isinstance(raw, str):
            raise TypeError(raw)
        return raw
    if isinstance(raw, bool):
        raise TypeError(raw)
    return float(raw)


def _leaf_paths(node: Any, prefix: str = "") -> list[str]:
    if not isinstance(node, dict):
        return [prefix]
    out: list[str] = []
    for k, v in node.items():
        path = f"{prefix}.{k}" if prefix else k
        out.extend(_leaf_paths(v, path))
    return out


# --- io ---------------------------------------------------------------------


def canonical_json(spec: dict) -> str:
    """Stable text for hashing: sorted keys, fixed separators.

    Python's built-in hash() is salted per process, so it cannot be used for
    anything that has to reproduce a tree tomorrow.
    """
    return json.dumps(spec, sort_keys=True, separators=(",", ":"))


def spec_hash(spec: dict) -> str:
    """What this SPEC is. Library identity: two specs with this hash are the
    same authored species, and `notes` is left out because it is free text for
    a person and changing it must not make the library think it has a new
    species.

    NOT what decides which individual you get -- that is `seed_hash` below, and
    the two are deliberately different functions.
    """
    body = {k: v for k, v in spec.items() if k != "notes"}
    return hashlib.blake2b(canonical_json(body).encode(), digest_size=8).hexdigest()


# Fields a spec may carry that must NOT change which individual comes out.
#
# WHY THIS IS A SECOND HASH AND NOT AN EDIT TO spec_hash. `spec_hash` answers
# "which species is this", and the library, the .vxa metadata and the cache key
# all want a pose change to be a different entry -- a perched raven and a flying
# raven are two assets, saved separately and placed separately. `pipeline.rng_for`
# is asking a different question: "which individual of this species is seed 7".
# The answer to that must not move when the pose does.
#
# WHAT WAS WRONG. `common-raven` seed 7 perched and `common-raven` seed 7 flying
# were two DIFFERENT ravens -- different length, different marking phase,
# different speckle field -- because the pose is part of the spec, the spec hash
# is mixed into the seed, and a different seed is a different animal. So a bird
# could not land: it changed size and markings on the way down. `forge/bird.py`
# wrote that up as something to fix here rather than there, and this is it.
#
# HOW IT IS APPLIED, AND WHY IT IS NOT A DELETE. Every validated spec carries
# every parameter, birds included, because `validate` starts from
# `default_spec()` -- so DELETING `bird.pose` from the body would change the
# canonical JSON of a tree, a rock and a fish as well, and reseed the entire
# library for the second time in a day. Instead the field is NORMALISED to the
# value a default spec has. Anything that never authored a pose already holds
# that value, so its canonical JSON comes back byte for byte identical and its
# seeding is untouched; and the fifteen perched species are untouched too,
# because "perched" IS the default. Only the five species authored `flying`
# reseed, once, onto the individual their perched twin already was -- which is
# the point of the change.
#
# WHAT ELSE WAS CONSIDERED AND LEFT OUT. `placement.*`, `biomes.*` and `flock.*`
# are read by no generator, so on the merits they belong here too: retuning
# where a species lives should not redraw the animal. They are out because every
# one of them is authored away from its default on nearly every spec, so
# normalising them would reseed the whole library -- the exact cost this change
# is written to avoid. If a library-wide reseed is ever acceptable for another
# reason, that is the moment to add them, and not before. `resolution_cm` is out
# for a different reason: `pipeline.build(..., resolution_cm=...)` already
# overrides the lattice without touching the spec, which is how
# `tools/birdprobe.py --lattice` compares 1 cm with 5 cm on one individual.
SEED_INVARIANT: tuple[str, ...] = ("bird.pose",)

# Fields DELETED from the seed hash rather than normalised in it, for the one
# reason normalising cannot cover: a parameter that did not exist yesterday.
#
# THE ARITHMETIC THAT FORCES THIS, because it is not obvious and it cost the
# library a full reseed twice already. `validate` starts from `default_spec()`,
# so every saved spec carries every parameter -- which means ADDING A ROW TO
# `PARAMS` PUTS A NEW KEY IN ALL 828 SPECS' CANONICAL JSON. Normalising that key
# to its default (the `SEED_INVARIANT` route above) does not help: the key is
# still in the body, the bytes are still new, every hash moves and every species
# in the library becomes a different individual. The README already records what
# that costs -- "adding any row to `spec.PARAMS` changes every spec's hash ...
# every species in the library moved to a different individual", which is how
# `hero-sequoia` landed on a shedding seed. Deleting the key restores the exact
# bytes the library hashed before the row existed, so the reseed is zero.
#
# `tools/trunkform.py --hashes` measures that claim against a snapshot rather
# than asserting it: 828 of 828 seed hashes unchanged, and it is a one-line
# check that would have caught the two earlier reseeds while they were still
# reversible.
#
# WHY TAPER BELONGS HERE ON ITS MERITS AS WELL, and not only as an accounting
# trick. A before/after render of a taper change has to be the SAME individual
# on both sides or it is not a comparison -- `tools/plantsheet.py` exists
# because scaling each tile separately "silently undoes the comparison", and
# drawing two different trees undoes it far more thoroughly. Seeded from a hash
# that taper is part of, the before tree and the after tree differ in height,
# lean, crown and every twig, and no amount of locked scale recovers that. So:
# taper changes the SHAPE OF THE STEM of a given individual, exactly as
# `bird.pose` changes the posture of a given bird.
#
# WHAT DOES NOT GO HERE: anything that makes it a different plant. Height,
# crown radius and trunk radius are all seeded, and should be -- they are what
# `variation` draws per individual. This list is for a field that redraws the
# same individual differently, and it should stay as short as `SEED_INVARIANT`.
SEED_EXCLUDED: tuple[str, ...] = ("trunk.taper",)


def seed_hash(spec: dict) -> str:
    """Which INDIVIDUAL of a species you get. See `SEED_INVARIANT` above.

    Equal to `spec_hash` for everything that does not author a field in that
    list, which today is every spec in the library except the five birds that
    fly. `tools/birdprobe.py --pose` measures both halves of that: that the two
    poses of one species agree, and that two seeds still do not.
    """
    body = {k: v for k, v in spec.items() if k != "notes"}
    for path in SEED_INVARIANT:
        if get(body, path, _MISSING) is _MISSING:
            continue          # not carried: nothing to normalise, bytes unchanged
        row = BY_PATH[path]
        if get(body, path) == row.default:
            continue          # already the default: bytes unchanged
        body = copy.deepcopy(body)
        set_(body, path, row.default)
    for path in SEED_EXCLUDED:
        if get(body, path, _MISSING) is _MISSING:
            continue
        body = copy.deepcopy(body)
        parts = path.split(".")
        cur = body
        for key in parts[:-1]:
            cur = cur.get(key) if isinstance(cur, dict) else None
            if not isinstance(cur, dict):
                break
        if isinstance(cur, dict):
            cur.pop(parts[-1], None)
    return hashlib.blake2b(canonical_json(body).encode(), digest_size=8).hexdigest()


def load(path: str | Path) -> tuple[dict, Report]:
    with open(path, "r", encoding="utf-8") as fh:
        return validate(json.load(fh))


def save(spec: dict, path: str | Path) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(spec, fh, indent=2, sort_keys=True)
        fh.write("\n")


def patch(spec: dict, changes: dict[str, Any]) -> tuple[dict, Report]:
    """Apply {"crown.radius_m": 5.0, ...} and re-validate.

    This is the one entry point sliders and language edits share.
    """
    out = copy.deepcopy(spec)
    for path, value in changes.items():
        set_(out, path, value)
    return validate(out)


def realize(spec: dict, rng) -> tuple[dict, Report]:
    """Turn a species spec into one individual of that species.

    The spec describes a kind of tree; this picks a particular one -- a bit
    taller, leaning a bit further, crown a bit wider. Applied before growth, so
    the difference between two seeds is a difference in the tree, not only in
    how its twigs happened to fall.

    Deterministic: the caller's rng is already derived from (spec, seed).
    """
    amount = float(get(spec, "variation.amount"))
    if amount <= 0.0:
        return spec, Report()

    def u() -> float:
        """A draw in [-1, 1], pushed away from the middle.

        A flat uniform draw has a mean absolute value of 0.5, so half the
        authored spread is never used and a batch of twelve piles up around the
        species average -- which is most of why a batch reads as one tree
        repeated even when the nominal spread is wide. Bending the draw toward
        its ends raises the mean absolute value to 1/1.6 = 0.625, a quarter more
        typical deviation, WITHOUT widening the range: no individual can leave
        the spread the designer set.
        """
        t = float(rng.random()) * 2.0 - 1.0
        return abs(t) ** 0.6 if t >= 0.0 else -(abs(t) ** 0.6)

    def spread(path: str) -> float:
        return amount * float(get(spec, path))

    changes: dict[str, Any] = {
        "height_m": get(spec, "height_m") * (1.0 + spread("variation.height") * u()),
        "crown.radius_m": get(spec, "crown.radius_m")
        * (1.0 + spread("variation.crown_radius") * u()),
        "trunk.radius_base_m": get(spec, "trunk.radius_base_m")
        * (1.0 + spread("variation.trunk_radius") * u()),
        "crown.squash": get(spec, "crown.squash") * (1.0 + spread("variation.shape") * u()),
        "trunk.lean_deg": max(0.0, get(spec, "trunk.lean_deg") + spread("variation.lean_deg") * u()),
        "growth.gravity": get(spec, "growth.gravity")
        + spread("variation.droop") * 0.30 * u(),
        "foliage.density": get(spec, "foliage.density")
        * (1.0 + spread("variation.density") * u()),
    }

    # --- proportion ---------------------------------------------------------
    # Size variation alone gives twelve copies of one tree at twelve sizes:
    # every individual still carries its crown over the same fraction of its
    # height, so every silhouette is the same silhouette. This varies the
    # SHARE of the tree that is crown.
    #
    # The crown is re-anchored by its TOP rather than its centre, so a longer
    # crown reaches further down the bole instead of pushing up past the
    # tree's own height and quietly re-introducing the height variation that
    # is already handled above.
    #
    # `trunk.clear_frac` moves the opposite way, because a long crown and a
    # long bare bole are not both available on the same stem. It is carried
    # along for consistency rather than for effect: measured over eight seeds,
    # taking it from 0.12 to 0.50 changes nothing at all under `colonize`
    # (growth targets sit in the crown envelope regardless, so branches simply
    # colonise back down into it) and moves the lowest foliage by 300% under
    # `whorl`, where it clamps the bottom ring directly.
    prop = spread("variation.proportion")
    if prop > 0.0:
        squash = float(changes["crown.squash"])
        hf0 = float(get(spec, "crown.height_frac"))
        top = float(get(spec, "crown.center_frac")) + hf0 * float(
            get(spec, "crown.squash")) * 0.5
        q = u()
        # Clamped HERE rather than left to `validate`, because the crown centre
        # below is derived from this number. Eight of the twenty-one tree specs
        # author a crown height fraction high enough to hit the ceiling, and
        # anchoring the centre against a value that then got clamped somewhere
        # else puts the crown's base underground -- where the targets are
        # silently discarded and the crown comes out short, with nothing
        # anywhere saying so.
        row = BY_PATH["crown.height_frac"]
        hf = min(max(hf0 * (1.0 + prop * q), row.lo), row.hi)
        # And the crown cannot hang below the ground. Its LENGTH is
        # height_frac times squash and squash varies too, so bounding
        # height_frac on its own is not enough: on the eight specs that
        # already author a crown near the ceiling, a long crown drawn on a
        # tall-squashed individual put the crown base up to 0.12 of the tree's
        # height underground, where `envelope.points` drops the targets on the
        # floor and the crown quietly comes out short instead.
        hf = min(hf, max(top, 0.15) / max(squash, 1e-6))
        hf = min(max(hf, row.lo), row.hi)
        changes["crown.height_frac"] = hf
        changes["crown.center_frac"] = top - hf * squash * 0.5
        changes["trunk.clear_frac"] = float(get(spec, "trunk.clear_frac")) * (
            1.0 - prop * 0.8 * q)
    # `variation.shape` still nudges where the crown sits, on top of the
    # re-anchor above rather than instead of it.
    changes["crown.center_frac"] = float(changes.get(
        "crown.center_frac", get(spec, "crown.center_frac"))) * (
        1.0 + spread("variation.shape") * 0.5 * u())
    # Last word on the crown: whatever the two nudges above worked out to, its
    # base sits on the ground rather than under it. Targets below 0.15 m are
    # discarded without a word, so a crown pushed underground does not fail --
    # it comes back shorter than the numbers say it is.
    half = 0.5 * float(changes.get(
        "crown.height_frac", get(spec, "crown.height_frac"))) * float(
        changes["crown.squash"])
    changes["crown.center_frac"] = max(float(changes["crown.center_frac"]), half)

    # --- lopsidedness -------------------------------------------------------
    # Multiplicative on purpose: a species authored as a perfect surface of
    # revolution (asymmetry 0) stays one, and a species authored lopsided gets
    # individuals that are more and less so.
    lop = spread("variation.lopsided")
    if lop > 0.0:
        changes["crown.asymmetry"] = get(spec, "crown.asymmetry") * (1.0 + lop * u())
        changes["crown.offset"] = get(spec, "crown.offset") * (1.0 + lop * u())

    # --- leaf distribution --------------------------------------------------
    # The two shell depths are much the strongest of these; the last three are
    # texture and measured as weak on every spec tried, but they cost nothing
    # and they stop two individuals of equal size and proportion from carrying
    # identically arranged foliage.
    fol = spread("variation.foliage")
    if fol > 0.0:
        changes["crown.shell_upper"] = get(spec, "crown.shell_upper") * (1.0 + fol * u())
        changes["crown.shell_lower"] = get(spec, "crown.shell_lower") * (1.0 + fol * u())
        changes["foliage.top_bias"] = get(spec, "foliage.top_bias") * (1.0 + fol * u())
        changes["foliage.clustering"] = get(spec, "foliage.clustering") * (
            1.0 + fol * 0.7 * u())
        changes["foliage.rough"] = get(spec, "foliage.rough") * (1.0 + fol * 0.7 * u())

    if get(spec, "variation.rotate"):
        facing = float(rng.random()) * 360.0
        changes["trunk.lean_dir_deg"] = facing
        changes["crown.lean_dir_deg"] = (facing + float(rng.random()) * 120.0 - 60.0) % 360.0

    return patch(spec, changes)


def params_for(kind: str) -> tuple[Param, ...]:
    """Parameters that apply to an asset kind.

    Mostly derived from each parameter's GROUP, so adding a parameter to an
    existing group picks up the right kinds automatically and a new kind is one
    entry in `kinds.py`. A row may narrow that further with `kinds=` when the
    group is right but the row is not -- the biome weights are the case: bare
    rock is a legitimate place for a boulder and an impossible one for an oak.
    """
    allowed = set(kindlib.groups_for(kind))
    return tuple(p for p in PARAMS
                 if p.group in allowed and kindlib.applies(p.kinds, kind))


def ui_schema(kind: str | None = None) -> list[dict]:
    """The slider table, as data. Feeds the web UI and the language box."""
    rows = params_for(kind) if kind else PARAMS
    return [
        {
            "path": p.path,
            "label": p.label,
            "kind": p.kind,
            "default": p.default,
            "lo": p.lo,
            "hi": p.hi,
            "step": p.step,
            "choices": list(p.choices),
            "group": p.group,
            "help": p.help,
        }
        for p in rows
    ]
