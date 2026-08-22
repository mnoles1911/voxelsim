// TASK #7: render the invisible 85% -- detail-lattice (L3) ground cover as
// instanced static meshes.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS, IN ONE PARAGRAPH
// ---------------------------------------------------------------------------
//
// voxel-core already resolves every grass tuft, fern, flower, reed and small
// rock deterministically (vxc::AssetField::instancesForRect with
// terrainOnly=false) -- through the same biome/elevation/slope/anchor gates as
// the trees -- and until this file existed nothing consumed the answer
// (docs/asset-placement-audit.md section 5.3: 85% of all instances, 266
// species, invisible). This subsystem walks a ring of 2x2 level-0 chunk
// groups around the streaming anchor, resolves each group's detail instances
// ON A WORKER (the same vxc::Amplifier::column the meshing workers already
// call concurrently -- never the render thread, never the game thread),
// converts each (species, seed) grid ONCE into a small vertex-coloured static
// mesh (naive visible-face quads: the grids are 100-5000 voxels and per-voxel
// colour jitter would defeat greedy merging anyway), and draws the instances
// through one HISM component per (species, seed), transformed by anchor
// position + yawQuarter.
//
// ---------------------------------------------------------------------------
// WHAT THIS DELIBERATELY DOES NOT DO
// ---------------------------------------------------------------------------
//
//  * NO ANIMALS (owner: they need animation first). Structural, not a filter:
//    fish/bird/quadruped/cetacean species carry layer 255
//    (vxc::kAssetLayerNotScattered), never enter the folded species table, and
//    therefore can never come out of instancesForRect. The worker still skips
//    any grid with rig parts (hasParts()) so the exclusion holds even if a
//    plant were ever baked with a rig by mistake.
//  * NO PLACEMENT LOGIC. Same seed + tiles => same instances, because this
//    file only consumes the resolver. It computes nothing about where
//    anything stands.
//  * NO WORLD-LATTICE VOXELS, NO COLLISION, NO EDITS. Detail grids never
//    enter the world grid by design (vxc::AssetGrid::onTerrainLattice() is
//    false for them, and the streaming bounds deliberately exclude detail
//    layers -- assetplacement.h). Presentation only: worldgen digests,
//    admission bounds and multiplayer determinism are untouched by
//    construction. Ground cover is walk-through (the same "no Chaos for
//    terrain" doctrine; a grass tuft with a collision body would also be a
//    desync risk, since clients do not agree on WHEN cover is resolved).
//  * NO REACTION TO DIG/PLACE EDITS (v1): a dug-out column keeps its resolved
//    cover until the group leaves the ring and re-resolves. Terrain-lattice
//    assets get this right through the overlay; cover accepting the same
//    staleness window as its ring is the v1 trade, recorded in
//    docs/detail-asset-rendering.md.
//
// ---------------------------------------------------------------------------
// THREADING, AND WHY IT IS SOUND
// ---------------------------------------------------------------------------
//
// The worker calls exactly what the level-0 meshing jobs already call from
// many threads at once: Amplifier::column (safe -- see VoxelFineTileStreamer.h
// "THREADING" note: resident tiles are fully decoded at load and the tile map
// is rwlocked) and IAssetBankSource::bankGrid (loads serialised under the
// library's own mutex; returned grids are immutable for the process
// lifetime). The one rule that makes worker column queries legal is the
// fine-tier residency gate: a worker query into a non-resident fine tile is a
// GATE LEAK and stops an unattended run. So a group is only dispatched after
// IsFootprintResident over its rect dilated by the layer table's maximum
// reach -- the same shape FootprintChunkZRangeCached uses for exact
// admission, with the dilation taken over ALL layers because
// instancesForRect(terrainOnly=false) evaluates terrain-layer anchors too.
//
// Job lifetime: every launched task is tracked and waited on in
// Deinitialize(), and Initialize() declares InitializeDependency on
// UVoxelWorldSubsystem, so this subsystem deinitializes FIRST and the
// borrowed AssetField/Amplifier/bank pointers outlive every job by
// construction.
//
// ---------------------------------------------------------------------------
// COLOUR
// ---------------------------------------------------------------------------
//
// One flat colour per voxel face from vxc::kMaterialPalette, face-classed
// top/side/bottom, varied by vxc::voxelTint -- the same table AND the same
// evaluation the terrain's asset voxels are shaded from, which is newer and
// less obvious than it sounds. This path used to carry its own formula: the
// lightness jitter at 0.35 of the authored amount (a factor with no derivation
// anywhere), no hue drift, and no patch term at all. The missing patch term is
// the one that mattered -- it is half of ADR-0008 invariant 3, it is the half
// that survives distance, and this path is 85% of every placement in the world,
// so a hillside of cover flattened to one grey at exactly the range where the
// variation was doing the most work.
//
// The tint is hashed from the voxel's LOCAL coordinates, salted with the
// (species, seed) identity: deterministic per mesh, so instances of one seed
// are identical exactly as terrain-lattice trees are, while two different seeds
// do not share a dither. Colours are authored sRGB; they are handed
// to the mesh build as LINEAR floats because UStaticMesh::BuildFromMeshDescription
// re-encodes vertex colours with ToFColor(true) (verified in the 5.8 source),
// and the GPU reads the colour buffer as raw UNORM -- so the material
// (M_VoxelDetailAsset, Tools/create_detail_asset_material.py) applies the
// sRGB decode. If that asset is missing this renders grey cover with correct
// shapes and says so loudly, never crashes.
//
// See docs/detail-asset-rendering.md for the architecture write-up and what a
// capture must show.

#include "VoxelDetailAssetSubsystem.h"

#include "VoxelCoords.h"
#include "VoxelDebug.h" // VoxelDebug::kHitchThresholdMs -- the adaptive budgets' bar
#include "VoxelEarth.h"
#include "VoxelFineTileStreamer.h"
#include "VoxelWorldSubsystem.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "MeshDescription.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "StaticMeshAttributes.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"

// voxel-core: allowed here (this .cpp is not UHT-parsed), same doctrine as
// VoxelWorldSubsystem.cpp.
#include "voxelcore/amplifier.h"
#include "voxelcore/assetfield.h"
#include "voxelcore/assetgrid.h"
#include "voxelcore/assetplacement.h"
#include "voxelcore/assetpolicy.h"
#include "voxelcore/core.h"
#include "voxelcore/materialcolor.h"
#include "voxelcore/materialpalette.h"

#include <atomic>

