#include "VoxelOceanCaptureFixture.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "VoxelEarth.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelWorldSubsystem.h"

#include "voxelcore/core.h"

// ===========================================================================
// -VoxelOceanSurvey and -VoxelOceanDig
// ===========================================================================
//
// The header carries why this exists. Three things in the mechanism are worth
// reading before the code:
//
// 1. THE GROUND IS THE ENGINE'S, NOT A TILE READ. Both modes call
//    UVoxelWorldSubsystem::GetSurfaceHeightUU, which is the amplified worldgen
//    surface -- the same quantity `oceanSurfaceMmAt(groundMm)` is gated on. A
//    site picked by decoding the .vxtl offline is picked from the BAKED
//    surface, before the client's own detail terms, and the two are not the
//    same number at the metre scale a 25 m framing works at. So offline
//    reading narrows the search and this prints the answer that decides.
//
// 2. THE PIT IS THE TEST'S PIT, IN THREE PASSES, AT WORLD SCALE.
//    voxel-core/tests/test_ocean.cpp's
//    `reservoir_v0_floods_an_inland_pit_the_datum_test_leaves_dry` digs a
//    column in three passes: surface->datum, then two bands below it. Pass 3
//    is where Reservoir v0 broke, because the top cell of pass 3 has pass 2's
//    own air above it, that air is below the datum and holds no CA fill, and
//    that is verbatim its "adjacent cell is implicit ocean" test. The passes
//    are reproduced here in that order, through CarveSphere -- the authority
//    edit path a player's explosive uses -- because a pit dug by a bypass
//    would not fire NotifyTerrainVoxelsCleared and would prove nothing.
//
//    Each CarveSphere is itself an edit and its own notification, so a pass is
//    many notifications rather than one. That is strictly MORE adversarial
//    than the test (more chances for a pass to read an earlier pass's air),
//    and the log says how many so the two are never confused.
//
// 3. THE WATER SNAPSHOT IS READ AFTER EVERY PASS AND AGAIN AT THE END.
//    "The pit stayed dry" is a claim about activeBricks/storedBricks/volume,
//    not about a picture, and the owner judges the picture. So the numbers go
//    in the log next to the pass that could have wetted it.
//
// NOTHING HERE POSES A CAMERA OR TAKES A SCREENSHOT. tools/voxel-capture.ps1
// drives these runs and -VoxelScreenshotAfter owns the shutter, the framing
// (-VoxelSpawnAltM/-VoxelSpawnPitch/-VoxelSpawnYaw) and the exit.

namespace
{
constexpr double kUUPerM = 100.0;
// One voxel is 10 UU. A sphere step below this leaves no gap between spheres
// at the radii these modes use.
constexpr double kSeaLevelUU = double(vxc::kSeaLevelMm) / 10.0;

struct FOceanRun
{
	TWeakObjectPtr<UWorld> World;

	// pit | breach | survey
	FString Mode;

	double SiteXUU = 0.0;
	double SiteYUU = 0.0;
	bool bHaveSite = false;

	double RadiusM = 3.0;
	double LengthM = 0.0;
	double HeadingDeg = 0.0;
	double DepthM = 3.0;
	double SurveyRadiusM = 60.0;
	double SurveyStepM = 5.0;

	float DelaySeconds = 90.f;
	float ReportSeconds = 40.f;

	FTimerHandle StartHandle;
	FTimerHandle ReportHandle;
};

using FOceanRunRef = TSharedRef<FOceanRun, ESPMode::ThreadSafe>;

int32 ImplicitOceanCVar()
{
	static IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Water.ImplicitOcean"));
	return CVar ? CVar->GetInt() : -1;
}

// One line, always the same shape, so two runs can be diffed by eye.
void LogWater(UWorld* World, const TCHAR* Stage)
{
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelOceanDig [%s]: no water subsystem."), Stage);
		return;
	}
	const FVoxelWaterPerfSnapshot Snap = Water->GetPerfSnapshot();
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelOceanDig WATER [%s]: activeBricks=%lld storedBricks=%lld volume=%llu maxFill=%d ")
	       TEXT("digest=0x%016llX implicitOceanCVar=%d"),
	       Stage, Snap.ActiveBricks, Snap.StoredBricks, (unsigned long long)Snap.TotalVolume, Water->GetMaxStoredFill(),
	       (unsigned long long)Water->GetWaterDigest(), ImplicitOceanCVar());
}

