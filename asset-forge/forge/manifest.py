"""VXM -- the species placement manifest, write side.

One versioned binary table per library build, carrying everything voxel-core's
placement policy needs to know about a species WITHOUT decoding a single voxel:
kind, layer, per-biome weight, abundance, spacing, cluster, elevation band,
slope limit, depth band, herd/shoal parameters and a bank reference. The C++
reader is `voxelcore/assetmanifest.h`, on the same parse-refusal model as
`assetgrid.h`: a table that fails any check is refused whole, with a named
reason, because half a species table is worse than none.

WHY A BINARY TABLE AND NOT THE 828 JSON FILES. The engine is integer-only,
deterministic and float-banned; a JSON float parsed on two machines is not a
worldgen input anybody can trust, and 828 file reads on a load path is not a
cost anybody wants. The conversion from authored floats to integer millimetres
and per-milles happens HERE, once, in the tool that owns the schema -- so the
engine never sees a float and the two sides cannot round differently.

THE LAYER TABLE RIDES IN THE MANIFEST HEADER. The four scatter lattices
(assetplacement.h's AssetLayer) are the price list the streaming bound pays,
and the species records are meaningless except against the exact table they
were filed under -- a species filed on "the 26 m layer" is a hole in the world
if the reader's 26 m layer is actually 14 m. Shipping the table inside the same
file makes that disagreement impossible rather than checked. It also makes the
table DATA, tuned by `vxc_assetprobe`'s widening measurement and re-exported,
rather than a constant compiled into two languages.

DETERMINISM CONTRACT. Once a world has been generated with a manifest, ANY
change to this file's output -- the layer table, a weight, an assignment rule --
is a worldgen change: bump vxc::kWorldGenVersion, re-bless goldens
deliberately. That is docs/asset-placement-architecture.md section 9, and it is
why MANIFEST_VERSION exists separately from the VXA version: the two move for
different reasons.

VERSION 2 (2026-08-18): the per-biome placement expansion. Three additions,
none touching the 152-byte species record:

    * a KIND x BIOME DENSITY TABLE (rules/biome-density.json) between the
      layer table and the species records -- the occupancy scalar per asset
      class per biome. 1000 everywhere is today's world exactly; the reader
      REFUSES values above 1000 (veto-only: the table may thin, never boost).
    * a NAMED RULE TABLE (rules/placement-rules.json): placement restrictions
      authored once, by name, each a masked subset of the gate fields.
    * a RULE ATTACHMENT TABLE: (species, biome) -> rule, one or many per
      pair, from each spec's `biome_rules` block. The READER composes a
      pair's rules by intersection (strictest wins) and splits the species
      into per-biome rows at import; this exporter validates the same
      composition BY NAME first, so a contradiction is refused here with the
      rule names in the message, never discovered as a parse error.

The species' biome ALLOWLIST (spec `biome_allow`, resolved by
forge.biomes.allowed) is enforced at encode: a weight outside the allowlist
is zeroed and reported by name, so the wire's weights ARE the allowlist and
the reader needs no second mechanism.

Format, all little-endian, packed (no alignment padding beyond what is spelled
out). Mirrors voxelcore/assetmanifest.h field for field.

    header (32 B):
        magic         u32   "VXM1" = 0x314D5856
        version       u32   = 2
        biome_count   u32   = 10   (len(BIOME_ORDER); reader refuses mismatch)
        layer_count   u32   = 4    (vxc::kAssetLayerCount; reader refuses mismatch)
        species_count u32
        record_bytes  u32   = SPECIES_RECORD_BYTES (reader refuses mismatch)
        rule_count    u32
        attach_count  u32

    biome names: biome_count x 16 bytes, ASCII, zero-padded. THE ORDER IS
        voxelcore/biome.h's BiomeId ORDER, and the names are in the file so the
        C++ reader can refuse a reordering BY NAME instead of silently reading
        rainforest weights as desert weights. That failure would be invisible:
        every weight is a valid weight.

    layers: layer_count x 24 B:
        cell_mm u32 | max_height_mm i32 | max_depth_mm i32 | max_radius_mm i32
        density_per_mille u16 | seed_count u16 | terrain_lattice u8 | pad u8 x 3

    class density: kAssetKindCount(10) x biome_count(10) u16 per-mille,
        kind-major in KIND_ORDER x BIOME_ORDER. <= 1000 or the reader refuses.

    rules: rule_count x 64 B, sorted by name (the whole authored library
        ships, referenced or not, so rule ids are stable across exports):
        name char[32] | field_mask u16 | abundance_q10 u16 | cluster_q10 u16
        water_kind u8 | water_mask u8 | elev_min_mm i32 | elev_max_mm i32
        slope_min i32 | slope_max i32 (pct x 10) | water_max_mm i32
        spacing_mm i32
        Unmasked fields are ZERO on the wire -- one encoding per meaning.

    species: species_count x record_bytes, sorted by name so the table is a
        deterministic function of the spec set:
        name              char[64]  zero-padded ASCII; the BANK REFERENCE --
                                    banks live at <banks>/<name>/<name>-NNNN.vxa
        kind              u8        KIND_ORDER index
        layer             u8        0..layer_count-1, or 255 = not on the
                                    scatter lattice (detail entities)
        flags             u8        bit0 = terrain lattice
        water_kind        u8        detail.water: WATER_ORDER index
        water_mask        u8        fresh=2 / brackish=4 / salt=8 admission bits
        pad               u8
        seeds_baked       u16       bank seeds on disk at export; 0 = none yet
        voxel_size_mm     u32
        biome_weight_pm   u16 x 10  per-mille, biomes.* alone (abundance is NOT
                                    folded here; the reader folds, see below)
        abundance_q10     u16       placement.abundance, 1024 = 1.0
        cluster_q10       u16       placement.cluster, 1024 = 1.0
        spacing_mm        i32       placement.spacing_m, AS AUTHORED (the layer
                                    assignment may not serve it; see the report)
        elev_min_mm       i32
        elev_max_mm       i32
        slope_min_mm_per_m i32      always 0 today: placement.slope_min_pct does
                                    not exist in the spec yet. The field is in
                                    the format so a slope BAND (scree) is a spec
                                    change, not a format bump.
        slope_max_mm_per_m i32      slope_max_pct x 10 (70% grade = 700 mm/m)
        water_max_mm      i32       placement.water_max_m; 0 = does not care.
                                    NOT SERVABLE by the engine today (the baked
                                    shore-distance plane covers lake basins
                                    only); carried so the gate exists the day a
                                    distance field does.
        height_mm         i32       nominal species height (tree height_m, rock
                                    size_m); the AUTHORED number. The baked
                                    grid's true extent is enforced per seed at
                                    bank load by assetLayerAdmitsHeight.
        depth_mm          i32       nominal reach below the anchor; 0 today
        depth_min_mm      i32       detail.depth_min_m (below the WATER surface)
        depth_max_mm      i32       detail.depth_max_m
        min_water_depth_mm i32      detail.min_water_depth_m
        group_min         u16       school/flock/herd size, by kind
        group_max         u16
        group_radius_mm   i32       school_radius_m / spread_m

    record_bytes = 152.

    attachments: attach_count x 8 B, sorted strictly ascending by
        (species_index, biome, rule_index) -- sorted AND unique:
        species_index u16 | biome u8 | pad u8 | rule_index u16 | pad u16

WHAT THE READER FOLDS, AND WHY NOT HERE. vxc::AssetSpecies::weightPerMille is
biome_weight x abundance x min(1, (cell/spacing)^2): the joint per-mille chance
the species takes a site. The manifest keeps the three factors separate because
they are separately authored facts and the fold destroys them (a 0 after the
fold cannot say WHICH factor zeroed it); the reader folds because the fold's
quantisation limit is the reader's to report -- a landmark rock at 3000 m
spacing folds to zero per-mille on a 24 m lattice, i.e. per-mille resolution
cannot express one-per-64-km^2, and the species is ABSENT AND COUNTED rather
than silently absent.
"""

