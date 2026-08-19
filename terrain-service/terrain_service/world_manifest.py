"""What made this world, written down beside the world.

WHY THIS EXISTS
---------------
On 2026-08-03 the same seed, the same repo commit and the same checkpoint
bundle id produced a DIFFERENT WORLD on a fresh pod
(docs/measurements/world-identity-not-reproducible-2026-08-03.txt). The
289-tile world that carries every owner-approved vista cannot be extended by
any machine, ever, and the reason it cannot be extended is NOT that the
sampler is unstable -- the control in that measurement is byte-identical on a
rerun. It is that nobody wrote down what the inputs were:

    "The original world's triple was never recorded, which is why it cannot
     be reconstructed even in principle now."

Two of the six conditioning files are BUILT by ``tools/bootstrap_pod.sh``
rather than downloaded (``etopo_10m.tif``, and ``synthetic_map_stats.json``
which is derived from it), and they came out different on the new pod -- one
of them differing in LENGTH by 902,131 bytes. That drift moves the
conditioning digest, which moves ``provider_id``, which moves the world.
Pinning those two artifacts is the real fix and is not this module's job.
This module's job is the part that has to be true FIRST: a world must carry a
record of the inputs that made it, or a future fix has nothing to aim at.

``docs/world-generation-architecture.md`` promises a server that keeps
generating an infinite world for as long as the game lives -- tile
(5000, -3000) generated in 2027 joining seamlessly to tile (0,0) generated in
2026. That promise is only as good as the identity record beside those tiles.

WHAT IT DOES
------------
1. RECORD. Every run that writes into a world drops (or confirms)
   ``world-identity.json`` in the world's own directory: provider_id, seed,
   checkpoint sha256, conditioning digest, the sha256 of EACH conditioning
   file, the terrain-diffusion version, and a UTC timestamp. It is automatic
   and costs one hash pass over ~25 MB of rasters per process, against ~10 s
   of GPU per tile. There is deliberately no flag to enable it: the world it
   would have saved was lost by a bring-up that had other things on its mind.

2. VERIFY. A run writing into a world that ALREADY has a manifest compares
   its identity against the recorded one and refuses on conflict, rather than
   appending tiles from a different planet into an existing namespace.
   ``world_identity_verdict`` is a pure function for the same reason
   ``pregen.superblock_gate_verdict`` is: the policy has to be testable
   without a GPU, a checkpoint, or 25 MB of rasters.

WHY VERIFY, WHEN provider_id IS ALREADY THE DIRECTORY NAME
----------------------------------------------------------
Content addressing normally makes this check impossible to fail: you cannot
land in a directory whose name disagrees with your identity, because the name
IS your identity. That guard held on 2026-08-03 and it is what caught the
divergence -- "the provider_id mismatch is the content-addressing working
exactly as designed".

It has exactly one hole: ``--provider-id-override`` /
``DiffusionConfig.provider_id_override`` returns a namespace string verbatim
and ignores every input that formed it. Using it to force a mismatch together
"would have [given] one namespace containing two different planets, with a
seam somewhere in the middle and no error anywhere". This manifest is that
missing error. The override still writes where it is told -- it is a
compatibility hatch for adopting a namespace, and it must stay usable for
that -- but it can no longer do it silently, because the recorded checkpoint
sha256 and conditioning file hashes are not overridable and will not match.
"""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

from .cache import WORLD_MANIFEST_NAME, TileCache

#: Bumped when the SHAPE of the record changes. A manifest written by a newer
#: schema is refused rather than half-understood: partial comprehension of an
#: identity record is indistinguishable from agreement with it.
MANIFEST_SCHEMA = 1

#: Sentinel for "this side did not record that field at all", which is a
#: different thing from "recorded, and different" -- see world_identity_verdict.
_ABSENT = object()


