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

## Phase 2: menu A/B (seg=MENU instrument + tick gates)

`tools\voxel-ui-capture.ps1 -Shot Menu -SettleSec 6 -ExtraArgs '-VoxelFramePhase=1','-VoxelMenuTickGates=N'`.
Arm proven by the read-back line `VoxelFrontEnd: menu tick gates=N`, which read 0,
then 1, then 0 again.

| arm | n | meanMs | p50Ms | p95Ms | p99Ms |
|---|---|---|---|---|---|
| gates=0 (A) | 1566 | 3.19 | 3.00 | 3.20 | 3.40 |
| gates=1 (B) | 1570 | 3.12 | 2.90 | 3.20 | 3.40 |
| gates=0 (A2) | 1561 | 3.20 | 3.00 | 3.20 | 3.40 |

**Instrument gate PASS**: `seg=MENU scope=window n>0` in every log (1566/1570/1561).
Before HOOK 0 this segment did not exist, so `n=0` was the failure it had to avoid.

**A/A noise floor**: 0.01 ms on mean, zero bins on p50. The arm moves mean by
0.07 ms (7x the floor) and p50 by one full 0.10 ms bin while A/A stays on the same
bin, so the reduction is real: about 2-3% of menu CPU frame time. p95 and p99 are
identical in all three arms -- the gates remove steady-state per-frame work, not
spikes.

**Image gate PASS**: `VoxelMenu00019/20/21` are byte-identical,
md5 `4ad4ec1ba2a7cc8edc45b9f5721ac82c`.

**What this does NOT show.** 3.2 ms is ~310 fps. The seg=MENU CPU number was never
what the owner felt on the title screen; the reported ~11 ms GPU and busy render
workers are untouched and this instrument cannot see them. That remains unmeasured.

**CORRECTION (do not repeat the earlier claim).** An earlier draft of this section,
and the body of commit `d2965c2`, blamed Lumen for the menu GPU cost. **That is
wrong.** Lumen is not armed by this project: the `DefaultEngine.ini:717-730` lines
the plan's F2 cited do not exist in the project file (they are engine
BaseScalability lines selected by this box's scalability level), and they are inert
because `r.DynamicGlobalIlluminationMethod` defaults to None and nothing sets it --
the project's own config says so at `DefaultEngine.ini:647`. Fog, sky and shadows
are also absent on the menu because the sky rig is deferred while the world is
held. What actually draws on the menu is the **ocean actor and the clipmap**. The
commit body is left as written rather than rewriting history; this is the
correction of record.

### UI scale (same commit)

`DefaultEngine.ini` gained `[/Script/Engine.UserInterfaceSettings]`
`UIScaleRule=ShortestSide` with an integer-stepped curve, because the engine
default interpolated to exactly 1.333 at a 1440 shortest side and a fractional
scale blurs the 1 px borders of a pixel-art UI. Measured in-game shell width in a
2560-wide frame: **1348 px (52.7%) before, 1053 px (41.1%) after** -- the intended
1.0. Every UI capture taken before commit `d2965c2` is the "before" set.

## Phase 3.5: tile-crossing legs

### The rig, and the trap that voided the first attempt

**The 8 km cascade reach, not the pawn position, is what bounds a flight leg
inside a baked set.** The first leg A was sized against a 4 km admission reach and
died at a FATAL gate leak on tile (-2,-5): the pawn never left the baked -3 column,
but the streaming footprint runs 8192 m ahead of it and crossed into the unbaked
-2 column. Arithmetic that must be used instead: fine tile = 15360 m,
`tile = floor(metres/15360)`; the baked -3 column spans [-46080, -30720), so the
pawn must satisfy `pawn_x + 8192 < -30720`, i.e. `pawn_x < -38912`. From the spawn
at -61440 that caps travel at 22528 m; `-RunSec 100` at 240 m/s asked for 24000 m
and overran by ~1.5 km. `-RunSec 85` (20400 m, pawn to -41040, reach to -32848,
~2.1 km margin) is the corrected size. **This trap has now cost a leg twice in
this project.**

Baked coverage is also much sparser than the plan assumed: 24 tiles total, and the
block around the spawn is a bare 3x3 (x -5..-3, y -5..-3) with (-3,-3) missing.

