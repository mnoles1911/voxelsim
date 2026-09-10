"""Compare completed routes while checking equivalent scene inputs explicitly."""
import argparse
import hashlib
import json
from pathlib import Path

from analyze_ecological_route_frames import analyze


def load_manifest(root):
    return json.loads((root / "run-manifest.json").read_text(encoding="utf-8-sig"))


def cache_models(run):
    path = Path(next(a.split("=", 1)[1] for a in run["arguments"] if a.startswith("-VoxelDetailMeshCache=")))
    if hashlib.sha256(path.read_bytes()).hexdigest().upper() != run["detailCacheManifestSha256"].upper():
        raise ValueError("Cache manifest changed after capture")
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    rows = [(m["id"], m["geometry_sha256"], m["appearance_sha256"], m["mesh_facts"]["attribute_sha256"])
            for m in data["models"]]
    if len({r[0] for r in rows}) != len(rows):
        raise ValueError("Duplicate cache model ids")
    return sorted(rows)


def compare(before, after):
    runs = [load_manifest(p) for p in (before, after)]
    for key in ("routeSha256", "configurationSha256", "speciesManifestSha256"):
        if runs[0][key].upper() != runs[1][key].upper():
            raise ValueError(f"Different scene input: {key}")
    def args(run):
        # Diagnostics run only after an aborted route; analyze() below requires
        # successful completion. Cache files may differ only if models match.
        ignored = ("-UserDir=", "-abslog=", "-VoxelEcologyRouteOutput=", "-VoxelDetailMeshCache=")
        # Policy flags whose OFF/ON is the comparison itself; anything else differing is a scene change.
        policy = ("-VoxelDetailSizeCull", "-VoxelDetailRetireUnused")
        return [a for a in run["arguments"] if not a.startswith(ignored) and a != "-VoxelEcologyRouteDiagnoseStalls" and a not in policy]
    if args(runs[0]) != args(runs[1]):
        raise ValueError("Different gameplay arguments outside allowed artifact paths")
    models = [cache_models(r) for r in runs]
    if models[0] != models[1]:
        raise ValueError("Cache source/appearance/render-attribute models differ")
    reports = [analyze(p) for p in (before, after)]
    metrics = {}
    for field in ("FrameTime", "GameThreadTime", "GPUTime", "VoxelStream/TickMs", "VoxelStream/SubmitMs"):
        a, b = [r["walking"]["metrics_ms"][field] for r in reports]
        metrics[field] = {"before_ms": a, "after_ms": b,
                          "change_percent": {k: 100 * (b[k] / a[k] - 1) if a[k] else None for k in a}}
    return {"schema": 1, "before": str(before.resolve()), "after": str(after.resolve()),
            "scope": "One same-route editor comparison of combined builds, not an isolated optimization A/B, repeated-run confidence, culling or default256m acceptance.",
            "matching_scene_pins": True, "matching_cache_model_fingerprints": len(models[0]),
            "runtime_module_hashes": [r.get("runtimeModuleHashes") for r in runs],
            "walking_frames": [r["walking"]["frames"] for r in reports],
            "route_evidence": [r["route"] for r in reports],
            "csv_column_diagnostics": [r["csv_column_diagnostics"] for r in reports], "metrics": metrics}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()
    report = compare(args.before, args.after)
    (args.after / "route-frame-comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
