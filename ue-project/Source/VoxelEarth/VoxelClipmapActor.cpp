#include "VoxelClipmapActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelWorldSubsystem.h"

// voxelcore/tiles.h ONLY here, in the .cpp -- VoxelClipmapActor.h is
// UHT-parsed and must stay voxel-core-free (doctrine, see
// VoxelWorldSubsystem.h's PImpl comment; the class comment above repeats
// this for context local to this file).
#include "voxelcore/tiles.h"

namespace
{
// m2-plan.md "Height source" row: TILE elevation directly (30m/px bilinear),
// through a vxc::SyntheticTileSampler using the SAME seed the voxel world
// uses (UVoxelWorldSubsystem::DefaultSeed) so clipmap terrain lines up with
// the ring cascade at their shared seam. SyntheticTileSampler is stateless
// (holds only seed_/pixelSizeMm_), so a function-local static instance is
// safe to reuse across every call -- game-thread only (AVoxelClipmapActor's
// Tick/RebuildLevel never run off-thread), so no synchronization is needed.
double SampleHeightUU(double WorldXUU, double WorldYUU)
{
	static vxc::SyntheticTileSampler Tiles(UVoxelWorldSubsystem::DefaultSeed);

	// mm -> UU is /10 (VoxelCoords::WorldToMm's inverse: 1 UU = 10 mm).
	const double PixelSizeUU = double(Tiles.pixelSizeMm()) / 10.0;
	const double Px = WorldXUU / PixelSizeUU;
	const double Py = WorldYUU / PixelSizeUU;
	const int64 Px0 = (int64)FMath::FloorToDouble(Px);
	const int64 Py0 = (int64)FMath::FloorToDouble(Py);
	const double Fx = Px - double(Px0);
	const double Fy = Py - double(Py0);

	// No capture needed: Tiles has static storage duration (accessible
	// directly, same as any other file/function-scope static).
	auto ElevUU = [](int64 X, int64 Y) { return double(Tiles.elevationMm(X, Y)) / 10.0; };
	const double H00 = ElevUU(Px0, Py0);
	const double H10 = ElevUU(Px0 + 1, Py0);
	const double H01 = ElevUU(Px0, Py0 + 1);
	const double H11 = ElevUU(Px0 + 1, Py0 + 1);
	const double Hx0 = FMath::Lerp(H00, H10, Fx);
	const double Hx1 = FMath::Lerp(H01, H11, Fx);
	return FMath::Lerp(Hx0, Hx1, Fy);
}

// Snow band (m2-plan.md "Material" row: "white above snowline (2800m,
// matching amplifier constants)") -- a linear ramp centred on the
// amplifier's 2800m snowline (voxel-core/src/amplifier.cpp,
// voxel-core/shaders/worldgen.hlsl MAT_SNOW threshold) rather than a hard
// cutoff, so the clipmap's coarse (64-512m/vertex) grid doesn't show a
// jagged single-vertex-row edge at the line.
constexpr double kSnowlineBandLowMeters = 2700.0;
constexpr double kSnowlineBandHighMeters = 2900.0;
} // namespace

AVoxelClipmapActor::AVoxelClipmapActor()
{
	PrimaryActorTick.bCanEverTick = true;

	ClipmapRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ClipmapRoot"));
	SetRootComponent(ClipmapRoot);

	// Same load-in-constructor pattern AVoxelOceanActor uses for M_Ocean:
	// StaticLoadObject works fine at CDO construction time, so no level
	// component ever renders with the engine default material for a frame.
	ClipmapMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_VoxelClipmap.M_VoxelClipmap")));

	LevelMeshes.SetNum(NumLevels);
	for (int32 Level = 0; Level < NumLevels; ++Level)
	{
		UProceduralMeshComponent* PMC = CreateDefaultSubobject<UProceduralMeshComponent>(
			*FString::Printf(TEXT("ClipmapLevel%d"), Level));
		PMC->SetupAttachment(ClipmapRoot);
		PMC->SetMobility(EComponentMobility::Movable); // recentred every rebuild, see RebuildLevel
		PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision); // v1: cosmetic distant terrain (m2-plan.md scope: no walking out here yet)
		PMC->bUseAsyncCooking = false; // no collision to cook, nothing to gain from async
		if (ClipmapMaterial)
		{
			PMC->SetMaterial(0, ClipmapMaterial);
		}
		LevelMeshes[Level] = PMC;
	}
}

void AVoxelClipmapActor::BeginPlay()
{
	Super::BeginPlay();

	if (!ClipmapMaterial)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("M_VoxelClipmap not found at /Game/Voxel/M_VoxelClipmap -- clipmap levels using the engine default material."));
	}

	BuildSharedTopology();
}

