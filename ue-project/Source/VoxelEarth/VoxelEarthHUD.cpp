#include "VoxelEarthHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "VoxelEarthPlayerController.h"

namespace
{
// vxc::MaterialId values (voxelcore/core.h) used by the creative placement
// palette (AVoxelEarthPlayerController::CyclePaletteMaterial): rock(2) ->
// soil/topsoil(6) -> sand(4). Kept as numeric literals here for the same
// reason as the player controller -- this HUD stays voxel-core-free.
FString PaletteMaterialName(uint8 MaterialId)
{
	switch (MaterialId)
	{
	case 2: return TEXT("Rock");
	case 6: return TEXT("Soil");
	case 4: return TEXT("Sand");
	default: return TEXT("?");
	}
}
} // namespace

void AVoxelEarthHUD::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas)
	{
		return;
	}

	// Crosshair: a small filled dot at the screen center (dig/place always
	// traces camera-through-crosshair, m1-plan.md "Cameras" row).
	const float CenterX = Canvas->SizeX * 0.5f;
	const float CenterY = Canvas->SizeY * 0.5f;
	DrawRect(FLinearColor(1.f, 1.f, 1.f, 0.9f), CenterX - CrosshairHalfSizePx, CenterY - CrosshairHalfSizePx,
	         CrosshairHalfSizePx * 2.f, CrosshairHalfSizePx * 2.f);

	const AVoxelEarthPlayerController* VoxelPC = Cast<AVoxelEarthPlayerController>(PlayerOwner);
	if (!VoxelPC)
	{
		return;
	}

	// Bottom-left text block: dig/place size and palette material.
	const int32 DigSize = VoxelPC->GetDigSizeVoxels();
	const FString DigSizeText = FString::Printf(TEXT("Dig %dx%dx%d"), DigSize, DigSize, DigSize);
	const FString MaterialText = FString::Printf(TEXT("Place: %s"), *PaletteMaterialName(VoxelPC->GetPaletteMaterialId()));

	float LineY = Canvas->SizeY - MarginPx - LineHeightPx * 2.f;
	DrawText(DigSizeText, FLinearColor::White, MarginPx, LineY, nullptr, 1.f, false);
	LineY += LineHeightPx;
	DrawText(MaterialText, FLinearColor::White, MarginPx, LineY, nullptr, 1.f, false);

	// "F charge" bar while charging (m1-plan.md HUD row), drawn just above
	// the two text lines.
	if (VoxelPC->IsChargingExplosive())
	{
		const float BarY = Canvas->SizeY - MarginPx - LineHeightPx * 2.f - ChargeBarHeightPx - 6.f;
		DrawRect(FLinearColor(0.1f, 0.1f, 0.1f, 0.6f), MarginPx, BarY, ChargeBarWidthPx, ChargeBarHeightPx);
		const float Alpha = VoxelPC->GetExplosiveChargeAlpha();
		DrawRect(FLinearColor(1.f, 0.45f, 0.05f, 0.95f), MarginPx, BarY, ChargeBarWidthPx * Alpha, ChargeBarHeightPx);
		DrawText(TEXT("F charge"), FLinearColor::White, MarginPx, BarY - LineHeightPx, nullptr, 1.f, false);
	}
}
