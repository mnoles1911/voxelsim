#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
// For FLakeSheetLod, which is a nested struct and so cannot be forward
// declared. The .cpp already included this; the ladder moved it up here.
#include "VoxelWaterSubsystem.h"

#include "VoxelWaterSheetActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;

// LAKE SHEETS AT RANGE -- docs/watershed-system-plan.md work item 5, §5.2's
// "clipmap bands" row.
//
// THE DEFECT THIS EXISTS FOR, measured rather than assumed. UVoxelWaterSubsystem's
// RefreshImplicitWater meshes implicit water only inside a 65-brick disc, 52 m
// across (kImplicitRadiusBricks). Basin 1 of tile (-12,-5) -- the exemplar lake
// -- is 2.0 x 2.4 km. So 99.9% of that lake is not merely coarse at range, it is
// NOT DRAWN. A vista over the basin shows a dry bowl; a shoreline capture cannot
// contain both the shallow band and the deep centre, because the disc is smaller
// than the shallow band is wide. Work item 4 shipped a lake you can stand in;
// this is the one you can see.
//
// WHAT IT DRAWS. One flat translucent rectangle set per baked basin, at that
// basin's own datum, from the SAME extent masks the near field's ImplicitFn
// consumes (UVoxelWaterSubsystem::BuildLakeSheetRects -> vxc::lakeSheetRects).
// No collision, no tick on the water CA, no replication, nothing persisted --
// §5.2's rule that water at range is a SURFACE, and §7's "content is free at
// runtime" unchanged: an untouched lake still costs 0 bricks and 0 bytes.
//
// WHY IT IS NOT PART OF AVoxelClipmapActor, which is the other flat-mesh-at-
// range actor and was the obvious home. The clipmap's geometry is a
// camera-centred lattice at a fixed vertex count, rebuilt whole when the camera
// crosses a cell; a sheet's geometry is per-BASIN, cached across camera motion,
// and rebuilt only when a lake enters or leaves range or the near-field hole
// moves. Sharing an actor would mean sharing neither the rebuild trigger nor the
// budget, i.e. sharing the file and nothing else.
//
// WHERE IT STOPS, AND WHY THAT EDGE IS A SUBTRACTION. Inside the implicit disc
// the near field already draws real water voxels at the same datum. Two coplanar
// translucent surfaces z-fight AND blend twice, so the sheet cuts the disc's
// exact footprint out of itself (vxc::subtractRect) instead of fading or
// offsetting. The cut is applied ONLY when that disc is actually meshing this
// basin's surface -- the disc is bounded in z as well as xy, so a camera 30 m
// above the water has no near-field water to hand over to and must not have a
// hole cut for it. GetImplicitWaterDiscUU reports both spans for exactly that
// test.
//
// THE MATERIAL IS M_WaterVoxel, THE SAME ONE THE VOXELS USE, and that is a
// correctness choice rather than a convenience: the owner-tuned constants
// (shallow/deep opacity, Beer-Lambert D, the two tints, the W6 Fresnel sky term)
// then apply to the vista and the near field by construction, and the two cannot
// diverge the way the clipmap's biome colours would have if it had authored its
// own. The vertex-colour convention is that file's, reproduced here:
//   R = fill fraction, 1.0 -- a sheet is a full surface, so the fill-drop WPO is
//       zero and the surface sits AT the datum.
//   G = AO, 1.0 -- no greedy-mesher occlusion applies to a free-standing sheet.
//   B = 1 -- this vertex IS on its cell's +Z boundary; it is what un-masks the
//       ripple normal and keeps the far water shimmering like the near water.
//   A = foam activity, 0 -- baked water is settled, exactly as every implicit
//       brick already passes.
// The depth cue needs nothing added: it is a scene-DEPTH read (thickness along
// the view ray), so a flat sheet over a real bowl gets pale at the shore and
// deep in the middle for free, from the geometry rather than from a second
// authored gradient.
UCLASS()
class VOXELEARTH_API AVoxelWaterSheetActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelWaterSheetActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	// Diagnostics for the capture writeup and the perf run: what is on screen,
	// and what it cost to put there. A sheet that is absent and a sheet that is
	// present but behind terrain look the same in a screenshot; these do not.
	int32 GetSheetCount() const { return Sheets.Num(); }
	int32 GetSheetRectCount() const { return TotalRects; }
	int32 GetUnresolvedBasinCount() const { return UnresolvedBasins; }

