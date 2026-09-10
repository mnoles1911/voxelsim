"""Read-only verification of installed spring variants, including VXA payloads.

This checks artifact identity, pitch, counts and material IDs, not botanical
geometry. The explicit vegetative list is backed by manual reference findings;
green material alone cannot prove the absence of a green flower-shaped organ.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from forge import inventory, vxa
from forge.understory_profiles import PROFILES

VEGETATIVE = {
    "yarrow", "cyclamen", "timothy", "cocksfoot", "reed-sweet-grass",
    "common-cottongrass", "soft-rush", "water-horsetail", "wood-horsetail",
    "meadow-grass", "dogs-mercury",
    "common-knapweed", "field-scabious", "harebell", "common-milkweed",
    "fireweed", "impatiens", "purple-loosestrife", "marsh-cinquefoil",
    "water-mint", "fringed-water-lily", "water-chestnut", "water-starwort",
    "curled-pondweed",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--require-complete', action='store_true',
                        help='Fail unless all scoped profiles are accepted and installed')
    args = parser.parse_args()
    findings = json.loads((ROOT / "out/understory-review/manual-findings.json").read_text())
    results, failures = [], []
    accepted = {name for name, r in findings.items() if r.get('status') == 'accepted'}
    missing = sorted(set(PROFILES) - accepted)
    if args.require_complete and (missing or accepted - set(PROFILES)):
        failures.append({'error': 'Accepted source scope mismatch', 'missing': missing,
                         'unexpected': sorted(accepted - set(PROFILES))})
    for species, review in sorted(findings.items()):
        if review.get("status") != "accepted":
            continue
        checked = 0
        material_ids = set()
        try:
            authority = json.loads((ROOT / 'library' / species / 'species.json').read_text())
            assert authority['generator_approved'], 'Source not approved'
            assert authority['approved_generator_digest'] == review['generator_digest'], 'Authority review digest'
            assert inventory.generator_digest(authority['baseline_spec']) == review['generator_digest'], 'Current generator drift'
            assert authority['art_baseline'] == {'season': 'spring', 'voxel_mm': 25}, 'Authority appearance baseline'
            assert len(authority['variants']) == 36, 'Authority variant inventory'
            assert {v['seed'] for v in authority['variants']} == set(range(1, 37)), 'Authority seeds'
            assert authority['reference_variant_id'] in {v['id'] for v in authority['variants']}, 'Missing reference instance'
        except (OSError, ValueError, KeyError, AssertionError) as exc:
            failures.append({'species': species, 'error': str(exc)})
        for seed in range(1, 37):
            folder = ROOT / "out/forge-candidates" / species / f"{species}-{seed:04d}"
            # Endorsed variants may move to the authoritative library.
            if not folder.is_dir():
                folder = ROOT / "library" / species / f"{species}-{seed:04d}"
            try:
                meta = json.loads((folder / "meta.json").read_text())
                path = folder / "tree.vxa"
                assert hashlib.sha256(path.read_bytes()).hexdigest() == meta["artifact_hashes"]["tree.vxa"], "VXA hash"
                assert meta["species"] == species and meta["seed"] == seed, "identity"
                assert meta["stats"]["season"] == "spring", "season"
                assert meta["generator_digest"] == review["generator_digest"], "review digest"
                grid = vxa.read(path)
                assert abs(grid.voxel_m - .025) < 1e-9, "25 mm pitch"
                ids, counts = np.unique(grid.data, return_counts=True)
                actual = {str(int(i)): int(n) for i, n in zip(ids, counts) if i}
                assert actual == meta["stats"]["by_material"], "material counts"
                assert sum(actual.values()) == meta["stats"]["voxels"], "voxel count"
                present = {int(i) for i in actual}
                if species in VEGETATIVE:
                    assert present <= {16, 19}, f"non-vegetative material IDs {present - {16, 19}}"
                material_ids.update(present)
                checked += 1
            except (OSError, ValueError, KeyError, AssertionError) as exc:
                failures.append({"species": species, "seed": seed, "error": str(exc)})
        results.append({"species": species, "checked": checked,
                        "material_ids": sorted(material_ids),
                        "vegetative_material_check": species in VEGETATIVE})
    report = {"schema": 1, "accepted_sources": len(results),
              "scope_profiles": len(PROFILES), "missing_sources": missing,
              "checked_variants": sum(r["checked"] for r in results),
              "limitations": "Payload and metadata verification; botanical form and spring flowering decisions still require the recorded visual reference review.",
              "species": results, "failures": failures}
    output = ROOT / "out/understory-review/spring-payload-audit.json"
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "species"}))
    raise SystemExit(bool(failures))


if __name__ == "__main__":
    main()
