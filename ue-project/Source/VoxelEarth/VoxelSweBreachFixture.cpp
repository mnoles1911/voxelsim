#include "VoxelSweBreachFixture.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelWorldSubsystem.h"

// ===========================================================================
// -VoxelSweBreachTest -- the surge/bed-seating discriminator
// ===========================================================================
//
// The header carries the argument for why this exists. This file is the
// mechanism, and there are four ideas in it worth reading before the code:
//
// 1. THE BASIN IS MEASURED, NOT ASSUMED. After carving, the fixture surveys the
//    topmost solid voxel of every column in a window (a heightfield), then runs
//    a spill-level search on it: the highest water level whose flooded region,
//    grown from the basin's own seed column, does NOT reach the window border.
//    That gives three numbers nothing else can supply -- the basin's SPILL
//    LEVEL, its CAPACITY in voxels, and its LOWEST RIM COLUMN -- and those are
//    what make the run self-checking. The pour is a fixed fraction of the
//    measured capacity (so the basin provably holds it), the breach is aimed at
//    the measured rim (so it provably drains), and, critically, the EXPECTED
//    SETTLED SURFACE HEIGHT is a geometric consequence of the survey rather
//    than something to eyeball. "Settles to the wrong level" only means
//    anything against a level derived independently of the solver.
//
// 2. VELOCITY, NOT PIXELS. vxc::SweGrid::velocityAt is the depth-averaged
//    column velocity in mm/s, derived exactly (swe.h S6) from the face fluxes
//    that ARE the momentum state. A surge is a directed velocity that appears
//    at the breach mouth, arrives downstream some ticks later, and decays. That
//    is a time series over half a dozen named columns, sampled at the
//    subsystem's own 10 Hz fixed step, and it is the ONLY form in which the
//    question "did it surge?" has an answer that survives being written down.
//    Screenshots are taken too, but they are corroboration, not the instrument.
//
// 3. THE BED AUDIT RUNS BEFORE THE BREACH. Beds are seated once, at arming,
//    from the topmost solid voxel around GetSurfaceHeightUU, and are never
//    re-seated (swe.h S5(a) is explicit that re-seating downward would be
//    wrong). So a stored bed that disagrees with the live terrain BEFORE
//    anything is dug is a defect with no other symptom than "the sheet rests at
//    the wrong height" -- exactly hypothesis (b). Auditing pre-breach makes
//    that reading unambiguous; auditing again post-breach then shows what the
//    carve legitimately invalidated, and the two counts are meant to be
//    compared against the carve footprint.
//
// 4. IT ALWAYS DIES. Every terminal path calls Finish(), every stage is
//    reachable only from a timer that Finish() clears, and an absolute
//    watchdog timer armed at the very start guarantees exit even if a poll
//    never converges. -VoxelWaterParityTest shipped without a self-quit once
//    and cost a session 2h40m of wall clock and 16,384 s of CPU.

namespace
{
// --- Tunables, all overridable and all logged ------------------------------

// Site search. Steps a grid looking for the steepest sustained downhill run.
constexpr double kDefaultSearchRadiusM = 120.0;
constexpr double kSiteGridStepM = 4.0;
// The run over which "downhill" is scored, in voxels (1 voxel = 10 UU = 0.1 m).
constexpr int32 kSlopeRunVoxels = 30; // 3.0 m
// Below this grade the site cannot discriminate a film from a mis-seated bed,
// and the run says so at Warning rather than pretending otherwise.
constexpr double kMinUsefulGrade = 0.08; // 8%
// And an upper bound, so the whole scenario stays inside the two windows it has
// to fit in: the 96-column survey (+/-4.8 m) and, more tightly, the sheet's
// single dense 128-column rectangle (+/-6.4 m). Past this grade the runout
// below the breach leaves the surveyed ground -- and possibly the sheet -- long
// before the surge has decayed, and the downstream probes would be reading
// columns the sheet does not own.
constexpr double kMaxUsefulGrade = 0.45; // 45%
// Keep the whole scenario well clear of sea level so the Reservoir v0 breach
// seeding (z<0 only) can never inject water into the ledger mid-run.
constexpr double kMinSiteHeightM = 20.0;

// Basin geometry, in voxels. A VERTICAL STACK of equal spheres, not one big
// sphere, and the reason is the same lid rule that shapes the breach below.
//
// A single sphere centred below the surface leaves an annulus near its rim
// where the sphere's top stops just under the terrain: a void with a thin rock
// LID over it. Those columns are invisible to a topmost-solid-voxel survey (it
// reports the lid, correctly), so they are excluded from the measured basin and
// never poured into -- but the CA flows into them anyway, through the void's
// own connection at depth. The pool would then drain into thousands of voxels
// of hidden space and settle BELOW the geometric prediction, which is precisely
// the signature this fixture reads as "mis-seated beds". A false positive on
// the headline question, manufactured by the fixture's own carve.
//
// Stacking spheres from above the surface down to the floor makes the pit open
// to the sky over its whole radius, so surveyed capacity and real capacity are
// the same number. The step is well under the radius so the union is
// continuous. Jitter is zero: this rim is about to be measured, and a ragged
// edge would only make the spill level harder to reason about.
constexpr int32 kBasinRadiusVoxels = 12;             // 1.2 m
constexpr int32 kBasinTopAboveSurfaceVoxels = 4;     // start the stack in open air
constexpr int32 kBasinBottomBelowSurfaceVoxels = 16; // last sphere centre; the floor is a further R down
constexpr int32 kBasinStepVoxels = 4;
constexpr double kBasinCarveJitterUU = 0.0;

// Survey window, in columns, centred on the site. Must fit inside the sheet
// (128 columns) with margin, and must be wide enough that the runout below the
// breach is inside it.
constexpr int32 kSurveyColumns = 96; // 9.6 m
constexpr int32 kSurveyScanAbove = 8;
constexpr int32 kSurveyScanDown = 56;

// Breach carve.
constexpr double kBreachRadiusUU = 40.0;     // 4 voxels
constexpr int32 kBreachBelowSurfaceVoxels = 3; // cut the mouth below the waterline
constexpr int32 kBreachOutwardVoxels = 3;      // second sphere, further into the wall

// Post-breach sampling.
constexpr float kSampleIntervalSeconds = 0.1f; // the subsystem's own fixed step
constexpr float kDefaultSampleSeconds = 8.f;

// Polls.
constexpr float kPollIntervalSeconds = 2.f;
constexpr int32 kMaxTerrainPolls = 30; // 60 s
constexpr int32 kMaxSettlePolls = 20;  // 40 s

// Absolute watchdog, measured from the moment the fixture is scheduled. Sized
// against the worst case the stages can actually reach (delay + a capped-out
// terrain poll + a capped-out settle poll + the sample window + two whole-sheet
// bed audits + the closing settle), roughly doubled. This timer is the ONLY
// thing standing between a stalled poll and an editor left running overnight;
// -VoxelWaterParityTest's missing self-quit cost one session 2h40m.
constexpr float kWatchdogSeconds = 480.f;

FORCEINLINE double VxToUU(int64 V)
{
	return (double(V) + 0.5) * VoxelCoords::VoxelSizeUU;
}

FORCEINLINE int64 UUToVx(double UU)
{
	return int64(FMath::FloorToDouble(UU / VoxelCoords::VoxelSizeUU));
}

// --- The surveyed heightfield ----------------------------------------------
//
// Top[i] is the voxel z of the topmost SOLID voxel in that column's scan
// window, per UVoxelWorldSubsystem::IsSolidAtVoxel (overlay-aware, so a carve
// is reflected immediately -- which is the whole reason the survey runs AFTER
// the basin is dug). A column that found nothing is marked not-found and is
// treated as an infinitely deep hole, i.e. as an escape route, so the spill
// search can never mistake an unresolved column for a wall.
struct FHeightField
{
	int32 W = 0;
	int64 OriginVx = 0, OriginVy = 0;
	TArray<int32> Top;
	TArray<uint8> Found;

