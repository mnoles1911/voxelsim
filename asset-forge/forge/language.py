"""Plain-language slider edits, computed locally.

No network, no model, no API key. A designer types "much shorter and more
gnarled, sparser canopy" and this maps it onto parameter changes using a fixed
vocabulary of design words.

The trade against asking a model is deliberate: this understands a bounded set
of phrases, but it is instant, free, offline, and — most importantly —
*deterministic*. The same sentence always produces the same edit, which is what
you want from a tool you are going to type into a hundred times. When it does
not recognise a word it says so, rather than guessing; an unknown word is a gap
in the vocabulary to fill, not a silent no-op.

Each concept is a recipe over the real parameters, written against how the
generator actually works — "twiggier" is a smaller consumption radius and more
growth targets, not a branch-count slider, because there is no branch-count
slider.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from . import spec as specmod

# How much one unit of a concept moves each parameter.
#   ("mul", a) -> value *= 1 + a * strength      (physical dimensions)
#   ("add", a) -> value += a * strength          (0..1 knobs, angles)
#   ("set", v) -> value = v                      (choices and switches)
MUL, ADD, SET = "mul", "add", "set"


@dataclass(frozen=True)
class Concept:
    key: str
    summary: str
    edits: tuple[tuple[str, str, object], ...]


CONCEPTS: dict[str, Concept] = {
    c.key: c
    for c in (
        Concept("height", "overall height", ((("height_m"), MUL, 0.28),)),
        Concept("trunk", "trunk thickness", (("trunk.radius_base_m", MUL, 0.35),)),
        Concept("gnarl", "gnarled, twisted wood",
                (("trunk.wander", ADD, 0.22), ("growth.jitter", ADD, 0.07),
                 ("trunk.lean_deg", ADD, 3.0))),
        Concept("droop", "drooping / weeping branches",
                (("growth.gravity", ADD, -0.28), ("foliage.droop_m", ADD, 0.22))),
        Concept("spread", "crown width", (("crown.radius_m", MUL, 0.28),)),
        Concept("density", "how full the canopy is",
                (("foliage.density", ADD, 0.14), ("foliage.coverage", ADD, 0.07),
                 ("crown.points", MUL, 0.22))),
        Concept("fine", "twiggier, finer branching",
                (("growth.kill_m", MUL, -0.30), ("growth.step_m", MUL, -0.22),
                 ("crown.points", MUL, 0.35), ("growth.tip_radius_m", MUL, -0.15))),
        Concept("clear", "bare trunk below the crown",
                (("trunk.clear_frac", ADD, 0.13),)),
        Concept("lean", "leaning / wind-swept",
                (("trunk.lean_deg", ADD, 9.0), ("crown.lean_deg", ADD, 7.0),
                 ("crown.shell_upper", ADD, -0.06))),
        Concept("clump", "size of each foliage clump",
                (("foliage.clump_radius_m", MUL, 0.32),)),
        # A thinner leafy layer, not a thicker one: the sense of this control
        # inverted when hollowness became a shell THICKNESS, so "airy" now
        # subtracts where it used to add.
        Concept("airy", "open, layered crown rather than a solid mass",
                (("crown.shell_upper", ADD, -0.18),
                 ("crown.shell_lower", ADD, -0.15),
                 ("foliage.coverage", ADD, -0.08))),
        Concept("variation", "how much individuals differ from each other",
                (("variation.amount", MUL, 0.45),)),
        Concept("roots", "flared roots at the base", (("trunk.buttress", ADD, 0.32),)),
        Concept("crownheight", "how tall the crown is",
                (("crown.height_frac", ADD, 0.13),)),
        Concept("crownup", "how high the crown sits",
                (("crown.center_frac", ADD, 0.08),)),
        Concept("squash", "crown squashed or stretched vertically",
                (("crown.squash", MUL, 0.25),)),

        # --- fish ------------------------------------------------------------
        # Same rule as the tree concepts: each is a recipe over the parameters
        # that actually exist, not a wish. "Streamlined" is not a slider — it is
        # a shallower body, a slimmer wrist and a harder taper together, which
        # is what a fish that swims fast is.
        Concept("fishdepth", "how deep-bodied the fish is",
                (("fish.depth_ratio", MUL, 0.30),)),
        Concept("fishwidth", "how round or knife-thin the fish is in section",
                (("fish.width_ratio", MUL, 0.30), ("fish.section", ADD, 0.25))),
        Concept("fishlength", "how long the fish is",
                (("fish.length_m", MUL, 0.25),)),
        Concept("streamline", "streamlined for speed",
                (("fish.peduncle", MUL, -0.30), ("fish.width_follow", ADD, 0.25),
                 ("fish.caudal_fork", ADD, 0.15), ("fish.snout", MUL, -0.15))),
        Concept("tailsize", "how big the tail fin is",
                (("fish.caudal_span", MUL, 0.28), ("fish.caudal_len", MUL, 0.25))),
        Concept("tailfork", "how deeply the tail is notched",
                (("fish.caudal_fork", ADD, 0.20),)),
        Concept("dorsalsize", "the fin on the back",
                (("fish.dorsal_height", MUL, 0.35),)),
        Concept("finsize", "the paired and belly fins",
                (("fish.pectoral", MUL, 0.35), ("fish.pelvic", MUL, 0.35),
                 ("fish.anal_height", MUL, 0.25))),
        Concept("countershade", "dark back over a pale belly",
                (("fish.back_frac", ADD, 0.12), ("fish.belly_frac", ADD, 0.10))),
        Concept("marking", "how bold the markings are",
                (("fish.pattern_width", MUL, 0.35),
                 ("fish.pattern_strength", ADD, 0.15))),
        Concept("eyesize", "eye size",
                (("fish.eye", ADD, 1.0),)),
        Concept("shoal", "how many of them turn up together",
                (("detail.school_max", MUL, 0.6), ("detail.per_100m2", MUL, 0.6))),

        # --- birds -----------------------------------------------------------
        # Same rule again: each is a recipe over the parameters that exist, not
        # a wish. "Upright" is not a slider -- it is a posture angle and a
        # steeper neck together, because a bird that sits up also lifts its
        # head, and moving only one of them gives a bird leaning back.
        #
        # THE FIVE SHARE ROWS ARE NORMALISED, so a concept that pushes one of
        # them up pushes the other four down in proportion. That is why
        # "longer tail" is a single edit and not a tail edit plus four
        # compensations: `bird._params` does the compensating.
        Concept("birdsize", "how big the bird is",
                (("bird.length_m", MUL, 0.25),)),
        Concept("birdtail", "how much of the bird is tail",
                (("bird.tail_frac", MUL, 0.30),)),
        Concept("birdneck", "how much of the bird is neck",
                (("bird.neck_frac", MUL, 0.55), ("bird.neck_up_deg", ADD, 8.0))),
        Concept("birdbill", "how long the bill is",
                (("bird.bill_frac", MUL, 0.35),)),
        Concept("billdepth", "how heavy the bill is",
                (("bird.bill_depth", MUL, 0.35),)),
        Concept("birdhead", "how big the head is",
                (("bird.head_size", MUL, 0.28),)),
        Concept("upright", "how upright the bird sits",
                (("bird.posture_deg", ADD, 16.0),
                 ("bird.neck_up_deg", ADD, 10.0))),
        Concept("plump", "how deep-bodied the bird is",
                (("bird.body_depth", MUL, 0.25), ("bird.breast", ADD, 0.08))),
        Concept("birdlegs", "how long the legs are",
                (("bird.leg_len", MUL, 0.55),)),
        Concept("crest", "the crest on the head",
                (("bird.crest", ADD, 0.35),)),
        Concept("wingspan", "how far the wings reach",
                (("bird.wing_span", MUL, 0.22),)),
        Concept("wingnarrow", "how narrow the wings are",
                (("bird.wing_aspect", MUL, 0.30),)),
        Concept("wingsweep", "how far back the wingtips are carried",
                (("bird.wing_sweep", ADD, 0.25),)),
        Concept("wingup", "how far up the wings are held",
                (("bird.wing_dihedral", ADD, 0.22),)),
        Concept("birdmark", "how bold the markings are",
                (("bird.mark_width", MUL, 0.35),
                 ("bird.mark_strength", ADD, 0.15))),
        Concept("birdeye", "eye size",
                (("bird.eye", ADD, 1.0),)),
        Concept("flock", "how many of them turn up together",
                (("flock.size_max", MUL, 0.7), ("flock.per_hectare", MUL, 0.7))),

        # --- land animals -----------------------------------------------------
        # Same rule a third time: each is a recipe over parameters that exist,
        # not a wish. "Leggy" is not a slider -- it is the shoulder height and
        # the hip height together, because raising one alone tips the animal
        # over rather than standing it up, and the height rows are what the limb
        # lengths are derived FROM.
        #
        # AND ONE CONCEPT HERE IS TWO EDITS FOR A REASON THE OTHERS ARE NOT.
        # `quad.hip_h` is a RATIO to the shoulder height, so "taller" has to
        # move the shoulder and leave the ratio alone or a taller animal also
        # becomes a differently-proportioned one. `humped` is the opposite case
        # and moves the ratio without touching the height, which is exactly what
        # separates a bison from a bigger cow.
        Concept("quadsize", "how big the animal is",
                (("quad.length_m", MUL, 0.25),)),
        Concept("leggy", "how tall it stands",
                (("quad.shoulder_h", MUL, 0.22),)),
        Concept("humped", "shoulders higher than hips",
                (("quad.hip_h", ADD, -0.12), ("quad.hump", ADD, 0.20))),
        Concept("quadneck", "how much of it is neck",
                (("quad.neck_frac", MUL, 0.45),)),
        Concept("necklift", "how high it carries its head",
                (("quad.neck_deg", ADD, 22.0),)),
        Concept("quadmuzzle", "how long the muzzle is",
                (("quad.muzzle_frac", MUL, 0.40),)),
        Concept("muzzledepth", "how heavy the muzzle is",
                (("quad.muzzle_depth", MUL, 0.30), ("quad.jaw", ADD, 0.15))),
        Concept("quadhead", "how big the head is",
                (("quad.head_size", MUL, 0.25),)),
        Concept("barrel", "how deep-bodied it is",
                (("quad.depth", MUL, 0.25), ("quad.waist", ADD, 0.10))),
        Concept("quadwidth", "how broad it is across the back",
                (("quad.width", MUL, 0.28),)),
        Concept("quadears", "how big the ears are",
                (("quad.ear_len", MUL, 0.45),)),
        Concept("quadhorn", "how big the horns or antlers are",
                (("quad.horn_len", MUL, 0.40), ("quad.horn_thick", MUL, 0.15))),
        Concept("hornspread", "how wide the horns reach",
                (("quad.horn_spread", ADD, 0.35),)),
        Concept("quadtail", "how long the tail is",
                (("quad.tail_len", MUL, 0.40),)),
        Concept("bushytail", "how thick the tail is",
                (("quad.tail_thick", MUL, 0.40), ("quad.tail_taper", ADD, 0.18))),
        Concept("tailup", "how high the tail is carried",
                (("quad.tail_deg", ADD, 35.0),)),
        Concept("quadmane", "the mane or crest along the neck",
                (("quad.mane", ADD, 0.30),)),
        Concept("quadlegs", "how thick the limbs are",
                (("quad.leg_thick", MUL, 0.30),)),
        Concept("crouch", "how folded the hind legs are",
                (("quad.hock", ADD, 0.25),)),
        Concept("quadmark", "how bold the markings are",
                (("quad.mark_width", MUL, 0.35),
                 ("quad.mark_strength", ADD, 0.15))),
        Concept("quadcount", "how many bands or spots",
                (("quad.mark_count", MUL, 0.6),)),
        Concept("herd", "how many of them turn up together",
                (("herd.size_max", MUL, 0.7), ("herd.per_hectare", MUL, 0.7))),
    )
}

# Phrase -> (concept, direction). Longest phrases match first, so "bare trunk"
# beats "bare" and "less dense" beats "dense".
PHRASES: dict[str, tuple[str, float]] = {}


CONFLICTS: list[str] = []


def _say(concept: str, more: str, less: str = "") -> None:
    """Register phrases for a concept. Duplicates are recorded, not silently
    overwritten -- as the vocabulary grows, a phrase quietly reassigned to a
    different concept is a bug nobody would notice."""
    for direction, group in ((+1.0, more), (-1.0, less)):
        for raw in (group.split("|") if group else []):
            phrase = raw.strip()
            if not phrase:
                continue
            existing = PHRASES.get(phrase)
            if existing is not None and existing != (concept, direction):
                CONFLICTS.append(f"{phrase!r}: {existing} vs {(concept, direction)}")
                continue
            PHRASES[phrase] = (concept, direction)


_say("height", "taller|tall|higher|bigger|larger", "shorter|short|smaller|squat|stunted|low")
_say("trunk", "thicker trunk|fatter trunk|stouter|thick trunk|heavier trunk",
     "thinner trunk|slender|slim|spindly|thin trunk")
_say("gnarl", "gnarled|twisted|knotted|weathered|craggy|contorted|wonky|characterful",
     "straight|clean|regular|orderly|neat")
_say("droop", "droopy|drooping|weeping|sagging|hanging|pendulous|cascading",
     "upright|reaching up|perky|erect")
_say("spread", "wider|broader|spreading|wide|broad|sprawling",
     "narrower|narrow|tighter|compact|slim crown")
_say("density", "denser|fuller|lush|dense|thick canopy|full canopy|leafy|bushier",
     "sparser|sparse|thinner canopy|patchy|scraggly|bare-ish|see-through")
_say("fine", "twiggier|finer|finer branching|more detailed|delicate|intricate|wispy",
     "coarser|chunkier|stubby|blockier|simpler")
_say("clear", "bare trunk|high canopy|tall trunk|leggy|clean trunk|raised crown",
     "low branches|bushy|branches to the ground|low canopy|shrubby")
_say("lean", "leaning|wind-swept|windswept|wind-blasted|wind-bent|storm-beaten|coastal|exposed",
     "vertical|plumb|even")
_say("clump", "bigger leaves|larger clumps|bigger clumps|coarse foliage|chunky foliage",
     "smaller leaves|finer foliage|smaller clumps|fine leaves")
_say("airy", "airy|open|layered|see-through crown|light canopy|tiered",
     "solid|blobby|one mass|heavy canopy")
_say("variation", "more varied|more variety|more different|more random",
     "more consistent|more uniform|more similar|less varied")
_say("roots", "flared roots|buttress|buttressed|root flare|flared base|rooted",
     "no roots|clean base")
_say("crownheight", "taller crown|deeper crown|longer crown", "shallower crown|flatter crown")
_say("crownup", "crown higher|higher crown|crown up", "crown lower|lower crown|crown down")
_say("squash", "stretched|elongated|tall crown", "squashed|flattened|squat crown")

# Bare comparatives and "more/less <noun>" phrasings. Longest-first matching
# means "thicker trunk" and "thicker foliage" still beat bare "thicker", so the
# unqualified word only fires when nothing more specific was said.
_say("height", "loftier|towering|huge|giant|massive|enormous|taller tree",
     "tiny|dwarf|miniature|low-growing|diminutive|small")
_say("trunk", "thicker|thick|stocky|beefier|sturdier|chunky trunk|wider trunk|fat trunk",
     "thinner|thin|skinny|willowy|narrow trunk|wispier trunk")
_say("density", "richer|rich|more leaves|more foliage|leafier|heavier foliage|"
                "thicker foliage|thicker canopy|denser canopy|packed|crowded|"
                "more leafy|fuller canopy",
     "fewer leaves|less leaves|less foliage|thinner foliage|sparser canopy|"
     "sparse canopy|bald|threadbare|skeletal|gappy|barer|emptier")
_say("spread", "expansive|splayed|wider crown|broader crown|more spread",
     "tighter crown|narrow crown|narrower crown|pinched")
_say("clump", "big leaves|chunkier leaves|broad leaves", "tiny leaves|small leaves")
_say("fine", "more branches|more branching|more twigs|denser branching|busier",
     "fewer branches|less branches|fewer twigs|sparser branching")

# Fish. Phrases are qualified ("longer fish", not "longer") wherever a bare word
# already belongs to a tree concept -- `_say` records a collision rather than
# silently reassigning it, and a phrase quietly moved to another concept is a
# bug nobody would ever notice.
_say("fishdepth", "deeper bodied|deeper body|deep-bodied|deep bodied|chunkier body|"
                  "taller body",
     "slimmer body|shallower body|slimmer fish|slenderer fish")
_say("fishwidth", "rounder in section|fatter fish|tubby|round-bodied|tube-shaped",
     "flatter|knife-edged|laterally flattened|compressed|thinner fish|"
     "flattened side to side")
_say("fishlength", "longer fish|bigger fish|larger fish",
     "shorter fish|smaller fish|tiny fish")
_say("streamline", "streamlined|torpedo|fast swimmer|built for speed|pelagic",
     "slow swimmer|lumbering|blunt")
_say("tailsize", "bigger tail|larger tail|bigger tail fin", "smaller tail|smaller tail fin")
_say("tailfork", "deeper fork|more forked|deeply forked", "shallower fork|less forked")
_say("dorsalsize", "taller dorsal|bigger dorsal|taller back fin|higher dorsal",
     "lower dorsal|smaller dorsal|lower back fin")
_say("finsize", "bigger fins|larger fins|finnier", "smaller fins|reduced fins|finless")
_say("countershade", "more countershading|darker back|stronger countershading|"
                     "paler belly",
     "less countershading|even colour|uniform colour|flat coloured")
_say("marking", "bolder markings|stronger markings|bolder pattern|louder",
     "fainter markings|subtler markings|weaker pattern|washed out")
_say("eyesize", "bigger eye|larger eye|big-eyed", "smaller eye|beady")
_say("shoal", "bigger shoal|more of them|denser shoal|schooling",
     "smaller shoal|fewer of them|solitary")

# Birds. Qualified ("longer tail", not "longer") wherever a bare word already
# belongs to a tree or a fish concept -- `_say` records a collision rather than
# silently reassigning it, and a phrase quietly moved to another concept is a
# bug nobody would ever notice. `forge.cli selftest` prints CONFLICTS.
_say("birdsize", "bigger bird|larger bird|longer bird",
     "smaller bird|tiny bird|little bird")
_say("birdtail", "longer tail|long-tailed|long tailed|bigger tail feathers",
     "shorter tail|short-tailed|short tailed|stumpy tail")
_say("birdneck", "longer neck|long-necked|long necked|craned",
     "shorter neck|no neck|neckless|hunched")
_say("birdbill", "longer bill|long-billed|longer beak|daggered",
     "shorter bill|stubby bill|short beak")
_say("billdepth", "heavier bill|thicker bill|deeper bill|stout bill",
     "finer bill|thinner bill|needle bill|slender bill")
_say("birdhead", "bigger head|big-headed|larger head", "smaller head|small-headed")
_say("upright", "upright bird|sits up|perky posture|alert|standing tall",
     "horizontal|level|lying flat|crouched")
_say("plump", "plump|rounder bird|fat bird|pot-bellied|chunky bird",
     "slim bird|lean bird|sleek bird|streamlined bird")
_say("birdlegs", "longer legs|long-legged|leggy bird|wading",
     "shorter legs|short-legged|squat legs")
_say("crest", "crested|with a crest|tufted|topknot", "no crest|uncrested|smooth head")
_say("wingspan", "wider wings|broader wings|longer wings|bigger wingspan",
     "shorter wings|narrower span|stubby wings")
_say("wingnarrow", "narrow wings|slender wings|high aspect|plank wings",
     "broad wings|wide wings|paddle wings")
_say("wingsweep", "swept wings|swept back|raked wings|scythe wings",
     "straight wings|unswept")
_say("wingup", "wings raised|held in a v|v-shaped wings|dihedral",
     "wings drooped|drooping wings|flat wings")
_say("birdmark", "bolder plumage|stronger plumage|louder plumage",
     "plainer plumage|subtler plumage|drabber")
_say("birdeye", "bigger eyes|larger eyes|big-eyed bird",
     "smaller eyes|beady eyes")
_say("flock", "bigger flock|larger flock|flocking|in a flock",
     "smaller flock|alone|single bird")

# --- land animals ------------------------------------------------------------
#
# EVERY PHRASE HERE IS QUALIFIED -- "longer tail" already belongs to the bird
# concept and "bigger head" to another, so these say "bushier tail" and
# "heavier head" instead. That is not politeness: `_say` records a duplicate in
# `CONFLICTS` and REFUSES it rather than overwriting, so an unqualified
# collision would leave the land-animal concept silently unreachable while the
# vocabulary listing went on advertising it. `forge.cli vocab` prints
# `CONFLICTS`, and it is empty.
_say("quadsize", "bigger animal|larger animal|longer animal|heavier animal",
     "smaller animal|littler animal|dwarf animal")
# NONE OF THESE PHRASES MAY COLLIDE WITH A BIRD OR FISH ONE. `_say` records a
# duplicate in `CONFLICTS` and REFUSES it rather than overwriting, so a bare
# "longer legs" -- which `birdlegs` already owns -- would leave the land-animal
# concept unreachable while `forge.cli vocab` went on advertising it. That is a
# silent no-op in the vocabulary itself, and the first draft of this block shipped
# fourteen of them. `CONFLICTS` is empty and is the check.
_say("leggy", "longer in the leg|taller legs|tall at the shoulder|stilted|"
              "stands taller|higher off the ground|long legs on it",
     "shorter in the leg|low-slung|squat animal|belly to the ground|"
     "close to the ground|short legs on it")
_say("humped", "humped|shoulder hump|humped shoulders|withers hump|"
               "high at the shoulder|sloping back|bison-backed|hyena-backed",
     "level back|even back|flat backed|high at the rump|rump-high")
_say("quadneck", "longer neck on it|more neck|giraffe-necked|craning neck",
     "shorter neck on it|less neck|neck pulled in")
_say("necklift", "head up|head held high|alert head|browsing|neck raised|"
                 "looking up",
     "head down|grazing|head lowered|nose to the ground|neck lowered")
_say("quadmuzzle", "longer muzzle|long-muzzled|longer snout|long snout|"
                   "drawn-out face",
     "shorter muzzle|short-muzzled|blunt face|snub-nosed|flat face")
_say("muzzledepth", "heavy muzzle|heavy jaw|blunt muzzle|deep muzzle|"
                    "powerful jaw|heavy head",
     "fine muzzle|slender muzzle|delicate jaw|narrow muzzle")
_say("quadhead", "bigger skull|larger skull|heavier skull",
     "smaller skull|finer skull")
_say("barrel", "barrel-bodied|deep through the chest|deep chest|barrel chest|"
               "heavy-bodied|thickset|portly",
     "slab-sided|shallow-bodied|tucked up|lean animal|greyhound build|"
     "racy|whippet-thin")
_say("quadwidth", "broad-backed|wide-bodied|broad across the back|"
                  "wide across the back",
     "narrow-bodied|slab-thin|narrow across the back")
_say("quadears", "bigger ears|longer ears|big-eared|long-eared|"
                 "ears like a hare|huge ears",
     "smaller ears|shorter ears|small-eared|tiny ears")
_say("quadhorn", "bigger horns|longer horns|bigger antlers|longer antlers|"
                 "heavier rack|bigger rack|antlered|horned",
     "smaller horns|shorter horns|smaller antlers|shorter antlers|"
     "lighter rack|hornless|antlerless")
_say("hornspread", "wider horns|wider antlers|wide rack|spreading antlers|"
                   "spreading horns",
     "narrower horns|narrower antlers|tight rack|close-set horns")
_say("quadtail", "longer tail on it|more tail",
     "shorter tail on it|stub tail|bobtailed|docked")
_say("bushytail", "bushier tail|thicker tail|brush tail|plume tail|"
                  "plumed tail|fuller tail",
     "thinner tail|whip tail|ratty tail|wiry tail|rat-tailed")
_say("tailup", "tail up|tail carried high|tail raised|flagged tail",
     "tail down|tail carried low|tail tucked|drooping tail")
_say("quadmane", "maned|with a mane|dorsal crest|bristle crest|"
                 "crest along the spine|ridged back|shaggy neck",
     "no mane|maneless|smooth neck|clean spine")
_say("quadlegs", "thicker limbs|heavier limbs|stout legs|pillar legs|"
                 "sturdy legs",
     "finer limbs|thinner limbs|spindly legs|slender legs|delicate legs")
_say("crouch", "folded hind legs|deep hocks|springy|"
               "coiled hindquarters|hare-legged|folded haunches",
     "straight legs|column legs|stiff-legged|upright hind legs")
_say("quadmark", "louder coat|bolder coat|bolder stripes|bolder spots",
     "plainer coat|quieter coat|washed-out coat|fainter stripes|fainter spots")
_say("quadcount", "more stripes|more spots|finer stripes|finer spots|"
                  "more bands",
     "fewer stripes|fewer spots|broader stripes|broader spots|fewer bands")
_say("herd", "bigger herd|larger herd|in a herd|herding|in numbers",
     "smaller herd|on its own|lone animal|a single animal")

# Silhouettes set a shape outright rather than nudging a number.
SHAPES: dict[str, str] = {
    "conical": "cone", "cone-shaped": "cone", "cone shaped": "cone",
    "christmas tree": "cone", "spire": "cone", "conifer-shaped": "cone",
    "round": "sphere", "rounded": "sphere", "ball-shaped": "sphere",
    "globe": "sphere", "domed": "sphere",
    "umbrella": "umbrella", "flat-topped": "umbrella", "flat top": "umbrella",
    "parasol": "umbrella", "acacia-shaped": "umbrella",
    "columnar": "column", "column": "column", "poplar-shaped": "column",
    "cypress-shaped": "column", "pencil": "column",
    "vase": "vase", "vase-shaped": "vase", "goblet": "vase",
    "wedge": "wedge", "sheared": "wedge",
    "curtain": "hanging", "skirted": "hanging",
}

# Whole-tree switches.
SWITCHES: dict[str, tuple[tuple[str, str, object], ...]] = {
    "dead": (("foliage.enabled", SET, False), ("materials.bark", SET, "deadwood"),
             ("materials.core", SET, "deadwood")),
    "leafless": (("foliage.enabled", SET, False),),
    "bare": (("foliage.enabled", SET, False),),
    "no leaves": (("foliage.enabled", SET, False),),
    "alive": (("foliage.enabled", SET, True), ("materials.bark", SET, "bark"),
              ("materials.core", SET, "heartwood")),
    "in leaf": (("foliage.enabled", SET, True),),
    "needles": (("materials.leaf", SET, "leaf_needle"),),
    "needled": (("materials.leaf", SET, "leaf_needle"),),
    "blossom": (("materials.leaf", SET, "leaf_blossom"),),
    "flowering": (("materials.leaf", SET, "leaf_blossom"),),
    "in blossom": (("materials.leaf", SET, "leaf_blossom"),),
    "autumn": (("materials.leaf", SET, "leaf_autumn"),),
    "autumnal": (("materials.leaf", SET, "leaf_autumn"),),
    "dry leaves": (("materials.leaf", SET, "leaf_dry"),),
    "jungle leaves": (("materials.leaf", SET, "leaf_jungle"),),
    "pale bark": (("materials.bark", SET, "bark_pale"),),
    "white bark": (("materials.bark", SET, "bark_pale"),),
    "birch bark": (("materials.bark", SET, "bark_pale"),),

    # --- fish: outlines, markings and fittings ------------------------------
    #
    # LUNATE AND EMARGINATE ARE NOT SHAPES HERE, they are a forked tail with a
    # deep or a shallow notch, so the phrase sets both. That is the whole reason
    # `fish.caudal_shape` has five entries and not seven: a choice that
    # duplicated a slider position would be a choice that silently ignored it.
    "forked tail": (("fish.caudal_shape", SET, "forked"),),
    "square tail": (("fish.caudal_shape", SET, "truncate"),),
    "truncate tail": (("fish.caudal_shape", SET, "truncate"),),
    "straight tail": (("fish.caudal_shape", SET, "truncate"),),
    "rounded tail": (("fish.caudal_shape", SET, "rounded"),),
    "round tail": (("fish.caudal_shape", SET, "rounded"),),
    "fan tail": (("fish.caudal_shape", SET, "rounded"),),
    "pointed tail": (("fish.caudal_shape", SET, "pointed"),),
    "lunate tail": (("fish.caudal_shape", SET, "forked"),
                    ("fish.caudal_fork", SET, 0.72),
                    ("fish.caudal_span", SET, 1.7)),
    "crescent tail": (("fish.caudal_shape", SET, "forked"),
                      ("fish.caudal_fork", SET, 0.72),
                      ("fish.caudal_span", SET, 1.7)),
    "emarginate tail": (("fish.caudal_shape", SET, "forked"),
                        ("fish.caudal_fork", SET, 0.15)),
    "no tail": (("fish.caudal_shape", SET, "none"),),

    "striped": (("fish.pattern", SET, "stripe"),),
    "horizontal stripe": (("fish.pattern", SET, "stripe"),),
    "lateral stripe": (("fish.pattern", SET, "stripe"),),
    "barred": (("fish.pattern", SET, "bars"),),
    "vertical bars": (("fish.pattern", SET, "bars"),),
    "banded": (("fish.pattern", SET, "bars"),),
    "spotted": (("fish.pattern", SET, "spots"),),
    "speckled": (("fish.pattern", SET, "spots"),),
    "mottled": (("fish.pattern", SET, "mottle"),),
    "blotchy": (("fish.pattern", SET, "mottle"),),
    "saddled": (("fish.pattern", SET, "saddle"),),
    "plain": (("fish.pattern", SET, "none"),),
    "unmarked": (("fish.pattern", SET, "none"),),

    "sailfin": (("fish.dorsal_shape", SET, "sail"),
                ("fish.dorsal_height", SET, 0.9)),
    "spiny dorsal": (("fish.dorsal_shape", SET, "spiny"),),
    "ridge fin": (("fish.dorsal_shape", SET, "ridge"),),
    "no dorsal": (("fish.dorsal_shape", SET, "none"),),
    "barbels": (("fish.barbels", SET, 4), ("fish.barbel_len", SET, 0.18)),
    "whiskers": (("fish.barbels", SET, 4), ("fish.barbel_len", SET, 0.18)),
    "no barbels": (("fish.barbels", SET, 0),),
    "adipose fin": (("fish.adipose", SET, True),),
    "no adipose": (("fish.adipose", SET, False),),

    "silvery": (("materials.fish_flank", SET, "skin_silver"),
                ("materials.fish_belly", SET, "skin_pale")),
    "orange fish": (("materials.fish_back", SET, "skin_orange"),
                    ("materials.fish_flank", SET, "skin_orange"),
                    ("materials.fish_belly", SET, "skin_orange"),
                    ("materials.fish_fin", SET, "skin_orange"),
                    ("materials.fish_pattern", SET, "skin_pale")),
    "yellow fish": (("materials.fish_flank", SET, "skin_yellow"),
                    ("materials.fish_fin", SET, "skin_yellow")),
    "blue fish": (("materials.fish_back", SET, "skin_blue"),
                  ("materials.fish_flank", SET, "skin_blue")),
    "green fish": (("materials.fish_back", SET, "skin_green"),
                   ("materials.fish_flank", SET, "skin_green")),
    "red fins": (("materials.fish_fin", SET, "skin_red"),),
    "olive back": (("materials.fish_back", SET, "skin_olive"),),
}

# --- bird switches, kept in their own table ---------------------------------
#
# NOT MERGED INTO THE LITERAL ABOVE, and this is not tidiness. A Python dict
# literal with a duplicate key keeps the LAST one and says nothing, and three
# of the phrases below -- "square tail", "rounded tail" and "barred" -- are
# also fish phrases. Written into one literal, the bird versions silently
# replaced the fish ones, so typing "square tail" in the Fish section would
# have set a bird parameter and done nothing at all. `_merge_switches` records
# a collision into `CONFLICTS` instead, which is the same rule `_say` follows
# and for the same reason.
#
# The three that genuinely mean the same thing in both sections set BOTH
# parameters. A fish spec never reads `bird.*` and a bird spec never reads
# `fish.*`, so the wrong half is a no-op rather than a silent kind change --
# the same trick the species table uses.
BIRD_SWITCHES: dict[str, tuple[tuple[str, str, object], ...]] = {
    #
    # POSE IS A SWITCH AND NOT A CONCEPT, because it is not a matter of degree:
    # a folded wing and a spread one are two different shapes and there is
    # nothing in between that is a bird.
    "perched": (("bird.pose", SET, "perched"),),
    "sitting": (("bird.pose", SET, "perched"),),
    "flying": (("bird.pose", SET, "flying"),),
    "in flight": (("bird.pose", SET, "flying"),),
    "wings out": (("bird.pose", SET, "flying"),),
    "soaring": (("bird.pose", SET, "flying"),
                ("bird.wing_shape", SET, "slotted"),
                ("bird.wing_slots", SET, 5),
                ("bird.wing_dihedral", SET, 0.25)),
    "gliding": (("bird.pose", SET, "flying"),
                ("bird.wing_shape", SET, "soaring"),
                ("bird.wing_sweep", SET, 0.18)),

    # Tail outlines, in field-guide vocabulary. "Emarginate" is not a separate
    # entry for the same reason lunate is not one on a fish: it is a notched
    # tail with a shallow notch, so the phrase sets both, and an entry that
    # duplicated a slider position would be a choice that silently ignored it.
    "square tail": (("bird.tail_shape", SET, "square"),
                    ("fish.caudal_shape", SET, "truncate")),
    "rounded tail": (("bird.tail_shape", SET, "rounded"),
                     ("fish.caudal_shape", SET, "rounded")),
    "graduated tail": (("bird.tail_shape", SET, "graduated"),),
    "stepped tail": (("bird.tail_shape", SET, "graduated"),),
    "wedge tail": (("bird.tail_shape", SET, "wedge"),),
    "wedge-shaped tail": (("bird.tail_shape", SET, "wedge"),),
    "notched tail": (("bird.tail_shape", SET, "notched"),),
    "emarginate": (("bird.tail_shape", SET, "notched"),
                   ("bird.tail_fork", SET, 0.22)),
    "forked tail bird": (("bird.tail_shape", SET, "forked"),),
    "deeply forked tail": (("bird.tail_shape", SET, "forked"),
                           ("bird.tail_fork", SET, 0.72)),
    "streamers": (("bird.tail_shape", SET, "forked"),
                  ("bird.tail_fork", SET, 0.80),
                  ("bird.tail_frac", SET, 0.46)),
    "pin tail": (("bird.tail_shape", SET, "pointed"),),
    "pointed tail bird": (("bird.tail_shape", SET, "pointed"),),

    # Wing planforms, after Savile.
    "rounded wings": (("bird.wing_shape", SET, "elliptical"),
                      ("bird.wing_aspect", SET, 5.0)),
    "elliptical wings": (("bird.wing_shape", SET, "elliptical"),),
    "pointed wings": (("bird.wing_shape", SET, "pointed"),
                      ("bird.wing_aspect", SET, 7.6),
                      ("bird.wing_sweep", SET, 0.42)),
    "falcon wings": (("bird.wing_shape", SET, "pointed"),
                     ("bird.wing_aspect", SET, 7.9),
                     ("bird.wing_sweep", SET, 0.45)),
    "gull wings": (("bird.wing_shape", SET, "soaring"),
                   ("bird.wing_aspect", SET, 9.7)),
    "albatross wings": (("bird.wing_shape", SET, "soaring"),
                        ("bird.wing_aspect", SET, 15.0),
                        ("bird.wing_span", SET, 2.7)),
    "fingered wings": (("bird.wing_shape", SET, "slotted"),
                       ("bird.wing_slots", SET, 5)),
    "slotted wings": (("bird.wing_shape", SET, "slotted"),
                      ("bird.wing_slots", SET, 5)),
    "eagle wings": (("bird.wing_shape", SET, "slotted"),
                    ("bird.wing_slots", SET, 6),
                    ("bird.wing_aspect", SET, 6.9),
                    ("bird.wing_span", SET, 2.45)),

    # Bills, by what they are for.
    "hooked bill": (("bird.bill_hook", SET, 0.75), ("bird.bill_depth", SET, 0.52)),
    "raptor bill": (("bird.bill_hook", SET, 0.80), ("bird.bill_depth", SET, 0.55)),
    "dagger bill": (("bird.bill_depth", SET, 0.20), ("bird.bill_gape", SET, 0.03),
                    ("bird.bill_frac", SET, 0.16)),
    "spear bill": (("bird.bill_depth", SET, 0.20), ("bird.bill_gape", SET, 0.03),
                   ("bird.bill_frac", SET, 0.16)),
    "conical bill": (("bird.bill_depth", SET, 0.58), ("bird.bill_frac", SET, 0.06)),
    "seed bill": (("bird.bill_depth", SET, 0.58), ("bird.bill_frac", SET, 0.06)),
    "chisel bill": (("bird.bill_depth", SET, 0.26), ("bird.bill_frac", SET, 0.105),
                    ("bird.bill_gape", SET, 0.16)),
    "spatula bill": (("bird.bill_gape", SET, 0.95), ("bird.bill_depth", SET, 0.28)),
    "duck bill": (("bird.bill_gape", SET, 0.95), ("bird.bill_depth", SET, 0.28)),
    "decurved bill": (("bird.bill_curve", SET, 0.55),),
    "downcurved bill": (("bird.bill_curve", SET, 0.55),),
    "upturned bill": (("bird.bill_curve", SET, -0.45),),
    "parrot bill": (("bird.bill_depth", SET, 0.85), ("bird.bill_hook", SET, 0.80),
                    ("bird.bill_curve", SET, 0.30)),

    # Markings, by region.
    "capped": (("bird.head_mark", SET, "cap"),),
    "black cap": (("bird.head_mark", SET, "cap"),
                  ("materials.bird_head_mark", SET, "skin_dark")),
    "masked": (("bird.head_mark", SET, "mask"),),
    "eye stripe": (("bird.head_mark", SET, "mask"),),
    "eyebrow": (("bird.head_mark", SET, "supercilium"),),
    "supercilium": (("bird.head_mark", SET, "supercilium"),),
    "bibbed": (("bird.head_mark", SET, "throat"),),
    "coloured throat": (("bird.head_mark", SET, "throat"),),
    "collared": (("bird.head_mark", SET, "collar"),),
    "wing bar": (("bird.wing_mark", SET, "bar"),),
    "two wing bars": (("bird.wing_mark", SET, "doublebar"),),
    "wing panel": (("bird.wing_mark", SET, "panel"),),
    "speculum": (("bird.wing_mark", SET, "panel"),),
    "black wingtips": (("bird.wing_mark", SET, "tip"),
                       ("materials.bird_mark", SET, "skin_dark")),
    "barred": (("bird.body_mark", SET, "barred"),
               ("fish.pattern", SET, "bars")),
    "streaked": (("bird.body_mark", SET, "streaked"),),
    "speckled bird": (("bird.body_mark", SET, "speckled"),),
    "breast band": (("bird.body_mark", SET, "breastband"),),
    "plain bird": (("bird.head_mark", SET, "none"),
                   ("bird.wing_mark", SET, "none"),
                   ("bird.body_mark", SET, "none")),

    # Colour schemes. These are the stylised end on purpose; see
    # `docs/bird-colour-proposal.md` for why a naturalistic palette weighted by
    # area is browns and greys.
    "iridescent": (("materials.bird_back", SET, "plume_iridescent"),
                   ("materials.bird_head", SET, "plume_iridescent"),
                   ("materials.bird_wing", SET, "plume_iridescent")),
    "glossy black": (("materials.bird_back", SET, "plume_iridescent"),
                     ("materials.bird_belly", SET, "skin_dark"),
                     ("materials.bird_head", SET, "plume_iridescent")),
    "grey bird": (("materials.bird_back", SET, "plume_grey"),
                  ("materials.bird_wing", SET, "plume_grey")),
    "white bird": (("materials.bird_back", SET, "plume_white"),
                   ("materials.bird_belly", SET, "plume_white"),
                   ("materials.bird_head", SET, "plume_white"),
                   ("materials.bird_wing", SET, "plume_white")),
    "slate back": (("materials.bird_back", SET, "plume_slate"),),
    "sandy bird": (("materials.bird_back", SET, "plume_buff"),
                   ("materials.bird_belly", SET, "plume_buff"),
                   ("materials.bird_head", SET, "plume_buff")),
    "chestnut": (("materials.bird_back", SET, "plume_rufous"),
                 ("materials.bird_head", SET, "plume_rufous")),
    "turquoise bird": (("materials.bird_back", SET, "plume_cyan"),
                       ("materials.bird_head", SET, "plume_cyan"),
                       ("materials.bird_wing", SET, "plume_cyan")),
    "scarlet bird": (("materials.bird_back", SET, "plume_crimson"),
                     ("materials.bird_belly", SET, "plume_crimson"),
                     ("materials.bird_head", SET, "plume_crimson")),
    "green parrot": (("materials.bird_back", SET, "plume_lime"),
                     ("materials.bird_belly", SET, "plume_lime"),
                     ("materials.bird_head", SET, "plume_lime")),
    "yellow bill": (("materials.bird_bill", SET, "skin_yellow"),),
    "orange legs": (("materials.bird_bill", SET, "skin_orange"),),
    "dark bill": (("materials.bird_bill", SET, "skin_dark"),)
}


def _merge_switches(extra: dict) -> None:
    """Fold another switch table into `SWITCHES`.

    A phrase already present is a CONFLICT and is recorded rather than
    overwritten -- unless the new entry is a strict SUPERSET of the old one, in
    which case it replaces it. That exception is what "square tail" needs:
    three phrases mean the same thing to a fish and to a bird, and the bird
    entry sets both parameters, so replacing the fish-only entry with it widens
    the phrase rather than stealing it. A fish spec never reads `bird.*` and a
    bird spec never reads `fish.*`, so the half that does not apply is a no-op
    -- the same trick the species tables use to avoid a silent kind change.
    """
    for phrase, edits in extra.items():
        old = SWITCHES.get(phrase)
        if old is not None and old != edits:
            if not set(old).issubset(set(edits)):
                CONFLICTS.append(f"{phrase!r}: switch {old} vs {edits}")
                continue
        SWITCHES[phrase] = edits


_merge_switches(BIRD_SWITCHES)


# --- species keywords -------------------------------------------------------
#
# "Generate fish shapes based on particular species and keyword inputs" was the
# ask, and this is it: a fixed local table, no network and no model, in exactly
# the style of the rest of this file.
#
# EACH ENTRY IS A WHOLE FISH, not a nudge. Typing "pike" replaces the body
# proportions, the fins, the tail and the marking together, because that is
# what a species IS at this size and setting one without the others gives a
# trout wearing pike colours.
#
# The numbers are the published medians for each shape class, converted into
# this file's parameters; `docs/fish-shape-research.md` has the sources and the
# conversion. They are deliberately NOT the numbers in `specs/*.json`: a spec on
# disk is a tuned species and these are the draft you would start one from.
#
# WHY THIS IS A TABLE AND NOT A DATASET. FishBase carries a body-shape class for
# 36,125 species and is explicitly not licensed for commercial use, and
# FishShapes, the only source with body WIDTH, contradicts itself about its own
# licence. Neither belongs inside a game. Forty species typed out by hand carry
# no licence at all.
#
# THE COUNT IN THIS COMMENT IS LOAD-BEARING AND WAS WRONG. It said "twenty"
# while the table held twenty-eight, which is the sort of drift that makes a
# reader stop trusting the rest of the paragraph -- and the rest of the
# paragraph is a licence argument.


def _cetacean(**kw) -> tuple[tuple[str, str, object], ...]:
    """A whale or dolphin keyword: the four things that are not a fish.

    A HORIZONTAL fluke, a cross-section that runs from a barrel at the middle
    to a blade at the wrist, long narrow flippers, a blowhole, and no pelvic or
    anal fin. Everything else is `_fish` with different numbers, which is the
    same reason `cetacean` is a kind sharing one generator rather than a second
    generator.

    Like `_fish` it does NOT set `kind`, so typing "dolphin" in the Fish
    section gives a fish shaped like a dolphin rather than silently switching
    which generator the section is authoring for.
    """
    d = {
        "caudal_plane": "horizontal",
        "caudal_shape": "forked",
        "section": 2.5,
        "section_tail": 1.3,
        "belly": 0.50,
        "pelvic": 0.0,
        "anal_height": 0.0,
        "barbels": 0,
        "blowhole": 1.0,
        "fin_min_vox": 2.0,
        "pattern": "none",
    }
    d.update(kw)
    return _fish(**d)


def _fish(**kw) -> tuple[tuple[str, str, object], ...]:
    """A species keyword's recipe.

    `materials_back=` becomes `materials.fish_back`; everything else gets the
    `fish.` prefix. Python keywords cannot contain a dot, which is the whole
    reason for the translation.

    IT DOES NOT SET `kind`. Typing "trout" in the Trees section would then
    switch the spec to a fish while the panel went on showing trunk and crown
    sliders, and the gallery would fill with fish under a heading that said
    Trees. Left alone, these edits land on fish parameters, which a tree spec
    carries and never reads -- so the wrong section is a no-op instead of a
    silent kind change.
    """
    out = []
    for k, v in kw.items():
        path = ("materials.fish_" + k[len("materials_"):]
                if k.startswith("materials_") else f"fish.{k}")
        out.append((path, SET, v))

    # AND THE VOXEL SIZE, because on these species it is not an authoring
    # preference -- it is part of what the animal IS.
    #
    # Voxel size became per-species when the whales arrived: a big animal needs
    # MORE voxels of length than a small one, because the features that
    # identify it are a smaller fraction of its length (a reef fish's dorsal
    # fin is 25% of its body, a blue whale's is 1.2%). A keyword that set the
    # length and left the lattice alone therefore produced a 25 m whale on a
    # 1 cm grid: 2,500 voxels long, 1.25 BILLION cells, and a machine that
    # stops responding. Typing "blue whale" into the small-fish section did
    # exactly that.
    #
    # The thresholds are the same ones `tools/seed_marine.py` authored against,
    # and they are stated in one place -- here -- rather than repeated per
    # entry, so a keyword cannot disagree with the rule it came from.
    length = float(kw.get("length_m", 0.0))
    if length:
        cm = "10" if length >= 10.0 else "5" if length >= 3.0 else (
            "2" if length >= 0.5 else "1")
        out.append(("resolution_cm", SET, cm))
    return tuple(out)


SPECIES: dict[str, tuple[tuple[str, str, object], ...]] = {
    # fusiform, the shape most fish are: length:depth about 3.2, depth:width 1.9
    "trout": _fish(length_m=0.30, depth_ratio=0.25, width_ratio=0.52,
                   depth_at=0.41, fullness=3.0, peduncle=0.30, snout=0.34,
                   caudal_shape="forked", caudal_fork=0.20, caudal_span=1.05,
                   dorsal_shape="triangular", dorsal_start=0.36, dorsal_height=0.34,
                   adipose=True, pattern="spots", pattern_scale=0.05,
                   materials_back="skin_olive"),
    "salmon": _fish(length_m=0.60, depth_ratio=0.22, width_ratio=0.52,
                    depth_at=0.42, peduncle=0.22, caudal_shape="forked",
                    caudal_fork=0.28, adipose=True, pattern="spots",
                    pattern_scale=0.04),
    "char": _fish(length_m=0.30, depth_ratio=0.22, width_ratio=0.55,
                  adipose=True, pattern="spots", pattern_scale=0.05),
    "bass": _fish(length_m=0.35, depth_ratio=0.30, width_ratio=0.45,
                  depth_at=0.38, dorsal_shape="spiny", dorsal_height=0.45,
                  caudal_shape="forked", caudal_fork=0.18, pattern="stripe"),
    "perch": _fish(length_m=0.22, depth_ratio=0.32, width_ratio=0.42,
                   fullness=4.2, peduncle=0.26, section=1.7,
                   dorsal_shape="spiny", dorsal_start=0.30, dorsal_len=0.30,
                   dorsal_height=0.50, caudal_shape="forked", caudal_fork=0.18,
                   pattern="bars", pattern_count=5, pattern_width=0.40),
    # sagittiform: the mass carried back toward the tail
    "pike": _fish(length_m=0.75, depth_ratio=0.16, width_ratio=0.55,
                  depth_at=0.58, fullness=2.2, snout=0.42, peduncle=0.44,
                  dorsal_shape="triangular", dorsal_start=0.72, dorsal_height=0.75,
                  anal_height=0.62, caudal_shape="forked", caudal_fork=0.30,
                  pattern="saddle", pattern_strength=0.35),
    "barracuda": _fish(length_m=0.90, depth_ratio=0.11, width_ratio=0.70,
                       depth_at=0.45, snout=0.30, peduncle=0.30,
                       dorsal_start=0.66, dorsal_len=0.12,
                       caudal_shape="forked", caudal_fork=0.55, pattern="none"),
    # anguilliform: almost no bulge at all
    "eel": _fish(length_m=0.70, depth_ratio=0.085, width_ratio=0.90,
                 depth_at=0.30, fullness=1.6, snout=0.74, peduncle=0.52,
                 width_follow=0.60, section=2.6, head_frac=0.14,
                 caudal_shape="pointed", caudal_len=0.06,
                 dorsal_shape="ridge", dorsal_start=0.28, dorsal_len=0.70,
                 dorsal_height=0.55, anal_height=0.40, anal_len=0.45,
                 pelvic=0.0, pattern="none"),
    "loach": _fish(length_m=0.14, depth_ratio=0.13, width_ratio=1.0,
                   snout=0.60, dorsal_shape="ridge", caudal_shape="rounded",
                   barbels=4, barbel_len=0.14, pattern="mottle"),
    # depressiform: wider than it is deep
    "catfish": _fish(length_m=0.38, depth_ratio=0.20, width_ratio=1.25,
                     depth_at=0.30, snout=0.62, belly=0.40, section=3.0,
                     dorsal_shape="ridge", dorsal_len=0.44, dorsal_height=0.30,
                     caudal_shape="rounded", barbels=4, barbel_len=0.20,
                     pectoral=0.55, pattern="mottle", pattern_strength=0.40,
                     materials_back="skin_brown"),
    "flounder": _fish(length_m=0.30, depth_ratio=0.46, width_ratio=1.60,
                      belly=0.50, section=3.2, dorsal_shape="ridge",
                      dorsal_len=0.80, dorsal_height=0.22, anal_len=0.60,
                      caudal_shape="rounded", pattern="mottle",
                      pattern_strength=0.45),
    # compressiform: deep and thin
    "bream": _fish(length_m=0.35, depth_ratio=0.44, width_ratio=0.32,
                   fullness=4.8, peduncle=0.24, section=1.5,
                   dorsal_shape="sail", dorsal_height=0.40,
                   caudal_shape="forked", caudal_fork=0.40),
    "carp": _fish(length_m=0.40, depth_ratio=0.42, width_ratio=0.40,
                  depth_at=0.42, fullness=3.6, dorsal_shape="sail",
                  dorsal_len=0.44, dorsal_height=0.30, caudal_shape="forked",
                  caudal_fork=0.35, barbels=2, barbel_len=0.07,
                  pattern="mottle", pattern_strength=0.28,
                  materials_back="skin_orange", materials_flank="skin_orange"),
    "goldfish": _fish(length_m=0.16, depth_ratio=0.40, width_ratio=0.48,
                      caudal_shape="rounded", caudal_span=1.3, pattern="none",
                      materials_back="skin_orange", materials_flank="skin_orange",
                      materials_belly="skin_orange", materials_fin="skin_orange"),
    "angelfish": _fish(length_m=0.16, depth_ratio=0.62, width_ratio=0.24,
                       fullness=5.5, peduncle=0.22, section=1.3,
                       dorsal_shape="sail", dorsal_len=0.60, dorsal_height=0.7,
                       anal_height=0.7, anal_len=0.40, caudal_shape="truncate",
                       pattern="bars", pattern_count=4, pattern_width=0.30),
    "tang": _fish(length_m=0.18, depth_ratio=0.58, width_ratio=0.30,
                  fullness=5.5, peduncle=0.22, section=1.35,
                  dorsal_shape="sail", dorsal_len=0.58, dorsal_height=0.30,
                  anal_height=0.26, anal_len=0.34, caudal_shape="forked",
                  caudal_fork=0.45, caudal_span=0.62, pattern="stripe",
                  materials_back="skin_blue", materials_flank="skin_blue",
                  materials_belly="skin_blue", materials_fin="skin_yellow",
                  materials_pattern="skin_yellow"),
    "clownfish": _fish(length_m=0.18, depth_ratio=0.42, width_ratio=0.36,
                       fullness=4.5, peduncle=0.48, section=1.6,
                       dorsal_shape="sail", dorsal_len=0.52, dorsal_height=0.28,
                       caudal_shape="rounded", caudal_span=0.95,
                       back_frac=0.0, belly_frac=0.0, pattern="bars",
                       pattern_count=3, pattern_width=0.36,
                       materials_back="skin_orange", materials_flank="skin_orange",
                       materials_belly="skin_orange", materials_fin="skin_orange",
                       materials_pattern="skin_pale"),
    "anemonefish": _fish(length_m=0.18, depth_ratio=0.42, width_ratio=0.36,
                         caudal_shape="rounded", pattern="bars",
                         pattern_count=3, pattern_width=0.36,
                         back_frac=0.0, belly_frac=0.0,
                         materials_back="skin_orange", materials_flank="skin_orange",
                         materials_belly="skin_orange", materials_fin="skin_orange",
                         materials_pattern="skin_pale"),
    "wrasse": _fish(length_m=0.20, depth_ratio=0.26, width_ratio=0.42,
                    depth_at=0.36, dorsal_shape="ridge", dorsal_len=0.55,
                    dorsal_height=0.22, caudal_shape="truncate",
                    pattern="stripe", materials_back="skin_green",
                    materials_flank="skin_green", materials_fin="skin_blue"),
    # thunniform: the crescent-tailed cruiser
    "tuna": _fish(length_m=1.20, depth_ratio=0.26, width_ratio=0.62,
                  depth_at=0.42, peduncle=0.12, width_follow=1.8,
                  caudal_shape="forked", caudal_fork=0.72, caudal_span=1.7,
                  caudal_len=0.16, dorsal_shape="spiny", dorsal_height=0.42,
                  pattern="none", materials_back="skin_blue",
                  materials_flank="skin_silver"),
    "mackerel": _fish(length_m=0.35, depth_ratio=0.18, width_ratio=0.60,
                      peduncle=0.14, caudal_shape="forked", caudal_fork=0.65,
                      caudal_span=1.4, pattern="bars", pattern_count=5,
                      materials_back="skin_blue", materials_flank="skin_silver"),
    "herring": _fish(length_m=0.20, depth_ratio=0.22, width_ratio=0.44,
                     depth_at=0.40, peduncle=0.16, width_follow=1.45,
                     caudal_shape="forked", caudal_fork=0.60, caudal_span=1.35,
                     dorsal_len=0.16, dorsal_height=0.30, pattern="none",
                     materials_back="skin_blue", materials_flank="skin_silver"),
    "sardine": _fish(length_m=0.15, depth_ratio=0.20, width_ratio=0.46,
                     peduncle=0.18, caudal_shape="forked", caudal_fork=0.55,
                     pattern="none", materials_back="skin_blue",
                     materials_flank="skin_silver"),
    "minnow": _fish(length_m=0.20, depth_ratio=0.30, width_ratio=0.62,
                    depth_at=0.40, peduncle=0.34, section=2.3,
                    caudal_shape="forked", caudal_fork=0.30, pelvic=0.0,
                    pattern="none", materials_flank="skin_silver"),
    "roach": _fish(length_m=0.22, depth_ratio=0.33, width_ratio=0.46,
                   caudal_shape="forked", caudal_fork=0.32, pattern="none",
                   materials_flank="skin_silver", materials_fin="skin_red"),
    # globiform: essentially a ball with fins
    "pufferfish": _fish(length_m=0.20, depth_ratio=0.60, width_ratio=1.10,
                        depth_at=0.45, fullness=6.0, snout=0.55, peduncle=0.30,
                        section=3.2, dorsal_shape="ridge", dorsal_start=0.66,
                        dorsal_len=0.16, dorsal_height=0.22,
                        caudal_shape="rounded", caudal_span=0.7, pelvic=0.0,
                        pattern="spots", pattern_scale=0.08),
    "piranha": _fish(length_m=0.22, depth_ratio=0.45, width_ratio=0.36,
                     fullness=4.6, section=1.5, dorsal_shape="triangular",
                     dorsal_height=0.34, anal_height=0.45, anal_len=0.30,
                     caudal_shape="truncate", pattern="spots",
                     pattern_scale=0.05, materials_fin="skin_red"),
    "sturgeon": _fish(length_m=1.10, depth_ratio=0.14, width_ratio=0.85,
                      depth_at=0.30, snout=0.30, peduncle=0.30, belly=0.42,
                      barbels=4, barbel_len=0.10, dorsal_start=0.74,
                      caudal_shape="pointed", pattern="none",
                      materials_back="skin_dark", materials_flank="skin_brown"),

    # --- sharks -------------------------------------------------------------
    #
    # What makes a shark a shark here is `caudal_upper`: the upper lobe of the
    # tail reaches further aft than the lower. Measured as a ratio of the two
    # lobes it is about 3:1 in a requiem shark, 5:1 or more in a nurse shark,
    # and only 1.1:1 in a great white -- which is nearly symmetric and the
    # opposite of how it is usually drawn. Every one of these also carries a
    # second dorsal fin, which on most is a nub of about 2% of body length and
    # survives only because of the fin height floor.
    "shark": _fish(length_m=2.5, depth_ratio=0.16, width_ratio=0.66,
                   depth_at=0.34, fullness=2.6, snout=0.32, peduncle=0.20,
                   belly=0.48, width_follow=1.40, section=2.3, section_tail=1.5,
                   head_frac=0.24, caudal_shape="pointed", caudal_len=0.26,
                   caudal_span=1.25, caudal_upper=0.58,
                   dorsal_shape="triangular", dorsal_start=0.32, dorsal_len=0.13,
                   dorsal_height=0.60, dorsal2_height=0.14, dorsal2_start=0.68,
                   dorsal2_len=0.08, anal_height=0.12, anal_len=0.08,
                   pectoral=0.75, pectoral_aspect=0.60, pelvic=0.10,
                   fin_min_vox=2.0, pattern="none",
                   materials_back="skin_brown", materials_flank="skin_silver",
                   materials_belly="skin_pale"),
    "reef shark": _fish(length_m=1.6, depth_ratio=0.16, caudal_upper=0.58,
                        caudal_shape="pointed", dorsal2_height=0.14,
                        pattern="stripe", pattern_pos=0.36, pattern_width=0.10,
                        materials_fin="skin_dark", materials_pattern="skin_pale"),
    # A lamnid: nearly homocercal, and the countershading boundary is described
    # in the literature as ABRUPT, which is why the two fractions nearly meet.
    "great white": _fish(length_m=4.5, depth_ratio=0.175, width_ratio=0.62,
                         peduncle=0.16, width_follow=1.50, head_frac=0.29,
                         caudal_shape="forked", caudal_len=0.20,
                         caudal_span=1.45, caudal_fork=0.55, caudal_upper=0.10,
                         dorsal_height=0.62, dorsal_start=0.30,
                         dorsal2_height=0.10, dorsal2_start=0.72,
                         pectoral=0.95, pectoral_aspect=0.55,
                         back_frac=0.56, belly_frac=0.40, pattern="none",
                         section_tail=1.5, fin_min_vox=2.0,
                         materials_back="skin_dark", materials_flank="skin_silver",
                         materials_belly="skin_pale"),
    "white shark": _fish(length_m=4.5, depth_ratio=0.175, caudal_upper=0.10,
                         caudal_fork=0.55, dorsal2_height=0.10,
                         back_frac=0.56, belly_frac=0.40,
                         materials_back="skin_dark", materials_belly="skin_pale"),
    "tiger shark": _fish(length_m=4.0, depth_ratio=0.17, width_ratio=0.70,
                         snout=0.46, caudal_shape="pointed", caudal_len=0.28,
                         caudal_upper=0.62, dorsal2_height=0.16,
                         pattern="bars", pattern_count=7, pattern_width=0.30,
                         materials_back="skin_brown", materials_flank="skin_brown",
                         materials_pattern="skin_dark"),
    "whale shark": _fish(length_m=9.0, depth_ratio=0.20, width_ratio=0.95,
                         snout=0.72, section=2.8, caudal_upper=0.45,
                         caudal_span=1.40, caudal_fork=0.40,
                         dorsal2_height=0.14, pectoral=0.90,
                         pattern="spots", pattern_count=22, pattern_scale=0.02,
                         materials_back="skin_dark", materials_flank="skin_dark",
                         materials_pattern="skin_pale"),
    "nurse shark": _fish(length_m=2.4, depth_ratio=0.15, width_ratio=0.90,
                         snout=0.60, caudal_shape="pointed", caudal_len=0.30,
                         caudal_upper=0.85, dorsal_start=0.52,
                         dorsal2_height=0.16, dorsal2_start=0.70,
                         barbels=2, barbel_len=0.04, pattern="none",
                         materials_back="skin_brown", materials_flank="skin_brown"),
    "thresher": _fish(length_m=4.5, depth_ratio=0.17, caudal_shape="pointed",
                      caudal_len=0.55, caudal_span=1.10, caudal_upper=0.80,
                      dorsal2_height=0.06, pattern="none",
                      materials_back="skin_dark", materials_flank="skin_silver"),
    # The cephalofoil, at last. `head_width` is tip to tip as a fraction of
    # BODY length; the published figure is 25-32% of TOTAL length, and a
    # hammerhead's tail is about a third of its body, so 0.38 here measures out
    # at 30% of the built animal. Until this parameter existed the keyword gave
    # a shark with hammerhead proportions and an ordinary head, because the
    # body loft derives width from depth and a hammerhead's head is its
    # shallowest part and its widest.
    "hammerhead": _fish(length_m=3.5, depth_ratio=0.13, width_ratio=0.62,
                        head_frac=0.24, head_width=0.38, snout=0.20,
                        caudal_shape="pointed", caudal_len=0.31,
                        caudal_upper=0.62, dorsal_height=1.00, dorsal_start=0.28,
                        dorsal2_height=0.16, pattern="none",
                        materials_back="skin_brown", materials_flank="skin_silver"),
    "bonnethead": _fish(length_m=1.0, depth_ratio=0.14, width_ratio=0.62,
                        head_frac=0.24, head_width=0.26, snout=0.24,
                        caudal_shape="pointed", caudal_len=0.28,
                        caudal_upper=0.60, dorsal_height=0.90, dorsal_start=0.28,
                        dorsal2_height=0.14, pattern="none",
                        materials_back="skin_brown", materials_flank="skin_silver"),

    # --- whales and dolphins -------------------------------------------------
    "dolphin": _cetacean(length_m=3.0, depth_ratio=0.195, width_ratio=0.95,
                         depth_at=0.40, fullness=2.6, snout=0.30, peduncle=0.28,
                         width_follow=1.35, head_frac=0.20, caudal_len=0.13,
                         caudal_span=1.25, caudal_fork=0.30,
                         dorsal_shape="triangular", dorsal_start=0.42,
                         dorsal_len=0.16, dorsal_height=0.55,
                         pectoral=0.55, pectoral_aspect=0.55,
                         back_frac=0.46, belly_frac=0.30,
                         materials_back="skin_dark", materials_flank="skin_silver",
                         materials_belly="skin_pale", materials_fin="skin_dark"),
    "porpoise": _cetacean(length_m=1.5, depth_ratio=0.21, width_ratio=0.95,
                          caudal_span=1.20, dorsal_shape="triangular",
                          dorsal_height=0.38, dorsal_start=0.44,
                          pectoral=0.40, pectoral_aspect=0.60,
                          back_frac=0.50, belly_frac=0.34,
                          materials_back="skin_dark", materials_belly="skin_pale"),
    # A male: the dorsal fin is 22-30% of body length where a female's is
    # 13-18% and a dolphin's is 11%.
    "orca": _cetacean(length_m=7.0, depth_ratio=0.21, width_ratio=0.95,
                      depth_at=0.38, fullness=2.4, snout=0.40, peduncle=0.26,
                      width_follow=1.40, head_frac=0.20, caudal_len=0.12,
                      caudal_span=1.20, caudal_fork=0.32,
                      dorsal_shape="sail", dorsal_start=0.38, dorsal_len=0.14,
                      dorsal_height=1.30, pectoral=0.60, pectoral_aspect=0.75,
                      eye_patch=2.0, back_frac=0.62, belly_frac=0.30,
                      pattern="saddle", pattern_scale=0.10, pattern_strength=0.10,
                      materials_back="skin_dark", materials_flank="skin_dark",
                      materials_belly="skin_pale", materials_fin="skin_dark",
                      materials_pattern="skin_silver", materials_patch="skin_pale"),
    "killer whale": _cetacean(length_m=7.0, depth_ratio=0.21, dorsal_shape="sail",
                              dorsal_height=1.30, eye_patch=2.0,
                              back_frac=0.62, belly_frac=0.30,
                              materials_back="skin_dark", materials_flank="skin_dark",
                              materials_belly="skin_pale",
                              materials_patch="skin_pale"),
    "beluga": _cetacean(length_m=4.5, depth_ratio=0.20, width_ratio=0.98,
                        snout=0.52, peduncle=0.30, section=2.8,
                        dorsal_shape="ridge", dorsal_start=0.50, dorsal_len=0.34,
                        dorsal_height=0.06, pectoral=0.45, pectoral_aspect=0.85,
                        back_frac=0.0, belly_frac=0.0,
                        materials_back="skin_pale", materials_flank="skin_pale",
                        materials_belly="skin_pale", materials_fin="skin_pale"),
    # A generic rorqual: dorsal fin far back and small, flippers modest.
    "whale": _cetacean(length_m=14.0, depth_ratio=0.20, width_ratio=0.95,
                       depth_at=0.36, fullness=2.2, snout=0.42, peduncle=0.22,
                       width_follow=1.45, head_frac=0.26, caudal_len=0.10,
                       caudal_span=1.35, caudal_fork=0.28,
                       dorsal_shape="triangular", dorsal_start=0.70,
                       dorsal_len=0.10, dorsal_height=0.14,
                       pectoral=0.70, pectoral_aspect=0.30,
                       back_frac=0.60, belly_frac=0.26,
                       materials_back="skin_dark", materials_flank="skin_dark",
                       materials_belly="skin_pale"),
    # Flippers 30.8% of body length, statistically longer than its size
    # predicts, and the most recognisable limb in the sea.
    "humpback": _cetacean(length_m=14.0, depth_ratio=0.24, width_ratio=0.90,
                          depth_at=0.34, snout=0.52, peduncle=0.24,
                          caudal_span=1.45, dorsal_start=0.62, dorsal_len=0.12,
                          dorsal_height=0.16, pectoral=1.20, pectoral_aspect=0.24,
                          back_frac=0.70, belly_frac=0.26,
                          materials_back="skin_dark", materials_belly="skin_pale",
                          materials_fin="skin_pale"),
    "blue whale": _cetacean(length_m=25.0, depth_ratio=0.157, width_ratio=1.00,
                            depth_at=0.36, fullness=2.0, snout=0.34,
                            peduncle=0.20, width_follow=1.50, caudal_len=0.08,
                            caudal_span=1.35, dorsal_start=0.74, dorsal_len=0.07,
                            dorsal_height=0.09, pectoral=0.60,
                            pectoral_aspect=0.30, back_frac=0.55, belly_frac=0.24,
                            pattern="mottle", pattern_scale=0.02,
                            pattern_strength=0.30,
                            materials_back="skin_blue", materials_flank="skin_silver",
                            materials_belly="skin_pale", materials_pattern="skin_pale"),
    # A quarter to a third of it is a square head, and it has no dorsal FIN --
    # a hump and a row of knuckles on the caudal third.
    "sperm whale": _cetacean(length_m=16.0, depth_ratio=0.20, width_ratio=0.92,
                             depth_at=0.22, fullness=2.6, snout=0.90,
                             peduncle=0.26, head_frac=0.30, section=2.8,
                             section_tail=1.4, caudal_len=0.10, caudal_span=1.30,
                             dorsal_shape="ridge", dorsal_start=0.62,
                             dorsal_len=0.26, dorsal_height=0.10,
                             pectoral=0.35, pectoral_aspect=0.70,
                             back_frac=0.60, belly_frac=0.18,
                             pattern="mottle", pattern_scale=0.04,
                             pattern_strength=0.18,
                             materials_back="skin_brown", materials_flank="skin_brown",
                             materials_belly="skin_brown",
                             materials_pattern="skin_pale"),
    "minke": _cetacean(length_m=8.5, depth_ratio=0.17, width_ratio=0.95,
                       peduncle=0.22, caudal_span=1.40, dorsal_start=0.66,
                       dorsal_len=0.10, dorsal_height=0.24,
                       pectoral=0.45, pectoral_aspect=0.35,
                       back_frac=0.58, belly_frac=0.28,
                       materials_back="skin_dark", materials_belly="skin_pale"),
}

SWITCHES.update(SPECIES)


# --- bird species keywords --------------------------------------------------
#
# The same idea as the fish table above and the same rule: EACH ENTRY IS A
# WHOLE BIRD, not a nudge. Typing "heron" replaces the five length shares, the
# posture, the bill, the tail and the legs together, because that is what a
# species IS at this size -- a heron with a songbird's neck is a grey songbird.
#
# The numbers are published medians converted into this file's parameters:
# family medians from AVONET for the ratios (tail/wing, tarsus/wing, beak/wing,
# hand-wing index), Alerstam's 129-species biometry for aspect ratio, and
# Cornell species accounts for total length. `docs/bird-shape-research.md` has
# the sources and the conversion. They are deliberately NOT the numbers in
# `specs/*.json`: a spec on disk is a tuned species and these are the draft you
# would start one from.
#
# IT DOES NOT SET `kind`, for exactly the reason `_fish` does not, and it does
# not set colours either. A species keyword here is a SHAPE. Colour is a
# separate vocabulary ("iridescent", "turquoise bird", "sandy bird") so that
# "raven" and "glossy black" compose instead of one overwriting the other --
# and so that a designer who wants a stylised species does not have to fight
# the keyword that got them the shape.


def _bird(**kw) -> tuple[tuple[str, str, object], ...]:
    """A bird species keyword's recipe. Everything gets the `bird.` prefix."""
    return tuple((f"bird.{k}", SET, v) for k, v in kw.items())


