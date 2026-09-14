# Size culling on a route, with the pictures

Two walks of the authored detour route at the 256 m ring, back to back on the
binary built 2026-09-11 10:24, cache `temperate-authored-lods-full-13`. Both
passed 8 of 8 waypoints (136.36 m and 136.39 m travelled). Size culling proved
engaged on the ON arm by the harness gate and by 228 `DetailSizeCull` lines in
its log. No foreign GPU load at start or during either run.

Screenshots and the full comparison:
<https://claude.ai/code/artifact/cce7dd92-382b-4756-90a7-b4aaa90980ab>

## The numbers

| counter (median ms) | cull OFF | cull ON | change |
|---|---|---|---|
| FrameTime | 35.33 | 28.92 | −18.1% |
| **GPUTime** | **34.77** | **20.02** | **−42.4%** |
| RenderThreadTime | 33.38 | 6.96 | −79.2% |
| **GameThreadTime** | **28.89** | **28.88** | **−0.0%** |
| GPU/Basepass | 11.70 | 4.31 | −63.1% |
| GPU/RenderVelocities | 11.56 | 4.32 | −62.6% |
| GPU/VoxelMarch | 9.38 | 9.31 | −0.7% |
| DrawCall/Basepass | 225 | 173 | −23.1% |
| GPUSceneInstanceCount | 129,562 | 129,914 | +0.3% |
| frames | 1,629 | 1,888 | |

**28 fps to 35 fps**, and the understory's two passes go 23.26 ms to 8.63 ms.

This is the third independent pair to say the same thing. Walk 24/25 gave
30.22 → 8.86 and walk 26/27 gave 29.80 → 8.78, both on 2026-09-10 walks; this
is a route on a different binary a day later, and it lands at 23.26 → 8.63. The
absolute numbers differ because the route's poses differ from the walk's. The
effect does not.

**The resident instance count went slightly up**, +0.3%, while cost fell 63%.
`SetCullDistances` on a hierarchical instanced component is consumed on the CPU
in the cluster-tree traversal, which drops whole subtrees before any run is
emitted; it removes nothing from the component or from GPUScene. Cost follows
the drawn set, and no counter in the CSV reports that set.

## The thing this pair settles that the earlier ones could not

**With size culling on, the frame stops being GPU-bound.** Game thread 28.88 ms
against a GPU of 20.02. The game thread did not move at all — 28.890 to 28.884 —
because nothing size culling does touches it.

So the ordering is now decided by measurement rather than by argument. Size
culling is the GPU lever and it is finished. The next 8.9 ms has to come off the
game thread, and
`docs/measurements/gamethread-query-recompute-2026-09-11/` says where: 69% of the
quiet game thread is expensive queries rebuilt every frame for answers that
barely change, none of which is a visual trade.

## How far each plant actually reaches

From the ON arm's own log, every distinct plant mesh it loaded and the distance
the rule handed it. 228 meshes:

| draw distance | meshes |
|---|---|
| 32 m | 77 |
| 48 m | 58 |
| 64 m | 31 |
| 80 m | 16 |
| 96 m | 6 |
| 112 m | 8 |
| 128–240 m | 17 |
| 256 m (full ring) | 15 |

**135 of 228 now stop at 48 m or nearer** — the tufts, mosses and low herbs.
Fifteen keep the whole ring, and those are what the far field is made of.

## The pairs are genuinely pose-matched

The route is scripted, but "should match" is not a measurement. Taken from the
two walks' own position logs, the arrival positions differ by:

| stand | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| apart | 2 cm | 6 cm | 2 cm | 19 cm | 8 cm | 5 cm | 2 cm | 1 cm |

Worst pair 19 cm, most within 8. So what differs inside a pair is culling, not
parallax. That is well inside the 2–6 cm band earlier moving-capture work
established as the floor, at the one stand that is worse than it.

## What this does not settle

- **Popping.** Still frames cannot show an instance crossing its draw distance
  while you walk. Not captured, and out of scope by the owner's instruction.
- **The fade.** The rule starts fading at 85% of each plant's distance, but
  nothing has confirmed the material fades across that band rather than
  switching.
- **Residency or memory.** Nothing is removed from the world; this is draw
  distance only.
- **The default.** That is the owner's call on the pictures.

## The route that was not used, and why

The first attempt used the shore *survey* route and aborted at its last
waypoint — arrived 6 of 7, 130.85 m of 135.47. That is not a 256 m problem:
route-capture-12 failed identically at the 48 m ring, 6 of 7 and 130.75 m. The
survey route's final leg is not walkable, which is why the detour route exists,
and the detour passes 8 of 8 at both rings. Use the detour.
