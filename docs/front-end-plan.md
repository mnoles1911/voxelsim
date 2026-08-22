# The front end: main menu, loading screen, and how to verify them

The port of the Mira-Thal / *Voxelmark* front end into VoxelEarth. This is the
operational document -- what exists, what is unproven, and what to run. The
decisions and their reasoning are in
[ADR-0009](adr/0009-slate-front-end-and-committed-ui-art.md); this file assumes
you have read it or do not need to.

## What ships

| Screen | Where |
|---|---|
| Main menu -- title, six buttons, version stamp | `SVoxelMainMenu` |
| LOAD GAME panel, with real save rows | `SVoxelMainMenu::BuildLoadPanel` |
| HELP / CREDITS / SETTINGS placeholders | `SVoxelMainMenu::BuildMessagePanel` |
| Loading screen -- hourglass, quip, bar, tip, FPS | `SVoxelLoadingScreen` |
| The animated hourglass | `SVoxelHourglass` |
| Named saves behind CONTINUE / LOAD / DELETE | `VoxelSaveLibrary` (in `VoxelEarth`) |

All of it is C++ Slate in `ue-project/Source/VoxelEarthUI/`. There is no
`.uasset`, no widget blueprint and no `.umap`.

**Deferred, deliberately:** the pause menu (its seam is the `Playing` state in
`UVoxelFrontEndSubsystem`), the settings screen, and menu music.

## The flow

```
boot -> Menu -> [NEW GAME | CONTINUE | LOAD] -> ArmLoading -> Loading -> HandOff -> Playing
                                                    |            |          |
                                          2 painted frames   the gate   0.4s fade
```

Streaming is held shut for the whole of `Menu`: `UVoxelWorldSubsystem::Tick`
returns on its first line while `ChunkOwner` is null, and
`StartWorldSession()` is what spawns it. Nothing about that path is new -- it
is the same early-out the dedicated server has always taken.

`ArmLoading` spends two FRAMES (not seconds) painting the curtain before the
world starts. Godot needed the equivalent because a synchronous scene load
froze the main thread for 5-8 s; here there is no blocking load, but
`StartWorldSession`'s first tick builds the initial desired set for a whole
cascade and is the most expensive frame of the session.

## The readiness gate

Two tests, both of which must hold for three consecutive 0.4 s polls:

1. **Spatial.** 7 radii (20-250 m) x 16 directions = 112 probes around the
   spawn column, each asking `IsChunkPresentableAt` one metre above the
   analytic surface. Every one must pass.
2. **Streamer idle.** Pending and in-flight both zero on every gated ring.

Both are needed. Idle alone passes in the lull before the streamer has been
asked for anything -- on a cold start, frame one. Spatial alone passes as soon
as the ground exists with half the visible ring still meshing.

Then: hold at least `-VoxelLoadMinHold` (15 s), leave as soon as the gate
passes, leave regardless at `-VoxelLoadMaxHold` (60 s, logged as a **warning**).

### `GateMaxRing = 3` is a hypothesis, not a measurement

R5 against a 60 s maximum would always time out -- a full 4 km cascade settles
in 80-86 s cold at 39,020 chunks -- and a gate that never passes is not a gate.
R4/R5 cover ground `AVoxelClipmapActor` already draws as a heightfield to
~30 km. The spatial probes stop inside R2, so R3 is one ring of margin.

**Run the sweep before quoting this number as fact:**

```powershell
foreach ($n in 1..5) { tools\voxel-ui-capture.ps1 -Shot GateSweep -GateRing $n -MaxHold 180 }
```

and record time-to-gate in `docs/measurements/front-end-gate-<date>.txt`.

## The progress bar

```
P = clamp(max(P_prev, max(elapsed/MaxHold, 0.25*probeHits/112 + 0.75*ringFill)), 0, 0.995)
```

Godot's was `elapsed/total` and nothing else -- honest about time, wrong about
work. On a warm cache it read 30% when the world was ready; on a cold fill,
100% while chunks were still landing. Each term here stops one failure: the
time floor stops a work bar sitting still through an R3 tail; the monotone
clamp stops it going backwards, which it genuinely would, because
`RecomputeDesiredSet` grows the desired set as the anchor settles; the 0.995
cap means 100% is only ever reached by the gate passing.

## Verification

CI does not compile the Unreal module (`ue-build.yml` is gated off: 30 GB
engine, 14 GB runner disk). Verification is headless screenshots plus two
lints.

### Screenshots

```powershell
tools\voxel-ui-capture.ps1 -Shot Menu                      # the main column at rest
tools\voxel-ui-capture.ps1 -Shot Panel -Panel load         # the LOAD panel, empty state
tools\voxel-ui-capture.ps1 -Shot Panel -Panel help
tools\voxel-ui-capture.ps1 -Shot Fallback                  # no font, no art
tools\voxel-ui-capture.ps1 -Shot Hourglass                 # a 0.0..1.0 strip
tools\voxel-ui-capture.ps1 -Shot Loading -At '0.5,6,20'
```

Each prints the run's `VoxelFrontEnd:` and `VoxelLoadGate:` log lines beside
the image it produced. **Read those first.** A capture whose log says
`VoxelFrontEnd: suppressed` is a photograph of something else.