namespace
{
// One 2x2 block of level-0 chunks: 64 voxels = 6.4 m on a side. Small enough
// that a group's resolve is ~100 amplifier columns (a level-0 mesh job pays
// ~1,150), large enough that the ring is ~1,000 groups rather than ~4,000.
constexpr int32 kGroupEdgeChunks = 2;
constexpr int32 kGroupEdgeVoxels = kGroupEdgeChunks * VoxelCoords::ChunkEdgeVoxels; // 64
constexpr double kGroupEdgeUU = double(kGroupEdgeVoxels) * VoxelCoords::VoxelSizeUU; // 640

// 112 -> 256 m, 2026-08-18 (owner: "extend the detail ring"). The old value
// sat inside R0's 128 m level-0 ring so every group stood on already-admitted
// ground; past it, groups can land on ground the streamer has not admitted
// yet -- which is SAFE here and always was, because dispatch is gated on
// IsFootprintResident (dilated by the full layer reach) and a group outside
// residency simply defers to a later tick instead of dispatching. The gate is
// the invariant; 128 m was belt-and-braces.
//
// Cost: instance count scales with area, 5.2x at this radius -- but the same
// day cut L3 density 1000 -> 300 per-mille (the owner walked into ground cover
// that blocked movement), so the net is ~1.6x the old resident count for 2.3x
// the visible radius. That pairing is the point: THINNER, but visible much
// further, which is what "the slopes look bare from the vista" actually was --
// 94% of a hillside's vegetation is detail-lattice and used to vanish at 112 m.
constexpr double kDefaultRingMeters = 256.0;

// Hysteresis on release, same shape as UVoxelWorldSubsystem's
// UnloadRingMultiplier: a group loads inside RingMeters and unloads past
// RingMeters * this, so the boundary does not thrash under small anchor
// motion.
constexpr double kUnloadMultiplier = 1.15;

// Per-tick budgets, ADAPTIVE since 2026-08-18 (the ten-minute first-arrival
// debt: 5,028 groups / 311 first-encounter meshes at the alpine lake pushed
// capture settle from ~350 s to 591-656 s under the old fixed 4/8/4/4).
//
// The original hitch reasoning still stands and is preserved, just made
// proportional: mesh builds and instance-table rebuilds run on the game
// thread and are the ones that could hitch a frame, so they are governed by a
// measured TIME budget per tick rather than a count (grids span 100-5000
// voxels; four big builds and four small ones are very different frames).
// Resolve jobs run on background workers and are bounded by an in-flight
// count (worker-side cost -- they share the task pool with the level-0
// meshing jobs).
//
// Both budgets scale with the headroom the LAST frame had against
// VoxelDebug::kHitchThresholdMs (the same 33.3 ms bar the "Hitch frame" log
// fires on): a fast frame gets the max values, a frame at 85% of the
// threshold gets the min values, linear in between. Convergence therefore
// runs hard exactly when the frame can afford it and backs off to roughly
// the old fixed budgets when it cannot -- it never ADDS a hitch to a frame
// already near the bar.
constexpr int32 kMinJobsInFlight = 4;        // the old fixed cap; floor under load
constexpr int32 kMaxJobsInFlight = 24;       // ceiling with full frame headroom
constexpr int32 kMaxDispatchPerTick = 32;    // refill limit per tick
// Game-thread time budget shared by mesh builds and HISM rebuilds each tick.
// 6 ms fits inside a 16.6 ms frame's idle time (the game thread idles ~75% at
// 2K -- voxelsim-draw-path-2k); 1.5 ms is roughly what the old 4-mesh budget
// spent on typical grids, so the busy floor is the old behaviour.
constexpr double kMinBuildBudgetMs = 1.5;
constexpr double kMaxBuildBudgetMs = 6.0;
// Hard ceilings under the time budget so a pathological run of tiny grids
// cannot spin the loops unboundedly inside one tick.
constexpr int32 kMaxMeshBuildsPerTick = 64;
constexpr int32 kMaxRebuildsPerTick = 64;
// Headroom window, as fractions of the hitch threshold: at or below 60%
// (20 ms) the frame is healthy and budgets are maxed; at or above 85%
// (28.3 ms) budgets are floored.
constexpr double kHealthyFrameFrac = 0.60;
constexpr double kBusyFrameFrac = 0.85;

// Refuse to dense-decode an absurd grid (nothing baked approaches this; the
// detail library is 100-5000 voxels per grid).
constexpr int64 kMaxGridCells = 8 * 1024 * 1024;

// (bankId << 16) | seedIndex -- the identity of one baked grid, and therefore
// of one static mesh and one HISM batch.
FORCEINLINE uint32 MakeMeshKey(uint16 BankId, uint16 SeedIndex)
{
	return (uint32(BankId) << 16) | uint32(SeedIndex);
}

struct FGroupKey
{
	int64 X = 0;
	int64 Y = 0;
	friend bool operator==(const FGroupKey&, const FGroupKey&) = default;
	friend uint32 GetTypeHash(const FGroupKey& K)
	{
		return HashCombine(::GetTypeHash(K.X), ::GetTypeHash(K.Y));
	}
};

// One resolved detail instance, in world space. Doubles deliberately: at
// -400 km from the origin a float has ~3 cm of resolution, and the HISM
// instance buffer's float precision is recovered by giving each component a
// nearby world origin and storing instances relative to it (see
// FMeshEntry::OriginUU).
struct FDetailInstanceRec
{
	uint32 MeshKey = 0;
	FVector3d PosUU = FVector3d::ZeroVector;
	uint8 YawQuarter = 0;
};

// Worker-built geometry for one (species, seed): parallel per-vertex arrays +
// an index list, converted to a MeshDescription on the game thread. Built off
// the game thread because the face walk over a few thousand voxels is the
// expensive half; the MeshDescription fill is a linear copy.
struct FMeshGeometry
{
	uint32 MeshKey = 0;
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> TangentsX;
	TArray<FVector4f> Colors; // LINEAR floats -- see the colour note up top
	TArray<FVector2f> UVs;
	TArray<uint32> Indices;
	uint64 SolidVoxels = 0;
};

struct FGroupResult
{
	FGroupKey Group;
	TArray<FDetailInstanceRec> Instances;
	TArray<TUniquePtr<FMeshGeometry>> NewGeometry;
	int32 SitesTotal = 0;      // everything instancesForRect resolved
	int32 DetailKept = 0;      // detail-lattice instances anchored in this group
	int32 BankMisses = 0;      // detail instances whose grid the library refused
};

// Everything a resolve job needs, captured by value at dispatch. The three
// borrowed pointers outlive every job -- Deinitialize() waits on all tasks
// and runs before UVoxelWorldSubsystem's own teardown (InitializeDependency).
struct FResolveJobInput
{
	const vxc::AssetField* Field = nullptr;
	const vxc::Amplifier* Amp = nullptr;
	const vxc::IAssetBankSource* Banks = nullptr;
	// The engine's ONE channel binding (UVoxelWorldSubsystem::
	// GetAssetChannelSource) -- ground cover must gate on the same water
	// distance / standing water / treeline the tree meshers gate on, or reeds
	// resolve where no lake is and grass resolves under it. Null (no fine
	// tier) means sentinel channels: fail-closed, the pre-channel world.
	// Thread-safe from workers; internally serialized.
	vxc::IAssetChannelSource* Channels = nullptr;
	FGroupKey Group;
	vxc::AssetVoxelRect Rect;
	// MeshKeys whose geometry the game thread already has (or has in flight
	// from an earlier job). A stale snapshot only means a duplicate geometry
	// build, which the drain discards -- never a missing one: the first job
	// to see a key always carries its geometry.
	TSet<uint32> KnownGeometry;
};

// sRGB palette -> linear, once, all face classes and all materials. The
// bank loader guarantees every material id in a served grid is
// < vxc::kMaterialCount (materialsWithinEngine is a load-time refusal), so
// indexing this table with a served grid's bytes cannot go out of range.
//
// ONLY THE BASE COLOURS ARE CACHED HERE. The variation is not a table any
// more: vxc::voxelTint reads the appearance itself, and the one thing that has
// to happen on this side is the sRGB decode, which voxel-core cannot do because
// it has no floats.
struct FPaletteLinear
{
	FLinearColor Face[vxc::kMaterialCount][vxc::kFaceClassCount];
	FPaletteLinear()
	{
		for (uint32 M = 0; M < uint32(vxc::kMaterialCount); ++M)
		{
			const vxc::MaterialAppearance& A = vxc::kMaterialPalette[M];
			for (uint32 F = 0; F < uint32(vxc::kFaceClassCount); ++F)
			{
				// FLinearColor(FColor) is the exact sRGB decode.
				Face[M][F] = FLinearColor(FColor(A.face[F].r, A.face[F].g, A.face[F].b));
			}
		}
	}
};

const FPaletteLinear& PaletteLinear()
{
	static const FPaletteLinear P; // thread-safe magic-static init
	return P;
}

// Naive visible-face quads over one baked grid. NOT the greedy mesher, on
// purpose: vxc::meshBrick is shaped around 8^3 bricks with a sampler apron,
// and -- decisive -- a greedy quad must be attribute-uniform, while the look
// doctrine (materialpalette.h) wants per-voxel colour jitter, which makes
// almost every merge illegal anyway. At 100-5000 voxels per grid the naive
// walk emits a few thousand faces and is nowhere near any budget.
//
// Winding and corner order copied from FVoxelChunkSceneProxy's proven
// convention (VoxelChunkComponent.cpp, the 2026-07-21 winding fix): corners
// (u0,v0)->(u0,v1)->(u1,v1)->(u1,v0) with U=(Axis+1)%3, V=(Axis+2)%3;
// positive faces index (0,1,2)(0,2,3), negative faces (0,2,1)(0,3,2).
void BuildNaiveFaceGeometry(const vxc::AssetGrid& Grid, uint32 MeshKey, FMeshGeometry& Out)
{
	const int32 SX = Grid.sizeX(), SY = Grid.sizeY(), SZ = Grid.sizeZ();
	const int64 Cells = int64(SX) * int64(SY) * int64(SZ);
	if (Cells <= 0 || Cells > kMaxGridCells)
	{
		return;
	}

	// Dense material grid, z fastest (matches columnRuns' delivery order).
	TArray<uint8> Mat;
	Mat.SetNumZeroed(int32(Cells));
	uint64 Solid = 0;
	for (int32 X = 0; X < SX; ++X)
	{
		for (int32 Y = 0; Y < SY; ++Y)
		{
			const int64 Base = (int64(X) * SY + Y) * SZ;
			Grid.columnRuns(X, Y,
			                [&](int32 Z0, int32 Len, vxc::MaterialId M)
			                {
				                if (M == vxc::MAT_AIR)
				                {
					                return;
				                }
				                Solid += uint64(Len);
				                for (int32 Z = Z0; Z < Z0 + Len; ++Z)
				                {
					                Mat[int32(Base + Z)] = uint8(M);
				                }
			                });
		}
	}
	Out.SolidVoxels = Solid;
	if (Solid == 0)
	{
		return;
	}

	auto MatAt = [&](int32 X, int32 Y, int32 Z) -> uint8
	{
		if (X < 0 || Y < 0 || Z < 0 || X >= SX || Y >= SY || Z >= SZ)
		{
			return 0;
		}
		return Mat[int32((int64(X) * SY + Y) * SZ + Z)];
	};

	// THE DETAIL GRID'S OWN PITCH, not the world's. assetgrid.h: "Read it
	// before placing anything ... at() deliberately does NOT scale by it."
	// This is the scale read; a 5 cm tuft renders at 5 cm.
	const float PitchUU = float(double(Grid.voxelSizeMm()) * 0.1);
	// The same pitch the geometry is built at, in the millimetres vxc::voxelTint
	// wants -- see the variation block below for why it must be the GRID's and
	// not the world's.
	const int32 PitchMm = int32(Grid.voxelSizeMm());
	const int32 Origin[3] = {Grid.originX(), Grid.originY(), Grid.originZ()};

	const FPaletteLinear& Pal = PaletteLinear();
	// Odd, so a MeshKey of 0 (bank 0, seed 0 -- a real combination) still salts.
	const uint32 TintSalt = (MeshKey << 1) | 1u;

	static const FVector3f AxisDir[3] = {FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1)};