	FORCEINLINE int32 Idx(int32 I, int32 J) const { return J * W + I; }
	FORCEINLINE int64 VxOf(int32 I) const { return OriginVx + I; }
	FORCEINLINE int64 VyOf(int32 J) const { return OriginVy + J; }
	FORCEINLINE bool InRange(int32 I, int32 J) const { return I >= 0 && J >= 0 && I < W && J < W; }
	FORCEINLINE bool IsBorder(int32 I, int32 J) const { return I == 0 || J == 0 || I == W - 1 || J == W - 1; }
};

void SurveyHeightField(const UVoxelWorldSubsystem& Terrain, FHeightField& H, int64 OriginVx, int64 OriginVy, int32 W)
{
	H.W = W;
	H.OriginVx = OriginVx;
	H.OriginVy = OriginVy;
	H.Top.SetNumUninitialized(W * W);
	H.Found.SetNumUninitialized(W * W);
	for (int32 J = 0; J < W; ++J)
	{
		for (int32 I = 0; I < W; ++I)
		{
			const int64 Vx = OriginVx + I;
			const int64 Vy = OriginVy + J;
			const double SurfUU = Terrain.GetSurfaceHeightUU(double(Vx) * VoxelCoords::VoxelSizeUU,
			                                                 double(Vy) * VoxelCoords::VoxelSizeUU);
			const int64 TopZ = int64(FMath::FloorToDouble(SurfUU / VoxelCoords::VoxelSizeUU)) + kSurveyScanAbove;
			int32 Found = 0;
			int32 Top = int32(TopZ - (kSurveyScanAbove + kSurveyScanDown));
			for (int32 K = 0; K < kSurveyScanAbove + kSurveyScanDown; ++K)
			{
				if (Terrain.IsSolidAtVoxel(Vx, Vy, TopZ - K))
				{
					Found = 1;
					Top = int32(TopZ - K);
					break;
				}
			}
			H.Top[H.Idx(I, J)] = Top;
			H.Found[H.Idx(I, J)] = uint8(Found);
		}
	}
}

// Columns reachable from the seed through 4-neighbours whose top solid voxel
// lies strictly below `Level` (i.e. columns that hold at least one voxel of
// water when the surface sits at `Level`). bOutEscaped means that region
// touched the survey border or an unresolved column -- water at this level is
// no longer contained by anything the fixture can see.
void FloodAtLevel(const FHeightField& H, int32 Si, int32 Sj, int32 Level, TArray<int32>& OutRegion, bool& bOutEscaped,
                  TArray<uint8>& Visited)
{
	OutRegion.Reset();
	bOutEscaped = false;
	Visited.Init(0, H.Top.Num());
	if (!H.InRange(Si, Sj) || H.Top[H.Idx(Si, Sj)] >= Level)
	{
		return;
	}

	TArray<int32> Stack;
	Stack.Add(H.Idx(Si, Sj));
	Visited[H.Idx(Si, Sj)] = 1;
	static const int32 DI[4] = {1, -1, 0, 0};
	static const int32 DJ[4] = {0, 0, 1, -1};
	while (Stack.Num() > 0)
	{
		const int32 Cur = Stack.Pop();
		OutRegion.Add(Cur);
		const int32 I = Cur % H.W;
		const int32 J = Cur / H.W;
		if (H.IsBorder(I, J) || H.Found[Cur] == 0)
		{
			bOutEscaped = true;
		}
		for (int32 D = 0; D < 4; ++D)
		{
			const int32 Ni = I + DI[D];
			const int32 Nj = J + DJ[D];
			if (!H.InRange(Ni, Nj))
			{
				continue;
			}
			const int32 N = H.Idx(Ni, Nj);
			if (Visited[N] || H.Top[N] >= Level)
			{
				continue;
			}
			Visited[N] = 1;
			Stack.Add(N);
		}
	}
}

// Capacity of a region at a level, in whole voxels of water.
int64 RegionCapacityVoxels(const FHeightField& H, const TArray<int32>& Region, int32 Level)
{
	int64 Sum = 0;
	for (int32 Idx : Region)
	{
		Sum += int64(Level - H.Top[Idx]);
	}
	return Sum;
}

// --- The probe set ---------------------------------------------------------
struct FProbe
{
	FString Name;
	int64 Vx = 0;
	int64 Vy = 0;
	int32 SurveyTop = 0;    // topmost solid voxel at survey time, for the runout profile
	bool bInPourPool = false; // inside the FLOODED REGION at the pour level, not merely below it
};

// --- Run state -------------------------------------------------------------
struct FSweBreachRun
{
	TWeakObjectPtr<UWorld> World;

	// Parameters as parsed.
	float DelaySeconds = 20.f;
	bool bArmSwe = true;
	double SearchRadiusM = kDefaultSearchRadiusM;
	double FillFraction = 0.7;
	float SampleSeconds = kDefaultSampleSeconds;
	bool bForcedSite = false;
	double SiteXUU = 0.0, SiteYUU = 0.0;

	// Site, as chosen.
	int32 SlopeDirI = 1, SlopeDirJ = 0;
	double SiteGrade = 0.0;
	double SiteSurfaceUU = 0.0;

	// Survey + basin, as measured.
	FHeightField Field;
	int32 SeedI = 0, SeedJ = 0;
	int32 SpillLevel = 0;
	int64 SpillCapacityVoxels = 0;
	int32 PourLevel = 0;
	double ExpectedLevelVz = 0.0;
	int64 TargetFillUnits = 0;
	int64 ActuallyPouredUnits = 0;
	int32 PourColumns = 0;

	// Breach, as aimed.
	int64 RimVx = 0, RimVy = 0;
	int32 RimTop = 0;
	int32 OutI = 1, OutJ = 0;
	int32 BreachVoxelsRemoved = 0;

	TArray<FProbe> Probes;

	// Camera.
	FVector CameraUU = FVector::ZeroVector;
	FRotator CameraRot = FRotator::ZeroRotator;

	// Poll/sample counters.
	int32 TerrainPolls = 0;
	int32 SettlePolls = 0;
	int32 Samples = 0;
	int32 MaxSamples = 0;

	// -1 == the sheet was never armed. 0 == every seated bed agreed with the
	// live terrain before anything was dug, i.e. hypothesis (b) is refuted and
	// the tuning question is legitimately open. Anything above 0 means it is
	// not, and the closing report says so instead of proposing constants.
	int32 PreBreachBedMismatches = -1;

	// The settle poll runs twice -- once before arming, once after the surge --
	// and this is which pass it is on. Draining a basin through a notch takes
	// far longer than the sampling window, and a "post-breach settled level"
	// read off a pool that is still emptying is not a settled level.
	bool bPostBreachSettle = false;

	// Per-probe surge bookkeeping, for the one-line verdict.
	//
	// THREE series, not one, because a correct breach does NOT put its surge
	// where a first guess would look for it. swe.h S5(a) is explicit: a column
	// whose bed voxel stops being solid is PUNCTURED and becomes a metered
	// source into the CA, then demotes after demoteDwellTicks -- "a hole in a
	// lake floor is confined 3D flow, i.e. CA territory by definition of the
	// split". The breach mouth is therefore expected to leave the sheet within
	// a few seconds and report velocity 0 for a CORRECT reason. The sheet's
	// surge shows up on the BASIN side, as a drawdown directed at the mouth,
	// while the outflow itself travels downstream as CA fill. So:
	//   PeakSpeed      -- |velocityAt|, the sheet's momentum magnitude.
	//   PeakAlongOut   -- the same velocity PROJECTED on the outflow direction.
	//                     Sign is the whole point: a drawdown is positive
	//                     (toward the mouth), noise is not directed at all.
	//   FirstWaterIdx  -- the first sample at which the column holds ANY water
	//                     from either solver. Across ds+2/ds+4/ds+8/ds+16 this
	//                     is the front's ARRIVAL TIME, and it is the one
	//                     measurement that works identically with the sheet on
	//                     and off, which is what makes the A/B comparable.
	TArray<int32> PeakSpeedMmPerSec;
	TArray<int32> PeakSampleIndex;
	TArray<int32> PeakAlongOutMmPerSec;
	TArray<int32> FirstWaterSampleIndex;

	bool bFinishing = false;

	FTimerHandle StartHandle;
	FTimerHandle TerrainPollHandle;
	FTimerHandle SettlePollHandle;
	FTimerHandle StageHandle;
	FTimerHandle SampleHandle;
	FTimerHandle WatchdogHandle;
	FTimerHandle QuitHandle;
};

using FRunRef = TSharedRef<FSweBreachRun, ESPMode::ThreadSafe>;

// Stage forward declarations -- the sequence, in order.
void StageBegin(FRunRef Run);
void StageTerrainPoll(FRunRef Run);
void StageCarveAndSurvey(FRunRef Run);
void StagePour(FRunRef Run);
void StageSettlePoll(FRunRef Run);
void StageArmSwe(FRunRef Run);
void StagePreBreachReport(FRunRef Run);
void StageBreach(FRunRef Run);
void StageSample(FRunRef Run);
void StageFinalReport(FRunRef Run);
void Finish(FRunRef Run, const TCHAR* Reason);

FORCEINLINE UWorld* WorldOf(const FRunRef& Run)
{
	return Run->World.Get();
}

void SetTimerOnce(FRunRef Run, FTimerHandle& Handle, float Delay, TFunction<void(FRunRef)> Fn)
{
	UWorld* W = WorldOf(Run);
	if (!W)
	{
		return;
	}
	W->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([Run, Fn]() { Fn(Run); }), FMath::Max(Delay, 0.01f),
	                              false);
}

void SetTimerLooping(FRunRef Run, FTimerHandle& Handle, float Interval, TFunction<void(FRunRef)> Fn)
{
	UWorld* W = WorldOf(Run);
	if (!W)
	{
		return;
	}
	W->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([Run, Fn]() { Fn(Run); }),
	                              FMath::Max(Interval, 0.01f), true);
}

// --- Terminal path ---------------------------------------------------------
//
// EVERY exit goes through here, including the watchdog and every "subsystems
// not ready" bail. Clears the whole timer set first so nothing can re-enter a
// stage after the final line, then arms the one timer that ends the process.
void Finish(FRunRef Run, const TCHAR* Reason)
{
	if (Run->bFinishing)
	{
		return;
	}
	Run->bFinishing = true;

	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSweBreachTest: FINISHED (%s). Quitting in 5s."), Reason);

	UWorld* W = WorldOf(Run);
	if (!W)
	{
		FPlatformMisc::RequestExit(/*bForce*/ false);
		return;
	}
	FTimerManager& TM = W->GetTimerManager();
	TM.ClearTimer(Run->StartHandle);
	TM.ClearTimer(Run->TerrainPollHandle);
	TM.ClearTimer(Run->SettlePollHandle);
	TM.ClearTimer(Run->StageHandle);
	TM.ClearTimer(Run->SampleHandle);
	TM.ClearTimer(Run->WatchdogHandle);
	TM.SetTimer(Run->QuitHandle, FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }), 5.f,
	            false);
}

// --- Pose ------------------------------------------------------------------
void PoseCamera(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	if (APawn* P = PC->GetPawn())
	{
		P->SetActorLocation(Run->CameraUU, false, nullptr, ETeleportType::TeleportPhysics);
		P->SetActorRotation(Run->CameraRot);
		PC->SetViewTarget(P);
	}
	PC->SetControlRotation(Run->CameraRot);
}

