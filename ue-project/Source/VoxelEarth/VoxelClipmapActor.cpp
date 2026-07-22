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
// m2-plan.md "Height source" row: TILE elevation directly (30m/px bilinear).
//
// Track B2 ("real .vxtl terrain tiles as a selectable tile source"): the
// primary path now routes through UVoxelWorldSubsystem::SampleTerrainHeightUU,
// which samples whichever ITileSampler the voxel world is ACTUALLY using this
// run (the synthetic sampler by default, or a loaded vxc::TileGridSampler
// under -VoxelTileDir=<path>) -- this is what keeps clipmap terrain and the
// ring cascade reading the SAME tiles and lining up at their shared seam,
// with or without real tiles loaded. Subsystem==nullptr (genuinely
// unavailable -- should not happen in practice; BeginPlay already warns if
// so) falls back to a fresh vxc::SyntheticTileSampler seeded from Seed,
// preserving the exact pre-Track-B2 behavior for that edge case.
// SyntheticTileSampler is stateless (holds only seed_/pixelSizeMm_, no heap
// allocation), so constructing one fresh per call is negligible. Game-thread
// only (AVoxelClipmapActor's Tick/RebuildLevel never run off-thread), so no
// synchronization is needed either way.
double SampleHeightUU(double WorldXUU, double WorldYUU, const UVoxelWorldSubsystem* Subsystem, uint64 Seed)
{
	if (Subsystem)
	{
		return Subsystem->SampleTerrainHeightUU(WorldXUU, WorldYUU);
	}

	vxc::SyntheticTileSampler Tiles(Seed); // elevationMm() is non-const (ITileSampler), so Tiles can't be const here

	// mm -> UU is /10 (VoxelCoords::WorldToMm's inverse: 1 UU = 10 mm).
	const double PixelSizeUU = double(Tiles.pixelSizeMm()) / 10.0;
	const double Px = WorldXUU / PixelSizeUU;
	const double Py = WorldYUU / PixelSizeUU;
	const int64 Px0 = (int64)FMath::FloorToDouble(Px);
	const int64 Py0 = (int64)FMath::FloorToDouble(Py);
	const double Fx = Px - double(Px0);
	const double Fy = Py - double(Py0);

	auto ElevUU = [&Tiles](int64 X, int64 Y) { return double(Tiles.elevationMm(X, Y)) / 10.0; };
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

// ---- UNDERGROUND VEIL constants (see VoxelClipmapActor.h for the diagnosis)
//
// Half-extent of the inward-facing occluder box. Must sit OUTSIDE every piece
// of voxel geometry so it can never occlude a real cave surface: the ring
// cascade tops out at RingPresets R4 outer = 1024 m, so 2 km is a 2x margin.
// It is deliberately INSIDE the clipmap's ~16.4 km reach -- occluding the
// clipmap is the point.
constexpr double kVeilHalfExtentUU = 200000.0; // 2 km

// Near-black rock. Multiplied into M_VoxelClipmap's BaseColor via its existing
// DebugTint VectorParameter, so the veil needs NO new material asset (and no
// Content/ change at all). Roughness 0.9 / metallic 0 means the lit result is
// essentially this value, whichever way a given box face happens to point --
// which is why the wrong-facing-normal problem that sinks a naive "make the
// terrain two-sided" fix does not apply here.
const FLinearColor kVeilTint(0.020f, 0.020f, 0.026f, 1.0f);

// Upward rock probe. Steps 2 m at a time to 80 m; two solid samples (~2 m of
// rock overhead) latches the veil ON, zero solid samples latches it OFF. The
// gap between those two conditions IS the hysteresis -- no separate
// enter/exit depth constants needed.
constexpr double kVeilProbeStepUU = 200.0;
constexpr double kVeilProbeMaxUU = 8000.0;
constexpr int32 kVeilProbeSolidsToLatch = 2;
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

	// M2 task "Config-driven seed": read the subsystem's RUNTIME seed
	// (-VoxelSeed=<u64> override, else DefaultSeed) rather than the
	// DefaultSeed constant, so the clipmap's heightmap sampling stays
	// seed-matched with the ring cascade even when -VoxelSeed overrides the
	// default (see SampleHeightUU's comment). The subsystem's Initialize()
	// (world-init time) always runs before this actor's BeginPlay
	// (spawned from AVoxelEarthGameMode::BeginPlay), so the seed is already
	// resolved by the time this reads it. Falls back to DefaultSeed with a
	// warning if the subsystem is somehow unavailable.
	if (const UWorld* World = GetWorld())
	{
		if (const UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>())
		{
			TerrainSeed = Subsystem->GetSeed();
		}
		else
		{
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("AVoxelClipmapActor::BeginPlay: UVoxelWorldSubsystem unavailable -- falling back to DefaultSeed (%llu); ")
			       TEXT("clipmap terrain may not line up with the ring cascade if -VoxelSeed was passed."),
			       (unsigned long long)UVoxelWorldSubsystem::DefaultSeed);
		}
	}

	// -VoxelUndergroundVeil=0 turns the veil off on the SAME binary, so the
	// before/after A/B (and the M1 perf A/B) never has to compare two builds.
	int32 VeilFlag = 1;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelUndergroundVeil="), VeilFlag))
	{
		bVeilEnabled = (VeilFlag != 0);
	}
	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelUndergroundVeil: %s"), bVeilEnabled ? TEXT("enabled") : TEXT("DISABLED"));

	BuildSharedTopology();
}

