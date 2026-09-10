#pragma once
// The menu-state scalability drop: render the 3D scene cheaply while nobody
// can see it, and put every cvar back before anybody can.
//
// WHAT THE MENU ACTUALLY COSTS, AND WHY THAT IS ABSURD. Parked on the title
// screen the process spends ~11 ms of GPU and keeps ~4 CPU cores busy with
// NOTHING on screen but a full-screen 2D Slate image. The world is held
// (UVoxelWorldSubsystem::Tick returns on its first line while ChunkOwner is
// null), so none of that is terrain streaming. It is the scene renderer,
// drawing a scene the player is not looking at: SVoxelMainMenu's bottom
// overlay slot is an OPAQUE full-viewport fill (VoxelUITheme::MenuBackdrop,
// FColor(0x0a,0x0a,0x0f), alpha 255) with the cover art and the panels on top
// of it, and viewport widgets composite AFTER the scene render. Every pixel
// the renderer produces during the menu is painted over before the frame
// reaches the screen.
//
// WHY THIS IS NOT AN INI CHANGE. ue-project/Config/DefaultEngine.ini is the
// IN-GAME configuration -- r.ScreenPercentage=65 and the shadow block are
// measured, owner-approved values with their reasoning written beside them.
// Nothing here changes what the game runs at. This is a temporary runtime
// override with an exact restore, on the same shape as
// UVoxelFrontEndSubsystem::CapStreamingForTheatre (which caps
// voxel.Stream.ApplyBudgetMs for the load theatre and puts it back at the
// reveal).
//
// WHERE IT IS APPLIED AND RELEASED
// --------------------------------
//   Apply()   from UVoxelFrontEndSubsystem::EnterMenu, once the menu widget
//             is on the viewport.
//   Restore() from TickLoading at the reveal -- BEFORE the curtain starts
//             fading, the same instant and for the same reason the streaming
//             budget is restored there -- and from TeardownMenu as the
//             backstop for every path that never reaches a reveal (a quit
//             from the menu, a PIE teardown, Deinitialize).
//
// So the drop covers Menu, ArmLoading and Loading -- the whole time an opaque
// curtain owns the screen -- and is gone for the first frame the player sees.
//
// ONE KNOWN EXPOSURE, RECORDED RATHER THAN GUESSED AT. The MENU's occlusion is
// guaranteed by a solid fill under the art, so it holds even when the art is
// missing. The LOADING screen's bottom layer is the cover image itself
// (SVoxelLoadingScreen.cpp:161) with no solid fill beneath it, so a run that
// deliberately breaks the art -- -VoxelUINoAssets -- could show the reduced-
// resolution world through the gaps for the duration of the load. No shot in
// tools/voxel-ui-capture.ps1 does that (-Shot Fallback is a MENU shot), and a
// player never can. If a loading-screen capture is ever run with
// -VoxelUINoAssets, expect the difference and do not read it as a defect in
// this file.
//
// THREE THINGS THAT KEEP IT OUT OF EVERY MEASUREMENT
// --------------------------------------------------
//  1. Apply() REFUSES, with an Error, unless VoxelFrontEnd::IsEnabledThisRun().
//     That predicate is already false for every unattended run, every
//     self-driving switch and every -nullrhi/dedicated-server run
//     (VoxelFrontEndPolicy.cpp rules 3-5), which is the whole verification
//     fleet. No perf leg and no capture can measure a scalability-dropped
//     frame by accident, and if a future caller tries, the log says so.
//  2. -VoxelMenuScalability=0 turns it off on the same binary, so the A/B
//     needs no rebuild.
//  3. -VoxelMenuScalability=2 is the DRY RUN: it logs the exact list it would
//     set, with each cvar's current value, and sets nothing. That is how the
//     list is proved right without touching a single frame.
//
// The menu-shot captures (-VoxelMenuShot, -VoxelMenuPanel, ...) DO run with
// the drop applied: they force the front end on (rule 3.5). That is
// deliberate and it is the image gate -- the menu PNG must hash identically
// with the drop on and off, because the scene behind it is fully occluded. A
// difference is a real defect (something on the menu is translucent), not
// noise.

#include "CoreMinimal.h"

namespace VoxelMenuScalability
{
// -VoxelMenuScalability=0|1|2, latched once. Default 1.
enum class EMode : uint8
{
	// 0: never touch a cvar. The control arm.
	Off,
	// 1 (default): apply on Menu, restore at the reveal.
	On,
	// 2: log the list and the values it WOULD set, change nothing. Proves the
	// list without changing the frame it is meant to be measured against.
	DryRun,
};

// Resolved once from the command line and cached for the process, the same
// way VoxelFrontEnd::MenuTickGatesEnabled() is: the answer is a property of
// the command line, which never changes.
VOXELEARTH_API EMode ModeThisRun();

// Drops the render cost for the menu. Idempotent: a second call while already
// applied does nothing, so re-entering the menu (the pause menu's EXIT TO MENU
// reopens the map) cannot save the dropped values as the "previous" ones.
//
// Logs exactly one line naming every cvar it changed with before -> after, or
// -- if it changed NOTHING -- one Error saying so. An arm that silently sets
// an empty list is the house failure this project keeps paying for; here it is
// visibly a failure instead.
VOXELEARTH_API void Apply();

// Puts every cvar back to the value Apply() read off it. Idempotent and silent
// when nothing was applied, because it has two callers by design (the reveal
// and the teardown backstop). Logs one line with each cvar's restored value,
// and an Error naming any cvar whose value did not come back.
VOXELEARTH_API void Restore();

// True between Apply() and Restore(). For tests and for a caller that wants to
// assert the drop is gone; nothing in the shipping path needs to ask.
VOXELEARTH_API bool IsApplied();
} // namespace VoxelMenuScalability
