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

import os
import sys
import time


def _allow_cpu() -> bool:
    return os.environ.get("VOXELSIM_ALLOW_CPU") == "1"


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

    # TerrainDiffusionBackend picks its device with
    #   device = "cuda" if torch.cuda.is_available() else "cpu"
    # which means a torch/driver mismatch does NOT raise -- it quietly runs
    # on CPU at minutes-to-hours per tile while a rented 4090 sits idle and
    # billing. That is exactly what a cu130 wheel on a 12.8 driver produced
    # on 2026-07-19. Refuse to proceed rather than discover it from the bill.
    if not _allow_cpu():
        import torch
        if not torch.cuda.is_available():
            print("ERROR: torch.cuda.is_available() is False -- inference "
                  "would run on CPU.")
            print(f"  torch: {torch.__version__}")
            print("  Fix: reinstall torch+torchvision from the wheel index "
                  "matching this host's driver:")
            print("    python tools/cuda_index.py    # prints the index URLs "
                  "to try, best first")
            print("  torchvision MUST come from the same --index-url or it "
                  "drags a PyPI torch back in.")
            print("  Set VOXELSIM_ALLOW_CPU=1 only if you genuinely mean to "
                  "wait hours for one tile.")
            return 3

    from terrain_service.providers.diffusion import (
        ConditioningDataMissing,
        DiffusionConfig,
        DiffusionProvider,
        ModelOutputMismatch,
        compute_conditioning_digest,
        resolve_conditioning_root,
        validate_model_output,
    )

    # Hash the conditioning rasters this box actually has. Without this the
    # digest defaults to UNVERIFIED and verify_conditioning_digest refuses to
    # run inference at all -- which is what silently broke pod bring-up when
    # identity schema v2 added the gate: pregen_at.py and scan_land.py were
    # updated to compute it, probe_tile.py was missed, and the failure
    # surfaced two steps later as a misleading "WorldClim did not land".
    try:
        digest = compute_conditioning_digest()
    except ConditioningDataMissing as e:
        print(f"ERROR: {e}")
        print(f"Run from the directory containing {resolve_conditioning_root()}, "
              f"and build ETOPO first:")
        print("  python tools/fetch_conditioning.py")
        return 2

    config = DiffusionConfig(checkpoint_id=checkpoint_dir,
                             checkpoint_sha256=checkpoint_sha256,
                             conditioning_digest=digest)
    print("provider_id:", config.provider_id())

    provider = DiffusionProvider(config=config)

    t0 = time.time()
    seed = int(os.environ.get("PROBE_SEED", "20260719"))
    print("probe seed:", seed)
    raster = provider._call_model(seed=seed, x=0, y=0, scale=1)
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
