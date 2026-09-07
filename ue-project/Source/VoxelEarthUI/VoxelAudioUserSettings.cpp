#include "VoxelAudioUserSettings.h"

#include "VoxelEarthUI.h"
#include "VoxelUIMusic.h"

#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"

namespace VoxelAudioUserSettingsDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

constexpr const TCHAR* kSection = TEXT("VoxelAudio");
constexpr const TCHAR* kMasterKey = TEXT("MasterVolume");
constexpr const TCHAR* kMusicKey = TEXT("MusicVolume");

// A value out of the ini is player-editable text and can be anything. Clamped
// on the way IN as well as on the way out, because a hand-edited 5.0 would
// otherwise reach FApp::SetVolumeMultiplier and be a genuinely damaging bug --
// the one settings mistake here that can hurt somebody.
float Sanitise(float Volume)
{
	return FMath::IsFinite(Volume) ? FMath::Clamp(Volume, 0.f, 1.f) : 1.f;
}

float ReadFloat(const TCHAR* Key)
{
	float Value = 1.f;
	if (GConfig)
	{
		GConfig->GetFloat(kSection, Key, Value, GGameUserSettingsIni);
	}
	return Sanitise(Value);
}

void WriteFloat(const TCHAR* Key, float Value)
{
	if (GConfig)
	{
		GConfig->SetFloat(kSection, Key, Value, GGameUserSettingsIni);
		// Flush now, not at shutdown -- VoxelGraphicsUserSettings' reason, and
		// it is sharper for a slider: the player has just HEARD the change and
		// would have no reason to doubt it stuck.
		GConfig->Flush(false, GGameUserSettingsIni);
	}
}
} // namespace VoxelAudioUserSettingsDetail

namespace VoxelAudioUserSettings
{
float GetMasterVolume()
{
	return VoxelAudioUserSettingsDetail::ReadFloat(VoxelAudioUserSettingsDetail::kMasterKey);
}

void SetMasterVolume(float Volume)
{
	VoxelAudioUserSettingsDetail::WriteFloat(VoxelAudioUserSettingsDetail::kMasterKey,
	                                         VoxelAudioUserSettingsDetail::Sanitise(Volume));
	ApplyAll();
}

float GetMusicVolume()
{
	return VoxelAudioUserSettingsDetail::ReadFloat(VoxelAudioUserSettingsDetail::kMusicKey);
}

void SetMusicVolume(float Volume)
{
	VoxelAudioUserSettingsDetail::WriteFloat(VoxelAudioUserSettingsDetail::kMusicKey,
	                                         VoxelAudioUserSettingsDetail::Sanitise(Volume));
	ApplyAll();
}

float GetEffectiveMusicVolume()
{
	return GetMasterVolume() * GetMusicVolume();
}

void ApplyAll()
{
	// FApp's multiplier is read by FAudioDevice every update, so this needs no
	// device handle and works before any audio device exists -- which matters,
	// because UVoxelFrontEndSubsystem::Initialize runs early.
	FApp::SetVolumeMultiplier(GetMasterVolume());
	FVoxelUIMusic::Get().ApplyVolume();
}
} // namespace VoxelAudioUserSettings