from __future__ import annotations

import json
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

from . import biomes as biomelib
from . import spec as sm

MAGIC = b"VXM1"
MANIFEST_VERSION = 2
HEADER_BYTES = 32
BIOME_NAME_BYTES = 16
LAYER_RECORD_BYTES = 24
SPECIES_RECORD_BYTES = 152
RULE_RECORD_BYTES = 64
RULE_NAME_BYTES = 32
ATTACH_RECORD_BYTES = 8

# Where the placement-wide data lives: the named rule library and the kind x
# biome density table. Species JSON stays in specs/; these are not species.
RULES_PATH = Path(__file__).resolve().parents[1] / "rules" / "placement-rules.json"
DENSITY_PATH = Path(__file__).resolve().parents[1] / "rules" / "biome-density.json"

# AssetOverrideField bits, mirroring assetmanifest.h bit for bit. Keys are the
# spec-side field names (spec.RULE_FIELDS).
RULE_FIELD_BITS = {
    "elev_min_m": 1 << 0,
    "elev_max_m": 1 << 1,
    "slope_min_pct": 1 << 2,
    "slope_max_pct": 1 << 3,
    "water_max_m": 1 << 4,
    "water": 1 << 5,
    "spacing_m": 1 << 6,
    "abundance": 1 << 7,
    "cluster": 1 << 8,
}

# voxelcore/biome.h's BiomeId order, BY HAND AND ON PURPOSE. forge/biomes.py
# orders its tuple for the app's UI; the engine's enum is append-only wire
# order. The names travel in the file so the C++ reader verifies this list
# against its own enum spelling by name -- if either side reorders, the load
# refuses instead of swapping every weight in the library.
BIOME_ORDER = (
    "ocean", "beach", "grassland", "temperate_forest", "rainforest",
    "desert", "savanna", "taiga", "tundra_alpine", "bare_rock",
)

# Stable kind ids. Order is this file's own contract (mirrored in
# assetmanifest.h), append-only; NOT forge/kinds.py's tuple order, which is
# arranged for the app and free to change.
KIND_ORDER = (
    "tree", "bush", "rock", "grass", "reed", "flower",
    "fish", "bird", "quadruped", "cetacean",
)

# detail.water choices, in spec.py's own order.
WATER_ORDER = ("any", "ocean", "river", "lake", "shallow", "reef")

# Salinity admission bits, mirroring assetdetail.h's kWaterMask*.
WATER_MASK_FRESH = 1 << 1
WATER_MASK_BRACKISH = 1 << 2
WATER_MASK_SALT = 1 << 3
WATER_MASK_ANY = WATER_MASK_FRESH | WATER_MASK_BRACKISH | WATER_MASK_SALT

# detail.water -> salinity admission. 'river' includes brackish because a river
# mouth is the reach where the river's own datum has descended to meet the sea
# (lakes.h:1417-1424) -- trout water runs to the tide line. 'shallow' is "the
# margin of anything" per the spec row, so it admits all three. 'reef' is salt
# plus a depth ceiling the reader applies from depth_max_mm.
WATER_KIND_TO_MASK = {
    "any": WATER_MASK_ANY,
    "ocean": WATER_MASK_SALT,
    "river": WATER_MASK_FRESH | WATER_MASK_BRACKISH,
    "lake": WATER_MASK_FRESH,
    "shallow": WATER_MASK_ANY,
    "reef": WATER_MASK_SALT,
}

KINDS_ON_SCATTER = ("tree", "bush", "rock", "grass", "reed", "flower")
KINDS_TERRAIN = ("tree", "rock")
LAYER_NOT_SCATTERED = 255


def load_rules(path: "Path | None" = None) -> "dict[str, dict]":
    """The named rule library, validated: {name: {field: value}}.

    RAISES on anything malformed rather than warning: the library is one
    shared file, a broken rule is referenced by an unknown number of species,
    and an export that silently dropped a restriction is the failure this
    whole mechanism exists to prevent. Values are clamped to the same Param
    ranges the placement sliders use, so a rule cannot say what a spec could
    not. Keys starting with '_' are notes and ignored."""
    p = Path(path) if path is not None else RULES_PATH
    raw = json.loads(p.read_text(encoding="utf-8"))
    rules_raw = raw.get("rules")
    if not isinstance(rules_raw, dict):
        raise ValueError(f"{p}: no 'rules' mapping")
    out: dict[str, dict] = {}
    for name in sorted(rules_raw):
        body = rules_raw[name]
        if not sm.RULE_NAME_RE.match(name or ""):
            raise ValueError(f"{p}: rule name {name!r} is not 1-31 chars of [A-Za-z0-9_-]")
        if not isinstance(body, dict):
            raise ValueError(f"{p}: rule {name!r} is not a mapping")
        rule: dict = {}
        for k, v in body.items():
            if k.startswith("_"):
                continue
            if k not in sm.RULE_FIELDS:
                raise ValueError(f"{p}: rule {name!r} field {k!r} is not one of "
                                 f"{sm.RULE_FIELDS}")
            if k == "water":
                if v not in WATER_ORDER:
                    raise ValueError(f"{p}: rule {name!r} water {v!r} not in {WATER_ORDER}")
                rule[k] = v
                continue
            row = sm.BY_PATH[f"placement.{k}"]
            try:
                val = float(v)
            except (TypeError, ValueError):
                raise ValueError(f"{p}: rule {name!r} field {k}: {v!r} is not a number")
            if val < row.lo or val > row.hi:
                raise ValueError(f"{p}: rule {name!r} field {k}: {val} outside "
                                 f"[{row.lo}, {row.hi}]")
            rule[k] = val
        if not rule:
            raise ValueError(f"{p}: rule {name!r} sets no fields -- it gates nothing")
        if "water_max_m" in rule and rule["water_max_m"] <= 0:
            raise ValueError(f"{p}: rule {name!r} water_max_m must be positive "
                             f"(omit the field for 'does not care')")
        if "spacing_m" in rule and rule["spacing_m"] <= 0:
            raise ValueError(f"{p}: rule {name!r} spacing_m must be positive")
        if ("elev_min_m" in rule and "elev_max_m" in rule
                and rule["elev_min_m"] > rule["elev_max_m"]):
            raise ValueError(f"{p}: rule {name!r} elevation band is inverted")
        if ("slope_min_pct" in rule and "slope_max_pct" in rule
                and rule["slope_min_pct"] > rule["slope_max_pct"]):
            raise ValueError(f"{p}: rule {name!r} slope band is inverted")
        out[name] = rule
    return out