// --- Probe logging ---------------------------------------------------------
//
// Two log families, both one line per record and both with a fixed field order
// so they can be grepped straight into a spreadsheet:
//
//   SweBreachTick -- one per sample, whole-system: the two solvers' volumes,
//                    their sum, the injected total they must equal, and the
//                    coupler's transfer ledgers.
//   SweBreachVel  -- one per sample PER PROBE COLUMN: bed, depth, the derived
//                    water surface, and velocityAt. This is the surge.
void LogProbeLine(const TCHAR* Tag, int32 SampleIdx, double ElapsedSeconds, const FProbe& P,
                  const FVoxelWaterColumnProbe& C, int32 OutI, int32 OutJ)
{
	// Sheet surface, in voxel-z units: bed + depth/255 (swe.h S2 -- depth is a
	// COLUMN TOTAL in fill units, 255 == one voxel of height, so this division
	// is a genuine elevation).
	const double SheetSurfaceVz = C.bInSheet ? double(C.Bed) + double(C.Depth) / 255.0 : 0.0;
	const int32 Speed = int32(FMath::RoundToInt(FMath::Sqrt(double(C.VelXMmPerSec) * double(C.VelXMmPerSec) +
	                                                        double(C.VelYMmPerSec) * double(C.VelYMmPerSec))));
	// Signed projection on the outflow direction: positive means the water is
	// moving the way the breach drains. Directedness is what separates a surge
	// from the sheet jiggling.
	const int32 AlongOut = C.VelXMmPerSec * OutI + C.VelYMmPerSec * OutJ;
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("%s n=%d t=%+.1fs probe=%s vx=%lld vy=%lld sweOwned=%d bed=%d surveyTop=%d terrainTop=%lld depth=%d ")
	       TEXT("sheetSurfaceVz=%.3f caTopVz=%lld caTopFill=%d caColumnFill=%d vel=(%d,%d)mm/s speed=%d ")
	       TEXT("velAlongOut=%+d flux=(%d,%d)fill/tick"),
	       Tag, SampleIdx, ElapsedSeconds, *P.Name, (long long)P.Vx, (long long)P.Vy, C.bSweOwned ? 1 : 0, C.Bed,
	       P.SurveyTop, C.bTerrainTopFound ? (long long)C.TerrainTopSolidVz : (long long)-9999, C.Depth, SheetSurfaceVz,
	       C.CaTopFill != 0 ? (long long)C.CaTopVz : (long long)-9999, int32(C.CaTopFill), C.CaColumnFill, C.VelXMmPerSec,
	       C.VelYMmPerSec, Speed, AlongOut, C.FluxXFillPerTick, C.FluxYFillPerTick);
}

void SampleAllProbes(const TCHAR* Tag, FRunRef Run, int32 SampleIdx, double ElapsedSeconds, bool bTrackPeaks)
{
	UWorld* W = WorldOf(Run);
	UVoxelWaterSubsystem* Water = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return;
	}
	for (int32 I = 0; I < Run->Probes.Num(); ++I)
	{
		const FProbe& P = Run->Probes[I];
		FVoxelWaterColumnProbe C;
		Water->GetWaterColumnProbe(P.Vx, P.Vy, int64(Run->PourLevel) + 8, 80, C);
		LogProbeLine(Tag, SampleIdx, ElapsedSeconds, P, C, Run->OutI, Run->OutJ);
		if (bTrackPeaks && Run->PeakSpeedMmPerSec.IsValidIndex(I))
		{
			const int32 Speed = int32(FMath::RoundToInt(FMath::Sqrt(double(C.VelXMmPerSec) * double(C.VelXMmPerSec) +
			                                                        double(C.VelYMmPerSec) * double(C.VelYMmPerSec))));
			if (Speed > Run->PeakSpeedMmPerSec[I])
			{
				Run->PeakSpeedMmPerSec[I] = Speed;
				Run->PeakSampleIndex[I] = SampleIdx;
			}
			const int32 AlongOut = C.VelXMmPerSec * Run->OutI + C.VelYMmPerSec * Run->OutJ;
			if (FMath::Abs(AlongOut) > FMath::Abs(Run->PeakAlongOutMmPerSec[I]))
			{
				Run->PeakAlongOutMmPerSec[I] = AlongOut;
			}
			if (Run->FirstWaterSampleIndex[I] < 0 && (C.CaColumnFill > 0 || (C.bSweOwned && C.Depth > 0)))
			{
				Run->FirstWaterSampleIndex[I] = SampleIdx;
			}
		}
	}
}

// The settled-level report: the one that answers "does the pooled water sit
// where the basin's own geometry says it should?".
void LogLevelReport(const TCHAR* Phase, FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWaterSubsystem* Water = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return;
	}
	FVoxelSweProbe S;
	const bool bArmed = Water->GetSweProbe(S);
	const double ToleranceVoxels = bArmed ? double(S.SettleToleranceFill) / 255.0 : 1.0 / 255.0;

	for (const FProbe& P : Run->Probes)
	{
		FVoxelWaterColumnProbe C;
		Water->GetWaterColumnProbe(P.Vx, P.Vy, int64(Run->PourLevel) + 8, 80, C);

		// The measured water surface, from whichever solver owns the column.
		// SWE: bed + depth/255. CA: the top filled voxel plus its own fraction.
		double MeasuredVz = 0.0;
		const TCHAR* Source = TEXT("none");
		bool bHaveWater = false;
		if (C.bSweOwned && C.Depth > 0)
		{
			MeasuredVz = double(C.Bed) + double(C.Depth) / 255.0;
			Source = TEXT("sheet");
			bHaveWater = true;
		}
		else if (C.CaTopFill != 0)
		{
			MeasuredVz = double(C.CaTopVz) + double(C.CaTopFill) / 255.0;
			Source = TEXT("ca");
			bHaveWater = true;
		}

		// The geometric answer, from the survey. MEMBERSHIP of the flooded
		// region, not merely "lower than the water level": a column downstream
		// of the rim is far below the pool's surface and holds none of it, and
		// scoring it against the pool level would manufacture a huge bogus
		// delta at exactly the probes the surge is measured at.
		const bool bInBasin = P.bInPourPool;
		const double ExpectedVz = bInBasin ? Run->ExpectedLevelVz : double(P.SurveyTop);
		const double DeltaVoxels = bHaveWater ? MeasuredVz - ExpectedVz : 0.0;

		UE_LOG(LogVoxelEarth, Log,
		       TEXT("SweBreachLevel phase=%s probe=%s vx=%lld vy=%lld source=%s inBasin=%d bed=%d surveyTop=%d ")
		       TEXT("depth=%d measuredSurfaceVz=%.3f expectedSurfaceVz=%.3f delta=%+.3f voxels (%+.1f mm) ")
		       TEXT("tolerance=%.3f voxels"),
		       Phase, *P.Name, (long long)P.Vx, (long long)P.Vy, Source, bInBasin ? 1 : 0, C.Bed, P.SurveyTop, C.Depth,
		       MeasuredVz, ExpectedVz, DeltaVoxels, DeltaVoxels * 100.0, ToleranceVoxels);
	}
}

// Returns the number of SWE-OWNED columns whose bed disagrees with the live
// terrain, or -1 if the sheet is not armed. That number is the gate on
// everything downstream: it is hypothesis (b), measured.
int32 LogBedAudit(const TCHAR* Phase, FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWaterSubsystem* Water = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return -1;
	}
	FVoxelSweBedAudit A;
	if (!Water->AuditSweBedSeating(A))
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("SweBreachBeds phase=%s: sheet not armed, no beds to audit (CA-only run)."), Phase);
		return -1;
	}
	const double MeanAbs = A.Mismatched > 0 ? double(A.SumAbsDeltaVoxels) / double(A.Mismatched) : 0.0;
	// worstStoredBedVz / worstReseatedBedVz are voxel Z COORDINATES of the one
	// worst column, not counts. The old label for the second one was `reseated=`,
	// which read as "5,383 columns were re-seated" in the 2026-07-29 run and sent
	// a whole investigation after a re-seating pass that did not exist at the
	// time: AuditSweBedSeating is const, and until ReseatEditedSweBeds landed
	// SweGrid::setBed had exactly one caller in the whole engine (MaybeArmSwe).
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("SweBreachBeds phase=%s columns=%d seated=%d sweOwned=%d mismatched=%d sweOwnedMismatched=%d ")
	       TEXT("maxAbsDelta=%d meanAbsDelta=%.2f voxels mismatchedVsBareTerrain=%d worst=(%lld,%lld) ")
	       TEXT("worstStoredBedVz=%d worstReseatedBedVz=%d"),
	       Phase, A.Columns, A.Seated, A.SweOwned, A.Mismatched, A.SweOwnedMismatched, A.MaxAbsDeltaVoxels, MeanAbs,
	       A.MismatchedVsTerrain, (long long)A.WorstVx, (long long)A.WorstVy, A.WorstStoredBed, A.WorstReseatedBed);

	// The loud verdict. A pre-breach mismatch has no innocent explanation: the
	// beds were seated from this same function against this same world and
	// nothing has edited terrain yet.
	if (A.SweOwnedMismatched > 0)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("SweBreachBeds phase=%s: %d SWE-OWNED column(s) are seated on a bed that disagrees with the live ")
		       TEXT("terrain by up to %d voxel(s). If phase=pre-breach, THIS IS THE ANSWER TO THE WHOLE QUESTION -- the ")
		       TEXT("sheet is resting at the wrong height and the 'thin film' is a bed-seating defect, NOT shallow-water ")
		       TEXT("physics. Do not tune damping/absorption until this is zero. If phase=post-breach, compare the count ")
		       TEXT("against the %d voxel(s) the breach carve removed. A mismatch inside the carve footprint is ")
		       TEXT("TRANSIENT and must clear: swe.h S5(a) does not re-seat a bed to follow a hole (a punctured column ")
		       TEXT("is a metered source into the CA and demotes on its own), and ReseatEditedSweBeds then re-seats it ")
		       TEXT("once it is CA-owned. A mismatch that PERSISTS to this line is the 2026-07-29 bug back: a column ")
		       TEXT("resting on air fails eligible()'s first test forever, so it can never promote, never be punctured ")
		       TEXT("again, and stays an inactive hard wall exactly where the ground was removed. Check bedsReseated= ")
		       TEXT("on the SweBreachTick lines: a carve inside the sheet that moved it is a carve the sheet saw."),
		       Phase, A.SweOwnedMismatched, A.MaxAbsDeltaVoxels, Run->BreachVoxelsRemoved);
	}
	else
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("SweBreachBeds phase=%s: every SWE-owned bed agrees with the live terrain. Hypothesis (b) ")
		       TEXT("(mis-seated beds) is REFUTED for this run; read the surge signature below on its own terms."),
		       Phase);
	}
	return A.SweOwnedMismatched;
}

