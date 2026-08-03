"""The conditioning files are PINNED BYTES, not a build recipe.

WHY THIS EXISTS
---------------
``world_manifest.py`` made every world record the six conditioning hashes that
made it, so a machine that cannot reproduce a world is told WHICH file moved.
This module is the other half: making the six files something a second machine
can actually obtain, so the answer to "which file moved" can be "none".

WHAT WAS MEASURED (2026-08-02, see
docs/measurements/etopo-build-not-reproducible-2026-08-02.txt)
-----------------------------------------------------------------
The earlier diagnosis was that ``tools/fetch_etopo.py`` drifts because it tries
four NOAA URLs (including both a ``_bed`` and a ``_surface`` variant) and then
resamples through whatever GDAL the pod resolved. That is true, and it is not
the whole story. The stronger fact is:

    **The canonical ``etopo_10m.tif`` was never produced by fetch_etopo.py, and
    fetch_etopo.py cannot produce it -- not with any URL, GDAL version,
    resampling kernel or compression setting.**

Three independent proofs, all from the file's own TIFF tags:

  * It is UNCOMPRESSED. 2160x1080 float32 = 9,331,200 B, plus 13,775 B of tags,
    is exactly its 9,344,975 B length, and ``Compression`` reads ``NONE``.
    fetch_etopo.py writes ``compress="deflate"``; deflate of these very pixels
    is 7,439,420 B at any zlib level from 1 to 9.
  * It carries ETOPO's OWN georeferencing: ``GeoAsciiParams`` =
    ``'WGS 84 + EGM2008 height|...'``, ``GDAL_NODATA`` = ``'-99999'``,
    ``ModelPixelScale`` z = 1.0. fetch_etopo.py writes
    ``profile = ref.profile.copy()`` taken from ``wc2.1_10m_bio_1.tif``, which
    stamps ``'WGS 84|'``, nodata ``-3.4e+38`` and z = 0.0. A copied profile
    cannot emit the source's vertical datum.
  * Its mtime is 2026-06-28. fetch_etopo.py's first commit (66aecae) is
    2026-07-22 -- 24 days later.

It is a hand-made ``gdal_translate``/``gdalwarp`` downsample of the ETOPO 2022
60 arc-second ``_bed`` product. (``_bed``, confirmed from the pixels: Greenland
centre reads -151 m and West Antarctica -1360 m; the ``_surface`` product reads
about +3000 m and +1800 m at those points.)

The pod's copy is not merely a re-compression of the same pixels either. At
8,442,844 B it sits between deflate (7,439,420) and no compression (9,337,952)
of the canonical pixels, and matches none of deflate levels 1-9, LZW
(9,153,174), LZW+predictor3 (7,473,209), deflate+predictor3 (6,011,456) or
packbits (9,413,552). Its PIXELS differ, not just its container.

WHAT FOLLOWS FROM THAT
----------------------
Making the builder byte-deterministic is worth doing and is done (see
``tools/fetch_etopo.py``), but it CANNOT recover the shipping world's bytes,
because the shipping world's bytes were never a build output. So the pinned
artifact is not a fallback for a hard-to-determinise build; it is the only
representation these two files have. Hence:

  * every conditioning file gets a sha256 pin in ``data/conditioning-artifacts.json``,
    including the four WorldClim rasters that have never drifted -- a pin that
    only covers the files known to be flaky stops being a check the day a
    fifth one moves;
  * ``tools/fetch_conditioning.py`` verifies against the pins and FAILS,
    loudly and by name, rather than letting a run proceed on the wrong bytes;
  * ``tools/fetch_etopo.py`` still builds the pair, but says in plain words
    that what it built starts a world of its own.

WHY THE EXPECTED DIGEST IS DERIVED, NOT WRITTEN DOWN TWICE
----------------------------------------------------------
``compute_conditioning_digest`` hashes the string ``name:sha256`` per file,
sorted by name. Given the pins, that value is computable WITHOUT the files
being present -- see ``digest_from_pins``. So bootstrap can state the digest it
is aiming for before it has downloaded anything, and a pin edited without its
digest being updated is caught by a test rather than by a world. The digest in
the JSON is a cross-check of the derivation, not a second source of truth.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path

#: Where the pin manifest lives, relative to the repo's terrain-service dir.
DEFAULT_PINS_PATH = Path(__file__).resolve().parent.parent / "data" / "conditioning-artifacts.json"

#: Bumped when the SHAPE of conditioning-artifacts.json changes. Same rule as
#: ``world_manifest.MANIFEST_SCHEMA``: a newer schema is refused, not guessed at.
PINS_SCHEMA = 1


class ConditioningPinsError(RuntimeError):
    """The pin manifest itself is unusable (missing, malformed, wrong schema)."""


@dataclass(frozen=True)
class ArtifactPin:
    """One conditioning file's expected bytes, and how a fresh box gets them."""

    name: str
    sha256: str
    size: int
    #: ``"downloaded"`` (upstream publishes these bytes) or ``"built"`` (some
    #: process on some box produced them, and that process is not a source).
    origin: str
    #: Immutable URLs to try in order. EMPTY for the two built artifacts until
    #: the hosting decision in the manifest is made -- see
    #: ``hosting_decision_required``. Empty is not "fall back to building": it
    #: is a hard failure with an explanation.
    sources: tuple[str, ...] = ()
    #: The command that CAN produce a file of this kind, for the record.
    builder: str | None = None
    #: Whether running ``builder`` is expected to reproduce ``sha256``. False
    #: for etopo_10m.tif and synthetic_map_stats.json: measured, not assumed.
    builder_reproduces_pin: bool = False
    note: str = ""