void AVoxelClipmapActor::EnsureVeilShell()
{
	if (VeilShell)
	{
		return;
	}

	VeilShell = NewObject<UProceduralMeshComponent>(this, TEXT("UndergroundVeilShell"));
	VeilShell->SetupAttachment(ClipmapRoot);
	VeilShell->RegisterComponent();
	VeilShell->SetMobility(EComponentMobility::Movable);
	VeilShell->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VeilShell->bUseAsyncCooking = false;
	// The box is recentred on the camera every tick, so a bounds-based cull
	// would be computing a cull for a primitive that always contains the
	// view point. Skip it rather than let a stale bound flicker the veil off.
	VeilShell->bUseAttachParentBound = false;
	VeilShell->SetCastShadow(false); // pure occluder; casting from it would black out the cave

	const double H = kVeilHalfExtentUU;
	// 8 corners, z-minor ordering: index bit0 = +X, bit1 = +Y, bit2 = +Z.
	TArray<FVector> V;
	V.Reserve(8);
	for (int32 B = 0; B < 8; ++B)
	{
		V.Add(FVector((B & 1) ? H : -H, (B & 2) ? H : -H, (B & 4) ? H : -H));
	}

	// INWARD-facing winding: each face is wound so its geometric normal points
	// at the box centre (i.e. at the camera). This is the one place in this
	// file where winding is load-bearing and NOT defensively covered by
	// M_VoxelClipmap's two-sidedness -- two-sided renders it either way, but
	// the normals below are what the shading reads, so they are authored
	// inward to match.
	TArray<int32> T;
	TArray<FVector> N;
	N.SetNum(8);
	auto AddFace = [&T](int32 A, int32 B, int32 C, int32 D)
	{
		T.Add(A); T.Add(B); T.Add(C);
		T.Add(A); T.Add(C); T.Add(D);
	};
	AddFace(0, 2, 3, 1); // -Z, seen from above inside
	AddFace(4, 5, 7, 6); // +Z
	AddFace(0, 1, 5, 4); // -Y
	AddFace(2, 6, 7, 3); // +Y
	AddFace(0, 4, 6, 2); // -X
	AddFace(1, 3, 7, 5); // +X
	for (int32 B = 0; B < 8; ++B)
	{
		N[B] = (-V[B]).GetSafeNormal(); // corner normal points at the centre
	}

	TArray<FVector2D> UV;
	UV.Init(FVector2D::ZeroVector, 8);
	TArray<FColor> C;
	// VertexColor.R = slope, .G = snow. Zero on both keeps M_VoxelClipmap on
	// its flat-terrain base colour, which the near-black tint then crushes.
	C.Init(FColor(0, 0, 0, 255), 8);

	VeilShell->CreateMeshSection(0, V, T, N, UV, C, TArray<FProcMeshTangent>(), /*bCreateCollision*/ false);

	if (ClipmapMaterial)
	{
		VeilShellMID = VeilShell->CreateDynamicMaterialInstance(0, ClipmapMaterial);
		if (VeilShellMID)
		{
			VeilShellMID->SetVectorParameterValue(TEXT("DebugTint"), kVeilTint);
		}
	}
	VeilShell->SetVisibility(false);
}

