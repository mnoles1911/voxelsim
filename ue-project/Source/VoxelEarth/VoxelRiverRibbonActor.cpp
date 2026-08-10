// ==========================================================================
// DEPRECATED 2026-08-09 -- DEFAULT OFF (-VoxelRiverRibbons=1 to build).
// Do not extend; do not delete. Rationale and the keep-not-delete rule:
// docs/water-deprecation-audit-2026-08-09.md. Banner and full note at the top
// of VoxelRiverRibbonActor.h. Lake sheets are unaffected.
// ==========================================================================

#include "VoxelRiverRibbonActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "ProceduralMeshComponent.h"
#include "VoxelEarth.h"
#include "VoxelWaterSubsystem.h"

namespace
{
// One pixel at screen centre subtends 1/1280 in tangent at 1440p and 90 degrees
// horizontal FOV -- half the screen (1280 px) subtends tan(45) = 1. Same
// constant vxc_riverribbonprobe prints its width table from, so the actor's
// sub-pixel count and the probe's table cannot disagree about what "one pixel"
// means.
constexpr double kTangentPerPixel = 1.0 / 1280.0;

// Miter clamp. At a sharp turn the miter vector length goes as 1/sin(theta/2)
// and diverges; past this the joint is squared off instead. Douglas-Peucker at
// a one-pixel tolerance leaves segments averaging ~14 m with gentle turns, so
// this fires rarely -- but "rarely" on a 5,000-point reach is still often
// enough to throw a vertex a kilometre sideways if it is not clamped.
constexpr double kMaxMiterScale = 4.0;

// Liang-Barsky segment-vs-rectangle clip in 2D doubles. Returns the parameter
// interval [T0,T1] of P0->P1 that lies INSIDE R, or false when the segment
// misses R entirely.
//
// EXACT, RATHER THAN A PER-SEGMENT INSIDE/OUTSIDE TEST. A simplified segment
// here averages 14 m and the near-field handover window is only 9-25 m wide, so
// dropping whole segments would put a 14 m hole in the water at the closest
// range the player ever sees the seam from.
bool ClipSegmentToRect(const FVector2D& P0, const FVector2D& P1, const FBox2D& R, double& T0, double& T1)
{
	T0 = 0.0;
	T1 = 1.0;
	const double Dx = P1.X - P0.X;
	const double Dy = P1.Y - P0.Y;
	const double P[4] = {-Dx, Dx, -Dy, Dy};
	const double Q[4] = {P0.X - R.Min.X, R.Max.X - P0.X, P0.Y - R.Min.Y, R.Max.Y - P0.Y};
	for (int32 i = 0; i < 4; ++i)
	{
		if (FMath::IsNearlyZero(P[i]))
		{
			// Parallel to this edge: inside iff already on the correct side.
			if (Q[i] < 0.0)
			{
				return false;
			}
			continue;
		}
		const double T = Q[i] / P[i];
		if (P[i] < 0.0)
		{
			T0 = FMath::Max(T0, T);
		}
		else
		{
			T1 = FMath::Min(T1, T);
		}
	}
	return T0 < T1;
}
} // namespace

AVoxelRiverRibbonActor::AVoxelRiverRibbonActor()
{
	PrimaryActorTick.bCanEverTick = true;

	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("RiverRibbons"));
	SetRootComponent(Mesh);
	Mesh->SetMobility(EComponentMobility::Movable);
	// Cosmetic, exactly like the lake sheets, the clipmap and the ocean plane.
	// Being IN the water is the datum's job (IsUnderwaterAtWorld), not this
	// mesh's; a ribbon must never become something a player stands on at 4 km.
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->bUseAsyncCooking = false;
	// A near-horizontal translucent strip casting a shadow would draw a dark
	// line along its own riverbed. The near-field water casts none either.
	Mesh->SetCastShadow(false);

	// Same load-in-constructor pattern the sheet, the ocean and the clipmap
	// use, so no section ever renders with the engine default for a frame.
	WaterMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_WaterVoxel.M_WaterVoxel")));
}

