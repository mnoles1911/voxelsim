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

UProceduralMeshComponent* AVoxelWaterSheetActor::GetOrCreateSheetComp(FSheet& Sheet)
{
	if (Sheet.Comp != nullptr)
	{
		return Sheet.Comp;
	}
	// Named for the log, not for lookup: nothing addresses these by name. The
	// serial suffix is load-bearing -- see CompNameSerial's comment in the
	// header for the displace-on-name-collision cost it avoids.
	UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(
		this, *FString::Printf(TEXT("LakeBasin_%d_%d_%d_g%u"), Sheet.TileX, Sheet.TileY, Sheet.BasinId,
		                       ++CompNameSerial));
	C->SetupAttachment(Mesh);
	C->SetMobility(EComponentMobility::Movable);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->bUseAsyncCooking = false;
	C->SetCastShadow(false);
	C->RegisterComponent();
	Sheet.Comp = C;
	return C;
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
	FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheetFineM="), FineBandRadiusM);
	FineBandRadiusM = FMath::Clamp(FineBandRadiusM, 0.0, 4000.0);
	FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheetSnapPx="), LodSnapPx);
	LodSnapPx = FMath::Clamp(LodSnapPx, 1, 4096);
	FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheetBands="), MaxBands);
	MaxBands = FMath::Clamp(MaxBands, 1, int32(UVoxelWaterSubsystem::FLakeSheetLod::kMaxBands));

	const bool bOnePath = !UVoxelWaterSubsystem::ShouldMeshImplicitLakes();
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Lake sheets: %s, scan radius %.0f m, %d cells/basin side, up to %d LOD band(s), finest %.0f m at "
	            "1.875 m/cell (re-centred every %d fine px = %.0f m). Lake basins: %s."),
	       bEnabled ? TEXT("enabled") : TEXT("DISABLED (-VoxelLakeSheets=0)"), ScanRadiusUU / 100.0,
	       TargetCellsPerSide, MaxBands, FineBandRadiusM, LodSnapPx, double(LodSnapPx) * 1.875,
	       bOnePath ? TEXT("ONE rendering path -- the sheet owns them at every range, no near-field hole")
	                : TEXT("TWO rendering paths (voxel.Water.MeshImplicitLakes=1) -- a hole is cut for the "
	                       "near-field disc"));
}

namespace
{
// A camera further than this from its own pawn is not a camera POSITION, it is
// a camera CACHE that has not caught up with where the pawn was placed. The
// largest legitimate separation anything in this project produces is the
// third-person boom -- 2.5 m back and 0.4 m to the active shoulder
// (VoxelMovementTuning::ThirdPersonBoomBackUU / ThirdPersonBoomRightUU, applied
// per frame in AVoxelEarthFlyPawn::UpdateThirdPersonCamera, no spring arm). 100
// m is two orders of magnitude past that, so this cannot separate a real first-
// or third-person camera from its pawn; it can only catch a cache still holding
// a position the pawn has left. Deliberately loose, because what it falls back
// to is not a refusal: it is the PAWN's location, which at a 10 km gather
// radius with 1 km of hysteresis answers every question this actor asks
// identically.
constexpr double kMaxCameraPawnSeparationUU = 10000.0; // 100 m
} // namespace

