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

BIOMES: tuple[Biome, ...] = (
    # OCEAN HOSTS SOMETHING NOW. It was empty because nothing that grows can
    # stand under the sea, and `plantable` said so; a fish is the first asset
    # that belongs there and the first reason `hosts` had to be separate from
    # `plantable` on this row rather than only on bare rock.
    Biome(0, "ocean", "Ocean", "mud",
          "below -3 m; decided before climate", False, ("fish", "cetacean", "bird")),
    Biome(1, "beach", "Beach", "sand",
          "-3 m to +4 m around sea level; decided before climate", True, _FLIES),
    Biome(2, "grassland", "Grassland", "grass",
          "dry and not hot, or semi-dry without a strong wet season", True, _FLIES),
    Biome(3, "temperate_forest", "Temperate forest", "topsoil",
          "moderate to wet, mild; the default forested band", True, _FLIES),
    Biome(4, "rainforest", "Rainforest", "jungle soil",
          "wet (>1600 mm/yr) and warm (>=18 C)", True, _FLIES),
    Biome(5, "desert", "Desert", "sand",
          "arid (<400 mm/yr) and hot (>=24 C)", True, _FLIES),
    Biome(6, "savanna", "Savanna", "savanna grass",
          "warm with a strongly seasonal wet season", True, _FLIES),
    Biome(7, "taiga", "Taiga", "podzol",
          "cold: mean annual temperature below 5 C", True, _FLIES),
    Biome(8, "tundra_alpine", "Tundra / alpine", "permafrost or rock",
          "above the treeline (900 m at 0 C, +150 m per degree)", True, _FLIES),
    # Not plantable, but very much not empty. The cliff gate fires before the
    # climate table, so this is every steep face in the world -- exactly where
    # boulders and scree belong, and the reason `hosts` exists separately from
    # `plantable` rather than reusing it.
    Biome(9, "bare_rock", "Bare rock", "rock",
          "ground steeper than a 70% grade (~35 degrees)", False,
          ("rock", "bird")),
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


def summary(spec: dict) -> str:
    w = weights(spec)
    if not w:
        return "unassigned"
    ranked = sorted(w.items(), key=lambda kv: -kv[1])
    return ", ".join(f"{BY_KEY[k].label} {v:.0%}" for k, v in ranked)
