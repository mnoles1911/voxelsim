"""Sea level is ONE symbol. This test is what makes that true.

``voxelcore/core.h`` declares ``kSeaLevelMm``. Four other places carry a copy
of the same datum and cannot include a C++ header:

* ``voxel-core/shaders/worldgen.ush`` -- HLSL, hand-mirrored (there is no
  generator; ``vxc_dump_biome_constants`` prints the values and a human pastes
  them, which the shader's own comment says in capitals).
* ``terrain_service/bake/incise.py`` and ``bake/pipeline.py`` -- the fluvial
  incision sea taper, whose TOP is sea level and which is duplicated between
  the two files.
* ``tools/world_map.py`` -- the reader every world map's waterline comes from.

CI has no C++ toolchain (``test_climate_contract.py`` records the same
constraint), so the mirror cannot be checked by compiling. It is checked by
parsing, exactly as the climate contract is.

WHY THIS EXISTS AT ALL. Before ``kSeaLevelMm`` the datum was **21 unnamed
literal zeros across five languages**, three disagreeing named constants, and
one consumer -- ``world_map.py`` -- silently drawing every published world map
at -3.0 m because it reused the beach band's lower edge as the waterline. None
of that was greppable. A divergence now fails here, by name.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
CORE_H = REPO / "voxel-core" / "include" / "voxelcore" / "core.h"
WORLDGEN_USH = REPO / "voxel-core" / "shaders" / "worldgen.ush"


def _plain_int(src: str, const: str, where: str) -> int:
    m = re.search(rf"\b{const}\s*=\s*(-?[\d']+)\s*;", src)
    assert m, f"{where} no longer defines {const} as a plain integer literal"
    return int(m.group(1).replace("'", ""))


@pytest.fixture(scope="module")
def core_src() -> str:
    if not CORE_H.is_file():
        pytest.skip(f"{CORE_H} not present (terrain-service checked out alone)")
    return CORE_H.read_text(encoding="utf-8")


def test_core_h_declares_sea_level(core_src):
    """The symbol exists, in millimetres, and the world is built on zero."""
    assert _plain_int(core_src, "kSeaLevelMm", "core.h") == 0


def test_voxel_lattice_twin_is_derived_not_written_twice(core_src):
    """``kSeaLevelVoxelZ`` must be an EXPRESSION over kSeaLevelMm, not a literal.

    A second literal is how the three pre-existing sea-level constants came to
    disagree on units. The static_assert beside it is what refuses a datum
    that is not a whole number of voxels.
    """
    m = re.search(r"kSeaLevelVoxelZ\s*=\s*([^;]+);", core_src)
    assert m, "core.h no longer defines kSeaLevelVoxelZ"
    expr = m.group(1).strip()
    assert "kSeaLevelMm" in expr and "kVoxelSizeMm" in expr, (
        f"kSeaLevelVoxelZ must be derived from kSeaLevelMm / kVoxelSizeMm, "
        f"got '{expr}'")
    assert "static_assert" in core_src.split("kSeaLevelVoxelZ", 1)[1][:300], (
        "the static_assert tying kSeaLevelVoxelZ back to kSeaLevelMm is gone")


def test_no_bare_sea_level_constants_left_in_voxel_core(core_src):
    """The three old spellings must now DERIVE, not re-declare.

    ``caves.h``'s kCaveMinVoxelZ, ``rivercouple.h``'s seaLevelVz default and
    ``rivernet.h``'s kRiverSeaLevelMm were each an independent ``= 0``.
    kRiverSeaLevelMm had zero references anywhere in the tree while
    ``channel.cpp`` -- in the same subsystem -- tested against a bare 0 twice.
    """
    checks = {
        "caves.h": ("kCaveMinVoxelZ", "kSeaLevelVoxelZ"),
        "rivercouple.h": ("seaLevelVz", "kSeaLevelVoxelZ"),
        "rivernet.h": ("kRiverSeaLevelMm", "kSeaLevelMm"),
    }
    for header, (const, must_reference) in checks.items():
        p = REPO / "voxel-core" / "include" / "voxelcore" / header
        if not p.is_file():
            pytest.skip(f"{p} not present")
        src = p.read_text(encoding="utf-8")
        m = re.search(rf"\b{const}\s*=\s*([^;]+);", src)
        assert m, f"{header} no longer defines {const}"
        assert must_reference in m.group(1), (
            f"{header}'s {const} is '{m.group(1).strip()}' -- it must derive "
            f"from core.h's {must_reference} rather than restate the datum")


def test_hlsl_mirror_agrees(core_src):
    """worldgen.ush is a hand-paste. This is the only thing checking it."""
    if not WORLDGEN_USH.is_file():
        pytest.skip(f"{WORLDGEN_USH} not present")
    ush = WORLDGEN_USH.read_text(encoding="utf-8")
    assert _plain_int(ush, "kSeaLevelMm", "worldgen.ush") == _plain_int(
        core_src, "kSeaLevelMm", "core.h")
    assert _plain_int(ush, "kSeaLevelVoxelZ", "worldgen.ush") == (
        _plain_int(core_src, "kSeaLevelMm", "core.h")
        // _plain_int(core_src, "kVoxelSizeMm", "core.h"))


def test_bake_sea_taper_top_is_sea_level(core_src):
    """The fluvial incision taper's TOP is sea level, in two duplicated places.

    ``incise.py`` and ``pipeline.py`` each carry the pair independently (the
    audit found them; a third cited copy turned out to be a duplicated
    *function* reading the named constants, not a third literal). Neither can
    import a C++ header, so equality is asserted rather than derived.
    """
    from terrain_service.bake import incise
    from terrain_service.bake import pipeline as bp

    sea_m = _plain_int(core_src, "kSeaLevelMm", "core.h") / 1000.0
    assert incise.SEA_TAPER_TOP_M == sea_m
    assert bp.CONSTANTS.sea_taper_top_m == sea_m
    # And the two Python copies must agree with each other, which is the
    # failure mode a single-sided check would miss.
    assert bp.CONSTANTS.sea_taper_top_m == incise.SEA_TAPER_TOP_M
    assert bp.CONSTANTS.sea_taper_bottom_m == incise.SEA_TAPER_BOTTOM_M


def test_missing_elevation_is_sea_level(core_src):
    """A coarse tile that does not exist reads as sea, on BOTH sides.

    ``pipeline.MISSING_ELEVATION_M`` and ``tilestore.cpp``'s missing-tile /
    missing-block returns are the same datum choice: absent data is open
    ocean. They were three separate zeros.
    """
    from terrain_service.bake import pipeline as bp

    assert bp.MISSING_ELEVATION_M == _plain_int(core_src, "kSeaLevelMm", "core.h") / 1000.0


def test_world_map_draws_the_waterline_at_sea_level():
    """The regression that produced every published map's wrong coastline.

    ``compose()`` defaulted its waterline to ``k["beach_lower_m"]`` -- the
    BOTTOM of the beach band, -3.0 m -- so maps flooded a strip of land on
    every coast. The two must now be different numbers, and the waterline must
    be the sea.
    """
    import sys

    sys.path.insert(0, str(REPO / "terrain-service" / "tools"))
    try:
        import world_map
    except SystemExit as e:  # _read_constants exits if a header moved
        pytest.fail(f"world_map could not read its constants: {e}")
    k = world_map._read_constants()
    assert "sea_level_m" in k, "world_map no longer reads kSeaLevelMm from core.h"
    assert k["sea_level_m"] == 0.0
    assert k["beach_lower_m"] < k["sea_level_m"], (
        "the beach band's lower edge must sit BELOW sea level; if they are "
        "equal the regression this test guards cannot be detected")
