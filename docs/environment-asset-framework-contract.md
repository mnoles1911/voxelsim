# Environment asset framework contract

The common framework must accept every Forge environment species by catalog identity and grid data. A tree name, four prototype indices, or a specific generator must never select the serialization format or the mesh implementation.

The current catalog has **446 environment specs**, all mapped by the read-only `forge.environment_contract` helper. This is source-spec coverage, not a claim that all banks are baked, accepted by scatter, or migrated to object rendering.

| Generator kind | Count | 100 mm | 50 mm | Current terrain route | Current detail route |
|---|---:|---:|---:|---:|---:|
| tree | 78 | 78 | 0 | 78 | 0 |
| rock | 102 | 102 | 0 | 102 | 0 |
| bush | 57 | 33 | 24 | 33 | 24 |
| grass | 89 | 3 | 86 | 3 | 86 |
| reed | 33 | 0 | 33 | 0 | 33 |
| flower | 87 | 0 | 87 | 0 | 87 |
| Total | 446 | 216 | 230 | 216 | 230 |

No current environment spec uses 25 mm. That pitch is nevertheless an allowed environment-detail authoring tier. The environment contract admits 100/50/25 mm; 12.5 mm remains exclusive to creatures and craftables. Existing tree/rock banks and the production terrain route remain 100 mm; finer authored grids can use the generic actor, within its current storage limits, while production placement still needs the fine-source bridge.

## Resolve the actual metadata

- **Category:** use `forge.categories.of(spec)`, including an explicit category override. Generator kind and gameplay category are different facts. An artifact generator categorized as environment needs generic grid handling plus an explicit gameplay profile; it is not automatically admitted to the existing scatter manifest.
- **Terrain versus detail:** use `manifest.is_terrain_lattice(kind, nominal_height_m, spec_name)`. Trees and rocks are terrain; bushes and grass at least 1.5 m high are promoted to terrain except named `black-coral-tree` and `mangrove-sapling`. Reading only `kinds.Kind.lattice` misses the 36 promoted shrub/grass species.
- **Pitch:** preserve integer micrometres in the common runtime descriptor. VXA v3 stores whole millimetres and v4 stores micrometres; VXM v2 stores millimetres and v3 stores micrometres. Do not infer pitch from kind or silently round it. File-header pitch must agree with the descriptor.
- **Placement eligibility:** use the manifest rules independently of geometry loading. `hero-arch-colossal` (90 m) and `hero-sequoia` (80 m) currently exceed the tallest 60 m scatter layer. They still belong to the supported environment asset catalog. The framework must not silently omit them or claim the current scatter system can place them.

## One geometry and state contract

All six kinds require the same mechanisms: arbitrary catalog identity, exact pitch, local material grid, transformed surface mesh, distance LOD, material-aware wind selection, local voxel queries/edits, versioned source snapshots, stable instance identity, streaming residency and authoritative replication. These are framework requirements; the helper does not advertise their production integration as complete.

Use profiles for behavior, not separate mesh or save implementations:

| Kind | Family | Wind profile | Interaction profile | Fracture profile |
|---|---|---|---|---|
| tree | woody vegetation | rooted wood and foliage | chop/harvest | structural wood |
| bush | woody vegetation | rooted wood and foliage | cut/harvest | woody stems |
| rock | mineral | none | mine/harvest | mineral |
| grass | flexible vegetation | rooted blades | cut/harvest | none |
| reed | flexible vegetation | rooted stems | cut/harvest | none |
| flower | flexible vegetation | rooted stems and blooms | pick/harvest | none |

These are recommended default profiles, not species-level physical truth. For example, the bush generator also authors coral. Explicit species gameplay/wind overrides will be needed; underwater coral must not acquire terrestrial leaf sway just because it shares a growth generator. The current runtime collision route remains authoritative: terrain species block via voxel queries; detail plants are currently non-colliding. Voxel interaction/picking does not require a rigid collision body for every blade or flower.

Wind uses material/part classification and local rooted height, not asset-name checks. Derived meshes and LODs are presentation caches of the same local grid. Local coordinates must remain stable through orientation, wind, origin rebasing, save/load, edits and detachment.

## Identity and revisions to serialize

Store the following independently:

| Field | Meaning |
|---|---|
| `source_asset_id` | Stable catalog key, e.g. `forge:environment:<spec-file-stem>`; retain the original spec name |
| `generator_kind`, `category`, profile IDs/version | Classification and behavior; arbitrary strings or versioned enums, never the four prototype names |
| `voxel_pitch_um`, dimensions, grid origin | Exact geometry coordinate system |
| `spec_hash` | Forge's 64-bit geometry-spec hash, retained as its hexadecimal string |
| `seed_hash`, bank seed | Which deterministic individual was generated |
| source VXA content hash and bake/schema version | Actual source-content identity; generator code can change while a spec hash does not |
| provider and catalog fingerprints | The world/source namespace used to validate restored placements |
| stable instance GUID and full placement provenance | One placement, not one species; distinguish all instances of the same bank asset |
| geometry revision | Monotonic local topology/material revision |
| object/state revision | Monotonic authoritative transform, lifecycle and interaction state revision |
| coarse projection revision | Must match the edited geometry before terrain/object ownership changes |

Classification metadata is deliberately excluded from `spec_hash` and `seed_hash` by Forge. Changing a category, curation, notes or placement rules must not silently reroll the generated individual. A separate classification/manifest revision tracks those changes. The probe's `coverage_catalog_sha256` describes its audit output and must not be substituted for the actual published VXM content fingerprint.

Do not infer resource yields, wood hardness, mineral identity or physics parameters from the asset name. Those need explicit species/material gameplay data. Unsupported behavior profiles should be reported while retaining generic geometry support.

