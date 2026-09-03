#include "VoxelClipmapActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/PointLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelEofDirtyLedger.h" // EndOfFrameUpdates attribution
#include "VoxelFrontEndPolicy.h" // IsWorldHeldForMenu -- backlog 0.0k
#include "VoxelWorldSubsystem.h"
// Biome appearance: the SAME climate->vertex-colour encoding
// UVoxelChunkComponent uses, so the vista and the near field cannot diverge.
#include "VoxelClimateProbe.h"

// vxc::classifyBiome / vxc::biomeSurfaceMaterial -- the far field's surface
// material now comes from the SAME classifier the voxels use, not a constant.
#include "voxelcore/biome.h"
// vxc::kMaterialPalette -- the ONE colour table, shared with the marcher via
// VoxelMaterialPalette.ush, which is generated from this same header.
#include "voxelcore/materialpalette.h"

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
// voxel-core/shaders/worldgen.ush MAT_SNOW threshold) rather than a hard
// cutoff, so the clipmap's coarse (64-512m/vertex) grid doesn't show a
// jagged single-vertex-row edge at the line.
// MOVED TO THE MATERIAL. The snow band is now a SnowlineLowMeters/
// SnowlineHighMeters ScalarParameter pair on M_VoxelClipmap and
// M_VoxelTerrain, evaluated per pixel from world Z (and biased by temperature)
// rather than per clipmap vertex. Two reasons: the voxel near field needs the
// same snowline and had no way to get it from a clipmap-only constant, and a
// per-pixel band no longer shows the jagged single-vertex-row edge this
// 200 m ramp existed to hide -- create_clipmap_material.py owns the values now.
// (2700/2900 straddled the amplifier's 2800 m MAT_SNOW threshold; the material
// parameters keep those same numbers.)

// ---- UNDERGROUND VEIL constants (see VoxelClipmapActor.h for the diagnosis)
//
// Half-extent of the inward-facing occluder box.
//
// The first version used 2 km (outside the ring cascade's 1 km R4 outer, for
// maximum clearance). That FAILED, and instructively: SkyAtmosphere's aerial
// perspective is integrated over the distance to the surface, and 2 km of it
// repainted the near-black box back to pale sky blue -- the shot came back
// looking like an overcast sky rather than rock. Distance is the enemy here,
// not the friend.
//
// 300 m is the right number: aerial perspective over 300 m is negligible, and
// nothing underground is ever resident beyond it. Underground residency is a
// +-25.6 m deep box plus a 38.4 m skirt (VoxelWorldSubsystem's
// BoxRadiusChunksL0/SkirtChunksNear), and the largest worldgen feature down
// here is a cavern at 24-56 m across -- all an order of magnitude inside 300
// m, so the veil can never occlude a real cave surface.
// (the runtime value lives in AVoxelClipmapActor::VeilHalfExtentUU, which this
// initialises; -VoxelVeilExtentM overrides it for diagnosis only)
constexpr double kVeilHalfExtentUUDefault = 30000.0; // 300 m

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

	// LOG THE SUCCESS TOO, not only the failure. "Does StaticLoadObject actually
	// return the material at runtime" was the first question asked of the
	// white-clipmap defect, and the honest answer from a log that only speaks up
	// on failure is "probably, but nothing in the run says so". A null here gives
	// every level UE's stand-in material, which is a flat grey -- so it is worth
	// ruling in or out in one line rather than inferring it from the picture.
	//
	// (For the record, on the reported defect it is NOT this: the magenta is an
	// authored WaterMarkerColor parameter and the white is the snow term, both of
	// which require the real material to be loaded and shading. See
	// VoxelClimateProbe.h's kSentinelSafeFloorU8 and this class's snow-override
	// block. Nothing about that makes the line below less worth having.)
	if (ClipmapMaterial)
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelClipmap material: loaded '%s' for %d levels."),
		       *ClipmapMaterial->GetName(), NumLevels);
	}
	else
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelClipmap material: /Game/Voxel/M_VoxelClipmap did NOT load -- every clipmap level is drawing ")
		       TEXT("with the engine default material (flat grey, no biome colour, no snow, no vertex colours read at all)."));
	}

	// Snow override + colour census. Both read here, once, and NOT as cvars, for
	// the reason bVeilEnabled's declaration gives (and the veil block a few
	// dozen lines below repeats): -ExecCmds lands after systems initialise, so a
	// cvar can silently measure the same state twice.
	bSnowOverrideActive |= FParse::Value(FCommandLine::Get(), TEXT("VoxelClipmapSnowlineLowM="), SnowlineLowMetersOverride);
	bSnowOverrideActive |= FParse::Value(FCommandLine::Get(), TEXT("VoxelClipmapSnowlineHighM="), SnowlineHighMetersOverride);
	bSnowOverrideActive |= FParse::Value(FCommandLine::Get(), TEXT("VoxelClipmapSnowTempMax="), SnowTempMaxOverride);
	bSnowOverrideActive |= FParse::Value(FCommandLine::Get(), TEXT("VoxelClipmapSnowTempFeather="), SnowTempFeatherOverride);
	if (bSnowOverrideActive)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelClipmap snow OVERRIDDEN: snowline %.0f-%.0f m, SnowTempMax %.3f feather %.3f ")
		       TEXT("(shipped material defaults are 2700-2900 m, 0.160, 0.100). This changes what is drawn."),
		       SnowlineLowMetersOverride, SnowlineHighMetersOverride, SnowTempMaxOverride, SnowTempFeatherOverride);
	}

	// --- ONE COLOUR AUTHORITY -----------------------------------------------
	//
	// -VoxelClipmapVertexAlbedo=1 makes the far field take its colour from the
	// SAME place the voxels do: vxc::classifyBiome -> vxc::biomeSurfaceMaterial
	// -> vxc::kMaterialPalette, computed per vertex here and sent as a COLOUR in
	// VertexColor.RGB. The material's VertexAlbedoWeight then bypasses the biome
	// LUT, the snow term and the slope-rock term -- all three are the clipmap's
	// own inventions, and the near field has none of them.
	//
	// ONE SWITCH ARMS BOTH HALVES on purpose: the vertex ENCODING and the
	// material's use of it must move together, and an arm where one moved and
	// the other did not is a picture nobody can interpret.
	//
	// -VoxelClipmapAlbedoSrgb=0 writes the palette bytes LINEARISED instead of
	// as authored. kMaterialPalette is authored in sRGB; whether UE's vertex
	// colour path wants sRGB or linear is NOT something to assume here, given
	// that this exact path is on record turning a written 4 into a read 6-8.
	// One capture pair settles it, and the wrong choice is a uniform brightness
	// shift rather than a structural error.
	FParse::Value(FCommandLine::Get(), TEXT("VoxelClipmapVertexAlbedo="), VertexAlbedoWeightOverride);
	bVertexAlbedoActive = VertexAlbedoWeightOverride > 0.0f;
	// -VoxelClipmapVoxelize=0 restores the smooth far field (control arm for
	// the virtual-ring voxelization seam A/B); default 1.0 = voxelized, the
	// owner-accepted direction. See ApplyLevelMaterial's push.
	FParse::Value(FCommandLine::Get(), TEXT("VoxelClipmapVoxelize="), VoxelizeWeightOverride);
	{
		int32 SrgbFlag = 1;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelClipmapAlbedoSrgb="), SrgbFlag))
		{
			bAlbedoAsSrgb = (SrgbFlag != 0);
		}
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Clipmap colour authority: %s (-VoxelClipmapVertexAlbedo=%.2f, bytes %s). ON = the ")
	       TEXT("palette colour of each vertex's REAL surface material, the same authority the voxel ")
	       TEXT("terrain uses. OFF = the biome LUT plus the clipmap's own snow and slope-rock terms."),
	       bVertexAlbedoActive ? TEXT("PALETTE (one authority, default)") : TEXT("biome LUT (control arm)"),
	       VertexAlbedoWeightOverride, bAlbedoAsSrgb ? TEXT("as authored sRGB") : TEXT("linearised"));

	bColorCensus = FParse::Param(FCommandLine::Get(), TEXT("VoxelClipmapColorCensus"));

	LevelMIDs.SetNum(NumLevels);

	// Load the climate tiles here, on the game thread, before the first clipmap
	// rebuild and before any meshing worker can need them. The probe is
	// self-initializing and thread-safe, so this is not required for
	// correctness -- it is here so the 25-tile disk read happens once at a
	// predictable point instead of inside whichever background chunk job
	// happens to touch climate first, and so the "tiles=N" log line lands early
	// enough to diagnose a bad -VoxelTileDir before the screenshot.
	VoxelClimate::EnsureInitialized();

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

	// Diagnostic: shrink the veil box so it can be photographed. See the
	// VeilHalfExtentUU declaration in the header.
	VeilHalfExtentUU = kVeilHalfExtentUUDefault;
	double VeilExtentM = 0.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelVeilExtentM="), VeilExtentM) && VeilExtentM > 0.1)
	{
		VeilHalfExtentUU = VeilExtentM * 100.0;
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelUndergroundVeil: DIAGNOSTIC half-extent %.1f m (shipped default is 300 m)"),
		       VeilExtentM);
	}

	// ---- Underground presentation rig ------------------------------------
	int32 CaveLightFlag = 1;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelCaveLight="), CaveLightFlag))
	{
		bCaveRigEnabled = (CaveLightFlag != 0);
	}
	FParse::Value(FCommandLine::Get(), TEXT("VoxelCaveEV="), CaveExposureEV100);
	FParse::Value(FCommandLine::Get(), TEXT("VoxelCaveLampLumens="), CaveLampLumens);
	float LampRadiusM = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelCaveLampRadiusM="), LampRadiusM) && LampRadiusM > 0.f)
	{
		CaveLampRadiusUU = LampRadiusM * 100.f;
	}
	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCaveLight: %s (EV100 %.2f, lamp %.0f lm / %.0f m)"),
	       bCaveRigEnabled ? TEXT("enabled") : TEXT("DISABLED"), CaveExposureEV100, CaveLampLumens,
	       CaveLampRadiusUU / 100.f);

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
	VoxelEofLedger::Count(VoxelEofLedger::ESource::Clipmap);
	VoxelEofLedger::CountRegister();
	VeilShell->SetMobility(EComponentMobility::Movable);
	VeilShell->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VeilShell->bUseAsyncCooking = false;
	// The box is recentred on the camera every tick, so a bounds-based cull
	// would be computing a cull for a primitive that always contains the
	// view point. Skip it rather than let a stale bound flicker the veil off.
	VeilShell->bUseAttachParentBound = false;
	VeilShell->SetCastShadow(false); // pure occluder; casting from it would black out the cave

	const double H = VeilHalfExtentUU;
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
	VoxelEofLedger::Count(VoxelEofLedger::ESource::Clipmap);

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

