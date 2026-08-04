#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelRiverRibbonActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;

// FLOWING WATER AT RANGE -- docs/water-handover-2026-08-04.md Phase 4.
//
// THE DEFECT THIS EXISTS FOR. UVoxelWaterSubsystem::RefreshImplicitWater meshes
// water only inside a 65x65x33 brick disc around the CAMERA brick --
// +/-25.6 m in xy and +/-12.8 m in z. The box follows the camera, not the
// water, so a river is drawn only while you are standing in it: a capture at
// 90 m altitude logs `0 candidate brick(s)`. Lakes already had a far-field path
// (AVoxelWaterSheetActor); rivers had none, because the sheet half of
// IWaterSampler is structurally lake-only -- the basin registry, holdsWater()
// and extentMaskFor all assume a basin, and CompositeWaterSampler forwards the
// sheet half to lakes on purpose. This actor is the missing consumer.
//
// ==========================================================================
// WHY A POLYLINE RIBBON AND NOT RECTANGLES
// ==========================================================================
//
// The obvious implementation was AVoxelWaterSheetActor's: run-length rectangle
// decomposition of the wet mask. The owner has already rejected that output, in
// these words: "sharp, rectangular, square edges where it meets land rather
// than a natural curving, arching shoreline." On a lake that is a bad EDGE. On
// a river it would be the WHOLE OBJECT -- a reach is 1-3 fine pixels wide, so a
// 15 m axis-aligned decomposition of it is a chain of disconnected squares
// rather than a river.
//
// So the geometry comes from voxelcore/riverribbon.h, which emits ORDERED
// CENTRELINE POLYLINES simplified by Douglas-Peucker against a ONE PIXEL
// perpendicular tolerance. That tolerance is the whole trick: an 8-connected
// raster staircase deviates at most half a pixel from its own chord and is
// deleted, while a meander's sagitta is tens of metres and is kept. The
// artefact is sub-pixel and the shape is not. This actor only sweeps that
// polyline into quads; it makes no shape decision of its own, which is what
// keeps the no-staircase property a tested property of voxel-core rather than
// an untested property of an engine file.
//
// ==========================================================================
// THE WIDTH POLICY, AND THE MEASUREMENT THAT DECIDED IT
// ==========================================================================
//
// THE RIBBON IS DRAWN AT THE WIDTH THE BAKE ACTUALLY DREW. It is NOT widened
// to hold a minimum screen width, and that is a measured decision, not a
// stylistic one.
//
// Measured with vxc_riverribbonprobe over corridor tile (-11,-5) of the bv10
// cache, 315 samples, water datum minus the AMPLIFIED drawn ground:
//
//     widening   drawn width   fraction of the edge BELOW drawn ground
//     none            2.65 m                            0.00%  (centreline)
//     1x  (natural)   2.65 m                            1.59%
//     2x              5.30 m                            7.94%
//     3x              7.94 m                           24.13%
//     4x             10.59 m                           27.94%
//     6x             15.89 m                           46.03%
//     8x             21.18 m                           58.41%
//
// Read the two ends together, because that is the finding. The unwidened
// ribbon stands clear of the terrain everywhere -- headroom p10 +673 mm, p50
// +829 mm, nothing buried. Every metre of widening pushes the edge out over a
// bank that is HIGHER than the water, and the depth test eats it. A ribbon that
// is 58% buried is not a wide river, it is a dashed line, and a dashed line
// that re-dashes differently as the camera moves is the shimmer this feature
// was explicitly told not to ship.
//
// AND WIDENING DOES NOT EVEN BUY THE VISIBILITY IT COSTS THAT FOR. At 1440p and
// 90 degrees horizontal FOV one pixel subtends 1/1280 in tangent, so a 2.65 m
// reach is 3.39 px at 1 km, 0.68 px at 5 km and 0.17 px at 20 km. At 20 km,
// merely reaching ONE pixel takes ~15.9 m of width -- a 6x widening, measured
// at 46.0% of the edge buried -- and a 2 px floor takes ~31 m, past the 8x arm
// that measured 58.4%. There is no widening factor that is both visible at
// 20 km and not buried.
// The honest conclusion, stated so nobody re-derives it: a 2.65 m creek is
// genuinely below the resolution of a 20 km vista, and the way to put a river
// on that horizon is to make the river WIDER IN THE BAKE where the discharge
// says it should be -- channel_width_m(Q) deciding extent the way
// water_depth_m(Q) already decides depth -- not to inflate a creek downstream
// of the bake. A 16 m trunk river is 1 px at 20 km with no widening at all.
//
// -VoxelRiverRibbonMinPx=F culls reaches whose projected width falls below F
// so that A/B can be taken, and it defaults to 0 -- draw everything, report the
// numbers, and let the owner judge the frame. GetSubPixelPathCount() and the
// DRAINED log line say how many reaches were below one pixel at the pose the
// capture was taken from, so "is that shimmering thread the bug" is answerable
// from the log rather than from opinion.
//
// ==========================================================================
// THE DATUM. WHICH GROUND.
// ==========================================================================
//
// Every vertex sits at reconstructedGroundMm + baked depth -- ground #2, the
// cubic B-spline reconstruction, read through RiverSampler::surfaceAtPixel.
// That is the SAME call the near field reaches through waterSurfaceMmAtVoxel,
// so the near and far surfaces agree on HEIGHT by construction rather than by
// tuning. It is NOT GetSurfaceHeightUU, the amplified surface, which is
// explicitly forbidden as a water datum in both tile_codec.py and tilestore.h.
// Three grounds exist in this codebase and they have been conflated three
// times across two languages; this file names which one, once, here.
//
// Tone is a different question and is NOT solved by construction: the near
// field is a voxel shell and this is a surface, and the difference has been
// measured at -14.1 to +9.0 mm-equivalent blueness with the SIGN FLIPPING with
// viewing angle. A fixed tint cannot reconcile them. This actor therefore uses
// M_WaterVoxel, the same material the voxels and the lake sheets use, so the
// owner-tuned constants apply to all three by construction and cannot diverge;
// what remains is Phase 5's problem and is not papered over here.
//
// ==========================================================================
// WHERE IT STOPS, AND WHY THAT EDGE IS A SUBTRACTION
// ==========================================================================
//
// Inside the implicit disc the near field already draws real water voxels at
// the same datum. Two coplanar translucent surfaces z-fight AND blend twice, so
// the ribbon CLIPS ITS SEGMENTS against the disc's exact footprint rather than
// fading or offsetting -- the same choice AVoxelWaterSheetActor made, for the
// same reason, and clipped exactly (Liang-Barsky in doubles) rather than
// dropped per segment, because a segment here is ~14 m and the handover window
// is only 9-25 m wide. A per-segment drop would put a 14 m hole in the water at
// the closest range, which is exactly where a seam is most visible.
//
// The cut is applied ONLY where the disc is actually meshing water at THIS
// point's datum. A river descends along its length, so that test is per POINT,
// not per path: the disc is bounded in z, and a reach that passes 30 m below
// the camera is owed no hole.
UCLASS()
class VOXELEARTH_API AVoxelRiverRibbonActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelRiverRibbonActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	// Diagnostics for the capture writeup. A ribbon that is absent, a ribbon
	// that is present but behind terrain, and a ribbon that is present and
	// sub-pixel all look the same in a screenshot; these do not.
	int32 GetPathCount() const { return Paths.Num(); }
	int32 GetQuadCount() const { return TotalQuads; }
	int64 GetWetPixelCount() const { return WetPixels; }
	int64 GetCentrePixelCount() const { return CentrePixels; }
	int64 GetUnresolvedBlockCount() const { return UnresolvedBlocks; }
	// Reaches whose MEAN width projects to under one pixel from the current
	// camera. Non-zero does not mean broken; it means the frame is being asked
	// to resolve something narrower than a pixel, which is the failure mode the
	// width table in this header is about.
	int32 GetSubPixelPathCount() const { return SubPixelPaths; }