void LogGlobalLine(const TCHAR* Tag, FRunRef Run, int32 SampleIdx, double ElapsedSeconds)
{
	UWorld* W = WorldOf(Run);
	UVoxelWaterSubsystem* Water = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return;
	}
	FVoxelSweProbe S;
	Water->GetSweProbe(S); // CaVolume is filled in either way; the rest only when armed
	const int64 Sum = S.SheetVolume + S.CaVolume;
	// bedsReseated/pendingReseats are on this line because `punctured` alone
	// cannot tell "the carve did nothing" from "the carve was never noticed".
	// punctured is a PER-TICK count that only ever fires for a column the sheet
	// OWNED at the instant of the carve, and only for demoteDwellTicks after it;
	// a notch cut through a rim hits columns that are mostly not owned, so
	// punctured=0 is the expected reading for a breach that the sheet HAS seen.
	// bedsReseated is the reading that says it saw it.
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("%s n=%d t=%+.1fs armed=%d sheet=%lld ca=%lld sum=%lld poured=%lld sumMinusPoured=%+lld ")
	       TEXT("injected=%lld sumMinusInjected=%+lld consFail=%lld sweCols=%d punctured=%d toCA=%lld toSWE=%lld ")
	       TEXT("bedsReseated=%lld pendingReseats=%d"),
	       Tag, SampleIdx, ElapsedSeconds, S.bArmed ? 1 : 0, (long long)S.SheetVolume, (long long)S.CaVolume,
	       (long long)Sum, (long long)Run->ActuallyPouredUnits, (long long)(Sum - Run->ActuallyPouredUnits),
	       (long long)S.Injected, (long long)(S.bArmed ? Sum - S.Injected : 0), (long long)S.ConservationFailures,
	       S.SweColumns, S.LastPunctured, (long long)S.TransferredToCA, (long long)S.TransferredToSWE,
	       (long long)S.BedsReseated, S.PendingBedReseats);
}

// ===========================================================================
// STAGES
// ===========================================================================

// Stage 1: resolve subsystems, choose the site, pose, start the residency poll.
void StageBegin(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWorldSubsystem* Terrain = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	UVoxelWaterSubsystem* Water = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Terrain || !Water)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelSweBreachTest: subsystems not ready."));
		Finish(Run, TEXT("subsystems missing"));
		return;
	}

	// --- Site selection -----------------------------------------------------
	//
	// The fixture is worthless on flat ground, so the site is chosen for
	// SUSTAINED downhill: the score is the drop over kSlopeRunVoxels in the
	// best of 4 axis directions, and only monotone descents (each third of the
	// run lower than the last) count -- a single cliff step followed by a
	// terrace would give the same total drop and no runout.
	double BestGrade = -1.0;
	double BestX = 0.0, BestY = 0.0;
	int32 BestDirI = 1, BestDirJ = 0;
	static const int32 DI[4] = {1, -1, 0, 0};
	static const int32 DJ[4] = {0, 0, 1, -1};

	if (Run->bForcedSite)
	{
		BestX = Run->SiteXUU;
		BestY = Run->SiteYUU;
		// Still score the forced site, so the log says how usable it is.
		const double H0 = Terrain->GetSurfaceHeightUU(BestX, BestY);
		for (int32 D = 0; D < 4; ++D)
		{
			const double RunUU = double(kSlopeRunVoxels) * VoxelCoords::VoxelSizeUU;
			const double H3 = Terrain->GetSurfaceHeightUU(BestX + DI[D] * RunUU, BestY + DJ[D] * RunUU);
			const double Grade = (H0 - H3) / RunUU;
			if (Grade > BestGrade)
			{
				BestGrade = Grade;
				BestDirI = DI[D];
				BestDirJ = DJ[D];
			}
		}
	}
	else
	{
		const int32 Steps = int32(Run->SearchRadiusM / kSiteGridStepM);
		const double StepUU = kSiteGridStepM * 100.0;
		const double RunUU = double(kSlopeRunVoxels) * VoxelCoords::VoxelSizeUU;
		double OriginX = 0.0, OriginY = 0.0;
		// Search around the requested spawn column so -VoxelSpawnAt still aims
		// the whole run, exactly as it does for the other water fixtures.
		{
			FString SpawnAtArg;
			if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSpawnAt="), SpawnAtArg, false))
			{
				FString XStr, YStr;
				if (SpawnAtArg.Split(TEXT(","), &XStr, &YStr))
				{
					OriginX = FCString::Atod(*XStr) * 100.0;
					OriginY = FCString::Atod(*YStr) * 100.0;
				}
			}
		}

		for (int32 Iy = -Steps; Iy <= Steps; ++Iy)
		{
			for (int32 Ix = -Steps; Ix <= Steps; ++Ix)
			{
				const double Cx = OriginX + double(Ix) * StepUU;
				const double Cy = OriginY + double(Iy) * StepUU;
				const double H0 = Terrain->GetSurfaceHeightUU(Cx, Cy);
				if (H0 < kMinSiteHeightM * 100.0)
				{
					continue; // keep clear of sea level: no reservoir injection mid-run
				}
				for (int32 D = 0; D < 4; ++D)
				{
					const double H1 = Terrain->GetSurfaceHeightUU(Cx + DI[D] * RunUU / 3.0, Cy + DJ[D] * RunUU / 3.0);
					const double H2 = Terrain->GetSurfaceHeightUU(Cx + DI[D] * RunUU * 2.0 / 3.0,
					                                             Cy + DJ[D] * RunUU * 2.0 / 3.0);
					const double H3 = Terrain->GetSurfaceHeightUU(Cx + DI[D] * RunUU, Cy + DJ[D] * RunUU);
					if (!(H1 < H0 && H2 < H1 && H3 < H2))
					{
						continue; // not a monotone descent: a terrace, not a hillside
					}
					const double Grade = (H0 - H3) / RunUU;
					if (Grade > kMaxUsefulGrade)
					{
						continue; // too steep for one sphere to dam -- see kMaxUsefulGrade
					}
					if (Grade > BestGrade)
					{
						BestGrade = Grade;
						BestX = Cx;
						BestY = Cy;
						BestDirI = DI[D];
						BestDirJ = DJ[D];
					}
				}
			}
		}
	}

	// A FORCED site is honoured even if it is flat or uphill -- the operator
	// asked for that column, and the low-grade warning below still fires. Only
	// the automatic search is allowed to fail for want of relief.
	if (Run->bForcedSite && BestGrade < 0.0)
	{
		BestGrade = 0.0;
	}

	if (BestGrade < 0.0)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSweBreachTest: no monotone downhill run found within %.0f m of the spawn column above %.0f m ")
		       TEXT("elevation. Re-run with -VoxelSpawnAt=<X,Y in metres> pointed at known relief, or raise ")
		       TEXT("-VoxelSweBreachSearchM."),
		       Run->SearchRadiusM, kMinSiteHeightM);
		Finish(Run, TEXT("no sloped site"));
		return;
	}

	Run->SiteXUU = BestX;
	Run->SiteYUU = BestY;
	Run->SlopeDirI = BestDirI;
	Run->SlopeDirJ = BestDirJ;
	Run->SiteGrade = BestGrade;
	Run->SiteSurfaceUU = Terrain->GetSurfaceHeightUU(BestX, BestY);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest SITE: (%.1f, %.1f) m, surface %.2f m, downhill dir=(%d,%d), grade %.1f%% over a ")
	       TEXT("%.1f m run%s. armSwe=%d fillFraction=%.2f sampleWindow=%.1fs"),
	       BestX / 100.0, BestY / 100.0, Run->SiteSurfaceUU / 100.0, BestDirI, BestDirJ, BestGrade * 100.0,
	       double(kSlopeRunVoxels) * VoxelCoords::VoxelSizeUU / 100.0, Run->bForcedSite ? TEXT(" (FORCED)") : TEXT(""),
	       Run->bArmSwe ? 1 : 0, Run->FillFraction, Run->SampleSeconds);

	if (BestGrade < kMinUsefulGrade)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSweBreachTest: the best site found has a grade of only %.1f%%, below the %.0f%% this fixture ")
		       TEXT("needs. A near-flat site is exactly the case that CANNOT distinguish correct thin-film spreading ")
		       TEXT("from mis-seated beds -- which is the entire reason this test exists. The run will continue and the ")
		       TEXT("BED AUDIT below is still valid, but treat the surge numbers as INCONCLUSIVE."),
		       BestGrade * 100.0, kMinUsefulGrade * 100.0);
	}

	// Pose: stand off to the side of the flow so the surge crosses the frame
	// left-to-right rather than running away from the camera.
	{
		const FVector Site(BestX, BestY, Run->SiteSurfaceUU);
		const FVector Flow(double(BestDirI), double(BestDirJ), 0.0);
		const FVector Perp(-Flow.Y, Flow.X, 0.0);
		const FVector Look = Site + Flow * 250.0; // 2.5 m downstream of the basin
		// High and well off to the side: the perpendicular ground can itself
		// rise several metres over this stand-off distance on a 45% site, and a
		// camera buried in rock films nothing.
		Run->CameraUU = Site + Perp * 700.0 - Flow * 300.0 + FVector(0, 0, 700.0);
		Run->CameraRot = FRotationMatrix::MakeFromX(Look - Run->CameraUU).Rotator();
		PoseCamera(Run);
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest: posed at (%.0f,%.0f,%.0f) rot(pitch %.1f yaw %.1f). Waiting for terrain residency ")
	       TEXT("to go quiet BEFORE carving (poll %.0fs, cap %d) -- a carve into a chunk that has not streamed is a ")
	       TEXT("survey of the wrong ground."),
	       Run->CameraUU.X, Run->CameraUU.Y, Run->CameraUU.Z, Run->CameraRot.Pitch, Run->CameraRot.Yaw,
	       kPollIntervalSeconds, kMaxTerrainPolls);

	SetTimerLooping(Run, Run->TerrainPollHandle, kPollIntervalSeconds, &StageTerrainPoll);
}