Final rig, all four arms identical apart from the two switches:
`-Flight line -SpawnAt '-61440,-61440' -RunSec 85 -PreflightSec 90 -LingerSec 30
-LogIntervalSec 2 -VoxelPerfSpeed=240 -VoxelPerfHeading=0 -VoxelFineLockMeter=1`.

### Containment and correctness (all four legs)

| leg | resident | absentOnDisk | blockingLoads | gateLeaks | ringCentre | ringMoves | mirrorOffThread | Fatal |
|---|---|---|---|---|---|---|---|---|
| A sync r0 | 6 | 0 | 2 | 0 | (-3,-4) | 1 | 0 | 0 |
| B async r1 | 8 | 5 | 3 | 0 | (-3,-4) | 1 | 0 | 0 |
| C async r0 | 6 | 0 | 5 | 0 | (-3,-4) | 1 | 0 | 0 |
| A2 sync r0 | 6 | 0 | 2 | 0 | (-3,-4) | 1 | 0 | 0 |

Every leg crossed the -4/-3 boundary (`ringMoves=1`, `ringCentre=(-3,-4)`) and
every leg stayed inside coverage. **Leg B's `absentOnDisk=5` with `gateLeaks=0` is
the important correctness result**: at ring radius 1 the 3x3 ring includes the
unbaked -2 column, and an absent file is recorded as known-missing rather than
answered at sea level. If an absent ring tile had produced a gate leak that would
have been a real loader defect; it did not.

### Two gate definitions were wrong and were replaced

Recorded rather than quietly passed:

* **`asyncLaunched>=8` on leg B was wrong.** Actual is 6, and 6 is correct: the
  ring is 9 tiles, 8 are baked, and 2 are already resident from the synchronous
  startup path before the residency tick first runs. Replacement gate, which can
  still fail: `asyncPublished == asyncLaunched` AND `asyncFailed=0` AND
  `asyncDropped=0` AND `gateLeaks=0` AND `mirrorOffThread=0` AND the ring fully
  resident. Leg B: 6/6, 0, 0, 0, 0, resident=8. **PASS.**
* **"leg A must show a `residency tick stalled` line" was wrong.** Zero legs show
  one, because at ring radius 0 the synchronous load is taken by the FUNNEL
  (`blockingLoads`), not by the residency tick, so that line cannot fire in either
  arm. As written the control was VOID and the whole comparison with it, which is
  not the truth. Replacement proof that the leg exercised the disease:
  `ringMoves>=1` AND `blockingLoads>=1` AND a multi-second `maxMs` -- satisfied by
  all four. The absent stall line is evidence of nothing.

### Timing, and the noise floor that decides it

`seg=SETTLED-MOVING scope=total`. Mean speeds 245.1-247.3 m/s, so the arms are
comparable.

| leg | n | p50Ms | p95Ms | p99Ms | maxMs | meanMs | hitches | stutterPct |
|---|---|---|---|---|---|---|---|---|
| A sync r0 | 1670 | 44.00 | 112.00 | 235.00 | 1668.09 | 52.69 | 1100 | 85.03 |
| B async r1 | 1785 | 41.00 | 103.00 | 229.00 | **726.46** | 48.59 | 1088 | 84.82 |
| C async r0 | 1527 | 49.00 | 123.00 | 247.00 | 1130.33 | 57.45 | 1080 | 87.56 |
| A2 sync r0 | 1293 | 57.00 | 147.00 | 1251.99 | 1251.99 | 68.26 | 996 | 89.33 |

**Noise floor = |A - A2|, the two identical arms:**

| metric | p50 | p95 | p99 | max | mean |
|---|---|---|---|---|---|
| floor | **13.00** | **35.00** | **1016.99** | **416.10** | **15.57** |

Against that floor:

| claim | delta | floor | verdict |
|---|---|---|---|
| B p50 vs A | -3.00 | 13.00 | does NOT clear |
| B p95 vs A | -9.00 | 35.00 | does NOT clear |
| B p99 vs A | -6.00 | 1016.99 | does NOT clear |
| B mean vs A | -4.10 | 15.57 | does NOT clear |
| **B max vs A** | **-941.63** | **416.10** | **CLEARS** |
| C p50 vs A | +5.00 | 13.00 | does NOT clear |
| C p95 vs A | +11.00 | 35.00 | does NOT clear |
| C p99 vs A | +12.00 | 1016.99 | does NOT clear |

