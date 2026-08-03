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


# --------------------------------------------------------------------------
# bootstrap_pod.sh's conditioning gate
#
# These read the SCRIPT TEXT, which is a blunt instrument and is chosen
# deliberately. The gate cannot be unit tested any other way -- it needs a pod,
# a GPU and 25 MB of rasters -- and the specific regressions below are the ones
# that already happened once each. A grep that fails loudly when someone
# reintroduces `|| true` is worth more than no check at all.
# --------------------------------------------------------------------------

BOOTSTRAP = (Path(__file__).resolve().parent.parent / "tools" / "bootstrap_pod.sh").read_text(
    encoding="utf-8"
)


def test_bootstrap_verifies_conditioning_against_the_pins():
    assert "tools/fetch_conditioning.py" in BOOTSTRAP


def test_the_conditioning_gate_is_not_swallowed():
    """The gate is the whole fix. `|| true` on it, or `|| warn`, would restore
    exactly the 2026-08-03 behaviour: a pod that reports success and then
    generates a second planet under the first one's seed."""
    gate_lines = [
        ln for ln in BOOTSTRAP.splitlines()
        if "fetch_conditioning.py --verify-only" in ln
    ]
    assert gate_lines, "the verifying gate call disappeared"
    for ln in gate_lines:
        assert "|| true" not in ln
        assert "|| warn" not in ln


def test_bootstrap_does_not_build_the_etopo_raster_as_a_normal_step():
    """fetch_etopo.py now requires --i-am-starting-a-new-world. Bootstrap must
    not pass it: building the raster is how the drift got in, and a bring-up
    script is exactly the place where 'just make it work' wins arguments."""
    for ln in BOOTSTRAP.splitlines():
        stripped = ln.strip()
        if stripped.startswith("#"):
            continue
        if not stripped.startswith("python3 tools/fetch_etopo.py"):
            continue
        # The one allowed mention is inside a die() message telling a human how
        # to deliberately start a new world. An actual invocation would not
        # carry the acknowledgement flag, because bootstrap cannot acknowledge
        # anything on the operator's behalf.
        assert "--i-am-starting-a-new-world" in stripped, (
            "bootstrap is building etopo again; it must fetch the pinned artifact"
        )


def test_bootstrap_still_refuses_to_recommend_the_override():
    """--provider-id-override would force two different planets into one
    namespace with a seam and no error. The script may name it; it must not
    suggest reaching for it."""
    assert "Do NOT reach for --provider-id-override" in BOOTSTRAP


# --------------------------------------------------------------------------
# fetch_conditioning -- the fetch path itself, offline
#
# Exercised over file:// URLs so the two rules that matter are asserted rather
# than trusted: bytes are hashed BEFORE they are installed, and a file that is
# already present and wrong is never replaced. Both were learned the expensive
# way -- see docs/measurements/world-identity-not-reproducible-2026-08-03.txt.
# --------------------------------------------------------------------------

import hashlib  # noqa: E402
import zipfile  # noqa: E402

import fetch_conditioning as fc  # noqa: E402


def _file_url(p: Path) -> str:
    return p.resolve().as_uri()


def test_a_plain_url_source_is_installed_only_after_it_hashes_right(tmp_path):
    body = b"the pinned bytes" * 10
    src = tmp_path / "served.bin"
    src.write_bytes(body)
    dest = tmp_path / "root" / "etopo_10m.tif"
    dest.parent.mkdir()

    f = fc.Fetcher()
    try:
        assert f.fetch(_file_url(src), dest, hashlib.sha256(body).hexdigest(), len(body))
    finally:
        f.close()
    assert dest.read_bytes() == body


def test_a_source_that_serves_the_wrong_bytes_installs_nothing(tmp_path):
    """The host is not trusted. A mutated release asset, a re-cut upstream zip
    and a truncated transfer are all this case, and all of them must leave the
    destination ABSENT rather than plausible."""
    src = tmp_path / "served.bin"
    src.write_bytes(b"not what was pinned")
    dest = tmp_path / "root" / "etopo_10m.tif"
    dest.parent.mkdir()

    f = fc.Fetcher()
    try:
        assert not f.fetch(_file_url(src), dest, "aa" * 32, 999)
    finally:
        f.close()
    assert not dest.exists(), "a rejected download was left on disk"
    # and no temp debris that a later run could mistake for progress
    assert list(dest.parent.iterdir()) == []


def test_a_zip_member_source_extracts_and_verifies_just_that_member(tmp_path):
    """WorldClim ships all 19 bio variables as one archive and forbids
    mirroring, so the pin addresses a member inside upstream's own zip."""
    want = b"bio_1 raster bytes"
    archive = tmp_path / "wc2.1_10m_bio.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("wc2.1_10m_bio_1.tif", want)
        z.writestr("wc2.1_10m_bio_4.tif", b"a different raster")
    dest = tmp_path / "root" / "wc2.1_10m_bio_1.tif"
    dest.parent.mkdir()

    f = fc.Fetcher()
    try:
        assert f.fetch(
            _file_url(archive) + "#wc2.1_10m_bio_1.tif",
            dest, hashlib.sha256(want).hexdigest(), len(want),
        )
    finally:
        f.close()
    assert dest.read_bytes() == want
    assert not (dest.parent / "wc2.1_10m_bio_4.tif").exists()


def test_the_archive_is_downloaded_once_per_run_not_once_per_member(tmp_path):
    """Four separate fetches of a 49.9 MB archive is 200 MB on every pod."""
    members = {f"m{i}.tif": f"body {i}".encode() for i in range(4)}
    archive = tmp_path / "wc2.1_10m_bio.zip"
    with zipfile.ZipFile(archive, "w") as z:
        for n, b in members.items():
            z.writestr(n, b)
    root = tmp_path / "root"
    root.mkdir()

    calls = []
    real_stream = fc._stream

    def counting_stream(url, into):
        calls.append(url)
        return real_stream(url, into)

    fc._stream = counting_stream
    f = fc.Fetcher()
    try:
        for n, b in members.items():
            assert f.fetch(
                _file_url(archive) + f"#{n}", root / n,
                hashlib.sha256(b).hexdigest(), len(b),
            )
    finally:
        f.close()
        fc._stream = real_stream

    assert len(calls) == 1, f"archive fetched {len(calls)} times"
    # and the cached archive is not left in the conditioning directory
    assert sorted(p.name for p in root.iterdir()) == sorted(members)


def test_a_missing_zip_member_fails_rather_than_installing_something_else(tmp_path):
    archive = tmp_path / "a.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("something_else.tif", b"x")
    dest = tmp_path / "root" / "wc2.1_10m_bio_1.tif"
    dest.parent.mkdir()

    f = fc.Fetcher()
    try:
        assert not f.fetch(
            _file_url(archive) + "#wc2.1_10m_bio_1.tif", dest, "aa" * 32, 1
        )
    finally:
        f.close()
    assert not dest.exists()


def test_a_present_but_wrong_file_is_never_overwritten_and_has_no_force_flag():
    """That file is usually one the box built for itself, and it is the ONLY
    evidence of what any tiles already in the cache were generated from.
    Overwriting it destroys the only way to identify them later, so the fetcher
    skips it, says so, and offers no flag to do otherwise -- the operator has to
    move it aside by hand, which is what keeps the evidence."""
    src = (Path(fc.__file__)).read_text(encoding="utf-8")
    assert "leaving it alone" in src
    for forcing in ("--force", "--overwrite", "--replace-wrong"):
        assert forcing not in src, f"{forcing} would let a bake erase its own provenance"
