"""Analyze the opt-in R0 entry diagnostic (-VoxelR0EntryProfile) from a capture's frames.csv.

The diagnostic (docs/handoff-patches/r0-entry-profile) emits per-frame ACCUMULATED
inclusive game-thread timers for the R0 admission entry and its nested slices,
plus counters. Nesting, from the patch README: Entry > Footprint > Memo > Compute >
Resolve; Sky is a second Footprint child; Nearest runs after the sweep (disjoint
from Footprint); Prefetch (RequestFootprint) may nest under admission or Nearest.

Rules this analyzer enforces, fail-closed:
  * the Entry series must exist and be unambiguous (per-thread duplicates of the
    same display name are refused, never summed);
  * every sample is finite and non-negative;
  * per frame, inclusive nesting holds within a tolerance:
      Resolve <= Compute <= Memo,  Memo + Sky <= Footprint,
      Footprint + Nearest <= Entry,  Prefetch <= Entry.
Per-frame maxima are reported per stage and MUST NOT be summed across stages: the
maximum frames differ. The five-second log lines carry slice-window maxima with
the same caveat; they are counted here, not aggregated.
"""
import argparse
import json
import math
import statistics
from pathlib import Path

from analyze_ecological_route_frames import frame_rows

STAGES = ["Entry", "Footprint", "Memo", "Compute", "Resolve", "Sky", "Nearest", "Prefetch"]
COUNTERS = ["ZCells", "Evaluations", "MemoHits", "EpochInvalidations"]
PREFIX = "VoxelStream/GameThread/R0Entry"
LOG_MARKER = "R0EntryProfile inclusive slice-window-max ms:"
NESTING = [  # (label, lhs stages summed, rhs stage) : sum(lhs) <= rhs + tolerance
    ("resolve<=compute", ["Resolve"], "Compute"),
    ("compute<=memo", ["Compute"], "Memo"),
    ("memo+sky<=footprint", ["Memo", "Sky"], "Footprint"),
    ("footprint+nearest<=entry", ["Footprint", "Nearest"], "Entry"),
    ("prefetch<=entry", ["Prefetch"], "Entry"),
]


def _column(kind, name):
    return PREFIX + name + kind


def _number(row, column, frame):
    raw = row.get(column, "0")
    try:
        value = float(raw) if raw not in ("", None) else 0.0
    except ValueError as exc:
        raise ValueError(f"Non-numeric {column} at frame {frame}: {raw!r}") from exc
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"Invalid {column} at frame {frame}: {raw!r}")
    return value


def _percentiles(values):
    if not values:
        return {"median": 0.0, "p95": 0.0, "max": 0.0}
    values = sorted(values)
    p = .95 * (len(values) - 1)
    lo = math.floor(p)
    return {"median": statistics.median(values),
            "p95": values[lo] + (values[math.ceil(p)] - values[lo]) * (p - lo),
            "max": values[-1]}


def analyze(frames_csv, log_path=None, tolerance_ms=0.05):
    frames_csv = Path(frames_csv)
    diagnostics = {}
    rows = list(frame_rows(frames_csv, diagnostics))
    wanted = ([_column("Ms", s) for s in STAGES] + [_column("Calls", s) for s in STAGES]
              + [_column("", c) for c in COUNTERS])
    ambiguous = sorted(k for k in diagnostics.get("ambiguous_columns_excluded", {}) if k in wanted)
    if ambiguous:
        raise ValueError(f"Ambiguous per-thread duplicates of R0 entry series: {ambiguous}")
    if not rows:
        raise ValueError("No frame rows")
    present = {c for c in wanted if c in rows[-1]}
    if _column("Ms", "Entry") not in present or _column("Calls", "Entry") not in present:
        raise ValueError("R0 entry profile absent from frames.csv (was -VoxelR0EntryProfile passed?)")
    missing = sorted(set(wanted) - present)

    per_frame = {s: [] for s in STAGES}
    calls = {s: 0 for s in STAGES}
    counters = {c: 0.0 for c in COUNTERS}
    violations = {label: {"frames": 0, "worst_excess_ms": 0.0} for label, _, _ in NESTING}
    entry_frames = 0
    residual = []
    for index, row in enumerate(rows):
        ms = {s: _number(row, _column("Ms", s), index) for s in STAGES}
        c = {s: _number(row, _column("Calls", s), index) for s in STAGES}
        for s in STAGES:
            if c[s] != int(c[s]):
                raise ValueError(f"Fractional call count for {s} at frame {index}")
            calls[s] += int(c[s])
        for name in COUNTERS:
            counters[name] += _number(row, _column("", name), index)
        if c["Entry"] == 0 and ms["Entry"] == 0:
            continue
        entry_frames += 1
        for s in STAGES:
            per_frame[s].append(ms[s])
        residual.append(ms["Entry"] - ms["Footprint"] - ms["Nearest"])
        for label, lhs, rhs in NESTING:
            excess = sum(ms[s] for s in lhs) - ms[rhs]
            if excess > tolerance_ms:
                violations[label]["frames"] += 1
                violations[label]["worst_excess_ms"] = max(violations[label]["worst_excess_ms"], excess)

    log_lines = 0
    if log_path is not None:
        log_lines = Path(log_path).read_text(errors="replace").count(LOG_MARKER)

    stages = {}
    for s in STAGES:
        stats = _percentiles(per_frame[s])
        stages[s] = {"total_ms": sum(per_frame[s]), "calls": calls[s],
                     "per_frame_median_ms": stats["median"], "per_frame_p95_ms": stats["p95"],
                     "per_frame_max_ms": stats["max"]}
    return {
        "schema": 1,
        "scope": ("Game-thread inclusive R0 entry slices accumulated per frame. Per-stage maxima are from "
                  "different frames and must not be summed; subtract only contemporaneous disjoint children."),
        "frames_total": len(rows),
        "frames_with_entry": entry_frames,
        "tolerance_ms": tolerance_ms,
        "missing_series": missing,
        "stages": stages,
        "counters": counters,
        "entry_residual_ms": {"total": sum(residual), **_percentiles(residual)},
        "nesting_violations": violations,
        "nesting_ok": all(v["frames"] == 0 for v in violations.values()),
        "log_window_lines": log_lines,
        "csv_diagnostics": diagnostics,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=Path,
                        help="capture directory (frames.csv + game.log) or a frames.csv path")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--tolerance-ms", type=float, default=0.05)
    args = parser.parse_args()
    root = args.capture
    frames = root if root.suffix == ".csv" else root / "frames.csv"
    log = None
    if root.suffix != ".csv" and (root / "game.log").exists():
        log = root / "game.log"
    result = analyze(frames, log, args.tolerance_ms)
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
