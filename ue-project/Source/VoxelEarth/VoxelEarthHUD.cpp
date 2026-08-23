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
#include "VoxelMarchRenderer.h" // VoxelMarchPeekLastHoleWindow -- the streaming panel's uncovered breakdown rows
#include "VoxelSkySubsystem.h" // FVoxelSkyState + VoxelSky::MonthDayFromDayOfYear -- the overlay's geo/calendar block
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

// Season from day-of-year AND hemisphere.
//
// ASTRONOMICAL CONVENTION -- seasons run solstice-to-equinox -- and not the
// meteorological one (whole months, spring = Mar/Apr/May) because the boundaries
// below then ARE the solar declination's own extremes and zero crossings, which
// is exactly what the ephemeris this reads from is built on
// (VoxelEphemeris.h:119-124). A season named on any other convention could
// disagree with the sun the frame is actually lit by, and the overlay's whole job
// is to state what the frame used.
//
// The boundary day-of-year values are in the ephemeris's REFERENCE YEAR 2000,
// which is the year FVoxelSkyState::DayOfYear counts in and which is a LEAP year
// -- so day-of-year 79 is 20 March, not 21 (VoxelEphemeris.h:150-153, and the
// same fact is what the kDaysBeforeMonth table in VoxelSkySubsystem.cpp exists to
// carry):
//     79  = 20 Mar, March equinox
//     172 = 21 Jun, June solstice
//     265 = 22 Sep, September equinox
//     355 = 21 Dec, December solstice
//
// THE SOUTHERN HEMISPHERE IS INVERTED, AND THAT IS NOT HYPOTHETICAL HERE.
// voxel.Sky.OriginLatitudeDeg defaults to +52, but FVoxelSkyState::LatitudeDeg is
// resolved from the PLAYER's position every tick, so a long enough southward
// flight legitimately crosses the equator. Printing "summer" over a southern
// midwinter is precisely the plausible-but-wrong reading this project prefers to
// have absent. Latitude exactly 0 falls to the northern naming; the equator has
// no seasons to get wrong, so there is nothing to arbitrate.
const TCHAR* SeasonName(int32 DayOfYear, double LatitudeDeg)
{
	const int32 Doy = FMath::Clamp(DayOfYear, 0, 365);
	const bool bNorth = LatitudeDeg >= 0.0;
	if (Doy >= 79 && Doy < 172)
	{
		return bNorth ? TEXT("spring") : TEXT("autumn");
	}
	if (Doy >= 172 && Doy < 265)
	{
		return bNorth ? TEXT("summer") : TEXT("winter");
	}
	if (Doy >= 265 && Doy < 355)
	{
		return bNorth ? TEXT("autumn") : TEXT("spring");
	}
	// Wraps the year end: 355..365 and 0..78, i.e. December solstice to March
	// equinox.
	return bNorth ? TEXT("winter") : TEXT("summer");
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

	// --- FPS, sampled every frame, republished at 4 Hz ---------------------
	//
	// WALL CLOCK, not world delta: world time is dilated by slomo, frozen by
	// pause, and clamped by frame-rate smoothing, so any of those would make
	// this report a rate the editor is not running at.
	//
	// This also happens to be the readout that would have caught the editor
	// throttling PIE to exactly 3 FPS when its window lost focus -- a whole
	// session was spent reading that as a streaming bug, because the panel
	// showed only per-subsystem milliseconds and none of them looked wrong.
	{
		const double Now = FPlatformTime::Seconds();
		if (FpsLastFrameSeconds > 0.0)
		{
			const double FrameMs = (Now - FpsLastFrameSeconds) * 1000.0;
			++FpsWindowFrames;
			FpsWindowWorstMs = FMath::Max(FpsWindowWorstMs, FrameMs);

			if (FpsHistoryMs.Num() < FpsHistorySize)
			{
				FpsHistoryMs.Add(FrameMs);
			}
			else
			{
				FpsHistoryMs[FpsHistoryNext] = FrameMs;
			}
			FpsHistoryNext = (FpsHistoryNext + 1) % FpsHistorySize;
		}
		else
		{
			FpsWindowStartSeconds = Now;
		}
		FpsLastFrameSeconds = Now;

		const double WindowSec = Now - FpsWindowStartSeconds;
		if (WindowSec >= double(FpsRefreshIntervalSeconds) && FpsWindowFrames > 0)
		{
			const double Fps = double(FpsWindowFrames) / WindowSec;
			const double MeanMs = (WindowSec * 1000.0) / double(FpsWindowFrames);

			// 1% LOW, the way a benchmark means it: the mean of the slowest 1%
			// of recent frames, expressed as a frame rate. A plain average
			// hides hitches, and hitches are what this project keeps chasing --
			// a world that streams at a smooth 60 with a 300 ms stall every
			// few seconds reads as "fine" on the mean and awful in the hands.
			double OnePercentLowFps = 0.0;
			if (FpsHistoryMs.Num() >= 20)
			{
				TArray<double> Sorted = FpsHistoryMs;
				Sorted.Sort([](const double& A, const double& B) { return A > B; }); // slowest first
				const int32 Count = FMath::Max(1, Sorted.Num() / 100);
				double SumMs = 0.0;
				for (int32 I = 0; I < Count; ++I)
				{
					SumMs += Sorted[I];
				}
				const double AvgWorstMs = SumMs / double(Count);
				OnePercentLowFps = (AvgWorstMs > 0.0) ? (1000.0 / AvgWorstMs) : 0.0;

				// Frame p50/p95 for the mode-3 streaming panel, from the SAME
				// sorted history -- two indexed reads on an array this block
				// already paid to sort. Sorted is DESCENDING (slowest first),
				// so p95 -- the frame time 95% of frames beat -- sits 5% in
				// from the slow end, and p50 is the middle.
				CachedFrameP95Ms = Sorted[FMath::Clamp(int32(Sorted.Num() * 0.05f), 0, Sorted.Num() - 1)];
				CachedFrameP50Ms = Sorted[FMath::Clamp(Sorted.Num() / 2, 0, Sorted.Num() - 1)];
			}
			CachedOnePercentLowFps = OnePercentLowFps;

			CachedFpsValue = Fps;
			CachedFpsText = FString::Printf(
				TEXT("FPS %.1f  (%.1f ms)   1%% low %.1f   worst %.0f ms"),
				Fps, MeanMs, OnePercentLowFps, FpsWindowWorstMs);

			FpsWindowStartSeconds = Now;
			FpsWindowFrames = 0;
			FpsWindowWorstMs = 0.0;
		}
	}

	// Mode 3: the GPU streaming panel replaces the mode-1/2 perf text
	// entirely. It is the flight-readable readout (few rows, pass/fail
	// colours); mixing it with the eleven-line perf block would defeat that.
	// The FPS machinery above still ran, so the panel's FPS + frame rows are
	// as live at mode 3 as at mode 1.
	if (VoxelDebug::GetDebugMode() >= 3)
	{
		DrawStreamPanel();
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
		// measured worker p95 ~296ms on high-level jobs). CPU arm only, like
		// the "CPU job ms" row below it.
		FString LevelWorkerMsLine = TEXT("CPU ms/level:");
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			LevelWorkerMsLine +=
				FString::Printf(TEXT("  R%d %.1f/%.1f"), Level, Snap.LevelWorkerMsP50[Level], Snap.LevelWorkerMsP95[Level]);
		}

		CachedPerfHUDText = FString::Printf(
			TEXT("voxel.Debug %d -- F3 to cycle\n")
			// Where the streaming anchor is and how fast it is moving. The
			// anchor, not the camera: it is the point every ring radius,
			// admission cutoff and retention decision is measured from, so it
			// is what the rest of this panel is responding to.
			//
			// Speed is finite-differenced from the anchor rather than read
			// from the pawn's movement component, which is what makes it
			// non-zero during -VoxelPerfFlight legs -- those teleport the pawn
			// each tick and never update UFloatingPawnMovement::Velocity. See
			// FVoxelPerfSnapshot::AnchorSpeedMetersPerSec.
			TEXT("Anchor: X %.1f m  Y %.1f m  Z %.1f m   speed %.1f m/s\n")
			TEXT("Streaming: loaded %lld (%.1f/s)  unloaded %lld (%.1f/s)\n")
			TEXT("  jobs %d/%d  queues job=%d gt=%d unload=%d\n")
			TEXT("  budget sat %.0f%%  stale discards %lld\n")
			TEXT("%s\n")
			// TWO rows for what was one, because the one row blended two
			// different quantities: the CPU worker's SERVICE TIME (task body
			// wall, pickup to enqueue) averaged with the GPU fork's
			// submit->deliver LATENCY (queue wait included) -- which is why
			// the old "Worker ms" row could read in seconds and say nothing
			// about either path. Labelled for what each actually measures;
			// they must never be summed or compared against each other.
			TEXT("CPU job ms (worker service): p50 %.2f  p95 %.2f  max %.2f\n")
			TEXT("GPU job ms (submit->deliver, incl. queue wait): p50 %.2f  p95 %.2f  max %.2f\n")
			TEXT("%s (p50/p95)\n")
			TEXT("Memory: components %d  quads %lld  overlay bricks %lld  edit log %lld\n")
			TEXT("  mip cache: bricks %lld  ~%.1f MB  evictions %lld\n")
			TEXT("  component pool: pooled %d  reuses %.1f/s (total %lld)\n")
			TEXT("Frame: subsystem tick %.2fms  frame %.2fms\n")
			TEXT("Counters: bricks %llu  cells %llu  quads %llu  edits %llu  columns %llu"),
			VoxelDebug::GetDebugMode(),
			Snap.AnchorXMeters, Snap.AnchorYMeters, Snap.AnchorZMeters, Snap.AnchorSpeedMetersPerSec,
			(long long)Snap.TotalChunksLoaded, Snap.ChunksLoadedPerSec, (long long)Snap.TotalChunksUnloaded, Snap.ChunksUnloadedPerSec,
			Snap.JobsInFlight, Snap.JobsInFlightCap, Snap.PendingJobQueueDepth, Snap.PendingGameThreadQueueDepth, Snap.PendingUnloadQueueDepth,
			Snap.BudgetSaturationPct, (long long)Snap.StaleResultsDiscarded,
			*RingsLine,
			Snap.WorkerMsP50, Snap.WorkerMsP95, Snap.WorkerMsMax,
			Snap.GpuLatencyMsP50, Snap.GpuLatencyMsP95, Snap.GpuLatencyMsMax,
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
	         PerfPanelLineHeightPx * float(Lines.Num() + (CachedFpsText.IsEmpty() ? 0 : 1)) + 8.f);

	float LineY = PerfPanelMarginPx + 4.f;

	// THE FPS LINE IS DRAWN FROM ITS OWN CACHE, not from CachedPerfHUDText,
	// because that string is rebuilt once a second and an FPS readout that
	// only moves at 1 Hz cannot be used for what people use an FPS readout
	// for. Same panel, same origin, different refresh rate.
	//
	// Coloured by frame rate rather than the panel's green, so "is it bad" is
	// answerable without reading the number: >= 55 green, >= 30 amber, below
	// that red. Those thresholds are just legibility, not a target.
	if (!CachedFpsText.IsEmpty())
	{
		const FLinearColor FpsColor =
			(CachedFpsValue >= 55.0) ? FLinearColor(0.2f, 1.f, 0.3f, 1.f)
			: (CachedFpsValue >= 30.0) ? FLinearColor(1.f, 0.85f, 0.2f, 1.f)
			                           : FLinearColor(1.f, 0.35f, 0.25f, 1.f);
		DrawText(CachedFpsText, FpsColor, PerfPanelMarginPx + 6.f, LineY, nullptr, 1.f, false);
		LineY += PerfPanelLineHeightPx;
	}

	for (const FString& Line : Lines)
	{
		DrawText(Line, FLinearColor(0.2f, 1.f, 0.3f, 1.f), PerfPanelMarginPx + 6.f, LineY, nullptr, 1.f, false);
		LineY += PerfPanelLineHeightPx;
	}
}

