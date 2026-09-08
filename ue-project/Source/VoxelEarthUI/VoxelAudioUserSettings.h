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

// Whether the soundtrack keeps playing once the player has the world.
//
// TRUE BY DEFAULT, AND THAT IS THE OWNER'S DIRECTIVE (2026-09-07, verbatim:
// "by default, music from the game's soundtrack/library should play when in
// game"). It is a persisted setting rather than a constant so that the standing
// settings-panel policy -- every visual or audible trade ships as a
// player-facing row -- can be satisfied with a row and no code change: the row
// is the only thing missing, and it reads and writes exactly this pair.
//
// A MISSING INI KEY MUST READ AS TRUE, for the same reason a missing volume key
// reads as 1.0: an installed game plays its music until the player says
// otherwise, and a fresh machine must not be silently different from the one
// the feature was written on.
VOXELEARTHUI_API bool GetMusicInGame();
VOXELEARTHUI_API void SetMusicInGame(bool bEnabled);

// Push both persisted values at their targets. Idempotent; called from
// UVoxelFrontEndSubsystem::Initialize alongside VoxelGraphicsUserSettings so a
// fresh process is at the player's chosen volume before the menu music starts,
// and from FVoxelUIMusic when a track begins so a component created after the
// last Set is not created at full volume.
VOXELEARTHUI_API void ApplyAll();

// --- The music system's cross-session memory (docs/music-design.md section 8)
//
// "Remember the last five cues played per pool so a relaunch does not open on
// the same one." Persisted here rather than in a file of its own because it is
// exactly what this namespace already is: a handful of small player-scoped
// values under one section of GGameUserSettingsIni.
//
// KEYED BY POOL, NOT BY BANK. Explore has five folders (Day/Night/Dawn/Dusk/
// Rain) and one memory, which is what the sentence asks for: the player is
// tired of a CUE, not of a folder, and a cue borrowed by Dusk from Day should
// not sound fresh again because it arrived through a different slot.
//
// STORED BY NAME, NOT BY INDEX. An index into a folder listing is meaningless
// after the designer drops a file in -- it would silently name a different cue
// -- and the filename is already the cue's identity everywhere else here (it
// is what the HUD label shows).
VOXELEARTHUI_API TArray<FString> GetRecentMusicCues(const TCHAR* PoolName);
VOXELEARTHUI_API void PushRecentMusicCue(const TCHAR* PoolName, const FString& CueName);

// The music component's own multiplier: master x music, which is what
// FVoxelUIMusic applies to the component it owns. Exposed rather than computed
// at the call site so the two settings can only be combined one way.
VOXELEARTHUI_API float GetEffectiveMusicVolume();
} // namespace VoxelAudioUserSettings
