# Handover, 2026-09-07 evening: water wake, menus, tile loading, loading screen, UI doctrine

The plan this session executed lives at `~/.claude/plans/linked-mixing-wave.md` (owner-approved
2026-09-07 afternoon). Its results are in three repo docs; this file is the map to them and the
honest list of what is open. Everything below is either committed or named as uncommitted.

## Commits, in order

| hash | set | state |
|---|---|---|
| `fe51c15` | Water: wake tap fix regenerated into M_WaterVoxel + M_Ocean; cold-launch tile evidence | image FAILED owner judgment; code retained, defect assigned (see Water) |
| `d2965c2` | Menu: `seg=MENU` frame instrument (HOOK 0) + tick gates behind `-VoxelMenuTickGates`; sun-scan throttle; the UI scale ini | measured, small real win, PNGs identical |
| `ab24732` | Async fine-tile loader behind `-VoxelFineTileAsync` (default 0); loading gate 3 `-VoxelLoadGateFineRing`; voxel-core `adoptWarmTile` | correct on every counter; worst frame -56% clears the floor; percentiles do not move |
| `668c96a` | Menu scalability behind `-VoxelMenuScalability` | frame time -36%, PNGs identical |
| `cf6dc9a` | Threaded loading curtain behind `-VoxelLoadingScreenThread`; `-VoxelLoadGateMaxWait` 60 -> 300; PSO config + `tools/voxel-pso-cache-record.ps1`; all doc corrections | `seg=LOADING` p99 23.9 ms through 7.6 s world stalls |

Uncommitted at time of writing: the scale-tolerant UI pass (~25 files in `VoxelEarthUI`,
compiled, being photographed and committed by an agent). The water-look work is NOT in the
tree: the owner paused wake/ripple tuning for live iteration, and the night pass is
preserved verbatim in `docs/patches/water-look-2026-09-07-night.patch` (commit 9288355),
with the four water files restored to `fe51c15`. Everything under `asset-forge/` belongs to a
Codex session and was never staged.

## Where the detail is

- `docs/tile-loading-async-2026-09-07.md`: Phase 0 (the "cold" launch that was not cold), the
  async loader design and its four-leg A/B with the A/A noise floor, the menu A/Bs, the Phase 4
  curtain gate, and the Lumen retraction.
- `docs/water-ocean-tides-plan-2026-09-04.md`: the wake-fix regen proofs, the owner FAIL on the
  wake image, the UI divergence list and the border-promotion table, and (once the Fable agent
  finishes) the Single Layer Water research and arms.
- `docs/adr/0011-scale-tolerant-ui.md`: the UI scaling doctrine, owner-directed. Read before
  touching any widget. Never a 1 px border; outline fonts only; icons vector or >= 2x; never
  rescale a widget constant to compensate for the display.
- `docs/backlog.md` §0.0o: the correction to the previous session's closing diagnosis.
- `docs/evidence/2026-09-07-lake-session-VoxelEarth.log`: the only copy of the log that session
  misread; md5 `010a9300e3efaa10d1810515e1e355c8`.

## What was refuted today, so nobody re-derives it

1. "The in-game hitches are synchronous tile loads." Four decodes of 262-326 ms in the whole
   session; `fineMs` 0.02-0.03 on every hitch frame; the stalls were render waits (50.6 s on
   frame 1, 6-8 s later) and raster-atlas fills. Held after a reboot too.
2. "The first launch after a reboot is a cold cache." Boot 13:03, launch 14:30, standby list
   41.5 GB, an untouched tile read at 4,015 MB/s. Evict the standby list before claiming cold.
3. "Lumen is armed in DefaultEngine.ini:717-730." Those lines are engine BaseScalability, inert
   because `r.DynamicGlobalIlluminationMethod` is never set. The menu draws the ocean actor and
   the clipmap; the sky rig is deferred while the world is held.
4. "Move the tile cache to the SSD (6.5 GB)." It is 17 GB; C: has 25.8 GB free. Dropped.
5. "Hand the loading widget to the MoviePlayer loading thread." Impossible for a NEW GAME in a
   running world (four engine facts in the memory note and `VoxelLoadingCurtainThread.h`).
6. "The menus are the wrong size at 1440p." They matched the zoomed mock to the pixel; the
   defect was a fractional scale on pixel-locked art. Re-authoring at 1440p is rejected.
7. The three menu backdrops' softness is real: 1920 px wide, upscaled 1.33x. Not a scale bug.

## Open, grouped by who can close it

