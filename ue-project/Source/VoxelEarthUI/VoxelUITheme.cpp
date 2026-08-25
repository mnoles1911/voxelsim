#include "VoxelUITheme.h"

#include "VoxelEarthUI.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"

namespace VoxelUIThemeDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// voxel.UI.SRGBTint -- THE ONE UNMEASURED THING IN THE PALETTE.
//
// Slate composites in linear space and encodes back to sRGB on output, so
// FLinearColor(FColor) -- which applies the sRGB->linear table -- SHOULD
// round-trip #f0c14b back to #f0c14b on screen. Should. That is a claim about
// the renderer's output transform, this project renders through TSR with a
// non-default screen percentage, and nobody has yet put a colour picker on a
// captured menu screenshot to check.
//
// So both arms are available behind one flag. 1 (default) is the correct-by-
// construction interpretation: the tokens are sRGB hex, decode them. 0 treats
// the bytes as if they were already linear, which is what a naive
// FLinearColor(R/255, G/255, B/255) would do and is the arm to try if the menu
// photographs washed out. -VoxelMenuShot plus a pixel sample at the title
// settles it; until then, docs/front-end-plan.md's R1 stands and nothing here
// should be called pixel-exact.
int32 GSRGBTint = 1;
FAutoConsoleVariableRef CVarSRGBTint(TEXT("voxel.UI.SRGBTint"),
                                     GSRGBTint,
                                     TEXT("1 (default): treat VoxelUITheme tokens as sRGB and decode to linear for Slate.\n")
                                     TEXT("0: pass the bytes through as linear. The A/B for washed-out menu colours."),
                                     ECVF_Default);
} // namespace VoxelUIThemeDetail

namespace VoxelUITheme
{
FLinearColor Tint(const FColor& Colour, float Alpha)
{
	FLinearColor Out;
	if (VoxelUIThemeDetail::GSRGBTint != 0)
	{
		// FLinearColor(FColor) is the sRGB-decoding constructor.
		Out = FLinearColor(Colour);
	}
	else
	{
		Out = FLinearColor(float(Colour.R) / 255.f, float(Colour.G) / 255.f, float(Colour.B) / 255.f,
		                   float(Colour.A) / 255.f);
	}
	// The token's own alpha (always 255 here) times the caller's, so
	// Tint(Gold, 0.55f) reads the same way the GDScript's Color(GOLD, 0.55)
	// does at the call site.
	Out.A *= Alpha;
	return Out;
}

FColor Darkened(const FColor& Colour, float Amount)
{
	// Godot: `Color.darkened(k)` returns `Color(r*(1-k), g*(1-k), b*(1-k), a)`
	// over its STORED components, which for a Color built from a hex literal
	// are the sRGB values. So this scales bytes, not light. See the header.
	const float Scale = FMath::Clamp(1.0f - Amount, 0.0f, 1.0f);
	return FColor(uint8(FMath::RoundToInt(float(Colour.R) * Scale)),
	              uint8(FMath::RoundToInt(float(Colour.G) * Scale)),
	              uint8(FMath::RoundToInt(float(Colour.B) * Scale)), Colour.A);
}
} // namespace VoxelUITheme

// --- FVoxelMenuLayout::Get --------------------------------------------------

