#!/usr/bin/env python3
"""One-tile probe: does the real terrain-diffusion output match our assumptions?

This is the decisive bring-up check from docs/pod-bringup-commands.md Block 3,
as a FILE rather than a heredoc -- pasting a multi-line heredoc into a web
terminal silently re-indents it, which leaves the shell waiting on a closing
EOF that never matches. A file has no such failure mode.

Usage (from terrain-service/):
    python tools/probe_tile.py <checkpoint_dir> <checkpoint_sha256>

Example:
    python tools/probe_tile.py /workspace/ckpt/terrain-diffusion-30m ed06c427...

Prints provider_id, per-tile wall time, one line per raster channel, and then
either "MATCHES our assumption" or the specific EXPECTED_CHANNELS mismatches.
Exits 0 on match, 1 on mismatch, 2 on bad arguments.
"""

import sys
import time


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    checkpoint_dir, checkpoint_sha256 = sys.argv[1], sys.argv[2]

    # Guard the single most likely bring-up mistake: pasting the placeholder
    # markers along with the value. A wrong hash does not fail loudly -- it
    # silently changes provider_id, which is the tile cache namespace AND the
    # edit-log provider stamp, so a typo here poisons both.
    if checkpoint_sha256.startswith("<") or checkpoint_sha256.endswith(">"):
        print("ERROR: strip the < > placeholder brackets from the sha256.")
        return 2
    if len(checkpoint_sha256) != 64:
        print(f"ERROR: sha256 should be 64 hex chars, got {len(checkpoint_sha256)}.")
        return 2

    from terrain_service.providers.diffusion import (
        DiffusionConfig,
        DiffusionProvider,
        ModelOutputMismatch,
        validate_model_output,
    )

    config = DiffusionConfig(checkpoint_id=checkpoint_dir,
                             checkpoint_sha256=checkpoint_sha256)
    print("provider_id:", config.provider_id())

    provider = DiffusionProvider(config=config)

    t0 = time.time()
    raster = provider._call_model(seed=20260719, x=0, y=0, scale=1)
    elapsed = time.time() - t0
    # The pregen budget is derived from this number, so print it prominently.
    print(f"per-tile: {elapsed:.1f}s  -> 25 tiles ~{elapsed * 25 / 60:.1f} min,"
          f" 289 tiles ~{elapsed * 289 / 60:.1f} min")

    for k, v in raster.items():
        print(f"{k}: shape={v.shape} dtype={v.dtype} "
              f"min={v.min():.3f} max={v.max():.3f}")

    try:
        validate_model_output(raster, config.channel_mapping)
    except ModelOutputMismatch as e:
        print("MISMATCH -- channel_mapping needs adjusting:")
        for issue in e.issues:
            print("-", issue)
        return 1

    print("MATCHES our assumption - no adaptation needed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