	// Rough reserve: ~4.5 visible faces per solid voxel is a generous upper
	// bound for sparse foliage; TArray growth handles the rest.
	const int32 ReserveFaces = int32(FMath::Min<uint64>(Solid * 5u, 200000u));
	Out.Positions.Reserve(ReserveFaces * 4);
	Out.Normals.Reserve(ReserveFaces * 4);
	Out.TangentsX.Reserve(ReserveFaces * 4);
	Out.Colors.Reserve(ReserveFaces * 4);
	Out.UVs.Reserve(ReserveFaces * 4);
	Out.Indices.Reserve(ReserveFaces * 6);

	int32 Cell[3];
	for (Cell[0] = 0; Cell[0] < SX; ++Cell[0])
	{
		for (Cell[1] = 0; Cell[1] < SY; ++Cell[1])
		{
			for (Cell[2] = 0; Cell[2] < SZ; ++Cell[2])
			{
				const uint8 M = MatAt(Cell[0], Cell[1], Cell[2]);
				if (M == 0)
				{
					continue;
				}

				// The variation, from vxc::voxelTint -- the ONE definition, the
				// same one VoxelMaterialPalette.ush evaluates for a terrain or
				// asset voxel and forge/render.py mirrors for a preview.
				//
				// THIS USED TO BE A LOCAL FORMULA AND IT WAS WRONG THREE WAYS,
				// which is worth spelling out because all three still rendered
				// and none of them looked like a bug. It applied the lightness
				// jitter at 0.35 of the authored amount -- a factor with no
				// derivation anywhere; it dropped the hue drift entirely; and it
				// dropped the PATCH TERM, which is half of ADR-0008 invariant 3
				// and the half that survives distance. A field of cover with no
				// patch term flattens to one grey at exactly the range where the
				// variation is doing the most work, and this path is 85% of all
				// placements in the world.
				//
				// THE PITCH, NOT THE WORLD'S. A detail grid carries its own,
				// typically 5 cm, and the patch wavelength is world-metric
				// (materialpalette.h, patchScaleDm) -- so passing the grid's
				// pitch is what makes a tuft's mottle the same physical size as
				// the hillside's behind it. Passing 100 here would stretch it to
				// double scale on every 5 cm asset in the library.
				//
				// THE SALT IS THE (species, seed) IDENTITY, because every grid
				// starts at its own local origin: without it every tuft of grass
				// in the world would carry the identical dither and a meadow
				// would read as a repeating texture.
				const vxc::VoxelTint Tint = vxc::voxelTint(
					vxc::MaterialId(M), Cell[0], Cell[1], Cell[2], PitchMm, TintSalt);

				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					for (int32 Positive = 0; Positive < 2; ++Positive)
					{
						int32 N[3] = {Cell[0], Cell[1], Cell[2]};
						N[Axis] += Positive ? 1 : -1;
						if (MatAt(N[0], N[1], N[2]) != 0)
						{
							continue; // face culled against a solid neighbour
						}

						const int32 U = (Axis + 1) % 3;
						const int32 V = (Axis + 2) % 3;
						const vxc::FaceClass FC =
							(Axis == 2) ? (Positive ? vxc::kFaceTop : vxc::kFaceBottom)
							            : vxc::kFaceSide;
						// Applied through vxc::applyTintQ16 rather than by
						// three lines here, so the ORDER (lightness on all
						// three, then the warm/cool tilt on red and blue) has
						// one definition too. Q16 resolves to 1/65536, far finer
						// than the 8-bit buffer this ends up in.
						const FLinearColor& Base = Pal.Face[M][FC];
						int32 Q16[3] = {
							int32(Base.R * float(vxc::kColorOne)),
							int32(Base.G * float(vxc::kColorOne)),
							int32(Base.B * float(vxc::kColorOne)),
						};
						vxc::applyTintQ16(Q16, Tint);
						FLinearColor C;
						C.R = FMath::Clamp(float(Q16[0]) / float(vxc::kColorOne), 0.0f, 1.0f);
						C.G = FMath::Clamp(float(Q16[1]) / float(vxc::kColorOne), 0.0f, 1.0f);
						C.B = FMath::Clamp(float(Q16[2]) / float(vxc::kColorOne), 0.0f, 1.0f);
						C.A = 1.0f;

						const float FaceCoord = float(Cell[Axis] + Positive);
						const float U0 = float(Cell[U]), U1 = float(Cell[U] + 1);
						const float V0 = float(Cell[V]), V1 = float(Cell[V] + 1);
						const float CornerU[4] = {U0, U0, U1, U1};
						const float CornerV[4] = {V0, V1, V1, V0};

						const FVector3f Normal = AxisDir[Axis] * (Positive ? 1.0f : -1.0f);
						const FVector3f TangentX = AxisDir[U];
						const uint32 Base = uint32(Out.Positions.Num());

						for (int32 Corner = 0; Corner < 4; ++Corner)
						{
							FVector3f P;
							P[Axis] = (FaceCoord + float(Origin[Axis])) * PitchUU;
							P[U] = (CornerU[Corner] + float(Origin[U])) * PitchUU;
							P[V] = (CornerV[Corner] + float(Origin[V])) * PitchUU;
							Out.Positions.Add(P);
							Out.Normals.Add(Normal);
							Out.TangentsX.Add(TangentX);
							Out.Colors.Add(FVector4f(C.R, C.G, C.B, 1.0f));
							// The material samples no texture; a stable planar
							// UV keeps every downstream assumption (non-zero
							// UV channel, finite derivatives) honest.
							Out.UVs.Add(FVector2f(CornerU[Corner], CornerV[Corner]) *
							            (PitchUU / 100.0f));
						}

						if (Positive)
						{
							Out.Indices.Append({Base + 0, Base + 1, Base + 2, Base + 0, Base + 2, Base + 3});
						}
						else
						{
							Out.Indices.Append({Base + 0, Base + 2, Base + 1, Base + 0, Base + 3, Base + 2});
						}
					}
				}
			}
		}
	}
}