private:
	// One traced reach, already in world UU, plus the mesh section drawing it.
	struct FRibbonPath
	{
		TArray<FVector> Points;   // centreline, at the datum
		TArray<double> HalfWidth; // UU, per point, as the bake drew it
		int32 Section = INDEX_NONE;
		int32 QuadCount = 0;
		bool bBuilt = false;
		// The near-field hole this mesh was cut with, so a moving camera
		// rebuilds only the reach it is standing in.
		bool bHadHole = false;
		FBox2D HoleUU = FBox2D(ForceInit);
		double MinZUU = 0.0, MaxZUU = 0.0;
		FBox2D BoundsXY = FBox2D(ForceInit);
	};

	// Which phase of the staged build the actor is in. The fill is banded
	// across ticks because it decodes 256x256 water blocks off disk; thin,
	// trace and simplify are near-linear scans of a mask that is ~0.01% wet and
	// run in one go.
	enum class EPhase : uint8
	{
		Idle,
		Filling,
		Ready
	};

	bool RebuildPath(FRibbonPath& Path);
	bool GetCameraLocationUU(FVector& Out) const;
	void StartGather(const FVector& CamUU);
	void CountSubPixelPaths(const FVector& CamUU);

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> WaterMaterial;

	TArray<FRibbonPath> Paths;
	int32 TotalQuads = 0;
	int64 WetPixels = 0;
	int64 CentrePixels = 0;
	int64 UnresolvedBlocks = 0;
	int32 SubPixelPaths = 0;

	EPhase Phase = EPhase::Idle;
	int32 BandCount = 0;
	int32 NextBand = 0;
	double GatherStartSec = 0.0;

	// Scan radius in UU. 4 km, and this is a MEMORY decision: the wet mask is
	// one byte per fine pixel over the whole square, which riverribbon.h itself
	// sizes at 4267^2 = 18 MB for exactly this radius. Doubling the radius
	// quadruples that. -VoxelRiverRibbonRangeM moves it, and the resident cost
	// is logged after each gather rather than assumed.
	double ScanRadiusUU = 400000.0; // 4 km

	// Camera position the ribbon set was last built at, and how far it may move
	// before it is rebuilt. A gather reads fine tiles, so it is not a per-tick
	// operation.
	FVector2D LastGatherXY = FVector2D::ZeroVector;
	bool bGathered = false;
	double RegatherDistanceUU = 100000.0; // 1 km

	// -VoxelRiverRibbons=0 removes the whole feature on the same binary. THE
	// CONTROL for every ribbon capture, and a switch rather than a cvar for the
	// reason -VoxelNoClipmap is one: -ExecCmds lands after BeginPlay.
	bool bEnabled = true;

	// Minimum projected width, in pixels, for a reach to be drawn at all.
	// 0 = draw everything. See the width policy in this header.
	double MinScreenPx = 0.0;

	// One reach re-meshed per tick at most, the same budget discipline
	// RefreshImplicitWater's kMaxImplicitMeshesPerTick and the lake sheet's
	// round robin already use.
	int32 RoundRobinCursor = 0;

	bool bLoggedFirstBuild = false;
};