bool AVoxelWaterSheetActor::GetCameraLocationUU(FVector& Out) const
{
	// WHY THIS IS NO LONGER THE FALLBACK CHAIN AVoxelClipmapActor::
	// GetCameraLocationUU, AVoxelRiverRibbonActor and AVoxelOceanActor::
	// UpdateFollowPlane still use ("camera manager first, pawn second").
	//
	// MEASURED 2026-08-23, the owner's PIE session. APlayerCameraManager is
	// spawned with the PlayerController, well before AVoxelEarthGameMode::
	// RestartPlayer picks a spawn column and puts a pawn in it, and until its
	// first UpdateCamera its cached POV is the DEFAULT: (0,0,0). The old chain
	// took that default for a camera position, so this actor's FIRST basin
	// gather ran at the world origin and logged
	//     "Lake sheets: scanning 4 fine tile(s) within 10000 m of (0, 0)"
	// -- tiles (0,0), (0,-1), (-1,0) and (-1,-1), the four that meet at the
	// origin, about 61 km from where the player actually spawned and none of
	// them ever baked (only 15 fine tiles exist, all in the -3..-15 band).
	// 10,814 elevation reads went into them over the first ~2.5 s, until the
	// pawn appeared and the gather re-ran correctly at (-6144000, -6144000).
	// The leak counter never moved again all session: ONE gather, at a fake
	// position, and the session was filed as a streaming regression on it.
	//
	// WHY THAT IS NOT JUST A NOISY COUNTER. A fine-tier query into a
	// non-resident tile does NOT fall back to the coarse tier. It returns SEA
	// LEVEL -- FVoxelFineTileStreamer::ReportGateLeak_Locked ends
	// `return Sampler_.elevationMm(px,py)` -- i.e. ground no other client
	// computes. And under -unattended the FIRST such query is UE_LOG(Fatal)
	// (SetLeakIsFatal, driven from FApp::IsUnattended), so on the headless path
	// this is not a warning, it KILLS THE RUN. tools/voxel-capture.ps1 launches
	// -unattended with a fine tile dir and does NOT pass
	// -VoxelFineTileGateFatal=0; tools/bv12-river-captures.ps1:43 and
	// tools/bv12-shoot-one.ps1:37 do, which is this bug being worked around
	// rather than diagnosed.
	//
	// THE CONDITION IS "IS THERE A POSSESSED PAWN", NOT "IS THE CAMERA AWAY
	// FROM THE ORIGIN". A player standing legitimately at (0,0) is a position,
	// not a default, and a test that cannot tell those apart would refuse that
	// player lakes forever. RestartPlayer places the pawn on a ground-derived
	// column before it possesses, so a pawn EXISTING is the honest evidence
	// that a real position exists -- it is the event actually being waited for,
	// not a proxy for it.
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return false;
	}
	const APawn* Pawn = PC->GetPawn();
	if (!Pawn)
	{
		// No pawn: whatever the camera manager reports is a default, not a place.
		return false;
	}
	const FVector PawnUU = Pawn->GetActorLocation();
	if (const APlayerCameraManager* Cam = PC->PlayerCameraManager)
	{
		// The pawn existing closes the "before RestartPlayer" window but NOT the
		// one-frame window after it: the camera manager fills its cache during
		// the PlayerController's own tick, and nothing orders this actor after
		// that. One frame at (0,0) is still one fatal gate leak on the
		// unattended path, so the cache is believed only when it is actually ON
		// the pawn. Every SetViewTarget in this project targets PC->GetPawn()
		// (VoxelSkyLadderFixture.cpp:392, VoxelSweBreachFixture.cpp:477,
		// VoxelWorldSubsystem.cpp:20948), so "on the pawn" is the whole truth
		// about where this camera is allowed to be.
		const FVector CamUU = Cam->GetCameraLocation();
		if (FVector::DistSquared(CamUU, PawnUU) < FMath::Square(kMaxCameraPawnSeparationUU))
		{
			Out = CamUU;
			return true;
		}
	}
	// Not a failure and not a guess: the pawn is a PLACED actor. This is also
	// the answer on the frame a pose fixture teleports the pawn kilometres
	// (SetActorLocation with TeleportPhysics) and the camera cache still holds
	// the pose before it.
	Out = PawnUU;
	return true;
}

namespace
{
// A fine pixel, in UU. 1.875 m; the fine tier's pixel, see tiles.h.
constexpr double kFinePixelUU = 187.5;
} // namespace

int32 AVoxelWaterSheetActor::StepForBasin(double SpanUU) const
{
	// SpanUU is the basin's longer bbox side. A fine pixel is 1.875 m; the step
	// is whatever makes the longer side about TargetCellsPerSide cells, floored
	// at 1 so a small pond meshes at full lattice resolution and never coarser
	// than the ground it sits in.
	//
	// THIS IS NOW THE COARSEST BAND'S STEP, not the whole basin's. It is
	// unchanged on purpose: the far field keeps exactly the decimation it had, so
	// the band ladder is a strict addition near the camera rather than a retune
	// of everything. See BuildLodForBasin.
	const double Cells = SpanUU / kFinePixelUU;
	return FMath::Max(1, FMath::CeilToInt(Cells / double(TargetCellsPerSide)));
}

FIntPoint AVoxelWaterSheetActor::LodKeyForCamera(const FVector& CamUU) const
{
	const double CellUU = double(FMath::Max(LodSnapPx, 1)) * kFinePixelUU;
	return FIntPoint(int32(FMath::FloorToDouble(CamUU.X / CellUU)),
	                 int32(FMath::FloorToDouble(CamUU.Y / CellUU)));
}

