#pragma once
// A full-bleed background image, scaled to COVER its box and centre-cropped.
//
// WHY THIS WIDGET EXISTS. The Godot menu and loading screen both set their
// background TextureRect to EXPAND_IGNORE_SIZE + STRETCH_KEEP_ASPECT_COVERED:
// scale the image until it covers the box on BOTH axes, centre it, and let the
// overflow crop. Slate has no equivalent. SScaleBox offers ScaleToFit (which
// letterboxes -- black bars, wrong), ScaleToFill (which distorts -- stretched
// faces, worse) and the two single-axis fits (each of which is correct exactly
// half the time, depending on whether the viewport is wider or narrower than
// the image).
//
// Cover is max(BoxW/ImgW, BoxH/ImgH), and the only place that ratio is known is
// inside OnPaint, where the allotted geometry finally exists. Hence a leaf
// widget of about twenty lines rather than a wrapper built from three.
//
// It also carries the crossfade opacity, because the loading screen rotates
// backgrounds every 20 s over a 1 s fade and wants two of these stacked with
// complementary alphas.

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class VOXELEARTHUI_API SVoxelCoverImage : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelCoverImage)
		: _Opacity(1.f)
	{}
		// Re-read every paint, and allowed to be null: FVoxelUIAssetLibrary
		// returns null while a decode is in flight, and "draw nothing this
		// frame" is the correct response to that, not an error.
		SLATE_ATTRIBUTE(const FSlateBrush*, Brush)
		SLATE_ATTRIBUTE(float, Opacity)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	                      FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                      bool bParentEnabled) const override;

	// A background has no opinion about how big it wants to be; it fills
	// whatever it is given. Returning zero keeps it from influencing the
	// layout of the SOverlay it lives in.
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }

private:
	TAttribute<const FSlateBrush*> BrushAttribute;
	TAttribute<float> OpacityAttribute;
};