// The worker body: resolve one group's sites (terrain AND detail -- the
// resolver has no detail-only mode, and terrain sites are the cheap 15%),
// keep the detail-lattice instances ANCHORED IN THIS GROUP (instancesForRect
// dilates by layer reach, so an instance can be returned by up to four
// neighbouring groups; anchor ownership is the dedup), and build geometry for
// any (species, seed) the game thread did not know at dispatch.
FGroupResult RunResolveJob(const FResolveJobInput& In)
{
	FGroupResult R;
	R.Group = In.Group;

	const std::vector<vxc::AssetInstance> Insts = In.Field->instancesForRect(
		In.Rect,
		[Amp = In.Amp, Ch = In.Channels](int64_t Vx, int64_t Vy)
		{
			// Channel-sourced facts, same binding as the terrain meshers; a
			// null source composes the sentinel (fail-closed) channels.
			return vxc::assetColumnFactsFromSample(
				Amp->column(Vx, Vy),
				Ch != nullptr ? Ch->channelsAt(Vx, Vy) : vxc::AssetColumnChannels{});
		},
		/*terrainOnly*/ false);
	R.SitesTotal = int32(Insts.size());

	const std::vector<vxc::AssetLayer>& Layers = In.Field->layers();
	TSet<uint32> BuiltHere;

	for (const vxc::AssetInstance& Inst : Insts)
	{
		if (size_t(Inst.layer) >= Layers.size() || Layers[Inst.layer].terrainLattice)
		{
			continue; // terrain-lattice: composed into world voxels already
		}
		const int64 AnchorVx = vxc::floorDiv(Inst.anchorXMm, int64(vxc::kVoxelSizeMm));
		const int64 AnchorVy = vxc::floorDiv(Inst.anchorYMm, int64(vxc::kVoxelSizeMm));
		if (AnchorVx < In.Rect.vx0 || AnchorVx > In.Rect.vx1 || AnchorVy < In.Rect.vy0 ||
		    AnchorVy > In.Rect.vy1)
		{
			continue; // owned by a neighbouring group
		}

		const vxc::AssetGrid* Grid = In.Banks->bankGrid(Inst.bankId, Inst.seedIndex);
		if (Grid == nullptr || !Grid->valid())
		{
			++R.BankMisses; // refused/missing bank: composes as nothing, counted
			continue;
		}
		if (Grid->onTerrainLattice())
		{
			continue; // category guard: a world-lattice grid is not ours to draw
		}
		if (Grid->hasParts())
		{
			continue; // animals are rig-parted and EXCLUDED (owner decision)
		}

		FDetailInstanceRec Rec;
		Rec.MeshKey = MakeMeshKey(Inst.bankId, Inst.seedIndex);
		// Anchor point (the jittered in-cell position), standing on the TOP
		// surface of the verified-solid anchor voxel. The terrain-lattice
		// convention (base slab shares the ground voxel) would bury a 5 cm
		// tuft entirely inside the 10 cm ground cube; cover sits ON the
		// ground it resolved against.
		Rec.PosUU.X = double(Inst.anchorXMm) * 0.1;
		Rec.PosUU.Y = double(Inst.anchorYMm) * 0.1;
		Rec.PosUU.Z = double(Inst.anchorVz + 1) * VoxelCoords::VoxelSizeUU;
		Rec.YawQuarter = Inst.yawQuarter & 3u;
		R.Instances.Add(Rec);
		++R.DetailKept;

		if (!In.KnownGeometry.Contains(Rec.MeshKey) && !BuiltHere.Contains(Rec.MeshKey))
		{
			TUniquePtr<FMeshGeometry> G = MakeUnique<FMeshGeometry>();
			G->MeshKey = Rec.MeshKey;
			BuildNaiveFaceGeometry(*Grid, Rec.MeshKey, *G);
			if (G->Indices.Num() > 0)
			{
				R.NewGeometry.Add(MoveTemp(G));
				BuiltHere.Add(Rec.MeshKey);
			}
		}
	}
	return R;
}

} // namespace

// ---------------------------------------------------------------------------
// The PImpl
// ---------------------------------------------------------------------------

struct FVoxelDetailAssetImpl
{
	// Config, resolved once in Initialize.
	double RingMeters = kDefaultRingMeters;
	bool bDisabled = false;
	bool bCastShadow = false;

	// Set true once the asset field was seen installed and the owner actor +
	// material exist. Until then Tick idles cheaply.
	bool bStarted = false;
	bool bMaterialFallbackWarned = false;

	// Cached once at start (immutable for the session): which layers are
	// detail, and the widest reach any layer dilates a rect by (residency
	// gate must cover the columns the resolve will actually touch).
	int64 MaxReachMm = 0;

	struct FGroupRecord
	{
		TArray<FDetailInstanceRec> Instances;
	};

	struct FMeshEntry
	{
		UStaticMesh* Mesh = nullptr; // GC-rooted via the subsystem's BuiltMeshes
		UHierarchicalInstancedStaticMeshComponent* Hism = nullptr; // rooted via HismComponents
		// Component world origin: instances are stored in the HISM buffer
		// relative to this (float precision at planet coordinates -- see
		// FDetailInstanceRec). Snapped to the voxel grid purely for tidiness.
		FVector3d OriginUU = FVector3d::ZeroVector;
		// Full rebuild wanted (mesh newly built, or a contributing group was
		// released -- HISM instance removal reshuffles ids, so release is a
		// budgeted rebuild-from-truth rather than index bookkeeping).
		bool bDirty = false;
		uint64 SolidVoxels = 0;
	};