BIRD_SPECIES: dict[str, tuple[tuple[str, str, object], ...]] = {
    # --- corvids: elliptical wings, long tails, heavy bills ------------------
    "raven": _bird(length_m=0.64, bill_frac=0.115, head_frac=0.135,
                   neck_frac=0.05, body_frac=0.36, tail_frac=0.34,
                   posture_deg=22, body_depth=0.70, tail_shape="wedge",
                   bill_depth=0.42, bill_hook=0.22, wing_shape="elliptical",
                   wing_aspect=5.9, wing_span=2.05, head_size=1.05),
    "crow": _bird(length_m=0.47, bill_frac=0.100, head_frac=0.135,
                  body_frac=0.37, tail_frac=0.35, posture_deg=24,
                  tail_shape="square", bill_depth=0.38, bill_hook=0.15,
                  wing_shape="elliptical", wing_aspect=5.9),
    "magpie": _bird(length_m=0.50, bill_frac=0.070, head_frac=0.100,
                    body_frac=0.28, tail_frac=0.52, posture_deg=26,
                    tail_shape="graduated", tail_width=0.24, bill_depth=0.36,
                    wing_shape="elliptical", wing_aspect=4.8),
    "jay": _bird(length_m=0.34, bill_frac=0.085, head_frac=0.145,
                 body_frac=0.35, tail_frac=0.38, posture_deg=32, crest=0.32,
                 tail_shape="rounded", bill_depth=0.36,
                 wing_shape="elliptical", wing_aspect=4.5, wing_span=1.75),
    # --- songbirds: upright, short-necked, square or notched tails -----------
    "robin": _bird(length_m=0.24, bill_frac=0.070, head_frac=0.165,
                   neck_frac=0.035, body_frac=0.38, tail_frac=0.32,
                   posture_deg=42, body_depth=0.92, head_size=1.05,
                   bill_depth=0.24, tail_shape="square", leg_len=0.13,
                   wing_shape="elliptical", wing_aspect=5.4,
                   head_mark="throat"),
    "sparrow": _bird(length_m=0.22, bill_frac=0.060, head_frac=0.160,
                     body_frac=0.38, tail_frac=0.33, posture_deg=34,
                     bill_depth=0.52, tail_shape="notched", tail_fork=0.22,
                     wing_shape="elliptical", wing_aspect=5.2),
    "finch": _bird(length_m=0.22, bill_frac=0.055, head_frac=0.165,
                   body_frac=0.375, tail_frac=0.34, posture_deg=34,
                   bill_depth=0.58, tail_shape="notched", tail_fork=0.30,
                   wing_shape="elliptical", wing_aspect=5.5,
                   wing_mark="bar"),
    "tit": _bird(length_m=0.24, bill_frac=0.065, head_frac=0.170,
                 neck_frac=0.030, body_frac=0.375, tail_frac=0.33,
                 posture_deg=36, head_size=1.10, bill_depth=0.30,
                 tail_shape="notched", wing_shape="elliptical",
                 wing_aspect=5.2, head_mark="mask", wing_mark="bar"),
    "wren": _bird(length_m=0.20, bill_frac=0.080, head_frac=0.175,
                  body_frac=0.44, tail_frac=0.24, posture_deg=46,
                  body_depth=1.05, tail_droop=-0.35, tail_shape="square",
                  bill_depth=0.20, wing_shape="elliptical", wing_aspect=4.6,
                  wing_span=1.35, wing_fold=0.20),
    "thrush": _bird(length_m=0.23, bill_frac=0.075, head_frac=0.150,
                    body_frac=0.375, tail_frac=0.335, posture_deg=34,
                    bill_depth=0.26, tail_shape="square",
                    wing_shape="elliptical", wing_aspect=5.6,
                    body_mark="speckled", mark_strength=0.32),
    "starling": _bird(length_m=0.21, bill_frac=0.090, head_frac=0.140,
                      body_frac=0.42, tail_frac=0.22, posture_deg=28,
                      bill_depth=0.22, tail_shape="square",
                      wing_shape="pointed", wing_aspect=6.6, wing_sweep=0.40,
                      wing_fold=0.85, body_mark="speckled",
                      mark_strength=0.26),
    "lark": _bird(length_m=0.22, bill_frac=0.070, head_frac=0.150,
                  body_frac=0.40, tail_frac=0.32, posture_deg=22, crest=0.40,
                  bill_depth=0.34, tail_shape="square",
                  wing_shape="elliptical", wing_aspect=6.0,
                  body_mark="streaked", mark_count=5),
    "swallow": _bird(length_m=0.26, bill_frac=0.045, head_frac=0.135,
                     neck_frac=0.025, body_frac=0.375, tail_frac=0.42,
                     posture_deg=8, body_depth=0.62, bill_depth=0.16,
                     bill_gape=0.60, tail_shape="forked", tail_fork=0.52,
                     tail_width=0.62, pose="flying", wing_shape="pointed",
                     wing_aspect=7.5, wing_sweep=0.62, wing_span=1.95,
                     leg_len=0.04, head_mark="throat"),
    "swift": _bird(length_m=0.24, bill_frac=0.035, head_frac=0.130,
                   neck_frac=0.020, body_frac=0.42, tail_frac=0.395,
                   posture_deg=4, body_depth=0.56, bill_depth=0.14,
                   bill_gape=0.70, tail_shape="forked", tail_fork=0.30,
                   pose="flying", wing_shape="pointed", wing_aspect=9.5,
                   wing_sweep=0.85, wing_span=2.30, leg_len=0.02,
                   wing_fold=1.20),
    # --- raptors and owls ---------------------------------------------------
    "eagle": _bird(length_m=0.85, bill_frac=0.055, head_frac=0.115,
                   neck_frac=0.055, body_frac=0.395, tail_frac=0.38,
                   posture_deg=4, body_depth=0.66, bill_depth=0.52,
                   bill_hook=0.85, tail_shape="rounded", tail_thick=3,
                   pose="flying", wing_shape="slotted", wing_slots=6,
                   wing_aspect=6.9, wing_span=2.45, wing_thick=3),
    "buzzard": _bird(length_m=0.52, bill_frac=0.050, head_frac=0.115,
                     body_frac=0.40, tail_frac=0.39, posture_deg=4,
                     bill_depth=0.48, bill_hook=0.72, tail_shape="rounded",
                     tail_width=0.62, pose="flying", wing_shape="slotted",
                     wing_slots=4, wing_aspect=5.6, wing_span=2.35,
                     wing_thick=2),
    "hawk": _bird(length_m=0.38, bill_frac=0.048, head_frac=0.115,
                  body_frac=0.39, tail_frac=0.42, posture_deg=6,
                  bill_depth=0.48, bill_hook=0.75, tail_shape="square",
                  pose="flying", wing_shape="elliptical", wing_aspect=6.2,
                  wing_span=1.90),
    "falcon": _bird(length_m=0.42, bill_frac=0.045, head_frac=0.115,
                    body_frac=0.39, tail_frac=0.41, posture_deg=8,
                    bill_depth=0.55, bill_hook=0.65, tail_shape="rounded",
                    pose="flying", wing_shape="pointed", wing_aspect=7.9,
                    wing_sweep=0.45, wing_span=2.25),
    "kestrel": _bird(length_m=0.34, bill_frac=0.045, head_frac=0.115,
                     body_frac=0.38, tail_frac=0.42, posture_deg=8,
                     bill_depth=0.55, bill_hook=0.60, tail_shape="rounded",
                     pose="flying", wing_shape="pointed", wing_aspect=7.5,
                     wing_sweep=0.30, wing_span=2.20),
    "vulture": _bird(length_m=1.00, bill_frac=0.055, head_frac=0.105,
                     neck_frac=0.090, body_frac=0.43, tail_frac=0.32,
                     posture_deg=4, bill_depth=0.42, bill_hook=0.70,
                     tail_shape="square", pose="flying", wing_shape="slotted",
                     wing_slots=6, wing_aspect=6.4, wing_span=2.40,
                     wing_dihedral=0.35, wing_thick=3),
    "owl": _bird(length_m=0.40, bill_frac=0.045, head_frac=0.185,
                 neck_frac=0.010, body_frac=0.475, tail_frac=0.285,
                 posture_deg=46, head_size=1.42, neck_thick=1.10,
                 body_depth=0.74, bill_depth=0.55, bill_hook=0.70,
                 tail_shape="rounded", eye=2.0, leg_len=0.08,
                 wing_shape="elliptical", wing_aspect=5.3, wing_span=2.45,
                 head_mark="mask"),
    # --- waterside and waterfowl -------------------------------------------
    "heron": _bird(length_m=1.00, bill_frac=0.150, head_frac=0.075,
                   neck_frac=0.260, body_frac=0.375, tail_frac=0.140,
                   posture_deg=6, neck_up_deg=64, neck_thick=0.26,
                   head_size=0.85, body_depth=0.70, bill_depth=0.20,
                   bill_gape=0.03, tail_shape="square", leg_len=0.30,
                   leg_thick=1.5, wing_shape="slotted", wing_slots=4,
                   wing_aspect=7.2, wing_span=1.85),
    "egret": _bird(length_m=0.90, bill_frac=0.145, head_frac=0.075,
                   neck_frac=0.265, body_frac=0.375, tail_frac=0.140,
                   posture_deg=6, neck_up_deg=66, neck_thick=0.24,
                   bill_depth=0.18, bill_gape=0.03, tail_shape="square",
                   leg_len=0.31, wing_shape="slotted", wing_aspect=7.2),
    "stork": _bird(length_m=1.10, bill_frac=0.170, head_frac=0.075,
                   neck_frac=0.220, body_frac=0.395, tail_frac=0.140,
                   posture_deg=4, neck_up_deg=50, neck_thick=0.30,
                   bill_depth=0.24, bill_gape=0.10, tail_shape="square",
                   leg_len=0.32, wing_shape="slotted", wing_slots=5,
                   wing_aspect=7.0, wing_span=2.20),
    "crane": _bird(length_m=1.20, bill_frac=0.100, head_frac=0.075,
                   neck_frac=0.280, body_frac=0.400, tail_frac=0.145,
                   posture_deg=4, neck_up_deg=62, neck_thick=0.26,
                   bill_depth=0.24, tail_shape="square", leg_len=0.34,
                   wing_shape="slotted", wing_aspect=7.5),
    "kingfisher": _bird(length_m=0.20, bill_frac=0.200, head_frac=0.150,
                        neck_frac=0.020, body_frac=0.415, tail_frac=0.115,
                        posture_deg=26, head_size=1.35, neck_thick=0.90,
                        bill_depth=0.24, bill_gape=0.05, tail_shape="square",
                        leg_len=0.055, wing_shape="elliptical",
                        wing_aspect=5.0, head_mark="mask"),
    "duck": _bird(length_m=0.58, bill_frac=0.115, head_frac=0.110,
                  neck_frac=0.100, body_frac=0.475, tail_frac=0.200,
                  posture_deg=4, body_width=0.92, section=2.6,
                  bill_depth=0.28, bill_gape=0.95, tail_shape="pointed",
                  leg_len=0.075, wing_shape="pointed", wing_aspect=9.2,
                  wing_span=1.55, head_mark="collar", wing_mark="panel"),
    "mallard": _bird(length_m=0.58, bill_frac=0.115, head_frac=0.110,
                     neck_frac=0.100, body_frac=0.475, tail_frac=0.200,
                     posture_deg=4, body_width=0.92, section=2.6,
                     bill_depth=0.28, bill_gape=0.95, tail_shape="pointed",
                     leg_len=0.075, wing_shape="pointed", wing_aspect=9.2,
                     head_mark="collar", wing_mark="panel"),
    "goose": _bird(length_m=0.85, bill_frac=0.090, head_frac=0.095,
                   neck_frac=0.190, body_frac=0.445, tail_frac=0.180,
                   posture_deg=6, neck_up_deg=48, neck_thick=0.42,
                   bill_depth=0.34, bill_gape=0.70, tail_shape="square",
                   leg_len=0.10, wing_shape="pointed", wing_aspect=8.7,
                   wing_span=1.90),
    "swan": _bird(length_m=1.45, bill_frac=0.070, head_frac=0.070,
                  neck_frac=0.330, body_frac=0.390, tail_frac=0.140,
                  posture_deg=4, neck_up_deg=60, neck_thick=0.32,
                  bill_depth=0.30, bill_gape=0.70, tail_shape="square",
                  leg_len=0.08, wing_shape="slotted", wing_aspect=8.7,
                  wing_span=1.70),
    "gull": _bird(length_m=0.60, bill_frac=0.075, head_frac=0.105,
                  neck_frac=0.070, body_frac=0.42, tail_frac=0.33,
                  posture_deg=4, body_width=0.78, bill_depth=0.30,
                  bill_hook=0.28, tail_shape="square", leg_len=0.14,
                  pose="flying", wing_shape="soaring", wing_aspect=9.7,
                  wing_span=2.35, wing_thick=2, wing_mark="tip"),
    "tern": _bird(length_m=0.36, bill_frac=0.105, head_frac=0.100,
                  body_frac=0.40, tail_frac=0.395, posture_deg=4,
                  bill_depth=0.22, tail_shape="forked", tail_fork=0.55,
                  leg_len=0.06, pose="flying", wing_shape="pointed",
                  wing_aspect=11.2, wing_sweep=0.35, wing_span=2.38,
                  head_mark="cap"),
    "albatross": _bird(length_m=1.15, bill_frac=0.110, head_frac=0.095,
                       neck_frac=0.090, body_frac=0.510, tail_frac=0.195,
                       posture_deg=4, bill_depth=0.30, bill_hook=0.35,
                       tail_shape="square", pose="flying",
                       wing_shape="soaring", wing_aspect=15.0, wing_span=2.70,
                       wing_thick=3),
    # --- open ground, rock and forest floor ---------------------------------
    "pigeon": _bird(length_m=0.33, bill_frac=0.055, head_frac=0.115,
                    neck_frac=0.055, body_frac=0.43, tail_frac=0.345,
                    posture_deg=16, head_size=0.85, body_depth=0.80,
                    section=2.3, bill_depth=0.30, tail_shape="square",
                    leg_len=0.095, wing_shape="pointed", wing_aspect=8.6,
                    wing_sweep=0.30, wing_span=1.85, wing_mark="doublebar"),
    "dove": _bird(length_m=0.30, bill_frac=0.050, head_frac=0.110,
                  body_frac=0.40, tail_frac=0.385, posture_deg=18,
                  head_size=0.82, bill_depth=0.26, tail_shape="graduated",
                  wing_shape="pointed", wing_aspect=8.6),
    "pheasant": _bird(length_m=0.75, bill_frac=0.040, head_frac=0.095,
                      neck_frac=0.075, body_frac=0.34, tail_frac=0.450,
                      posture_deg=14, body_depth=0.90, body_width=0.84,
                      section=2.5, bill_depth=0.42, bill_curve=0.20,
                      tail_shape="pointed", tail_width=0.16, leg_len=0.10,
                      wing_shape="elliptical", wing_aspect=4.6,
                      wing_span=1.30),
    "grouse": _bird(length_m=0.40, bill_frac=0.040, head_frac=0.110,
                    neck_frac=0.045, body_frac=0.53, tail_frac=0.275,
                    posture_deg=14, body_depth=0.88, body_width=0.86,
                    section=2.5, bill_depth=0.44, bill_curve=0.20,
                    tail_shape="square", leg_len=0.075,
                    wing_shape="elliptical", wing_aspect=4.6, wing_span=1.35),
    "ptarmigan": _bird(length_m=0.36, bill_frac=0.045, head_frac=0.115,
                       neck_frac=0.045, body_frac=0.52, tail_frac=0.275,
                       posture_deg=14, body_depth=0.86, body_width=0.84,
                       section=2.5, bill_depth=0.42, bill_curve=0.20,
                       tail_shape="square", leg_len=0.075,
                       wing_shape="elliptical", wing_aspect=4.6,
                       head_mark="supercilium"),
    "woodpecker": _bird(length_m=0.23, bill_frac=0.105, head_frac=0.130,
                        neck_frac=0.030, body_frac=0.42, tail_frac=0.315,
                        posture_deg=68, neck_up_deg=-6, neck_thick=0.70,
                        bill_depth=0.26, bill_gape=0.16, tail_shape="pointed",
                        tail_droop=0.80, tail_thick=2, leg_len=0.09,
                        wing_shape="elliptical", wing_aspect=5.2,
                        head_mark="cap", wing_mark="panel"),
    "hoopoe": _bird(length_m=0.27, bill_frac=0.160, head_frac=0.115,
                    neck_frac=0.045, body_frac=0.38, tail_frac=0.32,
                    posture_deg=20, crest=0.90, bill_depth=0.16,
                    bill_curve=0.55, bill_gape=0.05, tail_shape="square",
                    wing_shape="elliptical", wing_aspect=5.0,
                    wing_mark="doublebar"),
    "curlew": _bird(length_m=0.55, bill_frac=0.235, head_frac=0.085,
                    neck_frac=0.115, body_frac=0.36, tail_frac=0.205,
                    posture_deg=8, bill_depth=0.07, bill_curve=0.85,
                    bill_gape=0.04, tail_shape="square", leg_len=0.22,
                    wing_shape="pointed", wing_aspect=8.3),
    "avocet": _bird(length_m=0.44, bill_frac=0.180, head_frac=0.085,
                    neck_frac=0.135, body_frac=0.38, tail_frac=0.220,
                    posture_deg=6, bill_depth=0.06, bill_curve=-0.45,
                    bill_gape=0.04, tail_shape="square", leg_len=0.26,
                    wing_shape="pointed", wing_aspect=8.3),
    # --- tropical -----------------------------------------------------------
    "parrot": _bird(length_m=0.35, bill_frac=0.075, head_frac=0.135,
                    neck_frac=0.030, body_frac=0.40, tail_frac=0.360,
                    posture_deg=34, head_size=1.15, bill_depth=0.85,
                    bill_hook=0.80, bill_curve=0.30, tail_shape="graduated",
                    tail_width=0.30, leg_len=0.08, wing_shape="pointed",
                    wing_aspect=6.5, wing_span=1.45, head_mark="mask"),
    "macaw": _bird(length_m=0.85, bill_frac=0.070, head_frac=0.110,
                   neck_frac=0.030, body_frac=0.245, tail_frac=0.545,
                   posture_deg=30, head_size=1.15, bill_depth=0.85,
                   bill_hook=0.80, bill_curve=0.30, tail_shape="graduated",
                   tail_width=0.20, tail_thick=2, leg_len=0.075,
                   wing_shape="pointed", wing_aspect=6.5, wing_span=1.30,
                   head_mark="mask", wing_mark="panel"),
    "toucan": _bird(length_m=0.55, bill_frac=0.290, head_frac=0.100,
                    neck_frac=0.030, body_frac=0.33, tail_frac=0.250,
                    posture_deg=30, bill_depth=0.60, bill_curve=0.30,
                    bill_gape=0.35, tail_shape="square", leg_len=0.09,
                    wing_shape="elliptical", wing_aspect=4.8),
    "hummingbird": _bird(length_m=0.20, bill_frac=0.230, head_frac=0.140,
                         neck_frac=0.020, body_frac=0.36, tail_frac=0.250,
                         posture_deg=24, bill_depth=0.09, bill_gape=0.03,
                         tail_shape="notched", leg_len=0.03, pose="flying",
                         wing_shape="pointed", wing_aspect=7.5,
                         wing_sweep=0.45, wing_span=1.20),
}

