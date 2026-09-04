#pragma once
// Player-facing graphics settings (2026-09-04, owner request: "I'd like this
// setting to be toggle-able for players in game graphics settings").
//
// SCOPE, STATED SO IT STAYS SMALL. This is NOT a scalability system and not a
// UGameUserSettings subclass -- it is a flat list of named toggles the
// SETTINGS panel renders, each persisted under [VoxelGraphics] in
// GGameUserSettingsIni and applied by setting the cvar it fronts. A setting
// earns a row here only after the project's own A/B discipline sized its cost
// and the owner judged its image (the first entry's receipts:
// docs/perf-redesign-2026-09-03.md Phase 1).
//
// WHY ITS OWN FILE AND NOT VoxelUITheme: the theme reads DESIGN values at
// style-build time; these are RUNTIME toggles a player flips mid-session, and
// the apply path (cvar set) must run at boot in every world, menu or game --
// UVoxelFrontEndSubsystem::Initialize calls ApplyAll() for that.

#include "CoreMinimal.h"

namespace VoxelGraphicsUserSettings
{
// "Fine Detail Smoothing" -- fronts r.TSR.ThinGeometryDetection. OFF is the
// shipped default (2026-09-04): -0.24 ms/frame at the owner's config and an
// owner-judged close-range pair (VoxelVerify00656/00658) was
// indistinguishable. ON restores the engine default for players who want
// maximum thin-feature stability and will pay the frame time.
VOXELEARTHUI_API bool GetFineDetailSmoothing();
VOXELEARTHUI_API void SetFineDetailSmoothing(bool bEnabled);

// "Faster Terrain Drawing" -- fronts voxel.March.TemporalPrime. ON is the
// shipped default (2026-09-04, owner-approved): each frame's terrain rays
// start from the previous frame's reprojected hits; a mandatory full re-walk
// closes every miss, and the ship gates measured no image difference
// (docs/perf-redesign-2026-09-03.md Gate 3). OFF is the always-full-walk
// control for players who want to compare.
VOXELEARTHUI_API bool GetFasterTerrainDrawing();
VOXELEARTHUI_API void SetFasterTerrainDrawing(bool bEnabled);

// Push every persisted setting into its cvar. Idempotent; called from
// UVoxelFrontEndSubsystem::Initialize so a fresh process honours the player's
// saved choices before the first marched frame, and from every Set* so a
// toggle takes effect the frame it is clicked.
VOXELEARTHUI_API void ApplyAll();
} // namespace VoxelGraphicsUserSettings
