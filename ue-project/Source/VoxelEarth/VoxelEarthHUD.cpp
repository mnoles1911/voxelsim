#include "VoxelEarthHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelEarthFlyPawn.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelGI.h"
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

const FLinearColor kOverlayHeader(1.0f, 0.85f, 0.25f, 1.0f);
const FLinearColor kOverlayInfo(0.75f, 0.85f, 1.0f, 1.0f);
const FLinearColor kOverlayRow(0.85f, 0.85f, 0.85f, 1.0f);
const FLinearColor kOverlaySelected(1.0f, 1.0f, 0.35f, 1.0f);
const FLinearColor kOverlayOn(0.35f, 1.0f, 0.45f, 1.0f);
const FLinearColor kOverlayWarn(1.0f, 0.45f, 0.35f, 1.0f);

FString OnOff(bool bOn)
{
	return bOn ? TEXT("ON") : TEXT("off");
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

	// Usability task: F1 overlay (default OFF -- see the header) and the
	// always-on walk/fly mode line.
	if (bOverlayVisible)
	{
		DrawDebugOverlay();
	}
	DrawModeLine();

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
			TEXT("  component pool: pooled %d  reuses %.1f/s (total %lld)\n")
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
			Snap.PooledComponents, Snap.PoolReusesPerSec, (long long)Snap.TotalPoolReuses,
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

// ============================================================================
// In-game debug overlay (usability task)
// ============================================================================

AVoxelEarthFlyPawn* AVoxelEarthHUD::GetVoxelPawn() const
{
	return PlayerOwner ? Cast<AVoxelEarthFlyPawn>(PlayerOwner->GetPawn()) : nullptr;
}

void AVoxelEarthHUD::ToggleDebugOverlay()
{
	bOverlayVisible = !bOverlayVisible;
}

void AVoxelEarthHUD::MoveOverlaySelection(int32 Delta)
{
	const int32 Count = (int32)EOverlayRow::Count;
	const int32 Next = (((int32)OverlaySelection + Delta) % Count + Count) % Count;
	OverlaySelection = (EOverlayRow)Next;
}

void AVoxelEarthHUD::AdjustOverlaySelection(int32 Delta)
{
	switch (OverlaySelection)
	{
	case EOverlayRow::MovementMode:
		if (AVoxelEarthFlyPawn* Pawn = GetVoxelPawn())
		{
			Pawn->SetWalkMode(!Pawn->IsWalkMode());
		}
		break;

	case EOverlayRow::FlySpeed:
		if (AVoxelEarthFlyPawn* Pawn = GetVoxelPawn())
		{
			// Mode-aware, matching what this row displays: the walk gait tier
			// or the fly speed step.
			Pawn->AdjustSpeedDial(Delta >= 0 ? +1 : -1);
		}
		break;

	case EOverlayRow::PlayerBox:
		// No mode>=2 gate to work around, so this reads the same value it sets.
		VoxelDebug::SetPlayerBoxEnabled(!VoxelDebug::IsPlayerBoxEnabled());
		break;

	case EOverlayRow::DebugMode:
		// 0 -> 1 -> 2 -> 0, the same cycle F3 drives; Left steps backwards so
		// the row behaves like every other value row.
		VoxelDebug::SetDebugMode((VoxelDebug::GetDebugMode() + (Delta >= 0 ? 1 : 2)) % 3);
		break;

	case EOverlayRow::ChunkStates:
		VoxelDebug::SetChunkStatesEnabled(!VoxelDebug::GetChunkStatesCVar());
		break;

	case EOverlayRow::Bounds:
		VoxelDebug::SetBoundsEnabled(!VoxelDebug::GetBoundsCVar());
		break;

	case EOverlayRow::Rings:
		VoxelDebug::SetRingsEnabled(!VoxelDebug::GetRingsCVar());
		break;

	case EOverlayRow::GlobalIllumination:
		// voxel.GI.Enabled lives in VoxelGI.cpp and only exposes a getter
		// (VoxelGI::IsEnabled), so drive the cvar itself -- still the one
		// shared source of truth, not a parallel flag.
		if (IConsoleVariable* GICVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GI.Enabled")))
		{
			GICVar->Set(VoxelGI::IsEnabled() ? 0 : 1, ECVF_SetByCode);
		}
		break;

	case EOverlayRow::Wireframe:
		// Show flags have no console read-back, hence the tracked bool (see
		// the header). The voxel chunk/water scene proxies already honour
		// EngineShowFlags.Wireframe, so this costs nothing to support.
		bWireframeRequested = !bWireframeRequested;
		if (PlayerOwner)
		{
			PlayerOwner->ConsoleCommand(bWireframeRequested ? TEXT("ShowFlag.Wireframe 1") : TEXT("ShowFlag.Wireframe 0"),
			                            /*bWriteToLog*/ false);
		}
		break;

	case EOverlayRow::Count:
	default:
		break;
	}
}

void AVoxelEarthHUD::DrawOverlayInfo(const FString& Text, const FLinearColor& Color, float PanelX, float& InOutY)
{
	DrawText(Text, Color, PanelX + 8.f, InOutY, nullptr, 1.f, false);
	InOutY += OverlayLineHeightPx;
}

void AVoxelEarthHUD::DrawOverlayRow(EOverlayRow Row, const FString& Label, const FString& Value, float PanelX, float& InOutY)
{
	const bool bSelected = (Row == OverlaySelection);
	const FLinearColor Color = bSelected ? kOverlaySelected : kOverlayRow;
	if (bSelected)
	{
		// Selection band: a filled row behind the text reads instantly at a
		// glance, which a colour change alone does not over a busy scene.
		DrawRect(FLinearColor(0.25f, 0.25f, 0.05f, 0.75f), PanelX + 4.f, InOutY - 1.f, OverlayPanelWidthPx - 8.f,
		         OverlayLineHeightPx);
	}
	DrawText(FString::Printf(TEXT("%s %s"), bSelected ? TEXT(">") : TEXT(" "), *Label), Color, PanelX + 8.f, InOutY, nullptr, 1.f,
	         false);
	DrawText(Value, Color, PanelX + OverlayValueColumnPx, InOutY, nullptr, 1.f, false);
	InOutY += OverlayLineHeightPx;
}

void AVoxelEarthHUD::DrawDebugOverlay()
{
	UWorld* World = GetWorld();
	if (!World || !Canvas)
	{
		return;
	}
	UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>();
	AVoxelEarthFlyPawn* Pawn = GetVoxelPawn();

	// Top-right, so it never overlaps the mode>=1 perf panel (top-left, 620px
	// wide) or the dig/place text block (bottom-left).
	const float PanelX = FMath::Max(OverlayMarginPx, Canvas->SizeX - OverlayPanelWidthPx - OverlayMarginPx);
	// Row budget with a little slack: an over-estimate is invisible, an
	// under-estimate clips text outside the panel. Raised 28 -> 29 when the
	// Player volume row was added; the worst-case path (real tiles + bounding
	// box known, debug mode >= 1) now emits 28 lines plus 16px of spacers, which
	// the old budget was 4px short of covering.
	const float PanelHeight = OverlayLineHeightPx * 29.f + 12.f;
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.70f), PanelX, OverlayMarginPx, OverlayPanelWidthPx, PanelHeight);

	float Y = OverlayMarginPx + 5.f;
	DrawOverlayInfo(TEXT("VOXEL DEBUG OVERLAY   (F1 close)"), kOverlayHeader, PanelX, Y);
	DrawOverlayInfo(TEXT("Up/Down select   Left/Right/Enter change"), kOverlayInfo, PanelX, Y);
	Y += 4.f;

	// --- Where am I ---------------------------------------------------------

	FVector Pos = FVector::ZeroVector;
	if (Pawn)
	{
		Pos = Pawn->GetActorLocation();
	}
	else if (PlayerOwner && PlayerOwner->PlayerCameraManager)
	{
		Pos = PlayerOwner->PlayerCameraManager->GetCameraLocation();
	}

	DrawOverlayInfo(FString::Printf(TEXT("Pos     %.1f, %.1f, %.1f m"), Pos.X / 100.0, Pos.Y / 100.0, Pos.Z / 100.0), kOverlayInfo,
	                PanelX, Y);
	DrawOverlayInfo(FString::Printf(TEXT("Voxel   %lld, %lld, %lld"),
	                                 (long long)FMath::FloorToDouble(Pos.X / VoxelCoords::VoxelSizeUU),
	                                 (long long)FMath::FloorToDouble(Pos.Y / VoxelCoords::VoxelSizeUU),
	                                 (long long)FMath::FloorToDouble(Pos.Z / VoxelCoords::VoxelSizeUU)),
	                kOverlayInfo, PanelX, Y);

	if (Subsystem)
	{
		const double SurfaceUU = Subsystem->GetSurfaceHeightUU(Pos.X, Pos.Y);
		DrawOverlayInfo(FString::Printf(TEXT("Surface %.1f m   altitude %+.1f m"), SurfaceUU / 100.0, (Pos.Z - SurfaceUU) / 100.0),
		                kOverlayInfo, PanelX, Y);
	}

	// Which diffusion tile the pawn is standing in -- the tiles on disk are
	// named "<x>_<y>.vxtl", so this is directly checkable against the tile dir.
	{
		int32 TileX = 0, TileY = 0;
		int64 PixelX = 0, PixelY = 0;
		VoxelDebug::WorldToTileCoords(Pos.X, Pos.Y, TileX, TileY, PixelX, PixelY);
		DrawOverlayInfo(
			FString::Printf(TEXT("Tile    %d_%d   (tile px %lld, %lld)"), TileX, TileY, (long long)PixelX, (long long)PixelY),
			kOverlayInfo, PanelX, Y);
	}

	// --- Tile source: real diffusion tiles, or the synthetic sampler? -------
	//
	// Worth the panel space on its own: a wrong -VoxelSeed / -VoxelTileScale /
	// -VoxelTileDir silently boots a plausible-looking synthetic world, and
	// that has cost this project hours more than once.
	{
		const VoxelDebug::FTileSourceStatus Tiles = VoxelDebug::GetTileSourceStatus();
		if (!Tiles.bKnown)
		{
			DrawOverlayInfo(TEXT("Source  SYNTHETIC SAMPLER (no -VoxelTileDir)"), kOverlayWarn, PanelX, Y);
		}
		else if (Tiles.bUsingRealTiles)
		{
			DrawOverlayInfo(
				FString::Printf(TEXT("Source  REAL TILES  loaded=%d rejected=%d"), Tiles.TilesLoaded, Tiles.TilesRejected),
				Tiles.TilesRejected > 0 ? kOverlayWarn : kOverlayOn, PanelX, Y);
			if (Tiles.bBoxKnown)
			{
				DrawOverlayInfo(FString::Printf(TEXT("        tiles x=[%d,%d] y=[%d,%d]"), Tiles.MinTileX, Tiles.MaxTileX,
				                                 Tiles.MinTileY, Tiles.MaxTileY),
				                kOverlayInfo, PanelX, Y);
			}
		}
		else
		{
			DrawOverlayInfo(FString::Printf(TEXT("Source  SYNTHETIC (tile load FAILED, rejected=%d)"), Tiles.TilesRejected),
			                kOverlayWarn, PanelX, Y);
		}
	}

	// --- Streaming ----------------------------------------------------------

	if (Subsystem)
	{
		// The subsystem only REFRESHES FVoxelPerfSnapshot while voxel.Debug >= 1
		// (a deliberate "zero cost at mode 0" gate in TickStreaming), so at
		// mode 0 these rows would be permanently stale zeros. Say so instead of
		// showing a confident, wrong "R0 0/0" -- and point at the fix, which is
		// the very next row of this same overlay.
		if (VoxelDebug::GetDebugMode() >= 1)
		{
			const FVoxelPerfSnapshot Snap = Subsystem->GetPerfSnapshot();
			FString RingsLine = TEXT("Rings  ");
			for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
			{
				RingsLine += FString::Printf(TEXT(" R%d %d/%d"), Level, Snap.LevelLoadedCount[Level], Snap.LevelPendingCount[Level]);
			}
			DrawOverlayInfo(RingsLine + TEXT("   (loaded/pending)"), kOverlayInfo, PanelX, Y);
			DrawOverlayInfo(FString::Printf(TEXT("Jobs    %d/%d in flight   queues %d/%d/%d"), Snap.JobsInFlight,
			                                 Snap.JobsInFlightCap, Snap.PendingJobQueueDepth, Snap.PendingGameThreadQueueDepth,
			                                 Snap.PendingUnloadQueueDepth),
			                kOverlayInfo, PanelX, Y);
		}
		else
		{
			DrawOverlayInfo(TEXT("Rings   (set voxel.Debug 1 below -- snapshot is"), kOverlayWarn, PanelX, Y);
			DrawOverlayInfo(TEXT("Jobs     only collected at mode >= 1)"), kOverlayWarn, PanelX, Y);
		}

		// Per-position residency -- the same query walk mode's terrain-ready
		// gate uses, so "why am I hovering" is answerable from the overlay.
		bool bTracked = false, bHasComponent = false;
		int32 Quads = 0;
		const bool bFound = Subsystem->DebugChunkStatusAt(Pos, bTracked, bHasComponent, Quads);
		DrawOverlayInfo(FString::Printf(TEXT("Here    %s  tracked=%s  component=%s  quads=%d"),
		                                 bFound ? TEXT("chunk") : TEXT("none"), bTracked ? TEXT("y") : TEXT("n"),
		                                 bHasComponent ? TEXT("y") : TEXT("n"), Quads),
		                (bTracked && !bHasComponent) ? kOverlayWarn : kOverlayInfo, PanelX, Y);
	}

	DrawOverlayInfo(FString::Printf(TEXT("Veil    %s (run switch -VoxelUndergroundVeil)"),
	                                 VoxelDebug::IsUndergroundVeilEnabledForRun() ? TEXT("enabled") : TEXT("DISABLED")),
	                kOverlayInfo, PanelX, Y);

	// --- Movement state -----------------------------------------------------

	if (Pawn)
	{
		FString StateText;
		if (Pawn->IsWalkMode())
		{
			StateText = Pawn->IsWaitingForTerrain() ? TEXT("WAITING FOR TERRAIN (holding position)")
			            : Pawn->IsSwimmingNow()     ? TEXT("swimming")
			            : Pawn->IsGroundedNow()     ? TEXT("grounded")
			                                        : TEXT("airborne");
			if (Pawn->IsCrouched())
			{
				// A crouch that outlives the key is not a stuck input -- it is
				// the auto-stand waiting for headroom. Say which, or the player
				// reasonably concludes the key stopped working.
				StateText += Pawn->IsCrouchBlocked() ? TEXT("  + CROUCHED (no room to stand)") : TEXT("  + crouched");
			}
			if (Pawn->IsSprintEngaged())
			{
				StateText += TEXT("  + sprinting");
			}
		}
		else
		{
			StateText = TEXT("flying (no collision: clips terrain, water, debris)");
		}
		DrawOverlayInfo(FString::Printf(TEXT("State   %s"), *StateText),
		                Pawn->IsWaitingForTerrain() ? kOverlayWarn
		                : Pawn->IsCrouchBlocked()   ? kOverlayWarn
		                                            : kOverlayInfo,
		                PanelX, Y);
		DrawOverlayInfo(FString::Printf(TEXT("Camera  %s%s"),
		                                Pawn->IsThirdPerson() ? TEXT("third person") : TEXT("first person"),
		                                Pawn->IsThirdPerson() ? (Pawn->IsRightShoulder() ? TEXT(" (right shoulder)")
		                                                                                 : TEXT(" (left shoulder)"))
		                                                      : TEXT("")),
		                kOverlayInfo, PanelX, Y);
	}

	Y += 6.f;
	DrawOverlayInfo(TEXT("--- settings ---"), kOverlayHeader, PanelX, Y);

	// --- Selectable rows ----------------------------------------------------

	DrawOverlayRow(EOverlayRow::MovementMode, TEXT("Movement mode  (G)"),
	               Pawn ? (Pawn->IsWalkMode() ? TEXT("WALK") : TEXT("FLY")) : TEXT("(no pawn)"), PanelX, Y);

	// One row, whichever speed control the current mode owns -- the mouse wheel
	// drives both, so showing the inactive one would be actively misleading.
	FString SpeedValue = TEXT("(no pawn)");
	FString SpeedLabel = TEXT("Fly speed      (wheel)");
	if (Pawn)
	{
		if (Pawn->IsWalkMode())
		{
			SpeedLabel = TEXT("Gait           (wheel)");
			SpeedValue = FString::Printf(TEXT("%s  %d/%d  %.1f m/s%s"), Pawn->GetSpeedTierName(),
			                              Pawn->GetSpeedTierIndex() + 1, AVoxelEarthFlyPawn::GetSpeedTierCount(),
			                              Pawn->GetEffectiveWalkSpeedUU() / 100.0,
			                              Pawn->IsSprintEngaged()  ? TEXT("  (sprint)")
			                              : Pawn->IsCrouched()     ? TEXT("  (crouch capped)")
			                                                       : TEXT(""));
		}
		else
		{
			SpeedValue = FString::Printf(TEXT("step %d/%d  %.2f m/s%s"), Pawn->GetFlySpeedIndex() + 1,
			                              AVoxelEarthFlyPawn::GetFlySpeedStepCount(), Pawn->GetEffectiveFlySpeedUU() / 100.0,
			                              Pawn->IsFlyBoostActive() ? TEXT(" (boost)") : TEXT(""));
		}
	}
	DrawOverlayRow(EOverlayRow::FlySpeed, SpeedLabel, SpeedValue, PanelX, Y);

	// Deliberately carries NO "(needs voxel.Debug 2)" suffix -- unlike the three
	// layer rows below, this one is live at any debug mode and defaults on.
	DrawOverlayRow(EOverlayRow::PlayerBox, TEXT("Player volume  (walk)"),
	               FString::Printf(TEXT("%s  box + eye line + ground cells"), *OnOff(VoxelDebug::IsPlayerBoxEnabled())),
	               PanelX, Y);

	DrawOverlayRow(EOverlayRow::DebugMode, TEXT("voxel.Debug    (F3)"),
	               FString::Printf(TEXT("%d  (0 off, 1 perf HUD, 2 +layers)"), VoxelDebug::GetDebugMode()), PanelX, Y);

	// The three layer rows show the CVAR value and, when mode < 2 is holding
	// them back, say so -- otherwise flipping one at mode 1 looks broken.
	const TCHAR* Gated = (VoxelDebug::GetDebugMode() >= 2) ? TEXT("") : TEXT("  (needs voxel.Debug 2)");
	DrawOverlayRow(EOverlayRow::ChunkStates, TEXT("  Chunk states"),
	               FString::Printf(TEXT("%s%s"), *OnOff(VoxelDebug::GetChunkStatesCVar()), Gated), PanelX, Y);
	DrawOverlayRow(EOverlayRow::Bounds, TEXT("  Chunk bounds"),
	               FString::Printf(TEXT("%s%s"), *OnOff(VoxelDebug::GetBoundsCVar()), Gated), PanelX, Y);
	DrawOverlayRow(EOverlayRow::Rings, TEXT("  Ring level tint"),
	               FString::Printf(TEXT("%s%s"), *OnOff(VoxelDebug::GetRingsCVar()), Gated), PanelX, Y);

	DrawOverlayRow(EOverlayRow::GlobalIllumination, TEXT("Voxel GI"), OnOff(VoxelGI::IsEnabled()), PanelX, Y);
	DrawOverlayRow(EOverlayRow::Wireframe, TEXT("Wireframe"), OnOff(bWireframeRequested), PanelX, Y);

	Y += 6.f;
	DrawOverlayInfo(TEXT("WASD move   Space jump/up   Wheel speed   Shift sprint   Alt slow"), kOverlayInfo, PanelX, Y);
	DrawOverlayInfo(TEXT("G walk/fly   C crouch   V camera   Q shoulder   T material"), kOverlayInfo, PanelX, Y);
	DrawOverlayInfo(TEXT("LMB dig   RMB place   1/2/3 dig size   F hold-throw   F3 debug"), kOverlayInfo, PanelX, Y);
}