**The only claim that survives is B's `maxMs`.** B's worst frame is 726.46 ms
against 1668.09 (A) and 1251.99 (A2) -- below *both* controls, by 941.63 and
525.53 ms, both larger than the 416.10 ms floor. That is exactly the mechanism the
arm was built for: a multi-second blocking whole-tile load taken off the game
thread.

**C's apparent 10% regression does NOT clear the floor** and must not be reported
as one. And because it does not, neither does B's p50/p95 improvement: the two are
the same size. The honest summary is that **this rig cannot resolve percentile
differences at all** -- the A/A floor swamps them -- and the single surviving
result is the removal of the worst frame.

Worth noting even though it is n=1 per leg: both async arms have a lower `maxMs`
than both sync arms (726 and 1130 against 1252 and 1668), which is consistent with
the mechanism but is not a claim this leg set can make.

### Ring fill, joins, and whether prefetch stays ahead

| leg | max `queued` | ring-fill wall (first to last ASYNC publish) | joins pre-preflight | joins post-preflight | capRefusals | asyncWorkerMs | asyncPublishMs |
|---|---|---|---|---|---|---|---|
| A sync r0 | n/a | n/a | 0 | 0 | 0 | 0.0 | 0.00 |
| B async r1 | 5705 ms | 14 s (20:21:25 -> 20:21:39) | 1 | **0** | 17 | 4526.3 | 0.24 (6 publishes) |
| C async r0 | 83 ms | ~0 s (both publishes in one second) | 1 | **0** | 2 | 1639.7 | 0.10 (2 publishes) |
| A2 sync r0 | n/a | n/a | 0 | 0 | 0 | 0.0 | 0.00 |

`queued` climbs 0, 21, 70, 1103, 1599, 5705 ms as the ring fills: nine ring tiles
sharing `-VoxelFineTileAsyncCap=2` at roughly 600-960 ms of worker time each.

**Both joins happen during world init, before the preflight clock starts; there
are ZERO post-preflight joins in either async arm.** By the standing decision rule
the cap is adequate and **ships at 2**; no cap sweep is warranted. A
`-VoxelFineTileAsyncCap 4` arm was therefore not run.

**Does prefetch stay ahead of the flight?** Ring radius 1 populates in 14 s of wall
time. At 240 m/s a tile is 15360 m = 64 s of flight. So the ring fills roughly 4.5x
faster than the player crosses a tile, and prefetch stays ahead with margin. The
same 14 s is the floor on the Phase 3.4 loading-gate wait, since the `fineRing`
condition cannot release faster than the ring populates. The gate is live: an
in-game screen capture on this build logged
`VoxelLoadGate: ... fineRing gate ON ... READY after 9.02s (hits 112/112, ... fineRing=1/1)`.

4526.3 ms of worker wall time was moved off the game thread on leg B, against
0.24 ms of total game-thread publish cost for six publishes.

### Image row: VOID, and why

`python tools/capture-pixdiff.py` on the 18 km distance-triggered shots, changed
pixels at threshold 64:

| pair | changed @64 | region |
|---|---|---|
| **A vs A2 (the control floor)** | **95.47%** | whole frame |
| B vs A | 0.10% | one 565x265 box at top-left (the debug text overlay) |
| C vs A | 2.02% | 84% of frame |

**The control floor exceeds every arm comparison, so the image row is VOID and no
image claim may be made in either direction.** Two identical legs produced almost
completely different 18 km frames: the distance trigger catches whichever frame
happens to be current, and at 246 m/s the pose and streaming state differ enough to
change nearly every pixel. This is the known moving-capture floor.

The one thing worth noting, without claiming it: B vs A differs only inside the
overlay text box and not on terrain at all, which is what "the async arm did not
change the image" would look like -- but a single pair cannot establish that
against a 95% floor.

### A fifth arm was considered and skipped

A sync + ring-1 arm would cleanly isolate prefetch from async. It was not run: it
is exactly the configuration `VoxelFineTileStreamer.h` explains is unaffordable
(nine synchronous whole-tile decodes on the game thread), so its result is
predictable and the box time is better spent elsewhere.

