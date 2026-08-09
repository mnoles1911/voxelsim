"""Generate the SECTION_BASIN_TABLE conformance fixture.

WHY A FIXTURE AND NOT JUST A ROUND-TRIP TEST. The Python encoder and the C++
decoder are written independently against docs/vxtl-v2-format.md, and a
round-trip test in either language proves only that that language agrees with
itself. A committed file both sides parse is the only thing that catches a
disagreement -- and §3's "unknown flag bits are rejected" makes such a
disagreement a hard refusal at load rather than a subtle rendering fault,
which is why the two halves must move in lockstep.

The table it writes is chosen to break a lazy decoder, not to be typical:

* **All five kinds appear.** A decoder that treats `kind` as a bool ("water /
  no water") passes an all-lake fixture.
* **A NEGATIVE spill and surface.** Elevations are i32 millimetres and the
  bake's own filter refuses basins at or below sea level, so no production
  tile will carry one -- which is exactly why the fixture must, or the sign
  handling is never exercised until the first below-sea world.
* **surface == spill, surface < spill, and surface == the basin floor**, i.e.
  overflowing, terminal, and bone dry.
* **A basin whose bbox touches the last pixel of the tile** (511), so an
  off-by-one in the bounds check fails here rather than in the field.
* **A one-pixel basin**, where x0 == x1 and y0 == y1.
* **An id sequence that is exactly 0..n-1**, which both decoders enforce --
  the client indexes by id and the bake orders by (min_y, min_x) of extent, so
  a gap would let two processes disagree about which basin is "3".

Run:
    python tools/make_basin_fixture.py
It rewrites voxel-core/tests/fixtures/vxtl_v2_golden_basins_512.vxtl in place.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402

OUT = (Path(__file__).resolve().parents[2]
       / "voxel-core" / "tests" / "fixtures" / "vxtl_v2_golden_basins_512.vxtl")

SIZE = 512
BLOCK_LOG2 = 8

#: Ordered by (min_y, min_x) of extent, exactly as the bake orders them, so
#: the fixture also demonstrates the ordering rule rather than only the bytes.
BASINS = [
    # id 0: overflowing lake -- surface EXACTLY at the spill.
    tc.BasinEntry(basin_id=0, seed_px=(40, 12), bbox_px=(30, 10, 60, 40),
                  outlet_px=(29, 25), spill_mm=1_234_500,
                  surface_mm=1_234_500, kind=tc.BASIN_KIND_LAKE_OVERFLOWING),
    # id 1: terminal lake -- stands below its own outlet.
    tc.BasinEntry(basin_id=1, seed_px=(200, 130), bbox_px=(180, 120, 240, 175),
                  outlet_px=(179, 150), spill_mm=880_000,
                  surface_mm=863_250, kind=tc.BASIN_KIND_LAKE_TERMINAL),
    # id 2: seasonal, and BELOW SEA LEVEL. No bake will emit this (the
    # registry refuses a spill at or under the datum), which is precisely why
    # it is here: it is the only exercise the i32 sign gets.
    tc.BasinEntry(basin_id=2, seed_px=(300, 300), bbox_px=(300, 300, 300, 300),
                  outlet_px=(299, 300), spill_mm=-2_500,
                  surface_mm=-7_300, kind=tc.BASIN_KIND_SEASONAL),
    # id 3: salt flat -- dry, so surface sits at the floor.
    tc.BasinEntry(basin_id=3, seed_px=(100, 400), bbox_px=(90, 390, 130, 430),
                  outlet_px=(89, 410), spill_mm=57_000,
                  surface_mm=50_100, kind=tc.BASIN_KIND_SALT_FLAT),
    # id 4: dry playa, reaching the LAST pixel of the tile on both axes.
    tc.BasinEntry(basin_id=4, seed_px=(500, 500), bbox_px=(480, 480, 511, 511),
                  outlet_px=(479, 495), spill_mm=2_100_000,
                  surface_mm=2_093_400, kind=tc.BASIN_KIND_DRY_PLAYA),
]


def samples() -> np.ndarray:
    """A gentle, fully deterministic surface. The elevation plane is not what
    this fixture is about -- it exists so the file is a real tile rather than
    a bare table -- so it is smooth enough to compress and integer-exact."""
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
    return (100.0 + 0.5 * x + 0.25 * y
            + 20.0 * np.sin(x / 47.0) * np.cos(y / 61.0))


def main() -> int:
    flow = np.zeros((SIZE, SIZE), np.uint8)
    # Non-trivial flow, so the fixture also proves the basin table coexists
    # with an optional section rather than only with the required ones.
    flow[:, 200:210] = tc.FLOW_BIT_CHANNEL | 17
    flow[300:310, :] = tc.FLOW_BIT_BANK | 9

    data = tc.encode_fine(
        seed=20260719, x=-2, y=-4,
        elevation_m=samples(),
        flow=flow,
        basins=BASINS,
        bake_ver=8,
        block_log2=BLOCK_LOG2,
        # PINNED TO v1, and this file stays a v1 fixture forever. v2 arrived at
        # bake_ver 24 and has its own generator (make_basin_v2_fixture.py); the
        # compatibility claim -- that tiles already on disk in shipped
        # namespaces keep decoding -- is worth nothing if the artefact that
        # tests it silently follows the encoder's current default.
        basin_table_version=tc.BASIN_TABLE_VERSION_V1,
    )
    # Prove it before writing it: a fixture that does not round-trip through
    # its own encoder cannot be evidence about anyone else's decoder.
    back = tc.decode_v2(data)
    assert back.basins is not None and len(back.basins) == len(BASINS)
    for want, got in zip(BASINS, back.basins):
        assert want == got, f"round trip changed basin {want.basin_id}"
    kinds = {b.kind for b in BASINS}
    assert kinds == set(range(tc.BASIN_KIND_COUNT)), (
        f"the fixture must exercise every kind; it has {sorted(kinds)}")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    print(f"wrote {OUT} ({len(data)} bytes, {len(BASINS)} basins)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
