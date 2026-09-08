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
constexpr const TCHAR* kMusicInGameKey = TEXT("MusicInGame");

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

// DEFAULTED TRUE BEFORE THE READ, not after it. GConfig->GetBool leaves its
// out-parameter untouched when the key is absent, so the default has to be the
// value it starts at -- writing `bool Value = false;` here would make an
// unwritten ini mean "no music", which is the opposite of the shipped default
// and would be invisible on the machine that already has the key.
bool ReadBool(const TCHAR* Key, bool bDefault)
{
	bool bValue = bDefault;
	if (GConfig)
	{
		GConfig->GetBool(kSection, Key, bValue, GGameUserSettingsIni);
	}
	return bValue;
}

void WriteBool(const TCHAR* Key, bool bValue)
{
	if (GConfig)
	{
		GConfig->SetBool(kSection, Key, bValue, GGameUserSettingsIni);
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

bool GetMusicInGame()
{
	return VoxelAudioUserSettingsDetail::ReadBool(VoxelAudioUserSettingsDetail::kMusicInGameKey, /*bDefault=*/true);
}

void SetMusicInGame(bool bEnabled)
{
	VoxelAudioUserSettingsDetail::WriteBool(VoxelAudioUserSettingsDetail::kMusicInGameKey, bEnabled);
	// NO ApplyAll() HERE, and the asymmetry with the two volume setters is
	// deliberate. A volume is a level and applies to whatever is playing; this
	// is a policy about the HAND-OFF, read once at hand-off by
	// UVoxelFrontEndSubsystem::TickHandOff and once per session by
	// UVoxelScreensUISubsystem. Turning it off mid-game does not stop the track
	// the player is already listening to -- the PAUSE button does that, which is
	// the control they actually have their hand on.
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
