# Rock placement: self-classifying by size, mass and biome

*2026-08-18. The owner's requirement, verbatim: "advanced placement logic for
the allocation of rocks according to their size/volume/weight and respective
biome type... extensible such that as I build and generate more and more rock
assets of different types, they neatly fall into the large placement logic and
system."*

## The one rule that makes it extensible

**A rock's placement class is derived from its measured bake, not from
hand-authoring.** Generate a new rock species, bake it, export — and the
system has already measured it and priced it. Nothing to remember, nothing to
tune, no list anywhere that a new species must be added to.

The measurement happens in `tools/export_manifest.py` (the rock classifier
block), at the one moment the baked bytes and the manifest meet:

| measured | how | drives |
|---|---|---|
| volume (m³) | solid voxels × pitch³ of seed 1's bank | rarity (below) |
| mass (kg) | volume × ~2600 kg/m³ | transport class (bake-28 channels) |
| biome | authored weights, untouched | where it may appear at all |

## Rarity: the size-frequency law

Real rock populations follow a power law — N(>V) ∝ V^−1.8..−2 (see
`docs/placement-research.md`; the same family as the Damuth scaling the
wildlife densities use). The classifier expresses it as a spacing floor:

    spacing = max(authored, 6.0 × volume^0.35 metres)

Calibration: a 1 m³ boulder prices 6 m spacing; a 100 m³ tor ~30 m; cobbles
sit below their authored spacing and keep it. **Authored wins upward only** —
a designer may make a species rarer than physics, never more common than its
size allows. First run over the library repriced 24 of 102 rock species.

## Transport class: how a rock got where it is

Nature places rocks by how they were moved. The classes and their gates:

* **Talus / cliff-fall** (heavy, angular): lives on steep ground —
  `placement.slope_min_pct` (the scree band; schema row added the same day,
  wired to the engine's `slopeMinMmPerM` gate which had waited unexported
  since the format was defined). Seven species author bands today
  (alpine-scree ≥35%, cliff-fall-block ≥40%, fractured-outcrop ≥30%, …).
  These classes also take a cluster floor of 0.7 — fragmentation debris
  clusters into fans, and the library's authored median (0.35) read as
  scattered gravel.
* **Fluvial** (light, rounded): sorted along water — `placement.water_max_m`,
  served by the bake-28 distance-to-water plane. River-cobble and
  step-pool-boulder are the authored exemplars.
* **Glacial / residual** (any mass, gentle ground): erratics and moraine
  ridges place on low slope with no water tie; the default class when
  nothing else claims a species.

The bake-28 **talus channel** (debris flux from cliff-threshold slopes,
deposited below the ~38° angle of repose) is the next consumer of the mass
measurement: heavy classes will read the channel as a density gain below
cliffs, which is what turns "rocks allowed on slopes" into "rocks
accumulated where a cliff sheds them."

## What stays authored, deliberately

Biome weights (a desert rock authors desert), elevation bands, and any
explicit spacing/cluster override. The classifier fills gaps and enforces
physics floors; it never overrides a human's stated intent in the
restrictive direction, and the curation gate (`forge/spec.py`,
`docs/…354339b`) decides whether a species ships at all.

## Adding a new rock kind tomorrow

1. Author the spec (biomes, elevation; leave spacing/cluster default).
2. Bake seeds; approve the good ones in the viewer (curation block).
3. `python tools/export_manifest.py` — the classifier measures the bank,
   prices rarity, applies transport-class floors, and prints what it did.
4. Done. The species places at physical rarity in its authored biomes,
   banded by slope or water per its class.