private:
	// One basin's mesh and the state that says whether it is still current.
	struct FSheet
	{
		int32 TileX = 0, TileY = 0, BasinId = 0;
		int32 Section = INDEX_NONE;
		int32 StepPx = 1;
		int32 RectCount = 0;
		// The camera cell this basin's LOD bands were last centred on, and
		// whether they depend on the camera at all. A basin further out than the
		// outermost BOUNDED band decimates uniformly, so its geometry does not
		// change when the camera moves and it must leave the rebuild rotation --
		// with ~289 basins resident, rebuilding all of them because the camera
		// crossed a cell is the cost this flag exists to refuse.
		FIntPoint LodKey = FIntPoint(MIN_int32, MIN_int32);
		bool bUniformCoarse = true;
		// Built, as opposed to "has rectangles". A basin whose extent decimates
		// to zero cells at this range is BUILT and must leave the rebuild
		// rotation; keying the rotation on RectCount instead would re-mesh every
		// such basin on every tick forever.
		bool bBuilt = false;
		// Set once the basin's tile refused to decode. Retried never, counted
		// always -- see the .cpp.
		bool bUnresolved = false;
		double SurfaceZUU = 0.0;
		double MinXUU = 0.0, MinYUU = 0.0, MaxXUU = 0.0, MaxYUU = 0.0;
		// The near-field hole this mesh was cut with, so a moving camera only
		// rebuilds the one basin whose water it is standing in.
		bool bHadHole = false;
		FBox2D HoleUU = FBox2D(ForceInit);
	};

	// Rebuilds one basin's mesh section. Returns false if the basin would not
	// resolve (its tile or a block failed to decode) -- which is counted, not
	// swallowed, because it is indistinguishable from a dry basin on screen.
	bool RebuildSheet(FSheet& Sheet, const FVector& CamUU);

	// Decimation for a basin, in fine pixels per emitted cell. See the .cpp.
	int32 StepForBasin(double SpanUU) const;

	// The steps and radii for one basin -- fine underfoot, coarsening with range,
	// coarsest step still the basin-span one StepForBasin picks. No camera: this
	// is the ladder's SHAPE, which depends only on the basin.
	void BuildLadder(const FSheet& Sheet, UVoxelWaterSubsystem::FLakeSheetLod& OutLod) const;

	// BuildLadder plus the band centre, and whether this basin is far enough out
	// that every block lands in the last unbounded band (so it can leave the
	// rebuild rotation).
	void BuildLodForBasin(const FSheet& Sheet, const FVector& CamUU,
	                      UVoxelWaterSubsystem::FLakeSheetLod& OutLod, bool& bOutUniform) const;

	// Does this basin reach the outermost BOUNDED band at this camera? Evaluated
	// against the snapped cell, so it cannot flap.
	bool IsBandedAtCamera(const FSheet& Sheet, const FVector& CamUU) const;

	// Which hysteresis cell the camera is in. The bands only re-centre when this
	// changes, so a metre of walking rebuilds nothing.
	FIntPoint LodKeyForCamera(const FVector& CamUU) const;

	// The centre of that cell. The bands are centred here rather than on the
	// camera, which is what makes a basin's mesh a pure function of its LodKey.
	FVector2D SnappedCamXY(const FVector& CamUU) const;

	// The near-field cut-out, or false when the implicit disc is not meshing
	// water at this datum (too far above or below it) and no hole is owed.
	bool HoleForDatum(double SurfaceZUU, FBox2D& OutHoleUU) const;

	bool GetCameraLocationUU(FVector& Out) const;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> WaterMaterial;

	TArray<FSheet> Sheets;
	int32 TotalRects = 0;
	int32 UnresolvedBasins = 0;

	// Basin scan radius in UU. Defaults to the clipmap's own outer half-extent so
	// the sheet covers exactly the ground the far terrain draws -- water stops
	// where the world it sits in stops, and neither number is written down twice.
	// -VoxelLakeSheetRangeM overrides it for the range A/B.
	double ScanRadiusUU = 0.0;

	// Camera position the basin set was last gathered at, and how far it may move
	// before that set is re-gathered. A gather loads fine tiles, so it is not a
	// per-tick operation; a kilometre of travel cannot bring a basin into a
	// multi-kilometre radius that was not already in it.
	FVector2D LastGatherXY = FVector2D::ZeroVector;
	// The centre the CURRENT scan is clipping against, held apart from
	// LastGatherXY so a scan that spans several ticks keeps one origin and does
	// not admit a different set of basins on its last tile than on its first.
	FVector2D GatherCenterXY = FVector2D::ZeroVector;
	bool bGathered = false;
	double RegatherDistanceUU = 100000.0; // 1 km

	// Fine tiles this scan still has to read, one per tick. See Tick().
	TArray<FIntPoint> PendingTiles;

	// -VoxelLakeSheets=0 removes the whole feature on the same binary. This is
	// THE CONTROL for every sheet capture, and it is a switch rather than a cvar
	// for the reason -VoxelNoClipmap is: -ExecCmds lands after BeginPlay.
	bool bEnabled = true;

	// Fine pixels per emitted sheet cell is derived from a target cell COUNT per
	// basin side, not from a fixed metre size: a 30 m pond and a 2.4 km lake
	// otherwise get wildly different triangle budgets for the same screen area.
	// -VoxelLakeSheetCells overrides it.
	int32 TargetCellsPerSide = 128;

	// THE FINEST BAND'S HALF-EXTENT, in metres. Inside it the sheet meshes at ONE
	// FINE PIXEL (1.875 m), the finest the baked extent mask can express; each
	// band out doubles the radius and coarsens the step until the basin-span step
	// takes over.
	//
	// 96 m rather than something tidier because the number it has to beat is the
	// retired near disc's 25.6 m half-extent, and the hysteresis grid below can
	// take up to half a cell off the effective radius. 96 m with a 30 m grid
	// never drops the fine band below ~81 m, i.e. always more than three times
	// the coverage the voxel path had. -VoxelLakeSheetFineM moves it.
	double FineBandRadiusM = 96.0;

	// Camera hysteresis for the band centre, in fine pixels. The bands re-centre
	// when the camera crosses a cell of this size, not when it moves -- the same
	// recentring policy the fluid window and the clipmap use, and the reason a
	// basin is not remeshed because the player took a step. 16 fine pixels = 30 m.
	int32 LodSnapPx = 16;

	// THE LADDER'S OFF SWITCH, and the reason it is a rung COUNT rather than a
	// radius: -VoxelLakeSheetFineM 0 does NOT turn the ladder off. Radii round UP
	// to the band alignment (lcm of the rungs, 28 fine px on the exemplar basin),
	// so asking for a zero-metre fine band still yields a 52.5 m one. 1 here is
	// the honest control -- one band at the basin-span step, i.e. exactly the
	// decomposition the sheet shipped with before distance-aware LOD.
	int32 MaxBands = 4;

	// One basin rebuilt per tick at most, so a first frame in range never lands
	// as a hitch -- the same budget discipline RefreshImplicitWater's
	// kMaxImplicitMeshesPerTick and the clipmap's round-robin already use.
	int32 RoundRobinCursor = 0;

	bool bLoggedFirstBuild = false;
};
