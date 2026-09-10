"""Analyze opt-in actual-route CSV labels, independently of screenshot waits."""
import argparse
from collections import Counter
import csv
import json
import math
from pathlib import Path
import re
import statistics


def frame_rows(path, diagnostics=None):
    # UE's streaming CSV writer appends newly registered series over time and
    # writes the complete header at the end (CsvProfiler.cpp::Finalize). Earlier
    # rows legitimately have fewer columns; unregistered timing series are zero.
    with path.open(encoding="utf-8-sig", newline="") as stream:
        raw = list(csv.reader(stream))
    if not raw:
        raise ValueError("Empty frame CSV")
    header = raw[0]
    headers = [(0, header)]
    for i, row in enumerate(raw[1:], 1):
        if row and row[0] == header[0] and "FrameTime" in row:
            if row[:len(header)] != header:
                raise ValueError("Inconsistent expanded CSV header")
            header = row
            headers.append((i, row))
    header_indices = {i for i, _ in headers}
    counts = Counter(header)
    duplicates = {k: [i for i, name in enumerate(header) if name == k]
                  for k, count in counts.items() if count > 1}
    primary = {"FrameTime", "GameThreadTime", "GPUTime", "RenderThreadTime",
               "VoxelStream/TickMs", "VoxelStream/SubmitMs"}
    if any(k.startswith("VoxelRoute/") or k in primary for k in duplicates):
        raise ValueError("Ambiguous duplicate measured primary/route CSV column")
    # UE custom statistics may produce per-thread series with identical display
    # names. Do not overwrite, sum or call them GT substages without provenance.
    # Preserve unique primary metrics and explicitly disclose omitted series.
    if diagnostics is not None:
        diagnostics["ambiguous_columns_excluded"] = duplicates
        diagnostics["duplicate_policy"] = "Exclude every occurrence of ambiguous nonprimary names; primary/route duplicates are fatal. No inferred thread sums."
    unique_columns = [(i, name) for i, name in enumerate(header) if counts[name] == 1]
    for i, row in enumerate(raw):
        if i in header_indices or row and row[0].startswith("["):
            continue
        if len(row) > len(header):
            raise ValueError("Frame exceeds final CSV header")
        result = {name: row[index] if index < len(row) else
                  ("" if name.startswith("VoxelRoute/") else "0")
                  for index, name in unique_columns}
        yield result


def summary(values):
    values = sorted(values)
    p = .95 * (len(values) - 1)
    lo = math.floor(p)
    return {"median": statistics.median(values),
            "p95": values[lo] + (values[math.ceil(p)] - values[lo]) * (p - lo),
            "max": values[-1]}


