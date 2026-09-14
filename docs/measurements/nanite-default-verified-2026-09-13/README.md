# The Nanite default engages with no flag, and it reproduces

`walk-capture-59-default-nanite` against `walk-capture-60-nanite-off`, one
binary, frames 0:200, temperate forest at `-154740,-81476`, 256 m ring. Both
receipts passed. The two bakes differ only by the Nanite flag, which is in the
cache identity precisely because it changes the built mesh.

**The engagement proof is the command line.** Arm 59 carries **no Nanite flag at
all** and was accepted against the `nanite=1` bake; arm 60 carries
`-VoxelNoDetailNanite` and was accepted against the `nanite=0` bake. Had the
default not flipped, arm 59 would have computed `nanite=0` for its identity and
the guard would have refused that bake rather than measuring it. Zero
material-audit fallback lines in either log — the failure that made the
2026-09-11 null (`walk-capture-41`) say Nanite was worth +0.73 ms while 253 log
lines said the material lacked `bUsedWithNanite`.

## The result

| counter (median, frames 0:200) | Nanite off | default (on) | change |
|---|---|---|---|
| **FrameTime** | 20.594 | **13.935** | **−32.3%** |
| RenderThreadTime | 20.580 | 13.944 | −32.2% |
| **GPUTime** | 19.349 | **12.652** | **−34.6%** |
| GPU/Basepass | 4.399 | 0.030 | −99.3% |
| GPU/RenderVelocities | 4.398 | 0.006 | −99.9% |
| GPU/VoxelMarch | 8.882 | 8.904 | +0.2% |
| DrawCall/Basepass | 131 | 4 | |
| DrawCall/RenderVelocities | 127 | 0 | |
| GPUSceneInstanceCount | 125,062 | 125,167 | +0.1% |
| GameThreadTime | 6.335 | 6.310 | −0.4% |

**48.6 fps to 71.8 fps.** Every figure is within a few hundredths of the
2026-09-11 measurement on the opt-in flag (−32.3% frame, −34.6% GPU, basepass
−99.3%, velocities −99.9%), which is the reproduction this default needed.

The marcher does not move (+0.2%), which is the control that says this is the
ground cover and nothing else, and the instance count does not move either — the
plants are all still there, drawn a different way.

## What this does not settle

**Bake coverage is now load-bearing.** Only the baked path produces Nanite
meshes: `CreateDetailStaticMesh` uses `bFastBuild=true`, which never allocates
Nanite resources, and in a non-editor build that is the only option available. A
plant from a cache miss or the geometry fallback keeps the traditional proxy, so
partial coverage means a population drawn both ways at once. Zero fallbacks on
these two runs — but that is a property of THIS bake at THIS site, not of the
code, and it wants re-checking whenever the species set or the bake scope
changes.

Trees are untouched and keep their destructibility constraint.
