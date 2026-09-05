# The `artifact` kind: craft on the entity jurisdiction

What this covers: how the generator works, where the two shipped craft live on
disk, what the engine-side loader gets, and the four open decisions that belong
to somebody else.

Written 2026-09-04 for Phase D0 of `docs/water-ocean-tides-plan-2026-09-04.md`.

---

## 1. What an artifact is, and what it is not

`artifact` is the eleventh kind and the first one that was never alive. It is
also the only member of the **`craftable`** category (`forge/categories.py`,
owner request 2026-09-05) — the classification layer that answers "what is this
thing in the game" rather than "which generator drew it". The two are separate
axes on purpose: a torch will be an `artifact` and craftable, a rope may be
craftable and not an `artifact`. See README §Categories and
`library/README.md`.


Everything else in this package grows or erodes — a tree colonises space, a rock
accretes and is carved, a tuft sprays stems, an animal is a body with parts hung
off it at angles. A canoe does none of that. It was drawn, once, by a person,
out of two curves, and the whole reason a canoe reads as a canoe is that those
curves are **fair**: continuous, monotone where they should be, with no step in
them anywhere.

That single fact decides the design. There is no recursion, no growth model, no
habit table, no allometry, no competition and no iteration count. There are two
closed-form families and a handful of struts.

| family | `artifact.form` | what it is | who uses it |
|---|---|---|---|
| lofted hull | `hull` | a closed double-ended watercraft, hollowed | `canoe` |
| panel on spars | `wing` | a thin swept sail with a tube frame under it | `glider` |

## 2. How it draws, and why that IS the shape rule

**Every surface is a FIELD evaluated over the whole grid at once, not a stack of
stations.** A hull is the set of voxels satisfying

    keel(x) <= z <= sheer(x)    and    |y - centre(x)| <= halfbeam(x, z)

with all four smooth closed-form functions of the continuous coordinate.

This is the whole of the "no contour rings" rule, applied at the only place it
can be applied. The naive design — and the one a boat-building analogy pushes
you straight into — is to loft N stations and sweep between them. At 2.5 cm on a
3.9 m hull that is a ring every few voxels, and the rings are the first thing a
person sees in the render.

The other three standing rules fall out of the same choice:

- **The interior is not an erosion.** It is the *same* three functions
  re-evaluated one shell thickness in, and subtracted. An erosion of a quantised
  surface inherits every step that surface has and doubles it.
- **Nothing is cut on a diagonal.** Both families are axis-aligned in the grid.
  A swept wing gets its sweep from `x_le(y)`, a *function*, never from rotating
  a drawn shape. A rotated raster is the off-axis cut this package has paid for.
- **Thresholds are by depth, never by quantile.** `shell_vox` voxels of plank,
  `gunwale_vox` voxels of overhang, `panel_vox` voxels of fabric. Not "the
  thinnest 8%".
- **There is no per-voxel thinning pass and there will not be one.** A hull
  plank is two voxels thick; a pass that removed voxels independently would not
  thin it, it would perforate it.

## 3. What it reports, and why

Every drawing step returns a voxel-count delta into `stats["steps"]`, and a step
that was asked to draw and changed nothing is an **error** — `pipeline.health`
names it and `tools/artifactprobe.py --read` exits non-zero on it.

This is not defensiveness. Nine steps here can each fail that way — a gunwale
band thinner than a voxel, a thwart placed above the sheer, a cross-bar at span
zero, a strake subset that came out empty — and every one leaves an asset that
still builds, still passes every connectivity check, still looks like a boat in
every render this package can take, and is missing a part.

`shell_vox` is an authored number; `--read` prints the **drawn** one. And
`hollow_frac` is the measurement no camera can make: a canoe is a shell, a
player sits in it, and a solid block of hull voxels renders as a perfectly good
canoe from every angle at every scale.

Seed variation is deliberately subtle and never structural: `variation.*` does
not touch `artifact.*` at all, so a seed moves exactly two things — which
planking runs carry the second material, and how far the centreline bows to one
side (`artifact.asym_vox`, default 0.8 **voxels**). Measured over seeds 1-4 that
is a 0.13% spread on the canoe and 0.09% on the glider. A canoe is a
manufactured object; two of them are the same object.

## 4. The two craft, as built

Seed 1, pitch 25 mm, both one face-connected piece, both `health` clean.

| | canoe | glider |
|---|---|---|
| form | `hull` | `wing` |
| voxels | 16,998 | 35,701 |
| bbox (m) | 3.90 × 0.90 × 0.525 | 3.30 × 5.675 × 1.225 |
| bbox (voxels) | 156 × 36 × 21 | 132 × 227 × 49 |
| enclosed air | 66.8% | n/a (open frame) |
| planking drawn | 2 min / 2.9 mean voxels across the beam | 2-voxel fabric |
| `.vxa` | 54,863 B, v3, `voxel_mm` 25, 0 parts, 0 joints | 167,058 B, same |
| materials | heartwood hull, bark trim, deadwood strake | plume\_buff sail, plume\_white panels, beak\_horn tube, bark seat |

Neither uses a material above `MAT_BEAK_HORN` (46), so both are inside
`kMaterialCount` = 47 and `AssetGrid::materialsWithinEngine` admits them today.

## 5. Where the files are, and how a crafting system finds them

    specs/canoe.json                              the species
    specs/glider.json
    library/canoe/canoe-0001/tree.vxa             WHAT THE ENGINE LOADS
    library/canoe/canoe-0001/tree.vox             MagicaVoxel round trip
    library/canoe/canoe-0001/thumb.png            review camera
    library/canoe/canoe-0001/{spec,realized,meta}.json
    library/glider/glider-0001/...                the same six files
    out/artifact/<name>-0001-{review,profile,inside,headon}.png

