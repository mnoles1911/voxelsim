#include "SVoxelOverlayChrome.h"

#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

namespace VoxelOverlayChromeDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// A flat box in one colour. Every rule, ring, fill and plate below is one of
// these; the idiom is FVoxelUIStyle's white brush under a tint (see that file).
TSharedRef<SWidget> Box(const FSlateColor& Colour)
{
	return SNew(SImage).Image(FVoxelUIStyle::Get().SolidWhite()).ColorAndOpacity(Colour);
}
} // namespace VoxelOverlayChromeDetail

namespace VoxelOverlayChrome
{
TSharedRef<SWidget> Panel(TSharedRef<SWidget> Content, float Width, float Height, const FMargin& Padding)
{
	using namespace VoxelUITheme;
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// THE INSET RINGS ARE STACKED OUTSIDE IN, and the order is the CSS's.
	// `border:2px solid #000` is the outermost band; the two inset box-shadows
	// are painted inside it, the 1 px --leather-edge one ON TOP of the 3 px
	// --leather-1 one, so what a reader sees from the edge inward is 2 px black,
	// 1 px edge, 2 px leather, then the panel. Each ring is therefore a slot
	// padded by the sum of the bands outside it.
	const float Band0 = L.OverlayBorderPx;                      // black
	const float Band1 = Band0 + L.OverlayEdgeRingPx;            // --leather-edge
	const float Band2 = Band0 + L.OverlayInnerRingPx;           // --leather-1
	const FSlateColor PanelFill(
		Tint(Mix(OverlayPanelTop, OverlayPanelBottom), OverlayPanelAlpha));

	TSharedRef<SOverlay> Stack =
		SNew(SOverlay)
		// The drop shadow (0 8px 24px rgba(0,0,0,.8)), drawn the way
		// SVoxelMainMenu::WrapInPanelFrame draws the oak panel's: a black rect
		// inflated by the blur radius and shifted down by the offset. Hard-edged
		// where the CSS is soft; the note at that call site is the same note.
		+ SOverlay::Slot()
		.Padding(FMargin(-L.OverlayShadowSize, L.OverlayShadowOffsetY - L.OverlayShadowSize, -L.OverlayShadowSize,
		                 -(L.OverlayShadowSize + L.OverlayShadowOffsetY)))
		[
			VoxelOverlayChromeDetail::Box(FSlateColor(FLinearColor(0.f, 0.f, 0.f, 0.8f)))
		]
		+ SOverlay::Slot()
		[
			VoxelOverlayChromeDetail::Box(FSlateColor(FLinearColor::Black))
		]
		+ SOverlay::Slot().Padding(FMargin(Band0))
		[
			VoxelOverlayChromeDetail::Box(FSlateColor(Tint(LeatherEdge)))
		]
		+ SOverlay::Slot().Padding(FMargin(Band1))
		[
			VoxelOverlayChromeDetail::Box(FSlateColor(Tint(Leather1)))
		]
		+ SOverlay::Slot().Padding(FMargin(Band2))
		[
			VoxelOverlayChromeDetail::Box(PanelFill)
		]
		+ SOverlay::Slot().Padding(Padding + FMargin(Band2))
		[
			Content
		];

	TSharedRef<SBox> Sized = SNew(SBox).WidthOverride(Width);
	if (Height > 0.f)
	{
		Sized->SetHeightOverride(Height);
	}
	Sized->SetContent(Stack);
	return Sized;
}

TSharedRef<SWidget> Title(const FText& Text, int32 FontSize, int32 LetterSpacing)
{
	FSlateFontInfo Font = FVoxelUIStyle::Get().Serif(FontSize);
	Font.LetterSpacing = LetterSpacing;
	return SNew(STextBlock)
		.Text(Text)
		.Font(Font)
		.ColorAndOpacity(FVoxelUIStyle::TitleColour())
		.Justification(ETextJustify::Center)
		.ShadowOffset(FVector2D(2.f, 2.f))
		.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f));
}

TSharedRef<SWidget> Rule(float Alpha)
{
	using namespace VoxelUITheme;
	return SNew(SBox).HeightOverride(1.f)
		[
			VoxelOverlayChromeDetail::Box(FSlateColor(Tint(LeatherEdge, Alpha)))
		];
}

TSharedRef<SWidget> SectionHeader(const FText& Text)
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	FSlateFontInfo Font = Style.Serif(L.SettingsSectionSize);
	Font.LetterSpacing = L.SettingsSectionLetterSpacing;

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Text)
			.Font(Font)
			.ColorAndOpacity(FSlateColor(Tint(WarmPrimary)))
			.ShadowOffset(FVector2D(1.f, 1.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(10.f, 0.f, 0.f, 0.f))
		[
			Rule(0.45f)
		];
}

TSharedRef<SWidget> Diamond(float Size, const FSlateColor& Colour)
{
	return SNew(SBox)
		.WidthOverride(Size)
		.HeightOverride(Size)
		.RenderTransform(FSlateRenderTransform(FQuat2D(FMath::DegreesToRadians(45.f))))
		.RenderTransformPivot(FVector2D(0.5f, 0.5f))
		[
			VoxelOverlayChromeDetail::Box(Colour)
		];
}

TSharedRef<SWidget> KeyCap(const FText& Key)
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	// A tracked serif word on --leather-2 inside a 1 px --leather-edge rule.
	FSlateFontInfo Font = Style.Serif(L.KeyCapFontSize);
	Font.LetterSpacing = L.KeyCapLetterSpacing;

	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			VoxelOverlayChromeDetail::Box(FSlateColor(Tint(LeatherEdge)))
		]
		+ SOverlay::Slot().Padding(FMargin(1.f))
		[
			VoxelOverlayChromeDetail::Box(FSlateColor(Tint(Leather2)))
		]
		+ SOverlay::Slot().Padding(FMargin(L.KeyCapPadX, L.KeyCapPadY))
		[
			SNew(STextBlock)
			.Text(Key)
			.Font(Font)
			.ColorAndOpacity(FVoxelUIStyle::TitleColour())
		];
}

TSharedRef<SWidget> Scrim()
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	return VoxelOverlayChromeDetail::Box(FSlateColor(FLinearColor(0.f, 0.f, 0.f, L.OverlayDimAlpha)));
}
} // namespace VoxelOverlayChrome
