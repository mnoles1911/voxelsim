# Environment surface lighting diagnostic

The real terrain handoff at seed20260719 showed a green rhododendron becoming nearly black. It remained dark after10seconds but became green in Unlit view mode. This is persistent lighting-dependent darkening, not merely the first rendered frame. See the four captures recorded in the handover.

Terrain marching adds sky/ground hemisphere ambient and a bounded sun-response deficit through emissive. The environment actor material previously had only its lit base color. The new opt-in material path shares the terrain's float derivation and supplies those two missing contributions through `MPC_VoxelSurfaceLighting`. It preserves ordinary deferred direct sunlight and existing vertex-color conversion, foliage cutout and wind.

The first material iteration became overbright. The terrain shader writes support directly to scene color, whereas Unreal's material base pass multiplies emissive by view pre-exposure. The helper compensates with `View.OneOverPreExposure` to keep the same scene-color units. This requires a new generated-material comparison; sharing only the ambient arithmetic was insufficient. Runtime publication checks the actual collection schema and setter success, and the visual boundary submits deferred collection updates before draining render commands.

`voxel.Environment.SurfaceLightingDiagnostic` defaults to0. The host enables the contribution only when both `voxel.GI.Volume` and `voxel.Light.Propagated` are0. This compares the common ambient/wrap calculation with both volume modifiers disabled; it does not establish parity with either volume enabled. The collection's authored Enabled default is always0.

Regenerate with `ue-project/Tools/create_vegetation_materials.py` in an available isolated editor slot. This maintains the dedicated collection and rebuilds the two consuming materials without recreating the existing sky collection. The script checks saving and shader-map availability. Inspect the commandlet log for Python and shader errors; process exit alone is insufficient.

Use `-AllowCommandletRendering -dx12 -sm6` for shader-map validation. Graph assembly can trigger intermediate missing-input compilation warnings before connections are complete. Independently run `validate_vegetation_materials.py` in a fresh rendering-enabled commandlet and require its completion marker, both shader statistics, no Python errors and no material compilation failures. The September7 fresh validation passed (369/368 pixel instructions for detail/environment); its process exited1 because of the existing ProjectID and GameFeatureData configuration errors. The earlier non-rendering attempt failed shader-map validation and is not a pass.

Run the real handoff harness with `-Mode VisualPilot -SurfaceLightingDiagnostic`. It requires the host's applied marker plus all four capture files and normal process exit. Review images separately. Default gameplay ownership remains unchanged.

Remaining production lighting work includes propagated-light texture sampling, directional GI enclosure, corner ambient occlusion, frame-consistent sun publication, and continuous-frame depth/shadow comparison. The visual pilot uses an explicitly blocking boundary; it is not a performance acceptance test.

The exposure-corrected real probe passed the settled-color check: green foliage,
no glow. Fresh shader statistics were370/369 pixel instructions. The first revealed
frame initially remained black. Follow-up EXRs identified a gray checker fallback.
The subsequent material-readiness gate now requires the intended complete GT/RT
shader maps and render proxy before publication. The latest real probe
`environment-material-readiness-probe.log` passed with normal exit0: first-after
and settled views are green, and their BaseColor buffers have no gray checker.
The gate waited0.485seconds. This sampled, volume-off diagnostic still does not
establish continuous-frame, shadow, default-lighting or performance acceptance.
