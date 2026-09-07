#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
// Self-contained include for the VoxelOcean::kReachDiameterUU derivation below:
// this header used VoxelCoords::kNumLevels while riding a unity-build
// neighbour's include for years; the first adaptive (standalone) compile of the
// .cpp broke it.
#include "VoxelCoords.h"
#include "VoxelOceanActor.generated.h"

class UProceduralMeshComponent;
class UPostProcessComponent;
class UMaterialInstanceDynamic;

// ============================================================================
// THE OCEAN'S GEOMETRY, DERIVED RATHER THAN CHOSEN
// ============================================================================
//
// Everything the grid's shape depends on lives here, in one namespace, because
// the previous version of this actor kept its reach in a class constant that
// two separate far-field growths silently outgrew (see kReachDiameterUU below).
// A number that has to track something else is a derivation, not a constant.
namespace VoxelOcean
{
// DERIVED FROM THE FAR FIELD, NOT FROM THE CASCADE, AND THE DIFFERENCE HAS
// BITTEN TWICE. It was a flat 4,000,000 UU (+-20 km) that became a VISIBLE HARD
// EDGE when the far field grew past it; it was then derived from the CASCADE
// edge, which was right only while voxels were the farthest thing drawn. Since
// 2026-09-02 the farthest ground is the CLIPMAP again (outer half-extent = 8 x
// the cascade edge -- AVoxelClipmapActor: HalfIndex(32) / HoleHalfIndex(16) x
// 2^(NumLevels-1), i.e. 2 x 2^2 at 3 levels), and an ocean sized to the cascade
// alone would end at +-10 km under 65 km of clipmap terrain: a straight water
// edge across every coastal vista.
//
// The ring radii are geometric -- Outer(L) = 64 m * 2^L -- so the reach follows
// VoxelCoords::kNumLevels with no include of the streaming subsystem (which
// would be circular from here). kClipmapExtentMultiple restates the clipmap
// arithmetic for the same reason; the static_assert in VoxelOceanActor.cpp
// checks it against the actor's real constants. The 2x is the DIAMETER, with
// 25% margin so a camera near the cascade edge still sees water to the horizon.
//
// (This constant is the fifth thing in this codebase found sizing itself off a
// reach the far field outgrew -- grep for constants in METRES or UU before
// assuming any reach sweep is done.)
inline constexpr double kOutermostRingOuterMetres =
	64.0 * double(int64(1) << (VoxelCoords::kNumLevels - 1));
inline constexpr double kClipmapExtentMultiple = 8.0; // 2 * 2^(NumLevels-1), NumLevels 3
inline constexpr double kReachDiameterUU =
	2.0 * (kClipmapExtentMultiple * kOutermostRingOuterMetres) * 100.0 * 1.25;
// What the grid below actually has to cover, as a half-extent. The ONE place
// the diameter above becomes a radius.
inline constexpr double kRequiredHalfExtentUU = 0.5 * kReachDiameterUU;

// THE FINEST CELL, 1.5 m. It is also the recentre snap (see UpdateFollowPlane):
// the grid may only ever move by a whole cell of its own finest level, or the
// ring boundaries land on a different phase of the world each frame and the
// stitched seams stop lining up with the geometry they were stitched to.
inline constexpr double kCellUU = 150.0;

// THE OTHER SEED CELL, 3.0 m -- what "Ocean Mesh Detail: off"
// (voxel.Ocean.HalfDetail=1) builds with. It is a SEED, not a second geometry:
// everything below derives from whichever of these two is in play, so the half
// detail grid is not a parallel set of constants to keep in step. The centre
// patch keeps its +-96 m (it is sized by the material's WPO fade, which does not
// care how many vertices carry it) and simply holds a quarter as many vertices.
//
// THE DIVISOR IS 2, NOT 1.5 OR 3, and that is the same rule the voxel lattice
// obeys: the rings step 2:1 and the stitch (BuildOceanGrid) is a 2:1 stitch, so
// a seed cell that is not a power-of-two multiple of the other would put the
// ring boundaries on cell counts that do not halve and the fan would have no
// midpoint to fan from.
inline constexpr double kHalfDetailCellUU = 2.0 * kCellUU;

constexpr double CellUUForHalfDetail(bool bHalfDetail)
{
	return bHalfDetail ? kHalfDetailCellUU : kCellUU;
}

// THE CENTRE PATCH'S HALF-EXTENT, +-96 m, i.e. 128 x 128 cells at kCellUU.
//
// IT IS SIZED BY THE MATERIAL, NOT BY TASTE. The shared wave module fades World
// Position Offset to identically zero between 55 m and 72 m
// (Tools/create_water_voxel_material.py's WaveWpoFadeStartM/EndM, mirrored into
// M_Ocean) -- so every vertex that can actually MOVE is inside 72 m, and a
// centre patch that stopped short of that would have visible waves dying on a
// geometry boundary instead of on the material's fade. 96 m clears the fade end
// with margin, and matches AVoxelWaterSheetActor::FineBandRadiusM so the ocean
// and a lake sheet in the same frame carry displacement at the same density.
inline constexpr double kCentreHalfUU = 9600.0;

// ---- THE RINGS -----------------------------------------------------------
//
// Concentric square annuli, each with cells TWICE the size of the ring inside
// it. Two integers shape them, and both are floors rather than targets:
//
//   kMinRingHalfCells  -- the smallest a ring's outer half-extent may be, in
//                         ITS OWN cells. Must be EVEN: the next ring out uses
//                         this number as the width of its hole in cells of
//                         twice the size, and an odd count cannot be halved.
//   kMinRingWidthCells -- the fewest cells thick a ring may be.
//
// WHY THE RINGS ARE NOT SELF-SIMILAR, which is the textbook clipmap and was the
// first thing tried. A self-similar ring (hole n cells, outer 2n cells) has its
// n forced by the patch inside it -- here n = 64 -- and then every ring costs
// 12.7 k vertices and TEN of them are needed to reach 82 km: ~143 k vertices
// for a surface that is FLAT everywhere past 72 m, because the material's WPO
// fade has already zeroed the displacement there. The rings exist to carry a
// horizon, not detail, so they are allowed to shrink toward a floor: the hole
// halves in cell count each ring while the width stays at its minimum, and the
// whole grid comes in around 40 k vertices for 135 km of reach.
inline constexpr int32 kMinRingHalfCells = 22;
inline constexpr int32 kMinRingWidthCells = 10;
inline constexpr int32 kMaxRings = 24; // loop bound; the real answer is ~12

// The outer half-extent of the next ring out, in that ring's own cells, given
// the previous level's outer half-extent in the PREVIOUS level's cells. The
// hole is PrevHalfCells / 2 of this ring's cells across (this ring's cells are
// twice as big), so the width in cells is the difference.
constexpr int32 NextRingHalfCells(int32 PrevHalfCells)
{
	int32 N = PrevHalfCells / 2 + kMinRingWidthCells;
	if (N < kMinRingHalfCells)
	{
		N = kMinRingHalfCells;
	}
	if ((N & 1) != 0)
	{
		++N; // EVEN, always -- see kMinRingHalfCells
	}
	return N;
}

// How many rings it takes to cover kRequiredHalfExtentUU, and how far that
// actually reaches. DERIVED, so a change to VoxelCoords::kNumLevels or to the
// clipmap's extent moves the ocean with it and no sixth reach constant is
// written down anywhere.
//
// TAKES THE SEED CELL AS AN ARGUMENT (2026-09-05), because there are now two of
// them. Both answers are computed below and BOTH are asserted: a detail level
// that quietly stopped reaching the clipmap edge would be a straight water edge
// under far terrain -- the exact defect kReachDiameterUU exists to prevent --
// and it would only show up in the half of the toggle nobody happened to
// screenshot.
constexpr int32 RingCountForReach(double SeedCellUU)
{
	double HalfUU = kCentreHalfUU;
	int32 PrevHalfCells = int32(kCentreHalfUU / SeedCellUU); // 64 at full detail, 32 at half
	int32 Rings = 0;
	while (HalfUU < kRequiredHalfExtentUU && Rings < kMaxRings)
	{
		++Rings;
		PrevHalfCells = NextRingHalfCells(PrevHalfCells);
		HalfUU = double(PrevHalfCells) * SeedCellUU * double(int64(1) << Rings);
	}
	return Rings;
}

constexpr double ReachHalfExtentUU(double SeedCellUU)
{
	double HalfUU = kCentreHalfUU;
	int32 PrevHalfCells = int32(kCentreHalfUU / SeedCellUU);
	const int32 Rings = RingCountForReach(SeedCellUU);
	for (int32 r = 1; r <= Rings; ++r)
	{
		PrevHalfCells = NextRingHalfCells(PrevHalfCells);
		HalfUU = double(PrevHalfCells) * SeedCellUU * double(int64(1) << r);
	}
	return HalfUU;
}

inline constexpr int32 kRingCount = RingCountForReach(kCellUU);
inline constexpr double kReachHalfUU = ReachHalfExtentUU(kCellUU);
inline constexpr int32 kHalfDetailRingCount = RingCountForReach(kHalfDetailCellUU);
inline constexpr double kHalfDetailReachHalfUU = ReachHalfExtentUU(kHalfDetailCellUU);

// The two detail levels are ALLOWED TO BUILD A DIFFERENT NUMBER OF SECTIONS,
// and they do -- coarser cells reach the same distance in one ring fewer. That
// is why BuildOceanGrid clears before it builds rather than overwriting by
// index; there is no constant here for the maximum because nothing needs one.

static_assert(kRingCount > 0 && kRingCount < kMaxRings,
              "the ocean ring derivation did not terminate below its loop bound -- either the far "
              "field grew enormously or NextRingHalfCells stopped growing the reach");
static_assert(kHalfDetailRingCount > 0 && kHalfDetailRingCount < kMaxRings,
              "the HALF DETAIL ocean ring derivation did not terminate below its loop bound; see "
              "the full-detail assertion above");
// THE INVARIANT, NOT THE NUMBER. This deliberately does not pin kCellUU or the
// ring count to any particular value -- what has to hold is that the water
// reaches at least as far as the clipmap draws ground, at EVERY detail level
// the player can select. Pinning the numbers instead would make a legitimate
// retune fail to compile while a genuine short reach at one detail level went
// unnoticed.
static_assert(kReachHalfUU >= kRequiredHalfExtentUU,
              "the derived ocean ring set does not reach as far as the clipmap draws ground; water "
              "would end in a straight edge under far terrain, which is the defect kReachDiameterUU "
              "exists to prevent");
static_assert(kHalfDetailReachHalfUU >= kRequiredHalfExtentUU,
              "the HALF DETAIL ocean ring set does not reach as far as the clipmap draws ground -- "
              "turning Ocean Mesh Detail off would put a straight water edge under far terrain");
static_assert(kCentreHalfUU == double(int32(kCentreHalfUU / kCellUU)) * kCellUU &&
                  kCentreHalfUU == double(int32(kCentreHalfUU / kHalfDetailCellUU)) * kHalfDetailCellUU,
              "the centre patch's half-extent must be a whole number of finest cells AT BOTH DETAIL "
              "LEVELS, or ring 1's hole cannot line up with it and the 2:1 stitch has nothing to "
              "stitch to");
static_assert((int32(kCentreHalfUU / kCellUU) & 1) == 0 &&
                  (int32(kCentreHalfUU / kHalfDetailCellUU) & 1) == 0,
              "the centre patch's half-extent in cells must be EVEN at both detail levels -- ring 1 "
              "uses it as the width of its hole in cells of twice the size (see kMinRingHalfCells), "
              "and an odd count cannot be halved");
} // namespace VoxelOcean

