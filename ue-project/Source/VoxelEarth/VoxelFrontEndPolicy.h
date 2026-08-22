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
// See VoxelFrontEndPolicy.cpp for the six suppression rules and why the
// obvious one-line version (FApp::IsUnattended()) is not sufficient.

#include "CoreMinimal.h"

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

// Exposed for tools/lint-frontend-switch-coverage.py's counterpart test and
// for -VoxelFrontEndExplain: classify one switch NAME (no leading dash, no
// trailing '='), e.g. "VoxelGICaveTest" -> true, "VoxelSeed" -> false.
// See the rules table in the .cpp.
VOXELEARTH_API bool IsSelfDrivingSwitchName(const FString& SwitchName);
} // namespace VoxelFrontEnd
