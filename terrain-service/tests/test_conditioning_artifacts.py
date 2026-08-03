"""The conditioning files are pinned bytes, and the pin has to be the same
construction ``compute_conditioning_digest`` uses -- or the pin is decoration.

These are pure-function tests in the style of ``test_pregen``'s
``world_identity_verdict`` block and ``superblock_gate_verdict``: no GPU, no
network, no 25 MB of rasters. The policy they cover is the one that failed on
the fresh pod, phrased as an assertion instead of a comment: a box that does
not hold the pinned bytes must be told which file moved and must not proceed.
"""

import hashlib
import json
from pathlib import Path

import pytest

from terrain_service.conditioning_artifacts import (
    WRONG_BYTES,
    ArtifactPin,
    ArtifactPins,
    ConditioningPinsError,
    digest_from_pins,
    load_pins,
    verdict_from_observations,
    verify_root,
)
from terrain_service.providers.diffusion import (
    DEFAULT_CONDITIONING_FILES,
    compute_conditioning_digest,
)


# --------------------------------------------------------------------------
# the shipped manifest
# --------------------------------------------------------------------------


def test_shipped_manifest_loads():
    pins = load_pins()
    assert len(pins.pins) == len(DEFAULT_CONDITIONING_FILES)


def test_shipped_manifest_pins_exactly_the_files_that_condition_generation():
    """A pin set that is not DEFAULT_CONDITIONING_FILES is checking the wrong
    world. Drift in either direction is a bug: an unpinned conditioning file is
    an unchecked input, and a pinned non-conditioning file is a check that will
    one day fail for a reason that does not matter and get switched off."""
    assert set(load_pins().names) == set(DEFAULT_CONDITIONING_FILES)


def test_shipped_manifest_expected_digest_matches_its_own_pins():
    """The digest written in the JSON is a CROSS-CHECK of the derivation, not a
    second source of truth. Editing a sha256 without updating it fails here
    rather than in a bake."""
    pins = load_pins()
    assert pins.expected_conditioning_digest == digest_from_pins(pins)


def test_the_two_built_artifacts_are_marked_as_not_reproducible_by_their_builder():
    """Measured 2026-08-02: etopo_10m.tif was never a build output (it is
    uncompressed and carries ETOPO's own vertical datum, neither of which
    fetch_etopo.py can emit) and synthetic_map_stats.json is derived from it.
    If anyone ever flips these to True, they owe a measurement."""
    by_name = load_pins().by_name()
    assert by_name["etopo_10m.tif"].builder_reproduces_pin is False
    assert by_name["synthetic_map_stats.json"].builder_reproduces_pin is False
    for wc in ("wc2.1_10m_bio_1.tif", "wc2.1_10m_bio_4.tif",
               "wc2.1_10m_bio_12.tif", "wc2.1_10m_bio_15.tif"):
        assert by_name[wc].builder_reproduces_pin is True


def test_hosting_decision_is_recorded_as_open_while_the_urls_are_empty():
    """The two built artifacts have no sources yet. That is a DECISION NOT MADE,
    and it must stay visible: an empty source list with no explanation reads as
    an oversight and gets 'fixed' by whoever is in a hurry."""
    pins = load_pins()
    by_name = pins.by_name()
    unhosted = [n for n, p in by_name.items() if not p.sources and p.origin == "built"]
    if unhosted:
        h = pins.hosting_decision_required
        assert h, f"{unhosted} have no sources and no hosting_decision_required block"
        assert h.get("recommendation")
        assert "UNRESOLVED" in h.get("owner_decision", "")


# --------------------------------------------------------------------------
# digest_from_pins IS compute_conditioning_digest, without the files
# --------------------------------------------------------------------------


def _write(root: Path, name: str, body: bytes) -> str:
    (root / name).write_bytes(body)
    return hashlib.sha256(body).hexdigest()


def test_digest_from_pins_equals_compute_conditioning_digest(tmp_path):
    """The whole point of deriving the expected digest: bootstrap can state the
    number it is aiming for before it has downloaded a single byte. That is only
    safe while the two constructions agree, so they are asserted equal here
    rather than kept in step by comment."""
    root = tmp_path / "global"
    root.mkdir()
    pins = []
    for i, name in enumerate(DEFAULT_CONDITIONING_FILES):
        body = f"contents of {name} {i}".encode()
        sha = _write(root, name, body)
        pins.append(ArtifactPin(name=name, sha256=sha, size=len(body), origin="built"))
    ps = ArtifactPins(expected_conditioning_digest="", pins=tuple(pins))

    assert digest_from_pins(ps) == compute_conditioning_digest(root=root)


def test_digest_from_pins_is_order_independent(tmp_path):
    a = ArtifactPin("b.tif", "bb" * 32, 2, "built")
    b = ArtifactPin("a.tif", "aa" * 32, 1, "built")
    assert digest_from_pins(ArtifactPins("", (a, b))) == digest_from_pins(ArtifactPins("", (b, a)))


