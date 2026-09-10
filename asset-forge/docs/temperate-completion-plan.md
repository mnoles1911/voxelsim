# Temperate forest generator completion plan

Scope: all 49 profiles in `forge/forest_profiles.py`, spring appearance, 100 mm
production voxels initially (50 mm requires a separately identified output).
Target: 36 reproducible variants per profile, 1,764 total. Regional and archetype
profiles retain their habitat labels; inclusion here does not change world weights.

1. Curate references per profile: whole spring crown, visible branching, foliage
   detail where useful. Record source, image identity, botanical identity and
   cultivar/season limitations in the library review record. Never equate source
   discovery with a completed visual review.
2. Refactor architecture families, then tune individual species: broadleaf forks,
   dominant leaders, pendulous shoots, conifer boughs, mature pine crowns and fern
   fronds. Leaves must follow shoots. Reject radial shelves, spherical clumps,
   artificial split tops, disconnected wood and implausible player scale.
3. Generate seeds 1, 4 and 7 for small/medium/large review, plus woodland and edge
   samples. Compare silhouettes, exposed branches and shaded geometry against the
   selected photos. Record the exact generator digest and artifact hashes, concrete
   findings and accepted/rejected outcome. Iterate failed species.
4. Establish the accepted source in the species library record: baseline spec,
   generator revision/digest, reference instance and seed, references, spring art
   baseline and reviewed artifact identity. User endorsement of world variants is
   distinct from accepting a generator's design.
5. Freeze the generator digest and generate seeds 1–36 locally for each accepted
   species. Check deterministic repeatability, distinct content, dimensions,
   connectivity, voxel resolution and runtime. Inspect contact sheets for outliers.
6. Wire the exact saved outputs into Forge for inspection and endorsement. Endorsed
   variants live in Asset Library; game export copies those exact bytes. Verify
   reference selection, pawn toggle, generation progress, endorsement and export.
7. Replace each old collection only after its complete replacement verifies.
   Preserve a compact retirement ledger, remove old geometry and bank entries,
   consolidate obsolete oak aliases, and ensure no stale model remains selectable.
8. Final audit: 49 accepted sources, 36 candidates each, explicit endorsed library
   entries, no legacy model leakage, frontend build and backend checks passing.

Completed checkpoint (2026-09-07): all eight steps completed for the 49-profile
source and candidate collection. All 49 source records passed documented photo,
three-size pilot and 36-variant sheet review. All 1,764 saved variants are at 100 mm
and pending user endorsement. 77 superseded assets were retired after replacement
verification. See `temperate-completion-report.md` for results and remaining art
limitations; see `out/temperate-collection/completion-audit.json` for the audit.
