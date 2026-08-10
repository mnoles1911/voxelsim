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
PARAMS: tuple[Param, ...] = (
    P("name", "Species name", "unnamed", kind="text", group="general"),
    P("notes", "Notes", "", kind="text", group="general",
      help="Free text for the designer; ignored by the generator."),
    P("kind", "Asset kind", "tree", kind="choice", group="general",
      choices=kindlib.KEYS,
      help="What sort of asset this is. It decides which parameters apply and "
           "which generator runs — a rock has no trunk, crown or foliage."),
    P("height_m", "Height (m)", 12.0, 0.3, 40.0, 0.1, group="general",
      help="Ground to the top of the crown. The floor is low enough for ground "
           "cover; rocks ignore this and use their own size."),
    P("resolution_cm", "Voxel size", "10", kind="choice", group="general",
      choices=("10", "5", "2.5", "2", "1"),
      help="Edge length of one voxel. The engine's terrain is 10 cm; finer makes "
           "real twigs instead of one-voxel minimums, but cost is cubic — 2 cm is "
           "125x the voxels of 10 cm. Previews always render coarse; this is the "
           "size the asset exports at."),

    P("trunk.radius_base_m", "Trunk radius at base (m)", 0.30, 0.05, 1.60, 0.01, group="trunk"),
    P("trunk.clear_frac", "Branch-free height", 0.35, 0.0, 0.90, 0.01, group="trunk",
      help="Fraction of total height with bare trunk. High for jungle emergents, low for bushes."),
    P("trunk.lean_deg", "Lean", 3.0, 0.0, 40.0, 0.5, group="trunk"),
    P("trunk.lean_dir_deg", "Lean direction", 0.0, 0.0, 360.0, 1.0, group="trunk"),
    P("trunk.wander", "Wander", 0.15, 0.0, 1.0, 0.01, group="trunk",
      help="How much the trunk wobbles on its way up. Gnarled desert wood is high."),
    P("trunk.buttress", "Root flare", 0.0, 0.0, 1.0, 0.01, group="trunk",
      help="Thickening at the very base. Kapok and jungle trees want this."),

    P("crown.shape", "Crown shape", "sphere", kind="choice", group="crown",
      choices=("sphere", "cone", "umbrella", "column", "vase", "wedge", "hanging")),
    P("crown.radius_m", "Crown radius (m)", 3.5, 0.3, 16.0, 0.1, group="crown"),
    P("crown.height_frac", "Crown height", 0.65, 0.10, 1.0, 0.01, group="crown",
      help="Crown's vertical extent as a fraction of tree height."),
    P("crown.center_frac", "Crown centre", 0.66, 0.20, 0.98, 0.01, group="crown"),
    P("crown.shell", "Hollowness", 0.30, 0.0, 1.0, 0.01, group="crown",
      help="Pushes growth targets toward the crown's outer surface. Conifers and "
           "umbrella crowns read better high; dense broadleaves lower."),
    P("crown.squash", "Vertical squash", 1.0, 0.30, 2.5, 0.01, group="crown"),
    P("crown.lean_deg", "Crown lean", 0.0, 0.0, 45.0, 0.5, group="crown",
      help="Wind-shear. Leans the whole crown off the trunk axis."),
    P("crown.lean_dir_deg", "Crown lean direction", 0.0, 0.0, 360.0, 1.0, group="crown"),
    P("crown.points", "Growth targets", 900, 80, 5000, 10, kind="int", group="crown",
      help="More targets means denser, finer branching and a slower generate."),

    P("growth.model", "Growth model", "colonize", kind="choice", group="growth",
      choices=("colonize", "whorl", "frond"),
      help="How branches are placed. 'colonize' grows toward scattered targets and "
           "suits irregular broadleaf crowns. 'whorl' builds rings of branches up a "
           "straight leader, which is what a conifer actually is — a spruce's tiers "
           "are a real structure, not an irregular crown that happens to look tiered."),
    P("growth.step_m", "Segment length (m)", 0.35, 0.08, 1.50, 0.01, group="growth"),
    P("growth.influence_m", "Reach (m)", 3.0, 0.4, 14.0, 0.1, group="growth",
      help="How far a branch tip can see a growth target."),
    P("growth.kill_m", "Target consumption (m)", 0.70, 0.10, 4.0, 0.05, group="growth",
      help="Low values give long thin twigs, high values give stubby branching."),
    P("growth.gravity", "Droop", -0.12, -1.0, 1.0, 0.01, group="growth",
      help="Negative droops branches down (willow), positive lifts them up."),
    P("growth.phototropism", "Reach for light", 0.10, 0.0, 1.0, 0.01, group="growth"),
    P("growth.inertia", "Straightness", 0.45, 0.0, 1.0, 0.01, group="growth"),
    P("growth.jitter", "Wobble", 0.05, 0.0, 0.6, 0.01, group="growth"),
    P("growth.max_iter", "Growth iterations", 260, 20, 900, 10, kind="int", group="growth"),
    P("growth.tip_radius_m", "Twig radius (m)", 0.045, 0.01, 0.40, 0.005, group="growth"),
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
    P("foliage.clump_radius_m", "Clump radius (m)", 0.65, 0.15, 3.0, 0.05, group="foliage"),
    P("foliage.density", "Clump density", 0.60, 0.05, 1.0, 0.01, group="foliage"),
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
    P("foliage.droop_m", "Clump droop (m)", 0.15, -1.0, 2.0, 0.05, group="foliage"),
    P("foliage.squash", "Clump squash", 0.80, 0.30, 2.5, 0.01, group="foliage"),

    # How much individuals of this species differ from each other. Without
    # this, every seed produces the same tree with the twigs shuffled: the
    # growth randomness varies but the height, spread and trunk do not, so a
    # bank of 64 seeds gives a forest no variety at all.
    P("variation.amount", "Variation", 1.0, 0.0, 3.0, 0.05, group="variation",
      help="Master scale on everything below. Zero makes every seed the same size and "
           "shape, varying only in how the branches happen to grow."),
    P("variation.height", "Height spread", 0.18, 0.0, 0.6, 0.01, group="variation"),
    P("variation.crown_radius", "Crown spread", 0.18, 0.0, 0.6, 0.01, group="variation"),
    P("variation.trunk_radius", "Trunk spread", 0.18, 0.0, 0.6, 0.01, group="variation"),
    P("variation.shape", "Shape spread", 0.12, 0.0, 0.5, 0.01, group="variation",
      help="Varies crown squash and how high the crown sits."),
    P("variation.lean_deg", "Lean spread", 7.0, 0.0, 30.0, 0.5, group="variation"),
    P("variation.droop", "Droop spread", 0.25, 0.0, 1.0, 0.01, group="variation"),
    P("variation.density", "Density spread", 0.12, 0.0, 0.6, 0.01, group="variation"),
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
    P("placement.spacing_m", "Minimum spacing (m)", 6.0, 0.5, 60.0, 0.5, group="placement",
      help="Closest two individuals may stand. Roughly the canopy diameter for a "
           "closed forest, much larger for savanna or desert."),
    P("placement.cluster", "Grows in stands", 0.3, 0.0, 1.0, 0.01, group="placement",
      help="0 scatters individuals evenly; 1 gathers them into groves with open "
           "ground between."),
    P("placement.elev_min_m", "Lowest elevation (m)", 0.0, -10.0, 4000.0, 10.0,
      group="placement", help="Metres above sea level. The engine's sea level is z=0."),
    P("placement.elev_max_m", "Highest elevation (m)", 2000.0, 0.0, 5000.0, 10.0,
      group="placement",
      help="Above the treeline nothing but tundra/alpine is classified anyway "
           "(900 m at 0 C, rising ~150 m per degree), so this mainly separates "
           "lowland species from montane ones inside a biome."),
    P("placement.slope_max_pct", "Steepest ground (% grade)", 45.0, 0.0, 70.0, 1.0,
      group="placement",
      help="Ground steeper than this will not carry the species. Stated as a grade "
           "in percent, the same currency the engine's cliff gate uses — above 70% "
           "(~35 degrees) the ground classifies as bare rock and carries nothing."),
    P("placement.water_max_m", "Distance to water (m)", 0.0, 0.0, 500.0, 5.0,
      group="placement",
      help="0 means it does not care. Above 0, the species only appears within this "
           "distance of a watercourse — riverbank willows and jungle understorey."),

    P("rock.size_m", "Size (m)", 1.6, 0.2, 12.0, 0.1, group="rock",
      help="Longest dimension of the boulder."),
    P("rock.lumps", "Lumps", 5, 1, 16, 1, kind="int", group="rock",
      help="How many overlapping masses the rock is built from. One gives a clean "
           "ovoid; more gives a knobbly, weathered boulder."),
    P("rock.spread", "Lump spread", 0.42, 0.0, 1.0, 0.01, group="rock",
      help="How far the lumps scatter from the centre. High values make a broken "
           "pile rather than a single stone."),
    P("rock.flatten", "Flatten", 0.72, 0.15, 2.0, 0.01, group="rock",
      help="Vertical squash. Below 1 gives a slab; above 1 a standing stone."),
    P("rock.elongate", "Elongate", 1.25, 0.4, 3.0, 0.01, group="rock"),
    P("rock.angular", "Angularity", 0.45, 0.0, 1.0, 0.01, group="rock",
      help="Slices flat faces off the mass. 0 is a rounded river cobble, 1 a "
           "freshly fractured block."),
    P("rock.facets", "Facet count", 4, 0, 10, 1, kind="int", group="rock"),
    P("rock.erode", "Erosion", 0.35, 0.0, 1.0, 0.01, group="rock",
      help="Removes weakly-attached voxels, rounding sharp protrusions and "
           "pitting the surface."),
    P("rock.bury", "Buried fraction", 0.22, 0.0, 0.7, 0.01, group="rock",
      help="How much of the stone sits below ground. Everything under z=0 is cut "
           "away, so this controls how settled it looks rather than adding volume."),
    P("rock.rubble", "Rubble", 0.15, 0.0, 1.0, 0.01, group="rock",
      help="Loose stones scattered around the base."),
    P("materials.rock", "Rock", "rock", kind="choice", group="rock",
      choices=("rock", "bedrock", "gravel", "sand", "clay", "permafrost", "snow")),

    P("materials.bark", "Bark", "bark", kind="choice", group="materials",
      choices=materials.WOOD_NAMES),
    P("materials.core", "Heartwood", "heartwood", kind="choice", group="materials",
      choices=materials.WOOD_NAMES),
    P("materials.leaf", "Leaf", "leaf_broadleaf", kind="choice", group="materials",
      choices=materials.LEAF_NAMES),
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
            rep.warnings.append(
                f"{p.path}: {val!r} not one of {p.choices}, using {p.default!r}"
            )
            continue
        set_(out, p.path, val)

    # Cross-parameter checks. These are the combinations that produce a tree
    # that generates fine and looks wrong, so they warn rather than clamp.
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
    body = {k: v for k, v in spec.items() if k != "notes"}
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
        return float(rng.random()) * 2.0 - 1.0

    def spread(path: str) -> float:
        return amount * float(get(spec, path))

    changes: dict[str, Any] = {
        "height_m": get(spec, "height_m") * (1.0 + spread("variation.height") * u()),
        "crown.radius_m": get(spec, "crown.radius_m")
        * (1.0 + spread("variation.crown_radius") * u()),
        "trunk.radius_base_m": get(spec, "trunk.radius_base_m")
        * (1.0 + spread("variation.trunk_radius") * u()),
        "crown.squash": get(spec, "crown.squash") * (1.0 + spread("variation.shape") * u()),
        "crown.center_frac": get(spec, "crown.center_frac")
        * (1.0 + spread("variation.shape") * 0.5 * u()),
        "trunk.lean_deg": max(0.0, get(spec, "trunk.lean_deg") + spread("variation.lean_deg") * u()),
        "growth.gravity": get(spec, "growth.gravity")
        + spread("variation.droop") * 0.30 * u(),
        "foliage.density": get(spec, "foliage.density")
        * (1.0 + spread("variation.density") * u()),
    }
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