### Context these numbers must carry

Both arms are GOAL3-FAIL at p50 ~24 fps, and that is **expected, not a
regression**. This rig flies at 246 m/s, over 12x the 20 m/s the standing targets
are written against, deliberately, to force tile crossings inside a short leg.
These numbers may not be quoted as representative play, and per the house rule a
capture-leg `frameMs` is not an FPS claim.

Box sharing: a Codex `python` process was live throughout. CPU deltas across the
legs were 0.5 s (A), 13.6 s (B), 14.6 s (C) against ~245 s of wall each -- light
background work, no compile and no second editor, so no leg is marked
VOID-CONTENDED.

### Verdict

The async loader is **correct** (every counter clean on every arm, an absent ring
tile handled as known-missing rather than a gate leak, `mirrorOffThread=0`
throughout) and it **removes the worst frame** (`maxMs` -56% against a floor it
clears). It is **not** demonstrated to improve typical frame time: no percentile
delta in either direction clears the A/A noise floor of this rig. It ships behind
`-VoxelFineTileAsync` defaulting to 0.

## Phase 2b: menu scalability A/B

Three arms, `-Shot Menu -SettleSec 30 -VoxelFramePhase=1 -VoxelMenuScalability=N`.
SettleSec 30 rather than 2 because `seg=MENU` needs at least one five-second flush.

**Engagement, quoted in full, and every gate can fail:**

```
arm 0: MenuScalability: OFF (-VoxelMenuScalability=0). The menu renders at the
       shipped configuration; this is the control arm.
arm 1: MenuScalability: APPLIED changed=3 unchanged=0 missing=0 of 3 candidates
       | r.ScreenPercentage 65 -> 25 | r.TSR.History.ScreenPercentage 200 -> 100
       | r.BloomQuality 4 -> 0
arm 1: MenuScalability: RESTORED restored=3 mismatched=0
       | r.ScreenPercentage 25 -> 65 (released) | ... (released) | ... (released)
arm 2: MenuScalability: DRY RUN (-VoxelMenuScalability=2), nothing was set.
       wouldChange=3 unchanged=0 missing=0 of 3 candidates
       | r.ScreenPercentage 65 -> 25 | r.TSR.History.ScreenPercentage 200 -> 100
       | r.BloomQuality 4 -> 0
```

Never `APPLIED NOTHING`; `mismatched=0` on restore; and the dry run's
`wouldChange` list is **character-identical** to the arm's `changed` list.

| arm | n | meanMs | p50Ms | p95Ms | p99Ms |
|---|---|---|---|---|---|
| 0 control | 10175 | 2.95 | 2.90 | 3.30 | 3.50 |
| **1 applied** | **13166** | **1.90** | **1.80** | 3.00 | 3.70 |
| 2 dry run | 10080 | 2.98 | 3.00 | 3.40 | 3.80 |

**Noise floor is the dry run**, which is the better control here than a second
OFF arm: it walks the same code, computes the same candidate list, and sets
nothing. Control vs dry run is 0.03 ms on mean and 0.10 ms on p50. The applied
arm moves mean by **-1.05 ms (-36%)** and p50 by **-1.10 ms (-38%)**, i.e. 35x
and 11x the floor. Frame count over the same 30 s rises 10175 -> 13166 (+29%).

**Image gate PASS, and this is the one that had to be checked.** All three menu
PNGs are byte-identical: `VoxelMenu00026/00027/00028`, md5
`904e23666ea2275f0f91e667cdc520eb`. Dropping `r.ScreenPercentage` from 65 to 25
does not touch the menu image because the title screen is Slate over a UI
texture, not a 3D scene render -- so the scene the screen percentage governs has
nothing in it to shrink. That is exactly why this is affordable.

**A VOID attempt is on record.** The first run of this A/B produced no
`MenuScalability:` line in any of the three logs, because
`VoxelMenuScalability.h/.cpp` (15:54/15:57) postdated the binary under test
(15:21). All three arms still produced plausible seg=MENU numbers (2.95 / 2.92 /
2.93) and would have read as a clean null. **The engagement gate is the only
reason that was caught**, and the numbers were discarded rather than reported.

