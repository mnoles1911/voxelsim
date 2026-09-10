# Temperate appearance rollout

Completed September 8, 2026. Forge now applies the shared subtle voxel/face color variation to every saved asset with a positive temperate forest biome weight, respecting explicit biome exclusions. The authoritative scope contains 313 profiles: 49 trees, 113 understory, 30 rocks, 121 creatures. Habitat assignment is existing scope metadata, not a new ecological/reference acceptance.

5,956 saved assets were covered:1,764 existing tree appearances retained;4,068 understory variants, 121 creature assets and 3 rock assets received palette-preserving appearance. The 27 other rock profiles have no saved assets. Their future Forge previews and saved seeds use the same policy. This pass did not create geometry, endorse variants, or publish game banks.

Base material palettes and authored RGB coats are preserved. The same existing shader gains apply: voxel .085, face .045, warmth .025. New generic assets remain opaque, including petals and fine plant leaves. Existing tree foliage cutouts remain unchanged. Spring remains the modeling baseline. Screenshots spot-checked arrowhead, bluebell, daisy, hazel, kingfisher, red fox and granite; flower colors, coat regions and wood/leaf differences remain intact.

Preview payloads, thumbnails and runtime presentation are separate sidecars beside each saved asset. Future inspections and thumbnails create them lazily. The Asset Library species records, endorsement/rejection metadata, authored coats, specs and all VXA bytes passed before/after hashes. Geometry generator files and generator identity code were not changed.

All 4,192 new generic runtime packets passed full occupied-cell/material/coordinate and surface-RGB comparison against their saved sources. VAC1 version 2 stores header 32 pitch as integer micrometres, with header 44=0 for opaque surfaces. This preserves4,068 assets at25mm, 121 creatures at12.5mm, and 3 rocks at100mm. Legacy tree VAC1 version 1 remains byte-compatible. Local packets are preparation artifacts; game rendering support is separate work and is not claimed by this report.

Future bank exports now use a combined dispatcher with explicit visual endorsement, exact bank geometry and revocation gates. A single atomic publication inventory contains eligible legacy tree and generic appearances. No export command was run. Isolated tests passed pending refusal, exact-bank admission, missing/mismatched-bank refusal and rejection revocation despite retained stale packet files.

Validation: Python syntax and web TypeScript/Vite builds passed. Full protected-artifact rollout took 330.06s; exact runtime repair/audit took 90.75s, excluding initial broad export-file reads and visual inspection. No paid generation API calls were used.

Evidence: out/temperate-appearance-rollout/before.json, report.json, runtime-audit.json and scope.json. Live HTTP confirms new flower preview pitch2.5cm and foliage mode none; generic material colors receive variation without new transparency.