// Stage 2: hold the pose until streaming goes quiet.
void StageTerrainPoll(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWorldSubsystem* Terrain = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Terrain)
	{
		Finish(Run, TEXT("terrain subsystem lost"));
		return;
	}
	++Run->TerrainPolls;
	PoseCamera(Run);

	const FVoxelPerfSnapshot Snap = Terrain->GetPerfSnapshot();
	const bool bQuiet = Snap.JobsInFlight == 0 && Snap.PendingJobQueueDepth == 0 &&
	                    Snap.PendingGameThreadQueueDepth == 0 && Snap.PendingUnloadQueueDepth == 0 &&
	                    Snap.ChunksLoadedPerSec <= 0.f;

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest terrain-poll %d/%d: inFlight=%d pendingJob=%d pendingGT=%d pendingUnload=%d ")
	       TEXT("loaded/s=%.1f -> %s"),
	       Run->TerrainPolls, kMaxTerrainPolls, Snap.JobsInFlight, Snap.PendingJobQueueDepth,
	       Snap.PendingGameThreadQueueDepth, Snap.PendingUnloadQueueDepth, Snap.ChunksLoadedPerSec,
	       bQuiet ? TEXT("QUIET") : TEXT("still streaming"));

	if (!bQuiet && Run->TerrainPolls < kMaxTerrainPolls)
	{
		return;
	}
	W->GetTimerManager().ClearTimer(Run->TerrainPollHandle);
	if (!bQuiet)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSweBreachTest: terrain did NOT go quiet within %d polls; continuing anyway. The survey below ")
		       TEXT("may disagree with what is finally resident."),
		       kMaxTerrainPolls);
	}
	SetTimerOnce(Run, Run->StageHandle, 0.5f, &StageCarveAndSurvey);
}

// Stage 3: carve the basin, survey it, and derive spill level, capacity, pour
// volume, expected settled level, rim and probe set.
void StageCarveAndSurvey(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWorldSubsystem* Terrain = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Terrain)
	{
		Finish(Run, TEXT("terrain subsystem lost"));
		return;
	}

	// --- The basin (see kBasinRadiusVoxels for why it is a stack) -----------
	const int64 SurfaceVz = int64(FMath::FloorToDouble(Run->SiteSurfaceUU / VoxelCoords::VoxelSizeUU));
	int32 BasinRemoved = 0;
	int32 BasinSpheres = 0;
	for (int32 Dz = kBasinTopAboveSurfaceVoxels; Dz >= -kBasinBottomBelowSurfaceVoxels; Dz -= kBasinStepVoxels)
	{
		BasinRemoved += Terrain->CarveSphere(FVector(Run->SiteXUU, Run->SiteYUU, VxToUU(SurfaceVz + Dz)),
		                                     double(kBasinRadiusVoxels) * VoxelCoords::VoxelSizeUU, kBasinCarveJitterUU);
		++BasinSpheres;
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest BASIN: carved %d voxel(s) with %d r=%d-voxel sphere(s) stacked from %d above to %d ")
	       TEXT("below the surface voxel z=%lld at (%.0f,%.0f) -- an open pit, no lidded annulus."),
	       BasinRemoved, BasinSpheres, kBasinRadiusVoxels, kBasinTopAboveSurfaceVoxels, kBasinBottomBelowSurfaceVoxels,
	       (long long)SurfaceVz, Run->SiteXUU, Run->SiteYUU);

	// --- The survey ---------------------------------------------------------
	const int64 SiteVx = UUToVx(Run->SiteXUU);
	const int64 SiteVy = UUToVx(Run->SiteYUU);
	SurveyHeightField(*Terrain, Run->Field, SiteVx - kSurveyColumns / 2, SiteVy - kSurveyColumns / 2, kSurveyColumns);
	Run->SeedI = kSurveyColumns / 2;
	Run->SeedJ = kSurveyColumns / 2;

	const FHeightField& H = Run->Field;
	const int32 SeedTop = H.Top[H.Idx(Run->SeedI, Run->SeedJ)];

	// --- Spill level --------------------------------------------------------
	TArray<int32> Region, BestRegion;
	TArray<uint8> Visited;
	bool bEscaped = false;
	int32 Spill = INT32_MIN;
	for (int32 Level = SeedTop + 1; Level <= SeedTop + kSurveyScanDown; ++Level)
	{
		FloodAtLevel(H, Run->SeedI, Run->SeedJ, Level, Region, bEscaped, Visited);
		if (bEscaped || Region.Num() == 0)
		{
			break;
		}
		Spill = Level;
		BestRegion = Region;
	}
	if (Spill == INT32_MIN)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelSweBreachTest: the carve did NOT produce a closed basin -- water at the very first level ")
		       TEXT("already escapes the %d-column survey window. The site is too steep or the carve too shallow for "
		            "this geometry; nothing downstream of here would mean anything."),
		       kSurveyColumns);
		Finish(Run, TEXT("no closed basin"));
		return;
	}
	Run->SpillLevel = Spill;
	Run->SpillCapacityVoxels = RegionCapacityVoxels(H, BestRegion, Spill);

	// --- Pour volume and the level it implies -------------------------------
	Run->TargetFillUnits = int64(double(Run->SpillCapacityVoxels) * 255.0 * Run->FillFraction);
	int32 PourLevel = Spill;
	TArray<int32> PourRegion = BestRegion;
	int64 CapBelow = 0;
	for (int32 Level = SeedTop + 1; Level <= Spill; ++Level)
	{
		FloodAtLevel(H, Run->SeedI, Run->SeedJ, Level, Region, bEscaped, Visited);
		const int64 Cap = RegionCapacityVoxels(H, Region, Level);
		if (Cap * 255 >= Run->TargetFillUnits)
		{
			PourLevel = Level;
			PourRegion = Region;
			// Capacity one level down, for the fractional interpolation below.
			if (Level > SeedTop + 1)
			{
				TArray<int32> Below;
				bool bE = false;
				FloodAtLevel(H, Run->SeedI, Run->SeedJ, Level - 1, Below, bE, Visited);
				CapBelow = RegionCapacityVoxels(H, Below, Level - 1);
			}
			break;
		}
	}
	Run->PourLevel = PourLevel;
	{
		// A water level is only an integer if the volume happens to land on
		// one. Interpolate inside the last level using the flooded AREA at that
		// level -- capacity grows at one voxel per column per level.
		const double Area = double(PourRegion.Num());
		const double Vol = double(Run->TargetFillUnits) / 255.0;
		Run->ExpectedLevelVz = Area > 0.0 ? (double(PourLevel - 1) + (Vol - double(CapBelow)) / Area) : double(PourLevel);
	}

	// --- The rim: the lowest column adjacent to the basin at spill level -----
	//
	// Every neighbour of the spill-level region has Top >= Spill (a lower one
	// would have been inside the region), so the minimum IS the gate the basin
	// would overtop through. Ties are broken by the steepest runout beyond it,
	// which is what makes the breach drain somewhere instead of into a puddle.
	{
		TArray<uint8> InRegion;
		InRegion.Init(0, H.Top.Num());
		for (int32 Idx : BestRegion)
		{
			InRegion[Idx] = 1;
		}
		static const int32 NDI[4] = {1, -1, 0, 0};
		static const int32 NDJ[4] = {0, 0, 1, -1};
		int32 BestRimIdx = INDEX_NONE;
		int32 BestRimTop = INT32_MAX;
		double BestRunout = -1e30;
		int32 BestOutI = Run->SlopeDirI, BestOutJ = Run->SlopeDirJ;
		for (int32 Idx : BestRegion)
		{
			const int32 I = Idx % H.W;
			const int32 J = Idx / H.W;
			for (int32 D = 0; D < 4; ++D)
			{
				const int32 Ni = I + NDI[D];
				const int32 Nj = J + NDJ[D];
				if (!H.InRange(Ni, Nj) || InRegion[H.Idx(Ni, Nj)])
				{
					continue;
				}
				const int32 NIdx = H.Idx(Ni, Nj);
				// Runout: how far the ground keeps falling past this candidate.
				const int32 Fi = FMath::Clamp(Ni + NDI[D] * 16, 0, H.W - 1);
				const int32 Fj = FMath::Clamp(Nj + NDJ[D] * 16, 0, H.W - 1);
				const double Runout = double(H.Top[NIdx] - H.Top[H.Idx(Fi, Fj)]);
				if (H.Top[NIdx] < BestRimTop || (H.Top[NIdx] == BestRimTop && Runout > BestRunout))
				{
					BestRimTop = H.Top[NIdx];
					BestRunout = Runout;
					BestRimIdx = NIdx;
					BestOutI = NDI[D];
					BestOutJ = NDJ[D];
				}
			}
		}
		if (BestRimIdx == INDEX_NONE)
		{
			UE_LOG(LogVoxelEarth, Error, TEXT("VoxelSweBreachTest: the basin has no rim column (survey window too small?)."));
			Finish(Run, TEXT("no rim"));
			return;
		}
		Run->RimVx = H.VxOf(BestRimIdx % H.W);
		Run->RimVy = H.VyOf(BestRimIdx / H.W);
		Run->RimTop = BestRimTop;
		Run->OutI = BestOutI;
		Run->OutJ = BestOutJ;
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest BASIN GEOMETRY: seedTop=%d spillLevel=%d spillCapacity=%lld voxels ")
	       TEXT("(%lld fill units) | pourTarget=%lld fill units (%.0f%% of capacity) -> pourLevel=%d ")
	       TEXT("expectedSettledSurfaceVz=%.3f | rim=(%lld,%lld) rimTop=%d outflowDir=(%d,%d)"),
	       SeedTop, Run->SpillLevel, (long long)Run->SpillCapacityVoxels, (long long)(Run->SpillCapacityVoxels * 255),
	       (long long)Run->TargetFillUnits, Run->FillFraction * 100.0, Run->PourLevel, Run->ExpectedLevelVz,
	       (long long)Run->RimVx, (long long)Run->RimVy, Run->RimTop, Run->OutI, Run->OutJ);

	// --- The probe set ------------------------------------------------------
	//
	// One column inside the basin, one just upstream of the mouth, the mouth
	// itself, and four downstream. The downstream spacing is what turns the
	// velocity log into an ARRIVAL TIME: a surge shows up at ds+2 before ds+16.
	TSet<int32> PourPool;
	PourPool.Reserve(PourRegion.Num());
	for (int32 Idx : PourRegion)
	{
		PourPool.Add(Idx);
	}
	auto AddProbe = [&](const TCHAR* Name, int64 Vx, int64 Vy)
	{
		FProbe P;
		P.Name = Name;
		P.Vx = Vx;
		P.Vy = Vy;
		const int32 I = int32(Vx - H.OriginVx);
		const int32 J = int32(Vy - H.OriginVy);
		if (H.InRange(I, J))
		{
			P.SurveyTop = H.Top[H.Idx(I, J)];
			P.bInPourPool = PourPool.Contains(H.Idx(I, J));
		}
		Run->Probes.Add(P);
	};
	AddProbe(TEXT("basin"), H.VxOf(Run->SeedI), H.VyOf(Run->SeedJ));
	AddProbe(TEXT("up-4"), Run->RimVx - Run->OutI * 4, Run->RimVy - Run->OutJ * 4);
	AddProbe(TEXT("mouth"), Run->RimVx, Run->RimVy);
	AddProbe(TEXT("ds+2"), Run->RimVx + Run->OutI * 2, Run->RimVy + Run->OutJ * 2);
	AddProbe(TEXT("ds+4"), Run->RimVx + Run->OutI * 4, Run->RimVy + Run->OutJ * 4);
	AddProbe(TEXT("ds+8"), Run->RimVx + Run->OutI * 8, Run->RimVy + Run->OutJ * 8);
	AddProbe(TEXT("ds+16"), Run->RimVx + Run->OutI * 16, Run->RimVy + Run->OutJ * 16);
	Run->PeakSpeedMmPerSec.Init(0, Run->Probes.Num());
	Run->PeakSampleIndex.Init(-1, Run->Probes.Num());
	Run->PeakAlongOutMmPerSec.Init(0, Run->Probes.Num());
	Run->FirstWaterSampleIndex.Init(-1, Run->Probes.Num());

	// The runout profile, so the reader can see the ground actually falls away
	// past the breach rather than taking it on trust.
	{
		FString Profile;
		for (int32 D = 0; D <= 24; D += 4)
		{
			const int32 I = FMath::Clamp(int32(Run->RimVx - H.OriginVx) + Run->OutI * D, 0, H.W - 1);
			const int32 J = FMath::Clamp(int32(Run->RimVy - H.OriginVy) + Run->OutJ * D, 0, H.W - 1);
			Profile += FString::Printf(TEXT("+%d:%d "), D, H.Top[H.Idx(I, J)]);
		}
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSweBreachTest RUNOUT (topmost solid voxel z, downstream of the rim): %s| a MONOTONE FALL here ")
		       TEXT("is what makes a surge possible; a flat or rising profile means the breach drains into a puddle and ")
		       TEXT("the velocity numbers below will be small for a correct reason."),
		       *Profile);
	}

	// Store the pour region on the run by re-flooding in StagePour (cheap, and
	// keeps this stage from carrying a second copy of the field).
	SetTimerOnce(Run, Run->StageHandle, 0.5f, &StagePour);
}