## Phase 4: threaded loading curtain

`tools\voxel-ui-capture.ps1 -Shot GateSweep -GateRing 3 -MaxHold 300
-TimeoutSec 900 -VoxelFramePhase=1 -VoxelLoadGateMaxWait=300 -VoxelLoadTheatre=0
-VoxelSpawnAt=-56940,-56610`, arm `-VoxelLoadingScreenThread=1`, control `=0`.

`seg=LOADING` measures the **curtain's paint interval**, not the game frame. That
is deliberate: the owner said the game thread may take as long as it needs, so
the question is whether the screen keeps painting while it does.

**Engagement:**

```
arm:     curtain thread requested=1 available=yes
         curtain thread ARMED (armMs=40 coolDownFrames=30 primeFrames=6)
         curtain thread blocks=70 refusals=0 coveredSec=12.65
           longestBlockMs=7597.4 longestUncoveredFrameMs=39.2
           paints=1478 paintOverflow=0
control: curtain thread requested=0 available=switched off (-VoxelLoadingScreenThread=0)
```

`refusals=0`, so this is not the `GIsEditor` VOID case.

| arm | seg=LOADING p99 | seg=LOADING max | seg=LOADING mean | seg=FILL max |
|---|---|---|---|---|
| thread=1 | **23.90 ms** | **203.75 ms** | 13.02 | 7986.67 ms |
| thread=0 | 219.00 ms | **7633.41 ms** | 25.22 | 8305.02 ms |

**PASS on both halves.** The arm's curtain paints at 23.90 ms p99 (42 fps) while
`seg=FILL` in the *same log* still carries a 7986.67 ms game frame -- the world
tick was not made faster, it was made invisible. The control's curtain reads
seconds: a **7633.41 ms** worst paint gap, which is the freeze the owner reported.

The curtain covered 12.65 s across 70 blocks, the longest single block being
7597.4 ms, and the longest frame it failed to cover was **39.2 ms**.
`paintOverflow=0`.

Note the control's `seg=LOADING` p50 is *lower* (5.90 vs 16.30 ms): with no
curtain thread the widget only paints on the frames the game thread hands it, so
its median interval is short and its tail is catastrophic. The p99 and max are
the numbers that describe what a player sees; the p50 is an artefact of only
sampling when unblocked.

The PSO recording script (`tools/voxel-pso-cache-record.ps1`) was **not run**:
the bundled cache is unreachable from an editor binary and the script correctly
refuses without a packaged build.

The gate leg above **passed**, and the feature then **hung the owner's live session**
on the same build. The leg never lost window focus; the owner did. What follows is the
diagnosis and what changed as a result.

### The threaded curtain HUNG a live session, and is now default OFF (2026-09-07 evening)

Everything above shipped at 19:20 with `-VoxelLoadingScreenThread` defaulting to **1**. On the
owner's first interactive load it **hung the game**: process alive and `Responding=True`, CPU
flat, 61 threads, 7.9 GB, loading screen frozen on screen, unclickable, log dead mid-load,
killed after 14 minutes. Evidence:
`docs/evidence/2026-09-07-live-curtain-hang-VoxelEarth.log`.

**The default is now 0.** `=1` is an opt-in for a controlled leg with somebody watching. A
loading screen that can hang is worse than one that stutters.

#### The timing is the evidence

| time (UTC) | line |
|---|---|
| 23:27:20.181 | `LoadScreen: curtain thread ARMED (armMs=40 coolDownFrames=30 primeFrames=6)`; theatre rolls 41.4 s |
| 23:27:20.901 | `VoxelLoadGate: started ... max wait 300s` |
| 23:29:20.792 | `VoxelLoadGate: READY after 16.02s` — **119.9 s of wall clock later** |
| 23:29:28.826 | `Hitch frame: frameMs=54.63` — the **first** frame in the whole load over the 40 ms arm threshold |
| 23:29:28.874 | `Hitch frame: frameMs=67.22` — the second |
| 23:29:29.063 | last line in the file. Nothing after it, ever. |

The curtain armed its six prime frames at `:20.18` and then **did not arm again for two
minutes** — every frame was under 40 ms. The hang is on the **first arm/disarm cycle after two
minutes of not arming**, five frames after the threshold was first crossed. There is no other
candidate in the log: no ensure, no Fatal, no `refused`, no `blocks=` summary.

