# Placement spec schema — per-class density, biome allowlists, named rules

Date: 2026-08-18, worldgen v28, manifest format VXM2. This is the contract
the Asset Forge UI is built against. It describes every placement-relevant
field, its default, its JSON shape on disk, how the pieces compose, and what
the engine does with them. The authoritative implementations are
`asset-forge/forge/{spec,biomes,manifest}.py` (write side) and
`voxel-core/include/voxelcore/assetmanifest.h` + `src/assetmanifest.cpp` +
`assetpolicy.h` (read side); if this document and the code ever disagree, the
code is right and this document has a bug.

The owner's directive, which this schema implements verbatim:

> each asset class (trees vs bushes vs rocks vs grass vs flowers) should have
> their own density values per biome. All assets should have specs that allow
> for them to be allowlisted in one, multiple, or no biomes. Next, the asset's
> placement spec should allow for it to have different placement rules
> depending on the biome (ex: temperate type tree is almost unrestricted in
> temperate forest but faces strict placement rules in deserts such as must be
> near fresh water body). … rule overrides … will be custom authored and
> defined prior.

Three data sources feed one binary manifest (`out/engine/species.vxm`):

| what | where | scope |
|---|---|---|
| Per-species placement | `asset-forge/specs/<name>.json` | one species |
| Named rule library | `asset-forge/rules/placement-rules.json` | shared |
| Kind × biome density | `asset-forge/rules/biome-density.json` | shared |

**Determinism contract (read first).** The manifest bytes are worldgen input.
Any change to any of the three sources changes `species.vxm`, and once a
world has generated against a manifest that is a `vxc::kWorldGenVersion` bump
with goldens re-blessed. The UI should surface this: saving a placement edit
is a *world* edit, not a cosmetic one.

**Imported assets.** A species whose voxels came from an outside 3D source
(imported and saved into the library) authors exactly the same placement
data: nothing below depends on generator-only fields. The full placement
contract for any species is: `kind`, `height_m` (or `rock.size_m` for rocks),
`resolution_cm`, the `biomes.*` weights, the `placement.*` group, and the two
optional blocks — all of them plain JSON a UI can write without ever running
the generator.

---

## 1. Per-species placement (specs/<name>.json)

### 1.1 The biome weights — `biomes.*` (existing, unchanged)

One float per biome, `0.0..1.0`, step 0.05, default 0.0. "How common this
species is in that biome relative to the other species there." Zero means
absent. Only biomes that HOST the species' kind are authorable (the hosting
table lives in `forge/biomes.py`, e.g. ocean hosts no trees; the UI gets it
from `forge.biomes.for_kind(kind)`).

```json
"biomes": {"savanna": 1.0, "grassland": 0.3, "desert": 0.2, ...zeroes... }
```

### 1.2 The default placement rules — `placement.*` (existing, unchanged)

These are the species' rules EVERYWHERE it is allowed, unless a per-biome
attachment tightens them (section 3). All floats.

| field | default | range | meaning |
|---|---|---|---|
| `placement.abundance` | 0.5 | 0..1 | overall frequency where it occurs |
| `placement.spacing_m` | 6.0 | 0.1..3000 | closest two individuals may stand |
| `placement.cluster` | 0.3 | 0..1 | 0 = even scatter, 1 = groves |
| `placement.elev_min_m` | 0.0 | −200..4000 | lowest elevation (negative = depth) |
| `placement.elev_max_m` | 2000.0 | 0..5000 | highest elevation |
| `placement.slope_min_pct` | 0.0 | 0..70 | ground flatter than this refuses (scree band) |
| `placement.slope_max_pct` | 45.0 | 0..70 | ground steeper than this refuses |
| `placement.water_max_m` | 0.0 | 0..500 | 0 = does not care; >0 = only within this distance of water |
| `detail.water` | "any" | menu | water KIND: any / ocean / river / lake / shallow / reef |

**Author the default as the species' most permissive biome** and attach
restrictions where a biome is harsher — composition can only tighten
(section 3.3), never loosen. That is the veto-only contract surfacing in the
authoring model.

### 1.3 The biome allowlist — `biome_allow` (NEW, optional)

A top-level array of biome keys. Explicit allowlist semantics: the species
may appear in exactly these biomes, whatever the weights say.

```json
"biome_allow": ["savanna", "desert"]
```

* **Absent** (every existing spec): the allowlist is DERIVED — exactly the
  biomes with weight > 0. This is what the library always meant by its
  zeroes, so migration is a no-op.
* **Present**: authoritative. A weight > 0 outside the list is ZEROED at
  export and reported by name (`outside_allowlist` in the export report);
  `validate` warns at save time too.
* **`[]`** is legal and means allowed NOWHERE (a species parked out of the
  world without deleting its weights).
* Entries are clipped to biomes that host the species' kind; unknown or
  non-hosting entries are dropped with a warning.