double AVoxelClipmapActor::SpacingUUForLevel(int32 LevelIndex)
{
	// Level 0's inner hole must land exactly on the ring cascade's own
	// outer edge (UVoxelWorldSubsystem::RingPresets' last entry, R4 outer,
	// ~1km) -- hole half-extent = HoleHalfIndex * spacing (see class
	// comment / HoleHalfIndex doc), so spacing0 = ringEdgeUU / HoleHalfIndex.
	// Every subsequent level doubles spacing (doubling-annulus clipmap,
	// same structure UVoxelWorldSubsystem::RingPresets already uses for
	// R0-R4, extended outward): level L's hole then lands exactly on level
	// L-1's outer edge, by construction (both scale with the same 1<<L
	// factor), for every level.
	static const double RingEdgeUU = UVoxelWorldSubsystem::RingPresets[VoxelCoords::kNumLevels - 1].OuterMeters * 100.0;
	static const double Spacing0UU = RingEdgeUU / double(HoleHalfIndex);
	return Spacing0UU * double(int64(1) << LevelIndex);
}

void AVoxelClipmapActor::BuildSharedTopology()
{
	if (bTopologyBuilt)
	{
		return;
	}

	// UV0: plain [0,1]^2 across the grid -- not consumed by M_VoxelClipmap
	// (slope/height tint is entirely vertex-color-driven, see
	// Tools/create_clipmap_material.py), but PMC requires a UV0 array sized
	// to match Vertices, and a future texture pass wants this ready-made.
	SharedUV0.SetNumUninitialized(NumVertsTotal);
	for (int32 i = 0; i < NumVertsPerSide; ++i)
	{
		for (int32 j = 0; j < NumVertsPerSide; ++j)
		{
			SharedUV0[i * NumVertsPerSide + j] =
				FVector2D(double(i) / double(NumVertsPerSide - 1), double(j) / double(NumVertsPerSide - 1));
		}
	}

	// Annulus mask (m2-plan.md "Cracks/overlap" row): skip any quad fully
	// inside the [HalfIndex-HoleHalfIndex, HalfIndex+HoleHalfIndex) hole --
	// a finer level (or, for level 0, the voxel ring cascade) covers that
	// area instead. This mask, like every other part of the grid layout, is
	// identical for all 4 levels (see class comment), hence built once and
	// shared.
	SharedTriangles.Reset();
	SharedTriangles.Reserve((NumVertsPerSide - 1) * (NumVertsPerSide - 1) * 6);
	for (int32 i = 0; i < NumVertsPerSide - 1; ++i)
	{
		for (int32 j = 0; j < NumVertsPerSide - 1; ++j)
		{
			const bool bHoleQuad = (i >= HalfIndex - HoleHalfIndex) && (i < HalfIndex + HoleHalfIndex) &&
			                       (j >= HalfIndex - HoleHalfIndex) && (j < HalfIndex + HoleHalfIndex);
			if (bHoleQuad)
			{
				continue;
			}

			const int32 V00 = i * NumVertsPerSide + j;
			const int32 V10 = (i + 1) * NumVertsPerSide + j;
			const int32 V01 = i * NumVertsPerSide + (j + 1);
			const int32 V11 = (i + 1) * NumVertsPerSide + (j + 1);

			// Winding picked by hand (not visually verifiable in this
			// headless task) -- M_VoxelClipmap is two-sided defensively,
			// see Tools/create_clipmap_material.py.
			SharedTriangles.Add(V00);
			SharedTriangles.Add(V01);
			SharedTriangles.Add(V11);
			SharedTriangles.Add(V00);
			SharedTriangles.Add(V11);
			SharedTriangles.Add(V10);
		}
	}

	bTopologyBuilt = true;
}