// A band of overlapping spheres down one column, TopUU down to BottomUU.
// Returns voxels removed; OutSpheres counts the edits issued.
int32 CarveBand(UVoxelWorldSubsystem& Terrain, double XUU, double YUU, double TopUU, double BottomUU,
                double RadiusUU, int32& OutSpheres)
{
	int32 Removed = 0;
	OutSpheres = 0;
	// Half-radius step: consecutive spheres overlap, so the band is a shaft
	// rather than a string of beads.
	const double Step = FMath::Max(RadiusUU * 0.5, 10.0);
	for (double Z = TopUU; Z >= BottomUU - 1e-6; Z -= Step)
	{
		Removed += Terrain.CarveSphere(FVector(XUU, YUU, Z), RadiusUU, 0.0);
		++OutSpheres;
	}
	return Removed;
}

void RunSurvey(FOceanRunRef Run)
{
	UWorld* World = Run->World.Get();
	UVoxelWorldSubsystem* Terrain = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Terrain)
	{
		UE_LOG(LogVoxelEarth, Error, TEXT("VoxelOceanSurvey: no terrain subsystem -- nothing surveyed."));
		return;
	}

	double CxUU = Run->SiteXUU;
	double CyUU = Run->SiteYUU;
	if (!Run->bHaveSite)
	{
		APlayerController* PC = World->GetFirstPlayerController();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!Pawn)
		{
			UE_LOG(LogVoxelEarth, Error, TEXT("VoxelOceanSurvey: no -VoxelOceanDigAt= and no pawn to centre on."));
			return;
		}
		CxUU = Pawn->GetActorLocation().X;
		CyUU = Pawn->GetActorLocation().Y;
	}

	const double StepUU = Run->SurveyStepM * kUUPerM;
	const int32 N = FMath::Clamp(int32(Run->SurveyRadiusM / Run->SurveyStepM), 1, 60);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelOceanSurvey: GetSurfaceHeightUU over a %dx%d grid, step %.1f m, centred (%.1f, %.1f) m. ")
	       TEXT("Sea level is %.2f m. Rows are +Y downward, columns +X rightward; every value is METRES."),
	       2 * N + 1, 2 * N + 1, Run->SurveyStepM, CxUU / kUUPerM, CyUU / kUUPerM, kSeaLevelUU / kUUPerM);

	// Header row of X offsets, so a column can be read off without counting.
	FString Header = TEXT("        ");
	for (int32 Ix = -N; Ix <= N; ++Ix)
	{
		Header += FString::Printf(TEXT("%8.0f"), double(Ix) * Run->SurveyStepM);
	}
	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelOceanSurvey dX(m):%s"), *Header);

	double MinH = TNumericLimits<double>::Max();
	double MaxH = -TNumericLimits<double>::Max();
	for (int32 Iy = -N; Iy <= N; ++Iy)
	{
		FString Row = FString::Printf(TEXT("%8.0f"), double(Iy) * Run->SurveyStepM);
		for (int32 Ix = -N; Ix <= N; ++Ix)
		{
			const double H = Terrain->GetSurfaceHeightUU(CxUU + double(Ix) * StepUU, CyUU + double(Iy) * StepUU)
			                 / kUUPerM;
			MinH = FMath::Min(MinH, H);
			MaxH = FMath::Max(MaxH, H);
			Row += FString::Printf(TEXT("%8.1f"), H);
		}
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelOceanSurvey dY(m):%s"), *Row);
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelOceanSurvey: DONE. surface min %.2f m, max %.2f m over the grid. A column is OCEAN where ")
	       TEXT("this is below %.2f m and LAND where it is above -- that is the whole of oceanSurfaceMmAt."),
	       MinH, MaxH, kSeaLevelUU / kUUPerM);

	LogWater(World, TEXT("survey"));
}

