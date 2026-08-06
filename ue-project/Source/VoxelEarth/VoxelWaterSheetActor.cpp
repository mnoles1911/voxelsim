#include "VoxelWaterSheetActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "ProceduralMeshComponent.h"
#include "VoxelEarth.h"
#include "VoxelWaterSubsystem.h"

namespace
{
// Rectangles come out of vxc::lakeSheetRects as tile-local pixel spans and
// arrive here as world-UU boxes; this turns one into four vertices and two
// triangles at the datum.
//
// SEPARATE VERTICES PER RECTANGLE, not a shared vertex pool. Adjacent
// rectangles abut exactly and are coplanar, so a shared-vertex weld buys
// nothing visually and costs a hash per vertex over a set that is already only
// a few hundred quads per lake; and welding across rectangles is precisely
// where a T-junction crack would come from if the decomposition ever changed.
void AppendRectQuad(const FBox2D& RectUU, double ZUU, const FVector2D& UVOriginUU,
                    TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Normals,
                    TArray<FVector2D>& UVs, TArray<FColor>& Colors, TArray<FProcMeshTangent>& Tangents)
{
	const int32 Base = Verts.Num();
	const double X0 = RectUU.Min.X, Y0 = RectUU.Min.Y, X1 = RectUU.Max.X, Y1 = RectUU.Max.Y;
	Verts.Add(FVector(X0, Y0, ZUU));
	Verts.Add(FVector(X1, Y0, ZUU));
	Verts.Add(FVector(X1, Y1, ZUU));
	Verts.Add(FVector(X0, Y1, ZUU));

	// UVs in METRES, world-planar, anchored at the actor-relative origin. The
	// material's ripple reads TexCoord0 as metres (create_water_voxel_material.py:
	// "uv is in METRES"), which the pooled voxel vertex factory supplies wrapped
	// to a 32 m period. This does NOT wrap, on purpose: a rectangle here can be a
	// kilometre long and a wrap inside a quad would smear the whole period across
	// it. The consequence is a ripple PHASE offset against the near-field water,
	// which is invisible -- the voxel path's own 32 m wrap already makes its phase
	// discontinuous every 32 m (freq*32 is not a multiple of 2pi for any of the
	// four channels), so there was never a continuous phase to match.
	for (int32 i = Base; i < Verts.Num(); ++i)
	{
		UVs.Add(FVector2D((Verts[i].X - UVOriginUU.X) / 100.0, (Verts[i].Y - UVOriginUU.Y) / 100.0));
		Normals.Add(FVector::UpVector);
		Tangents.Add(FProcMeshTangent(1.f, 0.f, 0.f));
		// See the header for this convention -- it is M_WaterVoxel's, not a new
		// one. R=255 full fill, G=255 no AO, B=255 top boundary, A=0 no foam.
		Colors.Add(FColor(255, 255, 255, 0));
	}

	Tris.Add(Base + 0);
	Tris.Add(Base + 1);
	Tris.Add(Base + 2);
	Tris.Add(Base + 0);
	Tris.Add(Base + 2);
	Tris.Add(Base + 3);
}
} // namespace

AVoxelWaterSheetActor::AVoxelWaterSheetActor()
{
	PrimaryActorTick.bCanEverTick = true;

	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("LakeSheets"));
	SetRootComponent(Mesh);
	Mesh->SetMobility(EComponentMobility::Movable);
	// Cosmetic, exactly like the clipmap and the ocean plane: a sheet must never
	// be something a player can stand on out at 5 km. Being IN the water is the
	// datum's job (IsUnderwaterAtWorld, §5.3), not this mesh's.
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->bUseAsyncCooking = false;
	// Sheets are flat and horizontal; a shadow from one is a black rectangle on
	// its own lakebed, and the near-field water casts none either.
	Mesh->SetCastShadow(false);

	// Same load-in-constructor pattern AVoxelOceanActor and AVoxelClipmapActor
	// use, so no section ever renders with the engine default for a frame.
	WaterMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_WaterVoxel.M_WaterVoxel")));
}

