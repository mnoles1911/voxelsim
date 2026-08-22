# Handoff: compiling and verifying the front end on a local machine

Everything in `docs/front-end-plan.md` was written in a remote cloud session
with **no Unreal install**. It has never been compiled, never been run, and
never been photographed. CI cannot close that gap either — `ue-build.yml` is
gated off because UE 5.8 is a 30 GB install against a 14 GB runner disk.

This file is for the session that *can* build. Work it top to bottom; each
stage's failures are cheaper to diagnose before the next stage runs.

**What has been verified** (so you do not re-do it): both CI lints pass; the
hourglass geometry was rasterised from the same formulas the C++ encodes and
checked by eye (sand stays inside the glass at every progress value, the mound
reaches the waist at 1.0, caps and pillars are symmetric); all 37 loading-screen
strings are byte-identical to the GDScript source; and `FVoxelMenuLayout`'s ini
loader was diffed member-by-member against the struct.

---

## Stage 0 — before you build

```sh
python tools/lint-unity-collisions.py
python tools/lint-frontend-switch-coverage.py
```

Both should say `clean`. If the second one does not, somebody has added a
`-Voxel*` switch without classifying it — read its output, it says what to do.

**Unrelated pre-existing breakage you will hit and should ignore:**
`voxel-core/include/voxelcore/weather.h` uses `size_t` without including
`<cstddef>`, which fails several `bench/` probes under gcc 13. It fails
identically at `5b6226c` (the commit before this work), so it is not from the
front end. The registered ctest suite passes. Worth fixing separately.

## Stage 1 — build voxel-core, then the editor target

```sh
cmake -S voxel-core -B build/voxel-core-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/voxel-core-msvc
"<UE_5.8>/Engine/Build/BatchFiles/Build.bat" VoxelEarthEditor Win64 Development ^
  -project="<repo>/ue-project/VoxelEarth.uproject" -WaitMutex -NoHotReloadFromIDE -DisableAdaptiveUnity
```

`VoxelEarth.Build.cs` requires the voxel-core static lib to exist first, and
**never pass `-DisableUnity`** — that is what let PR #210's collision ship.

**Then build the GAME target too, not just the editor.** `VoxelEarthUI` is
`ClientOnly`, and at least one bug in this work (`MipGenSettings`, now guarded)
compiles cleanly in the editor and fails only in Game/Shipping:

```sh
"<UE_5.8>/Engine/Build/BatchFiles/Build.bat" VoxelEarth Win64 Development -project=...
```

### The four places I expect a first-build failure

In descending order of likelihood. All are API-shape guesses that a read-only
audit could not settle; none is a design problem.

1. **`IImageWrapper::GetRaw`** (`VoxelUIAssetLibrary.cpp`) — uses the
   `TArray64<uint8>` form. If 5.8 wants something else, change the parameter on
   `DecodeToBGRA`, `FinishDecode` (header and .cpp) and the captured `Pixels`
   together; they are one chain.
2. **`FSlateDrawElement::MakeLines`** (`SVoxelHourglass.cpp`) — passes
   `TArray<FVector2D>`, deliberately, on the grounds that the `FVector2f`
   overload is newer. If that is backwards in 5.8, change the `Pd` lambda to
   return `FVector2f` and delete the conversion. **Note the asymmetry is
   intentional:** `FSlateVertex::Make` genuinely does take `FVector2f`.
3. **`FSlateRenderer::GetResourceHandle`** (`SVoxelHourglass.cpp`) — passes the
   explicit three-argument form. If only the one-argument convenience exists,
   drop the trailing two.
4. **`FNavigationMetaData::SetNavigationWrap` / `SetNavigationStop`**
   (`SVoxelMenuButton.cpp`) — fairly confident, not verified.

### One thing that will COMPILE and may still be wrong

`UWorldSubsystem::DoesSupportWorldType(const EWorldType::Type)` is what both
`UVoxelFrontEndSubsystem` and the pre-existing `UVoxelWorldSubsystem` override.
If 5.8 has superseded it with a `const UWorld*` form, **both still compile and
both silently stop being called** — the subsystems would then be created in
editor worlds. Check the base class once. If it is wrong, it is wrong in the
older file too, which is worth knowing on its own.

## Stage 1b — run the headless tests

Cheapest feedback available, and it needs no display:

```sh
UnrealEditor-Cmd.exe VoxelEarth.uproject -unattended -nullrhi -nop4 ^
  -ExecCmds="Automation RunTests VoxelEarth.FrontEnd; Quit"
```

Three tests, covering the front end's pure logic — the parts whose invariants
are invisible in a screenshot:

- `LoadProgress` — the bar is monotone, floored by elapsed time, never reaches
  1.0, and clamps out-of-range inputs rather than propagating a NaN into a
  full-width fill.
- `Darkened` — Godot's `Color.darkened()` scales sRGB BYTES, not light. Both
  the pressed and disabled button states are defined through it, so the obvious
  linear-space implementation is wrong on every menu.
- `Slugify` — including the case that matters most: an all-punctuation save
  name must not slug to the empty string, because a save directory named `""`
  IS the saves root.

While you are there, `Automation RunTests VoxelEarth` runs the twelve
pre-existing ones too.

## Stage 2 — the non-regression diff (do this BEFORE looking at any menu)

This is the one that protects the capture archive, and it is why the engine
seams landed as a commit with no pixels in it.