void RunPit(FOceanRunRef Run)
{
	UWorld* World = Run->World.Get();
	UVoxelWorldSubsystem* Terrain = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Terrain)
	{
		UE_LOG(LogVoxelEarth, Error, TEXT("VoxelOceanDig pit: no terrain subsystem -- NOT DIGGING."));
		return;
	}

	const double SurfUU = Terrain->GetSurfaceHeightUU(Run->SiteXUU, Run->SiteYUU);
	const double SurfM = SurfUU / kUUPerM;

	// THE SITE CHECK, at Error, refusing to dig. An "inland pit" whose column
	// is below sea level is not one, and the frame would be judged as if it
	// were.
	if (SurfUU <= kSeaLevelUU)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelOceanDig pit: SITE IS NOT INLAND -- surface at (%.1f, %.1f) m is %.2f m, at or below sea ")
		       TEXT("level %.2f m. oceanSurfaceMmAt would answer SEA here, so a dry pit would prove nothing. ")
		       TEXT("NOT DIGGING; discard this capture."),
		       Run->SiteXUU / kUUPerM, Run->SiteYUU / kUUPerM, SurfM, kSeaLevelUU / kUUPerM);
		return;
	}

	const double RadiusUU = Run->RadiusM * kUUPerM;
	const double DepthUU = Run->DepthM * kUUPerM;
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelOceanDig pit: site (%.1f, %.1f) m, worldgen surface %.2f m ABOVE sea level %.2f m. ")
	       TEXT("Three passes, sphere radius %.1f m, floor %.2f m (%.2f m below the datum). ")
	       TEXT("This is test_ocean.cpp's inland pit at world scale."),
	       Run->SiteXUU / kUUPerM, Run->SiteYUU / kUUPerM, SurfM, kSeaLevelUU / kUUPerM, Run->RadiusM,
	       (kSeaLevelUU - DepthUU) / kUUPerM, Run->DepthM);

	LogWater(World, TEXT("pit pre-dig"));

	// Pass 1: surface down TO the datum. Reservoir v0's own guard rejects
	// this pass -- it is above sea level.
	// Pass 2: the first band below the datum. v0 rejects it too, because
	// every below-datum neighbour is in this same pass's cleared set.
	// Pass 3: "keep digging". THIS is the one that broke v0.
	const double P1Top = SurfUU;
	const double P1Bot = kSeaLevelUU;
	const double P2Top = kSeaLevelUU - 10.0; // one voxel below the datum
	const double P2Bot = kSeaLevelUU - DepthUU * 0.5;
	const double P3Top = P2Bot - 10.0;
	const double P3Bot = kSeaLevelUU - DepthUU;

	const TCHAR* Names[3] = {TEXT("1 surface->datum"), TEXT("2 first band below"), TEXT("3 keep digging")};
	const double Tops[3] = {P1Top, P2Top, P3Top};
	const double Bots[3] = {P1Bot, P2Bot, P3Bot};

	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		int32 Spheres = 0;
		const int32 Removed = CarveBand(*Terrain, Run->SiteXUU, Run->SiteYUU, Tops[Pass], Bots[Pass], RadiusUU, Spheres);
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelOceanDig pit pass %s: z %.2f m -> %.2f m, %d sphere edit(s), %d voxel(s) removed."),
		       Names[Pass], Tops[Pass] / kUUPerM, Bots[Pass] / kUUPerM, Spheres, Removed);
		LogWater(World, *FString::Printf(TEXT("pit after pass %s"), Names[Pass]));
	}
}