@dataclass(frozen=True)
class ArtifactPins:
    expected_conditioning_digest: str
    pins: tuple[ArtifactPin, ...]
    hosting_decision_required: dict = field(default_factory=dict)

    def by_name(self) -> dict[str, ArtifactPin]:
        return {p.name: p for p in self.pins}

    @property
    def names(self) -> tuple[str, ...]:
        return tuple(p.name for p in self.pins)


def load_pins(path: "str | Path | None" = None) -> ArtifactPins:
    """Read ``data/conditioning-artifacts.json``.

    Raises ``ConditioningPinsError`` rather than returning a degraded object:
    a half-understood pin manifest is indistinguishable from agreement with it,
    which is the same trap ``world_manifest`` refuses on schema mismatch.
    """
    p = Path(path) if path is not None else DEFAULT_PINS_PATH
    try:
        raw = json.loads(p.read_text(encoding="utf-8"))
    except FileNotFoundError as e:
        raise ConditioningPinsError(f"pin manifest not found at {p}") from e
    except json.JSONDecodeError as e:
        raise ConditioningPinsError(f"pin manifest at {p} is not valid JSON: {e}") from e

    schema = raw.get("schema")
    if schema != PINS_SCHEMA:
        raise ConditioningPinsError(
            f"pin manifest at {p} has schema {schema!r}, this code understands "
            f"{PINS_SCHEMA}. Refusing to interpret it."
        )

    pins = []
    for item in raw.get("artifacts", ()):
        try:
            pins.append(
                ArtifactPin(
                    name=item["name"],
                    sha256=item["sha256"],
                    size=int(item["size"]),
                    origin=item.get("origin", "built"),
                    sources=tuple(item.get("sources", ())),
                    builder=item.get("builder"),
                    builder_reproduces_pin=bool(item.get("builder_reproduces_pin", False)),
                    note=item.get("note", ""),
                )
            )
        except (KeyError, TypeError, ValueError) as e:
            raise ConditioningPinsError(f"malformed artifact entry in {p}: {item!r} ({e})") from e
    if not pins:
        raise ConditioningPinsError(f"pin manifest at {p} lists no artifacts")

    return ArtifactPins(
        expected_conditioning_digest=raw.get("expected_conditioning_digest", ""),
        pins=tuple(pins),
        hosting_decision_required=raw.get("hosting_decision_required", {}) or {},
    )


def digest_from_pins(pins: ArtifactPins) -> str:
    """The ``conditioning_digest`` a box holding exactly these bytes would compute.

    Reproduces ``providers.diffusion.compute_conditioning_digest``'s manifest
    construction (``name:sha256`` lines, names sorted, joined by newline,
    sha256 of the utf-8 of that) WITHOUT opening a single raster. Kept in step
    with that function by ``test_conditioning_artifacts`` rather than by
    comment; if the two ever disagree the test fails, which is the only way a
    duplicated hash construction is safe to have.
    """
    lines = [f"{p.name}:{p.sha256}" for p in sorted(pins.pins, key=lambda p: p.name)]
    return hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()


# --- verdicts -------------------------------------------------------------
#
# Pure, like world_manifest.world_identity_verdict and pregen.superblock_gate_
# verdict, and for the same reason: the policy has to be testable without a
# GPU, a network, or 25 MB of rasters on disk.

#: One artifact's state. Order matters only for reporting.
OK = "ok"
MISSING = "missing"
WRONG_BYTES = "wrong-bytes"