void AVoxelWaterSheetActor::BeginPlay()
{
	Super::BeginPlay();

	if (!WaterMaterial)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("M_WaterVoxel not found at /Game/Voxel/M_WaterVoxel -- lake sheets using the engine default "
		            "material. The frame is NOT comparable to a near-field water capture."));
	}

	int32 Flag = 1;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheets="), Flag))
	{
		bEnabled = (Flag != 0);
	}
	// -VoxelWaterMarkerOnly=1 wins over the switch above: the marker view exists
	// so ONE renderer describes where water is. Sheets draw the same baked lakes
	// in blue at ~15 m rectangles, which is a second, coarser answer overlaid on
	// the one being judged. See VoxelWaterSubsystem.cpp's note at the near-field
	// call site for the report that prompted this.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterMarkerOnly"))
	    || (FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterMarkerOnly="), Flag) && Flag != 0))
	{
		bEnabled = false;
	}

	// SCAN RADIUS: 10 km, and this is a MEMORY decision, not a taste one.
	//
	// The obvious default was AVoxelClipmapActor::OuterHalfExtentUU() -- put
	// water on all the ground the far terrain draws. At the shipped cascade that
	// is 65.5 km, which is 81 fine tiles, and a resident fine tile holds its
	// whole compressed .vxtl in memory (FineTileSampler keeps `bytes_`; the
	// lazily-decoded blocks are on top of that). The exemplar set runs 30-51 MB
	// a tile, so that default would have asked for ~3 GB of tile bytes to draw
	// water the player cannot resolve anyway.
	//
	// 10 km is the range the plan's own verification names ("capture at 2-10
	// km"), and its square touches at most 3x3 tiles. What it costs is stated
	// after each gather rather than assumed, and -VoxelLakeSheetRangeM moves it.
	// The honest limitation: a basin further out than this draws no sheet, so a
	// 30 km vista still has dry bowls in it. That is a bounded, logged absence
	// instead of an unbounded load.
	ScanRadiusUU = 1000000.0; // 10 km
	double RangeM = 0.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheetRangeM="), RangeM) && RangeM > 1.0)
	{
		ScanRadiusUU = RangeM * 100.0;
	}
	FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheetCells="), TargetCellsPerSide);
	TargetCellsPerSide = FMath::Clamp(TargetCellsPerSide, 8, 2048);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Lake sheets: %s, scan radius %.0f m, %d cells/basin side. Water beyond the implicit disc "
	            "(52 m) is drawn as flat sheets at the baked datum."),
	       bEnabled ? TEXT("enabled") : TEXT("DISABLED (-VoxelLakeSheets=0)"), ScanRadiusUU / 100.0,
	       TargetCellsPerSide);
}

bool AVoxelWaterSheetActor::GetCameraLocationUU(FVector& Out) const
{
	// Same fallback chain AVoxelClipmapActor::GetCameraLocationUU and
	// AVoxelOceanActor::UpdateFollowPlane use.
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

int32 AVoxelWaterSheetActor::StepForBasin(double SpanUU) const
{
	// SpanUU is the basin's longer bbox side. A fine pixel is 1.875 m; the step
	// is whatever makes the longer side about TargetCellsPerSide cells, floored
	// at 1 so a small pond meshes at full lattice resolution and never coarser
	// than the ground it sits in.
	const double PixelUU = 187.5; // 1.875 m; the fine tier's pixel, see tiles.h
	const double Cells = SpanUU / PixelUU;
	return FMath::Max(1, FMath::CeilToInt(Cells / double(TargetCellsPerSide)));
}

bool AVoxelWaterSheetActor::HoleForDatum(double SurfaceZUU, FBox2D& OutHoleUU) const
{
	const UWorld* World = GetWorld();
	const UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return false;
	}
	FBox2D DiscXY(ForceInit);
	double MinZ = 0.0, MaxZ = 0.0;
	if (!Water->GetImplicitWaterDiscUU(DiscXY, MinZ, MaxZ))
	{
		return false;
	}
	// THE Z TEST IS WHAT MAKES THE HOLE HONEST. RefreshImplicitWater's disc is
	// bounded in z by kImplicitRadiusBricksZ, so a camera 30 m above this lake is
	// meshing NO water at its datum -- cutting a hole for it would punch a 52 m
	// square of missing water out of a lake seen from a ridge, which is the very
	// defect this actor exists to fix, reintroduced at close range.
	if (SurfaceZUU < MinZ || SurfaceZUU >= MaxZ)
	{
		return false;
	}
	OutHoleUU = DiscXY;
	return true;
}

