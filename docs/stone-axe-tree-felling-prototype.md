# Stone axe and tree felling prototype

The directional hinge and impact-energy revision is documented in [Tree fall and impact destruction](tree-fall-and-impact-design.md). It replaces the original angular kick with a gravity-driven fall and replaces the tilt/impulse break gate with an estimated contact-energy threshold. The earlier measured run below is retained as a baseline, not as validation of the newer revision.

## Run

From the repository root, run `tools/voxel-tree-felling-prototype.ps1 -Capture -KeepOpen` after building VoxelEarthEditor. It refuses to launch alongside an existing editor/compiler. The test uses the existing isolated temperate oak and leaves the game open afterward.

The held axe uses **12.5 mm cubic voxels**. The editable oak uses **50 mm voxels**, with a derived **100 mm** standing-tree LOD. The source axe spec is `asset-forge/specs/bushcraft-stone-axe.json`; `asset-forge/tools/tree_felling_probe.py` regenerates its VXA, VOX, OBJ, preview and validation report in `asset-forge/out/tree-felling-prototype/`.

## Controls

- Left click swings the equipped axe; its hit occurs 0.22 seconds into a 0.65 second swing.
- Keys 2 / 3 / 4 select 100 / 200 / 300 mm **per side**. The default is 300 mm per side, not 300 cubic millimetres.
- The gold outline shows reachable wood within 3 metres and uses the same cut volume as the strike.
- Console `voxel.EnvironmentLOD.Reset` restores the standing tree and removes falling pieces and the stump collision proxy.
- Console `voxel.TreeFelling.Axe` toggles the axe off/on to return to ordinary mining.
- G switches walking/flying; `[` reduces flying speed for close inspection.

## Implemented behavior

The new axe generator creates a curved wooden haft, an irregular stone wedge with actual flake removals, and several cord wraps plus a diagonal hitch. It is one connected object, with 1,552 occupied voxels, and passes deterministic generation, health, connectivity and exact VXA round-trip checks.

Axe cuts edit the existing authoritative tree grid and incrementally update its LOD sections. A partial notch leaves the upper tree rooted. When a horizontal layer loses all supporting wood, the upper section detaches and the stump remains. The test cuts near human reach, approximately 1.7 m above the base; it does not automatically split the entire tree at its midpoint.

Detachment transfers existing finest-LOD mesh sections into a Chaos rigid body, rebuilding only sections that intersect the cut. On a qualifying ground impact, the upper body can split into two substantial fragments at a render-section boundary. Paired cross-section caps are prepared before impact and revealed on breakage.

## Deliberate prototype limits

- This applies to the isolated oak pilot, not every tree in generated terrain. The axe is a prototype equipped actor, not yet an inventory recipe with durability or resource costs.
- Support detection checks horizontal wood layers in this single-trunk tree. General branching structures need connectivity analysis and material strength before this can become the production destruction system.
- A 32 × 32 m terrain collision mesh, sampled at 200 mm, bridges the existing voxel terrain to Chaos. It does not update after terrain edits and cannot support bodies leaving its bounds.
- Rigid-body collision uses trunk boxes. Branches and leaves retain their voxel visuals but do not have matching collision; fragments ignore one another's physics bodies. Player movement still uses its existing voxel queries, so fallen bodies are not yet walkable obstacles.
- Physics uses 120 Hz substeps, up to 16 steps, zero restitution, friction, additional solver iterations, and bounded depenetration/angular speed for long timber. These are experimental session settings, not changes saved to project-wide physics configuration.
- Falling meshes currently retain the finest LOD. Streaming their geometry, reducing moving draw calls, better branch collision, sleep behavior, persistent fallen wood and harvesting are subsequent work.

## Validation

The UE editor target builds successfully. The rebuilt core test executable passed all 832 tests. Automated game runs exercise the actual axe swing and hit path; their logs and screenshots are under `Saved/tree-felling-game.log` and `ue-project/Saved/Screenshots/TreeFelling/`.

The original pre-hinge capture passed its validation: five successful partial cuts left the tree rooted; the sixth released it at 170 cm. The upper tree fell for approximately 4.3 seconds before ground impact split it into two bodies. Both fragments settled within about two seconds of the split and remained stationary through the observation period. The validator now additionally checks directional acceleration and impact energy; its latest report is `Saved/tree-felling-validation.json`.

The six cuts used 1.587–2.175 ms of CPU mesh work. Detachment and mesh transfer used 11.52 ms. These timings exclude subsequent rendering/GPU work and screenshot readback; detachment still needs optimization before widespread use.

Earlier runs exposed a backward-wound terrain collision mesh, which allowed bodies to pass through its upper surface. Correcting its winding was essential; damping and substeps alone did not solve it. The final run uses the corrected surface, only permits impact fracture against ground, and verifies sustained rest instead of treating a detachment log as success.