def load_density(path: "Path | None" = None) -> "dict[tuple[str, str], int]":
    """The kind x biome density table, per-mille: {(kind, biome): 0..1000}.

    Missing kinds or biomes read as 1000 (neutral). Unknown keys RAISE -- a
    typo'd biome silently reading as neutral is a silent no-op with the
    owner's tuning inside it. Values above 1.0 raise here for the reason the
    reader refuses them: the table may only thin."""
    p = Path(path) if path is not None else DENSITY_PATH
    raw = json.loads(p.read_text(encoding="utf-8"))
    table_raw = raw.get("density")
    if not isinstance(table_raw, dict):
        raise ValueError(f"{p}: no 'density' mapping")
    out: dict[tuple[str, str], int] = {}
    for kind, per_biome in table_raw.items():
        if kind.startswith("_"):
            continue
        if kind not in KIND_ORDER:
            raise ValueError(f"{p}: {kind!r} is not an asset kind ({KIND_ORDER})")
        if not isinstance(per_biome, dict):
            raise ValueError(f"{p}: density.{kind} is not a mapping")
        for biome, v in per_biome.items():
            if biome.startswith("_"):
                continue
            if biome not in BIOME_ORDER:
                raise ValueError(f"{p}: density.{kind}.{biome!r} is not a biome "
                                 f"({BIOME_ORDER})")
            val = float(v)
            if val < 0.0 or val > 1.0:
                raise ValueError(f"{p}: density.{kind}.{biome} = {val} outside [0, 1] "
                                 f"-- the table may only thin (veto-only), never boost")
            out[(kind, biome)] = _per_mille(val)
    return out


def compose_problems(name: str, biome_key: str, rule_names: "list[str]",
                     rules: "dict[str, dict]", body: dict) -> "list[str]":
    """The contradictions in attaching `rule_names` to (species, biome), as
    human sentences naming the rules -- empty means the composition is sound.

    MIRRORS assetmanifest.cpp's composeOverrides law (intersection, strictest
    wins) in the one place a human sees names instead of indices. The reader
    re-checks the same law on the wire; this exists so the failure is 'rules
    A and B contradict on species S in desert', not a parse refusal."""
    problems: list[str] = []
    elev_min = float(sm.get(body, "placement.elev_min_m"))
    elev_max = float(sm.get(body, "placement.elev_max_m"))
    slope_min = float(sm.get(body, "placement.slope_min_pct") or 0.0)
    slope_max = float(sm.get(body, "placement.slope_max_pct") or 70.0)
    water_kind = None
    water_kind_rule = None
    mask = WATER_KIND_TO_MASK[sm.get(body, "detail.water") or "any"]
    cluster = None
    cluster_rule = None
    for rn in rule_names:
        r = rules[rn]
        if "elev_min_m" in r:
            elev_min = max(elev_min, r["elev_min_m"])
        if "elev_max_m" in r:
            elev_max = min(elev_max, r["elev_max_m"])
        if "slope_min_pct" in r:
            slope_min = max(slope_min, r["slope_min_pct"])
        if "slope_max_pct" in r:
            slope_max = min(slope_max, r["slope_max_pct"])
        if "water" in r:
            if water_kind is not None and water_kind != r["water"]:
                problems.append(
                    f"{name} in {biome_key}: rules {water_kind_rule!r} and {rn!r} "
                    f"state different water kinds ({water_kind} vs {r['water']})")
            water_kind = r["water"]
            water_kind_rule = rn
            mask &= WATER_KIND_TO_MASK[r["water"]]
        if "cluster" in r:
            if cluster is not None and cluster != r["cluster"]:
                problems.append(
                    f"{name} in {biome_key}: rules {cluster_rule!r} and {rn!r} "
                    f"state different cluster strengths ({cluster} vs {r['cluster']})")
            cluster = r["cluster"]
            cluster_rule = rn
    if elev_min > elev_max:
        problems.append(f"{name} in {biome_key}: composed elevation band is empty "
                        f"({elev_min} > {elev_max} m) over rules {rule_names}")
    if slope_min > slope_max:
        problems.append(f"{name} in {biome_key}: composed slope band is empty "
                        f"({slope_min} > {slope_max} %) over rules {rule_names}")
    if water_kind is not None and mask == 0:
        problems.append(f"{name} in {biome_key}: composed salinity admission is empty "
                        f"(species water {sm.get(body, 'detail.water')!r} against rule "
                        f"water {water_kind!r})")
    return problems

# THE COLLISION LINE, and the owner drew it by height, not by kind
# (2026-08-18): "Small bushes, flowers, and grasses should not be collidable in
# terrain lattice. But I'm seeing many larger bushes and tall grasses that are
# as tall vertically as the player. It does not make sense for those to not be
# collidable."
#
# Terrain lattice == voxels in the world grid == solid, collidable, diggable,
# and drawn at EVERY LOD ring. Detail lattice == instanced meshes, no
# collision, and drawn only inside the detail ring. So this threshold decides
# two things at once, and both point the same way: a shrub you cannot see past
# is a shrub you should not walk through, and it is also a shrub that should
# still be there when you look at the hillside from across the valley.
#
# 1.5 m: below a 1.8 m player's eyeline but unmistakably body-scale. Measured
# against the library -- bush median is 1.80 m (46 of 57 over 1.0 m), grass
# tops out at 2.0 m with only 9 over 1.0 m -- so this promotes most true shrubs
# and the few genuinely tall grasses, and leaves tufts, flowers and low cover
# where they belong. Reeds are exempt: they stand in water, where a collidable
# voxel column would wall off a lake margin the player is meant to wade.
# LIVE as of 2026-08-18 (owner decision: "Proceed with plant collision as
# described"). The two blockers the first flip hit, and what each turned out
# to be:
#   1. RESOLUTION. A terrain-lattice species must be baked at the world's
#      100 mm pitch (assetLayerAdmitsVoxelSize); the 38 body-scale candidates
#      were authored at 5 cm (37) and 2 cm (1), so the flag alone produced a
#      manifest the engine refused WHOLE -- correctly. The candidates now
#      carry resolution_cm 10, except the two named in
#      TERRAIN_LATTICE_EXEMPT, whose shapes genuinely do not survive 10 cm.
#      The judgment call, against the project rule (smallest identifying
#      feature ~3 voxels across): a shrub's identifying feature is its
#      foliage mass and multi-stem silhouette, comfortably >30 cm on every
#      promoted bush; the three tall grasses ride grid.capsule's centreline
#      fallback (a stem thinner than half a voxel is drawn as a face-connected
#      one-voxel run -- ground.py's own contract), so a 2 m grass at 10 cm is
#      the same regime as a 40 cm tuft at 2 cm: 15-20 voxels tall, one-voxel
#      stems, connected. What degrades: big-bluestem's seed spike (0.16 m)
#      drops to ~2 voxels. Accepted -- at collision scale the read that
#      matters is "tall grass", not which grass.
#   2. SPACING. ~30 existing trees and rocks carry spacing tighter than their
#      layer's cell (birch 4 m on the 5 m lattice) yet the shipped manifest
#      passes assetSpeciesFits. RESOLVED, not a latent bug: the manifest
#      stores spacing AS AUTHORED, and the reader checks the SERVED spacing
#      -- authored floored at the cell pitch (assetmanifest.cpp's semantic
#      check computes `served = max(spacingMm, L.cellMm)` before calling
#      assetSpeciesFits). Tighter-than-cell spacing means "one per cell, full
#      density": the residual (cell/spacing)^2 fold clamps at 1, the exporter
#      reports every floor by name (underserved_spacing), and kSpacingTooTight
#      exists for callers that pass raw authored spacing (tests, importers).
#      The promoted shrubs join that reported population deliberately.
TERRAIN_LATTICE_MIN_HEIGHT_M = 1.5
KINDS_HEIGHT_PROMOTED = ("bush", "grass")