FVector2D AVoxelWaterSheetActor::SnappedCamXY(const FVector& CamUU) const
{
	// THE BANDS ARE CENTRED ON THE CELL, NOT ON THE CAMERA, and that is what
	// makes a basin's geometry a pure function of its LodKey. Centre them on the
	// raw camera instead and two things go wrong at once: the mesh a basin was
	// last built with depends on where inside the cell the camera happened to be,
	// and the uniform/banded verdict becomes a bare distance threshold that
	// flaps -- rebuilding a basin every frame while the camera jitters across it.
	const double CellUU = double(FMath::Max(LodSnapPx, 1)) * kFinePixelUU;
	const FIntPoint Key = LodKeyForCamera(CamUU);
	return FVector2D((double(Key.X) + 0.5) * CellUU, (double(Key.Y) + 0.5) * CellUU);
}

bool AVoxelWaterSheetActor::IsBandedAtCamera(const FSheet& Sheet, const FVector& CamUU) const
{
	if (Sheet.StepPx <= 1)
	{
		return false; // a pond already meshes at full lattice resolution everywhere
	}
	// The outermost BOUNDED band. How many rungs a ladder has depends on Base, so
	// which radius is the last bounded one does too -- ask BuildLadder, which is
	// the one place the ladder is constructed, rather than restating it here.
	UVoxelWaterSubsystem::FLakeSheetLod Lod;
	BuildLadder(Sheet, Lod);
	if (Lod.NumBands <= 1)
	{
		return false;
	}
	const double OuterBandedUU = double(Lod.RadiusPx[Lod.NumBands - 2]) * kFinePixelUU;
	const FVector2D Snap = SnappedCamXY(CamUU);
	const double DX = FMath::Max(FMath::Max(Sheet.MinXUU - Snap.X, Snap.X - Sheet.MaxXUU), 0.0);
	const double DY = FMath::Max(FMath::Max(Sheet.MinYUU - Snap.Y, Snap.Y - Sheet.MaxYUU), 0.0);
	return FMath::Max(DX, DY) < OuterBandedUU;
}

void AVoxelWaterSheetActor::BuildLadder(const FSheet& Sheet,
                                        UVoxelWaterSubsystem::FLakeSheetLod& OutLod) const
{
	const int32 Base = Sheet.StepPx;

	// THE LADDER IS {1, 2, 4, Base}, keeping only the rungs finer than Base.
	//
	// Base is NOT rounded to a power of two, and the far field therefore keeps
	// exactly the decimation it already had: the exemplar 1564 m basin picks 7,
	// and 7 is what its outer sheet still meshes at. voxel-core aligns the band
	// edges to the LCM of the rungs (28 px here) rather than demanding they
	// divide each other, which is what makes an arbitrary Base affordable -- see
	// lakeSheetRectsBanded. Rounding Base up instead would have coarsened the
	// far field by up to 2x to buy nothing.
	const int32 BandCap = FMath::Clamp(MaxBands, 1, int32(UVoxelWaterSubsystem::FLakeSheetLod::kMaxBands));
	int32 Steps[UVoxelWaterSubsystem::FLakeSheetLod::kMaxBands];
	int32 N = 0;
	for (const int32 Candidate : {1, 2, 4})
	{
		if (Candidate < Base && N < BandCap - 1)
		{
			Steps[N++] = Candidate;
		}
	}
	Steps[N++] = Base; // always the outermost, always unbounded

	OutLod = UVoxelWaterSubsystem::FLakeSheetLod();
	OutLod.NumBands = N;

	const int32 FineRadiusPx = FMath::Max(1, FMath::CeilToInt((FineBandRadiusM * 100.0) / kFinePixelUU));
	for (int32 k = 0; k < N; ++k)
	{
		OutLod.StepPx[k] = Steps[k];
		// Doubling radii against doubling-ish steps: a rectangle's SCREEN size is
		// then roughly constant across the bands, which is the only sense in which
		// a decimation ladder can be even.
		OutLod.RadiusPx[k] = FineRadiusPx << k;
	}
}

