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

// Front-end widgets alive right now; see FVoxelUIStyle::RegisterWidget.
int32 GLiveWidgets = 0;

const TCHAR* const kSerifRelativePath = TEXT("UI/Fonts/MacondoSwashCaps-Regular.ttf");
// The 2026-09-07 mocks' other faces. The IM Fell filenames are Google Fonts'
// own (IMFeENrm28P = roman, IMFeENit28P = italic); kept as shipped rather than
// renamed so the OFL provenance stays greppable.
const TCHAR* const kMonoRelativePath = TEXT("UI/Fonts/VT323-Regular.ttf");
const TCHAR* const kHandRelativePath = TEXT("UI/Fonts/IMFeENrm28P.ttf");
const TCHAR* const kHandItalicRelativePath = TEXT("UI/Fonts/IMFeENit28P.ttf");

// One face by path, with the same graceful-degradation contract Serif has:
// returns null (and says why, once) when the file is absent or assets are
// forced off, and the accessor falls back to the engine face.
TSharedPtr<FCompositeFont> LoadFaceOrNull(const TCHAR* RelativePath, const TCHAR* Role, bool bForceFallback)
{
	const FString Path = FPaths::ProjectContentDir() / RelativePath;
	if (!bForceFallback && IFileManager::Get().FileExists(*Path))
	{
		return MakeShared<FStandaloneCompositeFont>(NAME_None, Path, EFontHinting::Default,
		                                            EFontLoadingPolicy::LazyLoad);
	}
	UE_LOG(LogVoxelUI, Warning,
	       TEXT("Menu %s font not loaded (%s); falling back to the engine default face. Expected at: %s"),
	       Role, bForceFallback ? TEXT("-VoxelUINoAssets") : TEXT("file missing"), *Path);
	return nullptr;
}

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
	// NOT A check(). A module unloading with widgets still on the viewport is
	// a real ordering bug and worth shouting about, but taking the process
	// down at shutdown -- when the thing being torn down is a menu -- would
	// turn a cosmetic risk into a crash report. The log line is what makes it
	// findable; the dangling read, if it happens at all, happens after this.
	if (VoxelUIStyleDetail::GLiveWidgets != 0)
	{
		UE_LOG(LogVoxelUI, Error,
		       TEXT("FVoxelUIStyle::Shutdown with %d front-end widget(s) still alive. SButton and SImage hold BARE ")
		       TEXT("pointers into this style, so any further paint of them reads freed memory. Something is tearing ")
		       TEXT("the module down before the viewport."),
		       VoxelUIStyleDetail::GLiveWidgets);
	}
	VoxelUIStyleDetail::GInstance.Reset();
}

void FVoxelUIStyle::RegisterWidget()
{
	++VoxelUIStyleDetail::GLiveWidgets;
}