# Body-scale species that stay on the detail lattice BY NAME, because their
# authored resolution is the species. Each cites its own spec's notes:
#   black-coral-tree: 2 cm, ocean floor. tip_radius_m 0.015 is 1.5 voxels at
#     the authored pitch and "most wants 1 cm" -- at 10 cm the feathering that
#     IS a black coral vanishes into a stone lollipop. It also stands in open
#     water, where the reed argument applies as written.
#   mangrove-sapling: 5 cm, the water margin. Its spec says it outright: the
#     seedling stage lives "on the 5 cm detail lattice, where thirty voxels of
#     height is enough to carry stilts that a 10 cm lattice would lose" -- and
#     a collidable column in the wading margin is what the reed exemption
#     exists to prevent. The 7 m red-mangrove TREE already ships collidable.
TERRAIN_LATTICE_EXEMPT = ("black-coral-tree", "mangrove-sapling")


def is_terrain_lattice(kind: str, height_m: float, name: str) -> bool:
    """Does this species live in the world voxel grid (solid + all-LOD)?

    Trees and rocks always do. Bushes and grasses do IF they are body-scale --
    see TERRAIN_LATTICE_MIN_HEIGHT_M -- and not exempted by name (see
    TERRAIN_LATTICE_EXEMPT). One function so the manifest's flag, the layer
    assignment and any future consumer cannot disagree about it; `name` is
    required so no consumer can forget the exemptions.
    """
    if kind in KINDS_TERRAIN:
        return True
    if name in TERRAIN_LATTICE_EXEMPT:
        return False
    return kind in KINDS_HEIGHT_PROMOTED and float(height_m or 0.0) >= TERRAIN_LATTICE_MIN_HEIGHT_M


@dataclass(frozen=True)
class Layer:
    """One scatter lattice -- vxc::AssetLayer, in the same units."""
    cell_mm: int
    max_height_mm: int
    max_depth_mm: int
    max_radius_mm: int
    density_per_mille: int
    seed_count: int
    terrain_lattice: bool


# THE LAYER TABLE. This is the price list the streaming bound pays
# (assetplacement.h's header: vetoed regions pay it too, because the bound
# cannot read the policy), so every number here was chosen against a
# measurement, not by eye:
#
#   * vxc_assetprobe's widening census (voxel-core/bench/assetprobe.cpp) --
#     counts of admitted chunks with and without each candidate table over
#     real terrain -- picked the height caps and L0's density.
#   * The library's own authored numbers (828 specs, measured 2026-08-15:
#     trees height min/med/max 1.8/14/80 m, spacing 2.5/7/700 m; rocks
#     0.65/2.4/90 m, 1/14/3000 m) picked the cell pitches and the split
#     points. HALF the tree library is taller than 14 m at less than 8 m
#     spacing -- a boreal forest is tall AND tight -- so a "canopy" cap of
#     14 m (the architecture pass's illustrative guess) would demote 39 of 78
#     trees to a 24 m lattice and gut every conifer forest. The caps below are
#     where the library actually splits.
#
# ANY EDIT HERE IS A WORLDGEN CHANGE once a world has shipped against it:
# kWorldGenVersion bump, goldens re-blessed. max_radius_mm values are ceilings
# over the species filed on the layer (checked per baked grid at bank load);
# raising a species past its layer's ceiling means re-tuning the layer.
# A baked INDIVIDUAL overshoots the authored nominal height: `variation` draws
# each seed's own size, and the first full bake measured the worst case at
# +18% (small-leaved-lime, 25 m authored, 29.6 m baked -- with 15 other species
# refused for the same reason). Filing uses nominal x this margin against the
# layer cap, so the cap the BOUND pays already contains what the bakes
# actually reach; the per-file check at bake and load stays, because a margin
# is a prediction and the check is a measurement.
FILE_HEADROOM_NUM, FILE_HEADROOM_DEN = 13, 10  # x1.3

LAYERS = (
    # L0 hero/emergent. The tail: emergents and big landform rocks past L1's
    # filing line. Density is the knob that keeps a 60 m widening RARE -- at
    # 60 per-mille on a 24 m cell, a chunk footprint dilated by the 15 m reach
    # sees ~2.6 cells, so ~13% of footprints pay the L0 term and the rest pay
    # nothing (the per-layer bound composition buys a term only where the
    # layer has a site in reach).
    #
    # CAP AND RADIUS ARE MEASURED, NOT GUESSED: over the full 4-seed bank bake
    # the tallest L0 grid is 52.5 m and the widest reach is 14.5 m (kapok).
    # RADIUS 15_000 -> 15_500, 2026-08-18: the taper-term fix (0450a4a) reshaped
    # every tree; kapok's widest baked seed now reaches 15.3 m and the bake
    # REFUSED it against the old ceiling, which would have pinned kapok's bank
    # to the pre-taper species forever. The ceiling follows the measurement,
    # as this comment already promised it would; the extra 0.5 m of query
    # dilation is priced by the same widening census as the original number.
    # The first cut said 96 m / 20 m; the widening census priced that at ~11
    # wasted chunk layers on every L0-present footprint and ~2 chunk layers of
    # dilation slack, for headroom no baked asset uses. The wider heroes that
    # WOULD need more fold to zero everywhere (900 m+ spacing is past
    # per-mille resolution), are absent from the world, and export_banks skips
    # their banks -- paying dilation for species that cannot appear is paying
    # for nothing.
    Layer(cell_mm=24_000, max_height_mm=60_000, max_depth_mm=8_000,
          max_radius_mm=15_500, density_per_mille=60, seed_count=4,
          terrain_lattice=True),
    # L1 canopy. Most trees, including the boreal giants -- the library
    # measures HALF of all trees past 14 m at under 8 m spacing, so the cap is
    # set where the density can stay at 1000: 26 m nominal filing, 34 m baked
    # cap. The widening is effectively constant (density 1000), so this cap IS
    # the global price; vxc_assetprobe's census is what it is priced against.
    #
    # DENSITY 1000 -> 350, 2026-08-17, and this is the ONE forest-density
    # knob that actually works. Per-species spacing dilution folds into PICK
    # WEIGHTS, and in any biome rich enough to saturate the occupancy cap the
    # dilution cancels across species (measured: canopy spacing x1.75 moved
    # the census 381 -> 376). Layer density thins SITES before any of that,
    # uniformly. At 1000 the resolver stood a tree on 41% of 5 m cells --
    # ~830 stems/ha, a thicket the camera could not see 5 m into (the owner's
    # "mess" screenshots); 350 lands ~290/ha, mature-forest spacing with
    # sight lines, and cuts the composed far-forest quad bill by the same
    # factor (the naive thicket at coarse levels blew a 200M-quad pool with
    # 28,205 chunks refused). Species mix and biome patterns are untouched --
    # this thins WHERE trees stand, not WHICH trees stand there.
    Layer(cell_mm=5_000, max_height_mm=34_000, max_depth_mm=4_000,
          max_radius_mm=12_000, density_per_mille=350, seed_count=4,
          terrain_lattice=True),
    # L2 small terrain: boulders and shrubs-with-trunks to 5 m nominal.
    # Radius from the measured widest resident (boulder-beach, 5.8 m of reach:
    # a slab field is wide, not tall). The cap carries extra headroom past the
    # x1.3 filing margin because wild-banana measures 1.4x its nominal -- and
    # it is nearly free: the bound takes the MAX over layers, so wherever L1's
    # constant 34 m stands (density 1000, i.e. almost everywhere), L2's cap is
    # shadowed entirely.
    # Density 1000 -> 550 with L1's retune: understory thinned less than
    # canopy (shrubs read as texture at distance and fill gaps at eye level),
    # but a shrub on every second 2.2 m cell was part of the same wall.
    Layer(cell_mm=2_200, max_height_mm=7_500, max_depth_mm=2_000,
          max_radius_mm=6_000, density_per_mille=550, seed_count=4,
          terrain_lattice=True),
    # L3 ground cover and every other detail-lattice plant: excluded from the
    # streaming bound entirely (assetplacement.h's terrainLattice guard,
    # tested both ways), so its caps PRICE nothing -- they are the declared
    # box the load-time sanity check holds species to, and they hold the
    # library as authored: bushes and reeds run to 4 m and giant kelp to 28 m,
    # all of it geometry that never enters the world voxel grid.
    # DENSITY 1000 -> 300, 2026-08-18. At 1000 the detail lattice stood a
    # bush, tuft or flower on EVERY 800 mm cell -- the owner walked into it at
    # the temperate site and reported ground cover so thick it blocked
    # movement. 800 mm is the pitch a single tuft needs; it is not the pitch a
    # SHRUB needs, and the layer carries both. 300 leaves ground cover reading
    # as continuous at eye level (a tuft every ~1.5 m, and clustering gathers
    # them further) while opening the walkable gaps a player needs. Density is
    # the honest knob here for the same reason it was on L1: per-species
    # spacing folds into pick weights and cancels wherever a biome saturates.
    # max_radius 2_500 -> 10_000, 2026-08-18. The bake REFUSED 12 species whose
    # baked reach exceeds the declared radius (giant-kelp 9.3 m, feather-boa-kelp
    # 4.2 m, elephant-grass 4.05 m, giant-reed 2.85 m ...) with the correct
    # reason: "rect queries would miss its edge". Raising it is free HERE and
    # only here -- L3 is excluded from the streaming bound by the terrainLattice
    # guard, so unlike L0/L1 this radius prices no admitted chunks; it only
    # dilates the detail resolver's own query rect, which is what makes a
    # 9 m kelp frond findable from the cell its anchor sits in.
    Layer(cell_mm=800, max_height_mm=30_000, max_depth_mm=2_000,
          max_radius_mm=10_000, density_per_mille=300, seed_count=4,
          terrain_lattice=False),
)