void AVoxelEarthHUD::DrawModeLine()
{
	// Never in a headless verification capture (see the header). The overlay
	// above is default-off and key-driven so it needs no such guard, but this
	// line is always on and would otherwise be new pixels in every fixture
	// screenshot.
	if (VoxelDebug::IsUnattendedFixtureRun() || !Canvas)
	{
		return;
	}
	const AVoxelEarthFlyPawn* Pawn = GetVoxelPawn();
	if (!Pawn)
	{
		return;
	}

	FString Text;
	FLinearColor Color = FLinearColor::White;
	if (Pawn->IsWalkMode())
	{
		if (Pawn->IsWaitingForTerrain())
		{
			Text = TEXT("WALK - waiting for terrain");
			Color = kOverlayWarn;
		}
		else if (Pawn->IsSwimmingNow())
		{
			Text = TEXT("WALK - swimming");
			Color = kOverlayOn;
		}
		else
		{
			// The gait tier is the thing the wheel just changed, so it belongs
			// on the always-on line rather than only in the F1 overlay.
			Text = FString::Printf(TEXT("WALK - %s %.1f m/s%s"), Pawn->GetSpeedTierName(),
			                        Pawn->GetEffectiveWalkSpeedUU() / 100.0,
			                        Pawn->IsCrouched()      ? TEXT(" crouched")
			                        : Pawn->IsSprintEngaged() ? TEXT(" sprint")
			                                                  : TEXT(""));
			Color = kOverlayOn;
		}
	}
	else
	{
		Text = FString::Printf(TEXT("FLY - %.1f m/s%s"), Pawn->GetEffectiveFlySpeedUU() / 100.0,
		                        Pawn->IsFlyBoostActive() ? TEXT(" boost") : TEXT(""));
		Color = kOverlayHeader;
	}
	if (!bOverlayVisible)
	{
		Text += TEXT("   (G mode, F1 debug)");
	}

	// Bottom-right, clear of the bottom-left dig/place block.
	DrawText(Text, Color, Canvas->SizeX - 380.f, Canvas->SizeY - MarginPx - LineHeightPx, nullptr, 1.f, false);
}
