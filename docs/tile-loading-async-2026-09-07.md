# Tile loading: async streamer investigation (2026-09-07)

## Phase 0: cold-cache launch at the lake pose

### What was run

```
tools\voxel-capture.ps1 -Name coldlaunch-lake -SpawnAt '-65102,-51084' -SpawnAltM 8 -SpawnPitch -18 -SettleSec 60
```

Foreground, unattended, one editor on the box (`Get-Process UnrealEditor*` empty before launch).
Wall clock 403.9 s for a 60 s settle. Frame written to
`D:\voxelsim\ue-project\Saved\Screenshots\WindowsEditor\VoxelVerify00962.png`
(pose: column -65102,-51084, ground top z=1644.2 m, camera z=1652.2 m = +8.0 m above the
surface, pitch -18, yaw 45, sun frozen 12:00 on 03-20, river ribbons DISABLED by the harness
default).

Log: `D:\voxelsim\Saved\capture-coldlaunch-lake.log` (the harness passes `-abslog`; it does NOT
write `ue-project/Saved/Logs/VoxelEarth.log`, which is still the 12:54 session copy).

Tier line, quoted:

```
[2026.09.07-18.30.35:246][  0]LogVoxelEarth: Fine tier ENABLED: root=D:/voxelsim/tile-cache
provider=terrain-diffusion-unlabeled-80b9ca451a23eae4-b5e821e98 seed=20260719 pitch=1875 mm/px
ringRadius=0 budget=12.00 GiB.
```

### The four fine-tile loads

Four tiles, all inside the first 35 s of the world, exactly the set the last session named.
"Read gap" is the interval from the immediately preceding log line to the `resident:` line;
because the load is synchronous on the game thread, that gap is an **upper bound** on
read + decode. The decode ms is the engine's own number and is the prewarm half only.

| tile | MB read / file MB | preceding line | `resident:` line | read gap (s) | decode (ms) | implied read rate |
|---|---|---|---|---|---|---|
| (-5,-4) | 342.6 / 430.7 | 18:30:43.622 | 18:30:44.436 | 0.814 | 326 | ~700 MB/s |
| (-4,-4) | 315.4 / 403.5 | 18:31:02.489 | 18:31:03.363 | 0.874 | 261 | ~515 MB/s |
| (-5,-3) | 206.6 / 293.5 | 18:31:04.431 | 18:31:09.502 | 5.071 | 280 | ~43 MB/s |
| (-4,-3) | 289.3 / 376.5 | 18:31:09.502 | 18:31:10.233 | 0.731 | 275 | ~634 MB/s |

Total bounded game-thread cost of tile loading: **7.49 s**, of which decode is 1.14 s (15%).

`blockingLoads=3` of 4 loads; `gateLeaks=0`; `ringRadius=0 ringCentre=(-5,-4) ringMoves=0`:

```
[2026.09.07-18.31.14:086][267]LogVoxelPerf: Fine tier (window): resident=4 tile(s) 1.57/12.00 GiB
(decoded 0.50 GiB) | loaded=4 absentOnDisk=0 corrupt=0 identityMismatch=0 refusedTiles=0
retriesSuppressed=0 | blockingLoads=3 gateLeaks=0 | ringRadius=0 ringCentre=(-5,-4) ringMoves=0
```

**Zero `Fine tier: residency tick stalled the game thread` lines in the whole run.**

### Bracket choice changes the per-tile rate by 3x, so no clean cold number exists

Tile (-5,-4) can be bracketed two ways and they disagree:

```
[2026.09.07-18.30.42:948][  0] Fine tier ENABLED: root=D:/voxelsim/tile-cache ...
[2026.09.07-18.30.43:622][  0] Spawn column override: spawning at column (-65102.0, -51084.0) m
[2026.09.07-18.30.44:436][  0] Fine tile (-5,-4) resident: ... 342.6 MB read ... full decode 326 ms
```