### The non-regression diff

The one that matters most, and the reason step 1 of this work landed with no
pixels in it:

```powershell
# Through the front end...
tools\voxel-ui-capture.ps1 -Shot GateSweep   # or any -VoxelMenuAutoStart run
# ...against the same world without it, then:
python tools\imgdiff.py <before.png> <after.png>
```

Pass condition is the documented within-session noise floor. The front end must
not change a single pixel of a world capture.

### Lints (these DO run in CI)

```sh
python tools/lint-unity-collisions.py          # now covers VoxelEarthUI
python tools/lint-frontend-switch-coverage.py  # every -Voxel* switch classified
```

The second one exists because the front-end suppression rule is a naming
convention, and the failure when that convention does not reach a new switch is
silent: the capture stops at a menu nobody is there to click and hangs until
`-VoxelMenuWatchdog` kills it. Adding a switch that follows no convention now
forces a one-line decision in
`tools/frontend-switch-classification.txt` instead.

## Switches

| Switch | Effect |
|---|---|
| `-VoxelFrontEnd=0\|1`, `-VoxelNoMenu` | Force the front end off or on |
| `-VoxelMenuShot[=<s>]` | Settle, photograph the menu with UI, quit |
| `-VoxelMenuPanel=load\|help\|credits\|settings` | Open that panel first |
| `-VoxelHourglassShot=<p>[,<p>...]` | The hourglass alone, on a flat field |
| `-VoxelLoadingShot[=<s>]`, `-VoxelLoadingShotAt=<s,s,s>` | Press NEW GAME, capture at each offset |
| `-VoxelMenuAutoStart[=<s>]` | Press NEW GAME, no capture, no quit |
| `-VoxelReadyProbeLog` | One line per readiness poll |
| `-VoxelLoadGateMaxRing=<n>`, `-VoxelLoadMinHold=<s>`, `-VoxelLoadMaxHold=<s>` | Gate tuning |
| `-VoxelUINoAssets` | Force the no-font, no-art path |
| `-VoxelMenuWatchdog=<s>` | Unattended: exit rather than sit on the menu |

The four capture switches force the front end ON (`kFrontEndCaptureSwitches`),
because `-VoxelMenuShot` contains "Shot" and rule 5 would otherwise suppress
the very thing it exists to photograph.

## Saves

`Saved/SaveGames/<slug>/{meta.json, world.vxlog}`, newest first -- which is the
definition of CONTINUE, not a presentation choice. Five-autosave FIFO cap;
named saves are never pruned.

The pause menu is where the Godot build lets a player create a save, and it is
deferred, so these exist in the meantime:

```
voxel.SaveGame <name...>
voxel.ListSaves
voxel.DeleteSave <slug>
```

A session opened from a named save autosaves back INTO it on shutdown --
without that, the next CONTINUE would quietly reopen the state from the moment
of loading and lose the session.

## Known-unverified

**If you are the session that can build: read
[`front-end-local-verification-handoff.md`](front-end-local-verification-handoff.md)
first.** It sequences everything below and names the four places a first build
is most likely to break.

Nothing here has been compiled or photographed. One thing HAS been checked
without an engine: the hourglass geometry was rasterised from the same formulas
`SVoxelHourglass::OnPaint` encodes, and the sand stays inside the glass at every
progress value, the mound reaches the waist exactly at 1.0, and the caps and
pillars are symmetric about the diamond. That validates the maths, not the
Slate path that draws it. Two claims are specifically
open:

- **Colour round-trip.** `FLinearColor(FColor)` decodes sRGB and Slate
  re-encodes on output. Whether `#f0c14b` survives is untested;
  `voxel.UI.SRGBTint` A/Bs it at the single conversion site. **Do not call this
  port pixel-exact until that probe runs.**
- **Font metrics.** Macondo at 84 px will not lay out identically in Slate and
  Godot. Expect a few pixels of difference in title width and centring.

Two cvars exist today: `voxel.UI.SRGBTint` (the colour A/B above) and
`voxel.UI.HoverSlide`, which carries the HTML mock's 4 px hover slide and
defaults to 0 because the shipped Godot build does not have it.

The mock's other two extras -- the scanline grain overlay and the radial
vignette -- are **not implemented at all**, not merely switched off. The
vignette in particular should stay that way unless somebody measures it: the
Godot build had it, as a full-screen shader, and removed it because a
full-viewport per-pixel pass during chunk streaming spiked frame delta into
visible-stutter range. If it comes back, it should come back as a baked
texture on one full-screen image, which `FVoxelUIAssetLibrary` can now load
trivially.

Godot also sets `viewport.disable_3d` under the curtain. That is **not**
ported, and deliberately: `OnWorldBeginPlay` precaches the terrain PSO because
"the engine compiles its pipeline-state-object the first time a real chunk
actually reaches GetDynamicMeshElements, synchronously stalling whichever frame
that lands on". Suppressing world rendering for 15-60 s would push every
un-precached first draw onto the frame after the curtain lifts -- the exact
hitch that precache exists to prevent. Adding it later is a measurement, not a
port.
