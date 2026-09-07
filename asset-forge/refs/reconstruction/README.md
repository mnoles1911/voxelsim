# Anatomy-first reconstruction pilot

These are research inputs and rejected experiments, not approved assets.
The eight-species scope and primary visual gate are in `pilot-manifest.json`.
The initial procedural pass failed visual review; see `initial-visual-review.json`.
No replacement of the 382 existing wildlife models has been approved.

## Reproduce the experiments

Use the normal Forge Python environment, plus `trimesh==5.1.0` and
`rtree==1.4.1`. Blender source inspection was run with Blender 5.1.1.
Run commands from `asset-forge`:

```powershell
python tools/reconstruct_pilot.py --species grey-wolf
python tools/voxelize_reference_mesh.py out/creature-reconstruction/source-meshes/56de4df672654ed599777d2980bf0f53.glb out/creature-reconstruction/grey-wolf-source-study --long-axis 0 --length-m 1.81
```

Downloaded GLBs and generated previews stay under ignored `out/`.
The source study was obtained from Objaverse's public archive using
`objaverse.load_objects(uids=['56de4df672654ed599777d2980bf0f53'], download_processes=1)`.
Per-asset authorship, license metadata and reviews are preserved in
`grey-wolf/mesh-source-reviews.json`; the dataset license is not a substitute
for each asset's license. These source meshes are not vendored here.

## Current findings

The initial eight procedural candidates did not establish realistic anatomy
and color. The stag additionally has two detached antler sections. Wolf source
studies improve surface continuity but still fail the visual gate: one has
carved sculptural fur, the other has weak paws, oversized bright eyes and noisy
coat coloration. Finer voxels preserve these defects rather than fixing them.

The texture-preserving wolf conversion produced 101,602 occupied 12.5 mm voxels,
14,752 surface voxels and one connected component. Its six-view inspection
still rejects it as a finished animal. The assumed 1.81 m source bounding
length includes the tail and is not a measured specimen calibration.

RGB surface previews are research sidecars. Current VXA material IDs cannot
represent their full color range. No rig, animation or engine appearance
compatibility is claimed by the source-mesh conversion.

## Next work

1. Establish a credible wolf master against real reference images and anatomical
   landmarks, correcting eyes, paws, head proportions and coat region boundaries.
2. Validate the implemented seven-point linear-light texture filtering against
   real coat references; it reduces aliasing but cannot correct bad source art.
3. Compare six orthographic views, oblique views and gameplay-distance renders.
4. Resolve RGB appearance storage/rendering and explicit draft curation before
   any publication. Successful geometry export is not visual approval.
5. Continue the remaining species only after this workflow proves a visible gain.

The existing engine's dense body limit also requires an explicit solution for
the 12.5 mm orca. Research tiles are not a working runtime creature assembly.