_merge_switches(BIRD_SPECIES)

INTENSITY: dict[str, float] = {
    "slightly": 0.5, "a bit": 0.5, "a little": 0.5, "somewhat": 0.5, "marginally": 0.5,
    "a touch": 0.4, "barely": 0.35, "just": 0.5,
    "much": 2.0, "far": 2.0, "way": 2.0, "a lot": 2.0, "very": 1.8, "really": 1.8,
    "considerably": 1.8, "significantly": 1.8, "substantially": 1.8,
    "extremely": 3.0, "massively": 3.0, "hugely": 3.0, "dramatically": 2.6,
}

NEGATORS = {"less", "fewer", "not", "no", "reduce", "decrease", "lower", "drop"}
BOOSTERS = {"more", "increase", "raise", "add", "boost"}

STOPWORDS = {
    "a", "an", "and", "the", "it", "its", "is", "be", "make", "makes", "made",
    "please", "with", "but", "to", "of", "for", "that", "this", "i", "want",
    "should", "would", "like", "look", "looks", "looking", "bit", "little",
    "lot", "much", "very", "really", "so", "then", "them", "some", "little",
    "tree", "trees", "one", "give", "put", "keep", "on", "in", "at", "as",
    "up", "down", "out", "can", "you", "let", "s", "t",
    "more", "less", "fewer", "not", "reduce", "increase", "raise",
    "canopy", "crown", "trunk", "branch", "branches", "branching", "foliage",
    "leaf", "leaves", "height", "radius", "shape", "top", "base", "overall",
    "instead", "than", "bit", "little", "way", "far", "quite",
    *{w for phrase in INTENSITY for w in phrase.split()},
}

