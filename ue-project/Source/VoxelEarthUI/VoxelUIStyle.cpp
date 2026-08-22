#include "VoxelUIStyle.h"

#include "VoxelEarthUI.h"
#include "VoxelUITheme.h"
#include "VoxelFrontEndSwitches.h"

#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Styling/CoreStyle.h"

namespace VoxelUIStyleDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

TUniquePtr<FVoxelUIStyle> GInstance;

const TCHAR* const kSerifRelativePath = TEXT("UI/Fonts/MacondoSwashCaps-Regular.ttf");

// A 1x1 opaque white box, tinted per use. Every flat colour in this front end
// is one of these -- Slate has no "solid colour" primitive, and a white brush
// under a TintColor is the idiomatic substitute (FCoreStyle does the same).
FSlateBrush MakeSolidBrush()
{
	FSlateBrush Brush;
	Brush.DrawAs = ESlateBrushDrawType::Image;
	Brush.ImageSize = FVector2D(1.f, 1.f);
	Brush.TintColor = FSlateColor(FLinearColor::White);
	return Brush;
}

// A filled box with a border, which is what every Godot StyleBoxFlat in
// UIStyles.gd is.
//
// SLATE HAS NO BORDER-PLUS-FILL BRUSH, which is the one structural difference
// between the two systems and worth stating rather than discovering. A
// StyleBoxFlat carries bg_color, border_color, border_width and
// content_margin in one object; an FSlateBrush carries a texture, a tint and a
// margin. The faithful reproduction is therefore two brushes stacked -- the
// border colour behind, the fill in front, inset by the border width -- which
// is what SVoxelMenuButton and SVoxelMainMenu do at the widget level. What
// lives here is the fill half plus the border colour recorded alongside it, so
// that the two halves cannot drift.
FSlateBrush MakeFilledBrush(const FColor& Fill)
{
	FSlateBrush Brush = MakeSolidBrush();
	Brush.TintColor = FSlateColor(VoxelUITheme::Tint(Fill));
	return Brush;
}
} // namespace VoxelUIStyleDetail

void FVoxelUIStyle::Startup()
{
	if (!VoxelUIStyleDetail::GInstance)
	{
		VoxelUIStyleDetail::GInstance = MakeUnique<FVoxelUIStyle>();
		VoxelUIStyleDetail::GInstance->Initialise();
	}
}

void FVoxelUIStyle::Shutdown()
{
	VoxelUIStyleDetail::GInstance.Reset();
}

FVoxelUIStyle& FVoxelUIStyle::Get()
{
	// Startup() runs from FVoxelEarthUIModule::StartupModule, which is before
	// any widget can exist. The lazy branch is a belt-and-braces for a caller
	// that somehow gets here first (an automation test constructing a widget
	// directly, say) rather than an expected path.
	if (!VoxelUIStyleDetail::GInstance)
	{
		Startup();
	}
	return *VoxelUIStyleDetail::GInstance;
}

void FVoxelUIStyle::Initialise()
{
	using namespace VoxelUITheme;

	WhiteBrush = VoxelUIStyleDetail::MakeSolidBrush();

	// --- Font ---------------------------------------------------------------
	//
	// THE WHOLE ASSET STORY IS THIS ONE CALL. FStandaloneCompositeFont takes a
	// filesystem path, so the front end needs no .uasset, no editor import,
	// and no cook-time asset registry entry -- which is what makes it possible
	// to build this menu at all without an editor. EFontLoadingPolicy::LazyLoad
	// keeps the 31 KB face off the critical path until the first glyph.
	const FString SerifPath = FPaths::ProjectContentDir() / VoxelUIStyleDetail::kSerifRelativePath;
	const bool bForceFallback = VoxelFrontEndSwitches::Get().bNoAssets;
	if (!bForceFallback && IFileManager::Get().FileExists(*SerifPath))
	{
		SerifFont = MakeShared<FStandaloneCompositeFont>(NAME_None, SerifPath, EFontHinting::Default,
		                                                 EFontLoadingPolicy::LazyLoad);
		bProjectFontLoaded = true;
	}
	else
	{
		// GRACEFUL DEGRADATION IS A FIRST-CLASS PATH HERE, not a courtesy. The
		// art and the font are committed files that a shallow checkout, a
		// packaging misconfiguration, or -VoxelUINoAssets can all take away,
		// and a front end that fails to draw is a game that fails to start.
		// So: fall back, say so once, and keep every control usable.
		bProjectFontLoaded = false;
		UE_LOG(LogVoxelUI, Warning,
		       TEXT("Menu font not loaded (%s); falling back to the engine default face. Expected at: %s"),
		       bForceFallback ? TEXT("-VoxelUINoAssets") : TEXT("file missing"), *SerifPath);
	}

	// --- Oak button ---------------------------------------------------------
	// UIStyles.menu_button_styles(): four StyleBoxFlat, all 2px border and
	// 10px content margin. Only the FILL is carried on the brush; the border
	// colours travel with it in SVoxelMenuButton, which stacks them.
	const FMargin ButtonPadding(10.f);
	MenuButtonStyle.SetNormal(VoxelUIStyleDetail::MakeFilledBrush(PanelOak2));
	MenuButtonStyle.SetHovered(VoxelUIStyleDetail::MakeFilledBrush(PanelOak1));
	MenuButtonStyle.SetPressed(VoxelUIStyleDetail::MakeFilledBrush(Darkened(PanelOak2, 0.15f)));
	MenuButtonStyle.SetDisabled(VoxelUIStyleDetail::MakeFilledBrush(Darkened(PanelOak2, 0.30f)));
	MenuButtonStyle.SetNormalPadding(ButtonPadding);
	MenuButtonStyle.SetPressedPadding(ButtonPadding);

	// --- Oak panel ----------------------------------------------------------
	MenuBodyPanelBrush = VoxelUIStyleDetail::MakeFilledBrush(PanelOak1);
}

FSlateFontInfo FVoxelUIStyle::Serif(int32 SizePx) const
{
	if (SerifFont.IsValid())
	{
		return FSlateFontInfo(SerifFont, SizePx);
	}
	// FCoreStyle's default face. Metrics will not match Macondo's -- expect a
	// few pixels of difference in title width and vertical centring, which is
	// R2 in docs/front-end-plan.md and is accepted rather than papered over.
	return FCoreStyle::GetDefaultFontStyle("Regular", SizePx);
}

FSlateColor FVoxelUIStyle::TitleColour() { return FSlateColor(VoxelUITheme::Tint(VoxelUITheme::Gold)); }
FSlateColor FVoxelUIStyle::BodyColour() { return FSlateColor(VoxelUITheme::Tint(VoxelUITheme::Ink)); }
FSlateColor FVoxelUIStyle::DimColour() { return FSlateColor(VoxelUITheme::Tint(VoxelUITheme::InkDim)); }
FSlateColor FVoxelUIStyle::MutedColour() { return FSlateColor(VoxelUITheme::Tint(VoxelUITheme::InkMute)); }
