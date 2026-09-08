#include "VoxelScreenShellSettings.h"

#include "VoxelEarthUI.h" // LogVoxelUI

#include "Misc/ConfigCacheIni.h"

namespace VoxelScreenShellSettingsDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// THE SAME SECTION AS THE GRAPHICS ROWS, on purpose. A player looking at
// GameUserSettings.ini should find every setting this front end owns in one
// block; the file boundary in the source is about ownership, not about storage.
const TCHAR* const kSection = TEXT("VoxelGraphics");
const TCHAR* const kKey = TEXT("ScreenShellScale");

// 1.00 is the authored size -- ADR-0011's 1060x760 shell at whatever the
// engine's own resolution scale chose. The range and the step match INTERFACE
// SIZE's exactly, because a player who has learned one dial should not have to
// learn a second one with different stops.
constexpr float kDefault = 1.00f;
constexpr float kMin = 0.75f;
constexpr float kMax = 1.50f;
constexpr float kStep = 0.05f;

// SNAPPED AND CLAMPED IN ONE PLACE, and both directions go through it: the
// slider hands over a continuous value, the wheel hands over a step, and the
// ini can hold whatever a text editor put there. Without one funnel the stored
// number and the sixteen the row can display drift apart, and the knob stops
// sitting where it was left.
float Snap(float Scale)
{
	const float Snapped = FMath::RoundToFloat(Scale / kStep) * kStep;
	return FMath::Clamp(Snapped, kMin, kMax);
}

FOnVoxelScreenShellScaleChanged GOnChanged;
} // namespace VoxelScreenShellSettingsDetail

namespace VoxelScreenShellSettings
{
float ScaleMin() { return VoxelScreenShellSettingsDetail::kMin; }
float ScaleMax() { return VoxelScreenShellSettingsDetail::kMax; }
float ScaleStep() { return VoxelScreenShellSettingsDetail::kStep; }
float ScaleDefault() { return VoxelScreenShellSettingsDetail::kDefault; }

FOnVoxelScreenShellScaleChanged& OnScaleChanged()
{
	return VoxelScreenShellSettingsDetail::GOnChanged;
}

float GetScale()
{
	float Scale = VoxelScreenShellSettingsDetail::kDefault;
	if (GConfig)
	{
		GConfig->GetFloat(VoxelScreenShellSettingsDetail::kSection, VoxelScreenShellSettingsDetail::kKey, Scale,
		                  GGameUserSettingsIni);
	}
	return VoxelScreenShellSettingsDetail::Snap(Scale);
}

void SetScale(float Scale)
{
	const float Snapped = VoxelScreenShellSettingsDetail::Snap(Scale);
	const float Previous = GetScale();

	if (GConfig)
	{
		GConfig->SetFloat(VoxelScreenShellSettingsDetail::kSection, VoxelScreenShellSettingsDetail::kKey, Snapped,
		                  GGameUserSettingsIni);
		// Flush now, not at shutdown, for the reason every other row in this
		// front end flushes now: a crash between the resize and exit must not
		// silently revert a size the player watched take effect.
		GConfig->Flush(false, GGameUserSettingsIni);
	}

	// EQUALITY ON THE SNAPPED VALUES, NOT ON THE RAW ONES. A wheel notch that
	// lands past a clamp produces the same setting it started from, and firing
	// the delegate and the log line for that would make "the dial moved" and
	// "the dial was at its limit" look identical in a log -- which is exactly
	// the confirmation-that-cannot-fail this project keeps paying for.
	if (FMath::IsNearlyEqual(Snapped, Previous, UE_KINDA_SMALL_NUMBER))
	{
		return;
	}

	UE_LOG(LogVoxelUI, Log, TEXT("VoxelScreenShell: scale %.2f"), Snapped);
	VoxelScreenShellSettingsDetail::GOnChanged.Broadcast(Snapped);
}

void StepScale(int32 Steps)
{
	SetScale(GetScale() + float(Steps) * VoxelScreenShellSettingsDetail::kStep);
}
} // namespace VoxelScreenShellSettings
