# Wolf proportion study

The user's accepted coat is carried into a new continuous-mesh deformation
study using `tools/refine_wolf_proportions.py`. The original candidate remains
available. This is a modeling hypothesis, not a fitted biological specimen.

The source mesh is scaled uniformly by 0.84, then smoothly extended by 14 cm
through the torso. Head and paw shapes are not flattened. The resulting 12.5 mm
candidate has 69,525 occupied cells in one component, a 1.675 m bounding length,
and an approximately 86.25 cm ground-to-coat shoulder-region envelope, compared
with 102.5 cm previously. The manually selected shoulder window includes fur.

The comparison uses the same physical pixel scale for both animals. Six-view
inspection shows a less tall, relatively longer torso. Small facial features
lose resolution as expected at a smaller physical size. Head, paws and joint
landmarks still need detailed review; matching a size range is not anatomical
approval. The coat is transferred from the reviewed study with local weighted
sampling, retaining dark facial pixels.

Outputs: `out/creature-reconstruction/grey-wolf-proportion-study/`.
`colored-voxels.glb` contains the transferred coat. `master.glb` retains original
source textures and is for geometry inspection. Neither is a rigged game asset.
The source's CC BY attribution remains in `mesh-source-reviews.json`.

The next review should resolve head/neck proportions and paw contact shape
against compatible reference poses before broadening the pilot to another animal.