// Stage 4: pour the measured volume into the measured basin.
void StagePour(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWaterSubsystem* Water = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		Finish(Run, TEXT("water subsystem lost"));
		return;
	}

	const FHeightField& H = Run->Field;
	TArray<int32> Region;
	TArray<uint8> Visited;
	bool bEscaped = false;
	FloodAtLevel(H, Run->SeedI, Run->SeedJ, Run->PourLevel, Region, bEscaped, Visited);

	// Per-column hydrostatic amounts, then an exact fix-up so the total is the
	// measured target to the unit. A "pour" here means placing each column's
	// own share at its own floor, not dropping one tall tower and waiting: the
	// subject of this test is the BREACH, and starting from a known, settled,
	// geometrically-derived pool is what makes the post-breach numbers mean
	// something. Every unit still goes through the ordinary SpawnWaterAt ->
	// WaterCA::addWater path, so nothing about the state is special-cased.
	TArray<int64> Amounts;
	Amounts.SetNumZeroed(Region.Num());
	int64 Sum = 0;
	for (int32 K = 0; K < Region.Num(); ++K)
	{
		const double Depth = FMath::Max(0.0, Run->ExpectedLevelVz - double(H.Top[Region[K]]));
		Amounts[K] = int64(Depth * 255.0);
		Sum += Amounts[K];
	}
	int64 Diff = Run->TargetFillUnits - Sum;
	for (int32 K = 0; Diff != 0 && Region.Num() > 0; K = (K + 1) % Region.Num())
	{
		if (Diff > 0)
		{
			++Amounts[K];
			--Diff;
		}
		else if (Amounts[K] > 0)
		{
			--Amounts[K];
			++Diff;
		}
	}

	int64 Placed = 0;
	int32 Columns = 0;
	for (int32 K = 0; K < Region.Num(); ++K)
	{
		if (Amounts[K] <= 0)
		{
			continue;
		}
		const int32 I = Region[K] % H.W;
		const int32 J = Region[K] / H.W;
		const FVector Loc(VxToUU(H.VxOf(I)), VxToUU(H.VyOf(J)), VxToUU(int64(H.Top[Region[K]]) + 1));
		Placed += int64(Water->SpawnWaterAt(Loc, uint32(FMath::Min<int64>(Amounts[K], MAX_uint32))));
		++Columns;
	}
	Run->ActuallyPouredUnits = Placed;
	Run->PourColumns = Columns;

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest POUR: placed %lld / %lld fill units across %d column(s) at the basin floor ")
	       TEXT("(%.2f m3 of water; 255 fill units = 1 voxel = 1 litre at kVoxelSizeMm=100). Waiting for the CA to ")
	       TEXT("settle (poll %.0fs, cap %d)."),
	       (long long)Placed, (long long)Run->TargetFillUnits, Columns, double(Placed) / 255.0 * 0.001,
	       kPollIntervalSeconds, kMaxSettlePolls);

	if (Placed != Run->TargetFillUnits)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSweBreachTest: %lld fill unit(s) of the intended pour were REFUSED by the CA. Every ledger ")
		       TEXT("comparison below uses the ACTUAL placed total (%lld), so the conservation check stays honest."),
		       (long long)(Run->TargetFillUnits - Placed), (long long)Placed);
	}

	Run->SettlePolls = 0;
	SetTimerLooping(Run, Run->SettlePollHandle, kPollIntervalSeconds, &StageSettlePoll);
}

// Stage 5: wait for the pool to stop moving.
void StageSettlePoll(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWaterSubsystem* Water = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		Finish(Run, TEXT("water subsystem lost"));
		return;
	}
	++Run->SettlePolls;
	PoseCamera(Run);

	const FVoxelWaterPerfSnapshot Snap = Water->GetPerfSnapshot();
	const bool bSettled = Snap.ActiveBricks == 0;
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest settle-poll %d/%d (%s): activeBricks=%lld storedBricks=%lld volume=%llu maxFill=%d ")
	       TEXT("-> %s"),
	       Run->SettlePolls, kMaxSettlePolls, Run->bPostBreachSettle ? TEXT("post-breach") : TEXT("pre-arm"),
	       Snap.ActiveBricks, Snap.StoredBricks, (unsigned long long)Snap.TotalVolume, Water->GetMaxStoredFill(),
	       bSettled ? TEXT("SETTLED") : TEXT("still moving"));

	if (!bSettled && Run->SettlePolls < kMaxSettlePolls)
	{
		return;
	}
	W->GetTimerManager().ClearTimer(Run->SettlePollHandle);
	if (!bSettled)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSweBreachTest: water still active after %d polls (%s). The level report that follows is a ")
		       TEXT("snapshot of a MOVING surface, not a settled one -- read the deltas accordingly."),
		       kMaxSettlePolls, Run->bPostBreachSettle ? TEXT("post-breach") : TEXT("pre-arm"));
	}
	SetTimerOnce(Run, Run->StageHandle, 0.5f, Run->bPostBreachSettle ? &StageFinalReport : &StageArmSwe);
}

// Stage 6: arm (or deliberately do not arm) the shallow-water layer.
//
// ORDER MATTERS. MaybeArmSwe centres the single dense 128x128 sheet on the
// centroid of the CA's stored bricks, falling back to the player's viewpoint.
// Arming AFTER the pool exists and has settled therefore centres the sheet on
// the basin exactly, and exercises the seeding path (forcePromote over every
// water-bearing column with a real bed) -- which is precisely the code the
// bed-seating hypothesis is about.
void StageArmSwe(FRunRef Run)
{
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Water.SWE")))
	{
		// SetByConsole, not SetByCode: -ExecCmds="voxel.Water.SWE ..." lands at
		// console priority, and a SetByCode write is silently REJECTED under
		// one. This switch owns the flag for the duration of the run, so a
		// recipe that sets it both ways cannot end up measuring the opposite of
		// what its command line says. Use -VoxelSweBreachSwe=0|1, not -ExecCmds.
		CVar->Set(Run->bArmSwe ? TEXT("1") : TEXT("0"), ECVF_SetByConsole);
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSweBreachTest: set voxel.Water.SWE %d (the sheet centres on the settled pool, and the seeding ")
		       TEXT("path force-promotes every water-bearing column with a real bed)."),
		       Run->bArmSwe ? 1 : 0);
	}
	else
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelSweBreachTest: voxel.Water.SWE cvar not found; running CA-only."));
	}
	// Arming happens in UVoxelWaterSubsystem::Tick, and the coupler needs a few
	// fixed steps to settle membership, so give it a beat before measuring.
	SetTimerOnce(Run, Run->StageHandle, 3.f, &StagePreBreachReport);
}

