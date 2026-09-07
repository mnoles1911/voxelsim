# After-captures for the scale-tolerant UI pass (ADR-0011)

Fourteen before/after pairs for the uncommitted VoxelEarthUI pass photographed on
**2026-09-07, 21:58-22:08 UTC**. Nothing here is a verdict on how any of it
looks; the frames and the conditions are recorded so the owner can judge them.

## What binary these are pictures of

| | |
|---|---|
| `UnrealEditor-VoxelEarthUI.dll` | 2026-09-07 **16:44:51** -0400 |
| `UnrealEditor-VoxelEarth.dll` | 2026-09-07 **16:44:51** -0400 |
| newest `Source/VoxelEarthUI` file | `VoxelFrontEndSubsystem.cpp`, 16:38:46 -0400 |
| repo HEAD at capture time | `245960c` |

Every source file of the pass predates the DLL, so the pass is in this build.
**No build was run for these captures** — the harness `tools/voxel-ui-capture.ps1`
has no stale-lib guard (that is `tools/voxel-capture.ps1`), so photographing did
not disturb the build state.

## Pose and conditions

All fourteen were taken with `tools/voxel-ui-capture.ps1` at `-ResX=2560
-ResY=1440`. **Every frame is 2560x1398**, not 2560x1440: the log records
`work area: (0, 0) -> (2560, 1392)` for `\\.\DISPLAY2`, so Windows clamps the
requested window. This matters for absolute pixel figures — see *Measurements*.

World-bearing shots (pause, save, the five screens, death, dialogue, HUD) all
spawn at the harness's mandatory `-VoxelSpawnAt=-61440,-61440` and settle 2.0 s.
The load gate reported `READY` with `hits 112/112, fineRing=1/1` on every one.

The UI-scale row the pass adds was at its **default** on every capture; the
engagement line is in each log:

    VoxelGraphicsUserSettings: applied FineDetailSmoothing=0 FasterTerrainDrawing=1
    WaterWaveDetail=1 OceanMeshDetail=1 UIScale=1.00

so nothing here is a picture of a manual scale override.

Press Start 2P is still absent and says so once per run:

    Warning: Menu pixel font not loaded (file missing); falling back to the engine
    default face. Expected at: .../Content/UI/Fonts/PressStart2P-Regular.ttf

(The warning's wording is the shared loader's; `FVoxelUIStyle::Pixel` actually
falls through to VT323, not to the engine face.)

## The pairs

`pairs/*.png` are 5132x1442. Both halves are pasted at **1:1, unresampled** —
nothing is scaled, because a size comparison through a resample is not one. The
before frame is at x `0..2559`, the after frame at x `2572..5131`, both from y
`44`; crop those boxes to recover either original losslessly.

| screen | BEFORE frame | AFTER frame | capture command |
|---|---|---|---|
| Main menu | `VoxelMenu00015` | `VoxelMenu00029` | `-Shot Menu` |
| Settings panel | `VoxelMenu00016` | `VoxelMenu00030` | `-Shot Panel -Panel settings` |
| Load dialog | `VoxelMenu00018` | `VoxelMenu00031` | `-Shot Panel -Panel load` (0 saves, `-VoxelNoLoad`) |
| Loading screen | `VoxelLoading00004` | `VoxelLoading00005` | `-Shot Loading` (default `-At 6`) |
| Pause menu | `VoxelPause00005` | `VoxelPause00008` | `-Shot Pause` |
| Save dialog | `VoxelPause00006` | `VoxelPause00009` | `-Shot Pause -Panel save -DemoSaves` (`6 save(s) listed`) |
| Inventory | `captures/inventory.png` | `VoxelScreen00017` | `-Shot Screen -Panel inventory` |
| Journal | `captures/journal.png` | `VoxelScreen00018` | `-Shot Screen -Panel journal` |
| Map | `captures/map.png` | `VoxelScreen00019` | `-Shot Screen -Panel map` |
| Player | `captures/player.png` | `VoxelScreen00020` | `-Shot Screen -Panel player` |
| Codex | `captures/codex.png` | `VoxelScreen00021` | `-Shot Screen -Panel codex` |
| Death | `captures/death.png` | `VoxelDeath00001` | `-Shot Death` |
| Dialogue | `captures/dialogue.png` | `VoxelDialogue00002` | `-Shot Dialogue` |
| HUD | `captures/hud.png` | `VoxelHud00003` | `-Shot Hud` (implies `-VoxelDemoVitals=100,100,0`) |

Each before frame was taken with the **same** switches, so the pair differs in
the build and not in the harness. The load dialog is deliberately left at its
empty state in both, matching the earlier pass.

**Both sides were taken at the same UI scale.** ADR-0011 restored continuous
scaling and the before set was already taken under it, so the two halves are
directly comparable in size — no division by 1.333 is needed to compare them
with each other (only to compare either with the 1080p mocks).

### The captures are real