void AVoxelClipmapActor::RebuildLevel(int32 LevelIndex, const FVector2D& SnappedOriginUU)
{
	BuildSharedTopology();

	UProceduralMeshComponent* PMC = LevelMeshes.IsValidIndex(LevelIndex) ? LevelMeshes[LevelIndex] : nullptr;
	if (!PMC)
	{
		return;
	}

	const double Spacing = SpacingUUForLevel(LevelIndex);
	const double SkirtDropUU = 2.0 * Spacing; // m2-plan.md "Cracks/overlap" row

	// Pass 1: sample heights (bilinear tile taps -- "trivially cheap" per
	// m2-plan.md's Recenter row) and lay out local-space XY (offsets from
	// SnappedOriginUU; the component's own relative location supplies the
	// translation, set at the end of this function).
	TArray<double> HeightsUU;
	HeightsUU.SetNumUninitialized(NumVertsTotal);
	TArray<FVector> Positions;
	Positions.SetNumUninitialized(NumVertsTotal);

	for (int32 i = 0; i < NumVertsPerSide; ++i)
	{
		const double LocalX = double(i - HalfIndex) * Spacing;
		const double WorldX = SnappedOriginUU.X + LocalX;
		for (int32 j = 0; j < NumVertsPerSide; ++j)
		{
			const double LocalY = double(j - HalfIndex) * Spacing;
			const double WorldY = SnappedOriginUU.Y + LocalY;
			const double HeightUU = SampleHeightUU(WorldX, WorldY);
			const int32 Idx = i * NumVertsPerSide + j;
			HeightsUU[Idx] = HeightUU;
			Positions[Idx] = FVector(LocalX, LocalY, HeightUU);
		}
	}

	// Pass 2: normals (central-difference heightmap gradient, clamped to
	// forward/backward differences at the grid border) + slope/snow vertex
	// colors, computed from the UN-skirted heights so shading reflects the
	// real terrain shape.
	TArray<FVector> Normals;
	Normals.SetNumUninitialized(NumVertsTotal);
	TArray<FColor> VertexColors;
	VertexColors.SetNumUninitialized(NumVertsTotal);

	for (int32 i = 0; i < NumVertsPerSide; ++i)
	{
		const bool bInteriorX = (i > 0 && i < NumVertsPerSide - 1);
		const int32 IL = (i > 0) ? i - 1 : i;
		const int32 IR = (i < NumVertsPerSide - 1) ? i + 1 : i;
		for (int32 j = 0; j < NumVertsPerSide; ++j)
		{
			const bool bInteriorY = (j > 0 && j < NumVertsPerSide - 1);
			const int32 JD = (j > 0) ? j - 1 : j;
			const int32 JU = (j < NumVertsPerSide - 1) ? j + 1 : j;

			const double HL = HeightsUU[IL * NumVertsPerSide + j];
			const double HR = HeightsUU[IR * NumVertsPerSide + j];
			const double HD = HeightsUU[i * NumVertsPerSide + JD];
			const double HU = HeightsUU[i * NumVertsPerSide + JU];

			const double DhDx = (HR - HL) / (Spacing * (bInteriorX ? 2.0 : 1.0));
			const double DhDy = (HU - HD) / (Spacing * (bInteriorY ? 2.0 : 1.0));

			const int32 Idx = i * NumVertsPerSide + j;
			const FVector Normal = FVector(-DhDx, -DhDy, 1.0).GetSafeNormal();
			Normals[Idx] = Normal;

			const double Slope = FMath::Clamp(1.0 - double(Normal.Z), 0.0, 1.0);
			const double HeightMeters = HeightsUU[Idx] / 100.0;
			const double Snow = FMath::Clamp(
				(HeightMeters - kSnowlineBandLowMeters) / (kSnowlineBandHighMeters - kSnowlineBandLowMeters), 0.0, 1.0);

			VertexColors[Idx] = FColor(
				(uint8)FMath::Clamp(FMath::RoundToInt32(Slope * 255.0), 0, 255),
				(uint8)FMath::Clamp(FMath::RoundToInt32(Snow * 255.0), 0, 255),
				0, 255);
		}
	}

	// Pass 3: skirts -- drop the outer grid edge AND the inner hole
	// boundary by 2x this level's spacing (position only, after
	// normals/colors are computed -- m2-plan.md "Cracks/overlap" row, class
	// comment).
	for (int32 i = 0; i < NumVertsPerSide; ++i)
	{
		const int32 Dx = FMath::Abs(i - HalfIndex);
		const bool bOuterX = (i == 0 || i == NumVertsPerSide - 1);
		const bool bHoleEdgeX = (Dx == HoleHalfIndex);
		for (int32 j = 0; j < NumVertsPerSide; ++j)
		{
			const int32 Dy = FMath::Abs(j - HalfIndex);
			const bool bOuterY = (j == 0 || j == NumVertsPerSide - 1);
			const bool bHoleEdgeY = (Dy == HoleHalfIndex);

			const bool bOuterSkirt = bOuterX || bOuterY;
			const bool bHoleSkirt = (bHoleEdgeX && Dy <= HoleHalfIndex) || (bHoleEdgeY && Dx <= HoleHalfIndex);
			if (bOuterSkirt || bHoleSkirt)
			{
				Positions[i * NumVertsPerSide + j].Z -= SkirtDropUU;
			}
		}
	}

	const bool bFirstBuild = !bLevelBuilt[LevelIndex];
	if (bFirstBuild)
	{
		// Topology (SharedTriangles/SharedUV0) never changes for a given
		// level after this -- every later rebuild uses UpdateMeshSection
		// instead, which is strictly cheaper (no scene proxy recreation).
		PMC->CreateMeshSection(0, Positions, SharedTriangles, Normals, SharedUV0, VertexColors, TArray<FProcMeshTangent>(),
		                       /*bCreateCollision*/ false);
		if (ClipmapMaterial)
		{
			PMC->SetMaterial(0, ClipmapMaterial);
		}
	}
	else
	{
		PMC->UpdateMeshSection(0, Positions, Normals, SharedUV0, VertexColors, TArray<FProcMeshTangent>());
	}

	PMC->SetRelativeLocation(FVector(SnappedOriginUU.X, SnappedOriginUU.Y, 0.0));
}