* An illegible block (not a list) resolves to `[]` — closed, loud — because
  someone tried to restrict and nobody can tell to what.

The one resolver is `forge.biomes.allowed(spec)`; `tools/library.py --biome
desert` answers "which species are allowed in desert" from it, and the plain
`tools/library.py` summary counts explicit vs derived.

UI shape (owner): checkboxes/dropdown over the hosting biomes; writing the
block only when the user makes an explicit choice keeps the derived default.

### 1.4 Per-biome rule attachments — `biome_rules` (NEW, optional)

A top-level map: biome key → list of NAMED rule references (section 2). One
or many rules per biome.

```json
"biome_rules": {
  "desert":  ["near-fresh-water-60m", "sparse-outlier"],
  "savanna": ["sparse-outlier"]
}
```

* Rule names are validated for shape at save (`1-31` chars of
  `[A-Za-z0-9_-]`); the EXPORT refuses — with species, biome and rule named —
  on a reference to a rule that is not in the library. A typo must never
  silently drop a restriction.
* Attachments to a biome outside the species' allowlist, or where its weight
  is zero, are DEAD: reported by name at export, not written to the wire.
* An illegible block refuses the species at export (publishing it
  un-restricted would be worse than not publishing it).
* Attachments do not exist for detail entities (fish/bird/quadruped/
  cetacean) — the scatter gates never run for them; the reader refuses such
  a file.

`tools/library.py --rule <name>` answers "which assets use this rule".

### 1.5 Hashing: placement metadata does not re-bake banks

`biome_allow` and `biome_rules` are EXCLUDED from `spec_hash` and
`seed_hash` (like `curation`): tuning where a species stands must not re-bake
its banks or redraw individuals. The check that catches a stale manifest is
`tools/enginecheck.py`'s byte comparison, not the spec hash.

---

## 2. The named rule library (rules/placement-rules.json)

Rules are authored ONCE, "defined prior", then attached by reference — the
UI presents them as a dropdown, and a rule edit retunes every species that
attaches it.

```json
{
  "rules": {
    "near-fresh-water-60m": {"water_max_m": 60.0, "water": "lake"},
    "sparse-outlier":       {"abundance": 0.1, "spacing_m": 40.0},
    "scree-band":           {"slope_min_pct": 30.0}
  }
}
```

A rule is a mapping of ANY subset (at least one) of:

| field | range / menu | composition when several rules bind |
|---|---|---|
| `elev_min_m` | −200..4000 | max wins (band intersection) |
| `elev_max_m` | 0..5000 | min wins |
| `slope_min_pct` | 0..70 | max wins |
| `slope_max_pct` | 0..70 | min wins |
| `water_max_m` | >0..500 | min wins (closest requirement) |
| `water` | ocean/river/lake/shallow/reef/any | kinds must AGREE; salinity masks AND |
| `spacing_m` | >0..3000 | max wins (sparsest) |
| `abundance` | 0..1 | min wins (rarest) |
| `cluster` | 0..1 | setters must AGREE |

Keys starting with `_` are notes. Values clamp to the same ranges as the
placement sliders. `"water": "lake"` is "fresh water" (`"river"` admits
fresh + brackish) — the owner's "must be near fresh water body" is
`{"water_max_m": ..., "water": "lake"}`.

**Renaming/deleting a referenced rule breaks the export by name (on
purpose). Editing a rule's numbers is a worldgen change.**

---

## 3. Composition semantics (exact)

For each (species, biome) with attachments, the effective gates are the
INTERSECTION of the species' `placement.*` defaults and every attached rule
— strictest wins, per the table above, with the base value participating for
the ordered fields (so a rule can only TIGHTEN the default, never loosen it).
`cluster` and `water` have no strict order: the rule value replaces the base,
and two rules that disagree on them are a CONTRADICTION. So are an empty
composed band (`elev_min > elev_max`, `slope_min > slope_max`) and an empty
composed salinity mask. Contradictions are refused at export with the rule
names in the message; the engine re-checks the same law on the wire
(`kBadAttachment`) so a file that dodged the exporter cannot half-load.

Base `water_max_m = 0` means "unset" — it does not participate in the min.

**What happens at import (engine):** the species SPLITS. Its base gate row
loses its weight in every ruled biome; one variant row per ruled biome
carries that biome's weight with the composed gates (same bank, same layer).
A column has exactly one biome, so exactly one row of the species is ever
eligible at a site — pick arithmetic is unchanged, and the resolver's gates
stay biome-blind scalar compares. Consequences worth knowing:

* Per-biome `spacing_m` acts through the pick-weight fold
  (`(cell/spacing)^2`), NOT by re-filing the species on another lattice —
  the layer is chosen once, from the DEFAULT spacing.