`-Shot Panel -Panel save` produced a byte-identical copy of the main menu on the
earlier pass (md5 `e9d094f9…` on both `VoxelMenu00015` and `VoxelMenu00017`),
because the title screen has no save panel to open. All fourteen after frames
were md5'd: **fourteen distinct hashes, none equal to any before hash.**

    904e23666ea2275f0f91e667cdc520eb  VoxelMenu00029      (menu)
    c429b480f7460baf991973b496e027dc  VoxelMenu00030      (settings)
    ec83cd67de2227c6b4ac04f506a9d925  VoxelMenu00031      (load dialog)
    8df65215269052f82a831ee447818550  VoxelLoading00005   (loading)
    3894f86b4fc03908d8c478cb688ff08c  VoxelPause00008     (pause)
    49868c16244da8d7c54088a935312fdd  VoxelPause00009     (save dialog)
    e23aabcae9c0c2fbe32ca97c65b84e69  VoxelScreen00017    (inventory)
    adf80fc6a7c730837e005a80c0cdb424  VoxelScreen00018    (journal)
    2c8165866f4007ffadb67f13b19b1369  VoxelScreen00019    (map)
    7690975bf9aa750536a872567309470c  VoxelScreen00020    (player)
    4c1c09783d062a7f865d4dc91a36221f  VoxelScreen00021    (codex)
    11111c9e20cd024a1702f33d5d7676ba  VoxelDeath00001     (death)
    7090ca0d483685599b2d63e73e4b3ca0  VoxelDialogue00002  (dialogue)
    449efc7ae92b3ce6f17607568ae924c3  VoxelHud00003       (HUD)

## The three items the pass flagged for an owner verdict

`crops/*.png` are before over after, **cropped 1:1 with no resampling**, so what
is on screen is what the build drew.

| crop | what to look at | measured |
|---|---|---|
| `crops/hud-bar-ticks.png` | the nine tick rules across the HP bar, promoted 1 -> 2 units where the CSS asks for 1 px | the promotion most likely to read as too much black; the pass's own note says the honest fix, if it does, is a lower tick alpha and not a 1-unit rule |
| `crops/hud-vitals-bars.png` | how much of each bar is coloured fill vs black surround | the saturated-red run through the HP bar is **12 px before, 7 px after**; the saturated-blue run through the hunger bar is **12 px before, 9 px after** (measured as the rows passing a fixed saturation test, so the ramp's darkest stop is not counted) |
| `crops/settings-checkbox.png` | a ticked box's mark against its well | the warm mark is **18x18 px before, 13x13 px after** (`CheckboxMarkSize` 14 -> 10 units, forced by the well's two rings going 1 -> 2 units each) |

None of these can be settled without a picture, which is why they are crops and
not a claim.

## Measurements

### The settings panel fits, and the fold is gone

`SettingsPanelHeight = 660` is deleted; the panel is content-sized.

* **Before (`VoxelMenu00016`)**: a lighter 11-px-wide vertical band at x
  `1607..1617` runs the height of the panel body — the scroll bar. OCEAN MESH
  DETAIL is below it and not drawn.
* **After (`VoxelMenu00030`)**: no such band anywhere in x `1560..1680`; the
  only light columns there are the panel's own leather ring at `1663..1665`.
  All four GRAPHICS rows are drawn, **OCEAN MESH DETAIL included, with both
  lines of its hint**, above the ESC / APPLY / SAVE & LEAVE footer.
* Panel outer extent at the panel's centre column, background-to-background
  (this includes the drop shadow, which cannot be separated from a dark
  backdrop by brightness): **y `251..1167` = 917 px before, y `123..1295` =
  1173 px after.**
* `tools/capture-pixdiff.py` on the pair: 11.95% of pixels changed, bounding box
  `861..1698, 123..1295` — the panel and nothing else — with a background bias
  ratio of 0.071, so the control is sound and there is no global shift.

### The shell width, ADR-0011's own falsifier

ADR-0011: *"on a 2560-wide capture the in-game screen shell must measure ~1413
px. ~1060 px means a scale of 1.0 is in force and the curve did not take."*

**Measured: 1326 px.** The shell's outer black border is unambiguous on a
transect — at y=700 in `VoxelScreen00021`, background art at x=616, black at
617-619, and black at 1940-1942 with world again at 1943. Journal, map, player
and codex all measure `1326 x 906`; inventory uses the wider shell and measures
`1348 x 944`.

**The curve took.** 1060 authored units at a scale of 1.0 would be 1060 px, and
1326 is 25% above that. Two independent readings put the application scale at
~1.29, not the ADR's 1.333:

* the settings checkbox mark is authored 14 units before and 10 after, and
  measures 18 px and 13 px — 1.286 and 1.30;
* `RulePx = 2` units draws a 3-px band (x 617-619).

~1.29 rather than 1.333 is what the clamped window gives: the UI curve keys on
the **shortest side**, and this window is 1392-1398 tall rather than 1440
because Windows reports a 2560x1392 work area. A genuine fullscreen 1440p
session has a shortest side of 1440 and lands on 1.333. **That last sentence is
an inference from the curve, not a measurement — no fullscreen capture exists.**

The residual gap — 1326 px measured against 1060 x 1.29 = 1366 px expected — is
the shell already being drawn narrower than its own constant, which
`docs/water-ocean-tides-plan-2026-09-04.md` recorded for the Map screen before
this pass ("~7% narrower"). **The before captures measure the identical
`1326 x 906`, so this pass did not move it** and it is not a regression of the
pass. It is an open question of its own.

### Other pixel diffs worth quoting

* **Main menu**: 7.44% of pixels changed, background bias ratio 0.016. The heat
  map is confined to UI — the wordmark (shifted, `LogoRight` 120 -> 100), the
  callout's re-flowed body (the CSS-to-Slate line-height conversion), the menu
  buttons and the cartouche. The backdrop art is untouched.
* **Load dialog**: 1.14% changed, bounding box `779..1780, 289..1108` — the
  dialog only.
* **Death screen**: the version stamp `Milestone 5-3D — dev build` is present at
  bottom-left in the after frame and absent from the before frame.
* **Inventory**: the after frame carries `Search…` in a narrowed field, the
  bronze hexagon and `80 kg` weight readout, and `×16` stack badges on both the
  pack slot and the hotbar slot.
