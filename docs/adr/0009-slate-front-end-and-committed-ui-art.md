# ADR-0009: A pure-Slate front end in its own module, with its art committed as plain files

- **Status:** accepted
- **Date:** 2026-08-22
- **Doctrine sections affected:** none directly. §5 ("everything expensive is
  budgeted, never demand-driven") is the reason the menu backgrounds decode on
  a worker rather than through Slate's resource manager; nothing else in §2 is
  touched. No voxel-core change, no new float in voxel-core, no on-disk format
  change.
- **Human sign-off:** Matt, 2026-08-22 (chose a 1:1 clone of the Mira-Thal
  front end, main menu and loading screens in scope, pause menu deferred, and a
  full named-save system to back CONTINUE and LOAD GAME).

## Context

The project had no front end at all: it booted into `/Engine/Maps/Entry.Entry`,
spawned a fly pawn immediately, and streamed the world from wherever that pawn
landed. No UMG or Slate dependency, no `UUserWidget`, no widget blueprint, no
`.umap`, no `UGameInstance` subclass, no pause, no Escape binding, and no save
system beyond one `.vxlog` per seed. All existing UI was immediate-mode
`AVoxelEarthHUD::DrawHUD` canvas drawing.

The Mira-Thal (Godot) build has a finished, art-directed front end, and the
decision was to clone it. Its menus are built programmatically in GDScript --
the `.tscn` files are near-empty shells -- so nothing was copy-pasteable and
the whole thing had to be rebuilt in Unreal.

Three constraints shaped every decision below, and only the first is obvious.

1. **No editor is available to the implementing agent.** UE 5.8 is a 30 GB
   install; CI cannot compile the module at all (`ue-build.yml` is gated off
   for exactly this reason), and neither can the environment the port was
   written in.
2. **The project's verification is screenshots, not tests.** Roughly 190
   `-Voxel*` switches drive the game headlessly and photograph it. That archive
   is the regression baseline, and a front end is the one feature that can
   invalidate all of it at once by stopping every run at a menu.
3. **The frame budget is already spent.** `docs/backlog.md` §0: streaming is
   solved, the constraint is the frame-time tail. The loading screen is on
   screen at precisely the moment that tail is worst.

## Decision

### 1. Pure C++ Slate, not UMG, in a new `VoxelEarthUI` module

Widgets are `SCompoundWidget` / `SLeafWidget` subclasses added through
`UGameViewportClient::AddViewportWidgetContent`. No `.uasset` of any kind and
no `FSlateStyleSet`.

The font loads from a filesystem path via `FStandaloneCompositeFont` -- the
explicit form of what `FCoreStyle`'s `TTF_FONT` macro does -- and the
background images are decoded from raw bytes at runtime. That is what makes a
menu buildable without an editor at all, which under constraint 1 is not a
preference but a precondition.

UMG was the obvious alternative and lost on its own strongest argument. The
case for it is "a designer can edit it in the editor later"; that is true only
of a real Widget Blueprint, and a C++-built `WidgetTree` is exactly as
un-editable as a Slate tree. With the tiebreaker gone, the rest favours Slate:
the hourglass is a custom `_draw()` port, which is native as `SLeafWidget::OnPaint`
and would have to be written as that Slate widget under UMG anyway before
wrapping it; and there is no `.generated.h` churn in the diff.

Two mitigations replace the designer argument rather than dismissing it. Every
layout number lives in `FVoxelMenuLayout` and is overridable from
`Config/DefaultVoxelUI.ini`, keyed on the C++ member names through a macro so a
rename is a compile error rather than a dead ini key -- a designer moves the
title four pixels with a text editor and no compiler. And if a Widget Blueprint
is ever wanted, each `SVoxel*` widget gets a `UWidget` subclass whose
`RebuildWidget()` returns the existing `SWidget`, which is the standard
promotion path and rewrites no drawing code.

`FSlateStyleSet` was rejected separately: its `IMAGE_BRUSH` path routes image
loading through Slate's resource manager, which decides for itself when a
1920-wide JPEG decode happens. Under constraint 3 that timing is the whole
question, so `FVoxelUIAssetLibrary` decodes explicitly, on a worker at
`BackgroundNormal`, with only the texture upload on the game thread.

**A third module** rather than folding into `VoxelEarth`, for three reasons.
`VoxelEarth` is 59 `.cpp` files built as unity blobs, and UI code is dense in
file-local constants (`kGold`, `kBorderPx`, `kMenuZOrder`) -- precisely the
internal-linkage collision class `tools/lint-unity-collisions.py` exists for
after PR #210 shipped one. `ClientOnly` keeps Slate out of the dedicated-server
binary. And the dependency runs one way only, `VoxelEarthUI → VoxelEarth`: the
front end owns itself as a world subsystem, so the gameplay module never learns
it exists. The one fact that module does need -- whether the front end runs
this session -- lives in `VoxelEarth`'s own `VoxelFrontEndPolicy.h` for exactly
that reason.

This is a deviation worth recording because the only prior second-module
precedent, `VoxelEarthShaders`, had a hard technical reason (PostConfigInit
shader-directory registration). This one is a judgement.

### 2. The menu lives in the existing map; streaming is gated closed

No `.umap` is authored. Four reasons, in order of weight:

1. A `.umap` is a binary asset requiring the editor -- the one thing
   unavailable.
2. Two maps means a real level transition, which destroys and reconstructs
   `UVoxelWorldSubsystem`, which fires `Deinitialize()`'s autosave on the menu
   world. That code already carries a scar from this class of bug:
   `bWorldBegunPlay` exists because the transient Entry world's phantom
   subsystem instance was going to write empty state over a real save.
3. Code-spawned content in `Entry` is already the project's entire
   world-construction model, and `DefaultEngine.ini` says so in its own comment.
4. The gate is a four-line change reusing the early-out `Tick()` has always had
   (`if (!Impl || !ChunkOwner || !ChunkRoot) return;`) and which the dedicated
   server exercises in production every day. A separate `bStreamingGateClosed`
   flag would add a second way to be off and a second thing to keep in sync.

The cost is real and named: the menu and the world share one `UWorld`, so the
seed cannot change after `Initialize`, so a save recorded under a different
seed cannot be opened. See §4.

### 3. The art is committed, re-encoded, as plain files

`ue-project/Content/UI/` holds the typeface and six background JPEGs as
ordinary files, staged by `+DirectoriesToAlwaysStageAsNonUFS`.

`ue-project/Tools/prepare_ui_assets.py` re-encodes the sources to fit
1920x1080 at quality 82: **28.60 MB becomes 2.03 MB, 7.1% of the original**
(measured, not estimated). The originals are 2754x1536 and, for two of them,
5524x3072 -- resolution nobody can see, since the menu draws them at viewport
size behind a 55% black wash.

Committing 2 MB is an exception to this repository's practice and is recorded
as one. The largest tracked binary before this was 385 KB, and `.gitignore`
records that the 38.8 MB `T_SkyStarmap` was excluded for exactly this reason.
The sky precedent -- gitignore plus a pinned fetch script -- does **not**
transfer, because there is no upstream to fetch from: the source is a sibling
checkout that CI does not have, so a fetch script would fail everywhere except
one machine. Art the front end cannot render without is source, not build
output. `MENU_ART_CREDITS.md` records the sha256 of every original so a
re-prepare is reproducible and a swapped source shows up in the diff.

The font is Macondo Swash Caps under SIL OFL 1.1. That was read out of the font
file's own name table (entries 0 and 13) rather than asserted, and OFL
condition 2 requires the licence to travel with the font, so `OFL.txt` carries
the full text.

### 4. Named saves layered over the existing edit log

`Saved/SaveGames/<slug>/{meta.json, world.vxlog}`. The world half is the
existing `EditLog::serialize` format, written through the existing compaction
rule and atomic tmp+rename path, now parameterised by path; `SaveWorld()` still
writes to the seed-derived default, so nothing that predates this behaves
differently.

**Per-save seeds do not work, and cannot without a larger change.** `Seed` is
resolved in `Initialize` *before* `Impl` is constructed and is baked into the
amplifier there. The Godot menu has no seed picker, so a 1:1 clone does not
need one -- NEW GAME uses the run's seed and every save shares it. A save whose
recorded seed differs (reachable only through `-VoxelSeed=`) is listed but not
loadable, with the reason shown in place of its coordinates. Extracting a
`ConstructImplForSeed()` to lift that restriction is a named follow-up.

## Consequences

**The invariant that protects the archive.** When
`VoxelFrontEnd::IsEnabledThisRun()` is false, no code path differs from what it
was before this work. It is enforced three ways: six ordered suppression rules;
a new CI lint (`tools/lint-frontend-switch-coverage.py`) that fails on any
`-Voxel*` switch the naming-convention rule cannot classify and a human has not
listed; and a pixel diff of the capture archive taken before any UI existed.

`FApp::IsUnattended()` alone is **not** sufficient, which is worth recording
because it is the version everybody writes first:
`tools/fluid-spike-measure.ps1` launches `-game -windowed` without it and then
drives itself with a fixture switch.

**What is unverified.** Nothing here has been compiled or photographed --
constraint 1 again. Two specific claims are therefore open:

- **Colour.** `FLinearColor(FColor)` applies the sRGB→linear transfer function
  and Slate re-encodes on output; whether `#f0c14b` survives that round trip is
  an assumption. `VoxelUITheme::Tint()` is the only conversion site and
  `voxel.UI.SRGBTint` A/Bs it. **Nothing should be called pixel-exact until
  that probe runs.**
- **Font metrics.** Macondo at 84 px will not lay out identically in Slate and
  Godot. Expect a few pixels of difference in title width and centring.

**What is deliberately absent.** The pause menu (deferred, with the `Playing`
state left as its seam), the settings screen (a placeholder panel shaped like
HELP and CREDITS), and the menu music -- the three source WAVs are ~124 MB and
one exceeds GitHub's per-file limit, so the handoff hook is built and stubbed.

**Two performance simplifications carry forward as decisions**, with their
original reasoning transcribed into the code: no vignette, and a flat progress
bar instead of the mock's gradient. Each was implemented in the Godot build and
then removed because a full-viewport per-pixel pass during chunk streaming
spiked frame delta into visible-stutter range. Both look like omissions and are
not.