NUMBER_RULES = (
    (re.compile(r"(\d+(?:\.\d+)?)\s*(?:m|metre|metres|meter|meters)\s+(?:tall|high|height)"),
     "height_m"),
    (re.compile(r"\bheight\s*(?:of|=|:)?\s*(\d+(?:\.\d+)?)"), "height_m"),
    (re.compile(r"\bcrown\s+radius\s*(?:of|=|:)?\s*(\d+(?:\.\d+)?)"), "crown.radius_m"),
    (re.compile(r"\btrunk\s+radius\s*(?:of|=|:)?\s*(\d+(?:\.\d+)?)"), "trunk.radius_base_m"),
    (re.compile(r"\b(\d+(?:\.\d+)?)\s*(?:m|metre|metres|meter|meters)\s+(?:wide|across)"),
     "_diameter"),
)


def _all_phrases() -> tuple[tuple[str, str, object], ...]:
    """Every recognised phrase, longest first, so specific beats general."""
    merged: list[tuple[str, str, object]] = []
    merged += [(p, "switch", v) for p, v in SWITCHES.items()]
    merged += [(p, "shape", v) for p, v in SHAPES.items()]
    merged += [(p, "concept", v) for p, v in PHRASES.items()]
    return tuple(sorted(merged, key=lambda row: len(row[0]), reverse=True))