bool AVoxelWaterSheetActor::RebuildSheet(FSheet& Sheet)
{
	UWorld* World = GetWorld();
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water || !Mesh)
	{
		return false;
	}

	UVoxelWaterSubsystem::FLakeSheetBasin Basin;
	Basin.TileX = Sheet.TileX;
	Basin.TileY = Sheet.TileY;
	Basin.BasinId = Sheet.BasinId;
	Basin.SurfaceZUU = Sheet.SurfaceZUU;

	TArray<FBox2D> Rects;
	bool bResolved = false;
	Water->BuildLakeSheetRects(Basin, Sheet.StepPx, Rects, bResolved);
	if (!bResolved)
	{
		return false;
	}

	FBox2D Hole(ForceInit);
	Sheet.bHadHole = HoleForDatum(Sheet.SurfaceZUU, Hole);
	Sheet.HoleUU = Hole;

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colors;
	TArray<FProcMeshTangent> Tangents;
	Verts.Reserve(Rects.Num() * 4);
	Tris.Reserve(Rects.Num() * 6);

	// UV origin: this basin's own bbox corner. Absolute world XY would be ~180 km
	// here, i.e. 1.8e5 metres in a float TexCoord, which quantises a 0.7 m ripple
	// wavelength to nothing. Anchoring per basin keeps the UV in the low
	// thousands with no wrap seam inside any quad.
	const FVector2D UVOrigin(Sheet.MinXUU, Sheet.MinYUU);

	int32 Emitted = 0;
	for (const FBox2D& R : Rects)
	{
		if (!Sheet.bHadHole)
		{
			AppendRectQuad(R, Sheet.SurfaceZUU, UVOrigin, Verts, Tris, Normals, UVs, Colors, Tangents);
			++Emitted;
			continue;
		}
		// Rectangle minus rectangle, in UU. The pixel-space helper in lakes.h is
		// the tested one; this is the same four cases on doubles, which is what
		// world units are -- converting the hole into pixel space and back would
		// round the cut to a 1.875 m lattice and open a sub-pixel seam against
		// the voxel water it is supposed to meet exactly.
		if (Hole.Max.X <= R.Min.X || Hole.Min.X >= R.Max.X || Hole.Max.Y <= R.Min.Y || Hole.Min.Y >= R.Max.Y)
		{
			AppendRectQuad(R, Sheet.SurfaceZUU, UVOrigin, Verts, Tris, Normals, UVs, Colors, Tangents);
			++Emitted;
			continue;
		}
		auto Emit = [&](double X0, double Y0, double X1, double Y1)
		{
			if (X1 <= X0 || Y1 <= Y0)
			{
				return;
			}
			AppendRectQuad(FBox2D(FVector2D(X0, Y0), FVector2D(X1, Y1)), Sheet.SurfaceZUU, UVOrigin, Verts, Tris,
			               Normals, UVs, Colors, Tangents);
			++Emitted;
		};
		const double MY0 = FMath::Max(R.Min.Y, Hole.Min.Y);
		const double MY1 = FMath::Min(R.Max.Y, Hole.Max.Y);
		Emit(R.Min.X, R.Min.Y, R.Max.X, FMath::Min(Hole.Min.Y, R.Max.Y)); // below
		Emit(R.Min.X, FMath::Max(Hole.Max.Y, R.Min.Y), R.Max.X, R.Max.Y); // above
		Emit(R.Min.X, MY0, FMath::Min(Hole.Min.X, R.Max.X), MY1);         // left
		Emit(FMath::Max(Hole.Max.X, R.Min.X), MY0, R.Max.X, MY1);         // right
	}

	// One mesh section per basin; the section index IS the sheet's slot, which
	// is why the gather re-assigns both together and clears everything when it
	// does. CreateMeshSection rather than UpdateMeshSection because a hole
	// re-cut changes the TOPOLOGY (a rectangle becomes up to four), which
	// Update cannot express -- unlike the clipmap, whose vertex layout never
	// changes and which is right to prefer Update.
	if (Verts.Num() == 0)
	{
		Mesh->ClearMeshSection(Sheet.Section);
	}
	else
	{
		Mesh->CreateMeshSection(Sheet.Section, Verts, Tris, Normals, UVs, Colors, Tangents,
		                        /*bCreateCollision*/ false);
		Mesh->SetMaterial(Sheet.Section, WaterMaterial);
	}
	TotalRects += Emitted - Sheet.RectCount;
	Sheet.RectCount = Emitted;
	Sheet.bBuilt = true;
	return true;
}