bool AVoxelClipmapActor::GetCameraLocationUU(FVector& OutCameraLocationUU) const
{
	// Same fallback chain as AVoxelOceanActor::UpdateFollowPlane.
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return false;
	}
	if (PC->PlayerCameraManager)
	{
		OutCameraLocationUU = PC->PlayerCameraManager->GetCameraLocation();
		return true;
	}
	if (APawn* Pawn = PC->GetPawn())
	{
		OutCameraLocationUU = Pawn->GetActorLocation();
		return true;
	}
	return false;
}

void AVoxelClipmapActor::UpdateDebugTint()
{
	const bool bRingsEnabled = VoxelDebug::IsRingsEnabled();
	if (bRingsEnabled == bLastRingsEnabled)
	{
		return; // no transition -- matches the "no MIDs created while off" doctrine (see class comment)
	}
	bLastRingsEnabled = bRingsEnabled;

	for (UProceduralMeshComponent* PMC : LevelMeshes)
	{
		if (!PMC)
		{
			continue;
		}
		if (bRingsEnabled)
		{
			// UProceduralMeshComponent derives from UMeshComponent (unlike
			// UVoxelChunkComponent's bare UPrimitiveComponent), so the
			// built-in helper is available directly.
			if (UMaterialInstanceDynamic* MID = PMC->CreateDynamicMaterialInstance(0, ClipmapMaterial))
			{
				MID->SetVectorParameterValue(TEXT("DebugTint"), VoxelDebug::HeightmapBandTint());
			}
		}
		else if (ClipmapMaterial)
		{
			PMC->SetMaterial(0, ClipmapMaterial);
		}
	}
}

void AVoxelClipmapActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateDebugTint();

	FVector CameraLocUU;
	if (!GetCameraLocationUU(CameraLocUU))
	{
		return; // nothing to recenter around yet
	}

	if (!bBootstrapped)
	{
		// One-time: build every level immediately (see class comment
		// "Recenter") so terrain doesn't pop in over several frames at
		// spawn -- a one-off cost, not a recurring per-frame one.
		for (int32 Level = 0; Level < NumLevels; ++Level)
		{
			const double Spacing = SpacingUUForLevel(Level);
			const FVector2D SnappedOrigin(FMath::GridSnap(CameraLocUU.X, Spacing), FMath::GridSnap(CameraLocUU.Y, Spacing));
			RebuildLevel(Level, SnappedOrigin);
			LastSnappedOriginUU[Level] = SnappedOrigin;
			bLevelBuilt[Level] = true;
		}
		bBootstrapped = true;
		return;
	}

	// Steady state: round-robin scan for the first dirty level starting at
	// the cursor, rebuild at most that one this tick (m2-plan.md "Recenter"
	// row: "levels update round-robin (<=1 level rebuild per frame)").
	for (int32 Step = 0; Step < NumLevels; ++Step)
	{
		const int32 Level = (RoundRobinCursor + Step) % NumLevels;
		const double Spacing = SpacingUUForLevel(Level);
		const FVector2D SnappedOrigin(FMath::GridSnap(CameraLocUU.X, Spacing), FMath::GridSnap(CameraLocUU.Y, Spacing));
		if (!bLevelBuilt[Level] || SnappedOrigin != LastSnappedOriginUU[Level])
		{
			RebuildLevel(Level, SnappedOrigin);
			LastSnappedOriginUU[Level] = SnappedOrigin;
			bLevelBuilt[Level] = true;
			RoundRobinCursor = (Level + 1) % NumLevels;
			break;
		}
	}
}
