"""Check raft geometry and export seed 1 for review, without a curation verdict.

Run from asset-forge: python tools/raftprobe.py
"""
import json
import hashlib
from pathlib import Path

import numpy as np
from scipy import ndimage

import _path  # noqa: F401
from forge import pipeline, render, spec, vox, vxa


def main():
    root = Path(__file__).resolve().parents[1]
    body, report = spec.load(root / "specs/raft.json")
    assert not report.warnings, report.warnings
    output = root / "out/artifact"
    output.mkdir(parents=True, exist_ok=True)
    results = []
    digests = []
    for seed in (1, 2, 3, 4):
        asset = pipeline.build(body, seed)
        grid = asset.grid
        digests.append(hashlib.sha256(grid.data.tobytes()).hexdigest())
        assert not pipeline.health(asset), pipeline.health(asset)
        assert grid.voxel_m == 0.025, "raft must use 25 mm cubes"
        occupied = grid.data != 0
        _, pieces = ndimage.label(occupied)
        assert pieces == 1, f"seed {seed}: {pieces} detached pieces"
        assert asset.stats.get("bridges_added", 0) == 0, "generator needed repair"
        bounds = np.array(grid.shape) * grid.voxel_m
        assert 3.8 <= bounds[0] <= 4.4 and 1.9 <= bounds[1] <= 2.2, bounds
        # Central half: broad continuous timber coverage and modest deck relief.
        nx, ny, nz = grid.shape
        deck = occupied[nx // 4:3 * nx // 4, 3:ny - 3]
        coverage = float(deck.any(axis=2).mean())
        # Narrow seams between round logs are intentional. Requiring a sealed
        # top projection made the old logs overlap into an almost flat slab.
        assert coverage > 0.94, f"deck has excessive gaps: {coverage:.1%}"
        tops = np.where(deck, np.arange(nz), -1).max(axis=2)
        relief = float(np.diff(np.percentile(tops[tops >= 0], [5, 95]))[0]) * grid.voxel_m
        assert relief <= 0.25, f"deck relief too high: {relief}"
        logs = asset.stats['steps'][0]['logs']
        diameter_spread = float(np.ptp([2 * log['radius_m'] for log in logs]))
        bark_spread = float(np.ptp([log['bark_fraction'] for log in logs]))
        assert diameter_spread > 0.06, "logs became uniform stock"
        assert bark_spread > 0.5, "logs lost their distinct bark coverage"
        results.append({"seed": seed, "bbox_m": bounds.tolist(),
                        "voxels": int(occupied.sum()), "pieces": pieces,
                        "deck_coverage": coverage, "deck_relief_m": relief,
                        "diameter_spread_m": diameter_spread,
                        "bark_coverage_spread": bark_spread, "logs": logs})
        if seed == 1:
            repeated = pipeline.build(body, seed)
            assert np.array_equal(repeated.grid.data, grid.data), "raft is not deterministic"
            base = output / "raft-0001"
            vxa.write(grid, base.with_suffix(".vxa"))
            vox.write(grid, base.with_suffix(".vox"), name="raft-0001")
            restored = vxa.read(base.with_suffix(".vxa"))
            assert restored.voxel_m == grid.voxel_m
            assert np.array_equal(restored.data, grid.data), "VXA round trip changed voxels"
            render.view(grid, "iso", target_px=1500).save(output / "raft-0001-review.png")
            spec.save(body, output / "raft-0001-spec.json")
            (output / "raft-0001.json").write_text(
                json.dumps(asset.stats, indent=2) + "\n", encoding="utf-8")
    assert len(set(digests)) == 4, "seeds generated duplicate rafts"
    (output / "raft-validation.json").write_text(
        json.dumps(results, indent=2) + "\n", encoding="utf-8")
    for row in results:
        print(f"seed {row['seed']}: {row['voxels']:,} voxels, "
              f"{row['pieces']} piece, {row['deck_coverage']:.0%} deck coverage, "
              f"{row['diameter_spread_m'] * 100:.1f} cm diameter spread")
    print("raftprobe: PASS; VXA/VOX and review image exported to out/artifact")


if __name__ == "__main__":
    main()