#### Defect 1 — an unbounded engine wait, entered from inside the world tick

```
DisarmBlock()
  -> FMoviePlayerProxy::BlockingFinished()          MoviePlayerProxy.cpp:32
  -> FDefaultGameMoviePlayer::BlockingFinished()    DefaultGameMoviePlayer.cpp:683
  -> WaitForMovieToFinish()                         :445
  -> SyncMechanism->DestroySlateThread()            :452
  -> while (bMainLoopRunning) { PumpMessages(false); Sleep(0.001f); }
                                                    MoviePlayerThreading.cpp:75-81
```

That loop has **no timeout and no cancel**, and `PumpMessages` is exactly why Windows kept
reporting the process as Responding while it sat there. It exits only when the Slate thread sets
`bMainLoopRunning = false` (`MoviePlayerThreading.cpp:179`), which it cannot do until
`IsSlateDrawPassEnqueued()` goes false (`:174-177`), which happens in **exactly one place**:
`FDefaultGameMoviePlayer::Tick`, **on the render thread** (`DefaultGameMoviePlayer.cpp:520-537`).

And that render-thread tick is not guaranteed:

- in a threaded build the game thread never calls `TickRenderingTickables` —
  `LaunchEngineLoop.cpp:5610` guards it with `if (!GUseThreadedRendering)`;
- the sole driver is `FRenderingThreadTickHeartbeat` (`RenderingThread.cpp:466-486`), which
  **skips the tick entirely while `GSuspendRenderingTickables != 0`, i.e. during any
  `FlushRenderingCommands`** (`:433-440`) — and `WaitForMovieToFinish` itself calls
  `FlushRenderingCommands` twice;
- even when it runs, `TickRenderingTickables` returns early **without ticking anything** if less
  than `1/GRenderingThreadMaxIdleTickFrequency` (25 ms) has passed, off a shared function-local
  `static LastTickTime` (`TickableObjectRenderThread.cpp:34-45`).

So the game thread's exit from a disarm depends on a render-thread heartbeat that the engine
itself suppresses during rendering flushes. **We do not own the release condition, so we cannot
bound the wait.** Every frame of that load was already render-bound — `gameWaitMs ≈ 31 ms` of a
38 ms frame, which is `GGameThreadWaitTime`, the render fence
(`VoxelWorldSubsystem.cpp:12066`), *not* the proxy wait — which is precisely the state in which
the heartbeat is most starved.

**The standing rule this cost: never call an engine wait with no timeout from inside the world
tick.** If the release condition belongs to another thread and the engine gives you no deadline,
you do not get to enter it.

#### Defect 2 — the loading screen was *also* never going to lift, and that is separate

`VoxelLoadGate: READY after 16.02s` was printed **119.9 wall seconds** after
`VoxelLoadGate: started`. The probe accumulates the same `DeltaSeconds` the front end's
`LoadElapsedSeconds` does, and the engine's tick delta is clamped — so under this load the
front end's clock ran at about **one seventh of wall time**. The reveal test was
`LoadElapsedSeconds >= max(TheatreDuration = 41.4 s, LoadMinHold)`, with ~16 s banked at the
two-minute mark: **the theatre had roughly five more real minutes to run before the curtain
would have lifted**, hang or no hang. That is why the log contains no
`LoadScreen: world ready ...` and no `closing the curtain` line.

Fixed: the theatre fraction and the reveal test now run on the **wall clock**
(`LoadWallSeconds`), because the rolled duration is a quantity of the player's life, not of the
engine's clamped delta. The reveal line prints both so the divergence stays visible:
`VoxelFrontEnd: closing the curtain after %.2fs wall (%.2fs ticked) (...)`.
`-VoxelLoadingShotAt` offsets stay on the tick clock, unchanged.

#### The five guards the incident bought

1. **`-VoxelLoadingScreenThread` defaults to 0.**
2. **`bEndRequested`** — a one-way latch set by `End()` *before* it does anything else. No block
   can arm after it, so the hand-off's widget move can no longer race an arm.