void AVoxelWaterSheetActor::BuildLodForBasin(const FSheet& Sheet, const FVector& CamUU,
                                             UVoxelWaterSubsystem::FLakeSheetLod& OutLod,
                                             bool& bOutUniform) const
{
	BuildLadder(Sheet, OutLod);
	// The CELL centre, not the camera -- see SnappedCamXY. The mesh a basin
	// carries is then determined entirely by its LodKey, which is what makes
	// "rebuild when the key changes" both necessary and sufficient.
	const FVector2D Snap = SnappedCamXY(CamUU);
	OutLod.CamXUU = Snap.X;
	OutLod.CamYUU = Snap.Y;

	// UNIFORM means "every block of this basin lands in the last, unbounded
	// band", so its geometry does not depend on the camera at all and it can
	// leave the rebuild rotation for good.
	bOutUniform = !IsBandedAtCamera(Sheet, CamUU);
}

bool AVoxelWaterSheetActor::HoleForDatum(double SurfaceZUU, FBox2D& OutHoleUU) const
{
	const UWorld* World = GetWorld();
	const UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return false;
	}
	// ONE RENDERING PATH FOR LAKE BASINS (voxel.Water.MeshImplicitLakes = 0).
	//
	// The hole exists only because two renderers drew the same lake water and an
	// overlap of two coplanar surfaces z-fights. With the near-field voxel disc
	// retired for lakes there is no second renderer to hand over to -- and the
	// hole EDGE is itself the seam the owner is looking at: measured at +36.4/255
	// across adjacent pixels at matched depth and view angle. So no near disc, no
	// hole, no edge.
	//
	// THE ONE THING THAT STILL EARNS A HOLE is water the CA has taken over.
	// Digging into a lake mobilizes its bricks, and mobilized water IS still
	// meshed near-field (it always was -- the implicit sweep never drew it). That
	// water is real, moving, and must not have a flat sheet drawn on top of it.
	// So the hole is now keyed on the CA actually owning water at this datum
	// inside the disc, which is false for every untouched lake in the world and
	// costs one empty-set test to say so.
	if (!UVoxelWaterSubsystem::ShouldMeshImplicitLakes() && !Water->HasMobilizedWaterInImplicitDisc(SurfaceZUU))
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

