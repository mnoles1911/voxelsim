# Derived detail meshes

Status: implementation proposal, not a working cache. Runtime mesh construction
reached 105 ms in walk-capture-17. Vertex sharing reduces redundant buffers but
does not eliminate the synchronous mesh build.

The VXA/VAC publication remains authoritative. An editor bake should consume the
endorsed, visually approved publication and create persistent UStaticMesh packages
with committed source mesh descriptions, persisted LOD thresholds, and persistent
material instances. Transient runtime meshes and dynamic material instances must
not simply be saved as if they were cook-ready assets. An explicit preview mode
may consume the private test fixture, but its packages must remain separate from
approved production output and must never endorse source variants.

Share the existing geometry, appearance, LOD, and mesh-description code between
the runtime fallback and the bake. Identify outputs by geometry and appearance
SHA-256, mesher/LOD/material/wind contract versions and complete settings. Record
engine/platform cook identity separately. The index should include object paths,
expected pitch, bounds, LOD counts and triangle counts. Normal cooking must include
the derived packages explicitly.

Runtime async loading should skip geometry generation on validated cache hits.
Keep pending instances until loading completes; never mark missing geometry ready.
Development misses may use the current fallback with explicit telemetry. A
packaged publication needs complete compatible cache coverage before activation.
Retain loaded resources while groups use them; releasing unused resources must
account for in-flight geometry snapshots and rendering lifetimes.

Required validation: bake and close the process, reload in a fresh process, then
cook and load on the target platform. Compare attributes, cutouts, wind, bounds,
LOD buffers and transforms with the source path. Measure cold loading separately
from mesh construction, including the worst frame and retained memory. Neither an
editor-only load nor successful package saving proves packaged runtime readiness.