void RunBreach(FOceanRunRef Run)
{
	UWorld* World = Run->World.Get();
	UVoxelWorldSubsystem* Terrain = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Terrain)
	{
		UE_LOG(LogVoxelEarth, Error, TEXT("VoxelOceanDig breach: no terrain subsystem -- NOT DIGGING."));
		return;
	}

	const double SurfUU = Terrain->GetSurfaceHeightUU(Run->SiteXUU, Run->SiteYUU);
	if (SurfUU >= kSeaLevelUU)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelOceanDig breach: SITE IS NOT BELOW SEA LEVEL -- surface at (%.1f, %.1f) m is %.2f m, at or ")
		       TEXT("above sea level %.2f m. There is no sea over this column to breach, so the carve would just be a ")
		       TEXT("hole. NOT DIGGING; discard this capture."),
		       Run->SiteXUU / kUUPerM, Run->SiteYUU / kUUPerM, SurfUU / kUUPerM, kSeaLevelUU / kUUPerM);
		return;
	}

	const double RadiusUU = Run->RadiusM * kUUPerM;
	const double LengthUU = Run->LengthM * kUUPerM;
	const double Rad = FMath::DegreesToRadians(Run->HeadingDeg);
	const double DxUU = FMath::Cos(Rad);
	const double DyUU = FMath::Sin(Rad);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelOceanDig breach: site (%.1f, %.1f) m, worldgen surface %.2f m BELOW sea level %.2f m. ")
	       TEXT("Cutting a channel %.1f m long on heading %.0f deg, sphere radius %.1f m, each sphere centred on ")
	       TEXT("ITS OWN column's surface so the cut follows the seabed up the shore."),
	       Run->SiteXUU / kUUPerM, Run->SiteYUU / kUUPerM, SurfUU / kUUPerM, kSeaLevelUU / kUUPerM, Run->LengthM,
	       Run->HeadingDeg, Run->RadiusM);

	LogWater(World, TEXT("breach pre-dig"));

	const double Step = FMath::Max(RadiusUU * 0.5, 10.0);
	int32 Spheres = 0;
	int32 Removed = 0;
	double LastSurfM = SurfUU / kUUPerM;
	for (double S = 0.0; S <= LengthUU + 1e-6; S += Step)
	{
		const double X = Run->SiteXUU + DxUU * S;
		const double Y = Run->SiteYUU + DyUU * S;
		const double H = Terrain->GetSurfaceHeightUU(X, Y);
		Removed += Terrain->CarveSphere(FVector(X, Y, H), RadiusUU, 0.0);
		++Spheres;
		LastSurfM = H / kUUPerM;
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelOceanDig breach: %d sphere edit(s), %d voxel(s) removed. Far end column surface %.2f m ")
	       TEXT("(sea level %.2f m) -- above it, the channel has cut INTO the shore."),
	       Spheres, Removed, LastSurfM, kSeaLevelUU / kUUPerM);
	LogWater(World, TEXT("breach post-dig"));
}

void Begin(FOceanRunRef Run)
{
	if (Run->Mode == TEXT("survey"))
	{
		RunSurvey(Run);
	}
	else if (Run->Mode == TEXT("pit"))
	{
		RunPit(Run);
	}
	else if (Run->Mode == TEXT("breach"))
	{
		RunBreach(Run);
	}

	UWorld* World = Run->World.Get();
	if (!World)
	{
		return;
	}
	World->GetTimerManager().SetTimer(
		Run->ReportHandle, FTimerDelegate::CreateLambda([Run]() {
			LogWater(Run->World.Get(), TEXT("FINAL"));
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelOceanDig: FINAL report done (%.1fs after the dig). A pit that stayed dry reads ")
			       TEXT("activeBricks=0 storedBricks=0 volume=0; a breach that filled does not."),
			       Run->ReportSeconds);
		}),
		Run->ReportSeconds, false);
}
} // namespace