	// Game-thread state. Groups are the single source of truth for what is
	// resolved; KeyGroups is the reverse index a rebuild reads.
	TSet<FGroupKey> InFlight;
	TMap<FGroupKey, FGroupRecord> Groups;
	TMap<uint32, TSet<FGroupKey>> KeyGroups;
	TSet<uint32> GeometryKnown;                      // geometry seen (built or pending)
	TMap<uint32, TUniquePtr<FMeshGeometry>> PendingGeometry; // awaiting budgeted mesh build
	TMap<uint32, FMeshEntry> Meshes;

	// Worker plumbing.
	TQueue<TUniquePtr<FGroupResult>, EQueueMode::Mpsc> Results;
	TArray<UE::Tasks::TTask<void>> Tasks;

	// Telemetry.
	uint64 StatGroupsResolved = 0;
	uint64 StatInstancesLive = 0;
	uint64 StatMeshesBuilt = 0;
	uint64 StatBankMisses = 0;
	uint64 StatSitesResolved = 0;
	double LogTimer = 0.0;
	bool bFirstApplyLogged = false;

	// Convergence telemetry (2026-08-18). The convergence curve was previously
	// unmeasurable from the log -- the only signal was the capture harness's
	// settle time. These make it a first-class measurement:
	//  * StartTimeSeconds anchors time-to-first-full-ring;
	//  * bConvergedLogged latches the one CONVERGED line (first full ring:
	//    every group in the ring resolved, every encountered mesh built, no
	//    dirty HISM rebuilds outstanding);
	//  * the Window* pair measure mesh-build throughput per progress window;
	//  * StatPendingGroups is the dispatch step's last candidate count (groups
	//    inside the ring not yet resolved or in flight -- includes groups
	//    deferred by the residency gate).
	double StartTimeSeconds = 0.0;
	bool bConvergedLogged = false;
	double ProgressLogTimer = 0.0;
	uint64 WindowMeshBuilds = 0;
	double WindowBuildMs = 0.0;
	double StatBuildMsTotal = 0.0;
	int32 StatPendingGroups = 0;

	void WaitForAllJobs()
	{
		for (UE::Tasks::TTask<void>& T : Tasks)
		{
			T.Wait();
		}
		Tasks.Reset();
		// Drain and drop anything the jobs produced after the last tick.
		TUniquePtr<FGroupResult> R;
		while (Results.Dequeue(R))
		{
			R.Reset();
		}
	}
};

// ---------------------------------------------------------------------------
// Subsystem boilerplate
// ---------------------------------------------------------------------------

UVoxelDetailAssetSubsystem::UVoxelDetailAssetSubsystem() = default;
UVoxelDetailAssetSubsystem::~UVoxelDetailAssetSubsystem() = default;
UVoxelDetailAssetSubsystem::UVoxelDetailAssetSubsystem(FVTableHelper& Helper)
	: Super(Helper)
{
}

bool UVoxelDetailAssetSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same scope as UVoxelWorldSubsystem: game worlds only.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE ||
	       WorldType == EWorldType::GamePreview;
}

TStatId UVoxelDetailAssetSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelDetailAssetSubsystem, STATGROUP_Tickables);
}

void UVoxelDetailAssetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	// ORDERING, AND IT IS LOAD-BEARING: this makes UVoxelWorldSubsystem
	// initialize BEFORE this subsystem, and therefore deinitialize AFTER it --
	// so Deinitialize()'s WaitForAllJobs() always runs while the AssetField,
	// Amplifier and bank library the jobs borrow are still alive.
	Collection.InitializeDependency(UVoxelWorldSubsystem::StaticClass());
	Super::Initialize(Collection);

	Impl = MakeUnique<FVoxelDetailAssetImpl>();

	Impl->bDisabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelNoDetailAssets"));
	Impl->bCastShadow = FParse::Param(FCommandLine::Get(), TEXT("VoxelDetailShadows"));
	double Ring = kDefaultRingMeters;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelDetailRingMeters="), Ring))
	{
		Impl->RingMeters = FMath::Clamp(Ring, 16.0, 512.0);
	}
}

void UVoxelDetailAssetSubsystem::Deinitialize()
{
	if (Impl)
	{
		Impl->WaitForAllJobs();
		Impl.Reset();
	}
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// Game-thread helpers
// ---------------------------------------------------------------------------

namespace
{
// The engine-facing mesh from worker geometry. bFastBuild is the documented
// runtime path (BuildFromMeshDescriptions asserts it in non-editor builds and
// takes the direct render-data route in editor builds); no source model, no
// DDC, no collision.
UStaticMesh* CreateDetailStaticMesh(const FMeshGeometry& G, UMaterialInterface* Material)
{
	FMeshDescription MeshDesc;
	FStaticMeshAttributes Attributes(MeshDesc);
	Attributes.Register();

	const FPolygonGroupID PolyGroup = MeshDesc.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[PolyGroup] = FName(TEXT("Detail"));

	const int32 NumVerts = G.Positions.Num();
	const int32 NumTris = G.Indices.Num() / 3;
	MeshDesc.ReserveNewVertices(NumVerts);
	MeshDesc.ReserveNewVertexInstances(G.Indices.Num());
	MeshDesc.ReserveNewTriangles(NumTris);

	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> InstTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<FVector4f> InstColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> InstUVs = Attributes.GetVertexInstanceUVs();

	TArray<FVertexID> VertexIds;
	VertexIds.Reserve(NumVerts);
	for (int32 I = 0; I < NumVerts; ++I)
	{
		const FVertexID V = MeshDesc.CreateVertex();
		Positions[V] = G.Positions[I];
		VertexIds.Add(V);
	}

	for (int32 T = 0; T < NumTris; ++T)
	{
		FVertexInstanceID Corner[3];
		for (int32 C = 0; C < 3; ++C)
		{
			const uint32 SrcVert = G.Indices[T * 3 + C];
			const FVertexInstanceID VI = MeshDesc.CreateVertexInstance(VertexIds[int32(SrcVert)]);
			InstNormals[VI] = G.Normals[int32(SrcVert)];
			InstTangents[VI] = G.TangentsX[int32(SrcVert)];
			InstColors[VI] = G.Colors[int32(SrcVert)];
			InstUVs[VI] = G.UVs[int32(SrcVert)];
			Corner[C] = VI;
		}
		MeshDesc.CreateTriangle(PolyGroup, Corner);
	}

	UStaticMesh* Mesh = NewObject<UStaticMesh>(
		GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(),
		                     *FString::Printf(TEXT("VoxelDetail_%08x"), G.MeshKey)),
		RF_Transient);
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, FName(TEXT("Detail"))));

	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty = false;
	Params.bBuildSimpleCollision = false;
	Params.bAllowCpuAccess = false;

	TArray<const FMeshDescription*> Descs;
	Descs.Add(&MeshDesc);
	if (!Mesh->BuildFromMeshDescriptions(Descs, Params))
	{
		return nullptr;
	}
	return Mesh;
}

FTransform DetailInstanceTransform(const FDetailInstanceRec& Rec, const FVector3d& ComponentOrigin)
{
	// Quarter turns about +Z through the anchor point. UE's positive yaw maps
	// +X toward +Y, which is exactly assetgrid.cpp's yaw-1 forward map
	// ((u,v) -> (-v,u)), so yawQuarter * 90 degrees reproduces the baked
	// rotation convention.
	return FTransform(FRotator(0.0, 90.0 * double(Rec.YawQuarter), 0.0),
	                  FVector(Rec.PosUU - ComponentOrigin));
}
} // namespace

// ---------------------------------------------------------------------------
// The tick pipeline
// ---------------------------------------------------------------------------