namespace VoxelUILayoutDetail
{
const TCHAR* const kSection = TEXT("VoxelUI.Layout");
const TCHAR* const kIniName = TEXT("VoxelUI");

int32 OverrideCount = 0;

void ReadFloat(const TCHAR* Key, float& InOut)
{
	float Value = 0.f;
	if (GConfig && GConfig->GetFloat(kSection, Key, Value, kIniName))
	{
		InOut = Value;
		++OverrideCount;
	}
}

void ReadInt(const TCHAR* Key, int32& InOut)
{
	int32 Value = 0;
	if (GConfig && GConfig->GetInt(kSection, Key, Value, kIniName))
	{
		InOut = Value;
		++OverrideCount;
	}
}

FVoxelMenuLayout Load()
{
	FVoxelMenuLayout L;

	// The macro pairs the ini key to the member so the two can never disagree
	// -- a renamed member is a compile error here rather than an ini key that
	// silently stops working.
#define VOXELUI_LAYOUT_FLOAT(Member) ReadFloat(TEXT(#Member), L.Member)
#define VOXELUI_LAYOUT_INT(Member) ReadInt(TEXT(#Member), L.Member)

	VOXELUI_LAYOUT_FLOAT(MainPanelHalfWidth);
	VOXELUI_LAYOUT_FLOAT(MainPanelHalfHeight);
	VOXELUI_LAYOUT_FLOAT(MainColumnSeparation);
	VOXELUI_LAYOUT_FLOAT(TitleToButtonsSpacer);
	// Registered so the title width can be retuned from a capture without a
	// rebuild -- which matters more here than for the other layout floats,
	// because the only way to check it is to shoot the menu and measure the
	// rendered span. See TitleBoxWidth in the header.
	VOXELUI_LAYOUT_FLOAT(TitleBoxWidth);
	VOXELUI_LAYOUT_FLOAT(QuitSpacer);
	VOXELUI_LAYOUT_FLOAT(ButtonMinHeight);
	VOXELUI_LAYOUT_INT(ButtonFontSize);
	VOXELUI_LAYOUT_INT(TitleFontSize);
	VOXELUI_LAYOUT_INT(SubtitleFontSize);
	VOXELUI_LAYOUT_INT(VersionFontSize);
	VOXELUI_LAYOUT_FLOAT(VersionInsetLeft);
	VOXELUI_LAYOUT_FLOAT(VersionInsetBottom);
	VOXELUI_LAYOUT_FLOAT(BackgroundTintAlpha);

	VOXELUI_LAYOUT_FLOAT(SubPanelHalfWidth);
	VOXELUI_LAYOUT_FLOAT(SubPanelHalfHeight);
	VOXELUI_LAYOUT_FLOAT(SubPanelPadding);
	VOXELUI_LAYOUT_FLOAT(SubPanelSeparation);
	VOXELUI_LAYOUT_INT(SubPanelTitleSize);
	VOXELUI_LAYOUT_INT(SubPanelBodySize);
	VOXELUI_LAYOUT_FLOAT(SaveRowMinHeight);
	VOXELUI_LAYOUT_FLOAT(SaveRowSeparation);
	VOXELUI_LAYOUT_INT(SaveRowFontSize);
	VOXELUI_LAYOUT_FLOAT(SaveRowButtonWidth);
	VOXELUI_LAYOUT_FLOAT(SaveRowButtonHeight);
	VOXELUI_LAYOUT_INT(SaveRowButtonFont);
	VOXELUI_LAYOUT_FLOAT(DialogButtonWidth);
	VOXELUI_LAYOUT_FLOAT(DialogButtonHeight);

	VOXELUI_LAYOUT_FLOAT(LoadingColumnWidth);
	VOXELUI_LAYOUT_FLOAT(LoadingColumnHeight);
	VOXELUI_LAYOUT_FLOAT(LoadingSeparation);
	VOXELUI_LAYOUT_FLOAT(HourglassWrapWidth);
	VOXELUI_LAYOUT_FLOAT(HourglassWrapHeight);
	VOXELUI_LAYOUT_FLOAT(HourglassWidth);
	VOXELUI_LAYOUT_FLOAT(HourglassHeight);
	VOXELUI_LAYOUT_FLOAT(HourglassOffsetY);
	VOXELUI_LAYOUT_INT(LoadingTitleSize);
	VOXELUI_LAYOUT_INT(LoadingQuipSize);
	VOXELUI_LAYOUT_FLOAT(LoadingQuipMinHeight);
	VOXELUI_LAYOUT_FLOAT(LoadingBarWidth);
	VOXELUI_LAYOUT_FLOAT(LoadingBarHeight);
	VOXELUI_LAYOUT_INT(LoadingPctSize);
	VOXELUI_LAYOUT_INT(LoadingTipSize);
	VOXELUI_LAYOUT_FLOAT(LoadingTipInsetX);
	VOXELUI_LAYOUT_FLOAT(LoadingTipInsetBottom);
	VOXELUI_LAYOUT_FLOAT(LoadingTintAlpha);
	VOXELUI_LAYOUT_INT(FpsFontSize);

	VOXELUI_LAYOUT_FLOAT(FadeDuration);
	VOXELUI_LAYOUT_FLOAT(BackgroundRotate);
	VOXELUI_LAYOUT_FLOAT(BackgroundFade);
	VOXELUI_LAYOUT_FLOAT(QuipRotate);
	VOXELUI_LAYOUT_FLOAT(QuipFade);
	VOXELUI_LAYOUT_FLOAT(TipRotate);
	VOXELUI_LAYOUT_FLOAT(MusicFadeOut);
	VOXELUI_LAYOUT_FLOAT(MaxAnimationDelta);
	VOXELUI_LAYOUT_FLOAT(HourglassBobPeriod);
	VOXELUI_LAYOUT_FLOAT(HourglassBobPixels);
	VOXELUI_LAYOUT_FLOAT(FpsRefreshInterval);

#undef VOXELUI_LAYOUT_FLOAT
#undef VOXELUI_LAYOUT_INT

	if (OverrideCount > 0)
	{
		// Silent when there is no ini, loud when there is: a menu that looks
		// wrong on one machine and right on another is otherwise a long
		// afternoon.
		UE_LOG(LogVoxelUI, Log, TEXT("FVoxelMenuLayout: %d value(s) overridden from DefaultVoxelUI.ini."), OverrideCount);
	}
	return L;
}
} // namespace VoxelUILayoutDetail

const FVoxelMenuLayout& FVoxelMenuLayout::Get()
{
	static const FVoxelMenuLayout Layout = VoxelUILayoutDetail::Load();
	return Layout;
}
