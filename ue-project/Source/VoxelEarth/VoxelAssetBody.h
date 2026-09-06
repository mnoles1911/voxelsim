#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "VoxelAssetBody.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;

// ============================================================================
// AN ENTITY-LATTICE ASSET, RENDERED ON A MOVING ACTOR
// ============================================================================
//
// Phase D1 of docs/water-ocean-tides-plan-2026-09-04.md, and the first shipped
// path of its kind: asset-forge has been baking entity-lattice `.vxa` grids for
// months (animals, and now the plan's canoe/glider `artifact` kind) and NOTHING
// HAS EVER DRAWN ONE. The detail-asset subsystem draws the SCATTERED library --
// grass, ferns, boulders -- placed by the deterministic resolver and welded to
// the ground; entity species are structurally excluded from it (they carry
// layer 255 and never come out of instancesForRect at all,
// VoxelDetailAssetSubsystem.h). So a boat needed a renderer that takes one
// named grid and puts it on an actor that moves, which is this component, and
// it is deliberately the smallest thing that does that: a static grid, no
// animation, no LOD, no streaming. Animal rendering later grows from here.
//
// --- WHY IT IS ITS OWN LATTICE AND THAT IS NOT A BUG ------------------------
//
// ADR-0010 (docs/adr/0010-two-lattice-jurisdictions.md): the 10 / 5 / 2.5 cm
// binary ladder governs STATIC WORLD content. An entity carries its own pitch,
// never joins the world ladder, is excluded from world composition, and is
// placed as an object with its own transform. assetgrid.h says the same thing
// from the other side ("`at()` deliberately does NOT scale by [voxelSizeMm]:
// local coordinates are indices into the baked box, and what a caller does with
// a box of 10 mm voxels is a placement decision"). This component reads
// `voxelSizeMm()` and scales by it; it never assumes VoxelCoords::VoxelSizeUU,
// and a canoe baked at 25 mm renders at 25 mm whatever the world is on.
//
// --- THE READ PATH IS THE EXISTING ONE --------------------------------------
//
// `vxc::AssetGrid::parse` over bytes from FFileHelper::LoadFileToArray. That is
// the whole reader -- the same two calls VoxelWorldSubsystem.cpp:6500 makes for
// species.vxm and the same decoder vxc::AssetBankLibrary wraps. What this does
// NOT reuse is AssetBankLibrary itself, and that is a decision rather than an
// oversight: that class validates a file against its SPECIES' LAYER (height,
// depth, radius, lattice) out of a manifest, and a vehicle is not a species,
// has no layer, and is not scattered by anything. Running a canoe through those
// gates would be asking a question with no correct answer.
//
// The unrigged exclusion needs no code either. assetgrid.h: only animals carry
// rig parts, so a canoe and a glider answer `hasParts() == false`, and this
// component refuses a grid that answers true -- not because it would crash, but
// because it would render a rigged animal in its bind pose and that is the kind
// of "it rendered SOMETHING" outcome the shape rules on record keep costing us.
//
// --- MATERIALS: WE CLAMP, AND THE BANK LOADER REFUSES. BOTH ARE RIGHT -------
//
// assetgrid.h states plainly that every asset asset-forge has baked so far
// carries material ids past vxc::kMaterialCount, and that a host "must gate on
// this ... and refuse the asset otherwise". AssetBankLibrary does exactly that,
// correctly, BECAUSE IT STAMPS INTO TERRAIN: an out-of-range id there reads past
// kMineCostByMaterial, past the mips.h majority arrays, and into the mesher's
// merge key -- many arrays, all silent.
//
// This component indexes exactly ONE array, the appearance palette, for colour
// and nothing else. Nothing it produces enters the world lattice, a digest, or
// a mine cost. So it CLAMPS an unknown id to the palette's last entry, counts
// how many voxels that happened to, and logs the count loudly -- because the
// alternative, refusing the file, means the vehicle the owner asked for does not
// appear at all until the engine's material enum is appended (three known tails,
// docs/tree-asset-generator-research.md §8A). A wrongly-tinted canoe is a
// screenshot the owner can judge; an absent one is not.
//
// --- ONE ISM PER MATERIAL, WHICH IS THE PLAN'S "ONE ISM" PLUS THE ONE THING
//     THE ENGINE CANNOT DO ---------------------------------------------------
//
// The plan says one UInstancedStaticMeshComponent of engine cubes with
// per-instance flat colour. The first half is exactly what this does; the second
// half has no shipping route in this project today. Per-instance colour on an
// ISM is `PerInstanceCustomData`, which only reaches a pixel through a material
// that reads it, and NO material in ue-project/Tools authors that node -- grep
// PerInstanceCustomData across the tree and there are zero hits, in C++ and in
// Python alike. Authoring one is a Tools/ file, which this lane does not own.
//
// So instances are grouped by material id into one ISM each, and each ISM's MID
// carries the palette colour as a flat tint (VoxelProxyBody's "Color" parameter
// idiom, same best-effort caveat: a no-op if the resolved base material does not
// expose one). A vehicle uses a handful of materials, so this is a handful of
// batches for one actor -- immaterial against the draw path, and it renders
// correctly TODAY with zero new content. If a material lane ever authors an
// M_VoxelAssetBody that reads custom data, collapsing these back into one ISM is
// a small, local change and the counters below will say whether it changed
// anything.
//
// --- WHAT IT COSTS, AND THE COUNT IS THE ENGAGEMENT PROOF -------------------
//
// One cube instance per VISIBLE voxel -- the surface shell only, exactly
// AVoxelDebris::InitFromIsland's rule, because a voxel with all six face
// neighbours solid cannot be seen from any angle and drawing it is pure cost.
// A 4 m canoe hull at 25 mm is a thin shell, low tens of thousands of cells
// before the shell filter and low thousands to ~15k after. Both numbers are
// LOGGED on load together with the pitch and the resolved path, because "the
// boat is invisible" and "the boat loaded 0 voxels" and "no file was found" are
// three different diagnoses and a silent success is this project's house
// failure mode. voxel.AssetBody.MaxInstances strides the shell down uniformly
// (never a prefix -- a prefix renders the bottom slice of the hull and reads as
// half the boat missing) if a grid ever comes in heavier than expected.
//
// --- WHEN THE FILE IS NOT THERE ---------------------------------------------
//
// asset-forge is a separate program producing separate outputs on a separate
// schedule (docs: asset export is worldgen input, and it is gitignored). At the
// time this was written `library/` holds eight organic species and neither the
// canoe nor the glider. So a missing file is the EXPECTED first state, not an
// error path nobody will walk: it logs a Warning naming every path it tried and
// builds a placeholder of the same cubes at the same pitch, sized from
// FallbackSizeM. The boat then floats, wakes, beaches and is photographable
// before the art exists, and IsPlaceholder() is true so every log line and HUD
// row can say so rather than letting a grey box be mistaken for the asset.
// ============================================================================