void FVoxelUIStyle::UnregisterWidget()
{
	--VoxelUIStyleDetail::GLiveWidgets;
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

	// The three 2026-09-07 faces ride the same switch as Serif.
	MonoFont = VoxelUIStyleDetail::LoadFaceOrNull(VoxelUIStyleDetail::kMonoRelativePath, TEXT("mono"), bForceFallback);
	HandFont = VoxelUIStyleDetail::LoadFaceOrNull(VoxelUIStyleDetail::kHandRelativePath, TEXT("hand"), bForceFallback);
	HandItalicFont = VoxelUIStyleDetail::LoadFaceOrNull(VoxelUIStyleDetail::kHandItalicRelativePath,
	                                                    TEXT("hand-italic"), bForceFallback);

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

	// --- Title-screen item (transparent SButton) ----------------------------
	{
		FSlateBrush Clear = VoxelUIStyleDetail::MakeSolidBrush();
		Clear.TintColor = FSlateColor(FLinearColor::Transparent);
		CartoucheButtonStyle.SetNormal(Clear);
		CartoucheButtonStyle.SetHovered(Clear);
		CartoucheButtonStyle.SetPressed(Clear);
		CartoucheButtonStyle.SetDisabled(Clear);
		CartoucheButtonStyle.SetNormalPadding(FMargin(0.f));
		CartoucheButtonStyle.SetPressedPadding(FMargin(0.f));
	}

	// --- Audio slider (.sl-knob) --------------------------------------------
	{
		const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
		FSlateBrush Clear = VoxelUIStyleDetail::MakeSolidBrush();
		Clear.TintColor = FSlateColor(FLinearColor::Transparent);
		SliderStyle.SetNormalBarImage(Clear);
		SliderStyle.SetHoveredBarImage(Clear);
		SliderStyle.SetDisabledBarImage(Clear);

		// SSlider TAKES THE THUMB'S SIZE FROM THE BRUSH'S ImageSize, which is
		// the only reason this style knows a layout number at all.
		// --warm-highlight IS --gold; the CSS aliases them so the two families
		// cannot drift, and the port spells the alias out at the use site.
		FSlateBrush Knob = VoxelUIStyleDetail::MakeFilledBrush(Mix(Gold, WarmPrimary));
		Knob.ImageSize = FVector2D(L.SliderKnobWidth, L.SliderKnobHeight);
		SliderStyle.SetNormalThumbImage(Knob);
		// linear-gradient(180deg,--warm-highlight,--warm-primary) unchanged on
		// hover in the mock; the drag affordance is the knob's motion.
		SliderStyle.SetHoveredThumbImage(Knob);
		FSlateBrush DisabledKnob = Knob;
		DisabledKnob.TintColor = FSlateColor(Tint(IronDeep));
		SliderStyle.SetDisabledThumbImage(DisabledKnob);
		SliderStyle.SetBarThickness(L.SliderTrackHeight);
	}

	// --- Settings checkbox (.ck-box) ----------------------------------------
	{
		FSlateBrush Clear = VoxelUIStyleDetail::MakeSolidBrush();
		Clear.TintColor = FSlateColor(FLinearColor::Transparent);
		OverlayCheckBoxStyle.SetUncheckedImage(Clear);
		OverlayCheckBoxStyle.SetUncheckedHoveredImage(Clear);
		OverlayCheckBoxStyle.SetUncheckedPressedImage(Clear);
		OverlayCheckBoxStyle.SetCheckedImage(Clear);
		OverlayCheckBoxStyle.SetCheckedHoveredImage(Clear);
		OverlayCheckBoxStyle.SetCheckedPressedImage(Clear);
		OverlayCheckBoxStyle.SetUndeterminedImage(Clear);
		OverlayCheckBoxStyle.SetUndeterminedHoveredImage(Clear);
		OverlayCheckBoxStyle.SetUndeterminedPressedImage(Clear);
		OverlayCheckBoxStyle.SetBackgroundImage(Clear);
		OverlayCheckBoxStyle.SetBackgroundHoveredImage(Clear);
		OverlayCheckBoxStyle.SetBackgroundPressedImage(Clear);
		OverlayCheckBoxStyle.SetPadding(FMargin(0.f));
	}

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

FSlateFontInfo FVoxelUIStyle::Mono(int32 SizePx) const
{
	if (MonoFont.IsValid())
	{
		return FSlateFontInfo(MonoFont, SizePx);
	}
	return FCoreStyle::GetDefaultFontStyle("Mono", SizePx);
}

FSlateFontInfo FVoxelUIStyle::Hand(int32 SizePx) const
{
	if (HandFont.IsValid())
	{
		return FSlateFontInfo(HandFont, SizePx);
	}
	return FCoreStyle::GetDefaultFontStyle("Regular", SizePx);
}

FSlateFontInfo FVoxelUIStyle::HandItalic(int32 SizePx) const
{
	if (HandItalicFont.IsValid())
	{
		return FSlateFontInfo(HandItalicFont, SizePx);
	}
	return FCoreStyle::GetDefaultFontStyle("Italic", SizePx);
}

FSlateColor FVoxelUIStyle::TitleColour() { return FSlateColor(VoxelUITheme::Tint(VoxelUITheme::Gold)); }
FSlateColor FVoxelUIStyle::BodyColour() { return FSlateColor(VoxelUITheme::Tint(VoxelUITheme::Ink)); }
FSlateColor FVoxelUIStyle::DimColour() { return FSlateColor(VoxelUITheme::Tint(VoxelUITheme::InkDim)); }
FSlateColor FVoxelUIStyle::MutedColour() { return FSlateColor(VoxelUITheme::Tint(VoxelUITheme::InkMute)); }