### Current generic actor field mapping

The shared actor now exposes `InitializeAssetFromVxa` and a versioned generic `FVoxelEnvironmentAssetDescriptor`; it retains the legacy class name for compatibility. `environment_contract.runtime_descriptor(...)` maps a catalog entry into that descriptor:

| Actor field | Catalog/source value |
|---|---|
| `SpecId` | `spec_name`, the arbitrary spec filename stem |
| `Kind` | `generator_kind` |
| `Category` | resolved `environment` |
| `SourceHash` | MD5 of the actual source VXA bytes, verified by current actor initialization; never `spec_hash` |
| `SpecHash` | Forge `spec_hash` |
| `CatalogHash` | actual published catalog/manifest content fingerprint supplied by the integration |
| `ProviderHash` | world/provider fingerprint supplied by the integration |
| `SeedIndex` | actual bank seed, supplied by the integration |
| `Fellable` | explicit gameplay decision, not a name match |
| `Legacy` | false for generic catalog assets |

Current descriptor schema 1 fixes `SourceHash` to the 32-character VXA MD5. A different content-hash algorithm requires a versioned schema change. Exact pitch, grid dimensions and origin are stored with the grid rather than duplicated in this descriptor. `seed_hash` remains useful catalog provenance but is not yet a dedicated actor descriptor field.

Generic initialization and identity serialization do **not** activate ordinary production tree/rock ownership migration. The terrain publication bridge remains separately gated. Dense-grid admission also remains bounded at 64 MiCells and 4096 cells per axis; covering every catalog family is distinct from supporting every possible asset size.

## Verification

Run `asset-forge/tools/environment_contract_probe.py`. By default it reads specs and exported bank headers and prints a summary without modifying specs, banks, the library or generated geometry. `--out Saved/environment-contract-audit.json` optionally saves the detailed mapping. Bank inspection reads only each VXA's 48-byte header and filesystem size, reports absent banks and current dense-grid limit violations, and does not decode or regenerate geometry.

The current run passed all 446 mappings, declared-kind coverage, stable-ID uniqueness, input immutability, arbitrary-name handling for all six kinds, geometry-hash preservation under an environment category override, and rejection of non-environment overrides. Four pre-existing twig-thickness warnings and the two large-asset placement limits are reported rather than hidden. The existing `library/categories.json` also lists 446 environment species.

The exported bank header audit checked **1,742 VXA files covering 439 species**. No checked source exceeds 64 MiCells or 4096 cells on an axis; no header or pitch mismatch was found. The largest is a kapok seed at 239 × 237 × 357 cells, or 19.285 MiCells. This is only the material-grid size; derived LODs, meshes and temporary buffers cost additional memory.

Seven species have no current exported bank to inspect: `hero-arch-colossal`, `hero-balanced-rock`, `hero-basalt-colonnade`, `hero-natural-arch`, `hero-sea-stack`, `hero-sequoia` and `hero-tsingy-pinnacles`. Their eventual baked-grid admission has not been verified.

For a future 100→50 mm rebake at fixed physical bounds, the geometric estimate is eight times the dense cells. Six species (16 current seed bounds)—eastern cottonwood, jungle emergent, kapok, strangler fig, sweet chestnut and sycamore maple—would then exceed the present 64 MiCell cap; the largest estimated bound is 154.278 MiCells. This is a bounds estimate, not a rebake or measured memory/performance result. Universal finer-source support therefore needs chunked/sparse grids or another explicitly budgeted strategy, even though every currently exported environment bank fits today's cap.

Generic actor initialization and save/restore must additionally be exercised with at least one asset from every kind, every supported environment pitch, a large sparse grid, negative local origins and an unfamiliar valid spec name. Catalog coverage alone does not validate mesh output, collision or the production renderer publication bridge documented in `production-environment-ownership.md`.

## Implemented engine verification

`Saved/build-general-environment.log` records a successful Unreal editor build. `Saved/general-environment-tests.log` records all ten object tests passing. The new generic test covers every family at every supported environment pitch (18 cases), including arbitrary spec IDs, exact descriptor retention, source hash checks, immutable snapshots, staged restoration, LODs, collision queries and explicit felling eligibility. The earlier fixture and legacy snapshot tests also pass. The generic matrix uses small synthetic geometry to isolate these contracts; the bank audit checks actual exported headers rather than claiming visual/physics validation of all 1,742 assets.

Current initial VXA ingestion/rebuilding is synchronous; restoration uses the worker/staged path. Standing assets support translated, upright, unit-scale placement at 0/90/180/270 degree yaw. Collision, raycasts, carve/chop queries, mining previews and terrain support probes share exact sign/swap transforms so rendered placement agrees with the canonical grid. Initialization, snapshots, staged restoration and replication reject unsupported standing-asset rotations or scales; detached timber retains unrestricted physics rotation. Arbitrary tilt and scaled standing assets require a separate geometry policy.

The experimental timber ground proxy still has a finite footprint and needs a bounds-aware production collision provider for very large falling trees. The engine-free sparse grid described in `sparse-environment-grid.md` is a tested storage prerequisite, not an actor migration; dense actor admission remains unchanged. These are integration requirements, not reasons to fork save/mesh code per species.

`Saved/build-environment-yaw.log` records the subsequent successful build; `Saved/environment-yaw-tests.log` records all eleven object tests passing. `EnvironmentQuarterYaw` exercises all four rotations at each environment pitch with asymmetric geometry, negative origins, translated collision/trace/edit coordinates, preview bounds, immutable geometry invalidation and staged restoration. It also checks rejection of unsupported scale, tilt and 45-degree yaw. This is automated coordinate and lifecycle verification, not a visual review of production terrain placement.