From the tier line: 1.488 s for 342.6 MB = **~230 MB/s**, which sits right on the SATA HDD
sequential ceiling. From the last line before it: 0.814 s = **~421 MB/s** gross, ~700 MB/s once
the 326 ms decode is removed, which the platter cannot do. The wide bracket also contains the
spawn-pose work, so neither is a clean read time.

Tile (-4,-3) has no such ambiguity and still comes out too fast:

```
[2026.09.07-18.31.09:502][  9] Fine tile (-5,-3) resident: ... 206.6 MB read ... full decode 280 ms
[2026.09.07-18.31.10:233][  9] Fine tile (-4,-3) resident: ... 289.3 MB read ... full decode 275 ms
```

0.731 s for 289.3 MB = **~396 MB/s** gross, ~634 MB/s net of its 275 ms decode. Something is in
play beyond a platter read: OS read-ahead of the neighbouring tile file, the read overlapping the
previous tile's logging, or a warm cache. **No clean per-tile cold number should be claimed from
this leg.**

### The premise did not hold: the file cache was not cold

`Win32_OperatingSystem.LastBootUpTime = 2026-09-07 13:03:28` and this was the first *editor*
launch after the reboot, so the leg was scheduled correctly. But first-launch-after-reboot and
cold-file-cache are two different claims, and the second one is false here: 80 minutes elapsed
between boot and the leg, and in that time something read most of the tile cache into RAM.

The plan (F1) predicted "~2 s per tile off the HDD at ~150 MB/s". The measurement refuses that.

1. The same four tiles in the previous (warm) session's log, same bracket method:

   | tile | warm gap (s) | warm decode (ms) | "cold" gap (s) | "cold" decode (ms) |
   |---|---|---|---|---|
   | (-5,-4) | 0.623 | 275 | 0.814 | 326 |
   | (-4,-4) | 0.869 | 262 | 0.874 | 261 |
   | (-5,-3) | 4.976 | 263 | 5.071 | 280 |
   | (-4,-3) | 0.651 | 277 | 0.731 | 275 |
   | **total** | **7.119** | | **7.490** | |

   The "cold" run reproduced the warm run tile for tile, to within 0.2 s on three of four.

2. Three of the four implied read rates (515-700 MB/s) exceed the 600 MB/s SATA link that
   `D:` sits on, and `D:` is confirmed spinning rust:
   `Get-PhysicalDisk` gives disk 0 = `ST2000DM008-2FR102`, MediaType HDD, BusType SATA, and
   `Get-Partition` puts `D:` on disk 0.

3. Direct test. A 468.2 MB tile file in a **different provider namespace** (`b19d281fd`, not the
   `b5e821e98` this run used, so untouched by the run) read end to end in **0.117 s = 4015 MB/s**.
   That is memory speed, not disk speed.

4. The mechanism: box booted 13:03:28, the leg ran at 14:23-14:30, and immediately afterwards
   `\Memory\Standby Cache Normal Priority Bytes` = **41,526 MB** with 63.9 GB installed
   (`\Memory\Available MBytes` = 0, i.e. the standby list is holding everything it can).
   The 17 GB tile cache fits in the standby list twice over and is evidently in it.
   Something read it during the 80 minutes between boot and this leg.

Point 3 is the falsifier that matters: it is a file this leg never opened, in a namespace this
leg never resolved, and it came back at memory speed. A launch can be the first after a reboot
and still run against a warm page cache.

**So there is still no cold-disk number for fine-tile loading, and this leg cannot produce one
without a way to evict the standby list.**

### What the 5 s on (-5,-3) actually is

It is not read bandwidth. While the OS was serving this cache at 4 GB/s, the engine's smallest
read (206.6 MB) took 8x longer than its largest (342.6 MB). The gap spans frames 7 to 9, so two
frames elapsed inside it; the log attributes only 280 ms of it (the decode). The remaining ~4.8 s
is **unattributed** by any instrument in this log. It is the same ~5 s in the warm session, so it
is structural, not cache state.