bool AVoxelClipmapActor::IsCameraUnderRock(const FVector& CameraLocUU) const
{
	const UWorld* World = GetWorld();
	const UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return false; // fail OPEN: never veil the vista on a missing subsystem
	}

	const int64 Vx = (int64)FMath::FloorToDouble(CameraLocUU.X / VoxelCoords::VoxelSizeUU);
	const int64 Vy = (int64)FMath::FloorToDouble(CameraLocUU.Y / VoxelCoords::VoxelSizeUU);

	int32 Solids = 0;
	for (double Up = kVeilProbeStepUU; Up <= kVeilProbeMaxUU; Up += kVeilProbeStepUU)
	{
		const int64 Vz = (int64)FMath::FloorToDouble((CameraLocUU.Z + Up) / VoxelCoords::VoxelSizeUU);
		if (Subsystem->IsSolidAtVoxel(Vx, Vy, Vz))
		{
			if (++Solids >= kVeilProbeSolidsToLatch)
			{
				return true; // early-out: this is the common case underground
			}
		}
	}
	// Latch OFF only on ZERO solids overhead; 1 solid sample holds the
	// previous state (see kVeilProbeSolidsToLatch's comment -- the caller
	// implements that hold).
	return Solids > 0 ? bVeilActive : false;
}

void AVoxelClipmapActor::SetVeilActive(bool bActive)
{
	// No transition -- same "never touch components without a transition"
	// doctrine as UpdateDebugTint. bVeilStateKnown (not "VeilShell != null")
	// is the right guard: the shell is created lazily on the first ON, so
	// keying off it made every above-ground tick re-run the else-branch and
	// log once per frame.
	if (bVeilStateKnown && bActive == bVeilActive)
	{
		return;
	}
	bVeilStateKnown = true;
	bVeilActive = bActive;

	if (bActive)
	{
		EnsureVeilShell();
	}
	if (VeilShell)
	{
		VeilShell->SetVisibility(bActive);
	}
	for (UProceduralMeshComponent* PMC : LevelMeshes)
	{
		if (PMC)
		{
			// Hidden, not destroyed: the levels keep recentring/rebuilding
			// underneath so surfacing again is instant, with no pop-in.
			PMC->SetVisibility(!bActive);
		}
	}

	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelUndergroundVeil: %s (clipmap %s)"),
	       bActive ? TEXT("ON -- camera has rock overhead") : TEXT("off -- open sky overhead"),
	       bActive ? TEXT("hidden") : TEXT("visible"));
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

	// Track B2: fetched once per rebuild (not per vertex -- RebuildLevel runs
	// at most once/level/tick, see the class comment's "Recenter" section) so
	// every one of this rebuild's ~4225 height taps reads the SAME tile
	// source the ring cascade is using right now. See SampleHeightUU's doc
	// comment for the nullptr fallback.
	const UWorld* World = GetWorld();
	const UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;

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
			const double HeightUU = SampleHeightUU(WorldX, WorldY, Subsystem, TerrainSeed);
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

	// -VoxelWindingCheck (same measurement UVoxelChunkComponent's and
	// UVoxelWaterChunkComponent's scene proxies log, extended to the clipmap
	// because its winding was picked BY HAND and never verified -- see
	// BuildSharedTopology's comment and Tools/create_clipmap_material.py's
	// "flip to one-sided once a screenshot confirms correct winding"
	// follow-up). Terrain and water both read -1.000 when correct.
	// Topology is shared across all 4 levels, so any one level's number is
	// the answer for all of them; logged per level anyway since the NORMALS
	// differ per level and both operands matter.
	{
		static const bool bWindingCheck = FParse::Param(FCommandLine::Get(), TEXT("VoxelWindingCheck"));
		if (bWindingCheck && SharedTriangles.Num() >= 3)
		{
			double DotSum = 0.0;
			int32 TriCount = 0;
			for (int32 I = 0; I + 2 < SharedTriangles.Num(); I += 3)
			{
				const FVector& P0 = Positions[SharedTriangles[I + 0]];
				const FVector& P1 = Positions[SharedTriangles[I + 1]];
				const FVector& P2 = Positions[SharedTriangles[I + 2]];
				const FVector Geo = FVector::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();
				DotSum += FVector::DotProduct(Geo, Normals[SharedTriangles[I]]);
				++TriCount;
			}
			UE_LOG(LogTemp, Log,
			       TEXT("VoxelWindingCheck CLIPMAP: level=%d tris=%d meanDot(geometricNormal, shadingNormal)=%+.3f"),
			       LevelIndex, TriCount, TriCount > 0 ? DotSum / double(TriCount) : 0.0);
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

	// Underground veil: evaluated before the recenter/rebuild work so a
	// surfacing camera un-hides the levels on the same tick they recentre.
	if (bVeilEnabled)
	{
		SetVeilActive(IsCameraUnderRock(CameraLocUU));
		if (bVeilActive && VeilShell)
		{
			// Follow the camera. Component-relative because ClipmapRoot is at
			// the actor origin, same convention RebuildLevel uses.
			VeilShell->SetRelativeLocation(CameraLocUU - GetActorLocation());
		}
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
