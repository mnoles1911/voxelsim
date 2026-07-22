#!/usr/bin/env python3
"""Print the checkpoint's content sha256 -- and nothing else.

The one and only line of stdout is the 64-char hex digest, so callers can do
``SHA=$(python tools/checkpoint_sha256.py DIR)`` without parsing anything.

This value matters more than it looks. It feeds ``DiffusionConfig.provider_id``,
which is BOTH the tile-cache namespace and the edit-log provider stamp. A
wrong value does not fail loudly; it silently forks the namespace. On
2026-07-19 the placeholder was pasted with its angle brackets still attached
(``checkpoint_sha256="<ed06...>"``) -- hence the bracket guards in
probe_tile.py, scan_land.py and pregen_at.py, and hence this script, which
removes the copy-paste step entirely.

Usage (from terrain-service/):
    python tools/checkpoint_sha256.py /workspace/ckpt/terrain-diffusion-30m
"""

import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    from terrain_service.providers.diffusion import _sha256_of_checkpoint_path

    print(_sha256_of_checkpoint_path(sys.argv[1]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