_ALL_PHRASES = _all_phrases()


def vocabulary() -> dict:
    """What the box understands, for the UI to show."""
    groups: dict[str, list[str]] = {}
    for phrase, (key, direction) in sorted(PHRASES.items()):
        c = CONCEPTS[key]
        groups.setdefault(c.summary, []).append(phrase)
    return {
        "concepts": [{"summary": k, "phrases": v} for k, v in sorted(groups.items())],
        "shapes": sorted(set(SHAPES)),
        "switches": sorted(SWITCHES),
        "intensity": sorted(INTENSITY),
    }


def interpret(spec: dict, text: str) -> dict:
    """Map a sentence onto parameter changes. Never raises; reports what it missed."""
    raw = text.strip()
    # Commas survive as their own token: they bound a clause, so "much shorter,
    # sparser" applies "much" to the height only. Dropping them made one
    # intensity word amplify every phrase that followed it.
    cleaned = re.sub(r"[,;]+", " , ", raw.lower())
    cleaned = re.sub(r"[^a-z0-9.,\- ]+", " ", cleaned)
    low = " " + re.sub(r"\s+", " ", cleaned).strip() + " "

    changes: dict[str, object] = {}
    notes: list[str] = []
    consumed: list[tuple[int, int]] = []

    # 1. Explicit numbers win outright — "height 8" is not a nudge.
    for pattern, path in NUMBER_RULES:
        for m in pattern.finditer(low):
            value = float(m.group(1))
            if path == "_diameter":
                changes["crown.radius_m"] = value / 2.0
                notes.append(f"crown radius set to {value / 2:g} m (half of {value:g} m across)")
            else:
                changes[path] = value
                notes.append(f"{specmod.BY_PATH[path].label} set to {value:g}")
            consumed.append(m.span())

    # 2. Every phrase — switches, silhouettes and graded concepts — matched in
    #    ONE longest-first pass.
    #
    #    Two things this gets right that separate passes did not. A matched span
    #    covers the phrase only, not the spaces around it: including the
    #    delimiters made consecutive words share a character, so in "gnarled
    #    sparser" the second word looked already-consumed and was dropped. And
    #    a single merged ordering means "bare trunk" beats the shorter switch
    #    "bare", which a per-table pass got backwards.
    strengths: dict[str, float] = {}
    hits: list[tuple[tuple[int, int], tuple[str, float]]] = []
    for phrase, kind, payload in _ALL_PHRASES:
        span = _find(low, phrase, consumed)
        if span is None:
            continue
        consumed.append(span)
        if kind == "shape":
            changes["crown.shape"] = payload
            notes.append(f"crown shape set to {payload}")
        elif kind == "switch":
            for path, _k, value in payload:
                changes[path] = value
            notes.append(f"applied '{phrase}'")
        else:
            hits.append((span, payload))

    # Modifiers are read from the gap between a phrase and whatever was matched
    # before it, so "much" cannot reach past a comma or past another phrase.
    for span, (key, direction) in hits:
        before = _modifier_window(low, span, consumed)
        direction *= -1.0 if _negated(before) else 1.0
        strengths[key] = strengths.get(key, 0.0) + direction * _intensity(before)

    for key, strength in strengths.items():
        concept = CONCEPTS[key]
        for path, kind, amount in concept.edits:
            current = changes.get(path, specmod.get(spec, path))
            changes[path] = _apply(kind, current, amount, strength)
        notes.append(f"{concept.summary}: {_direction_word(strength)}")

    patched, report = specmod.patch(spec, changes)

    # 4. Say plainly what went unused, so an unknown word is visible rather
    #    than silently ignored. Words are matched by POSITION against the
    #    consumed spans, not by searching for the word again -- a word that
    #    appears twice would otherwise report wrongly.
    ignored = sorted({
        m.group(0)
        for m in re.finditer(r"[a-z][a-z\-]{2,}", low)
        if m.group(0) not in STOPWORDS and not _overlaps(m.start(), m.end(), consumed)
    })

    edits = []
    for path in changes:
        param = specmod.BY_PATH.get(path)
        if not param:
            continue
        old, new = specmod.get(spec, path), specmod.get(patched, path)
        if old != new:
            edits.append({"path": path, "label": param.label, "from": old, "to": new})

    return {
        "spec": patched,
        "edits": edits,
        "understood": notes,
        "ignored": ignored,
        "warnings": report.warnings,
    }


