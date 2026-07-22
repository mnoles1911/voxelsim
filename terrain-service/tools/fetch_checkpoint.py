#!/usr/bin/env python3
"""Download the terrain-diffusion checkpoint bundle and verify its shape.

The checkpoint id is the single most misleading thing in the terrain-diffusion
repo. ``terrain_diffusion.common.model_utils.MODEL_PATHS`` lists THREE separate
HuggingFace repos, which reads as "download all three" -- it is not.
``WorldPipeline.from_pretrained`` takes ONE path (a local directory or an HF
repo id) and pulls ``coarse_model`` / ``base_model`` / ``decoder_model`` out of
it via ``subfolder=``. Those three MODEL_PATHS entries are the separate
*training* repos the bundle was assembled from.

The bundle to use is ``xandergos/terrain-diffusion-30m`` (30 m/px native).
A ``-90m`` bundle also exists; it is a different resolution, not a newer
version, and would need its own DiffusionConfig/provider_id.

Usage (from terrain-service/):
    python tools/fetch_checkpoint.py [--repo ID] [--dest DIR]

Idempotent: ``snapshot_download`` reuses anything already on disk, and the
shape check is re-run either way. Exits 1 if the downloaded tree does not
look like a WorldPipeline snapshot, rather than letting the failure surface
hours later as a confusing load error.
"""

import argparse
import os
import sys

DEFAULT_REPO = "xandergos/terrain-diffusion-30m"
REQUIRED_SUBFOLDERS = ("coarse_model", "base_model", "decoder_model")


def check_shape(path: str) -> list[str]:
    """Return a list of problems; empty means the tree looks right."""
    problems = []
    if not os.path.isdir(path):
        return [f"{path} is not a directory"]
    for sub in REQUIRED_SUBFOLDERS:
        d = os.path.join(path, sub)
        if not os.path.isdir(d):
            problems.append(f"missing subfolder {sub}/")
            continue
        names = os.listdir(d)
        if not any(n.endswith(".safetensors") or n.endswith(".bin") for n in names):
            problems.append(f"{sub}/ has no *.safetensors weights")
        if "config.json" not in names:
            problems.append(f"{sub}/ has no config.json")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--dest", required=True)
    args = ap.parse_args()

    from huggingface_hub import snapshot_download

    print(f"downloading {args.repo} -> {args.dest}", flush=True)
    path = snapshot_download(args.repo, local_dir=args.dest)
    print(f"snapshot at: {path}")

    problems = check_shape(path)
    if problems:
        print("\nERROR: this does not look like a WorldPipeline snapshot:")
        for p in problems:
            print(" -", p)
        print("\nExpected a top-level config.json plus coarse_model/,")
        print("base_model/ and decoder_model/, each with config.json and")
        print("*.safetensors. If the repo layout has changed upstream, check")
        print("WorldPipeline.from_pretrained in the terrain-diffusion clone.")
        return 1

    print("shape OK: coarse_model/, base_model/, decoder_model/ all present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