This is the one thing worth chasing before the async loader is justified on tile I/O: a
5 s per-tile cost that does not scale with bytes and does not come from the disk is not a
loading problem, and moving it to a worker thread would hide it rather than remove it.

**The cheapest way to name it is already half-built.** The sync per-tile line
(`VoxelFineTileStreamer.cpp:569`) reports only `full decode %.0f ms` and no read time, which
is exactly why 4.8 s of this leg is unattributed. The async publish line the loader agent is
adding (`:1994`) already reports `worker read %.0f ms + full decode %.0f ms` separately. Give
the **sync** path the same split — it is the same two timers around the same two calls — and
the next leg names the 4.8 s without any async arm being enabled at all. That is a strictly
better first step than an A/B, because it can tell the async work what it is buying.

### What the frame instruments say the hitching is

First three `Voxel frame dist` windows (all `settleT=NOT-SETTLED`, `seg=FILL`):

```
[18:31:00.799] window=7.6s  seg=FILL scope=window n=1   hitches=1  meanMs=7565.85 maxMs=7565.85
[18:31:10.919] window=10.1s seg=FILL scope=window n=8   hitches=4  meanMs=1264.91 maxMs=6480.71
[18:31:15.923] window=5.0s  seg=FILL scope=window n=468 hitches=1  p50Ms=8.10 p95Ms=12.60 p99Ms=19.40 maxMs=1019.08
```

Largest hitch triplets, by `renderWaitMs`:

```
[18:31:00.799] Hitch frame: frameMs=400.00 | subsystemTickMs=14.53 elsewhereMs=385.47 |
  renderMs=23795.77 renderWaitMs=50626.63 rhiMs=0.00 gameWaitMs=0.00
[18:31:33.558] Hitch frame: frameMs=400.00 | subsystemTickMs=50.91 | renderMs=40.75 renderWaitMs=7907.47
[18:31:10.934] Hitch frame: frameMs=400.00 | subsystemTickMs=9.50  | renderMs=17.39 renderWaitMs=6450.91
[18:31:01.482] Hitch frame: frameMs=400.00 | subsystemTickMs=6.58  | renderMs=469.72 renderWaitMs=5236.94 rhiMs=334.64 gameWaitMs=588.30
[18:31:04.427] Hitch frame: frameMs=400.00 | subsystemTickMs=10.02 | renderMs=9.98  renderWaitMs=1918.42
```

`frameMs=400.00` is the instrument's clamp; only `renderWaitMs` carries the magnitude.
37 `Hitch frame:` lines in the run. The paired `recompute` lines carry `fineMs=0.02`-`0.03`
throughout, i.e. **the hitch census does not attribute any of these to fine-tile work**.

#### Which stalls follow a tile load and which do not

Interleaving the four `resident:` lines with the five largest stalls:

| time | event | relation to a tile load |
|---|---|---|
| 18:30:44.436 | tile (-5,-4) resident | — |
| 18:31:00.799 | **renderWaitMs=50626.63** (frame 1; window meanMs=7565.85) | **NOT after a tile load.** 16.4 s after tile 1 and 2.6 s *before* tile 2. Only one tile had ever loaded. |
| 18:31:01.482 | renderWaitMs=5236.94 (rhiMs=334.64, gameWaitMs=588.30) | NOT after a tile load; still between tiles 1 and 2. |
| 18:31:03.363 | tile (-4,-4) resident | — |
| 18:31:04.427 | renderWaitMs=1918.42 | 1.06 s after tile 2 — the only one of the five that plausibly trails a load. |
| 18:31:09.502 | tile (-5,-3) resident | — |
| 18:31:10.233 | tile (-4,-3) resident | — |
| 18:31:10.934 | **renderWaitMs=6450.91** | 0.70 s after tile 4, immediately after the third and fourth loads. |
| 18:31:33.558 | **renderWaitMs=7907.47** (subsystemTickMs=50.91, dispatchMs=36.57) | **NOT after a tile load.** 23.3 s after the last one; `loaded=4` never rises again all run. |