// Which placeholder to build when the asset file is absent. Shapes, not
// decoration: the point is that a screenshot of the fallback is instantly
// distinguishable from a screenshot of the real asset, and that the fallback
// still has the right gross dimensions so buoyancy and camera framing are being
// tested against something the right size.
UENUM()
enum class EVoxelAssetBodyFallback : uint8
{
	// An open-topped tapered box: a canoe you would not photograph, with a hull
	// bottom at the right draft and a beam at the right width.
	Hull,
	// A flat plate plus a spar and a seat block: a glider-shaped cross.
	Wing,
	// A plain closed box shell. For anything that is neither.
	Box,
};

UCLASS(ClassGroup = (VoxelEarth), meta = (BlueprintSpawnableComponent))
class VOXELEARTH_API UVoxelAssetBodyComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UVoxelAssetBodyComponent();

	// --- configuration, set before Build() -----------------------------------

	// asset-forge library name, e.g. "canoe". Resolved through the search order
	// in ResolveVxaPath; empty means "placeholder only, and say so".
	UPROPERTY(EditAnywhere, Category = "Voxel Earth|Asset Body")
	FString AssetName;

	// An explicit .vxa file. WINS over AssetName when non-empty, so a leg can
	// point at a file the resolver would never find without editing anything.
	UPROPERTY(EditAnywhere, Category = "Voxel Earth|Asset Body")
	FString AssetFilePath;

	UPROPERTY(EditAnywhere, Category = "Voxel Earth|Asset Body")
	EVoxelAssetBodyFallback FallbackShape = EVoxelAssetBodyFallback::Box;

	// Gross size of the placeholder in METRES (length along +X, beam along +Y,
	// height along +Z). Ignored once a real grid loads -- the grid's own box
	// wins, always.
	UPROPERTY(EditAnywhere, Category = "Voxel Earth|Asset Body")
	FVector FallbackSizeM = FVector(4.0, 1.0, 0.5);

	// The pitch the placeholder is built at, millimetres. 25 mm is the plan's
	// entity pitch for the canoe and glider (D0), so the fallback has the same
	// visual grain as the asset it stands in for.
	UPROPERTY(EditAnywhere, Category = "Voxel Earth|Asset Body")
	int32 FallbackPitchMm = 25;

	UPROPERTY(EditAnywhere, Category = "Voxel Earth|Asset Body")
	FLinearColor PlaceholderTint = FLinearColor(0.42f, 0.28f, 0.16f, 1.f);

	// --- the one call ---------------------------------------------------------
	//
	// Resolves, loads, meshes and attaches. Returns TRUE only when a real .vxa
	// grid was decoded and drawn -- false means the placeholder is on screen, and
	// a caller that reports "loaded" on a false return is reporting the failure
	// as the success. Safe to call again (rebuilds from scratch); safe to call
	// with no file, no directory and no engine cube.
	bool Build();

	// --- what actually happened (read these, do not infer them) ---------------

	bool IsPlaceholder() const { return bPlaceholder; }
	int32 GetInstanceCount() const { return InstanceCount; }
	int32 GetBatchCount() const { return Batches.Num(); }
	// The rendered lattice pitch in UU. The GRID's own, never the world's.
	double GetPitchUU() const { return PitchUU; }
	// Axis-aligned bounds of the drawn voxels in this component's local space,
	// including the outer half-voxel skin. A vehicle uses this to place its
	// probes and its camera against the hull it actually got, so a placeholder
	// and a real asset need no separate tuning.
	FBox GetLocalBoundsUU() const { return LocalBoundsUU; }
	// Whichever file was read, or empty. Printed by every caller's load line.
	const FString& GetResolvedPath() const { return ResolvedPath; }

	// The search order, exposed so a caller's failure log can name every path
	// that was tried rather than only the one it wanted:
	//   1. AssetFilePath verbatim (absolute, or relative to the project dir)
	//   2. <root>/<name>/<name>-NNNN/tree.vxa   -- asset-forge's library layout
	//   3. <root>/<name>/<name>-NNNN.vxa        -- export_banks.py's bank layout
	//   4. <root>/<name>.vxa
	// with <root> from voxel.AssetBody.LibraryRoot, whose default is the
	// asset-forge library beside the UE project. Returns the first path that
	// exists, or empty.
	static FString ResolveVxaPath(const FString& Name, const FString& ExplicitPath,
	                              TArray<FString>& OutTried);

private:
	void ClearBatches();
	// One cube instance per VISIBLE voxel of a dense material box (z fastest,
	// 0 = air). Shared by the grid path and the placeholder path so both get the
	// same shell filter, the same stride budget, the same batching and the same
	// bounds -- a fallback that measured differently from the asset would make
	// every buoyancy number tuned against it wrong twice.
	//
	// FlatTint non-null overrides the palette for every voxel, which is what
	// makes a placeholder read as a placeholder.
	void EmitDense(const TArray<uint8>& Mat, int32 SX, int32 SY, int32 SZ,
	               const FIntVector& OriginVoxels, double InPitchUU, const FLinearColor* FlatTint);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Batches;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CubeMesh;

	double PitchUU = 0.0;
	int32 InstanceCount = 0;
	bool bPlaceholder = true;
	FString ResolvedPath;
	FBox LocalBoundsUU = FBox(ForceInit);
};