def build_identity(provider, seed: int, namespace_id: str | None = None) -> dict:
    """The identity fields for the world ``provider`` is about to write into.

    ``namespace_id`` is the cache directory being written (``provider_id`` for
    coarse tiles, ``fine_provider_id`` for the bake's output), defaulting to
    ``provider.provider_id``. It is recorded alongside ``provider_id`` because
    the bake writes into a namespace derived from -- but not equal to -- the
    inference identity, and a manifest that named only one of the two would
    leave a fine tier unable to say which coarse world it was baked from.

    Providers with no ``DiffusionConfig`` (the synthetic dev provider) get the
    first four fields and nothing else, ON PURPOSE. ``synthetic-v1``'s entire
    identity is its provider_id string; writing "checkpoint_sha256: none" for
    it would create a field that a future diffusion run could be compared
    against and would mismatch for no reason. An absent field is honest; an
    invented one is the failure mode this whole subsystem exists to prevent.

    The conditioning FILE hashes are what this box has under the resolved
    conditioning root; ``conditioning_digest`` is what the running config
    CLAIMS. They are recorded separately and neither is derived from the
    other, because on 2026-08-03 the interesting question was precisely how a
    claimed digest and the files on disk came apart. If the rasters cannot be
    read at all the map is left out entirely (a bake, for one, needs no
    conditioning data to run) and the verdict reports the gap rather than
    inventing hashes.
    """
    identity: dict = {
        "namespace_id": namespace_id or provider.provider_id,
        "provider_id": provider.provider_id,
        "seed": int(seed),
        "seed_hex": f"{int(seed):016x}",
    }
    config = getattr(provider, "config", None)
    if config is None:
        return identity
    identity["checkpoint_label"] = config.checkpoint_label
    identity["checkpoint_sha256"] = config.checkpoint_sha256
    identity["conditioning_digest"] = config.conditioning_digest
    identity["terrain_diffusion_version"] = config.terrain_diffusion_version
    try:
        from .providers.diffusion import conditioning_file_digests

        identity["conditioning_file_sha256"] = conditioning_file_digests(
            config.conditioning_files
        )
    except Exception:
        # ConditioningDataMissing, an unreadable file, a permission error --
        # all mean the same thing here: this run cannot testify about the
        # conditioning bytes. It must not testify anyway.
        pass
    return identity


def build_manifest(identity: dict, *, tiles_predate_manifest: bool = False) -> dict:
    """Wrap an identity in the record that gets written to disk.

    Everything outside ``identity`` is METADATA and is never compared: a
    timestamp that differed between two runs of the same world would refuse
    every legitimate extension.

    ``tiles_predate_manifest`` marks a manifest written into a world that
    already had artifacts in it. Such a record describes the run that wrote it
    and NOTHING about the tiles that were already there -- those were made by
    inputs nobody recorded, which is the entire finding this module answers.
    It is kept out of ``identity`` so it cannot cause a mismatch, and it is
    repeated back in every later verdict so it never quietly hardens into a
    provenance claim it cannot support.
    """
    return {
        "manifest_schema": MANIFEST_SCHEMA,
        "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "tiles_predate_manifest": bool(tiles_predate_manifest),
        "identity": dict(identity),
    }


def _flatten(identity: dict) -> dict:
    """``{"conditioning_file_sha256": {"a.tif": "ab"}}`` -> ``{"...[a.tif]": "ab"}``.

    So that a per-file drift names the FILE. Reporting only that a dict of six
    hashes differs would reproduce the position the 2026-08-03 investigation
    started from, where the digest was known to have moved and finding out
    which of the six files moved it took a manual comparison against a pod
    that no longer exists.
    """
    flat: dict = {}
    for key, value in (identity or {}).items():
        if isinstance(value, dict):
            for sub, subvalue in value.items():
                flat[f"{key}[{sub}]"] = subvalue
        else:
            flat[key] = value
    return flat


def _q(value) -> str:
    """``repr`` with braces escaped, since the message is a format string.

    A recorded value is a hash or an id and will not contain a brace -- but it
    is read off disk, and a crash while formatting the message that explains a
    provenance failure would lose the failure along with the explanation.
    """
    return repr(value).replace("{", "{{").replace("}", "}}")


