#pragma once
// "Menu Size" -- the player's scale dial for the unified in-game screen shell
// (inventory / journal / map / player / codex).
//
// Owner directive, 2026-09-07, verbatim: *"for the unified inventory/journal/
// map/player/codex UI menu, make it resizable if a player wants to make the
// entire thing larger or smaller in run time."*
//
// WHY A SEPARATE FILE FROM VoxelGraphicsUserSettings, which is where UIScale
// lives and which this otherwise copies line for line. Two reasons, and the
// second is the one that matters:
//
//   * That namespace is documented as "a flat list of named toggles the
//     SETTINGS panel renders, each ... applied by setting the cvar it fronts".
//     This setting fronts no cvar. It is read by one widget.
//   * IT HAS A CHANGE NOTIFICATION AND THE OTHERS DO NOT. The shell can be
//     resized from two places at once -- the Settings row and Ctrl+wheel inside
//     an open screen -- so the row has to be able to hear about a change it did
//     not make. UIScale has no such second author (FSlateApplication is the one
//     store and Slate re-reads it every frame), so putting a delegate on that
//     namespace would be dead weight there and a second pattern to remember.
//
// WHAT IT IS, PRECISELY. A multiplier applied to SVoxelScreenShell as a Slate
// RENDER TRANSFORM about the frame's centre -- not a layout scale. ADR-0011
// keeps the shell authored at 1060x760 and lets the engine's own DPI curve size
// it for the screen; this is the player's dial on top of that, and because it
// is a render transform the layout never reflows. Nothing inside the shell
// re-wraps, re-measures or changes what fits, at any setting. See
// SVoxelScreenShell::GetShellRenderTransform.
//
// It is deliberately INDEPENDENT of INTERFACE SIZE (VoxelGraphicsUserSettings::
// GetUIScale). That one scales the whole application, HUD and menus together;
// this one scales the five in-game screens and nothing else, which is what the
// owner asked for. The two multiply.

#include "CoreMinimal.h"

// Fired whenever the stored scale changes, by whichever author changed it. The
// Settings row binds this so its slider tracks a Ctrl+wheel made while the
// panel is not even open.
DECLARE_MULTICAST_DELEGATE_OneParam(FOnVoxelScreenShellScaleChanged, float /*NewScale*/);

namespace VoxelScreenShellSettings
{
// The stored multiplier, always one of the sixteen stops between the bounds
// below: snapped and clamped on the way out as well as in, so a hand-edited ini
// cannot leave the slider sitting between two of its own stops.
VOXELEARTHUI_API float GetScale();

// Persists to [VoxelGraphics] ScreenShellScale in GGameUserSettingsIni, flushes
// immediately (a crash between a resize and exit must not silently revert a
// size the player watched take effect), broadcasts OnScaleChanged, and logs
// `VoxelScreenShell: scale %.2f`.
//
// THE LOG LINE IS NOT DECORATION. It is how a headless leg proves the dial
// moved: a resize that did nothing and a resize that worked otherwise produce
// identical output, which is this project's house failure. Only logged when the
// value actually changes, so the line means "something moved" rather than
// "something was asked".
VOXELEARTHUI_API void SetScale(float Scale);

// The bounds, exported so the Settings row maps its 0..1 slider onto them
// rather than restating them -- one authority for the range, as the cvar
// spellings are in VoxelGraphicsUserSettings.
VOXELEARTHUI_API float ScaleMin();
VOXELEARTHUI_API float ScaleMax();
VOXELEARTHUI_API float ScaleStep();
VOXELEARTHUI_API float ScaleDefault();

// One step up or down from wherever the setting currently is, clamped. What
// Ctrl+wheel and Ctrl+plus/minus call, so the keyboard and the wheel cannot
// disagree with each other or with the slider about what a step is.
VOXELEARTHUI_API void StepScale(int32 Steps);

// THE FUNNEL ITSELF, EXPORTED, and only because one author needs to show the
// value before it stores it. SVoxelScreenShell's corner drag paints the shell
// at a scale it has not committed yet -- the ini is written once, on release --
// so it has to snap the same way SetScale will, or the shell would settle onto
// a different size at the end of the gesture than the one the player was
// watching. Snapping locally in the widget would be a second copy of the rule.
VOXELEARTHUI_API float SnapScale(float Scale);

VOXELEARTHUI_API FOnVoxelScreenShellScaleChanged& OnScaleChanged();
} // namespace VoxelScreenShellSettings