```powershell
# Same world, same pose, with and without the front end in the path:
<capture>  -VoxelSpawnAt=-84480,53760 -VoxelSpawnAltM=2000 -VoxelSpawnPitch=-25 `
           -VoxelTimeOfDay=12:00 -VoxelDate=03-20 -VoxelTimeScale=0 -VoxelScreenshotAfter=150
<same>     -VoxelFrontEnd=1 -VoxelMenuAutoStart=1 <...identical switches...>
python tools\imgdiff.py <a.png> <b.png>
```

Pass condition is the documented within-session noise floor. **The front end
must not change a single pixel of a world capture.** If it does, the invariant
in `VoxelFrontEndPolicy.h` is broken and that is more important than anything
cosmetic below.

Also re-run two or three existing fixtures unchanged (`-VoxelOverlayShot=30`,
`-VoxelScreenshotAfter=120`, one water and one cave fixture) and diff those
against the archive.

## Stage 3 — first look at the menu

```powershell
tools\voxel-ui-capture.ps1 -Shot Menu
```

The script prints the run's `VoxelFrontEnd:` and `VoxelLoadGate:` log lines
**before** the file list. Read those first — a capture whose log says
`suppressed` is a photograph of something else entirely.

Expect: background art under a 55% wash, `VOXELMARK` in Macondo at 84 px in
gold, the subtitle, six oak buttons with a wide gap before QUIT, the version
stamp bottom-left, and **CONTINUE and LOAD GAME greyed out** (no saves exist
yet — that is correct, not a bug).

### Then the colour probe — the highest-value single thing you can do

`FLinearColor(FColor)` decodes sRGB and Slate re-encodes on output. Whether
`#f0c14b` survives that round trip is an **assumption**, and it is the one
claim that decides whether "1:1 clone" is true.

Sample the title's pixel colour in the capture. If it is not `#f0c14b`:

```
voxel.UI.SRGBTint 0
```

and capture again. Whichever arm lands on the source hex is correct; put the
answer in `VoxelUITheme.cpp`'s comment and **delete the hedge in
`docs/front-end-plan.md` that says nothing here is pixel-exact**. Until this
runs, that hedge stays.

Expect font metrics to differ by a few pixels regardless (Slate and Godot
hint and lay out differently). `Config/DefaultVoxelUI.ini` corrects that with a
text edit and no rebuild — that is what it is for.

## Stage 4 — the rest of the captures

```powershell
tools\voxel-ui-capture.ps1 -Shot Panel -Panel load     # empty-state string, 720x560 frame
tools\voxel-ui-capture.ps1 -Shot Panel -Panel help
tools\voxel-ui-capture.ps1 -Shot Fallback              # no font, no art
tools\voxel-ui-capture.ps1 -Shot Hourglass             # 0.0..1.0 strip
tools\voxel-ui-capture.ps1 -Shot Loading -At '0.5,6,20'
```

**Fallback** should show a legible menu on flat `#0a0a0f` in the engine's
default face, with **no dark wash** — the tint is dropped along with the art on
purpose, because black-on-black is how a graceful degradation becomes an
unreadable screen.

**Hourglass** — compare against `docs/images/` if you keep one, or against the
strip in this session's history. The geometry has been checked; what has not is
that Slate's `MakeCustomVerts` path renders it at all.

**Loading** at 6 s should have a partly-drained top chamber, a formed mound,
grains in flight, a part-filled bar, a quip, a TIP footer and an FPS readout.

## Stage 5 — the measurement

`GateMaxRing = 3` is reasoned, not measured, and its comment says so. Settle it:

```powershell
foreach ($n in 1..5) { tools\voxel-ui-capture.ps1 -Shot GateSweep -GateRing $n -MaxHold 180 }
```

on a **cold** tile cache. Record time-to-gate per ring in
`docs/measurements/front-end-gate-<date>.txt`, put the winning number in
`FVoxelReadyProbeConfig::GateMaxRingLevel`, and rewrite its comment to cite the
measurement instead of the reasoning. R5 is expected to always hit the 60 s
timeout — a full 4 km cascade settles in 80–86 s cold.

## Stage 6 — play it

The things no screenshot covers:

- NEW GAME → curtain → world, with no visible hitch as the curtain lifts. If
  there IS one, the suspect is the PSO precache, and
  `docs/front-end-plan.md` explains why `viewport.disable_3d` was deliberately
  not ported.
- `voxel.SaveGame Test One` → quit → relaunch → **CONTINUE is now enabled** and
  opens where you left off.
- Drive the whole menu on a gamepad without touching the mouse: D-pad moves,
  wrap works at both ends of the column, A activates, B backs out of a panel.
- Escape on a sub-panel returns to the main column; Escape on the main column
  does nothing (deliberate — an unprompted exit is how people lose work).

## When something is wrong

- `docs/front-end-plan.md` — what each piece does and which file it is in.
- `docs/adr/0009-slate-front-end-and-committed-ui-art.md` — why it is built
  this way, including the alternatives that were rejected and why.
- `docs/backlog.md` §11 — the open items, and the deferred work (pause menu,
  settings, music, per-save seeds) with their seams described.

Two failure modes worth naming in advance:

**"The menu never appears."** Check the log for `VoxelFrontEnd:`. If it says
`suppressed`, the reason is in the same line. If there is no such line at all,
`VoxelEarthUI` did not load — check the `.uproject` module entry and the
`ExtraModuleNames` in the target files.

**"A capture hangs for five minutes then exits with an error."** That is
`-VoxelMenuWatchdog` doing its job: an unattended run reached the menu. Either
the run needs `-VoxelMenuAutoStart`, or a switch it passes should have been
classified as self-driving — see `tools/frontend-switch-classification.txt`.
