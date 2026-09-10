"""Bind persistent-builder source-count checks to a completed cache manifest."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def inspect(manifest, log):
    data = json.loads(manifest.read_text())
    text = log.read_text(errors="replace")
    models = data["models"]
    if data["schema"] != 2 or ";authoredLOD=1" not in data["settings"]:
        raise ValueError("Not an authored-LOD-preserving cache")
    if len({r["id"] for r in models}) != len(models) or not models:
        raise ValueError("Missing or duplicate model IDs")
    complete = re.findall(r"DetailBake complete: (\d+) meshes;", text)
    if complete != [str(len(models))]:
        raise ValueError("Missing unique matching completed bake")
    checks = {}
    for path, count, pairs in re.findall(
        r"DetailAuthoredLOD preserved mesh=(\S+) lods=(\d+) authored/builtTriangles:([^\r\n]+)", text
    ):
        if path in checks:
            raise ValueError("Repeated persistent-build proof")
        values = re.findall(r" L(\d+)=(\d+)/(\d+)", pairs)
        if len(values) != int(count) or [int(v[0]) for v in values] != list(range(int(count))):
            raise ValueError("Incomplete LOD proof")
        if any(a != b or int(a) <= 0 for _, a, b in values):
            raise ValueError("Authored/built triangles differ")
        checks[path] = [int(a) for _, a, _ in values]
    if set(checks) != {r["object_path"] for r in models}:
        raise ValueError("Persistent-build proof does not cover every unique mesh")
    for row in models:
        if checks[row["object_path"]] != [v["triangles"] for v in row["mesh_facts"]["lods"]]:
            raise ValueError("Manifest differs from source-count proof: " + row["id"])
    return data


def compare(before, after, log):
    old = json.loads(before.read_text())
    new = inspect(after, log)
    a = {r["id"]: r for r in old["models"]}
    b = {r["id"]: r for r in new["models"]}
    if len(a) != len(old["models"]) or a.keys() != b.keys():
        raise ValueError("Changed or duplicate source inventory")
    changes = []
    for key in sorted(a):
        for field in ("species", "geometry_sha256", "appearance_sha256", "voxel_pitch_um"):
            if a[key][field] != b[key][field]:
                raise ValueError("Changed source identity: " + key + " " + field)
        x, y = [r[key]["mesh_facts"]["lods"] for r in (a, b)]
        if len(x) != len(y) or x[0]["triangles"] != y[0]["triangles"]:
            raise ValueError("Unexpected source LOD count or LOD0 change: " + key)
        changes.append({"id": key, "old_triangles": [v["triangles"] for v in x],
                        "authored_triangles": [v["triangles"] for v in y]})
    return {"schema": 1, "scope": "Source-count preservation at bake time and unchanged VXA/VAC inputs. Not pixel equivalence, fresh-load verification, cooking, VRAM or gameplay performance.",
            "manifest_sha256": hashlib.sha256(after.read_bytes()).hexdigest(),
            "models": len(b), "unique_meshes": len({r["object_path"] for r in b.values()}),
            "changed_lod_models": sum(r["old_triangles"] != r["authored_triangles"] for r in changes),
            "old_last_lod_triangles_sum": sum(r["old_triangles"][-1] for r in changes),
            "authored_last_lod_triangles_sum": sum(r["authored_triangles"][-1] for r in changes),
            "changes": changes}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = compare(args.before, args.after, args.log)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "changes"}, indent=2))
