> Historical report, superseded by [the completed 49-profile collection](temperate-completion-report.md). The active oak source is `temperate-oak`; the old family/pilot aliases and standalone Production UI are retired.

# Environment inventory production â€” 2026-09-07

Asset Forge now has a Production workspace backed by the same local batch service as `tools/environment_inventory.py`. Select species across environment kinds, choose a seed range and worker count, and build saved, reviewable candidates. Inspect opens the exact saved voxel asset. Keep promotes that asset into the existing keep-driven publication workflow.

Measured on this machine:

| Run | Result | Wall time | Output size |
|---|---|---:|---:|
| Oak family, seeds 1â€“36, 3 workers | 36 unique assets | 91.39 s | 24.6 MiB |
| Identical request through UI | 36 verified cache hits | 1.18 s | reused |
| Six environment kinds, 2 seeds each, 2 workers | 12 assets | 2.15 s | 1.24 MiB |

No paid model/API calls occur during generation. These timings include voxel generation, health checks, VXA round-trip verification, VOX export, thumbnail rendering and saving. They are measurements, not a guarantee for every species; existing large or detailed recipes can cost more.

The oak recipe grows connected curved scaffolds, secondary branches, twigs and leafy shoots. Foliage is attached along shoots in tapered groups. Small/medium/large classes and open/woodland/edge forms cover nine combinations, four seeds each in this inventory. Seed changes alter architecture, dimensions and asymmetry. All 36 inventory oaks use 100 mm cubic voxels and range from 3.9 to 14.4 m. The earlier pilot also supplies a 50 mm comparison; the shipped world-bank lattice remains 100 mm for these trees.

The nine-tree scale sheet uses a common physical scale with the game's 1.8 m pawn proxy beside each tree. The 36-tree contact sheet fits each thumbnail independently for inspecting form, so use the scale sheet for size comparison.

The service supports existing tree, bush, grass, flower, reed and rock generators. The new foliage architecture applies to `temperate-oak-family`; other species retain their authored generators. Future species recipes can use the same batching, cache, validation, storage and review service. This does not imply all botanical recipes have already received an art pass.

Run from asset-forge with Python plus numpy, scipy and Pillow:

```powershell
python tools/environment_inventory.py temperate-oak-family --seed-start 37 --count 36 --workers 3
python tools/environment_inventory.py birch meadow-grass water-reed --seed-start 201 --count 8 --workers 2
```

Use Production in the application for the same operations without a command line. The desktop launcher also detects the installed Codex Python runtime and the local dependency directory on this machine when standalone Python is absent.

Builds are bounded to 1â€“4 CPU workers, 128 variants per species, 20 species and 512 assets per request. Candidate metadata records spec and generator fingerprints plus artifact hashes. An identical rerun validates and reuses bytes. Changed source or damaged output refuses overwrite; use unused seeds or a new species recipe name. Generator fingerprints conservatively include every forge Python module. Atomic staged writes prevent incomplete entries from becoming visible as completed inventory.

Candidates are excluded from the kept-seed bank and remain unapproved. Explicit Keep validates saved bytes and current source before promotion. No generated oak was approved or published during this work. World stamping continues through existing placement and publication infrastructure; biome-dependent growth-form selection is future work.

Validation: TypeScript check and production build passed; quick forge selftest passed across 850 validated specs; 36 unique oak VXA hashes, deterministic rebuild, all 36 layer-1 lattice checks, cache corruption refusal, candidate publication exclusion and explicit Keep in an isolated fixture passed. The optional geometry rebuild of every existing species was not run. Existing spec/seed hash behavior is unchanged for all 849 specs without the new recipe block.

Evidence: `out/inventory-runs/e1d3d962969c/report.json`, `out/inventory-runs/fe6d04c8724f/report.json`, `out/inventory-runs/4a08cc7a3e06/report.json`, `out/inventory-validation.json`. Assets: `library/temperate-oak-family/`. Visuals: `out/oak-inventory/36-oaks.png` and `out/oak-inventory/size-and-form.png`.

Final preview check: saved large oak seed 7 served 1,474,818 bytes at 100 mm in 0.39 s over local HTTP. The server-only preview/coverage correction changed the conservative code fingerprint; all 48 candidate artifact hashes were verified before updating their fingerprint metadata, with the migration reason recorded in each entry. Geometry and export bytes did not change.

## Application verification and player reference â€” follow-up

The user hit a real Windows progress-file replacement failure from Production. Both the initial running update and the failure update were denied, leaving a stale starting report. Atomic writes now retry transient sharing failures; job reports also retain an in-memory snapshot so persistent write failures appear to the UI and release the active-job lock. Server-owned runs from an earlier server session appear interrupted. Running reports show elapsed wall time even between completed seeds. `tools/production_report_probe.py` verifies retry and persistent-error behavior.

Retried through the actual Production controls: tundra-pine seeds 2â€“37 built 35 new assets in 9.45 seconds. Seed 2 was protected because an existing hand-kept asset has a different spec. A clean seeds 3â€“38 request completed with 36 variants, zero failures (35 verified cache hits + one new asset, 1.61 seconds). Reports: `out/inventory-runs/f46a696942c1/report.json` and `out/inventory-runs/bb55c51db86e/report.json`. Existing seed 2 was not modified.

The Forge tab currently exposes existing procedural recipes and parameters; the oak's new architecture was authored through code and reference review in this conversation. Production multiplies existing recipes into seed inventory. It does not infer a new botanical generator from a reference image.

The shared VoxelCanvas now draws the game's standing six-box pawn beside any inspected asset. Dimensions match VoxelProxyBody.cpp and VoxelMovementTuning.h (1.8 m tall, 0.6 m wide). Scaling uses the actual response voxel pitch; pawn feet align with the lowest occupied voxel and the pawn stands outside the asset bounds. The person icon toggles the reference without changing camera position, and remembers the preference locally. Pawn geometry is display-only and excluded from asset exports and voxel statistics.

Generator fingerprints now exclude inventory/server/server-version/CLI orchestration, so reporting or UI changes no longer invalidate geometric assets. Existing artifact checksums were verified before this fingerprint-schema migration; migration reasons remain in metadata. TypeScript and production builds, inventory contract checks, and progress-report regression tests pass.