So the single largest stall in the run (50.6 s of render wait) and the third largest (7.9 s)
have no tile load anywhere near them, and the 6.45 s one does sit just after two loads. Even
that one carries `fineMs=0.02` on its own recompute line. On this leg the render-thread backlog
is the disease and tile loading is at most a contributor to one of five.

Raster-atlas fills, game thread per window (the 6561-page atlas):

```
4.5 ms -> 21.0 ms -> 1179.8 ms (178.50 MiB, 1428 fills) -> 1478.4 ms (94.62 MiB, 757 fills)
      -> 283.9 ms -> 240.4 ms -> 207.6 ms -> then flat at 31-34 ms with fills=0
```

**Up to 1.48 s of game thread per 5 s window during the fill phase**, which is the known
stutter owner (`voxelsim-freeze-is-raster-atlas-fill`), and it is an order of magnitude more
game-thread time than the whole tile-loading budget.

Steady state, parked, after the fill: `seg=SETTLED-PARKED scope=total n=4771 hitches=2
stutters=3 p50Ms=8.50 (118 fps) p95Ms=9.10 (110 fps) p99Ms=9.60 (104 fps) maxMs=1994.92`.

### Honest reading

- The warm-cache attribution from F1 **survives** this leg: the multi-second stalls at world
  start are render-thread waits and raster-atlas fills, not tile decodes.
- Tile decode is 261-326 ms x 4 = 1.14 s, unchanged between warm and "cold".
- The plan's case for the async loader **as a cold-HDD fix is unmeasured**, because the standby
  cache was full when the leg ran. The case for it as a *hitch* fix is weak on this evidence:
  `fineMs` is ~0.02 ms on every hitch frame in the run.
- The one live tile-side anomaly is the ~4.8 s unattributed cost on `(-5,-3)`, reproducible
  across two sessions. That should be instrumented (split the streamer's read from its parse
  and log both) before any async work is justified on it.
- A genuinely cold measurement needs the standby list evicted: a fresh reboot with the leg as
  the literal first process to touch `D:\voxelsim\tile-cache`, or an explicit standby-list flush.

## Phase 4: a loading screen that does not freeze (2026-09-07)

Owner: *"I want the main menu and loading screen to be buttery smooth and suffer no hitches.
Happy to have player sit on loading screen for more than a minute if that time is needed to
load the tiles and game world in. However, the loading screen should not feel chunky or
hitching."* Approved for execution the same afternoon, with video explicitly out of scope
(*"No need for video"*) - so the loading screen stays the existing Slate hourglass.

### The finding that shapes everything below

**UE 5.8 has no way to keep a loading screen on a background thread while the game thread
goes on ticking the world.** Four independent engine facts, each sufficient on its own:

| # | Engine anchor | What it says |
|---|---|---|
| 1 | `MoviePlayer.cpp:123` `IsMoviePlayerEnabled()` | requires `!GIsEditor`. In PIE `GetMoviePlayer()` (`MoviePlayer.cpp:107`) returns `FNullGameMoviePlayer`, every method an empty body. The whole mechanism is absent in PIE. |
| 2 | `LaunchEngineLoop.cpp:5616-5617` | every engine frame *opens* with `FMoviePlayerProxy::BlockingForceFinished()` and then `ensure(... && !GetMoviePlayer()->IsMovieCurrentlyPlaying())`. A loading screen may not span engine frames. |
| 3 | `DefaultGameMoviePlayer.cpp:452-458` vs `:527` | `WaitForMovieToFinish` **destroys the Slate loading thread as its first act**, then runs its own loop where `bAllowEngineTick` calls `GEngine->Tick`. So `bAllowEngineTick` and the loading thread are mutually exclusive: in that mode the world tick and the Slate tick are back on one thread - today's topology, today's freeze. |
| 4 | `MoviePlayerThreading.cpp:195-199` | `ensure(...)` "Only one system can use the SlateThread at the same time. GetMoviePlayer is not compatible with PreLoadScreen." `FPreLoadScreenManager` claims the same `GSlateLoadingThreadId` (`PreLoadSlateThreading.cpp:26`) and is destroyed at `LaunchEngineLoop.cpp:5896`, before the first engine tick - so PreLoadScreen is a startup mechanism and cannot serve a NEW GAME inside a running world. |

