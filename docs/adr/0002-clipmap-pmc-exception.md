# ADR-0002: Band 3 clipmap may use ProceduralMeshComponent (scoped exception)

- **Status:** accepted
- **Date:** 2026-07-20
- **Doctrine sections affected:** plan §3.3 Band 1 ("GPU greedy meshing →
  pooled custom FPrimitiveSceneProxy buffers (NOT ProceduralMeshComponent)")
- **Human sign-off:** Matt Noles, 2026-07-20 ("ADR-0002 approved")

## Context

The no-PMC rule exists because Band 1 voxel chunks are numerous (thousands),
churn constantly (streaming + edits), and need pooled buffers and a custom
vertex format — PMC's per-section overhead and update model are wrong for
that. The Band 3 heightmap clipmap is the opposite profile: FOUR components
total, each rebuilt at most once per frame round-robin, conventional
positions/normals/UVs, destined to be replaced by proper CDLOD in M2 polish.

## Decision

ProceduralMeshComponent is permitted ONLY for `AVoxelClipmapActor`'s
clipmap levels (Band 3/4 heightmap terrain). The voxel path's prohibition is
unchanged and absolute. If clipmap level count or update frequency ever
grows past ~8 components / per-frame rebuilds, the CDLOD replacement moves
from polish to required.

## Consequences

Band 3 v1 ships fast on boring engine machinery; the doctrine rule keeps
its teeth where it matters (the voxel path); one named, bounded exception
instead of erosion by precedent.
