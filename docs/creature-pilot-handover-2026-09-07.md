# Creature pilot handover

All eight static pilot appearances were accepted by the user on 2026-09-07:
"all pilot assets look great." Approval is recorded in each library entry and the pilot manifest.

The single maintained process and lessons document is [Astra creature modeling pipeline](astra-creature-modeling-pipeline.md).
Model IDs, dimensions and interpretation limits remain in [the manifest](../asset-forge/refs/reconstruction/pilot-manifest.json).
The delivered models and Forge workflow merged in PR #238 (20a9d74).

Review at http://127.0.0.1:8731/static/pilot-review.html. If unavailable, launch the Asset Forge desktop shortcut first: the gallery requires the local server. The launcher refreshes a verified stale Forge process from the current checkout.

The pilot automation is paused. No goblins or Tripo work. Rigging, animation, collisions,
LOD and game integration remain separate; all runtime_ready flags remain false.
The last game-export diagnostic had stale environment banks unrelated to appearance approval.
Do not bulk publish the pilot or alter other sessions' water/UI work or baseline curation edits.

Cleanup retired the failed procedural reconstruction generator and failed raven segmentation script,
while preserving the orthographic helper and every accepted-model refinement dependency.
Historical evidence and source licensing records remain available; old code remains in Git history.