// Stage 7: the pre-breach state of record -- the bed audit and the settled
// level report, taken while nothing has been dug.
void StagePreBreachReport(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWaterSubsystem* Water = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		Finish(Run, TEXT("water subsystem lost"));
		return;
	}
	PoseCamera(Run);

	FVoxelSweProbe S;
	const bool bArmed = Water->GetSweProbe(S);
	if (Run->bArmSwe && !bArmed)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSweBreachTest: voxel.Water.SWE was requested but the sheet is NOT armed. The usual cause is a ")
		       TEXT("net mode other than NM_Standalone (ADR-0004 item 3 refuses those on purpose) -- check LogVoxelWater ")
		       TEXT("for the refusal line. Continuing as a CA-only run, which is still a valid control."));
	}
	if (bArmed)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSweBreachTest SHEET: origin=(%lld,%lld) size=%dx%d columns, sweColumns=%d, ")
		       TEXT("sheetScanVoxels=%d, settleTolerance=%d fill units (%.3f voxels -- swe.h S4's derived deadband, ")
		       TEXT("the flatness a settled sheet is entitled to)."),
		       (long long)S.OriginVx, (long long)S.OriginVy, S.SizeX, S.SizeY, S.SweColumns, S.SheetScanVoxels,
		       S.SettleToleranceFill, double(S.SettleToleranceFill) / 255.0);
	}

	int32 MobBricks = 0;
	uint64 Deb = 0, Cred = 0, Short = 0;
	Water->GetMobilizationStats(MobBricks, Deb, Cred, Short);
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest mobilization: bricks=%d debited=%llu credited=%llu shortfall=%llu (shortfall MUST be ")
	       TEXT("0; a nonzero value means implicit water entered the ledger and the conservation columns below are ")
	       TEXT("contaminated)."),
	       MobBricks, (unsigned long long)Deb, (unsigned long long)Cred, (unsigned long long)Short);

	Run->PreBreachBedMismatches = LogBedAudit(TEXT("pre-breach"), Run);
	LogLevelReport(TEXT("pre-breach"), Run);
	LogGlobalLine(TEXT("SweBreachTick"), Run, -1, -1.0);

	FScreenshotRequest::RequestScreenshot(TEXT("VoxelSweBreach_0_prebreach"), false, true);
	SetTimerOnce(Run, Run->StageHandle, 1.5f, &StageBreach);
}

// Stage 8: BREACH. Two spheres through the measured rim, below the waterline,
// via UVoxelWorldSubsystem::CarveSphere -- the same edit-log authority path
// digging uses, so the terrain-edit notifications (which wake the CA and drive
// the coupler's puncture test) fire exactly as they do for a player.
void StageBreach(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWorldSubsystem* Terrain = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Terrain)
	{
		Finish(Run, TEXT("terrain subsystem lost"));
		return;
	}

	// AN OPEN NOTCH, NOT A TUNNEL, and this is not a cosmetic choice. The
	// coupler's eligibility predicate (swe.h S5) requires openClearanceVoxels of
	// non-solid space above the bed with NO LID -- "a flooded tunnel is NOT a
	// free surface". A single sphere cut below the waterline would leave the
	// rim's upper voxels arching over the mouth, the mouth column would fail
	// eligibility, demote to the CA, and report velocity 0 forever: the fixture
	// would have destroyed the exact measurement it exists to take. So the
	// breach is a stack of spheres from below the waterline up past the rim,
	// cut at two positions across the wall so the channel goes all the way
	// through. Stepped by less than the radius so the stack is continuous.
	const int32 BottomVz = int32(FMath::FloorToDouble(Run->ExpectedLevelVz)) - kBreachBelowSurfaceVoxels;
	const int32 TopVz = Run->RimTop + 4;
	const int32 StepVoxels = 4;
	int32 Removed = 0;
	int32 Spheres = 0;
	for (int32 Lateral = 0; Lateral <= kBreachOutwardVoxels; Lateral += kBreachOutwardVoxels)
	{
		const int64 Cvx = Run->RimVx + Run->OutI * Lateral;
		const int64 Cvy = Run->RimVy + Run->OutJ * Lateral;
		for (int32 Vz = BottomVz; Vz <= TopVz; Vz += StepVoxels)
		{
			Removed += Terrain->CarveSphere(FVector(VxToUU(Cvx), VxToUU(Cvy), VxToUU(Vz)), kBreachRadiusUU, 0.0);
			++Spheres;
		}
	}
	Run->BreachVoxelsRemoved = Removed;

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest BREACH: carved %d voxel(s) with %d r=%.0fUU sphere(s), an OPEN notch from voxel z=%d ")
	       TEXT("(%d below the expected water surface %.2f) up to z=%d (%d above the rim), at the measured rim ")
	       TEXT("(%lld,%lld) and %d voxel(s) outward along the outflow direction (%d,%d). Sampling every %.2fs for ")
	       TEXT("%.1fs from here."),
	       Removed, Spheres, kBreachRadiusUU, BottomVz, kBreachBelowSurfaceVoxels, Run->ExpectedLevelVz, TopVz,
	       TopVz - Run->RimTop, (long long)Run->RimVx, (long long)Run->RimVy, kBreachOutwardVoxels, Run->OutI, Run->OutJ,
	       kSampleIntervalSeconds, Run->SampleSeconds);

	if (Run->BreachVoxelsRemoved == 0)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSweBreachTest: the breach carve removed NOTHING -- the rim was already open there. Every ")
		       TEXT("velocity number below is measuring a pool that was never dammed."));
	}

	Run->Samples = 0;
	Run->MaxSamples = FMath::Max(1, int32(Run->SampleSeconds / kSampleIntervalSeconds));
	SetTimerLooping(Run, Run->SampleHandle, kSampleIntervalSeconds, &StageSample);
}

// Stage 9: the surge time series. One global line and one line per probe
// column per sample, at the subsystem's own 10 Hz fixed-step cadence.
void StageSample(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	if (!W)
	{
		Finish(Run, TEXT("world lost"));
		return;
	}
	const double T = double(Run->Samples) * double(kSampleIntervalSeconds);
	LogGlobalLine(TEXT("SweBreachTick"), Run, Run->Samples, T);
	SampleAllProbes(TEXT("SweBreachVel"), Run, Run->Samples, T, /*bTrackPeaks*/ true);

	if (Run->Samples == 5)
	{
		FScreenshotRequest::RequestScreenshot(TEXT("VoxelSweBreach_1_surge"), false, true);
	}
	else if (Run->Samples == 20)
	{
		FScreenshotRequest::RequestScreenshot(TEXT("VoxelSweBreach_2_running"), false, true);
	}

	++Run->Samples;
	if (Run->Samples < Run->MaxSamples)
	{
		return;
	}
	W->GetTimerManager().ClearTimer(Run->SampleHandle);
	// Let it settle again before the closing state of record -- polled, not a
	// fixed wait, because how long a breached basin takes to empty depends on
	// the notch and the volume, neither of which is known in advance.
	Run->bPostBreachSettle = true;
	Run->SettlePolls = 0;
	SetTimerLooping(Run, Run->SettlePollHandle, kPollIntervalSeconds, &StageSettlePoll);
}