void AVoxelRiverRibbonActor::BeginPlay()
{
	Super::BeginPlay();

	if (!WaterMaterial)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("M_WaterVoxel not found at /Game/Voxel/M_WaterVoxel -- river ribbons using the engine default "
		            "material. The frame is NOT comparable to a near-field water capture."));
	}

	// DEFAULT OFF, DEPRECATED (2026-08-09, first PBF playtest). The owner could
	// not tell a ribbon from a lake in the far field and asked for ribbons off.
	// The switch is unchanged and still the only control: -VoxelRiverRibbons=1
	// opts a capture back in. Lake SHEETS are a different actor with a different
	// switch and are UNAFFECTED -- they are the standing-water far field and
	// they stay on. See docs/water-deprecation-audit-2026-08-09.md.
	int32 Flag = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelRiverRibbons="), Flag))
	{
		bEnabled = (Flag != 0);
	}
	// -VoxelWaterMarkerOnly=1 wins over the switch above, for the same reason the
	// lake sheets honour it: the ribbon draws the SAME baked river as a flat
	// swept quad at the centreline width, so it overlays a second answer on the
	// geometry the marker is being judged for.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterMarkerOnly"))
	    || (FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterMarkerOnly="), Flag) && Flag != 0))
	{
		bEnabled = false;
	}
	double RangeM = 0.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelRiverRibbonRangeM="), RangeM) && RangeM > 1.0)
	{
		ScanRadiusUU = RangeM * 100.0;
	}
	FParse::Value(FCommandLine::Get(), TEXT("VoxelRiverRibbonMinPx="), MinScreenPx);
	MinScreenPx = FMath::Clamp(MinScreenPx, 0.0, 64.0);

	// The mask is one byte per fine pixel over the scan square. Stated at spawn
	// rather than left to be discovered, because it is the one cost of this
	// feature that grows as the square of a switch.
	const double EdgePx = (2.0 * ScanRadiusUU * 10.0) / 1875.0; // UU -> mm -> fine px
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("River ribbons: %s, scan radius %.0f m (wet mask %.0f x %.0f px, %.1f MB), min screen width %.2f px. "
	            "Flowing water beyond the implicit disc (52 m) is drawn as centreline ribbons at the baked datum, "
	            "at the width the bake drew -- NOT widened to a screen-width floor (see the header for the "
	            "burial measurement that decided that)."),
	       bEnabled ? TEXT("enabled") : TEXT("DISABLED (-VoxelRiverRibbons=0)"), ScanRadiusUU / 100.0, EdgePx, EdgePx,
	       (EdgePx * EdgePx) / (1024.0 * 1024.0), MinScreenPx);
	// ONE line, always, whichever way the switch went -- the deprecation is a
	// fact about the feature, not about this run's arm.
	UE_LOG(LogVoxelEarth, Warning,
	       TEXT("River ribbons are DEPRECATED and now default OFF. Far-field flowing water is being replaced by "
	            "the PBF fluid (docs/water-rearchitecture-plan-2026-08-09.md); the code and its tests are kept "
	            "until the replacement is proven (docs/water-deprecation-audit-2026-08-09.md: deprecate now, "
	            "delete after). Pass -VoxelRiverRibbons=1 to build them anyway. Lake sheets are unaffected."));
}

bool AVoxelRiverRibbonActor::GetCameraLocationUU(FVector& Out) const
{
	// Same fallback chain AVoxelWaterSheetActor and AVoxelClipmapActor use.
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const APlayerCameraManager* Cam = PC->PlayerCameraManager)
		{
			Out = Cam->GetCameraLocation();
			return true;
		}
		if (const APawn* Pawn = PC->GetPawn())
		{
			Out = Pawn->GetActorLocation();
			return true;
		}
	}
	return false;
}