def world_identity_verdict(existing: dict, current: dict) -> tuple[bool, str]:
    """May ``current`` write into the world ``existing`` describes?

    PURE -- two dicts in, a verdict out. Returns ``(ok, message)`` like
    ``pregen.superblock_gate_verdict``; the message takes ``{world}`` (the
    world directory) and is empty only when every recorded field was compared
    and agreed.

    Three outcomes, and the middle one is the point:

    * AGREE -- every field present on both sides matches. ``(True, "")``.
    * CONFLICT -- a field is recorded on both sides and differs. ``(False,
      ...)``. These tiles come from different inputs than the ones already
      there, and a world is not a place two generations may share: terrain
      must be identical for every client, and a seam between two planets in
      one namespace produces no error anywhere later. Refuse.
    * UNVERIFIABLE -- a field is recorded on one side only, and nothing
      conflicts. ``(True, warning)``. Not a refusal, because it has a routine
      and legitimate cause: ``--mode bake`` consumes cached coarse tiles and
      never opens the conditioning rasters, so a bake-only box may have no
      rasters to hash and can still be doing exactly the right thing. It is
      loud because the alternative reading -- someone has removed inputs the
      world depends on -- looks identical from here.

    A newer ``manifest_schema`` is refused outright. Comparing the fields a
    newer writer happens to share with this one, and ignoring the rest, would
    report agreement on the strength of what it could not read.
    """
    schema = existing.get("manifest_schema")
    if schema != MANIFEST_SCHEMA:
        return False, (
            f"error: world {{world}} carries a world-identity.json with "
            f"manifest_schema={schema!r}, and this build understands "
            f"{MANIFEST_SCHEMA}. Refusing to compare an identity record it "
            "cannot fully read -- agreeing on the fields that happen to "
            "overlap would be a provenance claim resting on the fields that "
            "do not. Use the build that wrote it, or migrate the record "
            "deliberately."
        )

    old = _flatten(existing.get("identity") or {})
    new = _flatten(current.get("identity") or {})
    conflicts: list[str] = []
    unverifiable: list[str] = []
    for key in sorted(set(old) | set(new)):
        a = old.get(key, _ABSENT)
        b = new.get(key, _ABSENT)
        if a is _ABSENT and b is _ABSENT:
            continue
        if a is _ABSENT:
            unverifiable.append(
                f"    {key}: not recorded with the world; this run has {_q(b)}"
            )
        elif b is _ABSENT:
            unverifiable.append(
                f"    {key}: recorded as {_q(a)}; this run cannot determine it"
            )
        elif a != b:
            conflicts.append(
                f"    {key}:\n      world: {_q(a)}\n      run:   {_q(b)}"
            )

    if conflicts:
        return False, (
            "error: this run's identity does not match the world already in "
            "{world}:\n"
            + "\n".join(conflicts)
            + "\n  These tiles would be generated from different inputs than "
            "the ones already there, and nothing downstream could tell the two "
            "apart afterwards: one namespace, two planets, a seam somewhere in "
            "the middle and no error anywhere (see "
            "docs/measurements/world-identity-not-reproducible-2026-08-03.txt)."
            "\n  Restore the inputs the world was made from -- the manifest "
            "names each file that moved -- or generate into a NEW world by "
            "letting the changed input roll provider_id, which is what it is "
            "for. Do not force the two together: an identity override would "
            "silence this message without making the terrain agree."
        )
    if unverifiable:
        return True, (
            "warning: this run could not verify every identity field of the "
            "world in {world} (nothing conflicts; these are one-sided):\n"
            + "\n".join(unverifiable)
            + "\n  Expected when a bake extends a world from cached coarse "
            "tiles, since baking never opens the conditioning rasters. If this "
            "run generates tiles, the missing inputs are the ones that decide "
            "what the terrain looks like -- confirm they are the world's own "
            "before adding to it."
        )
    if existing.get("tiles_predate_manifest"):
        return True, (
            "note: the identity record in {world} was back-filled after tiles "
            "already existed there. It matches this run, but it says nothing "
            "about how the ORIGINAL tiles were made -- that was never recorded "
            "and cannot be recovered."
        )
    return True, ""


