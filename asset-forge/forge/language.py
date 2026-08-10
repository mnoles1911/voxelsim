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
                 ("crown.shell", ADD, 0.08))),
        Concept("clump", "size of each foliage clump",
                (("foliage.clump_radius_m", MUL, 0.32),)),
        Concept("airy", "open, layered crown rather than a solid mass",
                (("crown.shell", ADD, 0.22), ("foliage.coverage", ADD, -0.08))),
        Concept("variation", "how much individuals differ from each other",
                (("variation.amount", MUL, 0.45),)),
        Concept("roots", "flared roots at the base", (("trunk.buttress", ADD, 0.32),)),
        Concept("crownheight", "how tall the crown is",
                (("crown.height_frac", ADD, 0.13),)),
        Concept("crownup", "how high the crown sits",
                (("crown.center_frac", ADD, 0.08),)),
        Concept("squash", "crown squashed or stretched vertically",
                (("crown.squash", MUL, 0.25),)),
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
}

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
