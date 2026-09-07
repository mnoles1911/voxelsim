#!/usr/bin/env python3
"""Summarize measured GPU asset request work; never infer frame latency from windows."""
import argparse
import json
import re
from pathlib import Path

MARKER = "Voxel gpu asset preparation (window): "
FIELDS = ("resolveMs", "spanLookupBuildMs", "tableCopyRebaseMs", "otherMs",
          "spanHits", "spanMisses", "copiedBytes")
PAIR = re.compile(r"(\w+)=(-?\d+(?:\.\d+)?)")
OPTIONAL_FIELDS = ("zCulled", "submitted")
REQUESTS = re.compile(r"Voxel gpu submit split \(window\): .*?calls=(\d+) perCallUs=")


def summarize(text):
    windows = []
    pending_requests = None
    for line in text.splitlines():
        request_match = REQUESTS.search(line)
        if request_match:
            pending_requests = int(request_match.group(1))
        if MARKER not in line:
            continue
        values = dict(PAIR.findall(line.split(MARKER, 1)[1]))
        if any(key not in values for key in FIELDS):
            raise ValueError("Incomplete asset preparation receipt")
        row = {key: float(values[key]) if key.endswith("Ms") else int(values[key])
               for key in FIELDS}
        if pending_requests is not None:
            row["requests"] = pending_requests
            pending_requests = None
        if any(key in values for key in OPTIONAL_FIELDS):
            if not all(key in values for key in OPTIONAL_FIELDS):
                raise ValueError("Incomplete vertical-culling receipt")
            row.update({key: int(values[key]) for key in OPTIONAL_FIELDS})
        if any(value < 0 for key, value in row.items() if key != "otherMs"):
            raise ValueError("Negative measured work or counter")
        # Small rounding noise is expected; materially negative residual means
        # the outer and inner brackets do not describe the same submissions.
        if row["otherMs"] < -0.2:
            raise ValueError("Asset timing brackets do not reconcile")
        windows.append(row)
    if not windows:
        raise ValueError("No asset preparation receipts; run the instrumented build")
    totals = {key: sum(row[key] for row in windows) for key in FIELDS}
    if any("zCulled" in row for row in windows):
        if not all("zCulled" in row for row in windows):
            raise ValueError("Mixed instrumentation versions in one log")
        totals.update({key: sum(row[key] for row in windows) for key in OPTIONAL_FIELDS})
    total_ms = sum(totals[key] for key in FIELDS if key.endswith("Ms"))
    result = {"windows": len(windows), "totals": totals,
            "measured_asset_ms": total_ms,
            "largest_window_ms": max(sum(row[key] for key in FIELDS if key.endswith("Ms"))
                                     for row in windows),
            "limitation": "Window totals are not frame times or latency percentiles."}
    if any("requests" in row for row in windows):
        if not all("requests" in row for row in windows):
            raise ValueError("Missing paired request-count receipts")
        count = sum(row["requests"] for row in windows)
        if count <= 0:
            raise ValueError("Measured windows have no requests")
        totals["requests"] = count
        result["per_request"] = {"asset_us": total_ms * 1000 / count,
                                 "resolve_us": totals["resolveMs"] * 1000 / count,
                                 "table_copy_us": totals["tableCopyRebaseMs"] * 1000 / count,
                                 "copied_bytes": totals["copiedBytes"] / count}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(summarize(args.log.read_text(encoding="utf-8", errors="replace")), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"Asset preparation analysis refused: {error}\n")