void AVoxelWaterSheetActor::Tick(float DeltaTime)
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

	// ---- Gather: which basins are in range at all ---------------------------
	//
	// Not every tick, and not all at once. A gather LOADS FINE TILES -- tens of
	// MB each, read and parsed on the game thread -- and a 10 km square is up to
	// nine of them. Taken in one call that is a multi-second stall at spawn, on
	// the same thread the streamer and the water CA are using. So the tile LIST
	// is computed when the camera has moved far enough to change the answer, and
	// exactly one tile is read per tick after that.
	const FVector2D CamXY(CamUU.X, CamUU.Y);
	if (!bGathered || FVector2D::Distance(CamXY, LastGatherXY) > RegatherDistanceUU)
	{
		int32 Tx0 = 0, Ty0 = 0, Tx1 = 0, Ty1 = 0;
		UVoxelWaterSubsystem::FineTileForWorldUU(CamUU.X - ScanRadiusUU, CamUU.Y - ScanRadiusUU, Tx0, Ty0);
		UVoxelWaterSubsystem::FineTileForWorldUU(CamUU.X + ScanRadiusUU, CamUU.Y + ScanRadiusUU, Tx1, Ty1);
		PendingTiles.Reset();
		for (int32 Ty = Ty0; Ty <= Ty1; ++Ty)
		{
			for (int32 Tx = Tx0; Tx <= Tx1; ++Tx)
			{
				PendingTiles.Add(FIntPoint(Tx, Ty));
			}
		}
		// Every previous section is about to be re-indexed, so clearing them all
		// is one call and cannot leave an orphan drawing a lake that is no
		// longer in range. The alternative -- matching old sheets to new by
		// basin key -- saves a rebuild the per-tick budget already spreads out.
		Mesh->ClearAllMeshSections();
		Sheets.Reset();
		GatherCenterXY = CamXY;
		LastGatherXY = CamXY;
		bGathered = true;
		UnresolvedBasins = 0;
		TotalRects = 0;
		RoundRobinCursor = 0;
		bLoggedFirstBuild = false;
		UE_LOG(LogVoxelEarth, Log, TEXT("Lake sheets: scanning %d fine tile(s) within %.0f m of (%.0f, %.0f)"),
		       PendingTiles.Num(), ScanRadiusUU / 100.0, CamUU.X, CamUU.Y);
	}

	if (PendingTiles.Num() > 0)
	{
		const FIntPoint T = PendingTiles.Pop(EAllowShrinking::No);
		TArray<UVoxelWaterSubsystem::FLakeSheetBasin> Found;
		const double StartSec = FPlatformTime::Seconds();
		Water->GatherLakeSheetBasinsInTile(T.X, T.Y, GatherCenterXY.X, GatherCenterXY.Y, ScanRadiusUU, Found);
		for (const UVoxelWaterSubsystem::FLakeSheetBasin& B : Found)
		{
			FSheet S;
			S.TileX = B.TileX;
			S.TileY = B.TileY;
			S.BasinId = B.BasinId;
			S.SurfaceZUU = B.SurfaceZUU;
			S.MinXUU = B.MinXUU;
			S.MinYUU = B.MinYUU;
			S.MaxXUU = B.MaxXUU;
			S.MaxYUU = B.MaxYUU;
			S.StepPx = StepForBasin(FMath::Max(B.MaxXUU - B.MinXUU, B.MaxYUU - B.MinYUU));
			S.Section = Sheets.Num();
			Sheets.Add(S);
		}
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Lake sheets: tile (%d,%d) contributed %d basin(s) in %.1f ms; %d tile(s) left, %d basin(s) so far"),
		       T.X, T.Y, Found.Num(), (FPlatformTime::Seconds() - StartSec) * 1000.0, PendingTiles.Num(),
		       Sheets.Num());
		return; // one unit of work per tick, tile reads included
	}

	// ---- Build/refresh: at most one basin per tick --------------------------
	//
	// A basin is rebuilt when it has no mesh yet, or when the near-field hole it
	// was cut with no longer matches the disc the water subsystem is meshing
	// now. The second case is normally ZERO basins: the hole only exists for the
	// basin the camera is standing in the water of.
	const int32 Num = Sheets.Num();
	for (int32 Step = 0; Step < Num; ++Step)
	{
		const int32 Index = (RoundRobinCursor + Step) % Num;
		FSheet& S = Sheets[Index];

		FBox2D WantHole(ForceInit);
		const bool bWantHole = HoleForDatum(S.SurfaceZUU, WantHole);
		const bool bHoleChanged = (bWantHole != S.bHadHole) || (bWantHole && WantHole != S.HoleUU);
		// A hole only matters where it actually meets this basin -- and BOTH the
		// old and the new one have to be tested. Testing only the new one is a
		// real bug and it was written that way first: when the camera walks out
		// of a lake the new hole is absent (an empty box at the origin), which
		// touches nothing, so the basin would keep the 52 m square cut out of it
		// forever and the lake would be left with a permanent hole in the middle.
		auto Touches = [&S](const FBox2D& H)
		{
			return !(H.Max.X <= S.MinXUU || H.Min.X >= S.MaxXUU || H.Max.Y <= S.MinYUU || H.Min.Y >= S.MaxYUU);
		};
		const bool bHoleTouches = (bWantHole && Touches(WantHole)) || (S.bHadHole && Touches(S.HoleUU));
		if (S.bUnresolved || (S.bBuilt && !(bHoleChanged && bHoleTouches)))
		{
			continue;
		}

		if (!RebuildSheet(S))
		{
			++UnresolvedBasins;
			// Take it out of the rotation: an unresolvable basin retried every
			// tick is a disk stat per tick forever. It is COUNTED, which is what
			// keeps "the lake is missing" answerable from the log.
			S.bUnresolved = true;
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("Lake sheets: basin %d of tile (%d,%d) would not resolve -- its water will be ABSENT, "
			            "which looks exactly like a dry basin. %d unresolved so far."),
			       S.BasinId, S.TileX, S.TileY, UnresolvedBasins);
		}
		RoundRobinCursor = (Index + 1) % Num;
		break;
	}

	if (!bLoggedFirstBuild && Num > 0)
	{
		int32 Settled = 0;
		for (const FSheet& S : Sheets)
		{
			// A basin with zero rectangles is settled too: an extent that
			// decimates to nothing at this range is a real answer, and waiting
			// for it to become non-zero would never log at all.
			if (S.bBuilt || S.bUnresolved)
			{
				++Settled;
			}
		}
		if (Settled == Num)
		{
			bLoggedFirstBuild = true;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: DRAINED build -- %d basin(s), %d rectangle(s), %d unresolved. The far field "
			            "now has water; a capture taken before this line has not."),
			       Num, TotalRects, UnresolvedBasins);
		}
	}
}