bool AVoxelWaterSheetActor::RebuildSheet(FSheet& Sheet, const FVector& CamUU)
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

	UVoxelWaterSubsystem::FLakeSheetLod Lod;
	bool bUniform = true;
	BuildLodForBasin(Sheet, CamUU, Lod, bUniform);
	Sheet.bUniformCoarse = bUniform;
	Sheet.LodKey = LodKeyForCamera(CamUU);

	TArray<FBox2D> Rects;
	bool bResolved = false;
	Water->BuildLakeSheetRectsBanded(Basin, Lod, Rects, bResolved);
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

	// One COMPONENT per basin (see FSheet::Comp for the 9 ms measurement that
	// forced the split). CreateMeshSection rather than UpdateMeshSection because
	// a hole re-cut changes the TOPOLOGY (a rectangle becomes up to four), which
	// Update cannot express -- unlike the clipmap, whose vertex layout never
	// changes and which is right to prefer Update.
	// Always section 0 OF THIS BASIN'S OWN COMPONENT -- see FSheet::Comp. The
	// proxy recreate this triggers now converts one basin's vertices, not every
	// resident basin's.
	if (Verts.Num() == 0)
	{
		if (Sheet.Comp != nullptr)
		{
			Sheet.Comp->ClearAllMeshSections();
		}
		// No component yet = nothing ever drew = nothing to clear. Do NOT create
		// one here: an empty PMC still registers a primitive, and a basin that
		// decimates to zero rectangles at range would pay it for nothing.
	}
	else
	{
		UProceduralMeshComponent* C = GetOrCreateSheetComp(Sheet);
		C->CreateMeshSection(0, Verts, Tris, Normals, UVs, Colors, Tangents,
		                     /*bCreateCollision*/ false);
		C->SetMaterial(0, WaterMaterial);
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

	// Destroy parked leftovers a few per tick. 32 x ~37 us (the measured
	// per-component DestroyComponent cost on the LSFIX leg) is ~1.2 ms worst
	// case -- against the 17.7 ms the old destroy-all-in-one-tick hitch cost.
	// Drained BEFORE any other work so a teleport's 495 leftovers cannot starve
	// behind a busy rebuild rotation.
	{
		int32 Destroyed = 0;
		while (PendingDestroy.Num() > 0 && Destroyed < 32)
		{
			UProceduralMeshComponent* C = PendingDestroy.Pop(EAllowShrinking::No);
			if (C != nullptr)
			{
				C->DestroyComponent();
			}
			++Destroyed;
		}
	}
	UWorld* World = GetWorld();
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return;
	}

	// ---- Anchor: is there a real position to gather around yet? -------------
	//
	// GetCameraLocationUU now REFUSES to answer until a pawn exists, because
	// the answer it used to give before that was (0,0,0) and it cost 10,814
	// fine-tier gate leaks into four unbaked origin tiles -- each answered at
	// SEA LEVEL, and fatal on the first one under -unattended. See that
	// function's note.
	//
	// AND THE WAIT IS COUNTED, not swallowed. A skip that logs nothing is the
	// same failure in the other direction, and this project has shipped that
	// shape before (the weathering pass that removed 20 voxels of 90,000 for
	// months). "No lake sheets drew" and "no lake sheets were ever asked for"
	// have to be different lines in a log, so the deferral is announced once,
	// warned about if it drags, and reported by the first gather that follows.
	FVector CamUU;
	if (!GetCameraLocationUU(CamUU))
	{
		const double NowSec = FPlatformTime::Seconds();
		if (DeferredGatherTicks == 0)
		{
			FirstDeferSeconds = NowSec;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: no possessed pawn yet -- DEFERRING the first basin gather. Gathering now would "
			            "scan the four fine tiles meeting at the world origin, which are unbaked; every elevation "
			            "read into them is answered at sea level, and the first one is FATAL under -unattended."));
		}
		++DeferredGatherTicks;
		// A menu session is a legitimate forever-wait: VoxelFrontEnd::
		// IsEnabledThisRun makes AVoxelEarthGameMode set
		// bStartPlayersAsSpectators, so no pawn is spawned at StartPlay and one
		// only appears on NEW GAME/CONTINUE. A -game capture is not: ten
		// seconds with no pawn there means the run is stuck on something else,
		// and absent lakes are a symptom of that rather than a lake bug. Say it
		// once; do not repeat it every tick on the menu.
		if (!bWarnedLongDefer && NowSec - FirstDeferSeconds > 10.0)
		{
			bWarnedLongDefer = true;
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("Lake sheets: still no possessed pawn after %d tick(s) / %.1f s. NO LAKE SHEETS WILL DRAW "
			            "until one exists. Expected while the front-end menu is up (VoxelFrontEnd::IsEnabledThisRun); "
			            "on a -game capture it means the pawn never spawned, which is not a water problem."),
			       DeferredGatherTicks, NowSec - FirstDeferSeconds);
		}
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
		const bool bFirstGather = !bGathered;
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
		// PARK, DON'T DESTROY -- see AdoptableSheets in the header. Destroying
		// everything here cost a measured 17.7 ms single-tick hitch and deleted
		// every lake for the ~5 s the rebuild drain took; parking lets the
		// re-gather adopt unchanged basins (~98% on the measured legs) and keeps
		// them DRAWING throughout. Whatever the new gather does not reclaim is
		// flushed to PendingDestroy when the last tile lands, and destroyed a few
		// per tick -- an out-of-range lake therefore lingers a few extra ticks,
		// which is the deliberate price of never blinking the in-range ones.
		for (FSheet& Old : Sheets)
		{
			AdoptableSheets.Add(FIntVector(Old.TileX, Old.TileY, Old.BasinId), Old);
		}
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
		if (bFirstGather)
		{
			// ALWAYS PRINTED, INCLUDING "0 deferred tick(s)". The fix is that
			// the first gather runs at a position something was actually placed
			// at; the proof is this line naming that position and what it
			// waited for. Printing it only when the wait was non-zero would
			// make its ABSENCE ambiguous -- which is precisely how a gather at
			// (0,0) went unnoticed until a leak counter forced the question.
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: FIRST gather ran at (%.0f, %.0f) after %d deferred tick(s), %.2f s waiting for "
			            "a possessed pawn. A first gather at (0, 0) would be the origin-gather bug, not a spawn on "
			            "the origin -- see GetCameraLocationUU."),
			       CamUU.X, CamUU.Y, DeferredGatherTicks,
			       DeferredGatherTicks > 0 ? FPlatformTime::Seconds() - FirstDeferSeconds : 0.0);
		}
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
			// ADOPT the previous gather's component when the basin is unchanged --
			// same datum, same extent, same decimation -- so it never rebuilds and
			// never stops drawing. Geometry equality is exact-compare on purpose:
			// these fields are copied from the basin table, not derived, so an
			// unchanged basin reproduces them bit-for-bit, and a tolerance would
			// only invent a class of nearly-adopted basins with stale meshes.
			// An entry that exists but fails the test is a CHANGED basin: its old
			// component is queued for destruction and it rebuilds fresh.
			if (FSheet* Old = AdoptableSheets.Find(FIntVector(S.TileX, S.TileY, S.BasinId)))
			{
				const bool bSame = Old->bBuilt && !Old->bUnresolved && Old->Comp != nullptr
					&& Old->SurfaceZUU == S.SurfaceZUU && Old->StepPx == S.StepPx
					&& Old->MinXUU == S.MinXUU && Old->MinYUU == S.MinYUU
					&& Old->MaxXUU == S.MaxXUU && Old->MaxYUU == S.MaxYUU;
				if (bSame)
				{
					S.Comp = Old->Comp;
					S.bBuilt = true;
					S.LodKey = Old->LodKey;
					S.bUniformCoarse = Old->bUniformCoarse;
					S.bHadHole = Old->bHadHole;
					S.HoleUU = Old->HoleUU;
					S.RectCount = Old->RectCount;
					TotalRects += Old->RectCount;
				}
				else if (Old->Comp != nullptr)
				{
					PendingDestroy.Add(Old->Comp);
				}
				AdoptableSheets.Remove(FIntVector(S.TileX, S.TileY, S.BasinId));
			}
			Sheets.Add(S);
		}
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Lake sheets: tile (%d,%d) contributed %d basin(s) in %.1f ms; %d tile(s) left, %d basin(s) so far"),
		       T.X, T.Y, Found.Num(), (FPlatformTime::Seconds() - StartSec) * 1000.0, PendingTiles.Num(),
		       Sheets.Num());
		if (PendingTiles.Num() == 0 && AdoptableSheets.Num() > 0)
		{
			// The gather is complete, so anything still parked is a basin the new
			// scan does not contain -- out of range, or gone from the table. Flush
			// to the amortised destroy queue; see the park site for why they were
			// kept drawing until now.
			for (TPair<FIntVector, FSheet>& Left : AdoptableSheets)
			{
				if (Left.Value.Comp != nullptr)
				{
					PendingDestroy.Add(Left.Value.Comp);
				}
			}
			int32 Adopted = 0;
			for (const FSheet& Sh : Sheets)
			{
				if (Sh.bBuilt)
				{
					++Adopted; // built at gather-complete = adopted (nothing has rebuilt yet)
				}
			}
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: gather complete -- %d of %d basin(s) adopted intact, %d leftover ")
			       TEXT("component(s) queued for amortised destroy."),
			       Adopted, Sheets.Num(), AdoptableSheets.Num());
			AdoptableSheets.Empty();
		}
		return; // one unit of work per tick, tile reads included
	}

	// ---- Build/refresh: at most one basin per tick --------------------------
	//
	// A basin is rebuilt when it has no mesh yet, when its LOD bands need
	// re-centring, or when the near-field hole it was cut with no longer matches
	// the disc the water subsystem is meshing now. The last case is normally ZERO
	// basins: with lake basins on one rendering path a hole is only owed where
	// the CA has actually taken water over.
	const FIntPoint LodKey = LodKeyForCamera(CamUU);
	const int32 Num = Sheets.Num();
	for (int32 Step = 0; Step < Num; ++Step)
	{
		const int32 Index = (RoundRobinCursor + Step) % Num;
		FSheet& S = Sheets[Index];

		// THE LOD TRIGGER, and it has to have BOTH terms.
		//
		// The second alone is the obvious version and it is wrong in a way that
		// only shows up when you walk towards a lake: a basin built while it was
		// far away is flagged uniform, and a trigger that reads only the flag
		// never rebuilds it, so approaching a lake leaves it meshed at its coarse
		// far-field step forever. The first term is the state FLIP -- uniform
		// becoming banded as you approach, and banded becoming uniform as you
		// leave -- evaluated against the live camera every tick.
		//
		// Both are functions of the SNAPPED camera cell, so neither can flap: the
		// verdict changes only when LodKey does.
		const bool bBandedNow = IsBandedAtCamera(S, CamUU);
		const bool bLodStale = (bBandedNow == S.bUniformCoarse) || (bBandedNow && S.LodKey != LodKey);

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
		if (S.bUnresolved || (S.bBuilt && !bLodStale && !(bHoleChanged && bHoleTouches)))
		{
			continue;
		}

		if (!RebuildSheet(S, CamUU))
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
