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

Format, all little-endian, packed (no alignment padding beyond what is spelled
out). Mirrors voxelcore/assetmanifest.h field for field.

    header (32 B):
        magic         u32   "VXM1" = 0x314D5856
        version       u32   = 1
        biome_count   u32   = 10   (len(BIOME_ORDER); reader refuses mismatch)
        layer_count   u32   = 4    (vxc::kAssetLayerCount; reader refuses mismatch)
        species_count u32
        record_bytes  u32   = SPECIES_RECORD_BYTES (reader refuses mismatch)
        reserved      u32 x 2 = 0

    biome names: biome_count x 16 bytes, ASCII, zero-padded. THE ORDER IS
        voxelcore/biome.h's BiomeId ORDER, and the names are in the file so the
        C++ reader can refuse a reordering BY NAME instead of silently reading
        rainforest weights as desert weights. That failure would be invisible:
        every weight is a valid weight.

    layers: layer_count x 24 B:
        cell_mm u32 | max_height_mm i32 | max_depth_mm i32 | max_radius_mm i32
        density_per_mille u16 | seed_count u16 | terrain_lattice u8 | pad u8 x 3

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

import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

from . import biomes as biomelib
from . import spec as sm

MAGIC = b"VXM1"
MANIFEST_VERSION = 1
HEADER_BYTES = 32
BIOME_NAME_BYTES = 16
LAYER_RECORD_BYTES = 24
SPECIES_RECORD_BYTES = 152

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
    # The first cut said 96 m / 20 m; the widening census priced that at ~11
    # wasted chunk layers on every L0-present footprint and ~2 chunk layers of
    # dilation slack, for headroom no baked asset uses. The wider heroes that
    # WOULD need more fold to zero everywhere (900 m+ spacing is past
    # per-mille resolution), are absent from the world, and export_banks skips
    # their banks -- paying dilation for species that cannot appear is paying
    # for nothing.
    Layer(cell_mm=24_000, max_height_mm=60_000, max_depth_mm=8_000,
          max_radius_mm=15_000, density_per_mille=60, seed_count=4,
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
    Layer(cell_mm=800, max_height_mm=30_000, max_depth_mm=2_000,
          max_radius_mm=2_500, density_per_mille=1000, seed_count=4,
          terrain_lattice=False),
)


@dataclass
class ExportReport:
    """Everything the export refused or bent, BY NAME. The signature failure
    this project keeps paying for is the silent no-op; an exporter that files
    828 species and says nothing is one."""
    species: int = 0
    underserved_spacing: list[tuple[str, float, float]] = field(default_factory=list)
    too_rare_to_express: list[tuple[str, int, float]] = field(default_factory=list)
    unplaceable: list[tuple[str, str]] = field(default_factory=list)
    no_biome: list[str] = field(default_factory=list)

    def lines(self) -> list[str]:
        out = [f"manifest: {self.species} species"]
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
    if kind not in KINDS_TERRAIN:
        return 3  # ground cover: the detail lattice
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
    spacing_m = float(sm.get(spec, "placement.spacing_m"))
    layer = assign_layer(kind, nominal_height_m(spec, kind), report, name, spacing_m)
    if layer < 0:
        return None

    weights = []
    any_weight = False
    for b in BIOME_ORDER:
        w = sm.get(spec, f"biomes.{b}")
        pm = _per_mille(w if w is not None else 0.0)
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
        1 if kind in KINDS_TERRAIN else 0,
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
        0,  # slope_min: placement.slope_min_pct does not exist in the spec yet
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


def encode(specs: "list[tuple[str, dict]]",
           seeds_baked: "dict[str, int] | None" = None,
           report: ExportReport | None = None) -> bytes:
    """The manifest for a list of (name, validated spec body), sorted by name
    so the byte stream is a deterministic function of the spec set."""
    report = report if report is not None else ExportReport()
    seeds_baked = seeds_baked or {}

    records = []
    for name, spec in sorted(specs, key=lambda t: t[0]):
        rec = species_record(spec, name, int(seeds_baked.get(name, 0)), report)
        if rec is not None:
            records.append(rec)
    report.species = len(records)

    header = MAGIC + struct.pack(
        "<IIIIIII", MANIFEST_VERSION, len(BIOME_ORDER), len(LAYERS),
        len(records), SPECIES_RECORD_BYTES, 0, 0)
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

    return header + biome_block + layer_block + b"".join(records)


def decode(blob: bytes) -> dict:
    """Round-trip check only -- the PRODUCTION reader is voxelcore's, and this
    one exists so the exporter's tests do not certify the exporter with the
    exporter."""
    if blob[:4] != MAGIC:
        raise ValueError("bad magic")
    version, biomes, layers, count, rec_bytes, _, _ = struct.unpack_from("<IIIIIII", blob, 4)
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
    return {"biomes": tuple(names), "layers": layer_rows, "species": species}