void UVoxelDetailAssetSubsystem::Tick(float DeltaTime)
{
	if (!Impl || Impl->bDisabled)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	UVoxelWorldSubsystem* VoxelWorld = World->GetSubsystem<UVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return;
	}

	// All three are null until -VoxelAssetDir installed a field (or forever,
	// on a run without assets) -- in which case there is, correctly, nothing
	// to render and this subsystem stays inert.
	const vxc::AssetField* Field = VoxelWorld->GetAssetField();
	const vxc::Amplifier* Amp = VoxelWorld->GetWorldgenAmplifier();
	const vxc::IAssetBankSource* Banks = VoxelWorld->GetAssetBankSource();
	if (Field == nullptr || Amp == nullptr || Banks == nullptr)
	{
		return;
	}

	FVoxelDetailAssetImpl& S = *Impl;

	// --- one-time start ----------------------------------------------------
	if (!S.bStarted)
	{
		bool bAnyDetailLayer = false;
		for (const vxc::AssetLayer& L : Field->layers())
		{
			if (L.cellMm > 0 && !L.terrainLattice)
			{
				bAnyDetailLayer = true;
			}
			if (L.cellMm > 0 && L.maxRadiusMm > S.MaxReachMm)
			{
				S.MaxReachMm = L.maxRadiusMm;
			}
		}
		if (!bAnyDetailLayer)
		{
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("VoxelDetailAssets: asset field installed but carries no detail layer -- "
			            "nothing to render; subsystem stays inert."));
			S.bDisabled = true;
			return;
		}

		if (!FParse::Param(FCommandLine::Get(), TEXT("VoxelDefaultMaterial")))
		{
			DetailMaterial = Cast<UMaterialInterface>(StaticLoadObject(
				UMaterialInterface::StaticClass(), nullptr,
				TEXT("/Game/Voxel/M_VoxelDetailAsset.M_VoxelDetailAsset")));
		}
		if (DetailMaterial == nullptr)
		{
			DetailMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
			if (!S.bMaterialFallbackWarned)
			{
				S.bMaterialFallbackWarned = true;
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("VoxelDetailAssets: M_VoxelDetailAsset not found at "
				            "/Game/Voxel/M_VoxelDetailAsset -- ground cover renders with the engine "
				            "default material (grey, correct shapes). Author it once with "
				            "Tools/create_detail_asset_material.py."));
			}
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		DetailOwner = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector,
		                                        FRotator::ZeroRotator, SpawnParams);
		if (DetailOwner == nullptr)
		{
			UE_LOG(LogVoxelEarth, Error,
			       TEXT("VoxelDetailAssets: could not spawn the detail owner actor -- detail "
			            "rendering disabled this run."));
			S.bDisabled = true;
			return;
		}
		DetailRoot = NewObject<USceneComponent>(DetailOwner, TEXT("VoxelDetailRoot"));
		DetailOwner->SetRootComponent(DetailRoot);
		DetailRoot->RegisterComponent();
#if WITH_EDITOR
		DetailOwner->SetActorLabel(TEXT("VoxelDetailAssetOwner"));
