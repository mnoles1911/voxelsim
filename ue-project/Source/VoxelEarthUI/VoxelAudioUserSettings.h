#pragma once
// Player-facing audio settings: the AUDIO section of the 2026-09-07 SETTINGS
// mock.
//
// THE SAME SHAPE AS VoxelGraphicsUserSettings, deliberately -- a flat namespace
// of Get/Set pairs persisted under one section of GGameUserSettingsIni and
// applied immediately on every Set, with an ApplyAll() the front end calls at
// boot. Its header's scope note applies here word for word; what differs is
// only that these are floats and that what they front is not a cvar.
//
// TWO SLIDERS, NOT THE MOCK'S THREE, AND THAT IS A MEASUREMENT.
//
//   MASTER fronts FApp::SetVolumeMultiplier, which FAudioDevice folds into its
//   primary volume (AudioDevice.cpp: `PrimaryVolume *= FApp::GetVolumeMultiplier()`).
//   It is genuinely engine-wide: everything this process ever plays goes
//   through it.
//
//   MUSIC scales the one music voice this project has, FVoxelUIMusic's audio
//   component, on top of MASTER.
//
//   SFX IS OMITTED. There is no sound effect anywhere in this project to bind
//   it to -- Content/Audio/SFX holds a README and nothing else, there is no
//   Sound Class or Sound Mix asset in the repository, and FVoxelUIMusic is the
//   only thing that ever creates an audio component. A third slider would move
//   a knob, write an ini key and change nothing audible, which is worse than an
//   absent row: it teaches the player that the settings screen lies. The row
//   arrives with the first sound effect, and the binding it will want (a
//   USoundClass named SFX, or an equivalent submix) is named here so it is one
//   edit rather than a rediscovery.

#include "CoreMinimal.h"

namespace VoxelAudioUserSettings
{
// 0..1, both defaulting to 1.0 -- an installed game is at full volume until the
// player says otherwise, and a missing ini key must read as that rather than as
// silence.
VOXELEARTHUI_API float GetMasterVolume();
VOXELEARTHUI_API void SetMasterVolume(float Volume);

VOXELEARTHUI_API float GetMusicVolume();
VOXELEARTHUI_API void SetMusicVolume(float Volume);

// Push both persisted values at their targets. Idempotent; called from
// UVoxelFrontEndSubsystem::Initialize alongside VoxelGraphicsUserSettings so a
// fresh process is at the player's chosen volume before the menu music starts,
// and from FVoxelUIMusic when a track begins so a component created after the
// last Set is not created at full volume.
VOXELEARTHUI_API void ApplyAll();

// The music component's own multiplier: master x music, which is what
// FVoxelUIMusic applies to the component it owns. Exposed rather than computed
// at the call site so the two settings can only be combined one way.
VOXELEARTHUI_API float GetEffectiveMusicVolume();
} // namespace VoxelAudioUserSettings