bool AVoxelRiverRibbonActor::RebuildPath(FRibbonPath& Path)
{
	if (!Mesh || Path.Points.Num() < 2)
	{
		return false;
	}

	// The disc is fetched ONCE per reach, not once per segment. The per-point z
	// test below then runs against these two doubles rather than re-entering
	// the subsystem thousands of times -- and, more importantly, against ONE
	// snapshot: a disc re-read mid-reach could recentre between two segments
	// and leave the ribbon clipped against two different holes, which is a
	// visible step in the water that nothing in the geometry explains.
	FBox2D Hole(ForceInit);
	double DiscMinZ = 0.0, DiscMaxZ = 0.0;
	bool bAnyHole = false;
	{
		const UWorld* World = GetWorld();
		const UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
		FBox2D DiscXY(ForceInit);
		if (Water && Water->GetImplicitWaterDiscUU(DiscXY, DiscMinZ, DiscMaxZ) && Path.MaxZUU >= DiscMinZ &&
		    Path.MinZUU < DiscMaxZ && DiscXY.Intersect(Path.BoundsXY))
		{
			Hole = DiscXY;
			bAnyHole = true;
		}
	}
	Path.bHadHole = bAnyHole;
	Path.HoleUU = Hole;
	// THE Z TEST IS WHAT MAKES THE HOLE HONEST, and for a river it has to be
	// applied per POINT rather than per reach: the disc is bounded in z by
	// kImplicitRadiusBricksZ, and a river DESCENDS along its length, so one
	// reach can pass through the disc's z span in its middle and far below it at
	// both ends. Cutting the whole reach would punch a 52 m gap in a river seen
	// from a ridge -- the very defect this actor exists to fix, at close range.
	auto DatumInDisc = [&](double ZUU) { return ZUU >= DiscMinZ && ZUU < DiscMaxZ; };

	const int32 N = Path.Points.Num();

	// PER-POINT FRAME. The perpendicular at a point is defined by its
	// neighbours along the path and by nothing else, which is why the producer
	// emits an ORDERED polyline rather than a set. Interior points use the
	// average of the incoming and outgoing directions (a miter), so the ribbon
	// is continuous through a bend instead of showing a wedge of missing water
	// on the outside of every meander.
	TArray<FVector2D> Perp;
	TArray<double> MiterScale;
	Perp.SetNum(N);
	MiterScale.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		FVector2D In(0.0, 0.0), Outv(0.0, 0.0);
		if (i > 0)
		{
			In = FVector2D(Path.Points[i].X - Path.Points[i - 1].X, Path.Points[i].Y - Path.Points[i - 1].Y);
			In.Normalize();
		}
		if (i + 1 < N)
		{
			Outv = FVector2D(Path.Points[i + 1].X - Path.Points[i].X, Path.Points[i + 1].Y - Path.Points[i].Y);
			Outv.Normalize();
		}
		FVector2D Dir = (i == 0) ? Outv : (i + 1 == N ? In : (In + Outv));
		if (!Dir.Normalize())
		{
			// Two coincident points; fall back to whichever neighbour direction
			// exists, and to +X if neither does, rather than emitting a NaN.
			Dir = In.IsNearlyZero() ? (Outv.IsNearlyZero() ? FVector2D(1.0, 0.0) : Outv) : In;
			Dir.Normalize();
		}
		Perp[i] = FVector2D(-Dir.Y, Dir.X);
		// 1/cos(half the turn) widens the miter so the OUTER edge still meets
		// the next segment's outer edge; clamped because it diverges at a
		// hairpin.
		double Scale = 1.0;
		if (i > 0 && i + 1 < N && !In.IsNearlyZero() && !Outv.IsNearlyZero())
		{
			const double Cos = FVector2D::DotProduct(In, Dir);
			Scale = (Cos > KINDA_SMALL_NUMBER) ? (1.0 / Cos) : kMaxMiterScale;
		}
		MiterScale[i] = FMath::Min(Scale, kMaxMiterScale);
	}

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colors;
	TArray<FProcMeshTangent> Tangents;
	Verts.Reserve(N * 4);
	Tris.Reserve(N * 6);

	// UV origin: this reach's own bbox corner. Absolute world XY is ~180 km
	// here, i.e. 1.8e5 metres in a float TexCoord, which quantises M_WaterVoxel's
	// sub-metre ripple wavelength to nothing. Anchoring per reach keeps the UV
	// in the low thousands. Same reasoning, same numbers, as the lake sheet's.
	const FVector2D UVOrigin = Path.BoundsXY.Min;

	auto AppendQuad = [&](const FVector& A0, const FVector& A1, const FVector& B0, const FVector& B1,
	                      const FVector2D& AlongDir)
	{
		const int32 Base = Verts.Num();
		Verts.Add(A0);
		Verts.Add(A1);
		Verts.Add(B1);
		Verts.Add(B0);
		for (int32 i = Base; i < Verts.Num(); ++i)
		{
			// UVs in METRES, world-planar -- create_water_voxel_material.py's
			// contract ("uv is in METRES"). World-planar rather than
			// along-the-ribbon so the ripple reads identically on the ribbon,
			// on a lake sheet and on a near-field voxel; a ribbon-local UV
			// would make the same material look different on the same water
			// depending on which renderer drew it.
			UVs.Add(FVector2D((Verts[i].X - UVOrigin.X) / 100.0, (Verts[i].Y - UVOrigin.Y) / 100.0));
			Normals.Add(FVector::UpVector);
			Tangents.Add(FProcMeshTangent(AlongDir.X, AlongDir.Y, 0.f));
			// M_WaterVoxel's vertex-colour convention, not a new one:
			//   R = fill fraction, 1.0 -- a ribbon is a full surface, so the
			//       fill-drop WPO is zero and it sits AT the datum;
			//   G = AO, 1.0 -- no greedy-mesher occlusion on a free surface;
			//   B = 1 -- this vertex IS on its cell's +Z boundary, which is
			//       what un-masks the ripple normal and keeps the far water
			//       shimmering like the near water;
			//   A = foam, 0 -- baked water is settled, exactly as every
			//       implicit brick already passes.
			Colors.Add(FColor(255, 255, 255, 0));
		}
		Tris.Add(Base + 0);
		Tris.Add(Base + 1);
		Tris.Add(Base + 2);
		Tris.Add(Base + 0);
		Tris.Add(Base + 2);
		Tris.Add(Base + 3);
	};

	int32 Emitted = 0;
	for (int32 i = 0; i + 1 < N; ++i)
	{
		const FVector& P0 = Path.Points[i];
		const FVector& P1 = Path.Points[i + 1];

		// The near-field cut. Both endpoints must be inside the disc's z span
		// for the hole to apply to this segment: the disc only meshes water
		// whose datum is inside it, so a segment dropping out of the span is
		// owed water for the part that leaves.
		double T0 = 0.0, T1 = 1.0;
		bool bClip = false;
		if (bAnyHole && DatumInDisc(P0.Z) && DatumInDisc(P1.Z))
		{
			bClip = ClipSegmentToRect(FVector2D(P0.X, P0.Y), FVector2D(P1.X, P1.Y), Hole, T0, T1);
		}

		// Emit the parts of [0,1] NOT covered by [T0,T1]. With no clip that is
		// the whole segment; with a clip it is up to two pieces, and when the
		// segment lies wholly inside the disc it is none.
		struct FSpan
		{
			double A, B;
		};
		TArray<FSpan, TInlineAllocator<2>> Spans;
		if (!bClip)
		{
			Spans.Add({0.0, 1.0});
		}
		else
		{
			if (T0 > 0.0)
			{
				Spans.Add({0.0, T0});
			}
			if (T1 < 1.0)
			{
				Spans.Add({T1, 1.0});
			}
		}

		FVector2D Along(P1.X - P0.X, P1.Y - P0.Y);
		Along.Normalize();

		for (const FSpan& S : Spans)
		{
			if (S.B - S.A <= KINDA_SMALL_NUMBER)
			{
				continue;
			}
			// Interpolate the centreline, the datum and the width along the
			// segment so a clipped piece keeps the profile it would have had.
			auto Lerp = [&](double T, FVector& OutC, double& OutHalf, FVector2D& OutPerp)
			{
				OutC = FMath::Lerp(P0, P1, T);
				OutHalf = FMath::Lerp(Path.HalfWidth[i], Path.HalfWidth[i + 1], T) *
				          FMath::Lerp(MiterScale[i], MiterScale[i + 1], T);
				OutPerp = FMath::Lerp(Perp[i], Perp[i + 1], T);
				OutPerp.Normalize();
			};
			FVector CA, CB;
			double HA = 0.0, HB = 0.0;
			FVector2D PA(0.0, 0.0), PB(0.0, 0.0);
			Lerp(S.A, CA, HA, PA);
			Lerp(S.B, CB, HB, PB);
			// A zero-width point would emit a degenerate quad. The bake's
			// minimum wet run is one pixel, so this only fires on a datum-less
			// point the producer already dropped; floored rather than skipped
			// so a reach never silently loses a segment.
			HA = FMath::Max(HA, 1.0);
			HB = FMath::Max(HB, 1.0);
			AppendQuad(FVector(CA.X + PA.X * HA, CA.Y + PA.Y * HA, CA.Z),
			           FVector(CA.X - PA.X * HA, CA.Y - PA.Y * HA, CA.Z),
			           FVector(CB.X + PB.X * HB, CB.Y + PB.Y * HB, CB.Z),
			           FVector(CB.X - PB.X * HB, CB.Y - PB.Y * HB, CB.Z), Along);
			++Emitted;
		}
	}

	// CreateMeshSection rather than UpdateMeshSection: a hole re-cut changes
	// the TOPOLOGY (one segment becomes two, or none), which Update cannot
	// express. Same call and same reason as the lake sheet's.
	if (Verts.Num() == 0)
	{
		Mesh->ClearMeshSection(Path.Section);
	}
	else
	{
		Mesh->CreateMeshSection(Path.Section, Verts, Tris, Normals, UVs, Colors, Tangents,
		                        /*bCreateCollision*/ false);
		Mesh->SetMaterial(Path.Section, WaterMaterial);
	}
	TotalQuads += Emitted - Path.QuadCount;
	Path.QuadCount = Emitted;
	Path.bBuilt = true;
	return true;
}