**`library/<name>/<name>-0001/tree.vxa` is the path.** It is written by
`forge.server.keep`, which is the same function the app's "Keep to library"
button calls, so the entity path and the authored path are the same path.

**And `library/categories.json` is how you find it without knowing the name.**
`categories.craftable.species[].grids[].vxa` lists every craftable's grid with
its `voxel_mm` and voxel count; `tools/export_categories.py --check` fails if
the file is stale. That is the intended seam for a crafting system — one file,
one resolver (`forge.categories.of`) shared with the app and the build check, so
the game and the forge cannot disagree about what is craftable.

There is **no bank and there must not be one.** `tools/export_banks.py` defaults
to terrain kinds (`tree`, `rock`); `forge.manifest.KINDS_ENTITY` refuses an
artifact from the species manifest by name; both craft carry zero biome weight,
so `biomes.allowed()` returns the empty tuple. Per ADR-0010 §Enforcement, the
ladder gate `check_on_ladder` fires at bank export and "does not and must not
police entities" — nothing here reaches it.

## 6. What the engine-side loader gets

`.vxa` v3, and the two facts that matter for `UVoxelAssetBodyComponent` (plan
§D1):

- **`voxel_mm` = 25.** The file states its own pitch, which is exactly why v2
  exists. Read it. `AssetGrid::at` takes plain integer voxel coordinates with no
  scale factor and **nothing in voxel-core resamples**, so a grid read at the
  wrong pitch is not an error, it is a craft at the wrong size in a world full
  of craft.
- **`part_runs` = 0 and `joints` = 0.** Unrigged, so `hasParts()` is false and
  both grids pass the `assetfield.h:420` / `VoxelDetailAssetSubsystem.cpp:863`
  world-composition exclusion untouched — which is what the plan's D1 note
  requires.

Instance counts at 25 mm, for the ISM budget: **16,998** cubes for the canoe and
**35,701** for the glider. The plan's risk register guessed "low thousands"; the
canoe is 4× that and the glider 9×.

**Drawing only the surface does not help, and that is worth stating before
somebody spends a day on it.** Measured: `grid.surface_mask()` keeps 13,826 of
the canoe's 16,998 voxels (81%) and 33,802 of the glider's 35,701 (95%). These
are a 2-voxel shell and a 2-voxel sheet — they are already almost entirely
surface, which is exactly what makes them cheap to author and useless to cull.
The lever that does work is **pitch**: the entity jurisdiction permits any
value, and each doubling is roughly 4× fewer instances on a shell (area, not
volume). 5 cm, measured rather than extrapolated, is 3,798 for the canoe and 8,596 for
the glider — at the cost of a 17-voxel beam and a 2-voxel leading-edge tube.

## 7. Open decisions — these are not mine to make

1. **Pitch.** 2.5 cm is the plan's directive and it is the world ladder's floor,
   but ADR-0010 is explicit that an entity is *not* on the ladder: 2 cm (where
   103 animal specs already sit) or 1 cm are both legal and would each roughly
   double the instance count per halving. 2.5 cm makes the canoe 36 voxels
   across the beam and the glider's 4.5 cm leading-edge tube 4 voxels across,
   which is the "smallest identifying feature about three voxels" rule met with
   little to spare.
2. **Where the origin is.** `.vxa` stores an origin (canoe `(2,2,2)`, glider
   `(0,0,26)`) that means "voxel offset from the asset's base at (0,0,0), z up"
   — a statement about a thing that stands on the ground. An entity has a
   transform and therefore a **pivot**, and nothing anywhere says where a
   craft's pivot is. A boat wants it at the waterline on the centreline
   amidships; a glider wants it at the hang point. Today the loader will get a
   corner. This needs deciding on the D1 side, and it is not visible until
   something rotates.
3. **The contrast strake.** The renders show it as a grey band along the topside
   of the hull. That is a real feature (a rubbing strake) and it is also the one
   thing in these assets that is a taste call rather than a measurement. Set
   `artifact.strake_share` to 0 for a plain hull; the step then reports itself
   as authored-off rather than silently absent.
4. **Terracing on a curved sail.** The glider's fabric shows the iso-height
   steps of its own billow. That is not the contour-ring defect — nothing here
   is stacked from stations — it is what a 2-voxel curved sheet at 2.5 cm looks
   like, and the only levers are a flatter sail, a thicker sail or a finer
   pitch. Owner's call against the render.

## 8. Running the instruments

    python tools/artifactprobe.py --read            # counts, bbox, step deltas
    python tools/artifactprobe.py --seeds           # deterministic AND seeds differ
    python tools/artifactprobe.py --views --px 1100 # four cameras per craft
    python tools/artifactprobe.py --arms canoe      # the MUST-FAIL arms
    python tools/buildcheck.py --category craftable # every craftable, any kind
    python tools/export_categories.py --check      # the index is not stale
    python tools/artifactprobe.py --hashes tools/spec_hashes.json

The last one is the one to run after touching `spec.PARAMS`. Adding a row there
puts a new key in every spec's canonical JSON and normally moves BOTH hashes for
all of them — every species becomes a different individual and every bank reads
as stale. `spec.KIND_SCOPED_PARAMS` is the mechanism that avoids it: a path
listed under kind K is deleted from the hash body of every spec whose kind is
not K. Measured at the time of writing: **828 of 828 unchanged on both hashes**,
with two red arms (scoping off, and the empty-container prune off) each moving
828 of 828.
