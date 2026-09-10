# Tree appearance status — September 8, 2026

The user-approved spring appearance is installed in Asset Forge for all 49 tree profiles and their 36 seeds: **1,764 existing variants**. Geometry, seed identities, endorsements and rejections were preserved. Appearance sidecars are independent of canonical VXA files and review metadata.

[Forge](http://127.0.0.1:8731/?tab=forge&species=temperate-oak) uses the updated appearance in the interactive inspector. The [49-source comparison board](http://127.0.0.1:8731/static/appearance-pilot.html?collection=1) remains available.

## Appearance contract

- Solid RGB per bark face. Branch-coordinate ridges, plates, fibers and marks use voxel colors, without a bark face texture.
- Broadleaf outlines and needle sprays provide the approved leaf-only cutout exception.
- Bark and foliage receive identical deterministic voxel brightness (±8.5%), face brightness (±4.5%), and small warm/cool variation. These stay stable in asset-local coordinates.
- Spring colors only; seasonal recoloring remains outside scope.
- New saved tree seeds receive the active appearance on first inspection.

The active revision and palette checksum are stored in `asset-forge/rules/tree-appearance-active.json`. The palette is `asset-forge/rules/tree-appearance-spring-v2.json`.

## Verification completed

Staging completed all 1,764 variants without errors. Installation checked geometry and payload hashes and asserted unchanged review metadata. A subsequent independent read-only verification confirmed all 1,764 installed payloads exactly match their staged bytes and geometry/palette hashes. See `asset-forge/out/tree-appearance-collection-v2/install-report.json` and `verification-report.json`.

The isolated HTTP probe verifies that first inspection of an asset without appearance sidecars produces the exact batch payload, sends correct headers, refuses corruption, and preserves geometry and decisions. It also exercises creation of the new thumbnail. TypeScript checking and web build passed for inspector integration. The live oak inspector was visually checked with the player pawn.

Appearance thumbnail refresh is complete for all 1,764 variants; coverage and appearance revision bindings were verified. Separate sidecars preserve original thumbnails and canonical artifact hashes. The renderer uses the same metric leaf shapes and face-color formula, with an isometric camera and lighting; it is not pixel-identical to WebGL. Progress is recorded in `asset-forge/out/tree-appearance-collection-v2/thumbnails.log`. The live server was checked: oak preview and thumbnail responses exactly match the new sidecars, and stale appearance thumbnails are refused in favor of the original fallback. The latest TypeScript check and Vite build passed.

## Unreal verification and remaining work

The latest medium oak and Scots pine diagnostic meshes were imported into `/Game/Voxel/AppearancePilot/V2`, with wood and foliage slots checked. Controlled opaque/masked, near/far screenshots were captured in an isolated level. A double-gamma error was corrected after inspecting Unreal's glTF importer: imported vertex colors connect directly to BaseColor.

Visual evidence is in `asset-forge/out/tree-appearance-pilot/game-v2/captures-linear`. This validates the isolated material experiment. The editable published-tree actor now consumes source-bound appearance packets in the game; ordinary terrain-composed world scatter still uses its existing palette and has not received this appearance bridge.

Hidden-editor GPU timings were rejected because some profiles captured idle UI instead of the scene. The user subsequently closed the editor and coordinated GPU priority. Eight isolated Nanite static-mesh captures completed after correcting Shader Model 6 configuration, an unsaved studio material and missing Nanite material usage flags. These captures are diagnostic only; they are not accepted game performance evidence.

The user correctly raised arbitrary voxel destruction as the key constraint. The existing editable environment path rebuilds procedural mesh sections. Nanite is not a production dependency for the approved appearance. A full-source non-Nanite baseline has been completed; disabling Nanite on the mesh and rebuilding is required, since simply disabling the renderer uses a simplified fallback. Static-mesh GPU measurements must still be distinguished from procedural mesh rendering and edit/rebuild costs. A future intact-Nanite/edited-procedural hybrid is an unproven optional optimization, not the chosen architecture.

The non-Nanite baseline subsequently completed eight clean game captures: two reversed-order repeats of each oak/pine opaque/masked pair. On the Radeon RX 7800 XT at 1280×720, median GPU time was 1.671 ms opaque versus 1.979 ms masked for oak, and 1.753 ms versus 2.599 ms for pine. These are entire single-tree studio frames, not isolated shader timings or forest frame budgets. Each run samples 700 settled frames after startup and the screenshot; repeated medians agree closely. Full source meshes had Nanite disabled and were rebuilt, avoiding reduced fallback geometry. Visual checks confirmed cubic geometry, colors, leaf cutouts and the pawn. `benchmark-nonnanite/analysis.json` retains exact run statistics. Normal-world integration, dense-forest load and edit/rebuild validation remain outstanding.

`tools/tree-appearance-game-benchmark.ps1` checks the prepared renderer mode and refuses to run alongside another Unreal process. Results for the non-Nanite baseline belong under `game-v2/benchmark-nonnanite`; the older `benchmark` and initial `benchmark-sm6` runs contained setup failures and must not be used interchangeably.

The diagnostic meshes are expensive: the original 24 GLBs total approximately 1.62 GB, with the largest cedar at 1,885,648 quads. These are not approved production forest meshes. Bounded LODs, dense-scene GPU tests and integration with the game's asset appearance path remain outstanding.

## Editable game path verified

`export_tree_runtime_appearance.py` exports VAC1 presentation packets for every occupied source cell, including interior cells that edits can expose. Packets bind to canonical VXA geometry, active palette, and payload integrity. Color and cutout coordinates remain in asset-local space. Geometry, collision, material IDs, seeds and review decisions are independent of these packets.

The generic non-Nanite environment actor applies appearance during initial construction, carving, synchronous restoration, staged restoration and tree detachment. Chopping a trunk preserves appearance on the falling geometry and fracture surfaces. Saved falling sections retain broadleaf/needle material flags. Legacy saved sections keep their former interpretation.

Native D3D12 tests passed with zero reported test errors/warnings:

- `Voxel.Appearance.SourceBinding`: packet integrity and geometry binding, interior/coarse samples and stable face variation.
- `Voxel.Appearance.EditableActor`: rotated fixture carve and synchronous/staged restoration of geometry, colors and UVs.
- `Voxel.Appearance.RealTree`: actual oak and Scots pine geometry, newly exposed faces, colors and UVs; optional felling path chops the source trunk and checks detached sections and restoration.
- `Voxel.Objects.StagedRestore`: legacy saved-object regression following the felling metadata change.
- Published inventory fixture: a valid leftover VXA/record/packet absent from the current inventory cannot authorize a new spawn.

Reports are under `asset-forge/out/tree-runtime-appearance-v1`, including `felling-oak-automation`, `felling-pine-automation`, `felling-legacy-automation` and `revoked-publication-automation`. These tests prove geometry/material data behavior, not full-forest frame time or visual quality of falling motion.

The actual engine export currently contains 21 explicitly endorsed oak/birch variants with matching appearance packets. `appearance/published.json` is refreshed from current endorsements and matching exported bank bytes. Withdrawn entries leave the placement inventory; retained sidecars can still support existing saved objects. Pending candidates cannot enter through `voxel.Tree.SpawnPublished`.

## Remaining validation and deployment

The nine-tile lowland terrain bake completed with no incomplete or stale superblocks. Existing hydrology routing-apron warnings remain recorded. A 16-tree visible scene completed placement, screenshots and a successful 0.69 ms carve; its wrapper timed out during otherwise normal Unreal shutdown, so it is diagnostic rather than a clean repeat. A hidden-tree control subsequently completed through the wrapper. Earlier alpine placement and missing-fine-tile failures remain rejected runs.

The preliminary visible capture measured about 11.43 ms median GPU time before the edit, versus 6.19 ms with those trees hidden. Both had roughly 122 ms total frames, with the visible capture spending about 87 ms in the outer Slate/UI timing scope; inner Slate drawing timings were much smaller. This is unresolved frame latency, not evidence of playable performance. The output is 1280×720 with an evidenced internal view of 832×468. The approved trees look coherent in the capture, but existing terrain and legacy world vegetation show visible defects. Neither timing nor appearance is accepted as a finished forest slice.

Clean repeated captures now use a frozen copy of forest banks, appearance packets and species.vxm. The wrapper fingerprints these inputs before and after capture; comparisons require matching hashes, actual placements, source seeds, module, palette and resolution. This prevents concurrent plant retirement from changing the baseline. Two clean visible and two clean hidden captures and further frame-latency investigation remain required.

Ordinary terrain-composed forest appearance, the full species-manifest deployment and its world-generation compatibility checks remain unfinished. The published actor path does not by itself complete automatic world population. No world-version bump or unrelated coral regeneration has been made as an appearance shortcut.

Cutouts decorate voxel surfaces rather than creating individual leaves throughout the canopy. Fresh-color patches are spatial rather than shoot-assigned. Nearest-branch bark coordinates can show seams at junctions. These remain visible review criteria.


## 2026-09-08 settled full-game stand comparison

Four matched passes (visible, hidden, hidden, visible) completed with streaming pending/in-flight counts zero and no streaming activity during measurement. Evidence: `out/tree-runtime-appearance-v1/forest-settled-sequence1/comparison.json` and each pass's analysis/CSV/screenshots. Sixteen published editable trees, RX7800XT,1280x720output,832x468internal. GPU median beforeedit13.67275ms visible versus8.05315ms hidden (+5.6196ms); afteredit13.547875 versus8.0861 (+5.461775ms). This is whole-tree/shadow/scene cost, not isolated leaf material cost.

Overall frame time remains approximately120ms in both visible and hidden controls after streaming settles. This fails a playable-frame-rate acceptance and needs CPU trace attribution. Cold startup to first settled capture took about15m58s. Root reviewed the first visible capture: revised trees look coherent; old surrounding terrain vegetation and ground remain visually inconsistent. This comparison does not establish automatic approved world-scatter integration or complete biome visual acceptance.