### Owner judgment (frames exist or are being produced)
- The boat wake: PAUSED for live iteration. The real mechanism is over-injection
  (`VoxelRippleField.cpp:895`, one full ring per station per tick, ~9x per metre), not the
  foam curve; the surface is displaced 0.4-0.6 m up, which is also the "water inside the
  boat". Start the live session by applying the patch, building, and reading the
  `injected=` count per metre (must fall ~9x) BEFORE looking at any picture.
- Shore foam: never had a version the owner accepted; confounded by saturated foam in every
  frame so far. Note the lake site is a snow shoreline; a darker-shore site may be needed.
- Three 2 px promotions flagged as possibly too heavy: HUD bar ticks, HUD vitals bars, the
  Settings checkbox.
- Sky-light pair (`voxel.Sky.SkyLightAtGroundZ` 0/1, `voxel.Sky.FogInSkyCapture` 1/0), caustics
  default (`voxel.Water.Caustics` 0.5), hull mask (`voxel.Boat.HullMask`), glider parked look.
- UI framing after ADR-0011 (55% of width at 1440p); the INTERFACE SIZE row is the dial.
- Whether to flip `-VoxelFineTileAsync=1 -VoxelFineTileRingRadius=1` on by default. Evidence
  says: worst frame halves, nothing else moves, ring fill ~6 s at cap 2.

### Owner must supply
- The Mira-Thal `assets/menu_backgrounds/` originals (2754-5524 px wide) so
  `prepare_ui_assets.py --max-width 3840` can regenerate the six backdrops. Command in ADR-0011.
- `ue-project/Content/UI/Fonts/PressStart2P-Regular.ttf` (OFL). That file at that path is the
  whole change; the loader already looks for it.

### Engineering follow-ups, cheapest first
- Suppress engine screen messages during Menu/Loading: the loading screen currently shows
  "Preparing Shaders (1)" and the DisableAllScreenMessages hint to a player.
- `SeedJournal` dates its newest placeholder day 12 while the live day is 11; hunk in the water
  doc, needs `VoxelScreensUISubsystem.cpp:398`.
- HUD stays lit behind overlay panels (z-order); death stamp lacks season/year/age.
- Remove dead constants `TitleFontSize`, `TitleBoxWidth` (no widget reads them).
- 1080p: the 1060x760 shell needs responsive trimming (ADR-0011 decision 4), not a scale change.
- Collapse the two readers of `-VoxelFineTileRingRadius` (`VoxelWorldSubsystem.cpp:31353`,
  `VoxelRasterAtlas.cpp:440`) into one before any default flip.
- Async cap sweep 2 -> 4 only if `asyncJoins` recur post-preflight; on the SATA HDD it may buy
  nothing, and that is a legitimate result.
- Tile (-5,-3) carries ~4.8 s unattributed on both launches; the sync per-tile line now prints
  `read N ms`, so the next synchronous leg names it for free.
- Bake tile (-3,-3) and the x=-2 column if flight legs need more room; bound legs by
  `pawn + 8192 m < column edge`, never by pawn position.
- Reconcile baked-tile coverage: `DefaultGame.ini:103-112` says 15, `VoxelFineTileStreamer.h:283`
  says 24 of 289. Run `vxc_terrainprobe` at the lake column to name its biome.
- A live console dial for `DisturbanceFoamGain` (a cvar poking the MIDs on BOTH the sheet and
  the implicit near-field path); `-VoxelWaterMatScalar` reaches the sheet only and is launch-only.
- Loading-phase scalability headroom is deliberately unspent (sky/fog/shadows are live behind
  the opaque curtain); needs its own image gate at the reveal.
- Glider: touchdown persistence has no test; the 4 m boarding range misses by 11 cm at water
  level (use 2D distance or 6 m).
- The loading curtain cannot cover the first long frame of a run and is absent under the
  editor; `longestUncoveredFrameMs` measures the residue. The ~50 s frame-1 shader compile is
  DDC fill and needs a warm DDC or a cooked build; the bundled PSO cache needs a packaged build.
- Phase 3.5 image row is VOID (moving-capture A/A floor 95%); a parked capture at a fixed pose
  after the crossing is the way to get an image claim for the loader.

## Rules this session paid for again

- Copy the log before the next launch; UE rotates it and the last session's "copy" never
  existed.
- A file:line quoted from an exploration report is a claim about a file. Grep it.
- A gate that cannot come out the other way is not a gate; two of mine could not.
- An A/B whose arms produce plausible numbers can still be VOID; only an engagement line that
  is absent when it should be present catches a binary that predates its source.
- One editor per box, and a capture is not read-only with respect to build state when another
  agent has voxel-core edits in flight.