3. **`End()` runs at the reveal**, before the curtain fade starts, not at the end of teardown —
   `TeardownMenu` stays as the idempotent backstop. Previously the mechanism was live across the
   0.4 s fade, during which a long frame could arm a block on the widget `TickHandOff` was
   animating.
4. **No arm while the application does not have focus** (`FApp::HasFocus()`).
   `tools/voxel-editor.ps1` arms `t.IdleWhenNotForeground`, the owner was alt-tabbing to a
   terminal, and the gate-sweep leg that validated the feature **never lost focus** — that state
   was never exercised before it shipped. Counted as `focusRefusals=`.
5. **A hard cap of 240 blocks per load** (`capRefusals=`), a **slow-join tripwire** at 250 ms
   that disables the mechanism for the rest of the load, and **a log line on both sides of the
   join** so a future hang names itself instead of stopping silently.

#### The exact lines a future run prints

Entering the wait (Verbose, at most once per block):

```
LoadScreen: curtain thread joining the Slate loading thread (block N) -- if this log ends here, THAT is the hang (DestroySlateThread has no timeout).
```

The tripwire, if the join comes back slowly instead of never:

```
LoadScreen: curtain thread join took NNN ms (block N, limit 250). That is the unbounded DestroySlateThread wait going slowly -- disabling the threaded curtain for the rest of this load; it stays on the game thread.
```

And the summary, which now separates "never fired" from "never allowed to fire":

```
LoadScreen: curtain thread blocks=N refusals=N focusRefusals=N capRefusals=N coveredSec=X longestBlockMs=X longestDisarmMs=X longestUncoveredFrameMs=X paints=N paintOverflow=N
```

### The curtain never lifted in a live session, and the probe's timeout was the reason (2026-09-08)

Owner report, 05:38 UTC: "UE5 game editor appears to just be looping on the loading screen."
Window closed by the owner at 05:41:09. Log preserved at
`docs/evidence/2026-09-08-live-session3-loading-loop-VoxelEarth.log`
(md5 `6a7094682b8c8122758a7621a04229ee`). Launch: the 01:23 foam-arm build with
`-VoxelFineTileAsync=1 -VoxelFineTileRingRadius=1 -VoxelFineLockMeter=2`, on a box where
Codex had 17 editor commandlets running. Timing on this launch is VOID for perf purposes;
the control-flow defect below is not.

| wall time | event |
|---|---|
| 05:29:25 | NEW GAME; theatre 48.5 s rolled; curtain thread off |
| 05:32:10 | first world tick, 165 s later (shader compile); `VoxelLoadGate: started ... max wait 300s`; first fine tile resident via ASYNC after a **159 s** worker read |
| 05:32-05:35 | fine tiles land: worker reads 6.3 s, 36.9 s, 0.3 s, **46.7 s**, 0.3 s; one game-thread join (`asyncJoins=1`); 879 `VoxelGpuMesh ... ended TimedOut` requeues, one queued **188.9 s** before its 11.7 s readback timeout |
| 05:36:36 | FILL segment ends: 1820 frames, one of **154.4 s**; SETTLED-PARKED begins |
| 05:37:08-47 | a 38.6 s frame; then 101 ms frames (the unfocused-window idle, owner alt-tabbed) |
| 05:38:34-05:41:04 | 154 s wall with 919 frames; a 7.5 s frame; no READY, no TIMED OUT |
| 05:41:09 | owner closes the window |

**Why no READY:** gate 2 wants the streamer quiet for three consecutive 0.4 s polls, and
the GPU mesh path was requeueing timed-out readbacks continuously on a saturated GPU.
That is the box, not the gate.

**Why no TIMED OUT, which is the defect:** `FVoxelWorldReadyProbe::Tick` did
`Status.ElapsedSeconds += DeltaSeconds`, and the world clamps DeltaSeconds to 0.4 s. The
166 s, 154 s and 38 s frames each counted as 0.4 s. Summing the segments' frame time with
the clamps applied, the probe had seen roughly 180 of its 300 s when the owner gave up, 539
wall seconds after it started. On the wall clock it would have lifted the curtain at
05:37:10. This is the same defect the theatre and the reveal test had on 2026-09-07 (the
"READY after 16.02s printed 119.9 wall seconds later" entry above), one layer down.