@dataclass(frozen=True)
class ArtifactStatus:
    name: str
    state: str
    expected_sha256: str
    expected_size: int
    actual_sha256: str | None = None
    actual_size: int | None = None
    pin: ArtifactPin | None = None

    @property
    def ok(self) -> bool:
        return self.state == OK


@dataclass(frozen=True)
class ConditioningVerdict:
    """Whether this box holds the pinned world, and if not, exactly why."""

    statuses: tuple[ArtifactStatus, ...]
    expected_digest: str
    #: The digest the files actually on disk produce, when all six are present.
    #: ``None`` when something is missing -- a digest over an incomplete set is
    #: not a weaker answer, it is a wrong one.
    actual_digest: str | None

    @property
    def ok(self) -> bool:
        return all(s.ok for s in self.statuses) and self.actual_digest == self.expected_digest

    @property
    def missing(self) -> tuple[ArtifactStatus, ...]:
        return tuple(s for s in self.statuses if s.state == MISSING)

    @property
    def wrong(self) -> tuple[ArtifactStatus, ...]:
        return tuple(s for s in self.statuses if s.state == WRONG_BYTES)

    def report(self) -> str:
        """A human-readable account, naming every file and both hashes.

        Deliberately verbose about the files that are RIGHT as well as the ones
        that are wrong: on 2026-08-03 the useful fact was not "the digest
        moved", it was "these two of the six moved and those four did not", and
        recovering that took a manual comparison against a pod that no longer
        existed.
        """
        lines = []
        for s in self.statuses:
            if s.state == OK:
                lines.append(f"  ok       {s.name}  {s.expected_sha256[:16]}  {s.expected_size:,} B")
            elif s.state == MISSING:
                lines.append(f"  MISSING  {s.name}  want {s.expected_sha256[:16]}  {s.expected_size:,} B")
            else:
                lines.append(
                    f"  WRONG    {s.name}\n"
                    f"             want {s.expected_sha256}  {s.expected_size:,} B\n"
                    f"             have {s.actual_sha256}  {s.actual_size:,} B"
                )
        lines.append(f"  expected conditioning_digest: {self.expected_digest}")
        lines.append(f"  actual   conditioning_digest: {self.actual_digest or '(incomplete -- files missing)'}")
        return "\n".join(lines)


def verdict_from_observations(
    pins: ArtifactPins,
    observed: "dict[str, tuple[int, str] | None]",
) -> ConditioningVerdict:
    """Pure core: compare pinned bytes against ``{name: (size, sha256) | None}``.

    ``None`` means the file is absent. Files present on disk but NOT pinned are
    ignored on purpose -- ``DEFAULT_CONDITIONING_FILES`` decides what conditions
    generation, and this manifest's job is to pin those, not to police the
    directory.
    """
    statuses = []
    for p in sorted(pins.pins, key=lambda p: p.name):
        obs = observed.get(p.name)
        if obs is None:
            statuses.append(
                ArtifactStatus(p.name, MISSING, p.sha256, p.size, pin=p)
            )
            continue
        size, sha = obs
        state = OK if (sha == p.sha256 and size == p.size) else WRONG_BYTES
        statuses.append(
            ArtifactStatus(p.name, state, p.sha256, p.size, actual_sha256=sha, actual_size=size, pin=p)
        )

    if any(s.state == MISSING for s in statuses):
        actual_digest = None
    else:
        lines = [f"{s.name}:{s.actual_sha256}" for s in statuses]
        actual_digest = hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()

    return ConditioningVerdict(
        statuses=tuple(statuses),
        expected_digest=digest_from_pins(pins),
        actual_digest=actual_digest,
    )


def sha256_of_file(path: "str | Path", chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(chunk), b""):
            h.update(block)
    return h.hexdigest()


def observe_root(root: "str | Path", pins: ArtifactPins) -> "dict[str, tuple[int, str] | None]":
    """Hash whatever of the pinned set is present under ``root``."""
    r = Path(root)
    out: dict[str, tuple[int, str] | None] = {}
    for p in pins.pins:
        f = r / p.name
        out[p.name] = (f.stat().st_size, sha256_of_file(f)) if f.is_file() else None
    return out


def verify_root(root: "str | Path", pins: "ArtifactPins | None" = None) -> ConditioningVerdict:
    """Convenience: ``verdict_from_observations(pins, observe_root(root, pins))``."""
    pins = pins if pins is not None else load_pins()
    return verdict_from_observations(pins, observe_root(root, pins))
