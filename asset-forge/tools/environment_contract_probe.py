"""Audit all environment specs without changing specs, banks or library files.

python tools/environment_contract_probe.py [--out /path/to/report.json]
An output path is optional: the default prints a summary and writes nothing.
"""
from __future__ import annotations
import argparse
from collections import Counter
import copy
import hashlib
import json
import math
from pathlib import Path
import struct

import _path  # noqa: F401
from forge import categories, environment_contract as contract, manifest, spec as sm

ROOT = Path(__file__).resolve().parents[1]


def audit(specs_dir: Path) -> dict:
    entries, failures, warnings = [], [], []
    for path in sorted(specs_dir.glob("*.json")):
        body, report = sm.load(path)
        category = categories.of(body)
        if category is None:
            failures.append(f"{path.stem}: illegible category")
            continue
        if category != "environment":
            continue
        original = copy.deepcopy(body)
        try:
            entry = contract.describe(path.stem, body)
        except (ValueError, KeyError) as exc:
            failures.append(str(exc))
            continue
        if body != original:
            failures.append(f"{path.stem}: descriptor mutated the spec")
        if entry["admission_concerns"]:
            warnings.append({"name": path.stem, "concerns": entry["admission_concerns"]})
        if report.warnings:
            warnings.append({"name": path.stem, "validation": report.warnings})
        entries.append(entry)
    declared = set(categories.BY_KEY["environment"].kinds)
    if declared != set(contract.PROFILES):
        failures.append(f"family coverage drift: declared={sorted(declared)}, mapped={sorted(contract.PROFILES)}")
    if declared != set(manifest.KINDS_ON_SCATTER):
        failures.append("manifest environment kinds differ from category contract")
    ids = [e["asset_id"] for e in entries]
    if len(ids) != len(set(ids)):
        failures.append("duplicate stable asset IDs")
    counts = {}
    for kind in sorted(declared):
        group = [e for e in entries if e["generator_kind"] == kind]
        counts[kind] = {
            "count": len(group),
            "pitches_um": dict(sorted(Counter(str(e["voxel_pitch_um"]) for e in group).items())),
            "terrain": sum(e["current_manifest"]["terrain_lattice"] for e in group),
            "detail": sum(not e["current_manifest"]["terrain_lattice"] for e in group),
        }
    canonical = json.dumps(entries, sort_keys=True, separators=(",", ":")).encode()
    return {"format": "asset-forge-environment-contract", "version": contract.VERSION,
            "count": len(entries), "coverage_catalog_sha256": hashlib.sha256(canonical).hexdigest(),
            "kinds": counts, "failures": failures, "warnings": warnings, "assets": entries}


def self_test(report: dict, specs_dir: Path) -> None:
    """Test arbitrary names and classification/geometry revision separation."""
    for kind in contract.PROFILES:
        match = next(e for e in report["assets"] if e["generator_kind"] == kind)
        body, _ = sm.load(specs_dir / f"{match['spec_name']}.json")
        # Generic initialization must never depend on the four prototype names.
        named = contract.describe(f"contract-unlisted-{kind}", body)
        assert named["generator_kind"] == kind
        assert named["asset_id"] == f"forge:environment:contract-unlisted-{kind}"
        assert named["spec_hash"] == sm.spec_hash(body)
        mapped = contract.runtime_descriptor(named, "0" * 32, 7)
        assert mapped["SpecId"] == f"contract-unlisted-{kind}" and mapped["Kind"] == kind
        assert mapped["Category"] == "environment" and not mapped["Legacy"]
        changed = copy.deepcopy(body)
        changed["category"] = "environment"
        assert contract.describe(match["spec_name"], changed)["spec_hash"] == match["spec_hash"]
        changed["category"] = "craftable"
        try:
            contract.describe(match["spec_name"], changed)
        except ValueError:
            pass
        else:
            raise AssertionError("non-environment category override was accepted")
    assert all(e["voxel_pitch_um"] in (100000, 50000, 25000) for e in report["assets"])


def audit_bank_headers(report: dict, banks: Path) -> dict:
    """Read 48-byte VXA headers only; do not decode, hash or regenerate grids."""
    files, malformed, absent, pitch_mismatches = [], [], [], []
    for entry in report["assets"]:
        name = entry["spec_name"]
        paths = sorted((banks / name).glob(f"{name}-*.vxa"))
        if not paths:
            absent.append(name)
        for path in paths:
            with path.open("rb") as handle:
                header = handle.read(48)
            if len(header) != 48:
                malformed.append(str(path))
                continue
            magic, version = struct.unpack_from("<II", header)
            dimensions = struct.unpack_from("<III", header, 20)
            wire_pitch = struct.unpack_from("<I", header, 32)[0]
            if magic != 0x31415856 or version not in (3, 4) or min(dimensions) <= 0 or wire_pitch <= 0:
                malformed.append(str(path))
                continue
            pitch_um = wire_pitch * 1000 if version == 3 else wire_pitch
            cells = math.prod(dimensions)
            item = {"name": name, "kind": entry["generator_kind"], "path": str(path),
                    "dimensions": list(dimensions), "pitch_um": pitch_um,
                    "dense_cells": cells, "dense_material_mib": cells / (1024 * 1024),
                    "file_bytes": path.stat().st_size,
                    "exceeds_64_micells": cells > 64 * 1024 * 1024,
                    "exceeds_4096_axis": max(dimensions) > 4096}
            files.append(item)
            if pitch_um != entry["voxel_pitch_um"]:
                pitch_mismatches.append(item)
    files.sort(key=lambda item: item["dense_cells"], reverse=True)
    finer = [item for item in files if item["pitch_um"] == 100000 and item["dense_cells"] * 8 > 64 * 1024 * 1024]
    return {"bank_root": str(banks), "files_checked": len(files),
            "species_with_banks": report["count"] - len(absent),
            "species_without_banks": absent, "malformed_headers": malformed,
            "pitch_mismatches": pitch_mismatches,
            "over_runtime_grid_limits": [item for item in files if item["exceeds_64_micells"] or item["exceeds_4096_axis"]],
            "largest_ten": files[:10],
            "hypothetical_50mm_dense_limit": {
                "estimate_only": "halve 100 mm pitch at fixed physical bounds: 8x dense cells; no rebakes or performance measurements",
                "files_over_64_micells": len(finer),
                "species": sorted({item["name"] for item in finer}),
                "largest_estimated_micells": max((item["dense_material_mib"] * 8 for item in finer), default=0)},
            "scope": "header-only inventory; no body integrity, spec freshness, curation or visual validation"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--specs", type=Path, default=ROOT / "specs")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--banks", type=Path, default=ROOT / "out" / "engine" / "banks")
    args = parser.parse_args()
    report = audit(args.specs)
    if not report["failures"]:
        self_test(report, args.specs)
    report["bank_headers"] = audit_bank_headers(report, args.banks)
    summary = {key: value for key, value in report.items() if key != "assets"}
    print(json.dumps(summary, indent=2, sort_keys=True))
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return int(bool(report["failures"]))


if __name__ == "__main__":
    raise SystemExit(main())