* Per-biome `abundance` folds into that biome's pick weight the same way.
* Per-biome `water` (salinity) is carried on the wire and composed, but for
  scatter kinds the served water-distance plane is salinity-blind today —
  the KIND gate becomes live the day the distance plane splits fresh/salt,
  exactly as `water_max_m` was carried before it was servable.
* The slope response curve and moisture affinity are re-derived per variant
  from the composed values, so a strict desert slope ceiling gets a strict
  taper with it.

---

## 4. The kind × biome density table (rules/biome-density.json)

```json
{
  "density": {
    "tree":  {"savanna": 0.15, "temperate_forest": 1.0, ...},
    "grass": {"savanna": 1.0, "desert": 0.15, ...},
    ...
  }
}
```

One value per (asset class, biome), `0.0..1.0`, default 1.0 for anything
missing. Kinds: `tree, bush, rock, grass, reed, flower` (the six scatter
classes; entries for detail-entity kinds are carried but unused). This is
"each asset class has its own density values per biome".

**Semantics:** the value scales the site OCCUPANCY test, after the species
pick: `P(site used) = min(Σ weights, 1000)/1000 × density[kind][biome]`.
Applying it post-pick makes it LINEAR — a 0.15 savanna tree density yields
15% of the trees whatever the weights sum to, which the weights themselves
cannot express once they saturate the occupancy cap (the measured
savanna == forest defect, audit §4.2). It composes MULTIPLICATIVELY with the
per-layer `densityPerMille` (the global knob in `forge/manifest.py LAYERS`):
layer density thins sites before anything runs; this table thins per kind
per biome after the pick. Multiply, not replace, because the two answer
different questions — "how dense is this lattice at all" vs "how much of
that budget does this class get here".

* Values above 1.0 are refused on both sides (write and parse): the table
  may only THIN. Making a class more common somewhere is a weight/abundance
  edit, not a density edit.
* `0.0` is legal: the class is absent from that biome regardless of weights.
* Side effect, intended: wherever the table drops the keep probability below
  certainty, the cluster field regains headroom, so thinned classes start
  forming groves in biomes that used to be saturated (audit §4.3's dead zone
  shrinks exactly where the table bites).

---

## 5. The wire format (VXM2), for completeness

Little-endian, packed. Reader refuses whole-file on any violation, with a
named reason (`assetManifestErrorText`).

```
header 32 B: magic "VXM1" | version=2 | biome_count=10 | layer_count=4
             | species_count | record_bytes=152 | rule_count | attach_count
biome names: 10 × 16 B ASCII (verified by spelling AND position)
layers:      4 × 24 B (unchanged from VXM1)
density:     10 kinds × 10 biomes × u16 per-mille, kind-major
             (KIND_ORDER × BIOME_ORDER); any value > 1000 refused
rules:       rule_count × 64 B, sorted by name:
             name char[32] | field_mask u16 | abundance_q10 u16
             | cluster_q10 u16 | water_kind u8 | water_mask u8
             | elev_min_mm i32 | elev_max_mm i32 | slope_min i32
             | slope_max i32 (pct×10) | water_max_mm i32 | spacing_mm i32
             (unmasked fields MUST be zero — one encoding per meaning)
species:     species_count × 152 B (byte-identical to VXM1 records)
attachments: attach_count × 8 B, strictly ascending (species, biome, rule):
             species u16 | biome u8 | pad u8 | rule u16 | pad u16
```

Field mask bits (`AssetOverrideField`): elev_min 1, elev_max 2, slope_min 4,
slope_max 8, water_max 16, water_kind 32, spacing 64, abundance 128,
cluster 256.

The allowlist has no wire representation of its own: it is enforced at
encode by zeroing weights, so the wire's weights ARE the effective biome
set.

---

## 6. What the UI needs to build (summary for the UI agent)

1. **Biome assignment** (existing weights, per species): sliders/checkboxes
   over `forge.biomes.for_kind(kind)`.
2. **Allowlist** (per species): checkbox set writing `biome_allow`; show
   "derived from weights" until the user makes it explicit; support the
   empty list ("no biomes").
3. **Rule attachment** (per species, per biome): dropdown of names from
   `rules/placement-rules.json`, multi-select per biome, writing
   `biome_rules`. Surface dead-attachment and contradiction feedback (the
   export report's `dead_rules` and the `ValueError` text name everything).
4. **Rule authoring** ("defined prior"): an editor over
   `rules/placement-rules.json` restricted to the section-2 fields/ranges.
5. **Density table**: a 6-kind × 10-biome grid over
   `rules/biome-density.json`, values 0..1.
6. Every save of 3/4/5 (and of weights) is a manifest change → show the
   worldgen-version warning and prompt a re-export
   (`tools/export_manifest.py`, then `tools/enginecheck.py`).

Do NOT touch `forge/server.py` routes from this document's side; the schema
above is the contract, the endpoints are the UI change's business.