// ---- Underground presentation rig -----------------------------------------
//
// THE MEASUREMENT THIS IMPLEMENTS (three same-binary captures of the
// -VoxelCaveTest cave, 14.5 m down, identical camera):
//
//   A  shipped                          -- lit faces clipped to pure white,
//                                          shadowed faces pale blue-grey.
//   B  -ExecCmds="r.EyeAdaptationQuality 0"
//                                       -- no clipping at all, warm rock,
//                                          black shadows. Same geometry, same
//                                          lights, same frame.
//   C  -ExecCmds="r.SkyLightingQuality 0"
//                                       -- shadowed faces go to pure black,
//                                          confirming the pale-grey fill in A
//                                          is the real-time-capture SkyLight
//                                          and NOT the material's base colour.
//
// A vs B is the whole story: nothing underground is over-lit in absolute
// terms (the sun is 8 lux and the ambient is a fraction of that). What is
// wrong is the EXPOSURE. UE's histogram eye adaptation defaults to a
// -10..+20 EV100 clamp, i.e. effectively unbounded, and a lightless cave has
// no 18%-grey subject anywhere in frame -- so adaptation walks the whole
// image up until something is grey, which means the lit faces clip.
//
// That same mechanism, and not SkyAtmosphere and not the tint, is what makes
// the veil render as a "flat pale plane": the veil's base colour is 0.020,
// and 0.020 lifted until the frame averages 18% grey is a mid-grey wall. The
// darker the thing you draw, the harder auto-exposure fights you -- which is
// why every previous attempt to fix the veil by making it DARKER could not
// have worked.
//
// So the fix is one thing, not three: pin the exposure. min == max collapses
// the histogram's search to a constant, which is manual exposure expressed in
// the parameters the rest of the engine already understands.
//
// EV100 0 is the measured value: it reproduces capture B, which is the frame
// that looked right. The lamp is then sized to that fixed stop rather than
// the other way round.
void AVoxelClipmapActor::EnsureCaveRig()
{
	if (CaveExposurePP)
	{
		return;
	}

	CaveExposurePP = NewObject<UPostProcessComponent>(this, TEXT("CaveExposurePP"));
	CaveExposurePP->SetupAttachment(ClipmapRoot);
	CaveExposurePP->RegisterComponent();
	VoxelEofLedger::Count(VoxelEofLedger::ESource::Clipmap);
	VoxelEofLedger::CountRegister();
	// Unbound: the volume has no shape, it applies everywhere. Correct here
	// because the gate is "is the camera under rock", which is a property of
	// the camera and not of a region a designer could author.
	CaveExposurePP->bUnbound = true;

	// =======================================================================
	// EXPOSURE OWNERSHIP RULE -- READ THIS BEFORE ADDING A THIRD VOLUME.
	// (The same rule is written into VoxelSkySubsystem.cpp beside the other
	// volume. If you change one copy, change both; they exist in duplicate
	// precisely because whoever next touches either file will only read that
	// one. The comment above this one used to say "the project ships none, so
	// this is future-proofing rather than a fight anyone is having today" --
	// that stopped being true when W5 landed, and this block replaces it.)
	//
	// Two UNBOUND post-process volumes that both override auto-exposure do not
	// blend. For any setting BOTH override, the higher Priority wins outright
	// and the loser's value vanishes -- no warning, no log line, no visual clue
	// except that one of them stopped working. There are exactly two:
	//
	//   AVoxelClipmapActor::CaveExposurePP   Priority 100   conditional
	//   UVoxelSkySubsystem::SkyExposurePP    Priority  10   always-on base
	//
	// THE RULE: the SKY volume is the BASE LAYER (it holds the day/night EV
	// curve for the whole world, which is what makes a night frame render dark
	// instead of being lifted to 18% grey by histogram adaptation -- the same
	// mechanism this rig was built to defeat underground). THIS volume is a
	// strictly MORE SPECIFIC override and therefore wins where it applies:
	// under rock the sun's altitude is not an input to anything, because no
	// daylight reaches the camera, so the sky curve has nothing to say and the
	// +10 EV100 below is the only exposure number in this project backed by an
	// actual A/B against a reference frame.
	//
	// WHY PRIORITY ORDERING AND NOT "THE SKY SUBSYSTEM OWNS BOTH". That was the
	// alternative and it was rejected for two concrete reasons. (1) LIFETIME:
	// this actor is suppressible (-VoxelNoClipmap) and its rig is built lazily
	// on the first underground transition, so the sky must not depend on it and
	// it must not depend on a subsystem that may be disabled. (2) PROVENANCE:
	// folding +10 into the sky's curve would re-derive by taste a number
	// arrived at by measurement (see CaveExposureEV100's declaration).
	//
	// THE INVARIANT A THIRD VOLUME MUST HOLD: override the SAME exposure fields
	// these two do (AutoExposureMethod AND AutoExposureBias), or it will win the
	// fields it overrides and silently inherit the rest from a lower-priority
	// volume, producing a hybrid exposure matching neither. And declare its
	// priority band in both of these comments.
	//
	// (The sky volume's clamped-auto mode additionally overrides the min/max
	// brightness clamps, which this one does not. That is safe and not an
	// exception: this volume wins AutoExposureMethod with AEM_Manual, and manual
	// exposure ignores those clamps entirely.)
	//
	// WHAT THE EXPOSURE RETUNES DID TO THE STEP BETWEEN THE TWO VOLUMES, and this
	// volume is on the other side of that step. Nothing about the ownership rule
	// changed, but the SIZE of the jump at a cave mouth did, three times now. The
	// sky's curve used to run +7.0 (day) to +12.0 (night); the W6 retune made it
	// +8.7 to +15.6; the deep-night ramp (DeepNightDropForSunAltitude) then gave
	// 2.0 stops back below -18 deg; and the measured retune (see
	// ExposureBiasForSunAltitude, whose anchors are now fitted to two full capture
	// ladders rather than to an illuminance table) then moved the cap's ONSET from
	// -2 deg to -6 deg, which is what changed the sunset case below out of all
	// recognition. CaveExposureEV100 below was deliberately NOT moved with any of
	// it -- +10 is the only exposure number in this project backed by an A/B
	// against a reference frame, and re-deriving it by taste to tidy up a step
	// would spend that measurement for nothing. The four cases that now exist:
	//
	//   into a cave at NOON               +8.8  -> +10.0   1.2 stops BRIGHTER
	//   into a cave at SUNSET  (0 deg)    +10.9 -> +10.0   0.9 stops DARKER
	//   into a cave in TWILIGHT (<= -6)   +15.6 -> +10.0   5.6 stops DARKER
	//   into a cave in DEEP NIGHT (<=-18) +13.6 -> +10.0   3.6 stops DARKER
	//
	// Twilight is the worst case and it is unchanged. Deep night, the one a player
	// actually meets most often, improved by the full 2.0 stops. And SUNSET --
	// which used to be a 4.1-stop DROP, because the old sky table had already
	// reached the +15.6 moon cap by 0 deg -- is now very nearly seamless. That is
	// a side effect of fixing the sky's DAY curve, not something anyone tuned for,
	// and it is the one case worth re-checking by eye rather than by arithmetic.
	//
	// So a cave is still markedly darker than the moonlit surface outside it,
	// where it used to be marginally brighter. If that reads wrong, the fix is
	// HERE -- re-measure CaveExposureEV100 against a night reference frame the way
	// it was originally measured against a day one, and note both numbers -- not
	// in flattening the sky's night end, which is holding a full moon at a legible
	// brightness and would take the whole night down with it.
	//
	// BOTH COPIES OF THIS COMMENT NOW AGREE. The sky-side copy used to carry a
	// note that this one was deliberately stale at the two-case W6 version, to be
	// pasted over the next time this file was open. It has been, and that pointer
	// is gone. The rule stands: if you change one, change both.
	// =======================================================================
	CaveExposurePP->Priority = 100.f;

	// EXACTLY TWO overrides. Everything else -- bloom, colour grading, DOF,
	// vignette -- is deliberately left to the project defaults, so that when
	// the rig switches off, the frame is byte-identical to a pre-change frame
	// rather than "close enough".
	//
	// AEM_Manual, NOT "histogram with min == max". The first version of this
	// did use the min/max clamp, which reads like the more surgical change --
	// and it produced a frame that was 100.0% pure white, every pixel clipped.
	// The clamp fields are interpreted through r.EyeAdaptation.ExposureFormat,
	// so "0" is not unambiguously "EV100 0", and the wrong reading of it is an
	// exposure divide that runs away. AEM_Manual has no such ambiguity: the
	// stop comes from the physical camera fields below, and AutoExposureBias
	// offsets it in whole stops.
	FPostProcessSettings& S = CaveExposurePP->Settings;
	S.bOverride_AutoExposureMethod = true;
	S.AutoExposureMethod = AEM_Manual;
	S.bOverride_AutoExposureBias = true;
	S.AutoExposureBias = CaveExposureEV100; // stops, -VoxelCaveEV=

	CaveLamp = NewObject<UPointLightComponent>(this, TEXT("CaveLamp"));
	CaveLamp->SetupAttachment(ClipmapRoot);
	CaveLamp->RegisterComponent();
	VoxelEofLedger::Count(VoxelEofLedger::ESource::Clipmap);
	VoxelEofLedger::CountRegister();
	CaveLamp->SetMobility(EComponentMobility::Movable);
	CaveLamp->SetIntensityUnits(ELightUnits::Lumens);
	CaveLamp->SetIntensity(CaveLampLumens);
	CaveLamp->SetVisibility(false);
	// Lamp off entirely at 0 lumens, so the exposure lock can be A/B'd on its
	// own without a second switch.
	if (CaveLampLumens <= 0.f)
	{
		CaveLamp->SetIntensity(0.f);
	}
	CaveLamp->SetAttenuationRadius(CaveLampRadiusUU);
	// A finite source radius, not a mathematical point. -VoxelCaveTest can
	// park the camera 0.4 m off a wall (it stands back against the wall
	// OPPOSITE the longest open run, which in a narrow pocket is very close
	// indeed), and a point light's 1/d^2 at 0.4 m blew 9.2% of that frame to
	// pure white. Treating the lamp as a 40 cm emitter bounds the near-field
	// term instead of letting it run to infinity.
	CaveLamp->SetSourceRadius(40.f);
	// Warm, like every lamp a person actually carries underground, and it
	// separates the lit near field from the neutral-grey ambient behind it.
	CaveLamp->SetLightColor(FLinearColor(1.0f, 0.86f, 0.68f));
	// NO shadow casting. Two reasons, both load-bearing: a point light at the
	// camera casts a shadow of nothing the camera can see (every shadow it
	// casts is hidden behind the caster), so the shadow pass is pure cost;
	// and a 6-face cube shadow map at 60 fps is exactly the sort of thing
	// that would show up in the M1 budget if the rig ever leaked above ground.
	CaveLamp->SetCastShadows(false);
	CaveLamp->SetVisibility(false);
}

