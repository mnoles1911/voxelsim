"""Tests for the GPU-pod bring-up helpers in ``tools/``.

These cover the only two pieces of bring-up logic that are pure computation
and therefore testable WITHOUT a GPU: choosing a PyTorch wheel index from a
driver version, and choosing a launch origin from a land scan. Everything
else in the bootstrap path (pip, HF download, CUDA inference) needs a real
pod and is verified there, loudly, by the script itself.
"""

import json
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

import cuda_index  # noqa: E402
import pick_origin  # noqa: E402


# --------------------------------------------------------------------------
# cuda_index -- the bug this prevents is silent CPU inference at GPU prices
# --------------------------------------------------------------------------

SMI_128 = """\
Sat Jul 19 02:11:04 2026
+-----------------------------------------------------------------------------+
| NVIDIA-SMI 570.86.15    Driver Version: 570.86.15    CUDA Version: 12.8      |
|-------------------------------+----------------------+----------------------+
| 0  NVIDIA GeForce RTX 4090    On  | 00000000:01:00.0 Off |                  N/A |
+-----------------------------------------------------------------------------+
"""

SMI_NO_BANNER = "bash: nvidia-smi: command not found\n"


def test_parses_driver_cuda_version():
    assert cuda_index.parse_driver_cuda(SMI_128) == (12, 8)


def test_parses_missing_banner_as_none():
    assert cuda_index.parse_driver_cuda(SMI_NO_BANNER) is None
    assert cuda_index.parse_driver_cuda("") is None


def test_ladder_never_exceeds_driver():
    """A cuXYZ wheel above the driver's CUDA version is exactly the failure
    we hit on 2026-07-19 (cu130 wheel, 12.8 driver, is_available() False)."""
    for driver in [(11, 8), (12, 1), (12, 4), (12, 6), (12, 8)]:
        for url in cuda_index.ladder(driver):
            tag = url.rsplit("/", 1)[-1]
            major = int(tag[2:4])
            minor = int(tag[4:])
            assert (major, minor) <= driver, f"{tag} > driver {driver}"


def test_ladder_prefers_known_good_when_driver_allows():
    urls = cuda_index.ladder((12, 8))
    assert urls[0].endswith(cuda_index.KNOWN_GOOD)


def test_ladder_omits_known_good_when_driver_too_old():
    urls = cuda_index.ladder((12, 1))
    assert not any(u.endswith("cu124") for u in urls)
    assert urls[0].endswith("cu121")


def test_ladder_has_fallbacks_not_just_one_answer():
    """The whole point is self-healing: if the top pick still yields
    is_available() False, the caller walks down the list."""
    assert len(cuda_index.ladder((12, 8))) >= 3


def test_ladder_never_empty_for_ancient_driver():
    assert cuda_index.ladder((10, 0)) == [
        cuda_index.INDEX_URL.format(tag="cu118")
    ]


def test_ladder_entries_are_unique():
    urls = cuda_index.ladder((12, 8))
    assert len(urls) == len(set(urls))


# --------------------------------------------------------------------------
# pick_origin -- the bug this prevents is a 25-tile pregen of open ocean,
# or of a featureless inland plateau
# --------------------------------------------------------------------------

def _scan(tiles, stride=3):
    return {"seed": 1, "radius": 2, "stride": stride, "checkpoint_dir": "x",
            "tiles": [{"x": x, "y": y, "land": land, "min": lo, "max": hi}
                      for (x, y, land, lo, hi) in tiles]}


def test_rejects_all_ocean_scan():
    scan = _scan([(0, 0, 0.0, -4000, -8.9), (3, 0, 0.01, -3000, 5.0)])
    assert pick_origin.score_candidates(scan) == []


def test_ocean_origin_is_never_chosen():
    """Tile (0,0) of seed 20260719 is entirely underwater -- the exact case
    that made a naive origin-centred pregen produce 25 ocean tiles."""
    scan = _scan([(0, 0, 0.0, -4200, -8.9), (6, -3, 0.72, -120, 900)])
    ranked = pick_origin.score_candidates(scan)
    assert (ranked[0]["x"], ranked[0]["y"]) == (6, -3)


def test_prefers_coastal_over_pure_inland():
    """Max land fraction is the WRONG objective: 100% land means no coast."""
    scan = _scan([(0, 0, 1.00, 300, 800), (3, 0, 0.70, -60, 800)])
    ranked = pick_origin.score_candidates(scan)
    assert (ranked[0]["x"], ranked[0]["y"]) == (3, 0)


def test_lone_peak_loses_to_broad_coast():
    """High relief + low land fraction is a spire in the sea, not a spawn."""
    scan = _scan([
        (0, 0, 0.15, -3000, 2400),   # lone peak, huge relief
        (3, 0, 0.68, -80, 600),      # broad coastline, modest relief
    ])
    ranked = pick_origin.score_candidates(scan)
    assert (ranked[0]["x"], ranked[0]["y"]) == (3, 0)


def test_neighbourhood_breaks_tie_against_isolated_island():
    """Two identical tiles; the one surrounded by land wins, because the
    pregen box around it is contiguous and will not run into open sea."""
    scan = _scan([
        (0, 0, 0.70, -50, 700),      # island: neighbours are ocean
        (3, 0, 0.05, -900, 2),
        (-3, 0, 0.02, -900, 1),
        (0, 3, 0.03, -900, 3),
        (0, -3, 0.01, -900, 2),
        (30, 0, 0.70, -50, 700),     # mainland: neighbours are land
        (33, 0, 0.80, -40, 800),
        (27, 0, 0.85, -30, 900),
        (30, 3, 0.75, -60, 750),
        (30, -3, 0.78, -20, 820),
    ])
    ranked = pick_origin.score_candidates(scan)
    assert (ranked[0]["x"], ranked[0]["y"]) == (30, 0)


def test_ranking_is_deterministic_under_tie():
    """Same scan must always name the same origin -- the origin ends up in a
    manifest and a cache namespace, so a coin-flip here is a real hazard."""
    tiles = [(0, 0, 0.70, -50, 700), (3, 3, 0.70, -50, 700)]
    a = pick_origin.score_candidates(_scan(tiles))
    b = pick_origin.score_candidates(_scan(list(reversed(tiles))))
    assert (a[0]["x"], a[0]["y"]) == (b[0]["x"], b[0]["y"])


def test_cli_prints_only_the_origin(tmp_path, capsys, monkeypatch):
    """generate_world.sh does ORIGIN=$(pick_origin ...), so stdout must be
    the bare coordinate and nothing else."""
    p = tmp_path / "scan.json"
    p.write_text(json.dumps(_scan([(0, 0, 0.0, -4000, -9),
                                   (6, -3, 0.70, -80, 700)])))
    monkeypatch.setattr(sys, "argv", ["pick_origin.py", str(p), "--explain"])
    assert pick_origin.main() == 0
    assert capsys.readouterr().out.strip() == "6,-3"


def test_cli_fails_loudly_on_all_ocean(tmp_path, capsys, monkeypatch):
    p = tmp_path / "scan.json"
    p.write_text(json.dumps(_scan([(0, 0, 0.0, -4000, -9)])))
    monkeypatch.setattr(sys, "argv", ["pick_origin.py", str(p)])
    assert pick_origin.main() == 1
    assert capsys.readouterr().out.strip() == ""
