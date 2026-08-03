#!/usr/bin/env python3
"""Get the six conditioning files onto this box AS THE PINNED BYTES, or fail.

This is the step ``bootstrap_pod.sh`` calls instead of hoping that building the
rasters lands on the same bytes twice. It does exactly three things:

  1. hashes whatever is already in ``data/global`` against
     ``data/conditioning-artifacts.json``;
  2. downloads what is missing, from the pinned URLs, verifying the sha256 of
     what arrived BEFORE putting it in place;
  3. exits non-zero, naming every file and both hashes, if the result is not
     byte-for-byte the pinned set.

There is no "close enough" and no warn-and-continue. A conditioning file that
is off by one byte is a different planet: it moves the conditioning digest,
which moves ``provider_id``, which means the tiles this pod generates can never
join the world it was asked to extend. Finding that out here costs a minute;
finding it out after a bake costs the bake.

WHY THIS DOES NOT JUST BUILD THEM
---------------------------------
Because the shipping world's ``etopo_10m.tif`` is not a build output and never
was -- see ``terrain_service/conditioning_artifacts.py`` for the three proofs
from the file's own TIFF tags. ``tools/fetch_etopo.py`` remains available and
has been made deterministic, but it produces a DIFFERENT file, and a different
file is a different world. ``--allow-build`` exists so that starting a new
world is possible; it is not a fallback and it is never automatic.

Usage:
    python3 tools/fetch_conditioning.py                 # verify + download
    python3 tools/fetch_conditioning.py --verify-only   # no network
    python3 tools/fetch_conditioning.py --print-expected-digest
    python3 tools/fetch_conditioning.py --expect-provider-id <id>
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from terrain_service.conditioning_artifacts import (  # noqa: E402
    ConditioningPinsError,
    load_pins,
    sha256_of_file,
    verify_root,
)

RED = "\033[31m"
YLW = "\033[33m"
GRN = "\033[32m"
BLD = "\033[1m"
RST = "\033[0m"


def _download(url: str, dest: Path, expect_sha: str, expect_size: int) -> bool:
    """Fetch ``url`` into ``dest`` only if it hashes to ``expect_sha``.

    Downloads to a sibling temp file and renames on success, so an interrupted
    or substituted download can never leave a plausible-looking wrong file
    where the next run will trust it because it is present.
    """
    tmp = None
    try:
        print(f"    trying {url}")
        req = urllib.request.Request(url, headers={"User-Agent": "voxelsim-bringup"})
        fd, tmpname = tempfile.mkstemp(dir=str(dest.parent), prefix=f".{dest.name}.")
        tmp = Path(tmpname)
        with urllib.request.urlopen(req, timeout=120) as r, os.fdopen(fd, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            got = 0
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                if total:
                    print(f"\r      {got / 1e6:.1f} / {total / 1e6:.1f} MB", end="", flush=True)
            print()
        actual = sha256_of_file(tmp)
        size = tmp.stat().st_size
        if actual != expect_sha or size != expect_size:
            print(
                f"{RED}      REJECTED: got sha256 {actual} ({size:,} B), "
                f"pinned is {expect_sha} ({expect_size:,} B){RST}"
            )
            return False
        tmp.replace(dest)
        tmp = None
        print(f"{GRN}      verified {expect_sha[:16]}{RST}")
        return True
    except Exception as e:  # noqa: BLE001 -- any failure means "try the next URL"
        print(f"      failed: {e}")
        return False
    finally:
        if tmp is not None and tmp.exists():
            tmp.unlink()


def _hosting_block(pins) -> str:
    h = pins.hosting_decision_required or {}
    if not h:
        return ""
    return (
        f"\n{BLD}THE HOSTING DECISION IS STILL OPEN.{RST}\n"
        f"  status: {h.get('owner_decision', '(unset)')}\n"
        f"  what:   {h.get('what', '')}\n"
        f"  why it is not decided in code: {h.get('why_not_decided_here', '')}\n"
        f"  recommendation: {h.get('recommendation', '')}\n"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--root",
        default=os.environ.get("TERRAIN_CONDITIONING_ROOT") or "data/global",
        help="conditioning directory (default: data/global, i.e. what upstream opens)",
    )
    ap.add_argument("--pins", default=None, help="path to conditioning-artifacts.json")
    ap.add_argument("--verify-only", action="store_true", help="never touch the network")
    ap.add_argument(
        "--print-expected-digest",
        action="store_true",
        help="print the conditioning_digest the pins imply and exit (no files needed)",
    )
    ap.add_argument(
        "--expect-provider-id",
        default=os.environ.get("EXPECT_PROVIDER_ID"),
        help=(
            "the world this box is supposed to be able to extend. Computes the "
            "provider_id these artifacts + this checkpoint would produce and "
            "FAILS if it differs. Needs --checkpoint-sha256."
        ),
    )
    ap.add_argument("--checkpoint-sha256", default=os.environ.get("CKPT_SHA"))
    ap.add_argument(
        "--allow-build",
        action="store_true",
        help=(
            "if the pinned bytes cannot be obtained, build a NEW pair instead. "
            "This starts a world of its own that can never join the pinned one."
        ),
    )
    args = ap.parse_args()

    try:
        pins = load_pins(args.pins)
    except ConditioningPinsError as e:
        print(f"{RED}{BLD}FAILED: {e}{RST}", file=sys.stderr)
        return 2

    if args.print_expected_digest:
        from terrain_service.conditioning_artifacts import digest_from_pins

        print(digest_from_pins(pins))
        return 0

    root = Path(args.root)
    root.mkdir(parents=True, exist_ok=True)

    verdict = verify_root(root, pins)

    # --- download whatever is missing ------------------------------------
    if not verdict.ok and not args.verify_only:
        for st in verdict.statuses:
            if st.state == "ok":
                continue
            pin = st.pin
            if st.state == "wrong-bytes":
                # Do NOT silently overwrite. A file that is present and wrong is
                # a fact worth stopping on: it usually means this box built its
                # own, and quietly replacing it would erase the evidence of what
                # any tiles already here were generated from.
                print(
                    f"{YLW}    {pin.name} is present but is NOT the pinned bytes; "
                    f"leaving it alone. Move it aside and re-run to replace it.{RST}"
                )
                continue
            if not pin.sources:
                continue
            print(f"  fetching {pin.name}")
            for url in pin.sources:
                if _download(url, root / pin.name, pin.sha256, pin.size):
                    break
        verdict = verify_root(root, pins)

    print()
    print(f"conditioning root: {root.resolve()}")
    print(verdict.report())
    # The per-file report has to reach the log BEFORE the failure banner, or a
    # pod operator reads "FAILED" with no evidence above it. stdout is
    # block-buffered when bootstrap tees it to a file; stderr is not.
    sys.stdout.flush()

    if verdict.ok:
        print(f"{GRN}{BLD}ok: this box holds the pinned conditioning set.{RST}")
    else:
        unhosted = [
            s.pin for s in (verdict.missing + verdict.wrong) if s.pin and not s.pin.sources
        ]
        print(file=sys.stderr)
        print(f"{RED}{BLD}FAILED: this box does NOT hold the pinned conditioning set.{RST}", file=sys.stderr)
        if unhosted:
            names = ", ".join(p.name for p in unhosted)
            print(
                f"{RED}  No download URL is pinned for: {names}{RST}\n"
                f"{RED}  These are the two files bootstrap used to BUILD, and building them{RST}\n"
                f"{RED}  does not reproduce them -- see terrain_service/conditioning_artifacts.py.{RST}",
                file=sys.stderr,
            )
            print(_hosting_block(pins), file=sys.stderr)
        if args.allow_build:
            print(
                f"{YLW}--allow-build was given. Build the pair with:{RST}\n"
                f"    python3 tools/fetch_etopo.py --i-am-starting-a-new-world\n"
                f"{YLW}and understand that the result is a DIFFERENT WORLD from the pinned{RST}\n"
                f"{YLW}one. Do not use --provider-id-override to make it look otherwise.{RST}",
                file=sys.stderr,
            )
        return 1

    # --- optional: can this box extend the world it was asked to? --------
    if args.expect_provider_id:
        if not args.checkpoint_sha256:
            print(
                f"{RED}{BLD}FAILED: --expect-provider-id needs --checkpoint-sha256 "
                f"(or $CKPT_SHA).{RST}",
                file=sys.stderr,
            )
            return 2
        from terrain_service.providers.diffusion import DiffusionConfig

        actual = DiffusionConfig(
            checkpoint_sha256=args.checkpoint_sha256,
            conditioning_digest=verdict.actual_digest,
        ).provider_id()
        print(f"provider_id from these artifacts: {actual}")
        if actual != args.expect_provider_id:
            print(
                f"\n{RED}{BLD}FAILED: this box cannot reproduce {args.expect_provider_id}.{RST}\n"
                f"{RED}  it would generate: {actual}{RST}\n"
                f"{RED}  The conditioning set above matches its pins, so the difference is{RST}\n"
                f"{RED}  NOT the rasters -- it is the checkpoint sha256, or generation code{RST}\n"
                f"{RED}  that entered the identity (world_shape, climate calibration, tile{RST}\n"
                f"{RED}  format, IDENTITY_SCHEMA_VERSION). Check out the commit that made{RST}\n"
                f"{RED}  that world.{RST}\n"
                f"{RED}  DO NOT reach for --provider-id-override: it would put two different{RST}\n"
                f"{RED}  planets in one namespace, with a seam in the middle and no error.{RST}",
                file=sys.stderr,
            )
            return 1
        print(f"{GRN}{BLD}ok: this box can extend {args.expect_provider_id}.{RST}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
