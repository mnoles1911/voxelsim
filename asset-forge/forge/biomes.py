"""The world's biomes, mirrored from the engine.

MIRRORS `vxc::BiomeId` in `voxel-core/include/voxelcore/biome.h`. That enum is
documented append-only, so these ids are stable — but if a biome is ever added
there it must be added here too, and `forge.cli biomes` prints this table so the
two can be compared by eye.

Three of the ten are decided by TERRAIN, not climate, because `classifyBiome`
runs morphology gates before the Whittaker table: sea level first (OCEAN /
BEACH), then a 35-degree cliff gate (BARE_ROCK), then a temperature-adjusted
treeline (TUNDRA_ALPINE). Only what survives all three gates gets classified by
temperature and precipitation. That ordering matters to a tree designer: a
species tagged for TEMPERATE_FOREST will never appear on a cliff face or above
the treeline no matter how wet and mild the climate there is.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Biome:
    id: int
    key: str          # spec path fragment, e.g. biomes.temperate_forest
    label: str
    surface: str      # the material the engine paints its topsoil with
    climate: str      # what puts a column here, in plain terms
    plantable: bool   # can anything with roots stand here
    hosts: tuple[str, ...] = ()   # asset kinds that may occur here; () = none


# Every plantable biome hosts everything that grows. Kept as a name so the two
# unplantable entries below read as the deliberate exceptions they are.
_GROWS = ("tree", "bush", "rock", "grass", "reed", "flower")

# Fish are hosted by the biome of the LAND AROUND THE WATER, not by a water
# biome, because that is what the engine classifies: a river running through
# temperate forest is classified temperate forest, and only open sea below -3 m
# comes out as OCEAN. So a brown trout is a temperate-forest fish and a reef
# fish is an ocean one, and the biome weights on a fish spec mean "which
# landscape's rivers and lakes hold this species".
_SWIMS = _GROWS + ("fish", "cetacean")

# Birds go wherever anything else does AND in the two places nothing else can.
# A gull is an ocean species and a raven and a ptarmigan live on bare rock, so
# the two biomes that were carved out as exceptions -- one for having no ground
# and one for being too steep to stand on -- are both perfectly good bird
# habitat. They are the first kind for which that is true.
_FLIES = _SWIMS + ("bird",)

# Land animals go wherever anything that grows does, and nowhere else yet.
#
# THE TWO BIOMES LEFT OUT ARE LEFT OUT ON PURPOSE. Ocean is obvious. Bare rock
# is not, and `docs/biomes/README.md` §5 makes the case for widening it: the 35
# degree gate is the angle of repose FOR LOOSE MATERIAL, which is not the angle
# at which an ibex, a chamois or a mountain goat loses its footing, and those
# rows are currently blocked twice over. That case is now half-answered -- the
# generator exists -- but widening the tuple is a PLACEMENT decision about where
# animals may stand in the world, not a consequence of a generator landing, and
# it belongs to whoever owns placement. Left alone, and said so here rather than
# done quietly.
_WALKS = _FLIES + ("quadruped",)

BIOMES: tuple[Biome, ...] = (
    # OCEAN HOSTS SOMETHING NOW. It was empty because nothing that grows can
    # stand under the sea, and `plantable` said so; a fish is the first asset
    # that belongs there and the first reason `hosts` had to be separate from
    # `plantable` on this row rather than only on bare rock.
    # ROCK AND PLANTS ADDED (owner, 2026-08-15). `plantable` stays False and
    # that is the distinction this row exists to make: `plantable` means "a
    # land plant can root here", which is still no. `hosts` means "an asset of
    # this kind belongs here", and kelp, seagrass and a reef plainly do. The
    # ocean had 67 fish and nothing whatever for them to swim around, which is
    # 15 blocked rows across the biome lists and the emptiest water in the
    # world.
    Biome(0, "ocean", "Ocean", "mud",
          "below -3 m; decided before climate", False,
          ("fish", "cetacean", "bird", "rock", "grass", "reed", "bush")),
    Biome(1, "beach", "Beach", "sand",
          "-3 m to +4 m around sea level; decided before climate", True, _WALKS),
    Biome(2, "grassland", "Grassland", "grass",
          "dry and not hot, or semi-dry without a strong wet season", True, _WALKS),
    Biome(3, "temperate_forest", "Temperate forest", "topsoil",
          "moderate to wet, mild; the default forested band", True, _WALKS),
    Biome(4, "rainforest", "Rainforest", "jungle soil",
          "wet (>1600 mm/yr) and warm (>=18 C)", True, _WALKS),
    Biome(5, "desert", "Desert", "sand",
          "arid (<400 mm/yr) and hot (>=24 C)", True, _WALKS),
    Biome(6, "savanna", "Savanna", "savanna grass",
          "warm with a strongly seasonal wet season", True, _WALKS),
    Biome(7, "taiga", "Taiga", "podzol",
          "cold: mean annual temperature below 5 C", True, _WALKS),
    Biome(8, "tundra_alpine", "Tundra / alpine", "permafrost or rock",
          "above the treeline (900 m at 0 C, +150 m per degree)", True, _WALKS),
    # Not plantable, but very much not empty. The cliff gate fires before the
    # climate table, so this is every steep face in the world -- exactly where
    # boulders and scree belong, and the reason `hosts` exists separately from
    # `plantable` rather than reusing it.
    Biome(9, "bare_rock", "Bare rock", "rock",
          "ground steeper than a 70% grade (~35 degrees)", False,
          # QUADRUPEDS ADDED (owner, 2026-08-15). The comment above this row
          # said the question was "where animals may stand in the world, not a
          # consequence of a generator landing" -- and it is still not a
          # consequence, it is a decision that has now been made. An ibex and a
          # chamois stand on ground far steeper than 35 degrees; that is the
          # whole point of them.
          # AND PLANTS (owner, 2026-08-15): "bare rock should host moss, lichen
          # and vines at times". `plantable` stays False and that is still the
          # distinction -- plantable means the climate-and-soil table will
          # AUTO-PLANT here, which a cliff face should not; `hosts` means an
          # asset of this kind may be placed here, and a moss cushion on a rock
          # face plainly may.
          #
          # grass carries the mosses, flower the crevice herbs, bush the vines
          # and the woody things that get a roothold in a crack. There is no
          # `lichen` kind yet -- it is on the blocked list in
          # docs/biomes/README.md section 9, wanting a crust primitive that
          # nothing here has -- so the owner's third example is named and not
          # yet servable.
          ("rock", "bird", "quadruped", "grass", "flower", "bush")),
)

BY_KEY = {b.key: b for b in BIOMES}
PLANTABLE = tuple(b for b in BIOMES if b.plantable)
HOSTING = tuple(b for b in BIOMES if b.hosts)


def for_kind(kind: str) -> tuple[Biome, ...]:
    """Biomes an asset kind can occur in."""
    return tuple(b for b in BIOMES if kind in b.hosts)


def weight_paths() -> tuple[str, ...]:
    return tuple(f"biomes.{b.key}" for b in HOSTING)


def weights(spec: dict) -> dict[str, float]:
    """Biome -> weight for a species, dropping the zeroes."""
    from .spec import get

    out = {}
    for b in HOSTING:
        w = float(get(spec, f"biomes.{b.key}") or 0.0)
        if w > 0:
            out[b.key] = w
    return out


def allowed(spec: dict) -> tuple[str, ...]:
    """THE ONE ALLOWLIST RESOLVER: which biomes this species may appear in.

    A species may be allowed in one, several, or NO biomes (owner directive,
    2026-08-18). Explicit form: a top-level `biome_allow` list of biome keys
    in the spec ([] means "allowed nowhere", deliberately). Absent, the
    allowlist is DERIVED from the weights -- exactly the biomes with weight
    above zero -- which is what every existing spec already meant by its
    zeroes, so a spec that never authors the block keeps today's effective
    biome set bit for bit. Either way the result is clipped to the biomes
    that HOST the species' kind, because a weight row a biome does not host
    was never authorable in the first place.

    The exporter enforces the explicit form: a weight above zero OUTSIDE an
    authored allowlist is zeroed at export and reported by name, so the
    allowlist is authoritative and auditable rather than advisory.

    One function, two callers by design -- the exporter and the library
    report -- so "which species are allowed in the desert" has exactly one
    answer everywhere.
    """
    from .spec import get

    kind = spec.get("kind")
    hosts = tuple(b.key for b in BIOMES if kind in b.hosts)
    explicit = spec.get("biome_allow")
    if isinstance(explicit, list):
        return tuple(k for k in hosts if k in explicit)
    return tuple(k for k in hosts if float(get(spec, f"biomes.{k}") or 0.0) > 0)


def is_explicit_allowlist(spec: dict) -> bool:
    """Did a human author the allowlist, or is it derived from the weights?"""
    return isinstance(spec.get("biome_allow"), list)


def summary(spec: dict) -> str:
    w = weights(spec)
    if not w:
        return "unassigned"
    ranked = sorted(w.items(), key=lambda kv: -kv[1])
    return ", ".join(f"{BY_KEY[k].label} {v:.0%}" for k, v in ranked)