There is no `LoadMap` here either: this project has zero `.umap` files and the world exists only
in PIE / `-game`, so the ordinary "loading screen around a map load" flow was never available.

### What is supported, and what shipped

The engine's own answer to *"the game thread is about to block; keep the loading screen alive"*
is the **MoviePlayer proxy, play-on-blocking** path, and it is used here in **widget-only mode**
(no movie streamer registered, `MoviePaths` empty - nothing plays video):

```
GetMoviePlayer()->SetIsPlayOnBlockingEnabled(true)     MoviePlayer.h:288 / DefaultGameMoviePlayer.cpp:1076
FMoviePlayerProxy::BlockingStarted()                   MoviePlayerProxy.h:21 -> :648 -> PlayMovie()
   -> FSlateLoadingSynchronizationMechanism            PlayMovie, DefaultGameMoviePlayer.cpp:406
      spawns "SlateLoadingThread", ticks + paints the widget at 60 Hz
      (MoviePlayerThreading.cpp:126-176) and hands each draw pass to the render
      thread, which presents it from FDefaultGameMoviePlayer::Tick (:520).
FMoviePlayerProxy::BlockingFinished()                  -> :683
```

`ue-project/Source/VoxelEarthUI/VoxelLoadingCurtainThread.h/.cpp` arms that block **around the
world tick, one game-thread frame at a time**, using
`FWorldDelegates::OnWorldPreActorTick` (`LevelTick.cpp:1675`) and `OnWorldPostActorTick`
(`:1906`). Those two brackets contain `FTickableGameObject::TickObjects` (`:1821`), which is
where `UVoxelWorldSubsystem::Tick` runs - and therefore where the tile decodes, the ring
recompute and the raster-atlas fills (`VoxelWorldSubsystem.cpp:11504`) all live. Nothing
Slate-related happens on the game thread between them, so the loading thread has Slate to itself
for the whole span.

**Why per-frame arming and not "leave it on".** `PlayMovie` creates a thread and swaps the main
window's content; `BlockingFinished` routes to `WaitForMovieToFinish`, which joins the thread and
calls `FlushRenderingCommands` twice. Paying that on all 3,600 frames of a smooth minute would
serialise the game and render threads for nothing. So the block is armed
(a) unconditionally for the first 6 frames - `StartWorldAndPawn`'s first tick builds the desired
set for a whole cascade and is known to be long *before* it runs - and
(b) afterwards by hysteresis: one frame over 40 ms arms the next 30 frames. The chunky phases are
runs, not singletons (the FILL window is seconds of multi-second frames), so one long frame
predicts the next; against a 6.3 s frame the arm cost is a rounding error, and during a smooth
run the arm never happens at all.

**One widget, moved - never two.** The single `SVoxelLoadingScreen` instance is removed from the
viewport for the duration of each block and put back after. A second instance would shuffle its
own background and tip orders and the picture would visibly change at every arm; and one instance
in two widget trees would have its `PersistentState` written by two threads.

**The arm proves itself or turns itself off.** After `BlockingStarted` the code asks
`IsMovieCurrentlyPlaying()`, which is literally `SyncMechanism != NULL`
(`DefaultGameMoviePlayer.cpp:638`) - "the loading thread exists and is running". If it is false
the attempt is counted as a refusal, the widget goes straight back, and after three consecutive
refusals the mechanism disables itself for the load with a Warning naming the count. In PIE that
is exactly what happens, by design, and the log says so.

Control arm: `-VoxelLoadingScreenThread=0` keeps the curtain a plain game-thread viewport widget,
byte-identical to before this change.