# --- helpers ----------------------------------------------------------------


def _find(text: str, phrase: str, consumed) -> tuple[int, int] | None:
    """Locate a whole-word phrase that has not already been claimed."""
    start = 0
    while True:
        pos = text.find(f" {phrase} ", start)
        if pos < 0:
            return None
        span = (pos + 1, pos + 1 + len(phrase))
        if not _overlaps(*span, consumed):
            return span
        start = pos + 1


def _overlaps(lo: int, hi: int, spans) -> bool:
    return any(lo < b and a < hi for a, b in spans)


def _modifier_window(text: str, span, consumed) -> str:
    """The words that can modify this phrase: since the previous match, and
    not across a comma. Capped at three words -- a modifier further away than
    that belongs to something else."""
    prev_end = 0
    for a, b in consumed:
        if b <= span[0]:
            prev_end = max(prev_end, b)
    gap = text[prev_end:span[0]]
    if "," in gap:
        gap = gap.rsplit(",", 1)[1]
    return " " + " ".join(gap.split()[-3:]) + " "


def _negated(before: str) -> bool:
    return any(w in NEGATORS for w in before.split())


def _intensity(before: str) -> float:
    for phrase, value in INTENSITY.items():
        if f" {phrase} " in before:
            return value
    return 1.0


def _apply(kind: str, current, amount, strength: float):
    if kind == SET:
        return amount
    if kind == ADD:
        return float(current) + amount * strength
    factor = 1.0 + amount * strength
    # Never let a multiplier invert or collapse a dimension.
    return float(current) * max(factor, 0.15)


def _direction_word(strength: float) -> str:
    if strength >= 1.8:
        return "much more"
    if strength > 0:
        return "more" if strength >= 0.9 else "slightly more"
    if strength <= -1.8:
        return "much less"
    return "less" if strength <= -0.9 else "slightly less"
