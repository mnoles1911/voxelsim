#pragma once
// Does the front end (main menu + loading screen) run this session?
//
// WHY THIS LIVES IN VoxelEarth AND NOT IN VoxelEarthUI. Three things change
// behaviour when the front end is active, and only one of them is UI:
//
//   * AVoxelEarthGameMode sets bStartPlayersAsSpectators, so the pawn is NOT
//     spawned at StartPlay -- the menu spawns it on NEW GAME/CONTINUE.
//   * UVoxelWorldSubsystem::OnWorldBeginPlay does NOT call StartWorldSession,
//     so ChunkOwner/ChunkRoot stay null and Tick() no-ops -- nothing streams
//     while the player is reading a menu.
//   * UVoxelFrontEndSubsystem (VoxelEarthUI) puts widgets on the viewport.
//
// The first two are gameplay-module decisions. If the predicate lived in the
// UI module, VoxelEarth would have to depend on VoxelEarthUI to ask it, which
// inverts the dependency (VoxelEarthUI -> VoxelEarth) that keeps Slate out of
// the gameplay module and out of the dedicated-server binary. So the predicate
// lives here and the UI module reads it, not the other way round.
//
// THE INVARIANT THIS FILE EXISTS TO PROTECT
// -----------------------------------------
// When IsEnabledThisRun() is false, NO code path differs from what it was
// before the front end was added. That is not a nicety: roughly forty
// `-Voxel*` verification switches drive the game themselves -- they spawn a
// pawn, fly it somewhere, wait N seconds and screenshot -- and every one of
// them would hang forever at a menu nobody is there to click. The archive of
// captures those switches produced is the project's regression baseline, so
// "the menu did not change any of them" has to be true by construction rather
// than by inspection.
//
// See VoxelFrontEndPolicy.cpp for the rules, in the order they are applied,
// and for why the obvious one-line version (FApp::IsUnattended()) is not
// sufficient on its own.

#include "CoreMinimal.h"

class UWorld;

namespace VoxelFrontEnd
{
// True when the main menu should own the session's first screen. Resolved
// ONCE on first call and cached for the lifetime of the process -- the answer
// is a property of the command line, which never changes, and callers ask
// from constructors, subsystem Initialize and per-tick code alike, so a stable
// answer matters more than a re-readable one.
VOXELEARTH_API bool IsEnabledThisRun();

// Why the answer was what it was, as a stable literal ("unattended run",
// "-VoxelNoMenu", "self-driving switch -VoxelGICaveTest", ...). Logged once at
// startup by UVoxelFrontEndSubsystem so that every capture log carries the
// evidence for which arm it ran -- a screenshot that came out wrong is then
// diagnosable from the log rather than by re-running it.
VOXELEARTH_API const TCHAR* WhyThisAnswer();

// True while the menu owns the screen and the world must not be touched.
//
// WHY THIS EXISTS, and it is not a nicety either. The comment at the top of
// this file states the invariant as already achieved -- "nothing streams while
// the player is reading a menu" -- and for the CHUNK STREAMER that was true:
// ChunkOwner stays null and UVoxelWorldSubsystem::Tick no-ops. Nothing held
// the other world-touching tickers, and on 2026-08-25 that made the main menu
// unphotographable on this box. Two of them queried worldgen every tick:
//
//   UVoxelWaterSubsystem::Tick -> RefreshImplicitWater -> EnsureWorldgenColumn
//   AVoxelOceanActor::UpdateUnderwaterState -> IsUnderwaterAtWorld
//
// and because the front end also sets bStartPlayersAsSpectators, no pawn
// exists yet, so the player viewpoint is the WORLD ORIGIN regardless of
// -VoxelSpawnAt. They therefore queried worldgen at (0,0) -- a place the run
// never intended to visit -- the fine tier had no tile baked there, and
// FVoxelFineTileStreamer is fatal by design on a gate leak in an unattended
// run. Three capture attempts, three fatals, no image. See backlog 0.0k.
//
// The deadlock is worth stating because it is not obvious: the held subsystem
// is the one that PREFETCHES. Hold it and leave the others running, and the
// others fault on tiles nothing will ever fetch for them. The log says so in
// as many words -- "The residency tick has NEVER RUN (no ring centre)".
//
// Derived, not stored. It reads UVoxelWorldSubsystem::HasWorldSessionStarted()
// -- the same ChunkOwner the streamer gates on -- rather than a flag the UI
// module sets on state transitions. A second copy of "is the menu up" is a
// second thing that can be wrong, and this codebase has been bitten enough by
// joins computed instead of checked.
//
// Returns false when the front end is not enabled this run, so the invariant
// at the top of this file holds: with no menu, no caller behaves differently.
VOXELEARTH_API bool IsWorldHeldForMenu(const UWorld* World);

// Exposed for tools/lint-frontend-switch-coverage.py's counterpart test and
// for -VoxelFrontEndExplain: classify one switch NAME (no leading dash, no
// trailing '='), e.g. "VoxelGICaveTest" -> true, "VoxelSeed" -> false.
// See the rules table in the .cpp.
VOXELEARTH_API bool IsSelfDrivingSwitchName(const FString& SwitchName);
} // namespace VoxelFrontEnd