def analyze_frames(path, log):
    begins = re.findall(r"VoxelRoute PROFILE_BEGIN mono=([\d.]+)", log)
    ends = re.findall(r"VoxelRoute PROFILE_END mono=([\d.]+)", log)
    if len(begins) != 1 or len(ends) != 1 or "VoxelRoute PROFILE_SAVED ok=1 " not in log:
        raise ValueError("Missing unique completed frame profile")
    origin, finish = float(begins[0]), float(ends[0])
    if not 0 < finish - origin < 7200:
        raise ValueError("Invalid profile interval")
    starts = [(int(i), float(t)) for i, t in re.findall(
        r"VoxelRoute WALK_BEGIN point=(\d+) mono=([\d.]+)", log)]
    stops = [(int(i), float(t)) for i, t in re.findall(
        r"VoxelRoute WALK_END arrived=(\d+) mono=([\d.]+)", log)]
    if not starts or [i for i, _ in starts] != list(range(len(starts))) or [i for i, _ in stops] != [i for i, _ in starts]:
        raise ValueError("Incomplete or duplicate walking intervals")
    intervals = {i: (a, b) for (i, a), (_, b) in zip(starts, stops)}
    if any(not origin <= a <= b <= finish for a, b in intervals.values()):
        raise ValueError("Walking interval outside profile")
    csv.field_size_limit(32 * 1024 * 1024)
    frames, labelled, selected, labelled_indices, missing_indices = [], [], [], [], []
    last_frame, last_elapsed = -1, -1.
    csv_diagnostics = {}
    for row in frame_rows(path, csv_diagnostics):
            try:
                dt = float(row["FrameTime"])
            except (KeyError, TypeError, ValueError):
                raise ValueError("Malformed frame duration")
            if not math.isfinite(dt) or dt <= 0:
                raise ValueError("Invalid frame duration")
            frames.append(dt)
            if row.get("VoxelRoute/CaptureFrame", "") == "":
                missing_indices.append(len(frames) - 1)
                continue  # Before the first route tick or after it stops.
            fields = [float(row["VoxelRoute/" + k]) for k in ("CaptureFrame", "ElapsedSeconds", "Walking", "Point")]
            frame, elapsed, walking, point = fields
            if any(not math.isfinite(v) for v in fields) or frame != int(frame) or frame <= last_frame or elapsed < last_elapsed or elapsed < 0:
                raise ValueError("Invalid or nonmonotonic route frame labels")
            if walking not in (0, 1) or point != int(point) or elapsed > finish - origin + 1:
                raise ValueError("Invalid route phase labels")
            last_frame, last_elapsed = frame, elapsed
            labelled.append(row)
            labelled_indices.append(len(frames) - 1)
            if not walking:
                continue
            if int(point) not in intervals:
                raise ValueError("Unknown walking point")
            a, b = intervals[int(point)]
            if not a - .002 <= origin + elapsed <= b + .002:
                raise ValueError("Walking label outside independent event interval")
            selected.append((int(point), row))
    if not frames or not labelled or not selected:
        raise ValueError("No measured walking frames")
    if any(labelled_indices[0] < i < labelled_indices[-1] for i in missing_indices):
        raise ValueError("Missing labels inside the route capture")
    if abs(sum(frames) / 1000 - (finish - origin)) > 1:
        raise ValueError("CSV/profile event clock mismatch")
    # UE's default FrameTime measures the prior logical frame at the next
    # UpdateFrameTime call, whereas route/custom tick labels describe this tick.
    # Keep only interior rows whose previous tick was also walking the same leg.
    # This avoids attributing transition durations to walking without pretending
    # the CPU/GPU timing columns share an identical per-row sampling boundary.
    by_frame = {int(float(r["VoxelRoute/CaptureFrame"])): r for r in labelled}
    candidate_count = len(selected)
    selected = [(point, row) for point, row in selected
                if (prev := by_frame.get(int(float(row["VoxelRoute/CaptureFrame"])) - 1))
                and float(prev["VoxelRoute/Walking"]) == 1
                and int(float(prev["VoxelRoute/Point"])) == point]
    if not selected:
        raise ValueError("No interior walking frames")
    # Transition-only legs may contain no fully walking frame. Never hide a
    # longer unsampled leg behind that exception.
    omitted = [i for i, (a, b) in intervals.items() if not any(p == i for p, _ in selected)]
    if any(intervals[i][1] - intervals[i][0] > .2 for i in omitted):
        raise ValueError("Missing walking frames for a nontrivial leg")
    metrics = [k for k in selected[0][1] if isinstance(k, str) and
               (k in ("FrameTime", "GameThreadTime", "RenderThreadTime", "GPUTime") or
                k.startswith(("VoxelStream/", "VoxelWorklist/")) and k.endswith("Ms"))]

    def measure(rows):
        result = {}
        for field in metrics:
            values = [float(r[field]) for r in rows]
            if any(not math.isfinite(v) or v < 0 for v in values):
                raise ValueError("Invalid measured timing")
            result[field] = summary(values)
        return {"frames": len(rows), "metrics_ms": result}

    return {"schema": 1, "scope": "Frame-labelled movement on one authored route; screenshot and transition frames excluded. No general navigation or default-range performance acceptance.",
            "csv_column_diagnostics": csv_diagnostics,
            "profile_elapsed_seconds": finish - origin, "csv_frames": len(frames),
            "labelled_frames": len(labelled), "unlabelled_frames": len(frames) - len(labelled),
            "boundary_rows_excluded": candidate_count - len(selected),
            "timing_semantics": "Interior walking rows only. Default UE FrameTime is prior-logical-frame duration; custom CPU scopes and GPU statistics must not be assumed causally aligned in the same row.",
            "transition_only_legs_without_walking_frames": omitted,
            "walking": measure([r for _, r in selected]),
            "legs": {str(i): measure([r for p, r in selected if p == i]) for i in intervals if i not in omitted}}


def analyze(root):
    from analyze_ecological_route import analyze as analyze_route
    route = analyze_route(root)
    manifest = json.loads((root / "run-manifest.json").read_text(encoding="utf-8-sig"))
    if "-VoxelEcologyRouteProfile" not in manifest["arguments"]:
        raise ValueError("Capture did not request route profiling")
    result = analyze_frames(root / "results/route-frames.csv", (root / "unreal.log").read_text(errors="replace"))
    result["route"] = {k: route[k] for k in ("pinsVerified", "arrivals", "trajectorySamples", "groundedSamples", "waitingSamples")}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    root = parser.parse_args().directory.resolve()
    report = analyze(root)
    (root / "route-frame-analysis.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