@dataclass
class ExportReport:
    """Everything the export refused or bent, BY NAME. The signature failure
    this project keeps paying for is the silent no-op; an exporter that files
    828 species and says nothing is one."""
    species: int = 0
    rules: int = 0
    attachments: int = 0
    underserved_spacing: list[tuple[str, float, float]] = field(default_factory=list)
    too_rare_to_express: list[tuple[str, int, float]] = field(default_factory=list)
    unplaceable: list[tuple[str, str]] = field(default_factory=list)
    no_biome: list[str] = field(default_factory=list)
    # (species, biome, authored weight) zeroed because the biome is outside
    # the species' authored allowlist -- the allowlist is authoritative.
    outside_allowlist: list[tuple[str, str, float]] = field(default_factory=list)
    # (species, biome, rule, why): attachments that gate nothing and were not
    # written -- the biome is dark or disallowed for that species.
    dead_rules: list[tuple[str, str, str, str]] = field(default_factory=list)

    def lines(self) -> list[str]:
        out = [f"manifest: {self.species} species, {self.rules} rules, "
               f"{self.attachments} attachments"]
        if self.outside_allowlist:
            out.append(f"  {len(self.outside_allowlist)} biome weights zeroed as OUTSIDE "
                       f"their species' authored allowlist:")
            for name, biome, w in sorted(self.outside_allowlist):
                out.append(f"    {name}: {biome} weight {w:g} -> 0")
        if self.dead_rules:
            out.append(f"  {len(self.dead_rules)} rule attachments DEAD (not written):")
            for name, biome, rule, why in sorted(self.dead_rules):
                out.append(f"    {name} in {biome}: {rule!r} -- {why}")
        if self.no_biome:
            out.append(f"  {len(self.no_biome)} species with zero weight in every biome "
                       f"(never appear anywhere): {', '.join(sorted(self.no_biome)[:8])}"
                       + (" ..." if len(self.no_biome) > 8 else ""))
        if self.underserved_spacing:
            out.append(f"  {len(self.underserved_spacing)} species authored tighter spacing "
                       f"than their layer's cell can express (served at the cell pitch):")
            for name, want, got in sorted(self.underserved_spacing):
                out.append(f"    {name}: authored {want} m, served at {got} m")
        if self.too_rare_to_express:
            out.append(f"  {len(self.too_rare_to_express)} species whose folded weight rounds "
                       f"to zero per-mille (ABSENT from the world, deliberately -- per-mille "
                       f"cannot express their rarity on their layer's lattice):")
            for name, layer, spacing in sorted(self.too_rare_to_express):
                out.append(f"    {name}: layer L{layer}, spacing {spacing} m")
        if self.unplaceable:
            out.append(f"  {len(self.unplaceable)} species REFUSED (not written to the table):")
            for name, why in sorted(self.unplaceable):
                out.append(f"    {name}: {why}")
        return out


# A bank seed file is <name>-NNNN.vxa; the NNNN is the seed.
SEED_FILE_RE = re.compile(r"-(\d{4})\.vxa$")


@dataclass
class CurationSummary:
    """Who the publish gate held back, BY NAME -- the same rule ExportReport
    states for itself: a gate that skips species silently is a silent no-op
    with a human's verdict inside it."""
    approved: int = 0
    grandfathered: int = 0
    draft: list[str] = field(default_factory=list)
    rejected: list[str] = field(default_factory=list)

    def lines(self) -> list[str]:
        held = len(self.draft) + len(self.rejected)
        out = [f"curation: {self.approved} approved "
               f"({self.grandfathered} grandfathered, never curated by a "
               f"human), {held} held back"]
        if self.draft:
            out.append(f"  draft, not exported: {', '.join(sorted(self.draft))}")
        if self.rejected:
            out.append(f"  rejected, not exported: {', '.join(sorted(self.rejected))}")
        return out


