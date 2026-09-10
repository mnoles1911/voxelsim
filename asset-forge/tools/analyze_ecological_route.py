"""Verify a pinned route capture; report traversal evidence, never broad acceptance."""
import argparse
import csv
import hashlib
import json
import re
import statistics
import math
from pathlib import Path


def foundation_evidence(results, route, manifest):
    request = route["foundation"]
    summary = json.loads((results / "foundation-summary.json").read_text(encoding="utf-8-sig"))
    for output, pin in (("route_sha256", "routeSha256"), ("configuration_sha256", "configurationSha256"),
                        ("species_manifest_sha256", "speciesManifestSha256")):
        if summary[output].lower() != manifest[pin].lower():
            raise ValueError("Foundation input pin mismatch")
    if any(summary[k] != request[k] for k in ("min_x_voxel", "min_y_voxel", "plane_z_voxel")):
        raise ValueError("Foundation coordinates differ from requested route")
    for field, value in (("schema", 1), ("voxel_mm", 100), ("width_mm", 5000), ("clearance_mm", 3000),
                         ("max_fill_mm_provisional", 500), ("bearing_inspection_mm", 1000)):
        if summary[field] != value:
            raise ValueError(f"Unexpected foundation survey contract: {field}")
    if summary["approved_build_site"] is not False:
        raise ValueError("Survey must not claim structural building approval")
    last = route["waypoints"][-1]
    distance = math.hypot(summary["pawn_x_m"] - last["x_m"], summary["pawn_y_m"] - last["y_m"])
    if not math.isfinite(distance) or distance > last["arrival_radius_m"] + .001:
        raise ValueError("Foundation pawn endpoint was not reached")
    with (results / "foundation-columns.csv").open(encoding="utf-8-sig", newline="") as handle:
        columns = [{k: int(v) for k, v in row.items()} for row in csv.DictReader(handle)]
    x, y = request["min_x_voxel"], request["min_y_voxel"]
    expected = {(x + dx, y + dy) for dy in range(50) for dx in range(50)}
    if len(columns) != 2500 or {(c["x_voxel"], c["y_voxel"]) for c in columns} != expected:
        raise ValueError("Incomplete or duplicate foundation columns")
    for c in columns:
        if c["ground_found"] and c["fill_gap_mm"] != (request["plane_z_voxel"] - 1 - c["first_ground_z_voxel"]) * 100:
            raise ValueError("Foundation ground/plane gap mismatch")
        qualifies = (c["ground_found"] == 1 and c["material_unknown"] == 0 and c["water_known"] == 1
                     and c["water_mm"] == 0 and c["overhead_occupied_voxels"] == 0
                     and c["non_ground_below_plane_voxels"] == 0 and c["bearing_thickness_mm"] == 1000
                     and 0 <= c["fill_gap_mm"] <= 500)
        if bool(c["provisional_criteria"]) != qualifies:
            raise ValueError("Foundation column criterion mismatch")
    counts = {"column_count": len(columns),
              "unknown_columns": sum(bool(c["material_unknown"]) or not c["water_known"] for c in columns),
              "wet_columns": sum(c["water_known"] == 1 and c["water_mm"] > 0 for c in columns),
              "obstructed_columns": sum(bool(c["overhead_occupied_voxels"] or c["non_ground_below_plane_voxels"]) for c in columns),
              "provisional_criteria_columns": sum(c["provisional_criteria"] for c in columns)}
    if any(summary[k] != v for k, v in counts.items()) or summary["complete_known_survey"] != (counts["unknown_columns"] == 0):
        raise ValueError("Foundation summary disagrees with column evidence")
    return dict(counts, endpoint_distance_m=distance, approved_build_site=False,
                scope="One reached footprint, 0.5m maximum fill and 1m finite bearing inspection; deeper caves and structural suitability unexamined.")


def analyze(root):
    manifest = json.loads((root / "run-manifest.json").read_text(encoding="utf-8-sig"))
    args = manifest["arguments"]

    def argument(name):
        return Path(next(a.split("=", 1)[1] for a in args if a.startswith(name + "=")))

    def pinned(path, field):
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual.lower() != manifest[field].lower():
            raise ValueError(f"Hash mismatch: {field}: {path}")

    source = argument("-VoxelEcologyRoute")
    pinned(source, "routeSha256")
    pinned(argument("-VoxelEcologyConfig"), "configurationSha256")
    assets = argument("-VoxelAssetDir")
    pinned(assets / "species.vxm", "speciesManifestSha256")
    results = root / "results"
    pinned(results / "route.json", "routeSha256")
    route = json.loads(source.read_text())
    log = (root / "unreal.log").read_text(errors="replace")
    if "detailCacheManifestSha256" in manifest:
        pinned(argument("-VoxelDetailMeshCache"), "detailCacheManifestSha256")
        if not re.search(r"DetailCache.*accepted", log, re.I):
            raise ValueError("Requested cache acceptance missing")
    count = len(route["waypoints"])
    arrivals = list(map(int, re.findall(r"VoxelRoute WALK_END arrived=(\d+)", log)))
    if arrivals != list(range(count)):
        raise ValueError(f"Incomplete or reordered arrivals: {arrivals}")
    result = (results / "result.txt").read_text()
    if "VoxelRoute COMPLETE PASS" not in log or f"arrived={count}/{count}" not in result:
        raise ValueError("Terminal route success missing")
    for index in arrivals:
        png = (results / f"checkpoint-{index:02d}.png").read_bytes()
        if not png.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError(f"Missing valid checkpoint image {index}")
    with (results / "route-samples.csv").open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows or any(float(b["seconds"]) <= float(a["seconds"])
                       for a, b in zip(rows, rows[1:])):
        raise ValueError("Empty or nonmonotonic trajectory")
    sampled = {int(r["waypoint"]) for r in rows}
    starts = dict((int(i), float(t)) for i, t in re.findall(
        r"VoxelRoute WALK_BEGIN point=(\d+) mono=([\d.]+)", log))
    ends = dict((int(i), float(t)) for i, t in re.findall(
        r"VoxelRoute WALK_END arrived=(\d+) mono=([\d.]+)", log))
    missing = sorted(set(arrivals) - sampled)
    # Older captures sample every 0.5 s and omit subinterval legs. Report this
    # explicitly; never excuse a longer missing trajectory or unknown point.
    if sampled - set(arrivals) or any(i not in starts or ends[i] - starts[i] > .5 for i in missing):
        raise ValueError("Missing trajectory for a leg longer than the sample interval")
    report = {
        "status": "passed", "pinsVerified": True, "arrivals": count,
        "checkpointImages": count, "trajectorySamples": len(rows),
        "legsShorterThanSampleIntervalWithoutCsvRows": missing,
        "actualSpeedMedianMps": statistics.median(float(r["actual_speed_m_s"]) for r in rows),
        "groundedSamples": sum(int(r["grounded"]) for r in rows),
        "waitingSamples": sum(int(r["waiting"]) for r in rows),
        "heightRangeM": [min(float(r["z_m"]) for r in rows), max(float(r["z_m"]) for r in rows)],
        "terminalResult": result.strip(),
        "authoredRouteScope": route.get("scope", "Unspecified authored route"),
        "scope": "One authored pawn route; no general navigation, sightline, building-support or performance acceptance."
    }
    if "foundation" in route:
        report["foundation"] = foundation_evidence(results, route, manifest)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    root = parser.parse_args().directory.resolve()
    report = analyze(root)
    (root / "route-analysis.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