// ============================================================================
// GPU streaming panel (voxel.Debug 3)
// ============================================================================

namespace
{
// Panel row colours. Distinct MUTED grey for "not measured": the one rule this
// panel enforces everywhere is that a disarmed or not-yet-warm source prints
// words in grey, never a zero in green -- this project retracted two findings
// in a single session because an instrument read zero and was believed.
const FLinearColor kStreamRowGood(0.2f, 1.0f, 0.3f, 1.0f);
const FLinearColor kStreamRowWarn(1.0f, 0.85f, 0.2f, 1.0f);
const FLinearColor kStreamRowBad(1.0f, 0.35f, 0.25f, 1.0f);
const FLinearColor kStreamRowNeutral(0.85f, 0.85f, 0.85f, 1.0f);
const FLinearColor kStreamRowMuted(0.55f, 0.55f, 0.55f, 1.0f);

// 4392 -> "4,392". The panel's job is to be read mid-flight against a
// five-digit floor; digit grouping is what makes 4392 vs 43920 a glance
// instead of a count. FText::AsNumber does this too but with a culture lookup
// per call; this is a fixed-format readout.
FString CommaInt(int64 Value)
{
	const bool bNegative = Value < 0;
	uint64 Magnitude = bNegative ? uint64(-(Value + 1)) + 1u : uint64(Value);
	FString Digits = FString::Printf(TEXT("%llu"), (unsigned long long)Magnitude);
	FString Out;
	const int32 Len = Digits.Len();
	for (int32 I = 0; I < Len; ++I)
	{
		if (I > 0 && (Len - I) % 3 == 0)
		{
			Out += TEXT(",");
		}
		Out += Digits[I];
	}
	return bNegative ? (TEXT("-") + Out) : Out;
}
} // namespace