#endif

		S.bStarted = true;
		S.StartTimeSeconds = FPlatformTime::Seconds();
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelDetailAssets: STARTED -- ring %.0f m, group %.1f m, reach dilation "
		            "%lld mm, shadows %s. Rendering detail-lattice plants/rocks only; detail "
		            "ENTITIES (animals) are excluded by design."),
		       S.RingMeters, kGroupEdgeUU / 100.0, (long long)S.MaxReachMm,
		       S.bCastShadow ? TEXT("on") : TEXT("off"));
	}

	// --- anchor (same rule as UVoxelWorldSubsystem::Tick) -------------------
	FVector Anchor = FVector::ZeroVector;
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* Pawn = PC->GetPawn())
		{
			Anchor = Pawn->GetActorLocation();
		}
	}

	const double RingUU = S.RingMeters * 100.0;
	const double UnloadUU = RingUU * kUnloadMultiplier;

	// --- adaptive budgets (see the constants' comment) -----------------------
	// DeltaTime is the LAST frame's duration -- the standard one-frame-lagged
	// headroom proxy, and the same quantity the "Hitch frame" log judges.
	const double FrameMs = double(DeltaTime) * 1000.0;
	const double HealthyMs = double(VoxelDebug::kHitchThresholdMs) * kHealthyFrameFrac;
	const double BusyMs = double(VoxelDebug::kHitchThresholdMs) * kBusyFrameFrac;
	const double Headroom =
		FMath::Clamp((BusyMs - FrameMs) / (BusyMs - HealthyMs), 0.0, 1.0);
	const int32 JobsInFlightCap =
		kMinJobsInFlight +
		FMath::RoundToInt32(Headroom * double(kMaxJobsInFlight - kMinJobsInFlight));
	const double BuildBudgetMs =
		kMinBuildBudgetMs + Headroom * (kMaxBuildBudgetMs - kMinBuildBudgetMs);
	// Shared by the mesh-build and rebuild steps below; each step is
	// guaranteed one unit of progress per tick so a long stretch of busy
	// frames stalls convergence to the old fixed-budget rate, never to zero.
	double BudgetSpentMs = 0.0;

	// --- 1. drain worker results -------------------------------------------
	{
		TUniquePtr<FGroupResult> R;
		while (S.Results.Dequeue(R))
		{
			S.InFlight.Remove(R->Group);
			S.StatSitesResolved += uint64(R->SitesTotal);
			S.StatBankMisses += uint64(R->BankMisses);

			// New geometry first (even if the group itself is dropped below --
			// the mesh is position-independent and the next group will want it).
			for (TUniquePtr<FMeshGeometry>& G : R->NewGeometry)
			{
				if (!S.GeometryKnown.Contains(G->MeshKey))
				{
					S.GeometryKnown.Add(G->MeshKey);
					S.PendingGeometry.Add(G->MeshKey, MoveTemp(G));
				}
				// else: duplicate from a concurrent job -- dropped.
			}

			// A group that left the ring while its job ran: drop the
			// instances; it will re-resolve (identically -- determinism) if
			// the anchor comes back.
			const FVector3d Centre((double(R->Group.X) + 0.5) * kGroupEdgeUU,
			                       (double(R->Group.Y) + 0.5) * kGroupEdgeUU, 0.0);
			const double DistSq = FMath::Square(Centre.X - Anchor.X) + FMath::Square(Centre.Y - Anchor.Y);
			if (DistSq > FMath::Square(UnloadUU))
			{
				continue;
			}

			FVoxelDetailAssetImpl::FGroupRecord& Rec = S.Groups.Add(R->Group);
			Rec.Instances = MoveTemp(R->Instances);
			++S.StatGroupsResolved;
			S.StatInstancesLive += uint64(Rec.Instances.Num());

			// Register + append to already-built components.
			TMap<uint32, TArray<FTransform>> Appends;
			for (const FDetailInstanceRec& Inst : Rec.Instances)
			{
				S.KeyGroups.FindOrAdd(Inst.MeshKey).Add(R->Group);
				if (FVoxelDetailAssetImpl::FMeshEntry* Entry = S.Meshes.Find(Inst.MeshKey);
				    Entry != nullptr && Entry->Hism != nullptr && !Entry->bDirty)
				{
					Appends.FindOrAdd(Inst.MeshKey)
						.Add(DetailInstanceTransform(Inst, Entry->OriginUU));
				}
			}
			for (TPair<uint32, TArray<FTransform>>& A : Appends)
			{
				S.Meshes[A.Key].Hism->AddInstances(A.Value, /*bShouldReturnIndices*/ false,
				                                   /*bWorldSpace*/ false);
			}

			if (!S.bFirstApplyLogged && Rec.Instances.Num() > 0)
			{
				S.bFirstApplyLogged = true;
				UE_LOG(LogVoxelEarth, Log,
				       TEXT("VoxelDetailAssets: first group applied -- %d detail instances of "
				            "%d resolved sites at group (%lld, %lld)."),
				       Rec.Instances.Num(), R->SitesTotal, (long long)R->Group.X,
				       (long long)R->Group.Y);
			}
		}
	}

	// --- 2. budgeted mesh builds -------------------------------------------
	{
		int32 Built = 0;
		for (auto It = S.PendingGeometry.CreateIterator();
		     It && Built < kMaxMeshBuildsPerTick &&
		     (Built == 0 || BudgetSpentMs < BuildBudgetMs);
		     ++It)
		{
			const uint32 Key = It->Key;
			TUniquePtr<FMeshGeometry> Geometry = MoveTemp(It->Value);
			It.RemoveCurrent();
			++Built;
			const double BuildStart = FPlatformTime::Seconds();

			UStaticMesh* Mesh = CreateDetailStaticMesh(*Geometry, DetailMaterial);
			if (Mesh == nullptr)
			{
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("VoxelDetailAssets: mesh build FAILED for key %08x (%d verts) -- "
				            "instances of this (species, seed) will not render this session."),
				       Key, Geometry->Positions.Num());
				BudgetSpentMs += (FPlatformTime::Seconds() - BuildStart) * 1000.0;
				continue;
			}
			BuiltMeshes.Add(Mesh);

			UHierarchicalInstancedStaticMeshComponent* Hism =
				NewObject<UHierarchicalInstancedStaticMeshComponent>(DetailOwner);
			Hism->SetStaticMesh(Mesh);
			Hism->SetMobility(EComponentMobility::Movable);
			Hism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Hism->SetCanEverAffectNavigation(false);
			Hism->SetCastShadow(Impl->bCastShadow);
			// Per-instance distance cull inside the ring: instances vanish at
			// the ring edge (10 cm cover at 112 m is subpixel; the pop is
			// invisible) rather than living until the 1.15x release boundary.
			Hism->SetCullDistances(int32(RingUU * 0.85), int32(RingUU));
			Hism->SetupAttachment(DetailRoot);
			// Component origin near the anchor: instance transforms stay small
			// (<= ring radius) so the instance buffer's float precision is
			// spent on centimetres, not on the 400 km to the world origin.
			const FVector3d Origin(FMath::GridSnap(Anchor.X, VoxelCoords::VoxelSizeUU),
			                       FMath::GridSnap(Anchor.Y, VoxelCoords::VoxelSizeUU), 0.0);
			Hism->SetWorldLocation(FVector(Origin));
			Hism->RegisterComponent();
			HismComponents.Add(Hism);

			FVoxelDetailAssetImpl::FMeshEntry& Entry = S.Meshes.Add(Key);
			Entry.Mesh = Mesh;
			Entry.Hism = Hism;
			Entry.OriginUU = Origin;
			Entry.SolidVoxels = Geometry->SolidVoxels;
			Entry.bDirty = true; // full rebuild picks up every already-applied group
			++S.StatMeshesBuilt;

			const double BuildMs = (FPlatformTime::Seconds() - BuildStart) * 1000.0;
			BudgetSpentMs += BuildMs;
			S.WindowBuildMs += BuildMs;
			S.StatBuildMsTotal += BuildMs;
			++S.WindowMeshBuilds;
		}
	}

	// --- 3. release groups past the unload ring ----------------------------
	{
		TArray<FGroupKey> ToRemove;
		for (const TPair<FGroupKey, FVoxelDetailAssetImpl::FGroupRecord>& G : S.Groups)
		{
			const double CX = (double(G.Key.X) + 0.5) * kGroupEdgeUU;
			const double CY = (double(G.Key.Y) + 0.5) * kGroupEdgeUU;
			const double DistSq = FMath::Square(CX - Anchor.X) + FMath::Square(CY - Anchor.Y);
			if (DistSq > FMath::Square(UnloadUU))
			{
				ToRemove.Add(G.Key);
			}
		}
		for (const FGroupKey& G : ToRemove)
		{
			FVoxelDetailAssetImpl::FGroupRecord Rec;
			S.Groups.RemoveAndCopyValue(G, Rec);
			S.StatInstancesLive -= uint64(Rec.Instances.Num());
			for (const FDetailInstanceRec& Inst : Rec.Instances)
			{
				if (TSet<FGroupKey>* Members = S.KeyGroups.Find(Inst.MeshKey))
				{
					Members->Remove(G);
				}
				if (FVoxelDetailAssetImpl::FMeshEntry* Entry = S.Meshes.Find(Inst.MeshKey))
				{
					// HISM instance removal reshuffles ids; rebuilding the
					// component from the surviving groups (budgeted, below) is
					// the correctness-by-construction path.
					Entry->bDirty = true;
				}
			}
		}
	}

	// --- 4. budgeted full rebuilds of dirty components ----------------------
	{
		int32 Rebuilt = 0;
		for (TPair<uint32, FVoxelDetailAssetImpl::FMeshEntry>& Pair : S.Meshes)
		{
			// Shares the tick's time budget with the mesh builds above (and the
			// same guaranteed-progress floor: at least one rebuild per tick).
			if (Rebuilt >= kMaxRebuildsPerTick ||
			    (Rebuilt > 0 && BudgetSpentMs >= BuildBudgetMs))
			{
				break;
			}
			FVoxelDetailAssetImpl::FMeshEntry& Entry = Pair.Value;
			if (!Entry.bDirty || Entry.Hism == nullptr)
			{
				continue;
			}
			++Rebuilt;
			Entry.bDirty = false;
			const double RebuildStart = FPlatformTime::Seconds();

			TArray<FTransform> Transforms;
			if (const TSet<FGroupKey>* Members = S.KeyGroups.Find(Pair.Key))
			{
				for (const FGroupKey& G : *Members)
				{
					if (const FVoxelDetailAssetImpl::FGroupRecord* Rec = S.Groups.Find(G))
					{
						for (const FDetailInstanceRec& Inst : Rec->Instances)
						{
							if (Inst.MeshKey == Pair.Key)
							{
								Transforms.Add(DetailInstanceTransform(Inst, Entry.OriginUU));
							}
						}
					}
				}
			}
			Entry.Hism->ClearInstances();
			if (Transforms.Num() > 0)
			{
				Entry.Hism->AddInstances(Transforms, /*bShouldReturnIndices*/ false,
				                         /*bWorldSpace*/ false);
			}
			BudgetSpentMs += (FPlatformTime::Seconds() - RebuildStart) * 1000.0;
		}
	}

	// --- 5. dispatch new resolve jobs --------------------------------------
	{
		// Prune finished task handles so the array (and Deinitialize's wait
		// list) stays small.
		S.Tasks.RemoveAll([](const UE::Tasks::TTask<void>& T) { return T.IsCompleted(); });

		const int32 Capacity =
			FMath::Min(kMaxDispatchPerTick, JobsInFlightCap - S.InFlight.Num());
		if (Capacity > 0)
		{
			FVoxelFineTileStreamer* Streamer = VoxelWorld->GetFineTileStreamer();

			struct FCandidate
			{
				double DistSq;
				FGroupKey Key;
			};
			TArray<FCandidate> Candidates;

			const int64 G0X = int64(FMath::FloorToDouble((Anchor.X - RingUU) / kGroupEdgeUU));
			const int64 G1X = int64(FMath::FloorToDouble((Anchor.X + RingUU) / kGroupEdgeUU));
			const int64 G0Y = int64(FMath::FloorToDouble((Anchor.Y - RingUU) / kGroupEdgeUU));
			const int64 G1Y = int64(FMath::FloorToDouble((Anchor.Y + RingUU) / kGroupEdgeUU));
			for (int64 GY = G0Y; GY <= G1Y; ++GY)
			{
				for (int64 GX = G0X; GX <= G1X; ++GX)
				{
					const FGroupKey Key{GX, GY};
					if (S.Groups.Contains(Key) || S.InFlight.Contains(Key))
					{
						continue;
					}
					const double CX = (double(GX) + 0.5) * kGroupEdgeUU;
					const double CY = (double(GY) + 0.5) * kGroupEdgeUU;
					const double DistSq =
						FMath::Square(CX - Anchor.X) + FMath::Square(CY - Anchor.Y);
					if (DistSq > FMath::Square(RingUU))
					{
						continue;
					}
					Candidates.Add({DistSq, Key});
				}
			}
			Candidates.Sort([](const FCandidate& A, const FCandidate& B)
			                { return A.DistSq < B.DistSq; });
			S.StatPendingGroups = Candidates.Num();

			// Time-to-first-full-ring, latched once. "Converged" is the full
			// conjunction: every group in the ring resolved (no candidates
			// left, none in flight), every first-encounter mesh built, and no
			// HISM still waiting on a rebuild -- i.e. everything the resolver
			// placed is actually on screen.
			if (!S.bConvergedLogged && S.Groups.Num() > 0 && Candidates.Num() == 0 &&
			    S.InFlight.Num() == 0 && S.PendingGeometry.Num() == 0)
			{
				bool bAnyDirty = false;
				for (const TPair<uint32, FVoxelDetailAssetImpl::FMeshEntry>& Pair : S.Meshes)
				{
					if (Pair.Value.bDirty && Pair.Value.Hism != nullptr)
					{
						bAnyDirty = true;
						break;
					}
				}
				if (!bAnyDirty)
				{
					S.bConvergedLogged = true;
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelDetailAssets: CONVERGED -- full ring resolved and built "
					            "%.1f s after start. %d groups, %llu instances, %d distinct "
					            "meshes (%.0f ms game-thread mesh-build time total)."),
					       FPlatformTime::Seconds() - S.StartTimeSeconds, S.Groups.Num(),
					       (unsigned long long)S.StatInstancesLive, S.Meshes.Num(),
					       S.StatBuildMsTotal);
				}
			}

			int32 Dispatched = 0;
			for (const FCandidate& C : Candidates)
			{
				if (Dispatched >= Capacity)
				{
					break;
				}

				const vxc::AssetVoxelRect Rect{C.Key.X * kGroupEdgeVoxels,
				                               C.Key.Y * kGroupEdgeVoxels,
				                               C.Key.X * kGroupEdgeVoxels + kGroupEdgeVoxels - 1,
				                               C.Key.Y * kGroupEdgeVoxels + kGroupEdgeVoxels - 1};

				// THE RESIDENCY GATE. A worker column into a non-resident fine
				// tile is a gate leak (fatal in unattended runs) -- defer the
				// group until the streamer has the tiles, dilated by the
				// widest layer reach because instancesForRect evaluates
				// anchors up to that far outside the rect.
				if (Streamer != nullptr)
				{
					const int64 X0Mm = Rect.vx0 * vxc::kVoxelSizeMm - S.MaxReachMm;
					const int64 Y0Mm = Rect.vy0 * vxc::kVoxelSizeMm - S.MaxReachMm;
					const int64 X1Mm = (Rect.vx1 + 1) * vxc::kVoxelSizeMm + S.MaxReachMm;
					const int64 Y1Mm = (Rect.vy1 + 1) * vxc::kVoxelSizeMm + S.MaxReachMm;
					if (!Streamer->IsFootprintResident(X0Mm, Y0Mm, X1Mm, Y1Mm))
					{
						continue; // retried automatically on a later tick
					}
				}

				FResolveJobInput Input;
				Input.Field = Field;
				Input.Amp = Amp;
				Input.Banks = Banks;
				Input.Channels = VoxelWorld->GetAssetChannelSource();
				Input.Group = C.Key;
				Input.Rect = Rect;
				Input.KnownGeometry = S.GeometryKnown; // snapshot; stale => dup, not miss

				S.InFlight.Add(C.Key);
				++Dispatched;

				// The queue outlives the job by the Deinitialize wait; plain
				// pointer capture of the impl-owned queue is safe under that
				// contract (WaitForAllJobs runs before Impl is destroyed).
				auto* Queue = &S.Results;
				S.Tasks.Add(UE::Tasks::Launch(
					TEXT("VoxelDetailResolve"),
					[Input = MoveTemp(Input), Queue]()
					{
						TUniquePtr<FGroupResult> R = MakeUnique<FGroupResult>(RunResolveJob(Input));
						Queue->Enqueue(MoveTemp(R));
					},
					UE::Tasks::ETaskPriority::BackgroundNormal));
			}
		}
	}

	// --- 6. periodic telemetry ---------------------------------------------
	// Convergence-curve log: every 5 s until the first full ring, so the curve
	// (groups resolved / pending, meshes built / pending, build throughput) is
	// readable straight off the capture log rather than inferred from settle
	// times. Goes quiet once converged; the 30 s census below continues.
	if (!S.bConvergedLogged)
	{
		S.ProgressLogTimer += DeltaTime;
		if (S.ProgressLogTimer >= 5.0 && S.StatGroupsResolved > 0)
		{
			S.ProgressLogTimer = 0.0;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelDetailAssets: converging -- %.1f s elapsed: %d groups live, "
			            "%d pending, %d in flight; %d meshes built, %d builds pending; "
			            "%llu instances; window: %llu builds in %.1f ms; headroom %.2f "
			            "(frame %.1f ms), jobsCap %d, budget %.1f ms"),
			       FPlatformTime::Seconds() - S.StartTimeSeconds, S.Groups.Num(),
			       S.StatPendingGroups, S.InFlight.Num(), S.Meshes.Num(),
			       S.PendingGeometry.Num(), (unsigned long long)S.StatInstancesLive,
			       (unsigned long long)S.WindowMeshBuilds, S.WindowBuildMs, Headroom,
			       FrameMs, JobsInFlightCap, BuildBudgetMs);
			S.WindowMeshBuilds = 0;
			S.WindowBuildMs = 0.0;
		}
	}

	S.LogTimer += DeltaTime;
	if (S.LogTimer >= 30.0)
	{
		S.LogTimer = 0.0;
		if (S.StatGroupsResolved > 0)
		{
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelDetailAssets: %llu groups live (%d pending, %d in flight), %llu "
			            "instances, %d distinct (species, seed) meshes (%d builds pending), "
			            "%d HISM components, %llu sites resolved total, %llu bank misses, "
			            "%.0f ms mesh-build time total. ZERO INSTANCES WITH GROUPS LIVE MEANS "
			            "THE RESOLVER IS GATING EVERYTHING OUT HERE (biome/elevation/slope), "
			            "not a wiring fault."),
			       (unsigned long long)S.Groups.Num(), S.StatPendingGroups, S.InFlight.Num(),
			       (unsigned long long)S.StatInstancesLive,
			       S.Meshes.Num(), S.PendingGeometry.Num(), HismComponents.Num(),
			       (unsigned long long)S.StatSitesResolved,
			       (unsigned long long)S.StatBankMisses, S.StatBuildMsTotal);
		}
	}
}