bool VoxelOceanCaptureFixture::StartFromCommandLine(UWorld* World)
{
	FString Mode;
	float SurveyRadius = 0.f;
	const bool bSurvey = FParse::Value(FCommandLine::Get(), TEXT("VoxelOceanSurvey="), SurveyRadius) ||
	                     FParse::Param(FCommandLine::Get(), TEXT("VoxelOceanSurvey"));
	const bool bDig = FParse::Value(FCommandLine::Get(), TEXT("VoxelOceanDig="), Mode);
	if (!bSurvey && !bDig)
	{
		return false;
	}
	if (!World)
	{
		return false;
	}

	FOceanRunRef Run = MakeShared<FOceanRun, ESPMode::ThreadSafe>();
	Run->World = World;
	Run->Mode = bSurvey ? TEXT("survey") : Mode.ToLower();
	if (SurveyRadius > 0.f)
	{
		Run->SurveyRadiusM = double(SurveyRadius);
	}
	float SurveyStep = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelOceanSurveyStepM="), SurveyStep) && SurveyStep > 0.f)
	{
		Run->SurveyStepM = double(SurveyStep);
	}

	if (Run->Mode != TEXT("survey") && Run->Mode != TEXT("pit") && Run->Mode != TEXT("breach"))
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("-VoxelOceanDig=%s is not a mode. Use pit or breach (or -VoxelOceanSurvey=<radiusM>)."), *Mode);
		return false;
	}

	FString AtArg;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelOceanDigAt="), AtArg, /*bShouldStopOnSeparator*/ false))
	{
		FString XStr, YStr;
		if (AtArg.Split(TEXT(","), &XStr, &YStr))
		{
			Run->bHaveSite = true;
			Run->SiteXUU = FCString::Atod(*XStr) * kUUPerM;
			Run->SiteYUU = FCString::Atod(*YStr) * kUUPerM;
		}
		else
		{
			UE_LOG(LogVoxelEarth, Error, TEXT("-VoxelOceanDigAt=%s malformed (expected X,Y in metres)."), *AtArg);
		}
	}
	// A DIG WITHOUT A NAMED COLUMN IS REFUSED. See the header: the site is
	// chosen deliberately and stated, never found at run time.
	if (Run->Mode != TEXT("survey") && !Run->bHaveSite)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("-VoxelOceanDig=%s needs -VoxelOceanDigAt=X,Y (metres). Refusing to pick a column for you -- ")
		       TEXT("run -VoxelOceanSurvey=<radiusM> first and read one off the grid."),
		       *Run->Mode);
		return false;
	}

	float F = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelOceanDigRadiusM="), F) && F > 0.f) { Run->RadiusM = double(F); }
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelOceanDigLenM="), F) && F >= 0.f)   { Run->LengthM = double(F); }
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelOceanDigHeadingDeg="), F))         { Run->HeadingDeg = double(F); }
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelOceanDigDepthM="), F) && F > 0.f)  { Run->DepthM = double(F); }
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelOceanDigAfter="), F) && F > 0.f)   { Run->DelaySeconds = F; }
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelOceanDigReport="), F) && F > 0.f)  { Run->ReportSeconds = F; }
	if (Run->Mode == TEXT("breach") && Run->LengthM <= 0.0) { Run->LengthM = 14.0; }
	if (Run->Mode == TEXT("breach") && Run->RadiusM <= 3.0) { Run->RadiusM = 6.0; }

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelOceanCaptureFixture: mode=%s armed for T+%.1fs at (%.1f, %.1f) m; report %.1fs later. ")
	       TEXT("voxel.Water.ImplicitOcean is currently %d. This fixture never poses a camera and never quits -- ")
	       TEXT("-VoxelScreenshotAfter owns both."),
	       *Run->Mode, Run->DelaySeconds, Run->SiteXUU / kUUPerM, Run->SiteYUU / kUUPerM, Run->ReportSeconds,
	       ImplicitOceanCVar());

	World->GetTimerManager().SetTimer(Run->StartHandle, FTimerDelegate::CreateLambda([Run]() { Begin(Run); }),
	                                  Run->DelaySeconds, false);
	return true;
}
