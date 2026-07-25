"""The climate wire encoding has two copies. This test fails when they diverge.

``EXPECTED_CHANNELS`` in ``terrain_service/providers/diffusion.py`` is both the
model-output validation range AND the quantization range
``adapt_raster_to_tile`` maps into uint8 -- so those four (min, max) pairs ARE
the wire format. ``voxel-core/include/voxelcore/climate.h`` is the consumer's
copy, and every biome threshold is derived from it at compile time.

Before climate.h existed, voxel-core did not know the encoding at all
(``tiles.h`` said only "service-defined encoding, see terrain-service"), and
the thresholds were calibrated against a different one. This test is what stops
that from happening again silently: change either side and it fails here,
naming both values.

The units deliberately differ -- climate.h uses integers throughout because
voxel-core bans floats, so temperature is milli-degrees and precipitation
variability is tenths of a percent. ``_SCALE`` records the conversion, and it
is part of the contract too.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from terrain_service.providers.diffusion import EXPECTED_CHANNELS

_CLIMATE_H = (
    Path(__file__).resolve().parents[2]
    / "voxel-core"
    / "include"
    / "voxelcore"
    / "climate.h"
)

#: our semantic channel name -> (climate.h min constant, max constant, how many
#: of climate.h's integer units make ONE of diffusion.py's physical units)
_CONTRACT: dict[str, tuple[str, str, int]] = {
    "temperature": ("kClimateTempMinMilliC", "kClimateTempMaxMilliC", 1000),
    "seasonality": ("kClimateSeasonalityMin", "kClimateSeasonalityMax", 1),
    "precipitation": ("kClimatePrecipMinMmPerYr", "kClimatePrecipMaxMmPerYr", 1),
    "precip_variability": (
        "kClimatePrecipVarMinDeciPct",
        "kClimatePrecipVarMaxDeciPct",
        10,
    ),
}


def _parse_climate_h() -> dict[str, int]:
    """Extract every `inline constexpr int64_t kClimate... = <int>;` value.

    Deliberately a regex over the text rather than a build-and-run of a C++
    helper: this test has to work in the terrain-service pytest job, which has
    no C++ toolchain. The C++ side's own correctness is covered by climate.h's
    static_asserts and test_climate.cpp.
    """
    if not _CLIMATE_H.is_file():
        pytest.skip(f"climate.h not found at {_CLIMATE_H} (running outside the repo?)")
    text = _CLIMATE_H.read_text(encoding="utf-8")
    pattern = re.compile(
        r"^inline\s+constexpr\s+int64_t\s+(kClimate\w+)\s*=\s*(-?[\d']+)\s*;",
        re.MULTILINE,
    )
    return {m.group(1): int(m.group(2).replace("'", "")) for m in pattern.finditer(text)}


def test_climate_h_defines_every_contract_constant() -> None:
    found = _parse_climate_h()
    missing = [
        name
        for lo, hi, _ in _CONTRACT.values()
        for name in (lo, hi)
        if name not in found
    ]
    assert not missing, (
        f"climate.h no longer defines {missing} -- the constants terrain-service's "
        "wire format is mirrored by were renamed or removed. Update _CONTRACT here "
        "in the same commit."
    )


@pytest.mark.parametrize("channel", sorted(_CONTRACT))
def test_climate_h_ranges_match_expected_channels(channel: str) -> None:
    found = _parse_climate_h()
    spec = next(c for c in EXPECTED_CHANNELS if c.name == channel)
    lo_name, hi_name, scale = _CONTRACT[channel]
    cpp_lo = found[lo_name] / scale
    cpp_hi = found[hi_name] / scale
    assert (cpp_lo, cpp_hi) == (spec.min, spec.max), (
        f"WIRE FORMAT DIVERGENCE on {channel!r}: climate.h says "
        f"[{cpp_lo}, {cpp_hi}] ({lo_name}={found[lo_name]}, {hi_name}={found[hi_name]}, "
        f"scale {scale}), diffusion.py EXPECTED_CHANNELS says [{spec.min}, {spec.max}]. "
        "These quantize and de-quantize the same bytes, so a mismatch silently "
        "shifts every biome threshold. If the change is intentional it rolls the "
        "provider_id (via _tile_format_fingerprint) and must be made on BOTH sides."
    )


def test_elevation_is_not_part_of_the_climate_contract() -> None:
    """Elevation rides the tile as int16 metres, not as a quantized u8 channel.

    Guards against someone "completing" _CONTRACT by adding elevation and
    inventing a climate.h range for it -- there is deliberately none.
    """
    assert "elevation" not in _CONTRACT
    assert any(c.name == "elevation" for c in EXPECTED_CHANNELS)


def test_every_expected_channel_is_covered() -> None:
    """A NEW channel in diffusion.py must gain a climate.h range, not be ignored."""
    climate_channels = {c.name for c in EXPECTED_CHANNELS} - {"elevation"}
    assert climate_channels == set(_CONTRACT), (
        f"EXPECTED_CHANNELS climate channels {sorted(climate_channels)} do not match "
        f"the contract {sorted(_CONTRACT)} -- a channel was added or renamed on one "
        "side only."
    )
