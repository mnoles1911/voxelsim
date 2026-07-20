#!/usr/bin/env python3
"""Assert thresholds against a -VoxelPerfRun JSON summary
(docs/debug-tooling-plan.md P1 "Regression harness").

UVoxelPerfRunSubsystem (ue-project/Source/VoxelEarth/VoxelPerfRunSubsystem.cpp)
writes ue-project/Saved/PerfRuns/perf_<timestamp>.json at the end of a
`-VoxelPerfRun=<seconds>` scripted flight run. This script reads that file and
asserts CLI-supplied thresholds, exiting nonzero on any violation -- this is
what makes a perf run into an automatable CI gate (this becomes the M1 60fps
and M2 no-hitch gate harness per docs/debug-tooling-plan.md's "Headless/CI"
access-model row).

Usage:
  python tools/check-perf-run.py <path/to/perf_*.json> [--max-p95-ms 33.3] [--max-hitches 0]
                                  [--max-max-ms 100.0] [--min-avg-chunks-per-sec 0]

Exit codes: 0 = all supplied thresholds passed; 1 = at least one violated;
2 = the JSON file could not be read/parsed.
"""

import argparse
import json
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("json_path", help="Path to a perf_<timestamp>.json file written by -VoxelPerfRun.")
    parser.add_argument("--max-p95-ms", type=float, default=None, help="Fail if p95FrameMs exceeds this.")
    parser.add_argument("--max-hitches", type=int, default=None, help="Fail if hitchCount exceeds this.")
    parser.add_argument("--max-max-ms", type=float, default=None, help="Fail if maxFrameMs exceeds this.")
    parser.add_argument(
        "--min-avg-chunks-per-sec", type=float, default=None, help="Fail if avgChunksPerSec is below this."
    )
    parser.add_argument(
        "--max-post-warmup-p95-ms",
        type=float,
        default=None,
        help="Fail if postWarmupP95FrameMs (frames from warmupExcludeSeconds onward) exceeds this.",
    )
    parser.add_argument(
        "--max-post-warmup-hitches",
        type=int,
        default=None,
        help="Fail if postWarmupHitchCount (docs/status.md 'Perf-run hitches' isolation task -- steady-state "
        "hitches, excluding the first warmupExcludeSeconds) exceeds this.",
    )
    args = parser.parse_args()

    try:
        with open(args.json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"FAIL: could not read/parse {args.json_path}: {exc}", file=sys.stderr)
        return 2

    checks = [
        ("--max-p95-ms", args.max_p95_ms, "p95FrameMs", lambda v, t: v <= t),
        ("--max-hitches", args.max_hitches, "hitchCount", lambda v, t: v <= t),
        ("--max-max-ms", args.max_max_ms, "maxFrameMs", lambda v, t: v <= t),
        ("--min-avg-chunks-per-sec", args.min_avg_chunks_per_sec, "avgChunksPerSec", lambda v, t: v >= t),
        ("--max-post-warmup-p95-ms", args.max_post_warmup_p95_ms, "postWarmupP95FrameMs", lambda v, t: v <= t),
        ("--max-post-warmup-hitches", args.max_post_warmup_hitches, "postWarmupHitchCount", lambda v, t: v <= t),
    ]

    failures = []
    ran_any = False
    for flag, threshold, field, ok in checks:
        if threshold is None:
            continue
        ran_any = True
        if field not in data:
            failures.append(f"{field} missing from {args.json_path}")
            continue
        value = data[field]
        status = "PASS" if ok(value, threshold) else "FAIL"
        print(f"{status}: {field}={value} vs {flag}={threshold}")
        if status == "FAIL":
            failures.append(f"{field}={value} violates {flag}={threshold}")

    if not ran_any:
        print("No thresholds supplied -- pass at least one of the --max-*/--min-* flags. Dumping summary:")
        print(json.dumps(data, indent=2))
        return 0

    print(json.dumps(data, indent=2))

    if failures:
        print(f"\n{len(failures)} threshold(s) violated:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print("\nAll thresholds passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