void AVoxelRiverRibbonActor::StartGather(const FVector& CamUU)
{
	UWorld* World = GetWorld();
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return;
	}
	// A re-gather while a window is open must not leak the previous mask.
	Water->AbandonRiverRibbonWindow();

	Mesh->ClearAllMeshSections();
	Paths.Reset();
	TotalQuads = 0;
	WetPixels = 0;
	CentrePixels = 0;
	UnresolvedBlocks = 0;
	SubPixelPaths = 0;
	RoundRobinCursor = 0;
	bLoggedFirstBuild = false;

	BandCount = Water->BeginRiverRibbonWindow(CamUU.X, CamUU.Y, ScanRadiusUU);
	NextBand = 0;
	GatherStartSec = FPlatformTime::Seconds();
	LastGatherXY = FVector2D(CamUU.X, CamUU.Y);
	bGathered = true;
	Phase = (BandCount > 0) ? EPhase::Filling : EPhase::Ready;

	if (BandCount <= 0)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("River ribbons: no fine water tier -- no ribbons will be drawn. This is the supported "
		            "'no baked water' world, NOT a dry valley."));
	}
	else
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("River ribbons: scanning %.0f m around (%.0f, %.0f) in %d band(s) of 256 px"),
		       ScanRadiusUU / 100.0, CamUU.X, CamUU.Y, BandCount);
	}
}