def curated_inputs(specs_dir: Path, banks_dir: Path
                   ) -> "tuple[list[tuple[str, dict]], dict[str, int], CurationSummary]":
    """Everything `encode` should see, with the publish gate applied: the
    approved (name, validated body) list, and per species the count of bank
    seeds ON DISK that are also on its approved list.

    ONE FUNCTION BECAUSE THERE ARE TWO CALLERS. `tools/export_manifest.py`
    writes the table from this and `tools/enginecheck.py` re-derives it as
    the staleness check; if each applied the gate itself, the day their two
    readings differed the check would certify a manifest the exporter would
    not write -- a derived fact in two places, which is the failure the check
    exists to catch.

    The seed count keeps the counting discipline the manifest has always had
    -- COUNTED off the disk, never assumed from the verdict -- and then
    intersects with the approved list. So a bake that failed still shows up
    as a lower number, and a stale file for a seed a curator pulled does not
    inflate it."""
    specs: list[tuple[str, dict]] = []
    summary = CurationSummary()
    approved_seeds: dict[str, set[int]] = {}
    for p in sorted(specs_dir.glob("*.json")):
        body, _report = sm.load(p)
        cur = sm.curation(body)
        if cur["status"] == "rejected":
            summary.rejected.append(p.stem)
            continue
        if cur["status"] == "draft":
            summary.draft.append(p.stem)
            continue
        summary.approved += 1
        if not cur["curated"]:
            summary.grandfathered += 1
        approved_seeds[p.stem] = set(cur["seeds"])
        specs.append((p.stem, body))

    seeds_baked: dict[str, int] = {}
    if banks_dir.is_dir():
        for d in sorted(banks_dir.iterdir()):
            if not d.is_dir() or d.name not in approved_seeds:
                continue
            ok = approved_seeds[d.name]
            n = 0
            for f in d.iterdir():
                m = SEED_FILE_RE.search(f.name)
                if m and int(m.group(1)) in ok:
                    n += 1
            if n:
                seeds_baked[d.name] = n
    return specs, seeds_baked, summary


def apply_rock_classification(specs: "list[tuple[str, dict]]",
                              banks_dir: Path) -> int:
    """ROCK SELF-CLASSIFICATION (owner directive, 2026-08-18): mutate the rock
    specs' placement in-memory from what the baked grid physically IS, before
    `encode` sees them. Returns how many species had spacing raised.

    volume  = solid voxels x pitch^3 of seed 1's bank (the library individual;
              seeds vary a few percent, rarity doesn't care).
    spacing = max(authored, 6.0 x volume^0.35 m) -- the size-frequency law
              N(>V) ~ V^-1.8..-2 (docs/placement-research.md). Authored always
              wins UPWARD: a designer may make a species rarer than physics,
              never more common than its size allows.
    cluster = floor of 0.7 for slope-banded (talus-class) rocks: fragmentation
              debris clusters, and the authored median (0.35) reads as
              scattered gravel where a talus fan should be.

    LIVES HERE, NOT IN export_manifest.py, FOR THE `curated_inputs` REASON
    STATED ABOVE IT: there are two callers. The exporter applies it before
    writing and enginecheck applies it before comparing; for a stretch of
    2026-08-18 only the exporter did, so enginecheck failed every manifest the
    exporter had just written -- the checker was rebuilding a table the
    exporter would never produce. Raised spacing also MOVES LAYERS
    (assign_layer files by spacing among the height survivors), so the drift
    was not cosmetic: layer bytes differed too."""
    from . import vxa  # lazy: vxa is only needed when banks are read

    derived_n = 0
    for name, body in specs:
        if body.get("kind") != "rock":
            continue
        bank = sorted((banks_dir / name).glob(f"{name}-*.vxa")) \
            if (banks_dir / name).is_dir() else []
        if not bank:
            continue
        g = vxa.read(str(bank[0]))
        solid = int((g.data != 0).sum())
        pitch = float(body.get("resolution_cm", 10.0)) / 100.0
        vol = solid * pitch ** 3
        pl = body.setdefault("placement", {})
        derived_sp = 6.0 * (vol ** 0.35)
        if derived_sp > float(pl.get("spacing_m") or 0.0):
            pl["spacing_m"] = round(derived_sp, 2)
            derived_n += 1
        if float(pl.get("slope_min_pct") or 0.0) > 0 and float(pl.get("cluster") or 0.0) < 0.7:
            pl["cluster"] = 0.7
    return derived_n


def _kind_group_params(spec: dict, kind: str) -> tuple[int, int, int]:
    """(group_min, group_max, group_radius_mm) from the group that owns this
    kind's sociality: shoals for what swims, flocks for what flies, herds for
    what walks. Plants group by the cluster field instead and carry 1/1/0."""
    if kind in ("fish", "cetacean"):
        lo = int(sm.get(spec, "detail.school_min"))
        hi = int(sm.get(spec, "detail.school_max"))
        r = float(sm.get(spec, "detail.school_radius_m"))
    elif kind == "bird":
        lo = int(sm.get(spec, "flock.size_min"))
        hi = int(sm.get(spec, "flock.size_max"))
        r = float(sm.get(spec, "flock.spread_m"))
    elif kind == "quadruped":
        lo = int(sm.get(spec, "herd.size_min"))
        hi = int(sm.get(spec, "herd.size_max"))
        r = float(sm.get(spec, "herd.spread_m"))
    else:
        return 1, 1, 0
    return lo, max(lo, hi), _mm(r)


def _mm(metres: float) -> int:
    return int(round(float(metres) * 1000.0))


def _q10(unit: float) -> int:
    v = int(round(float(unit) * 1024.0))
    return max(0, min(65535, v))


def _per_mille(unit: float) -> int:
    v = int(round(float(unit) * 1000.0))
    return max(0, min(1000, v))


def nominal_height_m(spec: dict, kind: str) -> float:
    """The species' authored vertical extent, for layer filing. rock.size_m is
    the LONGEST dimension, which bounds height from above -- conservative in
    the direction that cannot put a crown through the streaming bound."""
    if kind == "rock":
        return float(sm.get(spec, "rock.size_m"))
    return float(sm.get(spec, "height_m") or 0.0)


def assign_layer(kind: str, height_m: float, report: ExportReport, name: str,
                 spacing_m: float) -> int:
    """Which lattice a species is filed on.

    HEIGHT IS THE HARD GATE, SPACING PICKS AMONG THE SURVIVORS. The bound is
    made of the layer's max_height_mm, so a too-tall species on a short layer
    is a hole in the world at its own crown -- layers whose cap the species
    exceeds are simply not candidates. Among the candidates, take the COARSEST
    lattice the authored spacing can use: the lattice IS the spacing mechanism
    (a per-instance distance test cannot be a pure hash), and the residual
    (cell/spacing)^2 is folded into the pick weight by the reader. Filing by
    height alone put a 3 m glacial erratic authored at 45 m spacing on the
    2.2 m lattice, where its rarity folds to less than one per-mille and the
    species vanishes -- 27 species went absent that way in the first export of
    this table, which is what this rule exists to prevent.

    Both remaining bends are visible, not silent: spacing tighter than the
    densest candidate's cell is served at the cell pitch and reported;
    spacing so wide even the coarsest lattice folds it to zero per-mille is
    reported as absent (the true landmarks -- one arch per 3 km -- which
    per-mille pick weights cannot express and a scatter lattice should not
    try to).
    """
    if kind not in KINDS_ON_SCATTER:
        return LAYER_NOT_SCATTERED
    if not is_terrain_lattice(kind, height_m, name):
        return 3  # ground cover: the detail lattice (no collision, detail ring only)
    # Nominal x headroom against the cap: what the BAKES reach, not what the
    # spec says. See FILE_HEADROOM_*.
    h_mm = _mm(height_m) * FILE_HEADROOM_NUM // FILE_HEADROOM_DEN
    candidates = [li for li in (0, 1, 2) if h_mm <= LAYERS[li].max_height_mm]
    if not candidates:
        report.unplaceable.append(
            (name, f"height {height_m} m exceeds every terrain layer's maximum "
                   f"({LAYERS[0].max_height_mm / 1000.0} m)"))
        return -1
    # Coarsest candidate whose cell the spacing can use...
    fits = [li for li in candidates if spacing_m * 1000.0 >= LAYERS[li].cell_mm]
    if fits:
        return max(fits, key=lambda li: LAYERS[li].cell_mm)
    # ...or the densest candidate, with the spacing served at its cell pitch.
    li = min(candidates, key=lambda li: LAYERS[li].cell_mm)
    report.underserved_spacing.append((name, spacing_m, LAYERS[li].cell_mm / 1000.0))
    return li