def record_world_identity(
    cache: TileCache, provider, seed: int, namespace_id: str | None = None
) -> tuple[bool, str]:
    """Write or verify the identity record for one world. The entry point.

    Returns ``(ok, message)``; ``ok`` False means the caller must not write
    into this world. The message is already ``{world}``-formatted.

    Writes BEFORE any tile is generated, so an interrupted run still leaves
    behind what it was doing -- an eight-hour pregen killed at tile 200 is
    exactly the situation in which the identity is most worth having and least
    likely to be reconstructable from scrollback.
    """
    namespace_id = namespace_id or provider.provider_id
    world = cache.world_dir(namespace_id, seed)
    # Hashed ONCE per call: the conditioning rasters are ~25 MB and both
    # branches below need the same answer.
    identity = build_identity(provider, seed, namespace_id)

    raw = cache.get_world_manifest(namespace_id, seed)
    if raw is not None:
        try:
            existing = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as e:
            return False, (
                f"error: {world}/world-identity.json is unreadable ({e}). It is "
                "the only record of what made this world; refusing to overwrite "
                "it with a fresh one, because that would replace a damaged "
                "provenance claim with a confident and possibly false one. "
                "Restore it, or move the world aside."
            )
        ok, msg = world_identity_verdict(existing, build_manifest(identity))
        return ok, msg.format(world=world)

    predate = cache.world_has_artifacts(namespace_id, seed)
    manifest = build_manifest(identity, tiles_predate_manifest=predate)
    cache.put_world_manifest(
        namespace_id,
        seed,
        (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )
    if predate:
        return True, (
            f"warning: {world} already contained tiles but no identity record, "
            "so one has been written for THIS run. It does not describe the "
            "tiles that were already there: their inputs were never recorded "
            "and cannot be recovered (that is the 289-tile world's situation "
            "exactly). Treat the older tiles as unverified provenance."
        )
    return True, ""


def read_world_manifest(path: "str | Path") -> dict:
    """Load a manifest from an explicit path, for tools and for humans."""
    return json.loads(Path(path).read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------
# READING the record back: which namespace should a CLIENT be pointed at?
# ---------------------------------------------------------------------------


def find_fine_namespaces(
    cache_root: "str | Path",
    seed: int,
    *,
    tiles: "list[tuple[int, int]] | None" = None,
    coarse_provider_id: str | None = None,
) -> list[dict]:
    """Every baked fine namespace under ``cache_root`` for ``seed``, READ off
    disk. The answer to "what do I pass as ``-VoxelFineTileProviderId``?".

    WHY THIS IS A READER AND NEVER A RECOMPUTE
    ------------------------------------------
    On 2026-08-18 a capture run died three times on the fine-tier residency
    gate, and each time it was worked around by hand-pinning
    ``-VoxelFineTileProviderId``. The cause was not a wrong hash on either
    side. It was that NOTHING RECONCILES THE TWO SIDES:

    * The bake COMPUTES its namespace -- ``fine_id_for(provider_id())``, whose
      ``-bXXXXXXXX`` suffix is a sha256 over ``bake_identity_payload()`` and
      ``product_identity_payload()``. So it moves whenever a bake constant
      moves; ``bake_ver`` 27 -> 28 moved it ``-bdcab4bed`` -> ``-b19d281fd``.
    * The engine COMPUTES NOTHING. It reads a literal string from
      ``-VoxelFineTileProviderId`` or, failing that, ``DefaultFineTileProviderId``
      in ``ue-project/Config/DefaultGame.ini``.

    Between those two sits a human copying nine hex characters into an ini
    after every bake change. That is the entire reconciliation mechanism, and
    it is the thing that failed. Recomputing the fingerprint on the client side
    would not fix it -- it would need a second implementation of a Python
    sha256 in C++, i.e. a second answer to a question that content addressing
    exists to have exactly one answer to.

    So the authority is the record the bake already writes: ``world-identity.json``
    carries ``namespace_id``, which IS the directory it was written into. This
    function reads those records. It derives nothing.

    ``tiles`` filters to namespaces that actually hold every listed coarse tile,
    which is the question a capture harness really has -- "where can I fly THIS
    camera?" -- and is what distinguishes a stale pin (tile is baked, elsewhere)
    from a coverage gap (tile is baked nowhere).

    ``coarse_provider_id`` filters to fine namespaces derived from one coarse
    world. Matched by prefix, which is sound precisely because ``fine_id_for``
    SUFFIXES rather than re-hashes -- see its docstring for why that shape was
    chosen.

    Returns one dict per namespace, newest bake first::

        {"namespace_id", "path", "created_utc", "provider_id",
         "has_manifest", "tiles_present", "tiles_missing"}

    Namespaces with no manifest are still reported (``has_manifest`` False):
    the 289-tile world predates the manifest entirely, and a reader that hid
    the directories it cannot vouch for would answer "nowhere" for the world
    that matters most.
    """
    root = Path(cache_root)
    seed_hex = f"{int(seed):016x}"
    wanted = [tuple(t) for t in (tiles or [])]
    found: list[dict] = []
    if not root.is_dir():
        return found

    for entry in sorted(root.iterdir()):
        if not entry.is_dir():
            continue
        world = entry / seed_hex
        if not world.is_dir():
            continue  # this namespace holds nothing for this seed
        if coarse_provider_id and not entry.name.startswith(coarse_provider_id):
            continue

        record: dict = {}
        manifest_path = world / WORLD_MANIFEST_NAME
        try:
            record = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, ValueError):
            record = {}
        identity = record.get("identity") or {}

        present, missing = [], []
        for tx, ty in wanted:
            if (world / "s16" / f"{tx}_{ty}.vxtl").is_file():
                present.append((tx, ty))
            else:
                missing.append((tx, ty))

        found.append(
            {
                # The DIRECTORY NAME is the namespace, always. A manifest whose
                # recorded namespace_id disagreed with the directory holding it
                # would be a record that had been moved; reported below rather
                # than trusted over the path the client will actually open.
                "namespace_id": entry.name,
                "path": world,
                "created_utc": record.get("created_utc"),
                "provider_id": identity.get("provider_id"),
                "recorded_namespace_id": identity.get("namespace_id"),
                "has_manifest": bool(record),
                "tiles_present": present,
                "tiles_missing": missing,
            }
        )

    # Newest first, undated last: the freshest bake is what a capture almost
    # always wants, and an undated namespace is one nobody can date, not one
    # from the beginning of time.
    found.sort(key=lambda d: (d["created_utc"] is not None, d["created_utc"] or ""),
               reverse=True)
    return found