void AVoxelClipmapActor::SetCaveRigActive(bool bActive)
{
	if (bActive)
	{
		EnsureCaveRig();
	}
	if (CaveExposurePP)
	{
		CaveExposurePP->bEnabled = bActive;
	}
	if (CaveLamp)
	{
		CaveLamp->SetVisibility(bActive);
	}
	bCaveRigActive = bActive;
}

bool AVoxelClipmapActor::IsCameraUnderRock(const FVector& CameraLocUU) const
{
	const UWorld* World = GetWorld();
	const UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return false; // fail OPEN: never veil the vista on a missing subsystem
	}

	// Same fail-open, for the same reason, while the menu holds the world: the
	// question cannot be answered yet. The probe below walks IsSolidAtVoxel up
	// a column, which is a worldgen query, and during the menu nothing has been
	// prefetched anywhere -- so on a world whose column is not baked it is
	// fatal in an unattended run. There is also nothing to veil: the menu is
	// full-screen. Fourth and last offender in backlog 0.0k.
	if (VoxelFrontEnd::IsWorldHeldForMenu(World))
	{
		return false;
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
	// Same latch, same transition: the rig's "am I underground" predicate IS
	// the veil's, by construction, so the two can never disagree.
	if (bCaveRigEnabled)
	{
		SetCaveRigActive(bActive);
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
	//
	// Keyed off the OUTERMOST ACTIVE ring, not off kNumLevels-1: -VoxelMaxRingLevel
	// can retire the outer rings at runtime, and if the hole stayed pinned to the
	// compiled-in last preset it would sit outside where the voxels actually stop
	// and open an annulus of missing world between the two systems. Reading the
	// active edge keeps the two coverages adjacent at any cascade radius, which is
	// also what makes a radius A/B a fair comparison rather than one side
	// photographing a hole.
	static const double RingEdgeUU =
		UVoxelWorldSubsystem::GetRingPresets()[UVoxelWorldSubsystem::GetMaxRingLevel()].OuterMeters * 100.0;
	static const double Spacing0UU = RingEdgeUU / double(HoleHalfIndex);
	return Spacing0UU * double(int64(1) << LevelIndex);
}

double AVoxelClipmapActor::OuterHalfExtentUU()
{
	// The outermost level's grid runs +-HalfIndex vertices from the shared
	// origin at that level's spacing (RebuildLevel pass 1 lays out LocalX as
	// (i - HalfIndex) * Spacing). Derived rather than written down so it cannot
	// drift from the geometry: at the shipped defaults this is 32 * 8 *
	// (409600/16) = 6,553,600 UU, and it moves whenever the ring cascade's outer
	// radius does.
	//
	// The outer SKIRT hangs downward from this perimeter and adds no horizontal
	// extent, so this really is the farthest this actor draws along an axis.
	return double(HalfIndex) * SpacingUUForLevel(NumLevels - 1);
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
	// inside the hole, because something finer covers that area instead.
	//
	// TWO MASKS, NOT ONE, AND THE DIFFERENCE IS A HOLE THE OWNER FOUND FROM
	// ALTITUDE. What each level's hole has to abut is not the same shape:
	//
	//   * levels 1..N-1 abut the NEXT FINER CLIPMAP LEVEL, whose extent is a
	//     SQUARE (its grid runs +-HalfIndex vertices). A square hole is exactly
	//     right there and "lands exactly on level L-1's outer edge, by
	//     construction" as the spacing comment says.
	//   * level 0 abuts THE VOXEL RING CASCADE, which is admitted by RADIAL
	//     distance and therefore ends in a DISC of radius RingEdgeUU.
	//
	// A square hole of half-width R against a disc of radius R leaves the four
	// corners out to R*sqrt(2) covered by NEITHER system. Looking straight down
	// from altitude that reads as a circle of voxels failing to fill a square
	// of clipmap, with bare gaps at the diagonals -- which is exactly how the
	// owner reported it, and how he diagnosed it.
	//
	// So level 0 gets a RADIAL hole and the rest keep the square one. The
	// topology can no longer be shared across all four levels; that is the cost
	// and it is one extra index array.
	//
	// CONSERVATIVE ON PURPOSE: a quad is dropped only when its FARTHEST corner
	// is inside the disc, so the clipmap keeps every quad the voxels might not
	// reach and overlaps slightly UNDER the voxel disc at the seam. Drawing
	// must over-cover; that rule is why the lake sink once ate rivers over a
	// bounding box. Overlapping cheap heightfield triangles under opaque voxels
	// costs nothing visible; the reverse leaves a hole through the world.
	auto BuildTopology = [this](TArray<int32>& Out, bool bRadialHole)
	{
		Out.Reset();
		Out.Reserve((NumVertsPerSide - 1) * (NumVertsPerSide - 1) * 6);
		for (int32 i = 0; i < NumVertsPerSide - 1; ++i)
		{
			for (int32 j = 0; j < NumVertsPerSide - 1; ++j)
			{
				bool bHoleQuad;
				if (bRadialHole)
				{
					// Farthest corner of quad (i,j) from the grid centre, in
					// index units -- spacing is uniform per level, so index
					// radius and world radius agree up to that one scale.
					const double DX = FMath::Max(FMath::Abs(double(i) - double(HalfIndex)),
					                             FMath::Abs(double(i + 1) - double(HalfIndex)));
					const double DY = FMath::Max(FMath::Abs(double(j) - double(HalfIndex)),
					                             FMath::Abs(double(j + 1) - double(HalfIndex)));
					bHoleQuad = (DX * DX + DY * DY) <= double(HoleHalfIndex) * double(HoleHalfIndex);
				}
				else
				{
					bHoleQuad = (i >= HalfIndex - HoleHalfIndex) && (i < HalfIndex + HoleHalfIndex) &&
					            (j >= HalfIndex - HoleHalfIndex) && (j < HalfIndex + HoleHalfIndex);
				}
				if (bHoleQuad)
				{
					continue;
				}

				const int32 V00 = i * NumVertsPerSide + j;
				const int32 V10 = (i + 1) * NumVertsPerSide + j;
				const int32 V01 = i * NumVertsPerSide + (j + 1);
				const int32 V11 = (i + 1) * NumVertsPerSide + (j + 1);

				// Winding as below -- M_VoxelClipmap is two-sided defensively.
				Out.Add(V00);
				Out.Add(V01);
				Out.Add(V11);
				Out.Add(V00);
				Out.Add(V11);
				Out.Add(V10);
			}
		}
	};
	// -VoxelClipmapSquareHole restores the old square level-0 hole, so the seam
	// this fixes can be photographed on ONE binary instead of two builds. That
	// is the convention every change in this programme follows: the control arm
	// is a switch, not a rebuild, because then a difference in the capture pair
	// cannot be blamed on a different compile.
	const bool bSquareHoleControl = FParse::Param(FCommandLine::Get(), TEXT("VoxelClipmapSquareHole"));
	BuildTopology(SharedTriangles, /*bRadialHole*/ false);
	BuildTopology(SharedTrianglesLevel0, /*bRadialHole*/ !bSquareHoleControl);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Voxel clipmap topology: square hole %d tris (levels 1-%d), radial hole %d tris (level 0, ")
	       TEXT("abuts the voxel DISC). Radial keeps %d more quads -- those are the corner wedges that ")
	       TEXT("were bare before."),
	       SharedTriangles.Num() / 3, NumLevels - 1, SharedTrianglesLevel0.Num() / 3,
	       (SharedTrianglesLevel0.Num() - SharedTriangles.Num()) / 6);

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


	// Per-level histogram of the surface material this pass computes. See the
	// print in the census block for why it exists.
	uint32 MatHistogram[47] = {};
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

			// Vertex colour semantics are now IDENTICAL to
			// UVoxelChunkComponent's: R = material id, G = shade, B =
			// temperature, A = precipitation. They used to be R = slope,
			// G = snow -- a completely DIFFERENT convention from the voxel
			// chunks, which is a large part of why the 50 km vista rendered
			// pale green while the near field rendered beige. Sharing one
			// encoding (and therefore T_VoxelPalette + T_VoxelBiomeLUT) makes
			// the two agree at the seam by construction, instead of relying on
			// two independently hand-tuned colour schemes happening to match.
			//
			// Slope and snow are NOT lost -- they moved into the material,
			// where both graphs derive slope from the interpolated normal and
			// snow from temperature plus world Z. This pass already computes
			// real normals (Normals[Idx] above) and hands them to the PMC, so
			// the material recovers exactly the quantity this code used to
			// precompute, without spending a vertex byte on it.
			const double HeightMeters = HeightsUU[Idx] / 100.0;

			// THE REAL SURFACE MATERIAL, from voxel-core's own classifier.
			//
			// THIS USED TO BE A CONSTANT, and the constant was wrong. It read
			// `(HeightMeters < 0.0) ? MAT_MUD : MAT_SAND`, justified as "4 =
			// MAT_SAND is the id voxel-core actually emits for every land
			// surface voxel on these tiles". **It does not.**
			// vxc::biomeSurfaceMaterial (biome.h:297-320) emits GRASS, TOPSOIL,
			// PODZOL, JUNGLE_SOIL, SAVANNA_GRASS, PERMAFROST or ROCK by biome,
			// and MAT_SAND only for BEACH. So the far field was told every
			// square metre of land was the same material, and could not paint
			// the two that make alpine ground white and brown -- MAT_PERMAFROST
			// below the alpine rock line and MAT_ROCK above it. That is the
			// owner's "the clipmap is smooth green where the voxels are white
			// and brown", and it was never a tuning error: the information
			// never arrived.
			//
			// CALL THE CLASSIFIER, DO NOT RE-DERIVE IT. classifyBiome runs
			// morphology gates (sea level, beach band, treeline) BEFORE the
			// Whittaker table, and the treeline is temperature-dependent -- a
			// hand-rolled "cold and high means alpine" here would disagree with
			// the voxels at exactly the boundary this change exists to close.
			//
			// RAW bytes, not FVoxelClimateBytes: the thresholds are in wire
			// units. See SampleRawClimateAtWorldUU's declaration for why that
			// distinction is not cosmetic, and note the third argument is
			// precipitation VARIABILITY (biome.h:260-265 warns that a caller
			// which passes the old seasonality channel still compiles).
			const VoxelClimate::FVoxelRawClimate Raw = VoxelClimate::SampleRawClimateAtWorldUU(
				SnappedOriginUU.X + double(i - HalfIndex) * Spacing,
				SnappedOriginUU.Y + double(j - HalfIndex) * Spacing);

			// Slope in mm per metre from the heights this pass already has.
			// Central difference where both neighbours exist, one-sided at the
			// grid edge. Spacing is in UU (10 mm), so the run in metres is
			// Spacing/100 and the rise in mm is dHeightUU * 10.
			const double RunM = FMath::Max(Spacing / 100.0, 1.0);
			const int32 IPrev = FMath::Max(i - 1, 0), INext = FMath::Min(i + 1, NumVertsPerSide - 1);
			const int32 JPrev = FMath::Max(j - 1, 0), JNext = FMath::Min(j + 1, NumVertsPerSide - 1);
			const double DzX = (HeightsUU[j * NumVertsPerSide + INext]
			                  - HeightsUU[j * NumVertsPerSide + IPrev]) * 10.0
			                 / (RunM * FMath::Max(INext - IPrev, 1));
			const double DzY = (HeightsUU[JNext * NumVertsPerSide + i]
			                  - HeightsUU[JPrev * NumVertsPerSide + i]) * 10.0
			                 / (RunM * FMath::Max(JNext - JPrev, 1));
			const int64 SlopeMmPerM = (int64)FMath::Sqrt(DzX * DzX + DzY * DzY);

			const int32 SurfaceMm = (int32)FMath::Clamp(HeightMeters * 1000.0, -2.0e9, 2.0e9);
			// No sampler = no classification. Falling back to the old constant
			// is deliberate: it is the behaviour every capture in the archive
			// was taken under, so a run with no tiles looks like it always did
			// rather than like a new bug.
			const uint8 MatId = Raw.bValid
				? (uint8)vxc::biomeSurfaceMaterial(
					  vxc::classifyBiome(Raw.Temperature, Raw.Precipitation, Raw.PrecipVariability,
					                     SurfaceMm, SlopeMmPerM),
					  SurfaceMm)
				: (uint8)((HeightMeters < 0.0) ? 13 /*MAT_MUD*/ : 4 /*MAT_SAND*/);

			// Recomputed rather than carried over from pass 1 (which is where
			// WorldX/WorldY live): this is 2 multiply-adds against 65x65
			// vertices per level, four levels, only on a rebuild.
			const FVoxelClimateBytes Climate = VoxelClimate::SampleClimateAtWorldUU(
				SnappedOriginUU.X + double(i - HalfIndex) * Spacing,
				SnappedOriginUU.Y + double(j - HalfIndex) * Spacing);

			// THE MATERIAL HISTOGRAM, and it is the proof this change engaged.
			// The old constant made this a single bar at MAT_SAND; a run whose
			// far field is still all one material has NOT picked up the
			// classifier, whatever the picture looks like. Free (one increment
			// per vertex) and printed only under -VoxelClipmapColorCensus.
			if (MatId < UE_ARRAY_COUNT(MatHistogram))
			{
				++MatHistogram[MatId];
			}

			// G = 255: a heightfield clipmap has no per-vertex AO to carry, and
			// 255 is the identity for the material's AO multiply.
			if (bVertexAlbedoActive)
			{
				// THE PALETTE COLOUR OF THE REAL SURFACE MATERIAL -- the same
				// table VoxelMaterialPalette.ush gives the marcher, so both
				// halves of the terrain now read one authority.
				//
				// kFaceTop, because every clipmap vertex is by construction a
				// sky-facing heightfield sample. There are no side or bottom
				// faces out here to choose between.
				const vxc::Rgb Pal = vxc::kMaterialPalette[FMath::Min<int32>(MatId, vxc::kMaterialCount - 1)]
				                         .face[vxc::kFaceTop];
				VertexColors[Idx] = bAlbedoAsSrgb
					? FColor(Pal.r, Pal.g, Pal.b, 255)
					: FLinearColor(FColor(Pal.r, Pal.g, Pal.b, 255)).ToFColor(/*bSRGB*/ false);
			}
			else
			{
				VertexColors[Idx] = FColor(VoxelClimate::BiomeTintForFace(MatId, 2, HeightMeters >= 0.0), 255,
				                          Climate.Temperature, Climate.Precipitation);
			}
		}
	}

	// ---- COLOUR CENSUS (-VoxelClipmapColorCensus) --------------------------
	//
	// What the vertex colours just written will DO once M_VoxelClipmap shades
	// them. Every threshold below is transcribed from
	// Tools/terrain_material_common.py's build_terrain_base_color and is printed
	// with the line, so a reading taken after that graph moves is obviously
	// suspect instead of quietly wrong.
	//
	// This is a per-VERTEX census, not a per-pixel or per-area one. A clipmap
	// vertex covers 256 m at level 0 and 2048 m at level 3, so a level's numbers
	// are area-weighted by construction WITHIN that level, but the four levels
	// cover wildly different areas and must not be summed. Read one line at a
	// time -- and note that level 3 is the band the horizon is made of.
	if (bColorCensus)
	{
		// terrain_material_common.py, verbatim:
		//   is_marker = (1-ramp(B,0.02,0.06)) * (1-ramp(A,0.02,0.06)) * ramp(R,0.94,0.98)
		//   snow_from_temp = 1 - ramp(B, SnowTempMax, SnowTempMax+SnowTempFeather)
		//   snow_from_z    =     ramp(worldZ_m, SnowlineLowMeters, SnowlineHighMeters)
		constexpr float kMarkerLo = 0.02f, kMarkerHi = 0.06f;
		constexpr float kMarkerRLo = 0.94f, kMarkerRHi = 0.98f;
		const float SnowTempMax = bSnowOverrideActive ? SnowTempMaxOverride : 0.16f;
		const float SnowTempFeather = bSnowOverrideActive ? SnowTempFeatherOverride : 0.10f;
		const float SnowLowM = bSnowOverrideActive ? SnowlineLowMetersOverride : 2700.f;
		const float SnowHighM = bSnowOverrideActive ? SnowlineHighMetersOverride : 2900.f;

		auto Ramp = [](float V, float Lo, float Hi) { return FMath::Clamp((V - Lo) / FMath::Max(Hi - Lo, 1e-6f), 0.f, 1.f); };

		int32 Marker = 0, SnowByTemp = 0, SnowByZ = 0, PrecipPinnedLo = 0, PrecipPinnedHi = 0, TempPinnedLo = 0, TempPinnedHi = 0;
		for (int32 Idx = 0; Idx < NumVertsTotal; ++Idx)
		{
			const FColor& C = VertexColors[Idx];
			const float R = float(C.R) / 255.f;
			const float B = float(C.B) / 255.f;
			const float A = float(C.A) / 255.f;
			const float ZM = float(HeightsUU[Idx] / 100.0);

			if ((1.f - Ramp(B, kMarkerLo, kMarkerHi)) * (1.f - Ramp(A, kMarkerLo, kMarkerHi)) * Ramp(R, kMarkerRLo, kMarkerRHi) >= 0.5f)
			{
				++Marker;
			}
			if (1.f - Ramp(B, SnowTempMax, SnowTempMax + SnowTempFeather) >= 0.9f) { ++SnowByTemp; }
			if (Ramp(ZM, SnowLowM, SnowHighM) >= 0.9f) { ++SnowByZ; }
			// "Pinned" = the remap clamped this vertex onto an END of the biome
			// LUT axis, where it carries no information at all. This is the
			// number that says whether the vista has biome colour to lose.
			// <= the floor, not == it: with the sentinel guard ON nothing is
			// below the floor so this is exactly the clamped set, and with it
			// OFF (-VoxelClimateSentinelGuard=0) it still counts the same
			// ground rather than reading 0% and looking like a fix.
			if (C.A <= VoxelClimate::kSentinelSafeFloorU8) { ++PrecipPinnedLo; }
			if (C.A == 255) { ++PrecipPinnedHi; }
			if (C.B <= VoxelClimate::kSentinelSafeFloorU8) { ++TempPinnedLo; }
			if (C.B == 255) { ++TempPinnedHi; }
		}

		const float Inv = 100.f / float(NumVertsTotal);
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelClipmapColorCensus L%d spacing=%.0f m verts=%d | MAGENTA(marker gate) %.1f%% | ")
		       TEXT("SNOW by temp %.1f%% by altitude %.1f%% | LUT precip axis pinned lo/hi %.1f%%/%.1f%% ")
		       TEXT("temp axis pinned lo/hi %.1f%%/%.1f%% | thresholds marker(%.2f,%.2f,R %.2f,%.2f) ")
		       TEXT("snowTemp(%.3f,+%.3f) snowline(%.0f,%.0f m)"),
		       LevelIndex, Spacing / 100.0, NumVertsTotal,
		       float(Marker) * Inv, float(SnowByTemp) * Inv, float(SnowByZ) * Inv,
		       float(PrecipPinnedLo) * Inv, float(PrecipPinnedHi) * Inv,
		       float(TempPinnedLo) * Inv, float(TempPinnedHi) * Inv,
		       kMarkerLo, kMarkerHi, kMarkerRLo, kMarkerRHi,
		       SnowTempMax, SnowTempFeather, SnowLowM, SnowHighM);

		// WHAT THE FAR FIELD IS ACTUALLY MADE OF, per vxc::biomeSurfaceMaterial.
		// Before 2026-08-30 this was a single bar at MAT_SAND (4) for all land
		// and MAT_MUD (13) below sea level, because the id was a constant. If it
		// still reads that way, the classifier did not run.
		FString Hist;
		for (int32 M = 0; M < (int32)UE_ARRAY_COUNT(MatHistogram); ++M)
		{
			if (MatHistogram[M] > 0)
			{
				Hist += FString::Printf(TEXT(" mat%d=%.1f%%"), M, float(MatHistogram[M]) * Inv);
			}
		}
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelClipmapColorCensus L%d SURFACE MATERIALS (vxc::biomeSurfaceMaterial):%s")
		       TEXT("  [2=ROCK 4=SAND 6=TOPSOIL 8=GRASS 9=JUNGLE_SOIL 10=SAVANNA 11=PODZOL "
		            "12=PERMAFROST 13=MUD]"),
		       LevelIndex, *Hist);
	}

	// Pass 3: seam treatment at the outer grid edge (position only, after
	// normals/colors are computed from the true surface).
	//
	// WHY THE OLD SKIRTS PRODUCED THE ARTIFACT. The v1 approach dropped BOTH
	// the outer grid edge AND the inner hole boundary by 2x spacing. With the
	// rings concentric (Tick's CommonOrigin fix), a finer level's outer edge
	// (radius 32*S) and the next-coarser level's inner hole edge (also radius
	// 32*S, since 16*2S == 32*S) land on exactly the SAME world square -- so
	// both of those dropped rings sit at the same radius and dive AWAY from
	// each other into an open V-trench (~3 cells wide, 2-4x spacing deep, with
	// a whole-cell vertical gap at the floor). Looking down at the vista that
	// trench is the "dark rectangular slab / hard step" at every ring boundary
	// (below sea level it fills with ocean and reads as a blue band; on land it
	// reads as a dark shadowed slab). The concentric fix aligned the boundaries
	// but the coincident double-drop still opened the trench -- proven by the
	// -VoxelNoClipmap control (bands vanish) at -VoxelVistaShot=600.
	//
	// THE FIX: no skirts at internal seams. Instead close the seam exactly with
	// a T-junction STITCH. Along a finer level's outer edge the vertices at EVEN
	// offsets from the centre coincide (same world XY, same sampled height) with
	// the coarser level's hole-edge vertices; the ODD-offset vertices sit
	// halfway between two coarser vertices, where the coarser surface is just
	// the straight quad edge between them. Snapping each odd edge vertex to the
	// average of its two even neighbours makes the finer edge trace exactly that
	// straight coarser edge -- the two rings then share an identical polyline at
	// the boundary and the seam is watertight (C0), no gap, no step, no trench,
	// nothing to hide. (This is the localized CDLOD/geoclipmap edge-stitch; the
	// full per-vertex distance morph remains the ADR-0002 M2-polish item, but
	// the visible artifact was the trench, and the stitch removes it outright.)
	// It reads only this level's own heights (avg of neighbours reproduces the
	// coarser linear edge because both sample the same field at the even
	// points), so it needs nothing from the coarser PMC and is robust to the
	// round-robin rebuild order.
	//
	// The hole edge is left at full height on every level: from the finer
	// neighbour's side the seam is already stitched (above), and level 0's hole
	// edge meeting the 2 km voxel cascade at full height lines the two systems
	// up better than the old dropped notch did.
	//
	// The OUTERMOST level (no coarser neighbour) keeps a real downward skirt on
	// its outer perimeter: that edge is the true edge of the whole clipmap
	// (~16-32 km out), and the drop makes the world edge fall away into dark
	// downward geometry instead of showing void/sky past the horizon.
	const bool bIsOutermost = (LevelIndex == NumLevels - 1);
	if (bIsOutermost)
	{
		for (int32 i = 0; i < NumVertsPerSide; ++i)
		{
			const bool bOuterX = (i == 0 || i == NumVertsPerSide - 1);
			for (int32 j = 0; j < NumVertsPerSide; ++j)
			{
				const bool bOuterY = (j == 0 || j == NumVertsPerSide - 1);
				if (bOuterX || bOuterY)
				{
					Positions[i * NumVertsPerSide + j].Z -= SkirtDropUU;
				}
			}
		}
	}
	else
	{
		// T-junction stitch on the four outer edges. A vertex is on exactly one
		// edge line unless it is a corner; corners are at index 0/64, which are
		// EVEN offsets from HalfIndex (32) and therefore coincide with a coarser
		// vertex -- so no odd-offset vertex is ever a corner, and each stitched
		// vertex has both of its along-edge neighbours on the same edge line.
		const auto StitchEdgeZ = [&](int32 i, int32 j, int32 iA, int32 jA, int32 iB, int32 jB)
		{
			Positions[i * NumVertsPerSide + j].Z =
				0.5 * (HeightsUU[iA * NumVertsPerSide + jA] + HeightsUU[iB * NumVertsPerSide + jB]);
		};
		for (int32 k = 0; k < NumVertsPerSide; ++k)
		{
			const bool bOddOffset = (((k - HalfIndex) & 1) != 0);
			if (!bOddOffset)
			{
				continue; // even offsets already coincide with the coarser edge
			}
			// Top/bottom edges (i fixed at 0 / NumVertsPerSide-1, varying j=k).
			StitchEdgeZ(0, k, 0, k - 1, 0, k + 1);
			StitchEdgeZ(NumVertsPerSide - 1, k, NumVertsPerSide - 1, k - 1, NumVertsPerSide - 1, k + 1);
			// Left/right edges (j fixed at 0 / NumVertsPerSide-1, varying i=k).
			StitchEdgeZ(k, 0, k - 1, 0, k + 1, 0);
			StitchEdgeZ(k, NumVertsPerSide - 1, k - 1, NumVertsPerSide - 1, k + 1, NumVertsPerSide - 1);
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
		// Level 0 uses the radial-hole topology (it abuts the voxel DISC);
		// every coarser level uses the square-hole one (it abuts a square
		// clipmap level). Topology never changes after this CreateMeshSection
		// -- later rebuilds only UpdateMeshSection positions -- so picking here
		// is picking once per level for the session.
		const TArray<int32>& LevelTriangles =
			(LevelIndex == 0) ? SharedTrianglesLevel0 : SharedTriangles;
		PMC->CreateMeshSection(0, Positions, LevelTriangles, Normals, SharedUV0, VertexColors, TArray<FProcMeshTangent>(),
		                       /*bCreateCollision*/ false);
		VoxelEofLedger::Count(VoxelEofLedger::ESource::Clipmap);
		// Through ApplyLevelMaterial, not SetMaterial directly: with a snow
		// override in play the first build is where the level has to pick up its
		// MID, and a bare SetMaterial here would give level 0 the plain material
		// and leave the override applying to nothing until the next debug
		// toggle. See ApplyLevelMaterial's comment.
		ApplyLevelMaterial(LevelIndex);
	}
	else
	{
		PMC->UpdateMeshSection(0, Positions, Normals, SharedUV0, VertexColors, TArray<FProcMeshTangent>());
		// Counted, and DELIBERATELY not separated from the Create above: Update
		// is the cheap path (no proxy recreate) but it still enqueues the
		// component for EndOfFrameUpdates, which is what this ledger measures.
		VoxelEofLedger::Count(VoxelEofLedger::ESource::Clipmap);
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

// The ONE place that decides what material a clipmap level draws with, and the
// one place that pushes parameters into it.
//
// WHY IT IS ONE PLACE NOW. There used to be three: RebuildLevel's first build
// set the shared material, UpdateDebugTint created a MID when the ring debug
// layer came on, and UpdateDebugTint set the shared material back when it went
// off -- which THREW THE MID AWAY. With DebugTint the only parameter that ever
// moved, throwing it away was harmless (the tint's whole purpose is to be
// temporary). It stops being harmless the moment a second parameter source
// exists, because toggling an unrelated debug layer off would silently revert
// it, and a parameter that reverts when you press something else is the hardest
// kind of appearance bug to reproduce.
//
// A level gets a MID only if something actually needs one. With no switches
// passed and the ring layer off, every level holds the plain shared material
// exactly as it always did, and this function allocates nothing.
void AVoxelClipmapActor::ApplyLevelMaterial(int32 LevelIndex)
{
	UProceduralMeshComponent* PMC = LevelMeshes.IsValidIndex(LevelIndex) ? LevelMeshes[LevelIndex] : nullptr;
	if (!PMC || !ClipmapMaterial)
	{
		return;
	}
	if (!LevelMIDs.IsValidIndex(LevelIndex))
	{
		LevelMIDs.SetNum(NumLevels);
	}

	const bool bNeedsMID = bSnowOverrideActive || bLastRingsEnabled || bVertexAlbedoActive;
	if (!bNeedsMID)
	{
		LevelMIDs[LevelIndex] = nullptr;
		PMC->SetMaterial(0, ClipmapMaterial);
		return;
	}

	// UProceduralMeshComponent derives from UMeshComponent (unlike
	// UVoxelChunkComponent's bare UPrimitiveComponent), so the built-in helper
	// is available directly. Called again on a later transition it returns the
	// component's existing MID rather than making a second one.
	UMaterialInstanceDynamic* MID = PMC->CreateDynamicMaterialInstance(0, ClipmapMaterial);
	LevelMIDs[LevelIndex] = MID;
	if (!MID)
	{
		return;
	}

	// EVERY parameter, every time, not just the one that changed. The cost is
	// four SetScalar calls on a transition that happens at most once per level
	// per debug toggle; the alternative is tracking which parameter is stale in
	// which order, which is how the throw-away above got missed in the first
	// place.
	if (bVertexAlbedoActive)
	{
		MID->SetScalarParameterValue(TEXT("VertexAlbedoWeight"), VertexAlbedoWeightOverride);
	}
	// Virtual-ring voxelization (2026-09-02, owner-accepted): the material's
	// VoxelizeWeight defaults to 1.0 in the generated asset; this push exists
	// so -VoxelClipmapVoxelize=0 is the one-flag control arm for the seam A/B
	// (same doctrine as -VoxelClipmapVertexAlbedo). Pushed unconditionally --
	// the override's default equals the material's default, so a flag-free run
	// is unchanged by this call.
	MID->SetScalarParameterValue(TEXT("VoxelizeWeight"), VoxelizeWeightOverride);
	if (bSnowOverrideActive)
	{
		MID->SetScalarParameterValue(TEXT("SnowlineLowMeters"), SnowlineLowMetersOverride);
		MID->SetScalarParameterValue(TEXT("SnowlineHighMeters"), SnowlineHighMetersOverride);
		MID->SetScalarParameterValue(TEXT("SnowTempMax"), SnowTempMaxOverride);
		MID->SetScalarParameterValue(TEXT("SnowTempFeather"), SnowTempFeatherOverride);
	}

	// docs/debug-tooling-plan.md P1 palette: "heightmap band cyan". Opaque white
	// is the multiplicative identity the material ships with, so setting it
	// explicitly when the layer is off is how the tint gets CLEARED now that the
	// MID is no longer discarded.
	MID->SetVectorParameterValue(TEXT("DebugTint"),
	                             bLastRingsEnabled ? VoxelDebug::HeightmapBandTint() : FLinearColor::White);
}

void AVoxelClipmapActor::UpdateDebugTint()
{
	const bool bRingsEnabled = VoxelDebug::IsRingsEnabled();
	if (bRingsEnabled == bLastRingsEnabled)
	{
		return; // no transition -- matches the "no MIDs created while off" doctrine (see class comment)
	}
	bLastRingsEnabled = bRingsEnabled;

	for (int32 Level = 0; Level < NumLevels; ++Level)
	{
		ApplyLevelMaterial(Level);
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
		if (bCaveRigActive && CaveLamp)
		{
			CaveLamp->SetRelativeLocation(CameraLocUU - GetActorLocation());
		}
	}

	// CONCENTRIC recenter (the ring-seam fix). ALL levels share ONE
	// camera-snapped origin, so every level's outer boundary lands on exactly
	// the same world-space square as the next-coarser level's inner hole
	// boundary -- both are +-32*SpacingL == +-16*Spacing(L+1) measured from the
	// SAME centre, so they coincide by construction.
	//
	// Previously each level snapped to its OWN spacing grid
	// (GridSnap(cam, SpacingUUForLevel(Level))), giving each ring a DIFFERENT
	// origin. A finer ring's outer edge and the coarser ring's hole edge, both
	// nominally at the same radius, were then offset by up to a fine cell in
	// world space -- opening an annular GAP (up to a whole coarse cell wide at
	// the outer boundaries, i.e. hundreds of metres) that showed the dark
	// underground veil / background straight through the seam: the "dark
	// rectangular slabs of apparently-missing terrain" artifact. (Square,
	// symmetric holes can only line up on all four sides when adjacent levels
	// share an origin -- no per-level snap can fix it; concentricity is the
	// only geometry that does, and is what a real clipmap uses regardless.)
	// With the rings concentric the boundaries coincide, the gap closes, and
	// the existing skirts (RebuildLevel pass 3) only have the residual
	// T-junction crack left to hide -- exactly what a 2x-spacing skirt is for.
	//
	// Snapped to the FINEST level's spacing (not the coarsest): keeps level 0's
	// inner hole within one fine cell of the camera so it still lines up with
	// the voxel cascade's outer edge at the near (2 km) seam. The cost is that
	// all four levels go dirty together each time the shared origin moves, but
	// round-robin still rebuilds at most one per tick -- identical per-frame
	// cost to before (the coarser levels simply refresh over the next few
	// ticks; distant, cosmetic, never in the M1 budget's critical path).
	const double Spacing0 = SpacingUUForLevel(0);
	const FVector2D CommonOrigin(FMath::GridSnap(CameraLocUU.X, Spacing0), FMath::GridSnap(CameraLocUU.Y, Spacing0));

	if (!bBootstrapped)
	{
		// One-time: build every level immediately (see class comment
		// "Recenter") so terrain doesn't pop in over several frames at
		// spawn -- a one-off cost, not a recurring per-frame one.
		for (int32 Level = 0; Level < NumLevels; ++Level)
		{
			RebuildLevel(Level, CommonOrigin);
			LastSnappedOriginUU[Level] = CommonOrigin;
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
		if (!bLevelBuilt[Level] || CommonOrigin != LastSnappedOriginUU[Level])
		{
			RebuildLevel(Level, CommonOrigin);
			LastSnappedOriginUU[Level] = CommonOrigin;
			bLevelBuilt[Level] = true;
			RoundRobinCursor = (Level + 1) % NumLevels;
			break;
		}
	}
}