void AVoxelRiverRibbonActor::CountSubPixelPaths(const FVector& CamUU)
{
	// Projected width, in pixels, of each reach's mean width at its own
	// distance from the camera. This is the number that says whether a thread
	// in the frame is a river or an aliasing artefact, and it is recomputed per
	// count rather than cached because it moves with the camera.
	SubPixelPaths = 0;
	for (const FRibbonPath& P : Paths)
	{
		if (P.Points.Num() < 2)
		{
			continue;
		}
		double SumHalf = 0.0;
		for (double H : P.HalfWidth)
		{
			SumHalf += H;
		}
		const double MeanWidthUU = 2.0 * SumHalf / double(P.HalfWidth.Num());
		const FVector Mid = P.Points[P.Points.Num() / 2];
		const double DistUU = FVector::Distance(CamUU, Mid);
		if (DistUU <= KINDA_SMALL_NUMBER)
		{
			continue;
		}
		if ((MeanWidthUU / DistUU) / kTangentPerPixel < 1.0)
		{
			++SubPixelPaths;
		}
	}
}

void AVoxelRiverRibbonActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bEnabled || !Mesh)
	{
		return;
	}
	UWorld* World = GetWorld();
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return;
	}
	FVector CamUU;
	if (!GetCameraLocationUU(CamUU))
	{
		return;
	}

	const FVector2D CamXY(CamUU.X, CamUU.Y);
	if (!bGathered || FVector2D::Distance(CamXY, LastGatherXY) > RegatherDistanceUU)
	{
		StartGather(CamUU);
		return;
	}

	// ---- Fill: one band of the wet mask per tick ----------------------------
	//
	// A band is one 256 px block row, because riverRibbonFillWet iterates
	// BLOCK-MAJOR and decodes each 256x256 water block exactly once. Feeding it
	// a one-ROW band would decode the whole block row per row, the factor of
	// 256 the producer's own comment warns about.
	if (Phase == EPhase::Filling)
	{
		int64 BandWet = 0;
		if (!Water->FillRiverRibbonWindowBand(NextBand, BandWet))
		{
			// The window went away underneath us (a world teardown, or another
			// caller took it). Restart cleanly rather than building from a
			// half-filled mask, which would silently truncate every reach.
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("River ribbons: band %d of %d could not fill -- the window is gone. Re-gathering."),
			       NextBand, BandCount);
			bGathered = false;
			Phase = EPhase::Idle;
			return;
		}
		++NextBand;
		if (NextBand < BandCount)
		{
			return; // one unit of work per tick, block decodes included
		}

		const double FillSec = FPlatformTime::Seconds() - GatherStartSec;
		const double BuildStart = FPlatformTime::Seconds();
		TArray<UVoxelWaterSubsystem::FRiverRibbonPathUU> Built;
		Water->FinishRiverRibbonWindow(Built, WetPixels, CentrePixels, UnresolvedBlocks);

		for (const UVoxelWaterSubsystem::FRiverRibbonPathUU& B : Built)
		{
			FRibbonPath P;
			P.Points.Reserve(B.Points.Num());
			P.HalfWidth.Reserve(B.Points.Num());
			P.MinZUU = TNumericLimits<double>::Max();
			P.MaxZUU = TNumericLimits<double>::Lowest();
			for (const UVoxelWaterSubsystem::FRiverRibbonVertexUU& V : B.Points)
			{
				P.Points.Add(FVector(V.XUU, V.YUU, V.ZUU));
				P.HalfWidth.Add(V.HalfWidthUU);
				P.MinZUU = FMath::Min(P.MinZUU, V.ZUU);
				P.MaxZUU = FMath::Max(P.MaxZUU, V.ZUU);
				P.BoundsXY += FVector2D(V.XUU, V.YUU);
			}
			if (P.Points.Num() < 2)
			{
				continue;
			}
			P.Section = Paths.Num();
			Paths.Add(MoveTemp(P));
		}

		// The sub-pixel cull, applied AFTER tracing so the log can state what
		// was dropped rather than what never existed. Default 0 draws
		// everything.
		int32 Culled = 0;
		if (MinScreenPx > 0.0)
		{
			for (int32 i = Paths.Num() - 1; i >= 0; --i)
			{
				double SumHalf = 0.0;
				for (double H : Paths[i].HalfWidth)
				{
					SumHalf += H;
				}
				const double MeanWidthUU = 2.0 * SumHalf / double(Paths[i].HalfWidth.Num());
				const FVector Mid = Paths[i].Points[Paths[i].Points.Num() / 2];
				const double DistUU = FMath::Max(FVector::Distance(CamUU, Mid), 1.0);
				if ((MeanWidthUU / DistUU) / kTangentPerPixel < MinScreenPx)
				{
					Paths.RemoveAt(i);
					++Culled;
				}
			}
			for (int32 i = 0; i < Paths.Num(); ++i)
			{
				Paths[i].Section = i;
			}
		}

		Phase = EPhase::Ready;
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("River ribbons: window filled in %.1f ms, traced in %.1f ms -- %lld wet px, %lld centreline px, "
		            "%d reach(es)%s, %lld unresolved water block(s)."),
		       FillSec * 1000.0, (FPlatformTime::Seconds() - BuildStart) * 1000.0, (long long)WetPixels,
		       (long long)CentrePixels, Paths.Num(),
		       Culled > 0 ? *FString::Printf(TEXT(" (%d culled below %.2f px)"), Culled, MinScreenPx) : TEXT(""),
		       (long long)UnresolvedBlocks);
		if (UnresolvedBlocks > 0)
		{
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("River ribbons: %lld water block(s) would NOT decode. Their river is ABSENT, which looks "
			            "exactly like a dry valley."),
			       (long long)UnresolvedBlocks);
		}
		return;
	}

	if (Phase != EPhase::Ready)
	{
		return;
	}

	// ---- Build/refresh: at most one reach per tick --------------------------
	//
	// A reach is rebuilt when it has no mesh yet, or when the near-field hole
	// it was cut with no longer matches the disc being meshed now. The second
	// case is normally ZERO reaches: the hole exists only for the river the
	// camera is standing in.
	const int32 Num = Paths.Num();
	for (int32 Step = 0; Step < Num; ++Step)
	{
		const int32 Index = (RoundRobinCursor + Step) % Num;
		FRibbonPath& P = Paths[Index];

		FBox2D DiscXY(ForceInit);
		double MinZ = 0.0, MaxZ = 0.0;
		const bool bDisc = Water->GetImplicitWaterDiscUU(DiscXY, MinZ, MaxZ);
		const bool bWantHole =
			bDisc && P.MaxZUU >= MinZ && P.MinZUU < MaxZ && DiscXY.Intersect(P.BoundsXY);
		// BOTH the old and the new hole have to be tested. Testing only the new
		// one leaves a reach permanently cut when the camera walks out of it --
		// the same bug the lake sheet had first, and it looks like a river with
		// a 52 m gap punched in the middle.
		const bool bHoleChanged = (bWantHole != P.bHadHole) || (bWantHole && DiscXY != P.HoleUU);
		if (P.bBuilt && !bHoleChanged)
		{
			continue;
		}
		RebuildPath(P);
		RoundRobinCursor = (Index + 1) % Num;
		break;
	}

	if (!bLoggedFirstBuild && Num > 0)
	{
		int32 Settled = 0;
		for (const FRibbonPath& P : Paths)
		{
			if (P.bBuilt)
			{
				++Settled;
			}
		}
		if (Settled == Num)
		{
			bLoggedFirstBuild = true;
			CountSubPixelPaths(CamUU);
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("River ribbons: DRAINED build -- %d reach(es), %d quad(s), %d below one pixel from this "
			            "camera. The far field now has flowing water; a capture taken before this line has not."),
			       Num, TotalQuads, SubPixelPaths);
		}
	}
}