def folded_top_per_mille(spec: dict, layer: int, top_weight_pm: "int | None" = None) -> int:
    """The reader's folded pick weight for this species' best biome, predicted
    with the reader's own integer arithmetic (round-to-nearest). Zero means the
    species is ABSENT from the world: per-mille resolution cannot express its
    rarity on its layer's lattice. Used by the export report and by
    export_banks, which skips baking banks nothing can ever place."""
    if top_weight_pm is None:
        top_weight_pm = max(
            _per_mille(sm.get(spec, f"biomes.{b}") or 0.0) for b in BIOME_ORDER)
    cell_mm = LAYERS[layer].cell_mm
    spacing_mm = max(_mm(float(sm.get(spec, "placement.spacing_m"))), cell_mm)
    num = top_weight_pm * _q10(sm.get(spec, "placement.abundance")) * cell_mm * cell_mm
    den = 1024 * spacing_mm * spacing_mm
    return (num + den // 2) // den


def species_record(spec: dict, name: str, seeds_baked: int,
                   report: ExportReport) -> bytes | None:
    kind = sm.get(spec, "kind")
    if kind not in KIND_ORDER:
        report.unplaceable.append((name, f"unknown kind {kind!r}"))
        return None
    if isinstance(spec.get("biome_rules"), dict) and "__illegible__" in spec["biome_rules"]:
        # validate() named the damage already; exporting the species WITHOUT
        # its restrictions would be worse than not exporting it.
        report.unplaceable.append(
            (name, "biome_rules block is illegible -- refusing to publish the "
                   "species un-restricted; fix or remove the block"))
        return None
    spacing_m = float(sm.get(spec, "placement.spacing_m"))
    layer = assign_layer(kind, nominal_height_m(spec, kind), report, name, spacing_m)
    if layer < 0:
        return None

    # THE ALLOWLIST IS ENFORCED HERE, so the wire's weights ARE the effective
    # biome set and the reader needs no second mechanism. For a spec with no
    # authored `biome_allow` the allowlist is derived from the weights and
    # this zeroes nothing -- bit-for-bit today's export.
    allowed = biomelib.allowed(spec)
    weights = []
    any_weight = False
    for b in BIOME_ORDER:
        w = sm.get(spec, f"biomes.{b}")
        pm = _per_mille(w if w is not None else 0.0)
        if pm > 0 and b not in allowed:
            report.outside_allowlist.append((name, b, float(w or 0.0)))
            pm = 0
        any_weight = any_weight or pm > 0
        weights.append(pm)
    if not any_weight:
        report.no_biome.append(name)

    # Would the reader's fold round this species to nothing? Predicted here so
    # the report can say so next to the species' own numbers; the C++ importer
    # counts the same event on its side (AssetTableBuildStats::tooRare).
    if layer not in (LAYER_NOT_SCATTERED,) and any_weight:
        if folded_top_per_mille(spec, layer, max(weights)) == 0:
            report.too_rare_to_express.append((name, layer, spacing_m))

    water_kind = sm.get(spec, "detail.water") or "any"
    if water_kind not in WATER_ORDER:
        report.unplaceable.append((name, f"unknown detail.water {water_kind!r}"))
        return None

    res_cm = float(sm.get(spec, "resolution_cm"))
    voxel_mm = int(round(res_cm * 10.0))
    lo, hi, radius_mm = _kind_group_params(spec, kind)

    encoded_name = name.encode("ascii")
    if len(encoded_name) >= 64:
        report.unplaceable.append((name, "name longer than 63 bytes"))
        return None

    rec = struct.pack(
        "<64s"     # name
        "BBBB"     # kind, layer, flags, water_kind
        "BxH"      # water_mask, pad, seeds_baked
        "I"        # voxel_size_mm
        "10H"      # biome weights
        "HH"       # abundance_q10, cluster_q10
        "i"        # spacing_mm
        "ii"       # elev band
        "ii"       # slope band
        "i"        # water_max_mm
        "ii"       # height_mm, depth_mm
        "iii"      # depth band + min water depth
        "HH"       # group_min, group_max
        "i",       # group_radius_mm
        encoded_name,
        KIND_ORDER.index(kind),
        layer & 0xFF,
        1 if is_terrain_lattice(kind, nominal_height_m(spec, kind), name) else 0,
        WATER_ORDER.index(water_kind),
        WATER_KIND_TO_MASK[water_kind],
        seeds_baked,
        voxel_mm,
        *weights,
        _q10(sm.get(spec, "placement.abundance")),
        _q10(sm.get(spec, "placement.cluster")),
        _mm(spacing_m),
        _mm(sm.get(spec, "placement.elev_min_m")),
        _mm(sm.get(spec, "placement.elev_max_m")),
        int(round(float(sm.get(spec, "placement.slope_min_pct") or 0.0) * 10.0)),
        int(round(float(sm.get(spec, "placement.slope_max_pct") or 70.0) * 10.0)),
        _mm(sm.get(spec, "placement.water_max_m") or 0.0),
        _mm(nominal_height_m(spec, kind)),
        0,  # depth below anchor: measured per baked grid at bank load instead
        _mm(sm.get(spec, "detail.depth_min_m")),
        _mm(sm.get(spec, "detail.depth_max_m")),
        _mm(sm.get(spec, "detail.min_water_depth_m")),
        lo, hi, radius_mm,
    )
    assert len(rec) == SPECIES_RECORD_BYTES, len(rec)
    return rec


def _rule_record(name: str, rule: dict) -> bytes:
    """One 64-byte named rule, unmasked fields zero."""
    mask = 0
    for k in rule:
        mask |= RULE_FIELD_BITS[k]
    water_kind = rule.get("water")
    rec = struct.pack(
        "<32sHHHBBiiiiii",
        name.encode("ascii"),
        mask,
        _q10(rule["abundance"]) if "abundance" in rule else 0,
        _q10(rule["cluster"]) if "cluster" in rule else 0,
        WATER_ORDER.index(water_kind) if water_kind is not None else 0,
        WATER_KIND_TO_MASK[water_kind] if water_kind is not None else 0,
        _mm(rule["elev_min_m"]) if "elev_min_m" in rule else 0,
        _mm(rule["elev_max_m"]) if "elev_max_m" in rule else 0,
        int(round(rule["slope_min_pct"] * 10.0)) if "slope_min_pct" in rule else 0,
        int(round(rule["slope_max_pct"] * 10.0)) if "slope_max_pct" in rule else 0,
        _mm(rule["water_max_m"]) if "water_max_m" in rule else 0,
        _mm(rule["spacing_m"]) if "spacing_m" in rule else 0,
    )
    assert len(rec) == RULE_RECORD_BYTES, len(rec)
    return rec


def encode(specs: "list[tuple[str, dict]]",
           seeds_baked: "dict[str, int] | None" = None,
           report: ExportReport | None = None,
           rules: "dict[str, dict] | None" = None,
           density: "dict[tuple[str, str], int] | None" = None) -> bytes:
    """The manifest for a list of (name, validated spec body), sorted by name
    so the byte stream is a deterministic function of the spec set.

    `rules` and `density` default to the checked-in libraries
    (rules/placement-rules.json, rules/biome-density.json) so the exporter
    and enginecheck cannot read them differently. RAISES, with names, on an
    unknown rule reference or a contradictory composition: a typo that
    silently dropped a restriction would place species where a human fenced
    them out, which is worse than a failed export."""
    report = report if report is not None else ExportReport()
    seeds_baked = seeds_baked or {}
    rules = rules if rules is not None else load_rules()
    density = density if density is not None else load_density()

    rule_names = sorted(rules)
    rule_index = {n: i for i, n in enumerate(rule_names)}

    records = []
    attachments: list[tuple[int, int, int]] = []  # (species_idx, biome_id, rule_idx)
    problems: list[str] = []
    for name, spec in sorted(specs, key=lambda t: t[0]):
        rec = species_record(spec, name, int(seeds_baked.get(name, 0)), report)
        if rec is None:
            continue
        idx = len(records)
        records.append(rec)

        biome_rules = spec.get("biome_rules")
        if not isinstance(biome_rules, dict):
            continue
        allowed = biomelib.allowed(spec)
        for bkey in sorted(biome_rules, key=lambda k: BIOME_ORDER.index(k)
                           if k in BIOME_ORDER else 99):
            if bkey not in BIOME_ORDER:
                continue  # validate() warned already
            names = biome_rules[bkey]
            unknown = [n for n in names if n not in rules]
            if unknown:
                problems.append(f"{name} in {bkey}: unknown rule(s) "
                                f"{', '.join(repr(u) for u in unknown)} -- not in "
                                f"{RULES_PATH.name}")
                continue
            if bkey not in allowed:
                for n in names:
                    report.dead_rules.append(
                        (name, bkey, n, "biome is outside the species' allowlist"))
                continue
            if _per_mille(sm.get(spec, f"biomes.{bkey}") or 0.0) == 0:
                for n in names:
                    report.dead_rules.append(
                        (name, bkey, n, "species has zero weight in this biome"))
                continue
            problems.extend(compose_problems(name, bkey, names, rules, spec))
            for n in sorted(names, key=lambda n: rule_index[n]):
                attachments.append((idx, BIOME_ORDER.index(bkey), rule_index[n]))
    if problems:
        raise ValueError("manifest export refused:\n  " + "\n  ".join(problems))

    report.species = len(records)
    report.rules = len(rule_names)
    report.attachments = len(attachments)

    header = MAGIC + struct.pack(
        "<IIIIIII", MANIFEST_VERSION, len(BIOME_ORDER), len(LAYERS),
        len(records), SPECIES_RECORD_BYTES, len(rule_names), len(attachments))
    assert len(header) == HEADER_BYTES

    biome_block = b"".join(
        b.encode("ascii").ljust(BIOME_NAME_BYTES, b"\0") for b in BIOME_ORDER)

    layer_block = b""
    for L in LAYERS:
        layer_block += struct.pack(
            "<IiiiHHB3x", L.cell_mm, L.max_height_mm, L.max_depth_mm,
            L.max_radius_mm, L.density_per_mille, L.seed_count,
            1 if L.terrain_lattice else 0)
    assert len(layer_block) == LAYER_RECORD_BYTES * len(LAYERS)

    density_block = b""
    for kind in KIND_ORDER:
        for biome in BIOME_ORDER:
            density_block += struct.pack("<H", density.get((kind, biome), 1000))
    assert len(density_block) == 2 * len(KIND_ORDER) * len(BIOME_ORDER)

    rule_block = b"".join(_rule_record(n, rules[n]) for n in rule_names)

    # Attachments sorted strictly ascending by (species, biome, rule): the
    # reader refuses any other order, which is what makes the file's byte
    # encoding unique.
    attach_block = b""
    for sp, bio, ru in sorted(attachments):
        attach_block += struct.pack("<HBxH2x", sp, bio, ru)
    assert len(attach_block) == ATTACH_RECORD_BYTES * len(attachments)

    return (header + biome_block + layer_block + density_block + rule_block +
            b"".join(records) + attach_block)


def decode(blob: bytes) -> dict:
    """Round-trip check only -- the PRODUCTION reader is voxelcore's, and this
    one exists so the exporter's tests do not certify the exporter with the
    exporter."""
    if blob[:4] != MAGIC:
        raise ValueError("bad magic")
    version, biomes, layers, count, rec_bytes, rule_count, attach_count = \
        struct.unpack_from("<IIIIIII", blob, 4)
    if version != MANIFEST_VERSION:
        raise ValueError(f"version {version}")
    off = HEADER_BYTES
    names = []
    for _ in range(biomes):
        names.append(blob[off:off + BIOME_NAME_BYTES].rstrip(b"\0").decode("ascii"))
        off += BIOME_NAME_BYTES
    layer_rows = []
    for _ in range(layers):
        layer_rows.append(struct.unpack_from("<IiiiHHB3x", blob, off))
        off += LAYER_RECORD_BYTES
    density = {}
    for kind in KIND_ORDER:
        for biome in BIOME_ORDER:
            density[(kind, biome)] = struct.unpack_from("<H", blob, off)[0]
            off += 2
    rules = []
    for _ in range(rule_count):
        r = struct.unpack_from("<32sHHHBBiiiiii", blob, off)
        rules.append({
            "name": r[0].rstrip(b"\0").decode("ascii"),
            "field_mask": r[1],
            "abundance_q10": r[2],
            "cluster_q10": r[3],
            "water_kind": WATER_ORDER[r[4]],
            "water_mask": r[5],
            "elev_min_mm": r[6],
            "elev_max_mm": r[7],
            "slope_min": r[8],
            "slope_max": r[9],
            "water_max_mm": r[10],
            "spacing_mm": r[11],
        })
        off += RULE_RECORD_BYTES
    species = []
    for _ in range(count):
        rec = struct.unpack_from(
            "<64sBBBBBxHI10HHHiiiiiiiiiiiHHi", blob, off)
        species.append({
            "name": rec[0].rstrip(b"\0").decode("ascii"),
            "kind": KIND_ORDER[rec[1]],
            "layer": rec[2],
            "flags": rec[3],
            "water_kind": WATER_ORDER[rec[4]],
            "water_mask": rec[5],
            "seeds_baked": rec[6],
            "voxel_size_mm": rec[7],
            "weights": rec[8:18],
            "abundance_q10": rec[18],
            "cluster_q10": rec[19],
            "spacing_mm": rec[20],
            "elev_min_mm": rec[21],
            "elev_max_mm": rec[22],
            "slope_min": rec[23],
            "slope_max": rec[24],
            "water_max_mm": rec[25],
            "height_mm": rec[26],
            "depth_mm": rec[27],
            "depth_min_mm": rec[28],
            "depth_max_mm": rec[29],
            "min_water_depth_mm": rec[30],
            "group_min": rec[31],
            "group_max": rec[32],
            "group_radius_mm": rec[33],
        })
        off += rec_bytes
    attachments = []
    for _ in range(attach_count):
        a = struct.unpack_from("<HBxH2x", blob, off)
        attachments.append({"species": a[0], "biome": BIOME_ORDER[a[1]], "rule": a[2]})
        off += ATTACH_RECORD_BYTES
    return {"biomes": tuple(names), "layers": layer_rows, "density": density,
            "rules": rules, "species": species, "attachments": attachments}