// W1 first slice (docs/voxel-earth-implementation-plan.md SS3.7 "Implicit
// static" water state; plan SS4 water track W1): the world *looks* hydrated
// with ZERO voxel water data and no simulation. Ocean = implicit infinite
// water at z<0 (sea level z=0 == voxel z=0 == UE world origin z=0, per
// VoxelCoords.h). Spawned by AVoxelEarthGameMode::BeginPlay alongside the
// light rig (same "no authored map yet, spawn from code" reasoning).
//
// ---- FROM TWO TRIANGLES TO A RING GRID, 2026-09-04 (plan B5) --------------
//
// It WAS a single UStaticMeshComponent on /Engine/BasicShapes/Plane, scaled to
// 164 km, and the note here said a UProceduralMeshComponent would "blur the
// line with the custom scene-proxy voxel rendering path". That reasoning was
// about which renderer owns terrain and it is still right about terrain -- but
// it bought a surface with FOUR VERTICES, and the ocean is now asked to carry
// wind waves as World Position Offset. WPO is a per-VERTEX displacement: on two
// triangles it moves four corners 82 km apart and interpolates a plane between
// them, so every wave the material computes is invisible by construction. No
// material change can fix that; only vertices can.
//
// So the plane is a UProceduralMeshComponent carrying a concentric ring grid --
// the same component class the lake sheet uses, for the same reason (a runtime
// mesh with no asset behind it), and emphatically still NOT the voxel scene
// proxy. Built at BeginPlay and then only when the DETAIL LEVEL changes: it is
// camera-CENTRED but camera-independent in shape, so following the camera is a
// transform, not a re-mesh. See VoxelOcean above for the derivation of its
// extent, and BuildOceanGrid for the 2:1 stitching that keeps the ring seams
// closed.
//
// ---- WHAT MADE IT REBUILDABLE, 2026-09-05 ---------------------------------
//
// The player-facing "Ocean Mesh Detail" row (VoxelGraphicsUserSettings) fronts
// voxel.Ocean.HalfDetail, and a graphics setting that only takes effect on the
// next launch is a setting the player cannot judge. So Tick compares the cvar
// against what was actually BUILT and rebuilds once when they differ. Note the
// comparison is against the built state, not against a "dirty" flag: a flag set
// by whoever wrote the cvar would be one more thing that can be forgotten,
// while the built state cannot disagree with the mesh -- it IS what the mesh
// was made from.
//
// Doctrine that did NOT change: no collision (being IN the water is the datum's
// job, not this mesh's), no shadow, no decals, one material slot, and the mesh
// is cosmetic in every sense -- nothing gameplay-facing reads it.
UCLASS()
class VOXELEARTH_API AVoxelOceanActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelOceanActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	// The ring grid (rings around a fine centre patch), recentred in XY under
	// the camera and pinned in Z to the LIVE sea surface every tick -- see
	// UpdateFollowPlane. Built in BeginPlay by BuildOceanGrid, and again only
	// when voxel.Ocean.HalfDetail changes.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Ocean")
	TObjectPtr<UProceduralMeshComponent> OceanMesh;

	// Global (bUnbound) post-process volume-equivalent for the underwater
	// treatment; blend weight ramped between 0 and 1 by camera depth -- see
	// UpdateUnderwaterState. Attached to the actor so it streams with it
	// (there is exactly one ocean actor, so this never needs pooling).
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Ocean")
	TObjectPtr<UPostProcessComponent> UnderwaterPostProcess;

	// THE UNDERWATER LOOK ITSELF, as a post-process material instead of a
	// constant. Created in BeginPlay from /Game/Voxel/M_Underwater and pushed
	// into UnderwaterPostProcess->Settings.WeightedBlendables; null when that
	// asset is missing, which is a supported configuration (see BeginPlay's
	// fallback -- the old constant tint comes back and a Warning is logged).
	//
	// WHY A MATERIAL AT ALL, since the thing it replaces was three floats. The
	// three floats were measured at the OCEAN, at sea level, before the water
	// surface had any volumetric model, and they are now applied unchanged to
	// 2,049 baked lake basins standing at ~1650 m. The surface those lakes are
	// drawn with (/Game/Voxel/M_WaterVoxel, the same material the sheets, the
	// ribbons and the near-field voxels use) says one thing about what the water
	// is made of and the constant tint says another, so looking INTO a pond and
	// swimming IN it disagree. A constant cannot be made to agree: the
	// disagreement is depth-dependent and per-pixel, and a single tint has
	// neither. The material gets the camera's submerged depth every tick
	// (UVoxelWaterSubsystem::SubmergedDepthUUAtWorld) and computes extinction
	// from it.
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> UnderwaterMID;

	// Recenters OceanMesh's XY under the first player controller's camera
	// (falls back to its pawn if the camera manager isn't ready yet), and sets Z
	// to the LIVE sea surface.
	//
	// THE SNAP IS THE FINEST CELL OF THE GRID THAT WAS ACTUALLY BUILT
	// (BuiltCellUU, 1.5 m at full detail and 3.0 m at half), which replaces the
	// old 1 m FollowSnapUU and is not a tidy-up: with a ring grid
	// the snap decides where the LOD boundaries land in the world. Snap to
	// anything that is not a whole finest cell and the whole lattice lands on a
	// different sub-cell phase each time it moves, so the 2:1 stitched seams --
	// which are stitched in MESH space -- sweep across the world and the
	// vertices that were paired at a ring boundary stop being the same points
	// they were. A whole-cell snap makes the grid's world alignment a function
	// of the cell size alone.
	//
	// Z IS THE TIDE (plan B3/A4): UVoxelWaterSubsystem::SeaSurfaceZNowUU(),
	// which is the geological datum plus the QUANTISED tide offset, falling back
	// to SeaLevelZUU() when there is no water subsystem or its tide is disarmed.
	// The ocean is deliberately absent from the near-field meshing sweep, so
	// moving it is a transform and nothing re-meshes -- which is the whole
	// reason the plan can afford a full-datum tide at all.
	void UpdateFollowPlane();

	// Builds the ring grid into OceanMesh at the detail level
	// voxel.Ocean.HalfDetail asks for. Called from BeginPlay, and from Tick when
	// that cvar no longer matches BuiltHalfDetail. SAFE TO CALL TWICE: it clears
	// every existing section first, which is load-bearing rather than tidy --
	// the two detail levels do not build the same NUMBER of sections, so
	// overwriting by index would leave the last ring of the previous grid drawn
	// at its old cell size, a bright ring of stale water at the horizon. See the
	// .cpp for the stitching rule and the vertex-count arithmetic.
	void BuildOceanGrid();

	// Ramps the underwater post-process weight toward the water subsystem's
	// verdict for the camera position (UVoxelWaterSubsystem::IsUnderwaterAtWorld
	// -- NOT `camera Z < 0`, see the .cpp for the dry-cavern bug that rule
	// caused), and feeds the material its submerged depth. Logs LogVoxelEarth
	// once per above/below transition -- still the only signal available
	// without a screenshot for this branch, see the task verification notes.
	void UpdateUnderwaterState(float DeltaTime);

	// Last-known underwater state. Used for TWO different things now, and
	// keeping them separate is the point: this bool is the LOGGING edge and the
	// blend TARGET, while UnderwaterBlendWeight below is the continuous value
	// actually handed to the component. Before the blend existed they were the
	// same variable and the function early-returned when nothing had changed;
	// with a ramp the function must run every tick, so the transition log has to
	// hang off this bool's edge rather than off "we did some work this frame",
	// or it would fire once per frame for the whole time you are in the water.
	bool bUnderwater = false;

	// The value actually written to UnderwaterPostProcess->BlendWeight, ramped
	// toward 0/1 at 1/UnderwaterBlendSeconds per second.
	//
	// WHAT THIS FIXES: the weight used to snap 0 -> 1 in a single frame at the
	// waterline. Every underwater cue -- extinction, vignette, and previously a
	// whole fog actor becoming visible -- arrived at once, on one frame, which
	// reads as a shutter rather than as entering water, and is worst in exactly
	// the case that happens most (bobbing across the surface while swimming,
	// where the predicate can flip on consecutive frames).
	float UnderwaterBlendWeight = 0.f;

	// The last submerged depth pushed into the material, in METRES. Held rather
	// than recomputed during the fade-OUT: once the camera is above the surface
	// the depth query correctly answers 0, and pushing that while the weight is
	// still ramping down would collapse the extinction to nothing in one frame
	// -- reintroducing, at the exit, precisely the pop the ramp was added to
	// remove. So the depth freezes at its last submerged value and only the
	// weight moves.
	float LastSubmergedDepthM = 0.f;

	// The grid's shape lives in namespace VoxelOcean at the top of this header
	// (reach, cell size, ring derivation). What stays here is only what the
	// ACTOR needs to remember about the mesh it built.
	//
	// RETIRED WITH THE ENGINE PLANE: SourcePlaneSizeUU (the 100 UU source quad
	// that was scaled up), PlaneSizeUU (now VoxelOcean::kReachDiameterUU, the
	// same arithmetic, still asserted against the clipmap in the .cpp) and
	// FollowSnapUU (now BuiltCellUU -- see UpdateFollowPlane for why
	// the snap must be the finest CELL and not a round metre).

	// Vertices and triangles actually built, and the number of sections they
	// landed in. Diagnostics, not state: the grid is derived, so these are what
	// a log line can compare against the arithmetic in the .cpp's build comment
	// -- a ring loop that silently emitted nothing would otherwise look exactly
	// like a working one from every other symptom.
	int32 BuiltVertexCount = 0;
	int32 BuiltTriangleCount = 0;
	int32 BuiltSectionCount = 0;

	// WHAT THE MESH ON SCREEN IS ACTUALLY MADE OF, which is a different question
	// from what voxel.Ocean.HalfDetail currently says. Tick compares the two and
	// rebuilds when they differ, so this is the rebuild trigger's whole state.
	//
	// -1 MEANS "NOTHING BUILT YET", and it is not the same as 0. Initialising it
	// to 0 would make a first tick that ran before BeginPlay's build (it cannot,
	// but the invariant should not depend on that) agree with a cvar of 0 and
	// skip a build that never happened.
	int32 BuiltHalfDetail = -1;

	// The finest cell of the grid that was built, which is the recentre snap --
	// see UpdateFollowPlane for why the snap MUST be that cell and not a round
	// metre. Held rather than recomputed from BuiltHalfDetail so the snap cannot
	// disagree with the geometry even for the one tick between a cvar change and
	// the rebuild that answers it.
	double BuiltCellUU = VoxelOcean::kCellUU;

	// The sea surface the mesh was last moved to. Held so the Z push can be
	// skipped when the datum has not stepped -- the tide is quantised, so it is
	// unchanged on almost every tick, and an unchanged SetActorLocation still
	// dirties a transform.
	double LastSeaSurfaceZUU = 0.0;
	bool bHaveSeaSurfaceZ = false;

	// How long the underwater post-process takes to reach full strength.
	//
	// 0.15 s is chosen to be SHORT ENOUGH NOT TO BE A TRANSITION EFFECT and long
	// enough to cover the pop: at 60 fps it is 9 frames, at 30 fps it is 5, so
	// the cue arrives gradually at both and there is no frame rate at which it
	// degenerates back to a snap. It is deliberately not longer -- a slow fade
	// makes the water feel like a screen effect being applied to you rather than
	// a medium you moved into, and it would also lag the surface crossing badly
	// enough to be visible while swimming at the waterline.
	//
	// If this ever needs to differ between entering and exiting (entering fast,
	// exiting slower, is the usual asymmetry), split it into two constants
	// rather than averaging them.
	static constexpr float UnderwaterBlendSeconds = 0.15f;

	// The material parameter names, spelled once. These are a CONTRACT with
	// /Game/Voxel/M_Underwater (generated by ue-project/Tools/) -- a typo here
	// does not fail to compile and does not warn at runtime, it silently sets
	// nothing, so the names live in one place where they can be compared against
	// the generator. SubmergedDepthM is the only one this actor drives; see
	// BeginPlay for why the material's other three parameters
	// (UnderwaterExtinctionScale, UnderwaterAmbientGain, UnderwaterAmbientColor)
	// are deliberately left at the values the material itself authors.
	static const FName ParamSubmergedDepthM;
};
