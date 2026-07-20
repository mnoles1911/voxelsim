#include "VoxelEarthHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelWorldSubsystem.h"

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

	// docs/debug-tooling-plan.md P1 "Perf HUD": mode >= 1 (mode 2 additionally
	// activates the 3D visualization layers, handled in VoxelWorldSubsystem).
	if (VoxelDebug::GetDebugMode() >= 1)
	{
		DrawPerfHUD();
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

void AVoxelEarthHUD::DrawPerfHUD()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>();
	if (!Subsystem)
	{
		return;
	}

	// docs/debug-tooling-plan.md P1 "Perf HUD": "1Hz refresh of text,
	// per-frame collection" -- the subsystem already collects per-frame and
	// only republishes FVoxelPerfSnapshot at 1Hz, so this only needs to
	// rebuild the formatted string on the same cadence; DrawText itself still
	// runs every frame from the cached string.
	const float NowSeconds = World->GetTimeSeconds();
	if (CachedPerfHUDText.IsEmpty() || (NowSeconds - PerfHUDLastRefreshWorldSeconds) >= PerfRefreshIntervalSeconds)
	{
		PerfHUDLastRefreshWorldSeconds = NowSeconds;
		const FVoxelPerfSnapshot Snap = Subsystem->GetPerfSnapshot();

		// M2 item 1: "Per-level loaded/pending counters into the perf
		// snapshot/HUD (extend the P1 HUD rows minimally)" -- one extra line,
		// "R<level> loaded/pending" pairs.
		FString RingsLine = TEXT("Rings:");
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			RingsLine += FString::Printf(TEXT("  R%d %d/%d"), Level, Snap.LevelLoadedCount[Level], Snap.LevelPendingCount[Level]);
		}

		// M2 wave 2 item 1 ("Cross-job mip caching"): per-level worker ms
		// row -- this is the number the shared mip cache targets (wave 1
		// measured worker p95 ~296ms on high-level jobs).
		FString LevelWorkerMsLine = TEXT("Worker ms/level:");
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			LevelWorkerMsLine +=
				FString::Printf(TEXT("  R%d %.1f/%.1f"), Level, Snap.LevelWorkerMsP50[Level], Snap.LevelWorkerMsP95[Level]);
		}

		CachedPerfHUDText = FString::Printf(
			TEXT("voxel.Debug %d -- F3 to cycle\n")
			TEXT("Streaming: loaded %lld (%.1f/s)  unloaded %lld (%.1f/s)\n")
			TEXT("  jobs %d/%d  queues job=%d gt=%d unload=%d\n")
			TEXT("  budget sat %.0f%%  stale discards %lld\n")
			TEXT("%s\n")
			TEXT("Worker ms: p50 %.2f  p95 %.2f  max %.2f\n")
			TEXT("%s (p50/p95)\n")
			TEXT("Memory: components %d  quads %lld  overlay bricks %lld  edit log %lld\n")
			TEXT("  mip cache: bricks %lld  ~%.1f MB  evictions %lld\n")
			TEXT("Frame: subsystem tick %.2fms  frame %.2fms\n")
			TEXT("Counters: bricks %llu  cells %llu  quads %llu  edits %llu  columns %llu"),
			VoxelDebug::GetDebugMode(),
			(long long)Snap.TotalChunksLoaded, Snap.ChunksLoadedPerSec, (long long)Snap.TotalChunksUnloaded, Snap.ChunksUnloadedPerSec,
			Snap.JobsInFlight, Snap.JobsInFlightCap, Snap.PendingJobQueueDepth, Snap.PendingGameThreadQueueDepth, Snap.PendingUnloadQueueDepth,
			Snap.BudgetSaturationPct, (long long)Snap.StaleResultsDiscarded,
			*RingsLine,
			Snap.WorkerMsP50, Snap.WorkerMsP95, Snap.WorkerMsMax,
			*LevelWorkerMsLine,
			Snap.ResidentComponents, (long long)Snap.ResidentQuads, (long long)Snap.OverlayBrickCount, (long long)Snap.EditLogEntries,
			(long long)Snap.MipCacheBrickCount, double(Snap.MipCacheBytes) / (1024.0 * 1024.0), (long long)Snap.MipCacheEvictions,
			Snap.SubsystemTickMs, World->GetDeltaSeconds() * 1000.0,
			Snap.BricksGenerated, Snap.CellsWritten, Snap.QuadsEmitted, Snap.EditsApplied, Snap.ColumnEvals);

		// W2 (docs/debug-tooling-plan.md P3 "Water" row): active bricks,
		// total volume, steps/s, replicated bytes/s -- task item 3's HUD
		// requirement. Appended only if UVoxelWaterSubsystem exists this run
		// (it always should once W2 has landed, but this stays defensive the
		// same way every other subsystem lookup in this file is).
		if (UVoxelWaterSubsystem* WaterSubsystem = World->GetSubsystem<UVoxelWaterSubsystem>())
		{
			const FVoxelWaterPerfSnapshot WaterSnap = WaterSubsystem->GetPerfSnapshot();
			CachedPerfHUDText += FString::Printf(
				TEXT("\nWater: active bricks %lld  stored %lld  volume %llu  steps/s %.1f (last step %lld bricks)\n")
				TEXT("  reservoir cells %d  replicated %.2f KB/s  tick %.2fms"),
				WaterSnap.ActiveBricks, WaterSnap.StoredBricks, (unsigned long long)WaterSnap.TotalVolume, WaterSnap.StepsPerSec,
				WaterSnap.LastSteppedBrickCount, WaterSnap.ReservoirCells, WaterSnap.ReplicatedBytesPerSec / 1024.0, WaterSnap.TickMs);
		}
	}

	TArray<FString> Lines;
	CachedPerfHUDText.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.55f), PerfPanelMarginPx, PerfPanelMarginPx, 620.f,
	         PerfPanelLineHeightPx * float(Lines.Num()) + 8.f);

	float LineY = PerfPanelMarginPx + 4.f;
	for (const FString& Line : Lines)
	{
		DrawText(Line, FLinearColor(0.2f, 1.f, 0.3f, 1.f), PerfPanelMarginPx + 6.f, LineY, nullptr, 1.f, false);
		LineY += PerfPanelLineHeightPx;
	}
}