### Wait for the world, do not time out into it

`FVoxelReadyProbeConfig::MaxWaitSeconds` 60 -> 300, and the probe's ceiling is now its own switch,
`-VoxelLoadGateMaxWait=<s>`. Until now that ceiling *was* `-VoxelLoadMaxHold` - one number doing
two jobs. The owner's directive separates them: how patient the readiness **gate** is, and how
long the **curtain** holds, are different questions, and 60 s was short enough that a cold
8-ring cascade took the timeout path - i.e. the curtain lifted on a world that was still
landing, which is the one thing the gate exists to prevent. 300 rather than "no ceiling":
a gate with no ceiling is a hang, and the timeout arm logs a greppable Warning precisely so a
run that took it can be told from one that passed.

**Caption rotation over minutes.** The quip changes once per turn of the hourglass (~7.8 s,
26 lines => ~203 s per cycle); the tip is on an 8 s timer with 15 lines => **120 s per cycle**, so
at the new 300 s ceiling a player sees the tip list two and a half times *in the same order*.
Both cursors now reshuffle their order when they wrap, from a stream seeded once per show
(`MakeVoxelUIRandomStream`, so it stays fixed under `-unattended` and capture strips remain
diffable), and the reshuffle refuses to put the line that is currently on screen first.
Backgrounds rotate every 20 s and wrap correctly (`FVoxelUIAssetLibrary::RequestBackground` mods
by the pool size); their order is *not* reshuffled, because changing the image order mid-show is
visible in a way changing the text order is not.

### PSO precaching and the shader pipeline cache

Two mechanisms, only one of which can reach this project as it is run today. Config landed in
`Config/DefaultEngine.ini` (`[SystemSettings]`) and `Config/DefaultGame.ini`; workflow in
`tools/voxel-pso-cache-record.ps1` (**not run** - the box was owned by another agent).

1. **PSO precaching** (`r.PSOPrecache.*`). Runtime, works uncooked out of the editor binary,
   which is how every leg and every launch of this project runs. `r.PSOPrecaching` already
   defaults to 1 (`PipelineStateCache.cpp:211`) and `r.PSOPrecache.Components` to 1
   (`PSOPrecache.cpp:18`); the one that was off and matters is `Resources`, and it is
   `ECVF_ReadOnly` - settable only from an ini before startup, never by `-ExecCmds`. Now set,
   with `ProxyCreationStrategy=1` and `GlobalShaders=1`.

2. **The bundled shader pipeline cache** (`r.ShaderPipelineCache.*`, `*.upipelinecache`).
   **Cooked only, and unreachable from the editor binary**, for two independent reasons:
   `r.ShaderPipelineCache.Enabled` defaults to `PIPELINE_CACHE_DEFAULT_ENABLED = (!WITH_EDITOR)`
   (`PipelineFileCache.h:19`), and `FShaderPipelineCache::Initialize` - the only caller of
   `FPipelineFileCacheManager::Initialize` (`ShaderPipelineCache.cpp:772`) - is compiled behind
   `#if !UE_EDITOR` *and* requires `FPlatformProperties::RequiresCookedData()`
   (`LaunchEngineLoop.cpp:3196-3205`). A recording leg out of `UnrealEditor-Cmd.exe` logs nothing
   and produces an empty cache that is indistinguishable from a working one, so
   `voxel-pso-cache-record.ps1 -Mode Bundle` **refuses** without `-PackagedDir` instead.

   The recording workflow, for the day a cooked client exists:
   - run the client with `-logPSO` and `r.ShaderPipelineCache.LogPSO 1` -> `*.rec.upipelinecache`
   - `UnrealEditor-Cmd <proj> -run=ShaderPipelineCacheTools Expand <rec...> <*.shk> <out.stablepc.csv>`
   - `... Build <in.stablepc.csv> <*.shk> <VoxelEarth_PCD3D_SM6.upipelinecache>`
     (`ShaderPipelineCacheToolsCommandlet.cpp:2899, :2904`; the name must be
     `<LastOpened>_<ShaderPlatform>` or nobody loads it and nothing reports an error)
   - copy to `ue-project/Content/PipelineCaches/Windows/` - **not** `Build/<Platform>/PipelineCaches`,
     which is the UE4 location; `CopyBuildToStagingDirectory.Automation.cs:2058` stages the former.
   - `bShareMaterialShaderCode=True` is a hard prerequisite (`ShaderPipelineCache.h:76`); it is now
     set in `DefaultGame.ini`.

   The reachable arm today is `voxel-pso-cache-record.ps1 -Mode Precache`, which flies a leg
   through `tools/voxel-run-flight-leg.ps1` with the precache reporting armed.

