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

EXIT CODES -- the contract callers gate on:

  0  every supplied threshold was compared against a real number and passed.
  1  at least one threshold was VIOLATED, or named a field this JSON does not
     carry (or carries as null / a non-number). Either way the answer is "no".
  2  the run could NOT be gated. The file is unreadable or unparseable, or NO
     thresholds were supplied, or the frame-time numbers in it are marked
     inadmissible, or the sample a threshold addresses is empty. Exit 2 is a
     configuration or instrument error. IT IS NEVER A PASS.

WHY EXIT 2 COVERS "NO THRESHOLDS SUPPLIED" (this path used to return 0).
A caller that forgets its flags -- a CI step whose arguments got mangled, a
wrapper whose variable expanded empty -- got a green gate that had tested
nothing, and it looked exactly like a run that passed. That is this project's
house failure: a check that cannot come out the other way is not a check. The
summary is still dumped, because dumping the summary is a useful thing to do;
it just is not a pass.

WHY EXIT 2 ALSO COVERS frameTimingAdmissible=0 AND AN EMPTY SAMPLE.
Same defect class, one level in. VoxelPerfRunSubsystem writes
`frameTimingAdmissible: 0` on any leg that fired screenshot shutters, because a
shutter stalls the frame it is serviced on and those stalls land in p95 and max
-- the subsystem's own words are "THE FRAME-TIME NUMBERS IN THIS RUN'S SUMMARY
ARE NOT ADMISSIBLE AS TIMING". Gating p95 against those numbers is a verdict on
the screenshot code. And a run with frameCount 0 reports p95 0.000, which sails
under every ceiling anyone will ever set: a pass that could not have failed.
Both refuse instead. --allow-inadmissible-timing exists for the operator who
knows what they are doing and says so out loud on the command line.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

# Frame-time fields: numbers a screenshot shutter contaminates, and whose
# sample must be non-empty for a ceiling on them to mean anything. The value is
# the JSON field carrying that sample's frame count.
TIMING_FIELD_SAMPLE = {
    "p95FrameMs": "frameCount",
    "maxFrameMs": "frameCount",
    "hitchCount": "frameCount",
    "postWarmupP95FrameMs": "postWarmupFrameCount",
    "postWarmupHitchCount": "postWarmupFrameCount",
}