# --------------------------------------------------------------------------
# the verdict
# --------------------------------------------------------------------------

P1 = ArtifactPin("etopo_10m.tif", "11" * 32, 100, "built")
P2 = ArtifactPin("synthetic_map_stats.json", "22" * 32, 50, "built")
PINS = ArtifactPins(expected_conditioning_digest="", pins=(P1, P2))
GOOD_DIGEST = digest_from_pins(PINS)


def test_all_pinned_bytes_present_is_ok():
    v = verdict_from_observations(
        PINS, {"etopo_10m.tif": (100, "11" * 32), "synthetic_map_stats.json": (50, "22" * 32)}
    )
    assert v.ok
    assert v.actual_digest == GOOD_DIGEST == v.expected_digest


def test_one_wrong_file_is_refused_and_named():
    v = verdict_from_observations(
        PINS, {"etopo_10m.tif": (200, "99" * 32), "synthetic_map_stats.json": (50, "22" * 32)}
    )
    assert not v.ok
    assert [s.name for s in v.wrong] == ["etopo_10m.tif"]
    # The useful fact is not "the digest moved", it is WHICH file moved --
    # recovering that took a manual comparison against a pod that no longer
    # existed.
    assert "etopo_10m.tif" in v.report()
    assert "99" * 32 in v.report() and "11" * 32 in v.report()


def test_a_file_that_is_right_still_appears_in_the_report():
    v = verdict_from_observations(
        PINS, {"etopo_10m.tif": (200, "99" * 32), "synthetic_map_stats.json": (50, "22" * 32)}
    )
    assert "ok       synthetic_map_stats.json" in v.report()


def test_correct_sha_but_wrong_size_is_still_refused():
    """Belt and braces. A sha256 collision is not the threat; a truncated
    download that a hash check happens to be fed from a cached value is."""
    v = verdict_from_observations(
        PINS, {"etopo_10m.tif": (99, "11" * 32), "synthetic_map_stats.json": (50, "22" * 32)}
    )
    assert not v.ok
    assert v.statuses[0].state == WRONG_BYTES


def test_missing_file_yields_no_digest_at_all():
    """A digest over an incomplete set is not a weaker answer, it is a wrong
    one -- it would be a perfectly well-formed identity for a world that has
    never existed."""
    v = verdict_from_observations(PINS, {"etopo_10m.tif": None, "synthetic_map_stats.json": (50, "22" * 32)})
    assert not v.ok
    assert v.actual_digest is None
    assert [s.name for s in v.missing] == ["etopo_10m.tif"]
    assert "incomplete" in v.report()


def test_unpinned_extra_files_in_the_directory_are_ignored(tmp_path):
    """DEFAULT_CONDITIONING_FILES decides what conditions generation. This
    manifest pins those; it does not police the directory, or a stray download
    left beside the rasters would refuse a pod that is entirely correct."""
    root = tmp_path / "global"
    root.mkdir()
    body = b"x" * 100
    (root / "etopo_10m.tif").write_bytes(body)
    (root / "synthetic_map_stats.json").write_bytes(b"y" * 50)
    (root / "_etopo_source.tif").write_bytes(b"leftover download")
    pins = ArtifactPins(
        "",
        (
            ArtifactPin("etopo_10m.tif", hashlib.sha256(body).hexdigest(), 100, "built"),
            ArtifactPin("synthetic_map_stats.json", hashlib.sha256(b"y" * 50).hexdigest(), 50, "built"),
        ),
    )
    assert verify_root(root, pins).ok


def test_verify_root_reports_missing_on_an_empty_directory(tmp_path):
    v = verify_root(tmp_path, PINS)
    assert not v.ok
    assert len(v.missing) == 2


# --------------------------------------------------------------------------
# the manifest file itself
# --------------------------------------------------------------------------


def test_unknown_schema_is_refused_not_guessed_at(tmp_path):
    """Same rule as world_manifest: partial comprehension of a pin manifest is
    indistinguishable from agreement with it."""
    p = tmp_path / "pins.json"
    p.write_text(json.dumps({"schema": 99, "artifacts": []}))
    with pytest.raises(ConditioningPinsError, match="schema"):
        load_pins(p)


def test_missing_manifest_is_refused(tmp_path):
    with pytest.raises(ConditioningPinsError, match="not found"):
        load_pins(tmp_path / "nope.json")


def test_malformed_artifact_entry_is_refused(tmp_path):
    p = tmp_path / "pins.json"
    p.write_text(json.dumps({"schema": 1, "artifacts": [{"name": "a.tif"}]}))
    with pytest.raises(ConditioningPinsError, match="malformed"):
        load_pins(p)


def test_empty_artifact_list_is_refused(tmp_path):
    """An empty pin set verifies successfully against every possible box, which
    is the most dangerous way for this check to fail."""
    p = tmp_path / "pins.json"
    p.write_text(json.dumps({"schema": 1, "artifacts": []}))
    with pytest.raises(ConditioningPinsError, match="no artifacts"):
        load_pins(p)