**And one thing none of this fixes, stated so it is not claimed:** the ~50 s `renderWaitMs` on
frame 1 of a cold launch with *"Preparing Shaders"* on screen is the **editor shader compiler**
filling the DDC (`GShaderCompilingManager`), not PSO creation. No pipeline cache touches it. The
answers to that one are a warm DDC or a cooked build.

### The instrument: `seg=LOADING`

`VoxelFramePhase` gains a fifth segment beside `seg=MENU`, fed by
`VoxelFramePhase::NoteLoadingFrame` from the front end's loading-state tick.

**It measures the curtain's PAINT INTERVAL, not the game frame, and that is the whole point.**
The owner has said the game thread may take as long as it needs; what must stay smooth is how
often the hourglass is redrawn. `SVoxelLoadingScreen::Tick` calls
`VoxelLoadingCurtain::NotePaint()`, and `SWidget::Paint` invokes widget `Tick` for any widget
with `bCanTick` - so it fires on the game thread on ordinary frames and **on the Slate loading
thread inside an armed block**, with no separate hook for either. Intervals are queued under a
lock and drained on the game thread, because `VoxelFramePhase`'s buckets and its 5 s flush are
game-thread state and must stay that way.

**Phase 4 gate.** On an unattended run with `-VoxelFramePhase=1`, `seg=LOADING` p99 stays under
33 ms while the same log's `seg=FILL` still shows multi-second frames. The two must diverge, or
the thread move did nothing. With `-VoxelLoadingScreenThread=0` the same instrument reads
seconds - a confirmation that cannot come out the other way is not one.

**Reading rules.**
- `seg=LOADING n=0` -> `NoteLoadingFrame` is not being called; the leg says nothing.
- `paintOverflow>0` on the `LoadScreen: curtain thread ...` line -> intervals were dropped between
  two drains, from precisely the worst window. Do not quote a p99 over it.
- `blocks=0` with a long load -> the block never armed. Read `available=` on the
  `LoadScreen: curtain thread requested=... available=...` line before blaming the mechanism;
  `available=PIE (...)` is the expected answer in the editor.
- `longestUncoveredFrameMs` large with `blocks` large -> the hysteresis missed the *first* frame
  of a run, which it does by construction: a frame's length is not known until it is over.

### The command for the gate

One unattended leg that goes through NEW GAME and sits on the loading screen:

```
tools\voxel-ui-capture.ps1 -Shot GateSweep -GateRing 3 -MaxHold 300 -TimeoutSec 900 `
  -ExtraArgs '-VoxelFramePhase=1' '-VoxelLoadingScreenThread=1' '-VoxelLoadGateMaxWait=300' `
             '-VoxelLoadTheatre=0' '-VoxelSpawnAt=-56940,-56610'
```

`-Shot GateSweep` is the arm that already passes `-VoxelMenuAutoStart=1` and
`-VoxelReadyProbeLog` (`tools/voxel-ui-capture.ps1:231`), so the run presses NEW GAME on its own
and prints one line per readiness poll. Then the control arm, identical but
`-VoxelLoadingScreenThread=0`. Read from each log: the `Voxel frame dist seg=LOADING scope=total`
row, the `seg=FILL` row from the same log, and the `LoadScreen: curtain thread ...` counters.