def selftest() -> int:
    """Prove the exit-code contract above, on fabricated inputs, with no editor.

    This exists because `python -m py_compile` cannot see the defect this file
    was fixed for. The vacuous pass -- "no thresholds supplied" returning 0 --
    was syntactically perfect for as long as it shipped. The only check that
    could have caught it is one that asserts the CODE, so this asserts the code:
    each case below is run as a real subprocess and its exit status compared.

    Every case is a case that CAN fail. Case 3 is the regression itself: if
    someone ever restores the `return 0`, this goes red.
    """
    base = {
        "durationSeconds": 120.0,
        "frameCount": 33747,
        "p50FrameMs": 7.285,
        "p95FrameMs": 9.685,
        "maxFrameMs": 400.0,
        "hitchCount": 134,
        "postWarmupFrameCount": 33471,
        "postWarmupP95FrameMs": 9.438,
        "postWarmupHitchCount": 85,
        "avgChunksPerSec": 4558.83,
        "frameTimingAdmissible": 1,
    }
    variants = {
        "good": base,
        "slow": dict(base, p95FrameMs=44.0),
        "imageleg": dict(base, frameTimingAdmissible=0),
        "empty": dict(base, frameCount=0, p95FrameMs=0.0),
        "nullp95": dict(base, p95FrameMs=None),
        "nofield": {k: v for k, v in base.items() if k != "p95FrameMs"},
    }
    cases = [
        ("thresholds supplied and met", "good", ["--max-p95-ms", "33.3"], 0),
        ("p95 over the ceiling", "slow", ["--max-p95-ms", "33.3"], 1),
        ("NO THRESHOLDS -- must NOT be a pass", "good", [], 2),
        ("field the JSON does not carry", "nofield", ["--max-p95-ms", "33.3"], 1),
        ("field present but null", "nullp95", ["--max-p95-ms", "33.3"], 1),
        ("image leg (frameTimingAdmissible=0)", "imageleg", ["--max-p95-ms", "33.3"], 2),
        ("image leg, operator override", "imageleg", ["--max-p95-ms", "33.3", "--allow-inadmissible-timing"], 0),
        ("empty frame sample", "empty", ["--max-p95-ms", "33.3"], 2),
        ("non-timing threshold on an empty leg", "empty", ["--min-avg-chunks-per-sec", "50000"], 1),
        ("unparseable file", "@corrupt", ["--max-p95-ms", "33.3"], 2),
        ("JSON that is not an object", "@list", ["--max-p95-ms", "33.3"], 2),
    ]

    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        paths = {}
        for name, payload in variants.items():
            p = os.path.join(tmp, f"perf_{name}.json")
            with open(p, "w", encoding="utf-8") as f:
                json.dump(payload, f)
            paths[name] = p
        paths["@corrupt"] = os.path.join(tmp, "perf_corrupt.json")
        with open(paths["@corrupt"], "w", encoding="utf-8") as f:
            f.write("{ not json")
        paths["@list"] = os.path.join(tmp, "perf_list.json")
        with open(paths["@list"], "w", encoding="utf-8") as f:
            f.write("[1, 2, 3]")

        for label, variant, flags, expected in cases:
            proc = subprocess.run(
                [sys.executable, os.path.abspath(__file__), paths[variant]] + flags,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            ok = proc.returncode == expected
            print(f"{'PASS' if ok else 'FAIL'}: exit {proc.returncode} (expected {expected}) -- {label}")
            if not ok:
                failed += 1
                print(proc.stdout.decode("utf-8", "replace"))

    if failed:
        print(f"\nselftest: {failed} of {len(cases)} case(s) FAILED.", file=sys.stderr)
        return 1
    print(f"\nselftest: all {len(cases)} exit-code cases hold.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "json_path", nargs="?", help="Path to a perf_<timestamp>.json file written by -VoxelPerfRun."
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="Assert this script's own exit-code contract against fabricated summaries and exit. Needs no "
        "editor, no leg and no artifact -- this is what CI runs.",
    )
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
    parser.add_argument(
        "--allow-inadmissible-timing",
        action="store_true",
        help="Gate frame-time thresholds even when the JSON says frameTimingAdmissible=0 (an IMAGE leg, whose "
        "p95/max/hitches carry screenshot stalls). Off by default; say so on the command line if you mean it.",
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.json_path:
        parser.error("a perf_<timestamp>.json path is required (or --selftest)")

    try:
        with open(args.json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"FAIL: could not read/parse {args.json_path}: {exc}", file=sys.stderr)
        return 2

    if not isinstance(data, dict):
        print(
            f"FAIL: {args.json_path} parsed as {type(data).__name__}, not a JSON object -- "
            f"this is not a -VoxelPerfRun summary.",
            file=sys.stderr,
        )
        return 2

    checks = [
        ("--max-p95-ms", args.max_p95_ms, "p95FrameMs", lambda v, t: v <= t),
        ("--max-hitches", args.max_hitches, "hitchCount", lambda v, t: v <= t),
        ("--max-max-ms", args.max_max_ms, "maxFrameMs", lambda v, t: v <= t),
        ("--min-avg-chunks-per-sec", args.min_avg_chunks_per_sec, "avgChunksPerSec", lambda v, t: v >= t),
        ("--max-post-warmup-p95-ms", args.max_post_warmup_p95_ms, "postWarmupP95FrameMs", lambda v, t: v <= t),
        ("--max-post-warmup-hitches", args.max_post_warmup_hitches, "postWarmupHitchCount", lambda v, t: v <= t),
    ]

    requested = [(flag, threshold, field, ok) for flag, threshold, field, ok in checks if threshold is not None]

    # ---- UNTESTED-IS-NOT-A-PASS GUARDS ------------------------------------
    # Each of these returns 2. None of them is a verdict on the run; all of
    # them say the run was not gated. See the exit-code contract above.
    if not requested:
        print("Dumping summary:")
        print(json.dumps(data, indent=2))
        print(
            "\nFAIL (exit 2): no thresholds supplied = nothing was tested; this is a configuration error, "
            "not a pass. Pass at least one of the --max-*/--min-* flags.",
            file=sys.stderr,
        )
        return 2

    timing_requested = sorted({field for _, _, field, _ in requested if field in TIMING_FIELD_SAMPLE})

    if timing_requested and not args.allow_inadmissible_timing:
        admissible = data.get("frameTimingAdmissible")
        if admissible is not None and admissible == 0:
            print(json.dumps(data, indent=2))
            print(
                f"\nFAIL (exit 2): {args.json_path} carries frameTimingAdmissible=0 -- this is an IMAGE leg and "
                f"its p50/p95/max/hitch numbers are NOT admissible as timing (screenshot shutters stall the "
                f"frames they are serviced on). Gating {', '.join(timing_requested)} against them tests the "
                f"shutter, not the frame. Take timings on a leg with no -VoxelPerfShotEveryM=, or pass "
                f"--allow-inadmissible-timing if you truly mean to.",
                file=sys.stderr,
            )
            return 2

    for field in timing_requested:
        sample_field = TIMING_FIELD_SAMPLE[field]
        sample = data.get(sample_field)
        if isinstance(sample, bool) or not isinstance(sample, (int, float)) or sample <= 0:
            print(json.dumps(data, indent=2))
            print(
                f"\nFAIL (exit 2): threshold on {field} cannot be tested -- {sample_field}={sample!r}. A ceiling "
                f"on a frame-time statistic drawn from an empty sample passes no matter what the ceiling is, "
                f"which is a check that cannot fail.",
                file=sys.stderr,
            )
            return 2

    # ---- THE ACTUAL THRESHOLDS --------------------------------------------
    failures = []
    for flag, threshold, field, ok in requested:
        if field not in data:
            print(f"FAIL: {field} missing from {args.json_path} (asked for by {flag}={threshold})")
            failures.append(f"{field} missing from {args.json_path}")
            continue
        value = data[field]
        # A null / string / bool value would either crash the comparison or
        # compare in a way nobody intended. Both mean "the check did not
        # happen", and the caller must hear about that as a failure.
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            print(f"FAIL: {field}={value!r} is not a number, so {flag}={threshold} could not be tested")
            failures.append(f"{field}={value!r} is not a number ({flag}={threshold} could not be tested)")
            continue
        status = "PASS" if ok(value, threshold) else "FAIL"
        print(f"{status}: {field}={value} vs {flag}={threshold}")
        if status == "FAIL":
            failures.append(f"{field}={value} violates {flag}={threshold}")

    print(json.dumps(data, indent=2))

    if failures:
        print(f"\n{len(failures)} threshold(s) violated:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print(f"\nAll thresholds passed ({len(requested)} checked).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