// Stage 10: the closing state of record, the verdict line, and the exit.
void StageFinalReport(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWaterSubsystem* Water = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		Finish(Run, TEXT("water subsystem lost"));
		return;
	}
	PoseCamera(Run);

	LogGlobalLine(TEXT("SweBreachTick"), Run, -2, -2.0);
	LogLevelReport(TEXT("post-breach"), Run);
	LogBedAudit(TEXT("post-breach"), Run);

	// --- The summary ---------------------------------------------------------
	//
	// Three numbers per probe. See FSweBreachRun's comment on the peak arrays
	// for why the mouth is NOT where the sheet's surge is expected to be.
	FString Peaks;
	int32 BasinIdx = INDEX_NONE, UpIdx = INDEX_NONE;
	TArray<int32> DownstreamIdx;
	for (int32 I = 0; I < Run->Probes.Num(); ++I)
	{
		Peaks += FString::Printf(TEXT("%s[peak=%dmm/s@n=%d alongOut=%+dmm/s firstWater@n=%d] "), *Run->Probes[I].Name,
		                         Run->PeakSpeedMmPerSec[I], Run->PeakSampleIndex[I], Run->PeakAlongOutMmPerSec[I],
		                         Run->FirstWaterSampleIndex[I]);
		if (Run->Probes[I].Name == TEXT("basin"))
		{
			BasinIdx = I;
		}
		else if (Run->Probes[I].Name == TEXT("up-4"))
		{
			UpIdx = I;
		}
		else if (Run->Probes[I].Name.StartsWith(TEXT("ds+")))
		{
			DownstreamIdx.Add(I);
		}
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest SURGE SUMMARY (samples are %.2fs apart; peak = max |velocityAt|, alongOut = the ")
	       TEXT("largest signed projection of that velocity on the outflow direction (%d,%d), firstWater = the first ")
	       TEXT("sample at which the column held ANY water from either solver): %s"),
	       kSampleIntervalSeconds, Run->OutI, Run->OutJ, *Peaks);

	// Did the front travel? firstWater must be non-decreasing with distance.
	bool bFrontOrdered = true;
	bool bFrontArrived = false;
	int32 PrevArrival = -1;
	for (int32 I : DownstreamIdx)
	{
		const int32 Arr = Run->FirstWaterSampleIndex[I];
		if (Arr < 0)
		{
			continue; // never wetted; ordering is unaffected, reach is
		}
		bFrontArrived = true;
		if (Arr < PrevArrival)
		{
			bFrontOrdered = false;
		}
		PrevArrival = Arr;
	}

	FVoxelSweProbe S;
	const bool bArmed = Water->GetSweProbe(S);
	const int32 BasinDrawdown =
		(BasinIdx != INDEX_NONE ? FMath::Abs(Run->PeakAlongOutMmPerSec[BasinIdx]) : 0) +
		(UpIdx != INDEX_NONE ? FMath::Abs(Run->PeakAlongOutMmPerSec[UpIdx]) : 0);

	if (!bArmed)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSweBreachTest VERDICT: CA-ONLY control run (voxel.Water.SWE off or refused). Every velocity ")
		       TEXT("above is 0 BY CONSTRUCTION -- waterca.h Phase C computes a static equilibrium level and models no ")
		       TEXT("momentum at all, which is the gap W4 exists to fill. The comparable numbers against the ")
		       TEXT("-VoxelSweBreachSwe=1 run are the firstWater arrival indices (how fast the front travelled ")
		       TEXT("downstream: front arrived=%d, ordered by distance=%d) and the SweBreachLevel deltas."),
		       bFrontArrived ? 1 : 0, bFrontOrdered ? 1 : 0);
	}
	else if (BasinDrawdown == 0 && S.TransferredToCA == 0)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSweBreachTest VERDICT: the sheet was ARMED but NOTHING happened on the SWE side -- no directed ")
		       TEXT("velocity anywhere in the basin and not one fill unit crossed to the CA. That is not 'SWE spreads ")
		       TEXT("too much'; it is the coupling failing to engage at all, and it invalidates the whole comparison. ")
		       TEXT("Read bedsReseated= on the SweBreachTick lines FIRST -- it splits this into two very different ")
		       TEXT("failures. bedsReseated=0 with a carve inside the sheet means the sheet never noticed the carve, ")
		       TEXT("which is the 2026-07-29 bug (columns left resting on air are frozen out of the sheet forever); ")
		       TEXT("that is a wiring failure and nothing downstream of it means anything. bedsReseated>0 with no ")
		       TEXT("drawdown means the sheet DID re-form around the notch and still could not spill through it, ")
		       TEXT("which is the open swe.h S5 gap: there is no SWE->CA channel at a LATERAL boundary, so an ")
		       TEXT("SWE-owned pool cannot pour into a CA-owned notch however much lower its bed is. See ")
		       TEXT("docs/adr/0007. Do not tune anything in either case."));
	}
	else
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSweBreachTest VERDICT: sheet armed. Basin-side drawdown peaked at %d mm/s directed along the ")
		       TEXT("outflow; %lld fill unit(s) crossed SWE->CA through the puncture channel; the downstream front ")
		       TEXT("arrived=%d and was ordered by distance=%d.\n")
		       TEXT("  HOW TO READ THIS. A CORRECT breach does NOT show its sheet velocity at the mouth: swe.h S5(a) ")
		       TEXT("makes a column whose bed voxel stops being solid a PUNCTURED, metered source into the CA, which ")
		       TEXT("then demotes after demoteDwellTicks -- confined flow is CA territory by the design of the split. ")
		       TEXT("So the signature of a correct surge is (i) directed alongOut velocity on 'basin' and 'up-4', ")
		       TEXT("growing then decaying, (ii) punctured>0 and toCA climbing in the SweBreachTick lines, and (iii) ")
		       TEXT("firstWater increasing with distance across ds+2/ds+4/ds+8/ds+16. All three present = the momentum ")
		       TEXT("state is real and SWE is doing the job the CA structurally cannot. Velocity present but ")
		       TEXT("undirected (alongOut near zero while peak is large), or a front that arrives everywhere at once, ")
		       TEXT("is not a surge."),
		       BasinDrawdown, (long long)S.TransferredToCA, bFrontArrived ? 1 : 0, bFrontOrdered ? 1 : 0);
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest LEDGER: poured=%lld sheet=%lld ca=%lld sum=%lld sumMinusPoured=%+lld ")
	       TEXT("conservationFailures=%lld. sumMinusPoured must be 0 -- a nonzero value with conservationFailures=0 ")
	       TEXT("means water entered or left OUTSIDE the coupled window (reservoir top-up, mobilization), not that the ")
	       TEXT("solvers lost any."),
	       (long long)Run->ActuallyPouredUnits, (long long)S.SheetVolume, (long long)S.CaVolume,
	       (long long)(S.SheetVolume + S.CaVolume),
	       (long long)(S.SheetVolume + S.CaVolume - Run->ActuallyPouredUnits), (long long)S.ConservationFailures);

	// --- The tuning question, answered only if it is legitimately open -------
	//
	// This block is GATED on the pre-breach bed audit coming back clean and on
	// the mouth actually surging, because those are exactly the two conditions
	// under which "tune SWE toward CA-style pooling" is a physics preference
	// rather than a way of hiding a defect.
	const bool bSurged = bArmed && BasinDrawdown > 0 && S.TransferredToCA > 0 && bFrontArrived && bFrontOrdered;
	if (!bArmed)
	{
		// Control run: nothing to tune.
	}
	else if (Run->PreBreachBedMismatches != 0 || !bSurged)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSweBreachTest TUNING: WITHHELD. preBreachBedMismatches=%d (must be 0), basinDrawdown=%d (must ")
		       TEXT("be >0), toCA=%lld (must be >0), frontArrived=%d frontOrdered=%d (both must be 1). Damping/")
		       TEXT("absorption tuning toward pooling is exactly what would BURY whichever of these failed, and would ")
		       TEXT("make attributing it later far harder. Fix the seating or the coupling first, re-run this fixture, ")
		       TEXT("and only then consider constants."),
		       Run->PreBreachBedMismatches, BasinDrawdown, (long long)S.TransferredToCA, bFrontArrived ? 1 : 0,
		       bFrontOrdered ? 1 : 0);
	}
	else
	{
		// The honest menu, with the honest costs. Read swe.h S4 and S5 with it.
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSweBreachTest TUNING: the beds are clean and the breach surged, so the pooling preference is a ")
		       TEXT("genuine physics choice and these are the only knobs that touch it.\n")
		       TEXT("  (1) SweConfig::dampingQ8 (224 today, Q8 per-tick flux retention). LOWERING it bleeds momentum ")
		       TEXT("faster, so the film creeps less far per unit of head. COST, and it is exact: swe.h S4's settle ")
		       TEXT("deadband is d_min = 2^gainShift * (256 - dampingQ8) / 256, so 224 -> 192 DOUBLES it from 16 to 32 ")
		       TEXT("fill units -- the settled surface goes from flat within 6.3%% of a voxel to flat within 12.5%%, ")
		       TEXT("which is a visible step. It also weakens the surge this fixture just measured, by the same factor.\n")
		       TEXT("  (2) SweConfig::gainShift (7 today). RAISING it to 8 halves the head-to-flux gain: slower to ")
		       TEXT("spread. COST: it doubles the same deadband (16 -> 32 fill units) AND halves the surge amplitude. ")
		       TEXT("It buys stability margin (the CFL analogue dampingQ8 + ((8<<8)>>gainShift) < 256 goes from 240 to ")
		       TEXT("232) that is not currently short. This is the worst of the three.\n")
		       TEXT("  (3) SweCoupleConfig::absorbPerTick (128) / promoteDwellTicks (8). Lowering absorb / raising the ")
		       TEXT("dwell delays the sheet TAKING UP a fresh pour, so it pools as CA water first and spreads later. ")
		       TEXT("COST: swe.h S5 is explicit that a CA inflow faster than absorbPerTick leaves a bounded standing ")
		       TEXT("residue inside the sheet's z-range -- conserved and correctly ledgered, and the renderer already ")
		       TEXT("draws the union, but it is a real double-owned-looking region and it grows as absorb falls. It ")
		       TEXT("changes only the TRANSIENT: the same water reaches the same film in the end.\n")
		       TEXT("  WHAT IS NOT ON THE MENU, AND IS THE POINT: none of these turns spreading into pooling. Spreading ")
		       TEXT("to a level surface is what the mass equation DOES; damping and gain set how FAST and how FLAT, not ")
		       TEXT("whether. The only structural lever is the coupler's eligibility partition (swe.h S5) -- and it has ")
		       TEXT("NO DEPTH TERM: openClearanceVoxels gates headroom above the bed and minOpenNeighbours gates lateral ")
		       TEXT("openness, so a 1-fill-unit film on open ground is exactly as eligible as a lake. If thin water ")
		       TEXT("should stay CA-owned, that is a NEW predicate term, i.e. a swe.h change, a kSweVersion bump and a ")
		       TEXT("moved golden -- an ADR-0004 amendment, not a tuning pass. Say that out loud before anyone edits a ")
		       TEXT("constant."));
	}

	FScreenshotRequest::RequestScreenshot(TEXT("VoxelSweBreach_3_settled"), false, true);
	SetTimerOnce(Run, Run->StageHandle, 2.f, [](FRunRef R) { Finish(R, TEXT("sequence complete")); });
}

} // namespace

bool VoxelSweBreachFixture::StartFromCommandLine(UWorld* World)
{
	float Delay = 20.f;
	if (!FParse::Value(FCommandLine::Get(), TEXT("VoxelSweBreachTest="), Delay) &&
	    !FParse::Param(FCommandLine::Get(), TEXT("VoxelSweBreachTest")))
	{
		return false;
	}
	if (!World)
	{
		return false;
	}

	FRunRef Run = MakeShared<FSweBreachRun, ESPMode::ThreadSafe>();
	Run->World = World;
	Run->DelaySeconds = Delay;

	int32 SweFlag = 1;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSweBreachSwe="), SweFlag))
	{
		Run->bArmSwe = SweFlag != 0;
	}
	float SearchM = float(kDefaultSearchRadiusM);
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSweBreachSearchM="), SearchM))
	{
		Run->SearchRadiusM = FMath::Clamp(double(SearchM), 4.0, 2000.0);
	}
	float FillPct = 70.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSweBreachFillPct="), FillPct))
	{
		Run->FillFraction = FMath::Clamp(double(FillPct) / 100.0, 0.05, 0.99);
	}
	float SampleSec = kDefaultSampleSeconds;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSweBreachSampleSec="), SampleSec))
	{
		Run->SampleSeconds = FMath::Clamp(SampleSec, 1.f, 60.f);
	}
	FString AtArg;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSweBreachAt="), AtArg, /*bShouldStopOnSeparator*/ false))
	{
		FString XStr, YStr;
		if (AtArg.Split(TEXT(","), &XStr, &YStr))
		{
			Run->bForcedSite = true;
			Run->SiteXUU = FCString::Atod(*XStr) * 100.0;
			Run->SiteYUU = FCString::Atod(*YStr) * 100.0;
		}
		else
		{
			UE_LOG(LogVoxelEarth, Warning, TEXT("-VoxelSweBreachAt=%s malformed (expected X,Y in metres); searching."),
			       *AtArg);
		}
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSweBreachTest: armed. Starting in %.1fs. voxel.Water.SWE will be set to %d. Watchdog quits this ")
	       TEXT("process unconditionally %.0fs from now."),
	       Run->DelaySeconds, Run->bArmSwe ? 1 : 0, kWatchdogSeconds);

	// The watchdog goes on FIRST, before anything that could stall.
	World->GetTimerManager().SetTimer(
		Run->WatchdogHandle, FTimerDelegate::CreateLambda([Run]() { Finish(Run, TEXT("WATCHDOG -- a stage stalled")); }),
		kWatchdogSeconds, false);

	SetTimerOnce(Run, Run->StartHandle, Run->DelaySeconds, &StageBegin);
	return true;
}