**Fix (this commit):** `ElapsedSeconds` is now wall time from a `WallStartSeconds` taken
in `Start()`; the READY and TIMED OUT lines say "wall". The 0.4 s poll rate limit stays on
the tick clock because it bounds per-frame cost, not time. Standing rule: every timeout
that guards a curtain runs on the wall clock.

**Also visible in this log, for the loader verdict:** under HDD contention the async
worker reads ran 6-159 s per tile instead of ~0.3 s, and the one join blocked the game
thread for the remainder of one of them. The safety net worked as designed; the ring
prefetch cannot get ahead of a disc that is shared with 17 other processes.

## Quiet-box loading measurement, 2026-09-09 (the box handed over by the owner)

Two `-Shot Loading` legs at the lake column with `-VoxelFineTileAsync=1
-VoxelFineTileRingRadius=1 -VoxelFineLockMeter=2 -VoxelFramePhase=1`, no other editor or
build on the machine. Logs: `Saved/loading-leg1-quiet-box.log` (first launch, OS file cache
cold for these tiles) and `Saved/loading-leg2-warm-cache.log` (second launch, warm).

| | leg 1 (cold cache) | leg 2 (warm cache) |
|---|---|---|
| NEW GAME to gate READY | 27.3 s | 14.8 s |
| curtain (theatre 36 s) lifted at | 36.5 s | 39.0 s |
| `seg=LOADING` | n=1026, hitches=20, p99 101 ms, max **11,430 ms** | (same instrument) |
| lake-sheet first gather, per tile | **11,267 / 1,962 / 384 / 0.1 ms** | 379 / 360 / 425 / 0.1 ms |
| fine tiles | 6 async, 1 join (698 ms, the spawn tile) | same shape |

**The freeze the owner sees on the hourglass is the lake-sheet gather.** `AVoxelWaterSheetActor`
pops one fine tile per tick and calls `GatherLakeSheetBasinsInTile`, which reaches the lake
tier's PRIVATE `FineTileSampler` and loads the whole `.vxtl` synchronously on the game thread
(`FLakeWaterSampler::EnsureTile`). On leg 1 tile (-4,-3) was being read by the streamer's async
worker at the same moment, on the same SATA HDD, and the lake tier's read of it took 11.3 s;
the tiles the streamer had already finished cost ~0.4 s each (a second read of a 300-500 MB
file from the OS cache plus parse). Frames 44-50 spanned 17 s of wall clock. `renderWaitMs`
on those frames is the render thread idle (see voxelsim-counter-name-lies), not a render
stall.

Other game-thread items under the curtain, leg 1: ring-5 entry recompute 578 ms (known,
R7 bound); raster-atlas DEMAND fills 1,033 and 1,171 ms of game thread per 5 s window (the
theatre's 0.5 ms cap applies to the sweep, not to demand fills: `served=30951 fills=800`);
two frames of 5.7 s (ending 03:04:51) and 4.6 s (the reveal frame, 03:05:05) with nothing
logged inside them. UE's `stat dumphitches` printed nothing on leg 2 (needs the stats
system armed differently under `-unattended`); those two remain unattributed. A `-Shot
Loading -At` value later than the reveal never fires (TickLoading stops at HandOff), which is
why both legs ran to the tool's 600 s timeout.

**Fix landed (this commit):** the lake tier gains an async arm mirroring the streamer's:
`FLakeWaterSampler::RequestTileAsync` reads and fully decodes the tile into a private
sampler on a worker, `PumpAsync` (from `UVoxelWaterSubsystem::Tick`) adopts it with
`adoptWarmTile`, and `UVoxelWaterSubsystem::IsLakeTileReadyForGather` gates the sheet
actor's per-tile gather: it waits for the streamer's ring to settle (so the read is
cache-warm and never contends with the streamer) and then for the worker. The gather now
runs only against a tile the lake tier already holds; the log line is
`Lake tier: fine tile (x,y) loaded ASYNC: read N ms + full decode N ms OFF the game thread`.
Leg 3 below is the check.

**Loader default flip (this commit):** `tools/voxel-editor.ps1` passes
`-VoxelFineTileAsync=1 -VoxelFineTileRingRadius=1` on interactive launches. Headless legs
keep the constant default (0) so their numbers stay comparable.