void AVoxelEarthHUD::DrawStreamPanel()
{
	UWorld* World = GetWorld();
	if (!World || !Canvas)
	{
		return;
	}

	// Same cadence contract as the mode-1 panel: the subsystem publishes at
	// 1 Hz, so the row strings are rebuilt at 1 Hz and drawn every frame from
	// the cache. The rebuild below is a handful of Printf calls and two
	// IConsoleManager name lookups -- comfortably under 0.05 ms once a second.
	const float NowSeconds = World->GetTimeSeconds();
	if (CachedStreamPanelRows.Num() == 0 ||
	    (NowSeconds - StreamPanelLastRefreshWorldSeconds) >= PerfRefreshIntervalSeconds)
	{
		StreamPanelLastRefreshWorldSeconds = NowSeconds;
		CachedStreamPanelRows.Reset();
		const auto AddRow = [this](const FString& Text, const FLinearColor& Color)
		{
			CachedStreamPanelRows.Emplace(Text, Color);
		};

		// Header states HOW this panel was armed, so a screenshot is
		// self-describing about its own conditions.
		const int32 Prototype = VoxelDebug::GetGpuStreamPrototype();
		AddRow(Prototype != 0
			       ? FString::Printf(TEXT("GPU STREAMING  (voxel.GpuStream.Prototype %d)"), Prototype)
			       : FString(TEXT("GPU STREAMING  (voxel.Debug 3 -- F3 cycles)")),
		       FLinearColor(1.0f, 0.85f, 0.25f, 1.0f));

		UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>();
		const FVoxelStreamPanelSnapshot Snap =
			Subsystem ? Subsystem->GetStreamPanelSnapshot() : FVoxelStreamPanelSnapshot{};

		if (!Snap.bValid)
		{
			// NOT zeros. Either the first 1 s window has not completed since
			// the perf path came alive, or there is no subsystem yet. Every
			// data row is withheld rather than printed at rest value.
			AddRow(Subsystem
				       ? FString(TEXT("collecting... (throughput valid after the first full 1 s window)"))
				       : FString(TEXT("n/a -- no voxel world subsystem in this world")),
			       kStreamRowMuted);
		}
		else
		{
			// --- 1. Throughput vs the floor, no arithmetic left to the reader
			const float Floor = VoxelDebug::kGpuStreamChunksPerSecFloor;
			const bool bPass = Snap.ChunksPerSec >= Floor;
			const FLinearColor RateColor =
				bPass ? kStreamRowGood : (Snap.ChunksPerSec >= 0.5f * Floor ? kStreamRowWarn : kStreamRowBad);
			AddRow(FString::Printf(TEXT("Chunks/s: %s / %s floor  [%s x%.2f]   peak(30s) %s"),
			                       *CommaInt(int64(Snap.ChunksPerSec + 0.5f)), *CommaInt(int64(Floor)),
			                       bPass ? TEXT("PASS") : TEXT("FAIL"),
			                       Floor > 0.f ? Snap.ChunksPerSec / Floor : 0.f,
			                       *CommaInt(int64(Snap.ChunksPerSecPeak + 0.5f))),
			       RateColor);

			// --- 2. Producer split: the programme's headline number ---------
			const int64 WindowTotal = Snap.WindowAddsGpu + Snap.WindowAddsCpu;
			if (WindowTotal <= 0)
			{
				// Zero chunks moved this window. That is a fact about DEMAND
				// (standing still on covered ground), not a 0% GPU share --
				// claiming a split of nothing is how a stalled arm hides.
				AddRow(FString::Printf(TEXT("Producer: no chunks this window (lifetime GPU %s / CPU %s)%s"),
				                       *CommaInt(Snap.TotalAddsGpu), *CommaInt(Snap.TotalAddsCpu),
				                       Snap.bGpuArmEnabled ? TEXT("") : TEXT("   GPU arm OFF")),
				       kStreamRowMuted);
			}
			else
			{
				const double GpuPct = 100.0 * double(Snap.WindowAddsGpu) / double(WindowTotal);
				// Colour tracks the PROGRAMME, not health: near-0% GPU is
				// today's truth (under 5% measured), near-100% is the goal.
				// Neutral in between; amber callout if the arm is switched off
				// entirely, because then the % is a config fact, not a result.
				AddRow(FString::Printf(TEXT("Producer: GPU %.1f%% (%s)  CPU %.1f%% (%s) this window%s"),
				                       GpuPct, *CommaInt(Snap.WindowAddsGpu),
				                       100.0 - GpuPct, *CommaInt(Snap.WindowAddsCpu),
				                       Snap.bGpuArmEnabled ? TEXT("") : TEXT("   GPU arm OFF (voxel.GPU.BrickPack 0)")),
				       Snap.bGpuArmEnabled ? kStreamRowNeutral : kStreamRowWarn);
			}
		}

		// --- 3. The hole metric: the number the owner actually cares about --
		// Found BY NAME because the instrument is being built in parallel and
		// this HUD must not link against a symbol that may not exist tonight.
		// Every failure mode gets its own words; none of them prints a number.
		{
			IConsoleManager& ConsoleManager = IConsoleManager::Get();
			IConsoleVariable* HoleSwitch = ConsoleManager.FindConsoleVariable(TEXT("voxel.March.HoleStats"));
			if (!HoleSwitch)
			{
				AddRow(TEXT("Holes: n/a -- voxel.March.HoleStats is not in this build"), kStreamRowMuted);
			}
			else if (HoleSwitch->GetInt() == 0)
			{
				AddRow(TEXT("Holes: off (voxel.March.HoleStats 0 -- voxel.GpuStream.Prototype 1 arms it)"),
				       kStreamRowMuted);
			}
			else
			{
				IConsoleVariable* HolePct =
					ConsoleManager.FindConsoleVariable(TEXT("voxel.March.HoleStats.UncoveredPct"));
				const float UncoveredPct = HolePct ? HolePct->GetFloat() : -1.0f;
				if (UncoveredPct < 0.0f)
				{
					AddRow(TEXT("Holes: armed, no sample yet (voxel.March.HoleStats.UncoveredPct unset)"),
					       kStreamRowWarn);
				}
				else
				{
					// Thresholds from the 2026-08-23 measurements: 0.03%
					// standing still, 3.99% flying at 30 m/s. Green means
					// "at or better than today's standing-still reading";
					// red means "the flying failure the programme exists
					// to remove".
					const FLinearColor HoleColor = (UncoveredPct <= 0.1f) ? kStreamRowGood
					                             : (UncoveredPct <= 1.0f) ? kStreamRowWarn
					                                                      : kStreamRowBad;
					AddRow(FString::Printf(TEXT("Holes: uncovered %.2f%% of rays  [%s]"), UncoveredPct,
					                       (UncoveredPct <= 0.1f) ? TEXT("OK")
					                       : (UncoveredPct <= 1.0f) ? TEXT("WARN") : TEXT("BAD")),
					       HoleColor);
				}

				// ---- the uncovered BREAKDOWN (voxel.March.HoleStats 2) ----
				// Which ring level's chunk was missing, and why -- the two
				// rows this panel exists for while the owner chases the dark
				// arcs. Direct API (this module links VoxelEarthShaders), a
				// PEEK of the window the 5 s perf log drained: the panel must
				// never become a second drainer of one accumulator. Every
				// not-measured state gets words, never zeros -- two readings
				// were retracted this week for zeros mistaken for
				// measurements.
				{
					const FVoxelMarchHoleStats B = VoxelMarchPeekLastHoleWindow();
					if (!B.bBreakdownArmed)
					{
						AddRow(TEXT("Holes breakdown: not measured (voxel.March.HoleStats 2 arms ")
						       TEXT("per-level + per-reason)"),
						       kStreamRowMuted);
					}
					else if (B.BreakdownFrames == 0)
					{
						AddRow(TEXT("Holes breakdown: armed, no 5s window landed yet"),
						       kStreamRowWarn);
					}
					else
					{
						uint64 Attributed = 0;
						for (int32 L = 0; L < 6; ++L) { Attributed += B.UncoveredByLevel[L]; }
						if (Attributed == 0)
						{
							// A real, measured zero: uncovered rays existed
							// (or not) but none were attributed. Distinct
							// wording from every disarmed state above.
							AddRow(TEXT("Holes breakdown: measured, 0 attributed misses this ")
							       TEXT("window"),
							       kStreamRowGood);
						}
						else
						{
							const double Inv = 100.0 / double(Attributed);
							AddRow(FString::Printf(
							           TEXT("Holes by level: L0 %.0f%%  L1 %.0f%%  L2 %.0f%%  ")
							           TEXT("L3 %.0f%%  L4 %.0f%%  L5 %.0f%%  (of %s misses)"),
							           double(B.UncoveredByLevel[0]) * Inv,
							           double(B.UncoveredByLevel[1]) * Inv,
							           double(B.UncoveredByLevel[2]) * Inv,
							           double(B.UncoveredByLevel[3]) * Inv,
							           double(B.UncoveredByLevel[4]) * Inv,
							           double(B.UncoveredByLevel[5]) * Inv,
							           *CommaInt(int64(Attributed))),
							       kStreamRowNeutral);
							// Reason order is the shader's bucket codes:
							// never / pending / evicted / unattributed.
							// pending dominating = throughput; never =
							// coverage rule; evicted = eviction policy --
							// three different fixes, which is the entire
							// point of the row.
							AddRow(FString::Printf(
							           TEXT("Holes why: never %.0f%%  pending %.0f%%  ")
							           TEXT("evicted %.0f%%  unattrib %.0f%%"),
							           double(B.UncoveredByReason[0]) * Inv,
							           double(B.UncoveredByReason[1]) * Inv,
							           double(B.UncoveredByReason[2]) * Inv,
							           double(B.UncoveredByReason[3]) * Inv),
							       kStreamRowNeutral);
						}
					}
				}
			}
		}

		// --- 4. Frame percentiles, from the FPS block's own sorted history --
		if (CachedFrameP50Ms <= 0.0)
		{
			AddRow(TEXT("Frame: warming up (needs ~20 frames of history)"), kStreamRowMuted);
		}
		else
		{
			const FLinearColor FrameColor = (CachedOnePercentLowFps >= 55.0) ? kStreamRowGood
			                              : (CachedOnePercentLowFps >= 30.0) ? kStreamRowWarn
			                                                                 : kStreamRowBad;
			AddRow(FString::Printf(TEXT("Frame: p50 %.1f ms  p95 %.1f ms  1%% low %.1f fps"),
			                       CachedFrameP50Ms, CachedFrameP95Ms, CachedOnePercentLowFps),
			       FrameColor);
		}

		if (Snap.bValid)
		{
			// --- 5. Pool: occupancy plus the two counters gated at zero -----
			const double OccupancyPct = Snap.PoolChunkCapacity > 0
				? 100.0 * double(Snap.PoolResidentChunks) / double(Snap.PoolChunkCapacity)
				: 0.0;
			const bool bPoolBad = Snap.PoolEvictions > 0 || Snap.PoolAllocFailures > 0;
			AddRow(FString::Printf(TEXT("Pool: %s / %s chunks (%.1f%%)  %.0f MiB  evict %s [%s]  allocFail %s"),
			                       *CommaInt(Snap.PoolResidentChunks), *CommaInt(int64(Snap.PoolChunkCapacity)),
			                       OccupancyPct, double(Snap.PoolResidentBytes) / (1024.0 * 1024.0),
			                       *CommaInt(Snap.PoolEvictions),
			                       Snap.PoolEvictions == 0 ? TEXT("OK") : TEXT("GATE FAIL"),
			                       *CommaInt(Snap.PoolAllocFailures)),
			       bPoolBad ? kStreamRowBad : (OccupancyPct > 90.0 ? kStreamRowWarn : kStreamRowGood));

			// --- 6. Queue health --------------------------------------------
			AddRow(FString::Printf(TEXT("Queues: pending %s | cpu %d/%d  gpuDemand %d/%d  spec %d/%d"),
			                       *CommaInt(Snap.PendingJobs),
			                       Snap.CpuJobsInFlight, Snap.CpuJobsCap,
			                       Snap.GpuDemandInFlight, Snap.GpuInFlightCap,
			                       Snap.GpuSpecInFlight, Snap.SpecInFlightCap),
			       kStreamRowNeutral);

			// The dispatch loop's exit split says which half is the limiter:
			// every cap exit is a tick where the WORKERS were full, every
			// empty exit a tick where the DISPATCHER ran out of admitted work.
			const int32 Exits = Snap.DispatchExitCap + Snap.DispatchExitEmpty;
			if (Exits <= 0)
			{
				AddRow(TEXT("Dispatch exits: none this window (streaming tick idle?)"), kStreamRowMuted);
			}
			else
			{
				const double CapPct = 100.0 * double(Snap.DispatchExitCap) / double(Exits);
				AddRow(FString::Printf(TEXT("Dispatch exits: cap %d / empty %d (%.0f%% cap)  -> %s"),
				                       Snap.DispatchExitCap, Snap.DispatchExitEmpty, CapPct,
				                       CapPct >= 60.0 ? TEXT("worker-limited")
				                       : CapPct <= 40.0 ? TEXT("admission-limited") : TEXT("balanced")),
				       kStreamRowNeutral);
			}
		}
	}

	// --- Draw: background, then the 4 Hz FPS line, then the 1 Hz rows -------
	const int32 TotalLines = CachedStreamPanelRows.Num() + (CachedFpsText.IsEmpty() ? 0 : 1);
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.55f), PerfPanelMarginPx, PerfPanelMarginPx, 620.f,
	         PerfPanelLineHeightPx * float(TotalLines) + 8.f);

	float LineY = PerfPanelMarginPx + 4.f;
	if (!CachedFpsText.IsEmpty())
	{
		const FLinearColor FpsColor =
			(CachedFpsValue >= 55.0) ? kStreamRowGood
			: (CachedFpsValue >= 30.0) ? kStreamRowWarn
			                           : kStreamRowBad;
		DrawText(CachedFpsText, FpsColor, PerfPanelMarginPx + 6.f, LineY, nullptr, 1.f, false);
		LineY += PerfPanelLineHeightPx;
	}
	for (const TPair<FString, FLinearColor>& Row : CachedStreamPanelRows)
	{
		DrawText(Row.Key, Row.Value, PerfPanelMarginPx + 6.f, LineY, nullptr, 1.f, false);
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
		// 0 -> 1 -> 2 -> 3 -> 0, the same cycle F3 drives (3 = the GPU
		// streaming panel); Left steps backwards so the row behaves like
		// every other value row.
		VoxelDebug::SetDebugMode((VoxelDebug::GetDebugMode() + (Delta >= 0 ? 1 : 3)) % 4);
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
	// box known, debug mode >= 1) then emitted 28 lines plus 16px of spacers,
	// which the old budget was 4px short of covering.
	//
	// Raised 29 -> 33 for the four-line sky block (Geo / Time / Date / Season),
	// which is the same arithmetic and the same trap: those lines are the LAST
	// thing before the settings rows, so under-budgeting here does not clip them
	// -- it clips the keybinding help at the bottom of the panel, several lines
	// away from the change that caused it.
	const float PanelHeight = OverlayLineHeightPx * 33.f + 12.f;
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

	// --- Where on the globe, and where in the year --------------------------
	//
	// READ-ONLY STATUS, so these are DrawOverlayInfo lines and deliberately NOT
	// EOverlayRow entries -- nothing here is adjustable, and that enum exists to
	// make "selectable" and "handled in AdjustOverlaySelection" the same thing
	// (VoxelEarthHUD.h:43-46). The knobs behind these values are cvars
	// (voxel.Sky.*) and CLI pins (-VoxelTimeOfDay / -VoxelDate / -VoxelTimeScale),
	// which is where they belong.
	//
	// LatitudeDeg/LongitudeDeg are ALREADY the player's position, not the world
	// origin's: UVoxelSkySubsystem::Tick resolves the observer through
	// ResolveObserverXYUU and converts with GeoFromWorldUU, so there is no position
	// work to do here and none should be added -- a second conversion in this file
	// is a second answer that can drift from the one the sun was computed at.
	//
	// FETCHED DEFENSIVELY and the whole block SKIPPED when the subsystem is absent,
	// the same shape DrawPerfHUD uses for the water snapshot
	// (VoxelEarthHUD.cpp:234-247). A zeroed readout here would be far worse than a
	// missing one: "0.0000 N, 0.0000 E" at "00:00" is a real place at a real time
	// -- midnight on the equator -- so it is indistinguishable from a correct
	// reading. VoxelPerfRunSubsystem.cpp's LogSky declines to print a line for the
	// same reason, in the same words.
	//
	// AND "THE SUBSYSTEM EXISTS" IS NOT THE SAME QUESTION AS "IT HAS EVER RUN",
	// which is the trap this block would otherwise walk straight into. With
	// voxel.Sky.Enabled 0 the subsystem is still constructed and still returns a
	// reference, but UVoxelSkySubsystem::IsTickable is false (VoxelSkySubsystem.cpp
	// :1837-1844, the zero-cost-when-off gate), so FVoxelSkyState is never written
	// and every field is its default -- i.e. EXACTLY the midnight-at-the-equator
	// reading the paragraph above refuses to draw, arriving through the guard rather
	// than around it. JulianDay is the sentinel because it is the one field with no
	// legal zero: Tick always sets it to JD(2000-01-01) plus the elapsed clock
	// (VoxelSkySubsystem.cpp:1924-1930), which is ~2451544.5 and up. bClockRunning
	// would NOT do -- a deliberately pinned clock is false too, and that case must
	// still draw.
	UVoxelSkySubsystem* Sky = World->GetSubsystem<UVoxelSkySubsystem>();
	if (Sky && Sky->GetSkyState().JulianDay > 0.0)
	{
		const FVoxelSkyState& SkyState = Sky->GetSkyState();

		// HEMISPHERE LETTERS, NOT SIGNED DECIMALS. A leading '-' has to be decoded
		// before it means anything and at a glance reads as a stray character;
		// "1.2461 W" cannot be misread. 4 decimal places is ~11 m of latitude,
		// i.e. finer than a chunk, which is the resolution at which this is
		// actually useful for cross-checking against a tile or a probe.
		DrawOverlayInfo(FString::Printf(TEXT("Geo     %.4f %s, %.4f %s"),
		                                 FMath::Abs(SkyState.LatitudeDeg), SkyState.LatitudeDeg >= 0.0 ? TEXT("N") : TEXT("S"),
		                                 FMath::Abs(SkyState.LongitudeDeg), SkyState.LongitudeDeg >= 0.0 ? TEXT("E") : TEXT("W")),
		                kOverlayInfo, PanelX, Y);

		// A FROZEN CLOCK IS CALLED OUT, AND WARN-COLOURED, BECAUSE IT IS THE NORMAL
		// STATE IN A CAPTURE. Every perf and screenshot leg pins the sun
		// (-VoxelTimeScale=0, or voxel.Sky.Enabled 0) so that it cannot drift
		// between the settle wait and the shutter -- so a still clock is expected
		// far more often than not, and without the label it reads as a broken
		// clock. bClockRunning is the subsystem's own answer to that question
		// (voxel.Sky.Enabled && TimeScale != 0), not a re-derivation from the cvars.
		//
		// Minutes TRUNCATED, not rounded, matching PerfClockFromLocalHours in
		// VoxelPerfRunSubsystem.cpp: rounding lets 11:59.9 print as "12:00", and a
		// frame labelled noon that was taken a minute off it is how two legs with
		// different sun angles come to look identical. Truncation also cannot
		// produce "12:60".
		const int32 Hour = FMath::Clamp((int32)SkyState.LocalHours, 0, 23);
		const int32 Minute = FMath::Clamp((int32)((SkyState.LocalHours - (double)Hour) * 60.0), 0, 59);
		DrawOverlayInfo(FString::Printf(TEXT("Time    %02d:%02d%s   sun %+.1f deg (%s)"), Hour, Minute,
		                                 SkyState.bClockRunning ? TEXT("") : TEXT("  CLOCK FROZEN"),
		                                 SkyState.SunAltitudeDeg, SkyState.bSunUp ? TEXT("up") : TEXT("down")),
		                SkyState.bClockRunning ? kOverlayInfo : kOverlayWarn, PanelX, Y);

		// THE RAW DAY-OF-YEAR IS SHOWN ALONGSIDE THE DATE, not instead of it and not
		// hidden behind it. Two reasons, both about the compressed calendar. (1)
		// day-of-year is what actually drives the solar declination
		// (VoxelEphemeris.h:119-124); the MM-DD is a rendering of it. (2) Only
		// voxel.Sky.DaysPerYear distinct dates are REACHABLE in a game year -- 48 by
		// default, so 7.6 real days apart -- which is why -VoxelDate=06-21 generally
		// does not produce 21 June. Printing the year length makes a date that is
		// several days off the request read as the calendar working rather than as
		// the switch being ignored.
		//
		// MM-DD is the exact form -VoxelDate= takes, so this line is pasteable back
		// into the switch that reproduces the frame -- the same choice
		// VoxelPerfRunSubsystem's date= field makes. Formatted through the now-single
		// VoxelSky::MonthDayFromDayOfYear, so the overlay, the perf log and the sky
		// resolve log cannot disagree about what day-of-year 79 is called.
		int32 Month = 1, Day = 1;
		VoxelSky::MonthDayFromDayOfYear(SkyState.DayOfYear, Month, Day);
		DrawOverlayInfo(FString::Printf(TEXT("Date    %02d-%02d   day-of-year %d   (%.0f-day year)"), Month, Day,
		                                 SkyState.DayOfYear, VoxelSky::GetDaysPerYear()),
		                kOverlayInfo, PanelX, Y);

		// The hemisphere is NAMED rather than left implicit in the N/S letter three
		// lines up, because "summer" is the half of this line a reader takes away
		// and it means the opposite thing on either side of the equator. See
		// SeasonName for the convention and the boundary dates.
		DrawOverlayInfo(FString::Printf(TEXT("Season  %s (%s hemisphere)"),
		                                 SeasonName(SkyState.DayOfYear, SkyState.LatitudeDeg),
		                                 SkyState.LatitudeDeg >= 0.0 ? TEXT("northern") : TEXT("southern")),
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
		bool bTracked = false, bHasComponent = false, bSettled = false;
		int32 Quads = 0;
		const bool bFound = Subsystem->DebugChunkStatusAt(Pos, bTracked, bHasComponent, Quads, bSettled);
		// settled= is what makes this row answer "why am I hovering": tracked
		// with no component reads alarming, but settled=y means the chunk meshed
		// to nothing and is FINAL (all air), which is not a reason to wait. Only
		// the unsettled case gates movement, so only it is warn-coloured.
		DrawOverlayInfo(FString::Printf(TEXT("Here    %s  tracked=%s  component=%s  quads=%d  settled=%s"),
		                                 bFound ? TEXT("chunk") : TEXT("none"), bTracked ? TEXT("y") : TEXT("n"),
		                                 bHasComponent ? TEXT("y") : TEXT("n"), Quads, bSettled ? TEXT("y") : TEXT("n")),
		                (bTracked && !bHasComponent && !bSettled) ? kOverlayWarn : kOverlayInfo, PanelX, Y);
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
	               FString::Printf(TEXT("%d  (0 off, 1 perf HUD, 2 +layers, 3 streaming)"), VoxelDebug::GetDebugMode()), PanelX, Y);

	// The three layer rows show the CVAR value and, when mode < 2 is holding
	// them back, say so -- otherwise flipping one at mode 1 looks broken.
	// == 2, matching the layer helpers themselves (VoxelDebug.cpp): mode 3 is
	// the streaming panel and deliberately does NOT arm the 3D layers, so the
	// hint must reappear there too.
	const TCHAR* Gated = (VoxelDebug::GetDebugMode() == 2) ? TEXT("") : TEXT("  (needs voxel.Debug 2)");
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
