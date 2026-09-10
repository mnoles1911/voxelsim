# Temperate slice acceptance ledger

Updated September 9, 2026 UTC. The requested Asset Forge production, appearance rollout and active plant-renderer integration deliverables are verified. This does not declare the entire playable biome or other game systems complete.

| Requirement | Current evidence | State |
|---|---|---|
| Reference-backed understory profiles and 36 deterministic seeds each | `asset-forge/out/understory-review/checkpoint.json`: 113 accepted generator profiles, 4,068 pending variants; completion report and spring payload audit under `asset-forge/docs/understory-completion-report.md` and `out/understory-review/` | Library production complete; pending variants remain subject to user endorsement |
| Tree spring appearance and thumbnail rollout | `asset-forge/out/tree-appearance-collection-v2/verification-report.json`: 1,764 installed and verified; geometry and decisions unchanged | Complete |
| Shared variation policy across saved temperate assets | `asset-forge/out/temperate-appearance-rollout/report.json`: 5,956 assets, no protected-file failures; runtime audit checks 4,192 non-tree packets against original records | Asset Forge rollout complete |
| Explicit publication authority | Published appearance catalog binds endorsed inventory to exact accepted bank bytes; tests cover identity and stale/revoked inputs | Implemented and tested; no automatic endorsement |
| Paired geometry/appearance lifecycle | TerrainGpuState, QuadPoolLifecycle and RecursivePageAdmission results under `asset-forge/out/tree-runtime-appearance-v1/` | Targeted runtime tests pass |
| Destruction, source capture and restoration | WorldDebrisCapture, DebrisRestore, GenericOpaqueRestore and TouchedPage reports | Targeted runtime tests pass; current-game capture still pending |
| Coarse and recursive source ownership | Core appearance tests; native RecursiveMipTrace in default and majority modes; TerrainPagePreparation; actual QuadSurfaceGpu including version 3 offsets and independent source UV expectations | Tests pass |
| Actual pooled raster color and leaf openings | `raster-rgb-tests/index.json`: opaque and four leaf rotations, zero RGB/depth errors; alpha checked separately as specified by Unreal capture | Pass with existing diagnostic warnings |
| Actual pooled leaf shadows | `recursive-complete-tests/index.json`: QuadShadowPixels, four rotations with lit/opaque/masked controls and receiver depth | Pass with existing diagnostic warnings |
| Production brick traversal through foliage | `brick-traversal-tests/index.json`: actual flat/hierarchical production functions and real pool buffers, 44 cases with exact hit assertions | Pass, no warnings or failures |
| Representative current-build forest performance | Prior four-pass capture `approved-appearance-sequence1` and current-build `current-render-integration1` completed and passed integrity/runtime gates; current edit 0.809 ms, post-edit frame median 20.170 ms, GPU median 18.717 ms | Measured; single current-build run, not a performance target or full-biome acceptance |
| Fully populated, visually accepted temperate game world | Frozen benchmark contains legacy background trees and sparse ground cover | Not established by the asset/render tests |

## Interpretation

Generator acceptance and variant endorsement are different decisions. The 4,068 understory variants are produced and reviewable; this work does not promote them into an endorsed game bank without review. The game tests use explicitly published sources and isolated fixtures where noted.

The forest benchmark measures a 16-tree stand within the running game, not isolated material cost. It uses 1280×720 output and 832×468 internal rendering. A successful capture cannot establish that every species is visually accepted, all biome placement is finished, or all creature animation paths have been exercised.

The Asset Forge production and active plant-renderer deliverables are complete. The next user-facing stage is endorsing variants and selecting population/microhabitat rules for a playable biome; those decisions were not automatically made by the generation or appearance rollout.


## Final scope audit

The active 25 mm understory route is the production DetailAsset worker → appearance-aware face geometry → static mesh → HISM placement. It captures the world's approved appearance binding and sets the material's appearance and cutout policy. Its mesh data and generic source/restoration behavior have passing native tests. Publication and placement must select the endorsed seed for this route to receive its approved appearance.

The optional 50 mm cover renderer is disabled by default and has no approved-appearance upload. It is not the active 25 mm understory route. Rigged creatures are explicitly rejected by the existing detail/body renderers; agents still use placeholder cubes. This work applies their appearance policy in Asset Forge and supports generic unrigged/detail data, but does not create a production animated creature renderer. The separate manual AssetBody path is used by boats and gliders and remains outside this temperate plant task.

The final evidence review read the underlying inventory and payload audit summaries, verified the applicable named native test results (including later successful reruns that supersede earlier fixture failures), inspected runtime images, and analyzed the completed current-build forest capture. No variants were automatically endorsed or game banks republished to bypass user review.
