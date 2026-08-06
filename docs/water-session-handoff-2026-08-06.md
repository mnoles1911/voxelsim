# Water session handoff — 2026-08-06

Branch **`claude/f6-interior-rim-injection`**, 7 commits ahead of `main`.
Read `docs/water-system-architecture.md` first — it is the durable design doc
and it already contains everything below in permanent form. This file is the
"what to do next" note.

---

## What changed, and what is measured

**Three mechanisms were losing river. All three are now located; one is fixed.**

| | state | where |
|---|---|---|
| **F6** — the pyramid delivered discharge onto the tile's 960 m apron, not into the tile | **FIXED**, off by default | `HYDROLOGY_RESIDUALS` #7, `water_inject_at_interior_rim` |
| **F3** — the depth law has no slope term | open | `bake/water.py:200` |
| **F2** — drawn width barely tracks discharge | open | `fill_to_local_surface` |

**F6, measured on three tiles.** Both arms in one process, superblock shared:

```
-7,-5    interior max Q  1.30362e6 -> 3.91347e8 m3/yr   (0.041 -> 12.40 m3/s)  x300.20
-14,-5   interior max Q   2.2185e6 -> 1.38127e7          (0.070 ->  0.438)       x6.23
-4,-4    interior max Q  9.71616e7 -> 9.71616e7          bit-identical            x1.00  (control)
```

The OFF arm reproduces `HYDROLOGY_RESIDUALS` #7's own independently recorded
figures for (-14,-5) exactly (2.22e6, 1.32e7, "a 6x drop"), which validates the
harness as well as the fix. **Water-only gate clean on all three: elevation,
accumulation and flow bit-identical, 0 of 67,108,864 cells.**

**The default has NOT been flipped.** Doing so rolls `bake_ver` and invalidates
every baked water plane in every cache root. That is the next decision.

**Two things I claimed and then retracted** — both are in
`docs/measurements/valley-bed-vs-extent-2026-08-05.txt` with the method errors:

* *"The detail band buries the river."* False, and already measured before I
  said it: 0.52 m clearance at the centreline, 1.05% buried. I had done
  arithmetic on `kFineDetailOctaves` without applying `kDetailNoiseScale`.
* *"Channels are V-notches with no bed."* False. The terrain offers p50 24–37 m
  of submerged room against a law asking 1.5 m, already 75–82% used. **No
  terrain re-bake, no lateral erosion, no bed correction is warranted** — off
  the network the bake reproduces its own carrier to 2 cm.

---

## Stage A: the magenta water marker — BUILT, NOT YET CAPTURED

`-VoxelWaterMarker=1` voxelises every column between its ground and the baked
water surface as solid magenta `MAT_WATERMARK`, through the **terrain** path —
so it is visible at full clipmap range instead of only inside the near-field
renderer's ±25.6 m horizontal / ±12.8 m vertical disc. `-VoxelWaterMarkerOcean=0`
marks inland water only.

`Build.bat` → **Result: Succeeded**. `ctest` 100%, 492 assertions.

### The one capture that has not been taken

```powershell
tools\voxel-capture.ps1 -Name water-marker-wet-trunk `
  -SpawnAt '-60688,-51716' -SpawnAltM 400 -SpawnPitch -35 -SettleSec 180 `
  -CoarseTileDir 'D:\vox-wet-cache\terrain-diffusion-unlabeled-80b9ca451a23eae4\000000000135276f\s1' `
  -FineTileDir  'D:\vox-wet-cache' `
  -FineProviderId 'terrain-diffusion-unlabeled-80b9ca451a23eae4-b10cf6d2c' `
  -Cvars 'voxel.GPU 0' `
  -ExtraArgs '-VoxelWaterMarker=1','-VoxelWaterMarkerOcean=0'
```

Four things in that command are load-bearing:

* **`voxel.GPU 0` is mandatory.** `worldgen.ush` has no `MAT_WATERMARK` branch,
  so the GPU path disagrees with the CPU path while the marker is on. Mirroring
  it is the top open task.
* **`-FineTileDir` is the cache ROOT; `-CoarseTileDir` is an s1 LEAF.** Different
  shapes. Swapping them fails in different ways.
* **The spawn is the trunk, not the tile centre** — the highest-discharge cell
  measured on (-4,-4), Q = 8.82e7 m³/yr.
* **Altitude + downward pitch**, because a ground-level shot is evidence about
  ground cover and not about landform.

**Read the log before reading the picture.** Two failure modes here produce a
frame that looks like a placement bug: a fine tile `was REFUSED` (the
`Source/ThirdParty/zstd` module is absent so `CODEC_ZSTD` is compiled out — the
wet tiles look RAW at 229 MB, but confirm), and a leg that did not settle.

---

## Open, in priority order

1. **Mirror `MAT_WATERMARK` into `voxel-core/shaders/worldgen.ush`** and re-gate
   with `vxc_gpu`. Until then the marker is CPU-only.
2. **Take the capture above** and let the owner judge it. Do not offer a verdict.
3. **Decide whether `water_inject_at_interior_rim` becomes the default.** Rolls
   `bake_ver`; invalidates every baked water plane.
4. **F3 — put slope in the depth law.** Falsifiable acceptance test already
   stated: if the slope term is right, `bridge_to_face_contact` should become
   close to a no-op on steep reaches. If it is still doing heavy lifting, the
   depth model is still wrong.
5. **F2 — the extent rule.** It must thread between the current rule and
   nearest-channel, which was measured at a **209×** flood.
6. **The exponent decision** (`water-system-architecture.md` §11b). `width ∝
   Q^0.398` means a 10× bigger river needs ~320× the discharge and this world
   spans about 6× at the trunk. An owner decision, not an implementation detail.

---

## Environment notes that cost time this session

* **Epic's MCP server is real and already configured.** UE 5.8 ships
  `ModelContextProtocol`; the `.uproject` enables it;
  `Config/DefaultEditorPerProjectUserSettings.ini` sets `bAutoStartServer=True`
  on port 8000 path `/mcp`; repo-root `.mcp.json` points at it. It only needs an
  editor running. **A Claude Code session started while the editor is down will
  not see the server at all** — launch the editor first, then start the session.
* Measurement legs now default the MCP server OFF via a `-ini:` override
  (`-Mcp` keeps it on). `ShouldAutoStartServer()` has no command-line opt-out.
* **One editor per box.** An open editor holds `UnrealEditor-VoxelEarth.dll` and
  makes `Build.bat` fail with `LNK1104`.
* **`Build.bat` exits 0 on failure.** Gate on `Result: Succeeded`.
* **`dump_stage_heightfields.py --bake` produces NO water.** Its `_coarse_fetch`
  returns elevation only and discards climate, so `padded_climate` is None and
  the entire B6 pass is skipped silently. Its `S1` is also the **B3.relaxed**
  stage, not the shipped surface — B4/B4b/B5 run after it and move the channel
  12.6 m.
* **Reuse the bake's own instrument.** Three wrong answers in one session came
  from re-deriving something the bake already computes: flow direction, the
  perpendicular transect, and which surface discharge was accumulated on.
