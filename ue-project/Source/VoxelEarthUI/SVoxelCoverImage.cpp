#include "SVoxelCoverImage.h"

#include "Rendering/DrawElements.h"

void SVoxelCoverImage::Construct(const FArguments& InArgs)
{
	BrushAttribute = InArgs._Brush;
	OpacityAttribute = InArgs._Opacity;
	// The overflow has to be cropped, not drawn over the neighbours.
	SetClipping(EWidgetClipping::ClipToBounds);
}

int32 SVoxelCoverImage::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
                                const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
                                int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FSlateBrush* Brush = BrushAttribute.Get();
	const float Opacity = FMath::Clamp(OpacityAttribute.Get(), 0.f, 1.f);
	if (Brush == nullptr || Opacity <= 0.f)
	{
		return LayerId;
	}

	const FVector2D BoxSize = AllottedGeometry.GetLocalSize();
	const FVector2D ImageSize = FVector2D(Brush->ImageSize);
	if (BoxSize.X <= 0.f || BoxSize.Y <= 0.f || ImageSize.X <= 0.f || ImageSize.Y <= 0.f)
	{
		return LayerId;
	}

	// COVER: the larger of the two axis ratios, so neither axis is left short.
	// (ScaleToFit would take the smaller and letterbox.)
	const double Scale = FMath::Max(BoxSize.X / ImageSize.X, BoxSize.Y / ImageSize.Y);
	const FVector2D DrawSize = ImageSize * Scale;
	// Centred, so the crop takes equal amounts off both sides -- which is what
	// assets/menu_backgrounds/README.md's framing rule assumes ("subject in the
	// middle two-thirds, below the top quarter").
	const FVector2D Offset = (BoxSize - DrawSize) * 0.5;

	FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint();
	Tint.A *= Opacity;

	FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
	                           AllottedGeometry.ToPaintGeometry(DrawSize, FSlateLayoutTransform(Offset)), Brush,
	                           ESlateDrawEffect::None, Tint);
	return LayerId;
}
