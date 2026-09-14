# Nanite on the understory, actually measured this time

Three walks at the temperate forest site, 256 m ring, size culling on (the
default since this afternoon), on the binary built 2026-09-11 18:2x. Quiet
stretch of each, frames 0–199. Receipts passed.

The control and the Nanite arm come from **two bakes that differ only by
`-VoxelDetailNanite`**, because that flag is in the cache identity precisely
because it changes the built mesh. One cache cannot serve both arms; the
identity guard refused that attempt twice and was right to.

## The result

| counter (median) | control | Nanite | change |
|---|---|---|---|
| **FrameTime** | 20.522 ms | **13.887 ms** | −32.3% |
| **GPUTime** | 19.310 ms | **12.632 ms** | −34.6% |
| RenderThreadTime | 20.527 ms | 13.880 ms | −32.4% |
| GameThreadTime | 12.537 ms | 12.816 ms | +2.2% |
| **GPU/Basepass** | 4.402 ms | **0.029 ms** | **−99.3%** |
| **GPU/RenderVelocities** | 4.399 ms | **0.006 ms** | **−99.9%** |
| GPU/NaniteVisBuffer | — | 1.045 ms | |
| GPU/NaniteBasePass | — | 0.120 ms | |
| GPU/VoxelMarch | 8.879 ms | 8.917 ms | +0.4% |
| DrawCall/Basepass | 131 | **4** | |
| DrawCall/RenderVelocities | 127 | **0** | |
| GPUSceneInstanceCount | 125,060 | 125,114 | +0.04% |

**49 fps to 72 fps.** The ground cover's two passes go from 8.801 ms to 0.035,
and Nanite's own passes cost 1.165, so the net is **8.80 → 1.20 ms**.

The marcher does not move, which is the check that this is the ground cover and
nothing else. `RHI/DrawCalls` rises 501 → 943 because Nanite's internal
dispatches are counted there; the draws that matter, the base pass and velocity
submissions, collapse to 4 and 0.

## The velocity pass did not get cheaper. It stopped existing.

Zero draw calls. Nanite vertex factories are excluded from velocity shader
compilation entirely and velocity is exported from the visibility buffer
instead.

That matters beyond the milliseconds: **it removes the velocity cost without the
visual trade.** The per-component flag landed earlier today
(`-VoxelDetailNoWpoVelocity`) buys roughly the same time by discarding wind
motion vectors, which smears foliage under temporal upscaling. Nanite keeps the
motion vectors and removes the pass. If Nanite ships, that flag is redundant.

## What the earlier null was

`walk-capture-41` on 2026-09-11 measured +0.73 ms and read as "Nanite is a null".
It was not measuring Nanite. The plant material lacked the `bUsedWithNanite`
usage flag, so the material audit failed and every component fell back to the
traditional proxy while Nanite resources sat registered and streaming. That
capture's own log said so 253 times. Every log in this set says it **zero**
times.

One line in `create_detail_asset_material.py`, and the asset regenerated.

## Two things that are not settled

**The lighting changes, and nobody has looked.** Enabling Nanite puts these
plants into the Lumen scene for the first time — the traditional dynamic
instanced path is excluded from it. That is a visual change, and a 34% GPU win
is not a reason to ship one unseen. The same route should be shot both ways and
judged the way size culling was judged.

**Cache coverage becomes load-bearing.** There are two mesh producers. The baked
path goes through the normal derived-data build and honours the Nanite settings.
The runtime path never allocates Nanite resources at all, and in a non-editor
build it is the only option available. So **any plant that comes from a cache
miss or the geometry fallback can never be Nanite**, and a shipped build with
partial coverage gets a permanently mixed population with the traditional proxy
still drawing part of it. That is a shipping question, not a measurement one,
and it needs answering before this becomes a default.

## Where the frame now stands

At the 256 m ring, standing still, with size culling and the frame-scoped asset
shortlist both in:

| | this morning | now | with Nanite |
|---|---|---|---|
| Frame | 41.7 ms | 20.5 ms | **13.9 ms** |
| GPU | 40.5 ms | 19.3 ms | **12.6 ms** |
| Game thread | 21.6 ms | 12.5 ms | 12.8 ms |
| fps | 24 | 49 | **72** |

**The marcher is now 8.9 of a 12.6 ms GPU frame — 70% of it.** It has become the
constraint, and the height pyramid that passed its gate this afternoon is the
lever aimed at it.
